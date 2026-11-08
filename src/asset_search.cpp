#include "asset_search.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "bsa/reader.hpp"
#include "pattern_value.h"

namespace skqe {
namespace {

std::string Utf8(const wchar_t* w, size_t len) {
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string s(n, ' ');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Utf8(const std::wstring& w) { return Utf8(w.data(), w.size()); }

std::string LowerPath(std::string s) {
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring FullDir(const std::wstring& d) {
    std::wstring in = d;
    std::replace(in.begin(), in.end(), L'/', L'\\');
    DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (!n) return in;
    std::wstring out(n, L' ');
    n = GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
    out.resize(n);
    if (out.empty() || out.back() != L'\\') out.push_back(L'\\');
    return out;
}

std::wstring LongPath(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

bool Wildcard(const char* s, const char* p) {
    const char* star = nullptr;
    const char* mark = nullptr;
    while (*s) {
        if (*p == '*') {
            star = p++;
            mark = s;
        } else if (*p == '?' || *p == *s) {
            ++p;
            ++s;
        } else if (star) {
            p = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return !*p;
}

struct Id {
    uint32_t volume = 0;
    uint64_t file = 0;
    bool operator<(const Id& o) const { return volume != o.volume ? volume < o.volume : file < o.file; }
};

struct Hit {
    std::string key, shown, archive;
    Id id;  // of the loose file, or of the archive
    int root = -1;
    int64_t size = -1;
};

struct Archive {
    std::wstring path;
    std::string name;
    std::vector<int> roots;
};

struct Job {
    int root;
    std::wstring rel;
};

// Directories to list, shared by the workers, so one huge root uses every core.
class JobQueue {
public:
    void Push(Job j) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_.push_back(std::move(j));
            ++open_;
        }
        cv_.notify_one();
    }
    bool Pop(Job* j) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty() || !open_; });
        if (q_.empty()) return false;
        *j = std::move(q_.back());
        q_.pop_back();
        return true;
    }
    void Done() {
        bool last;
        {
            std::lock_guard<std::mutex> l(m_);
            last = !--open_;
        }
        if (last) cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Job> q_;
    size_t open_ = 0;
};

}  // namespace

bool AssetPathMatches(const std::string& path, const std::string& pattern) {
    if (pattern.find_first_of("*?") == std::string::npos) return path.find(pattern) != std::string::npos;
    if (pattern.find('\\') != std::string::npos) return Wildcard(path.c_str(), pattern.c_str());
    const size_t slash = path.rfind('\\');
    return Wildcard(path.c_str() + (slash == std::string::npos ? 0 : slash + 1), pattern.c_str());
}

std::vector<AssetSearchRoot> StagingRoots(const std::wstring& staging) {
    const std::wstring dir = FullDir(staging);
    std::vector<AssetSearchRoot> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != L"." && n != L"..") out.push_back({dir + n, Utf8(n)});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const AssetSearchRoot& a, const AssetSearchRoot& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool SearchAssets(const AssetSearchOptions& opt, AssetSearchResult* result, std::string* error) {
    const std::string pattern = LowerPath(opt.pattern);
    if (pattern.empty()) {
        *error = "nothing to search for";
        return false;
    }
    std::vector<std::wstring> bases;
    for (const auto& r : opt.roots) {
        bases.push_back(LongPath(FullDir(r.path)));
        if (GetFileAttributesW(bases.back().c_str()) == INVALID_FILE_ATTRIBUTES) {
            *error = "no folder at " + Utf8(r.path);
            return false;
        }
    }
    const int threads = opt.threads > 0 ? opt.threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    std::vector<uint32_t> volumes(opt.roots.size(), 0);
    std::vector<Hit> hits;
    std::map<Id, Archive> archives;
    std::mutex shared;
    std::atomic<int64_t> loose{0}, packed{0};
    auto warn = [&](std::string w) {
        std::lock_guard<std::mutex> l(shared);
        result->warnings.push_back(std::move(w));
    };

    JobQueue queue;
    for (int i = 0; i < static_cast<int>(opt.roots.size()); ++i) queue.Push({i, L""});
    auto lister = [&] {
        std::vector<uint64_t> buf(64 * 1024 / 8);
        std::vector<Hit> mine;
        std::vector<std::pair<Id, Archive>> found;
        Job job;
        while (queue.Pop(&job)) {
            const std::wstring dir = bases[job.root] + job.rel;
            HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                warn("cannot open folder " + Utf8(dir.substr(4)));
                queue.Done();
                continue;
            }
            if (job.rel.empty()) {
                BY_HANDLE_FILE_INFORMATION fi{};
                if (GetFileInformationByHandle(h, &fi)) volumes[job.root] = fi.dwVolumeSerialNumber;
            }
            const uint32_t volume = volumes[job.root];
            FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
            while (GetFileInformationByHandleEx(h, cls, buf.data(), static_cast<DWORD>(buf.size() * 8))) {
                cls = FileIdBothDirectoryInfo;
                auto* p = reinterpret_cast<const uint8_t*>(buf.data());
                for (;;) {
                    auto* e = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
                    const std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
                    if (name != L"." && name != L"..") {
                        if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            if (!(e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) queue.Push({job.root, job.rel + name + L"\\"});
                        } else {
                            ++loose;
                            const Id id{volume, static_cast<uint64_t>(e->FileId.QuadPart)};
                            std::string shown = Utf8(job.rel + name);
                            std::string key = LowerPath(shown);
                            if (job.rel.empty() && key.size() > 4 && !key.compare(key.size() - 4, 4, ".bsa")) {
                                Archive a;
                                a.path = dir + name;
                                a.name = shown;
                                a.roots.push_back(job.root);
                                found.push_back({id, std::move(a)});
                            }
                            if (AssetPathMatches(key, pattern)) {
                                Hit hit;
                                hit.key = std::move(key);
                                hit.shown = std::move(shown);
                                hit.id = id;
                                hit.root = job.root;
                                hit.size = e->EndOfFile.QuadPart;
                                mine.push_back(std::move(hit));
                            }
                        }
                    }
                    if (!e->NextEntryOffset) break;
                    p += e->NextEntryOffset;
                }
            }
            CloseHandle(h);
            queue.Done();
        }
        std::lock_guard<std::mutex> l(shared);
        std::move(mine.begin(), mine.end(), std::back_inserter(hits));
        for (auto& [id, a] : found) {
            auto [it, fresh] = archives.try_emplace(id, a);
            if (!fresh) it->second.roots.push_back(a.roots[0]);
        }
    };
    {
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i) pool.emplace_back(lister);
        for (auto& t : pool) t.join();
    }

    // Each distinct archive once, however many folders hold it.
    std::vector<std::pair<Id, const Archive*>> work;
    for (const auto& [id, a] : archives) work.push_back({id, &a});
    std::atomic<size_t> next{0};
    auto reader = [&] {
        std::vector<Hit> mine;
        for (size_t i; (i = next.fetch_add(1)) < work.size();) {
            const Archive& a = *work[i].second;
            bsa::Reader r;
            bsa::Status s = r.open(a.path);
            if (s != bsa::Status::Ok) {
                warn(a.name + " in " + opt.roots[a.roots[0]].name + ": " + bsa::to_string(s));
                continue;
            }
            const uint32_t count = r.header().fileCount;
            packed += count;
            for (uint32_t k = 0; k < count; ++k) {
                bsa::Entry e;
                std::string_view folder, name;
                if (r.entry(k, e) != bsa::Status::Ok || r.folder_name(e.folderIndex, folder) != bsa::Status::Ok ||
                    r.file_name(e, name) != bsa::Status::Ok)
                    continue;
                std::string rel(folder);
                if (!rel.empty()) rel += '\\';
                rel.append(name);
                std::string shown = TextToUtf8(reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
                std::string key = LowerPath(shown);
                if (!AssetPathMatches(key, pattern)) continue;
                Hit hit;
                hit.key = std::move(key);
                hit.shown = std::move(shown);
                hit.archive = a.name;
                hit.id = work[i].first;
                mine.push_back(std::move(hit));
            }
        }
        std::lock_guard<std::mutex> l(shared);
        std::move(mine.begin(), mine.end(), std::back_inserter(hits));
    };
    {
        std::vector<std::thread> pool;
        for (int i = 0; i < threads; ++i) pool.emplace_back(reader);
        for (auto& t : pool) t.join();
    }

    // One result per path, one copy per distinct file.
    struct Merged {
        std::string shown;
        bool looseName = false;
        std::map<std::pair<bool, Id>, AssetSearchCopy> copies;
        std::map<std::pair<bool, Id>, std::vector<int>> where;
    };
    std::unordered_map<std::string, Merged> byPath;
    byPath.reserve(hits.size());
    for (auto& h : hits) {
        Merged& m = byPath[h.key];
        const bool isArchive = !h.archive.empty();
        const std::pair<bool, Id> copyKey{isArchive, h.id};
        // A path packed twice in one archive is still that archive's one copy.
        auto [it, fresh] = m.copies.try_emplace(copyKey);
        if (fresh) {
            it->second.archive = h.archive;
            it->second.size = h.size;
        }
        if (isArchive) m.where[copyKey] = archives[h.id].roots;
        else m.where[copyKey].push_back(h.root);
        if (m.shown.empty() || (!isArchive && !m.looseName)) {
            m.shown = h.shown;
            m.looseName = !isArchive;
        }
    }
    auto rootLess = [&](int a, int b) { return a < b; };  // Data comes first, then mods by name
    for (auto& [key, m] : byPath) {
        AssetSearchFile f;
        f.path = m.shown;
        std::vector<std::pair<int, AssetSearchCopy>> ordered;
        for (auto& [copyKey, copy] : m.copies) {
            auto& where = m.where[copyKey];
            std::sort(where.begin(), where.end(), rootLess);
            where.erase(std::unique(where.begin(), where.end()), where.end());
            if (where.size() > 1) result->hardlinksMerged += static_cast<int64_t>(where.size()) - 1;
            for (int r : where) copy.in.push_back(opt.roots[r].name);
            ordered.push_back({where.empty() ? 0 : where[0], std::move(copy)});
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            if (a.second.archive.empty() != b.second.archive.empty()) return a.second.archive.empty();
            return a.first != b.first ? a.first < b.first : _stricmp(a.second.archive.c_str(), b.second.archive.c_str()) < 0;
        });
        for (auto& c : ordered) f.copies.push_back(std::move(c.second));
        result->files.push_back(std::move(f));
    }
    std::sort(result->files.begin(), result->files.end(), [](const AssetSearchFile& a, const AssetSearchFile& b) {
        return LowerPath(a.path) < LowerPath(b.path);
    });
    std::sort(result->warnings.begin(), result->warnings.end());
    result->looseScanned = loose;
    result->packedScanned = packed;
    result->archivesRead = static_cast<int64_t>(work.size());
    return true;
}

}  // namespace skqe
