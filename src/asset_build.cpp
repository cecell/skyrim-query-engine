#include "asset_build.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <asset/metadata.hpp>
#include <bsa/reader.hpp>

#include "asset_readers.h"
#include "db_build.h"
#include "db_util.h"
#include "pattern_value.h"

namespace spf {
namespace {

constexpr int kAssetSchemaVersion = 1;
constexpr size_t kBatchRows = 20000;
// Header reads: how much of a file a reader may need. Whole files are read
// for NIF texture paths, PEX and TRI (names at the end), up to these caps.
constexpr size_t kAudioProbe = asset::kAudioProbeBytes;
constexpr size_t kAudioMax = size_t{64} << 20;
constexpr size_t kPackedMax = size_t{128} << 20;  // largest packed file decompressed for headers / hash
constexpr size_t kHashChunk = size_t{1} << 20;

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Absolute, backslashes, trailing backslash.
std::wstring FullDir(const std::wstring& d) {
    std::wstring in = d;
    for (auto& c : in)
        if (c == L'/') c = L'\\';
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(in.c_str(), 4096, buf, nullptr);
    std::wstring out = n > 0 && n < 4096 ? std::wstring(buf, n) : in;
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    return out;
}

// \\?\ form for long paths.
std::wstring LongPath(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

int64_t UnixTime(int64_t fileTime) { return fileTime / 10000000 - 11644473600LL; }

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadTextFile(const std::wstring& path, std::vector<std::string>* lines) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (lines->empty() && s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xEF) s.erase(0, 3);  // BOM
        lines->push_back(s);
    }
    fclose(fp);
    return true;
}

// key's value in [section] of an ini file; false when absent.
bool IniValue(const std::wstring& file, const char* section, const char* key, std::string* value) {
    std::vector<std::string> lines;
    if (!ReadTextFile(file, &lines)) return false;
    std::string cur;
    bool found = false;
    for (const auto& raw : lines) {
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos || raw[a] == ';' || raw[a] == '#') continue;
        std::string l = raw.substr(a);
        if (l[0] == '[') {
            cur = Lower(l.substr(1, l.find(']') == std::string::npos ? std::string::npos : l.find(']') - 1));
            continue;
        }
        size_t eq = l.find('=');
        if (eq == std::string::npos || cur != Lower(section)) continue;
        std::string k = l.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (Lower(k) != Lower(key)) continue;
        std::string v = l.substr(eq + 1);
        size_t b = v.find_first_not_of(" \t");
        *value = b == std::string::npos ? "" : v.substr(b);
        found = true;  // the last occurrence wins, as the game reads it
    }
    return found;
}

// ---- SHA-256 (Windows CNG) ----

BCRYPT_ALG_HANDLE Sha256Alg() {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) a = nullptr;
        return a;
    }();
    return alg;
}

class Sha256 {
public:
    Sha256() {
        if (Sha256Alg()) BCryptCreateHash(Sha256Alg(), &h_, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (h_) BCryptDestroyHash(h_);
    }
    void Add(const uint8_t* p, size_t n) {
        while (h_ && n) {
            ULONG k = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
            BCryptHashData(h_, const_cast<PUCHAR>(p), k, 0);
            p += k;
            n -= k;
        }
    }
    std::string Finish() {
        std::string out(32, '\0');
        if (!h_ || BCryptFinishHash(h_, reinterpret_cast<PUCHAR>(out.data()), 32, 0) != 0) return {};
        return out;
    }

private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

// ---- Roots and the folder walk ----

struct Root {
    int64_t id = 0;
    std::string kind, name, manager;  // manager '' = NULL
    std::wstring path;                // absolute, trailing backslash
    int enabled = -1;                 // -1 = NULL
    int64_t priority = 0;
    bool hasPriority = false;
    int64_t fileCount = 0, totalSize = 0, maxMtime = 0;
    uint32_t volume = 0;
    std::string walkError;
};

// One loose file as the walk found it.
struct Loose {
    std::string path;     // normalized, relative to its root
    std::string topName;  // the name as on disk, for files directly in the root (plugins, archives)
    int64_t size = 0, mtime = 0;
    uint64_t fileId = 0;
    int root = 0;         // index into roots
    bool inData = false;
    int64_t dbId = 0;
};

// Every file under a root, with its NTFS file ID, read from directory
// listings (FileIdBothDirectoryInfo): no file is opened. Directory reparse
// points (junctions, links) are not followed.
void WalkRoot(Root* root, std::vector<Loose>* out, int rootIndex, std::atomic<int64_t>* reparse) {
    std::vector<std::wstring> dirs{L""};
    std::vector<uint64_t> buf(64 * 1024 / 8);
    const std::wstring base = LongPath(root->path);
    bool first = true;
    while (!dirs.empty()) {
        std::wstring rel = dirs.back();
        dirs.pop_back();
        HANDLE h = CreateFileW((base + rel).c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (first) root->walkError = "cannot open the folder";
            continue;
        }
        if (first) {
            BY_HANDLE_FILE_INFORMATION fi{};
            if (GetFileInformationByHandle(h, &fi)) root->volume = fi.dwVolumeSerialNumber;
            first = false;
        }
        FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
        while (GetFileInformationByHandleEx(h, cls, buf.data(), static_cast<DWORD>(buf.size() * 8))) {
            cls = FileIdBothDirectoryInfo;
            auto* p = reinterpret_cast<const uint8_t*>(buf.data());
            for (;;) {
                auto* e = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
                std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
                if (name != L"." && name != L"..") {
                    if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ++*reparse;
                        else dirs.push_back(rel + name + L"\\");
                    } else {
                        Loose f;
                        f.path = assets::NormalizePath(Utf8(rel + name));
                        if (rel.empty()) f.topName = Utf8(name);
                        f.size = e->EndOfFile.QuadPart;
                        f.mtime = UnixTime(e->LastWriteTime.QuadPart);
                        f.fileId = static_cast<uint64_t>(e->FileId.QuadPart);
                        f.root = rootIndex;
                        ++root->fileCount;
                        root->totalSize += f.size;
                        root->maxMtime = std::max(root->maxMtime, f.mtime);
                        out->push_back(std::move(f));
                    }
                }
                if (!e->NextEntryOffset) break;
                p += e->NextEntryOffset;
            }
        }
        CloseHandle(h);
    }
}

// ---- Rows flowing from the read workers to the writer ----

enum Table { kArchives, kDds, kTga, kNif, kNifTextures, kAudio, kPex, kHkx, kTri, kTriMorphs, kTableCount };
const char* kTableNames[kTableCount] = {"archives", "dds", "tga", "nif", "nif_textures", "audio", "pex", "hkx", "tri",
                                        "tri_morphs"};
const int kTableColumns[kTableCount] = {9, 11, 7, 9, 6, 9, 3, 5, 7, 4};

struct FileRow {
    int64_t id = 0;
    std::string path;
    int64_t root = 0;
    int64_t archive = -1;  // -1 = loose
    int64_t size = 0;
    int64_t packed = -1;   // -1 = NULL
    int64_t mtime = -1;    // -1 = NULL
    int inData = 0;
    std::string hash, error;
};

struct TableRow {
    int table;
    std::vector<Cell> cells;
};

struct AssetBatch {
    std::vector<FileRow> files;
    std::vector<TableRow> rows;
    size_t Size() const { return files.size() + rows.size(); }
};

using AssetQueue = BoundedQueue<AssetBatch>;

Cell OptInt(bool has, int64_t v) { return has ? Cell::Int(v) : Cell(); }
Cell OptText(const std::string& s) { return s.empty() ? Cell() : Cell::Text(s); }

// How many bytes of a file its reader needs (0 = no reader for that type).
size_t BytesNeeded(const std::string& ext, bool textures) {
    if (ext == "dds") return asset::kDdsProbeBytes;
    if (ext == "tga") return asset::kTgaProbeBytes;
    if (ext == "hkx") return assets::kHkxProbeBytes;
    if (ext == "wav" || ext == "xwm" || ext == "fuz") return kAudioProbe;
    if (ext == "nif") return textures ? assets::kNifMaxBytes : asset::kNifProbeBytes;
    if (ext == "pex") return assets::kPexMaxBytes;
    if (ext == "tri") return assets::kTriMaxBytes;
    return 0;
}

// Header facts of one file into detail rows. `whole`: d holds the whole file.
// Returns the error text for files.error ('' when fine). `more` is set when
// the reader needs more bytes than d holds (audio, NIF headers).
std::string ReadFacts(int64_t id, const std::string& ext, const uint8_t* d, size_t n, bool whole, bool textures,
                      std::vector<TableRow>* rows, bool* more) {
    *more = false;
    // The reader's status, and for a refused file what it holds instead
    // (a DDS named .tga, an MP3 named .wav).
    auto st = [&](asset::Status s, const char* what) {
        std::string e = std::string(what) + ": " + asset::to_string(s);
        if (n == 0) return e + " (empty file)";
        if (s != asset::Status::NotRecognized && s != asset::Status::Corrupt) return e;
        static const char* kKinds[] = {nullptr, "DDS", "NIF", "FUZ", "WAV", "XWM"};
        const char* kind = kKinds[static_cast<int>(asset::detect(d, n))];
        if (n >= 3 && memcmp(d, "ID3", 3) == 0) kind = "MP3";
        else if (n >= 4 && memcmp(d, "OggS", 4) == 0) kind = "OGG";
        else if (n >= 4 && memcmp(d + 1, "PNG", 3) == 0) kind = "PNG";
        if (kind) e += std::string(" (content is ") + kind + ")";
        return e;
    };
    if (ext == "dds") {
        asset::DdsInfo i;
        asset::Status s = asset::read_dds(d, n, i);
        if (s != asset::Status::Ok) return st(s, "dds");
        std::string fmt = asset::to_string(i.format);
        if (i.format == asset::DdsFormat::Other) fmt = i.fourcc[0] ? i.fourcc : "DXGI " + std::to_string(i.dxgiFormat);
        rows->push_back({kDds,
                         {Cell::Int(id), Cell::Int(i.width), Cell::Int(i.height), Cell::Text(fmt),
                          OptInt(i.dx10, i.dxgiFormat), Cell::Int(i.mipmaps), Cell::Int(i.cubemap), Cell::Int(i.srgb),
                          Cell::Int(i.hasAlpha), Cell::Int(i.dx10), Cell::Int(i.arraySize)}});
    } else if (ext == "tga") {
        asset::TgaInfo i;
        asset::Status s = asset::read_tga(d, n, i);
        if (s != asset::Status::Ok) return st(s, "tga");
        rows->push_back({kTga,
                         {Cell::Int(id), Cell::Int(i.width), Cell::Int(i.height), Cell::Int(i.bitsPerPixel),
                          Cell::Int(i.imageType), Cell::Int(i.alphaBits > 0), Cell::Int(i.rle)}});
    } else if (ext == "nif") {
        asset::NifInfo i;
        asset::Status s = asset::read_nif(d, n, i);
        if (s == asset::Status::Truncated && !whole) {
            *more = true;
            return {};
        }
        if (s != asset::Status::Ok) return st(s, "nif");
        static const std::set<std::string> kShapes = {"BSTriShape", "BSDynamicTriShape", "BSMeshLODTriShape",
                                                      "BSSubIndexTriShape", "BSLODTriShape", "BSSegmentedTriShape",
                                                      "NiTriShape", "NiTriStrips"};
        int64_t shapes = 0;
        for (uint16_t t : i.blockTypeIndex)
            if (t < i.blockTypes.size() && kShapes.count(i.blockTypes[t])) ++shapes;
        const char* game = i.engine == asset::NifEngine::SkyrimSe       ? "se"
                         : i.engine == asset::NifEngine::SkyrimLe       ? "le"
                         : i.engine == asset::NifEngine::SkyrimEither   ? "both"
                                                                        : "unknown";
        // vertex_count / triangle_count: not in beth-asset-metadata v0.1.0.
        rows->push_back({kNif,
                         {Cell::Int(id), Cell::Text(asset::nif_version_string(i.version)), Cell::Int(i.userVersion),
                          OptInt(i.hasBsHeader, i.bsVersion), Cell::Int(i.blockCount), Cell::Int(shapes), Cell(),
                          Cell(), Cell::Text(game)}});
        if (textures && whole) {
            std::vector<assets::NifTexture> tex;
            std::string terr;
            assets::ReadNifTextures(d, n, &tex, &terr);
            int64_t idx = 0;
            for (const auto& t : tex) {
                std::string norm = assets::NormalizeTexturePath(t.path);
                if (norm.empty()) continue;
                rows->push_back({kNifTextures,
                                 {Cell::Int(id), Cell::Int(idx++), Cell::Text(t.block), Cell::Int(t.slot),
                                  Cell::Text(norm), Cell()}});
            }
            if (!terr.empty() && i.engine != asset::NifEngine::Unknown) return terr;
        }
    } else if (ext == "wav" || ext == "xwm" || ext == "fuz") {
        asset::AudioInfo i;
        asset::Status s = asset::read_audio(d, n, i);
        if ((s == asset::Status::Truncated || (s == asset::Status::Ok && i.dpdsTruncated)) && !whole) {
            *more = true;
            return {};
        }
        if (s != asset::Status::Ok) return st(s, "audio");
        rows->push_back({kAudio,
                         {Cell::Int(id), Cell::Text(asset::to_string(i.container)),
                          Cell::Text(asset::codec_name(i.formatTag, i.payload)), Cell::Int(i.sampleRate),
                          Cell::Int(i.channels), Cell::Int(i.bitsPerSample), Cell::Int(i.bitRate),
                          i.durationSeconds > 0 ? Cell::Real(i.durationSeconds) : Cell(),
                          OptInt(i.container == asset::AudioContainer::Fuz, i.lipBytes)}});
    } else if (ext == "pex") {
        assets::PexInfo i;
        std::string err;
        if (!assets::ReadPex(d, n, &i, &err)) return err;
        rows->push_back({kPex, {Cell::Int(id), Cell::Text(i.scriptName), OptText(i.parentScript)}});
        return err;
    } else if (ext == "hkx") {
        assets::HkxInfo i;
        std::string err;
        bool ok = assets::ReadHkx(d, n, &i, &err);
        rows->push_back({kHkx,
                         {Cell::Int(id), OptText(i.havokVersion), OptInt(i.pointerSize > 0, i.pointerSize),
                          i.valid ? Cell::Text(i.pointerSize == 8 ? "se" : "le") : Cell(), Cell::Int(i.valid)}});
        (void)ok;
        return err;
    } else if (ext == "tri") {
        assets::TriInfo i;
        std::string err;
        if (!assets::ReadTri(d, n, &i, &err)) return err;
        if (!whole && err.empty()) err = "tri: larger than the read cap (names not read)";
        const bool g = !i.bodySlide;  // BodySlide morphs carry no geometry counts
        rows->push_back({kTri,
                         {Cell::Int(id), Cell::Text(i.version), OptInt(g, i.vertexCount), OptInt(g, i.triangleCount),
                          OptInt(g, i.uvCount), Cell::Int(i.morphCount), OptInt(g, i.modifierCount)}});
        int64_t idx = 0;
        if (whole) {
            for (const auto& m : i.morphs)
                rows->push_back({kTriMorphs, {Cell::Int(id), Cell::Int(idx++), Cell::Text("morph"), Cell::Text(m)}});
            for (const auto& m : i.modifiers)
                rows->push_back({kTriMorphs, {Cell::Int(id), Cell::Int(idx++), Cell::Text("modifier"), Cell::Text(m)}});
        }
        return err;
    }
    return {};
}

// Reads the first `want` bytes of a file (the whole file when hashing,
// keeping only `want` of it). False with `error` when it cannot be read.
bool ReadLoose(const std::wstring& path, size_t want, int64_t size, Sha256* hash, std::vector<uint8_t>* out,
               std::string* error) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = "cannot open (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    want = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(want), size));
    out->resize(want);
    size_t have = 0;
    bool ok = true;
    std::vector<uint8_t> chunk;
    for (;;) {
        // Into `out` until it holds `want`; past that (hashing) through a chunk.
        uint8_t* dst;
        DWORD ask;
        if (have < want) {
            dst = out->data() + have;
            ask = static_cast<DWORD>(std::min<size_t>(want - have, kHashChunk));
        } else if (hash) {
            chunk.resize(kHashChunk);
            dst = chunk.data();
            ask = static_cast<DWORD>(kHashChunk);
        } else {
            break;
        }
        DWORD got = 0;
        if (!ReadFile(h, dst, ask, &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        if (hash) hash->Add(dst, got);
        if (have < want) have += got;
    }
    CloseHandle(h);
    out->resize(have);
    if (!ok) *error = "read failed";
    return ok;
}

struct ArchivePlanEntry {
    std::string loadedBy, plugin;
    int rank = -1;
};

struct Shared {
    const std::vector<Root>* roots;
    const std::vector<Loose>* files;
    const std::unordered_map<size_t, ArchivePlanEntry>* plan;  // loose index -> load facts (loaded archives)
    bool hash = false, textures = true;
    std::atomic<int64_t>* nextFileId;
};

void FlushIfFull(AssetBatch* b, AssetQueue* q) {
    if (b->Size() < kBatchRows) return;
    AssetBatch next;
    std::swap(next, *b);
    q->Push(std::move(next));
}

// One loose file: header facts (and hash) -> its file row and detail rows.
void ProcessLoose(const Shared& sh, size_t index, AssetBatch* batch) {
    const Loose& f = (*sh.files)[index];
    const Root& r = (*sh.roots)[f.root];
    FileRow row;
    row.id = f.dbId;
    row.path = f.path;
    row.root = r.id;
    row.size = f.size;
    row.mtime = f.mtime;
    row.inData = f.inData;
    std::vector<TableRow> rows;
    const std::string ext = assets::SplitPath(f.path).ext;
    size_t want = BytesNeeded(ext, sh.textures);
    std::vector<uint8_t> bytes;
    std::unique_ptr<Sha256> hash(sh.hash ? new Sha256 : nullptr);
    const std::wstring full = r.path + Widen(f.path);
    if (ReadLoose(full, want, f.size, hash.get(), &bytes, &row.error)) {
        if (hash) row.hash = hash->Finish();
        if (want) {
            bool more = false;
            bool whole = static_cast<int64_t>(bytes.size()) >= f.size;
            row.error = ReadFacts(f.dbId, ext, bytes.data(), bytes.size(), whole, sh.textures, &rows, &more);
            if (more) {  // audio / NIF header longer than the probe: read more
                size_t cap = ext == "nif" ? assets::kNifMaxBytes : kAudioMax;
                std::string err;
                if (ReadLoose(full, cap, f.size, nullptr, &bytes, &err)) {
                    whole = static_cast<int64_t>(bytes.size()) >= f.size;
                    rows.clear();
                    row.error = ReadFacts(f.dbId, ext, bytes.data(), bytes.size(), true, sh.textures && whole, &rows,
                                          &more);
                } else {
                    row.error = err;
                }
            }
        }
    }
    batch->files.push_back(std::move(row));
    for (auto& t : rows) batch->rows.push_back(std::move(t));
}

// One BSA: its file row, archives row, and a file row (plus header facts)
// per packed file. Packed files take file ids from a range reserved here.
void ProcessArchive(const Shared& sh, size_t index, AssetBatch* batch, AssetQueue* queue) {
    const Loose& f = (*sh.files)[index];
    const Root& r = (*sh.roots)[f.root];
    FileRow row;
    row.id = f.dbId;
    row.path = f.path;
    row.root = r.id;
    row.size = f.size;
    row.mtime = f.mtime;
    row.inData = f.inData;
    auto pl = sh.plan->find(index);
    auto archiveRow = [&](const bsa::Header* h) {
        TableRow a{kArchives, std::vector<Cell>(9)};
        a.cells[0] = Cell::Int(f.dbId);
        if (h) {
            a.cells[1] = Cell::Int(h->version);
            a.cells[2] = Cell::Int(h->archiveFlags);
            a.cells[3] = Cell::Int(h->fileFlags);
            a.cells[4] = Cell::Int(h->fileCount);
            a.cells[5] = Cell::Int((h->archiveFlags & bsa::archive_flags::Compressed) != 0);
        }
        if (pl != sh.plan->end()) {
            a.cells[6] = Cell::Text(pl->second.loadedBy);
            a.cells[7] = OptText(pl->second.plugin);
            a.cells[8] = Cell::Int(pl->second.rank);
        }
        return a;
    };
    const std::wstring full = r.path + Widen(f.path);
    if (sh.hash) {
        Sha256 h;
        std::vector<uint8_t> none;
        std::string err;
        if (ReadLoose(full, 0, f.size, &h, &none, &err)) row.hash = h.Finish();
    }
    bsa::Reader reader;
    bsa::Status s = reader.open(LongPath(full));
    if (s != bsa::Status::Ok) {
        row.error = std::string("bsa: ") + bsa::to_string(s);
        batch->files.push_back(std::move(row));
        batch->rows.push_back(archiveRow(nullptr));
        return;
    }
    const bsa::Header& h = reader.header();
    batch->files.push_back(std::move(row));
    batch->rows.push_back(archiveRow(&h));
    const int64_t base = sh.nextFileId->fetch_add(h.fileCount);
    std::vector<uint8_t> buf;
    int64_t failed = 0;
    for (uint32_t i = 0; i < h.fileCount; ++i) {
        bsa::Entry e;
        std::string_view folder, name;
        if (reader.entry(i, e) != bsa::Status::Ok || reader.folder_name(e.folderIndex, folder) != bsa::Status::Ok ||
            reader.file_name(e, name) != bsa::Status::Ok) {
            ++failed;
            continue;
        }
        FileRow p;
        p.id = base + i;
        std::string rel(folder);
        if (!rel.empty()) rel += '\\';
        rel.append(name);
        p.path = assets::NormalizePath(Utf8Lossy(reinterpret_cast<const uint8_t*>(rel.data()), rel.size()));
        p.root = r.id;
        p.archive = f.dbId;
        p.inData = f.inData;
        uint32_t usize = 0;
        if (reader.uncompressed_size(e, usize) != bsa::Status::Ok) {
            p.error = "bsa: entry size unreadable";
            batch->files.push_back(std::move(p));
            continue;
        }
        p.size = usize;
        const uint8_t* raw = nullptr;
        size_t rawSize = 0;
        bool haveRaw = reader.raw(e, raw, rawSize) == bsa::Status::Ok;
        if (e.compressed && haveRaw) p.packed = static_cast<int64_t>(rawSize);
        const std::string ext = assets::SplitPath(p.path).ext;
        const size_t want = BytesNeeded(ext, sh.textures);
        std::vector<TableRow> rows;
        if (want || sh.hash) {
            const uint8_t* d = nullptr;
            size_t n = 0;
            if (!e.compressed && haveRaw) {  // stored as is: read in place
                d = raw;
                n = rawSize;
            } else {
                bsa::Status rs = reader.read(e, buf, kPackedMax);
                if (rs == bsa::Status::Ok) {
                    d = buf.data();
                    n = buf.size();
                } else {
                    p.error = std::string("bsa: ") + bsa::to_string(rs);
                }
            }
            if (d) {
                if (sh.hash) {
                    Sha256 hh;
                    hh.Add(d, n);
                    p.hash = hh.Finish();
                }
                if (want) {
                    bool more = false;
                    p.error = ReadFacts(p.id, ext, d, n, true, sh.textures, &rows, &more);
                }
            }
        }
        batch->files.push_back(std::move(p));
        for (auto& t : rows) batch->rows.push_back(std::move(t));
        FlushIfFull(batch, queue);
    }
    if (failed) {
        // The entry's names did not read: noted on the archive's own row.
        for (auto& fr : batch->files)
            if (fr.id == f.dbId) fr.error = "bsa: " + std::to_string(failed) + " entries unreadable";
    }
}

// ---- SQL ----

const char kSchema[] = R"SQL(
CREATE TABLE spf_meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;

CREATE TABLE roots(
  root_id    INTEGER PRIMARY KEY,
  kind       TEXT NOT NULL,
  name       TEXT NOT NULL,
  path       TEXT NOT NULL UNIQUE,
  manager    TEXT,
  enabled    INTEGER,
  priority   INTEGER,
  file_count INTEGER,
  total_size INTEGER,
  max_mtime  INTEGER
);

CREATE TABLE paths(
  path_id INTEGER PRIMARY KEY,
  path    TEXT NOT NULL UNIQUE,
  dir     TEXT NOT NULL,
  name    TEXT NOT NULL,
  ext     TEXT NOT NULL,
  kind    TEXT NOT NULL
);

CREATE TABLE files(
  file_id     INTEGER PRIMARY KEY,
  path_id     INTEGER NOT NULL REFERENCES paths,
  root_id     INTEGER NOT NULL REFERENCES roots,
  archive_id  INTEGER REFERENCES files,
  size        INTEGER NOT NULL,
  packed_size INTEGER,
  mtime       INTEGER,
  in_data     INTEGER NOT NULL,
  is_winner   INTEGER NOT NULL,
  hash        BLOB,
  error       TEXT
);

CREATE TABLE archives(
  file_id    INTEGER PRIMARY KEY REFERENCES files,
  version    INTEGER,
  flags      INTEGER,
  file_flags INTEGER,
  file_count INTEGER,
  compressed INTEGER,
  loaded_by  TEXT,
  plugin     TEXT,
  load_rank  INTEGER
);

CREATE TABLE dds(file_id INTEGER PRIMARY KEY, width INTEGER, height INTEGER, format TEXT,
  dxgi INTEGER, mips INTEGER, is_cubemap INTEGER, is_srgb INTEGER, has_alpha INTEGER,
  is_dx10 INTEGER, array_size INTEGER);
CREATE TABLE tga(file_id INTEGER PRIMARY KEY, width INTEGER, height INTEGER, bits INTEGER,
  image_type INTEGER, has_alpha INTEGER, rle INTEGER);
CREATE TABLE nif(file_id INTEGER PRIMARY KEY, version TEXT, user_version INTEGER,
  bs_version INTEGER, block_count INTEGER, shape_count INTEGER, vertex_count INTEGER,
  triangle_count INTEGER, game TEXT);
CREATE TABLE nif_textures(
  file_id INTEGER NOT NULL, idx INTEGER NOT NULL, block TEXT, slot INTEGER,
  texture TEXT NOT NULL,
  path_id INTEGER,
  PRIMARY KEY(file_id, idx)) WITHOUT ROWID;
CREATE TABLE audio(file_id INTEGER PRIMARY KEY, container TEXT, codec TEXT,
  sample_rate INTEGER, channels INTEGER, bits_per_sample INTEGER, avg_bit_rate INTEGER,
  duration REAL, lip_size INTEGER);
CREATE TABLE voice(
  file_id INTEGER PRIMARY KEY, plugin TEXT NOT NULL, voice_type TEXT NOT NULL,
  object_id INTEGER NOT NULL,
  response INTEGER NOT NULL);
CREATE TABLE facegen(
  file_id INTEGER PRIMARY KEY, kind TEXT NOT NULL,
  plugin TEXT NOT NULL, object_id INTEGER NOT NULL);
CREATE TABLE pex(file_id INTEGER PRIMARY KEY, script_name TEXT, parent_script TEXT);
CREATE TABLE hkx(file_id INTEGER PRIMARY KEY, havok_version TEXT, pointer_size INTEGER,
  game TEXT, is_valid INTEGER);
CREATE TABLE tri(file_id INTEGER PRIMARY KEY, version TEXT, vertex_count INTEGER,
  triangle_count INTEGER, uv_count INTEGER, morph_count INTEGER, modifier_count INTEGER);
CREATE TABLE tri_morphs(file_id INTEGER NOT NULL, idx INTEGER NOT NULL,
  kind TEXT NOT NULL,
  name TEXT NOT NULL, PRIMARY KEY(file_id, idx)) WITHOUT ROWID;

CREATE TABLE st_tables(
  table_name  TEXT PRIMARY KEY,
  kind        TEXT NOT NULL,
  record_type TEXT,
  subrecord   TEXT,
  module      TEXT,
  description TEXT
) WITHOUT ROWID;

CREATE TABLE st_columns(
  table_name  TEXT NOT NULL REFERENCES st_tables,
  column_name TEXT NOT NULL,
  position    INTEGER NOT NULL,
  sql_type    TEXT,
  kind        TEXT,
  subrecord   TEXT,
  path        TEXT,
  refers_to   TEXT,
  description TEXT,
  notes       TEXT,
  PRIMARY KEY(table_name, column_name)
) WITHOUT ROWID;
)SQL";

// The copy the game loads. A loose copy in Data wins outright; otherwise the
// copy in the loaded archive with the highest load_rank. Paths the game does
// not see (only in staging, or in archives nothing loads) have no winner.
const char kWinners[] = R"SQL(
UPDATE files SET is_winner = 1 WHERE file_id IN (
  SELECT file_id FROM (
    SELECT f.file_id,
           ROW_NUMBER() OVER (PARTITION BY f.path_id
                              ORDER BY f.archive_id IS NULL DESC, a.load_rank DESC, f.file_id DESC) AS rn
    FROM files f LEFT JOIN archives a ON a.file_id = f.archive_id
    WHERE f.in_data = 1 AND (f.archive_id IS NULL OR a.load_rank IS NOT NULL))
  WHERE rn = 1);
)SQL";

const char kViews[] = R"SQL(
CREATE VIEW winning_files AS
SELECT p.path, p.kind, r.name AS root, r.kind AS root_kind, ap.path AS archive, f.size, f.packed_size, f.mtime,
       f.file_id, f.path_id, f.root_id, f.archive_id
FROM files f
JOIN paths p ON p.path_id = f.path_id
JOIN roots r ON r.root_id = f.root_id
LEFT JOIN files af ON af.file_id = f.archive_id
LEFT JOIN paths ap ON ap.path_id = af.path_id
WHERE f.is_winner;

CREATE VIEW conflicts AS
SELECT p.path, p.kind, r.name AS root, ap.path AS archive, f.in_data, f.is_winner, f.size,
       f.file_id, f.path_id, f.root_id, f.archive_id
FROM files f
JOIN paths p ON p.path_id = f.path_id
JOIN roots r ON r.root_id = f.root_id
LEFT JOIN files af ON af.file_id = f.archive_id
LEFT JOIN paths ap ON ap.path_id = af.path_id
WHERE f.path_id IN (SELECT path_id FROM files GROUP BY path_id HAVING count(*) > 1);
)SQL";

// Extras: optional, on by default; --skip leaves them out. nif_textures is
// read from the meshes; the indexes are plain CREATE INDEX statements.
struct AssetExtra {
    const char* name;
    const char* description;
    const char* sql;  // nullptr = filled while reading
};
const AssetExtra kAssetExtras[] = {
    {"nif_textures", "every texture path a mesh expects (nif_textures rows)", nullptr},
    {"files_path", "index files(path_id): every copy of a path", "CREATE INDEX files_path ON files(path_id);"},
    {"files_root", "index files(root_id): every file of a root", "CREATE INDEX files_root ON files(root_id);"},
    {"files_archive", "index files(archive_id): every file packed in an archive",
     "CREATE INDEX files_archive ON files(archive_id) WHERE archive_id IS NOT NULL;"},
    {"paths_kind", "index paths(kind)", "CREATE INDEX paths_kind ON paths(kind);"},
};

// st_tables rows: name, kind, description.
const char* kTableDocs[][3] = {
    {"spf_meta", "core", "Build settings, sources and counts."},
    {"roots", "core", "Where files come from: the Data folder, each staging (mod) folder, MO2's overwrite folder."},
    {"paths", "core", "Every normalized relative path (lower case, backslashes, relative to Data), once."},
    {"files", "core", "Every copy of every file: loose in a root, or packed in an archive (archive_id)."},
    {"archives", "core", "One row per .bsa (its own files row): header facts and whether / why / in which order the game loads it."},
    {"dds", "detail", "DDS texture headers."},
    {"tga", "detail", "TGA texture headers."},
    {"nif", "detail", "NIF mesh headers."},
    {"nif_textures", "extra", "Every texture path a mesh expects, from its BSShaderTextureSet and BSEffectShaderProperty blocks."},
    {"audio", "detail", "WAV / XWM / FUZ headers."},
    {"voice", "detail", "Voice files, from the path: Sound\\Voice\\<plugin>\\<voicetype>\\<x>_<formid>_<n>.<ext>."},
    {"facegen", "detail", "FaceGen files, from the path: FaceGenData\\FaceGeom|FaceTint\\<plugin>\\<formid>.nif|.dds."},
    {"pex", "detail", "Compiled Papyrus scripts: the script and the script it extends."},
    {"hkx", "detail", "Havok packfile headers."},
    {"tri", "detail", "FaceGen TRI (morph) headers."},
    {"tri_morphs", "detail", "The named morphs (difference morphs) and modifiers (stat morphs) of each TRI."},
    {"st_tables", "core", "The data dictionary: tables."},
    {"st_columns", "core", "The data dictionary: columns."},
    {"winning_files", "view", "The copy of each path the game loads: path, root, archive, size."},
    {"conflicts", "view", "Paths with more than one copy: one row per copy, the winner marked."},
};

// st_columns descriptions (and refers_to, notes) for the columns that need one.
const char* kColumnDocs[][5] = {
    {"spf_meta", "key", "setting name", nullptr, nullptr},
    {"spf_meta", "value", "its value", nullptr, nullptr},
    {"roots", "root_id", "the root", nullptr, nullptr},
    {"roots", "kind", "data | staging | overwrite (MO2)", nullptr, nullptr},
    {"roots", "name", "mod / folder name", nullptr, nullptr},
    {"roots", "path", "absolute folder path", nullptr, nullptr},
    {"roots", "manager", "vortex | mo2 | NULL", nullptr, nullptr},
    {"roots", "enabled", "from the mod manager; NULL = unknown", nullptr, nullptr},
    {"roots", "priority", "MO2 order, higher wins; NULL otherwise", nullptr, nullptr},
    {"roots", "file_count", "loose files in the folder at build time", nullptr, "fingerprint: rescan a root only when file_count, total_size or max_mtime changed"},
    {"roots", "total_size", "their total size in bytes", nullptr, nullptr},
    {"roots", "max_mtime", "their newest mtime, Unix seconds", nullptr, nullptr},
    {"paths", "path", "textures\\armor\\iron\\cuirass.dds", nullptr, "join with the plugin DB on this text"},
    {"paths", "dir", "the folder part ('' at the top)", nullptr, nullptr},
    {"paths", "name", "the file name", nullptr, nullptr},
    {"paths", "ext", "the extension without the dot; '' when none", nullptr, nullptr},
    {"paths", "kind", "texture mesh audio voice script animation plugin archive strings interface facegen bodyslide other", nullptr, nullptr},
    {"files", "path_id", "the path", "paths.path_id", nullptr},
    {"files", "root_id", "the root it is in; packed: the archive's root", "roots.root_id", nullptr},
    {"files", "archive_id", "the .bsa it is packed in; NULL = loose", "files.file_id", nullptr},
    {"files", "size", "bytes; packed: the unpacked size", nullptr, nullptr},
    {"files", "packed_size", "packed and compressed only: the stored size", nullptr, nullptr},
    {"files", "mtime", "loose only, Unix seconds", nullptr, nullptr},
    {"files", "in_data", "1 = the game sees this copy in Data: a loose Data file (or the staging file hardlinked to it, stored once), or packed in such an archive", nullptr, "MO2 with nothing deployed: the virtual Data (enabled mods by priority over Data)"},
    {"files", "is_winner", "1 = the copy the game loads: the loose in_data copy, else the copy in the loaded archive with the highest load_rank", nullptr, nullptr},
    {"files", "hash", "SHA-256 (--hash only); packed: of the unpacked bytes", nullptr, "raw bytes; SELECT hex(col) for hex text"},
    {"files", "error", "why the file or its header could not be read", nullptr, nullptr},
    {"archives", "file_id", "the archive's files row", "files.file_id", nullptr},
    {"archives", "version", "103 / 104 / 105", nullptr, nullptr},
    {"archives", "flags", "archive flags", nullptr, nullptr},
    {"archives", "file_flags", "content types: 1 meshes, 2 textures, 4 menus, 8 sounds, 16 voices, 32 shaders, 64 trees, 128 fonts, 256 misc", nullptr, nullptr},
    {"archives", "file_count", "packed files", nullptr, nullptr},
    {"archives", "compressed", "1 = files are compressed by default", nullptr, nullptr},
    {"archives", "loaded_by", "ini | plugin | NULL (not loaded)", nullptr, nullptr},
    {"archives", "plugin", "the plugin that loads it", nullptr, nullptr},
    {"archives", "load_rank", "order among loaded archives; higher wins", nullptr, nullptr},
    {"dds", "format", "BC1..BC7, R8G8B8A8, ...; a FourCC or 'DXGI n' when not in the list", nullptr, nullptr},
    {"dds", "dxgi", "DXGI_FORMAT number (DX10 headers only)", nullptr, nullptr},
    {"tga", "image_type", "1/2/3 plain, 9/10/11 RLE", nullptr, nullptr},
    {"nif", "version", "\"20.2.0.7\"", nullptr, nullptr},
    {"nif", "bs_version", "Bethesda header version (83 LE, 100 LE and vanilla SE, 130 FO4)", nullptr, "not the game: see game"},
    {"nif", "shape_count", "geometry blocks (BSTriShape family, NiTriShape, NiTriStrips)", nullptr, nullptr},
    {"nif", "vertex_count", "not filled yet", nullptr, "needs beth-asset-metadata support"},
    {"nif", "triangle_count", "not filled yet", nullptr, "needs beth-asset-metadata support"},
    {"nif", "game", "le | se | both | unknown, from the block types (BSTriShape family = se, NiTriShape/NiTriStrips = le, neither = both)", nullptr, nullptr},
    {"nif_textures", "file_id", "the mesh", "files.file_id", nullptr},
    {"nif_textures", "idx", "position within the mesh", nullptr, nullptr},
    {"nif_textures", "block", "BSShaderTextureSet | BSEffectShaderProperty", nullptr, nullptr},
    {"nif_textures", "slot", "position in the texture set (0 diffuse, 1 normal, 2 glow, 3 height, 4 environment, 5 env mask, 6 multilayer, 7 specular / back light); effect shader: 0 source, 1 greyscale", nullptr, nullptr},
    {"nif_textures", "texture", "normalized as in paths, under textures\\", nullptr, nullptr},
    {"nif_textures", "path_id", "NULL when no such file exists anywhere", "paths.path_id", nullptr},
    {"audio", "container", "wav | xwm | fuz", nullptr, nullptr},
    {"audio", "codec", "PCM, WMAv2, ...", nullptr, nullptr},
    {"audio", "avg_bit_rate", "bits per second", nullptr, nullptr},
    {"audio", "duration", "seconds; NULL when unknown", nullptr, nullptr},
    {"audio", "lip_size", "fuz: bytes of embedded lip data", nullptr, nullptr},
    {"voice", "plugin", "the plugin folder in the path", nullptr, nullptr},
    {"voice", "voice_type", "the voice type folder", nullptr, nullptr},
    {"voice", "object_id", "INFO object id in that plugin (low 24 bits of the FormID in the name)", nullptr, nullptr},
    {"voice", "response", "the response number", nullptr, nullptr},
    {"facegen", "kind", "geom | tint", nullptr, nullptr},
    {"facegen", "plugin", "the plugin folder in the path", nullptr, nullptr},
    {"facegen", "object_id", "the NPC's object id (low 24 bits of the file name)", nullptr, nullptr},
    {"pex", "script_name", "the script (object) name", nullptr, nullptr},
    {"pex", "parent_script", "the script it extends; NULL when none", nullptr, nullptr},
    {"hkx", "havok_version", "\"hk_2010.2.0-r1\"", nullptr, nullptr},
    {"hkx", "pointer_size", "4 or 8", nullptr, nullptr},
    {"hkx", "game", "le (4-byte pointers) | se (8)", nullptr, nullptr},
    {"hkx", "is_valid", "1 = a binary packfile the game can load", nullptr, nullptr},
    {"tri", "version", "\"FRTRI003\"", nullptr, nullptr},
    {"tri", "uv_count", "texture coordinates", nullptr, nullptr},
    {"tri", "morph_count", "difference morphs", nullptr, nullptr},
    {"tri", "modifier_count", "stat morphs", nullptr, nullptr},
    {"tri_morphs", "kind", "morph | modifier", nullptr, nullptr},
    {"tri_morphs", "name", "its name", nullptr, nullptr},
};

const char* kDefaultArchives1 =
    "Skyrim - Misc.bsa, Skyrim - Shaders.bsa, Skyrim - Interface.bsa, Skyrim - Animations.bsa, Skyrim - Meshes0.bsa, "
    "Skyrim - Meshes1.bsa, Skyrim - Sounds.bsa";
const char* kDefaultArchives2 =
    "Skyrim - Voices_en0.bsa, Skyrim - Textures0.bsa, Skyrim - Textures1.bsa, Skyrim - Textures2.bsa, Skyrim - "
    "Textures3.bsa, Skyrim - Textures4.bsa, Skyrim - Textures5.bsa, Skyrim - Textures6.bsa, Skyrim - Textures7.bsa, "
    "Skyrim - Textures8.bsa, Skyrim - Patch.bsa";

std::wstring DocumentsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p) == S_OK) out = p;
    CoTaskMemFree(p);
    return out;
}

// The ini archive lists: Skyrim.ini, then SkyrimCustom.ini overriding it
// (MO2 profile-local inis first when they exist); the vanilla list when
// neither sets them. `source` says which.
std::vector<std::string> IniArchives(const std::vector<std::wstring>& iniDirs, std::string* source) {
    std::string list1 = kDefaultArchives1, list2 = kDefaultArchives2;
    std::string from;
    for (const auto& dir : iniDirs) {
        bool any = false;
        for (const wchar_t* name : {L"Skyrim.ini", L"SkyrimCustom.ini"}) {
            std::wstring file = dir + name;
            if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            any = true;
            std::string v;
            bool used = false;
            if (IniValue(file, "Archive", "sResourceArchiveList", &v)) list1 = v, used = true;
            if (IniValue(file, "Archive", "sResourceArchiveList2", &v)) list2 = v, used = true;
            if (used) from += (from.empty() ? "" : " + ") + Utf8(file);
        }
        if (any) break;  // the first folder that has inis is the one the game reads
    }
    *source = from.empty() ? "default vanilla list (no Skyrim.ini / SkyrimCustom.ini sets one)" : from;
    std::vector<std::string> out = assets::SplitArchiveList(list1);
    for (auto& n : assets::SplitArchiveList(list2)) out.push_back(n);
    return out;
}

struct Mo2Layout {
    std::wstring mods, overwrite, profileDir;
    std::vector<std::pair<std::string, int>> modlist;  // name, +1 enabled / 0 disabled; highest priority first
};

// ModOrganizer.ini's folders (custom base / mod / overwrite / profiles
// directories, %BASE_DIR%), and the profile's modlist.txt.
bool ReadMo2(const std::wstring& instance, const std::wstring& profile, Mo2Layout* out, std::string* error) {
    const std::wstring inst = FullDir(instance);
    const std::wstring ini = inst + L"ModOrganizer.ini";
    std::string base, v;
    auto dirSetting = [&](const char* key, const wchar_t* def) {
        std::wstring d;
        if (IniValue(ini, "Settings", key, &v) && !v.empty()) {
            size_t k = v.find("%BASE_DIR%");
            if (k != std::string::npos) v.replace(k, 10, base);
            d = Widen(v);
        } else {
            d = Widen(base) + L"\\" + def;
        }
        return FullDir(d);
    };
    base = Utf8(inst.substr(0, inst.size() - 1));
    if (IniValue(ini, "Settings", "base_directory", &v) && !v.empty()) base = v;
    out->mods = dirSetting("mod_directory", L"mods");
    out->overwrite = dirSetting("overwrite_directory", L"overwrite");
    out->profileDir = dirSetting("profiles_directory", L"profiles") + profile + L"\\";
    if (!IsDir(out->mods)) {
        *error = "MO2: no mods folder at " + Utf8(out->mods);
        return false;
    }
    std::vector<std::string> lines;
    if (!ReadTextFile(out->profileDir + L"modlist.txt", &lines)) {
        *error = "MO2: cannot read " + Utf8(out->profileDir) + "modlist.txt";
        return false;
    }
    for (const auto& l : lines) {
        if (l.size() < 2 || l[0] == '#') continue;
        if (l[0] == '+' || l[0] == '-') out->modlist.push_back({l.substr(1), l[0] == '+' ? 1 : 0});
    }
    return true;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> AssetExtras() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : kAssetExtras) out.push_back({e.name, e.description});
    return out;
}

namespace assets {

std::vector<ArchiveLoad> PlanArchives(const std::vector<std::string>& archivesInData,
                                      const std::vector<std::string>& iniList,
                                      const std::vector<std::string>& loadOrder) {
    std::unordered_map<std::string, size_t> byLower;
    for (size_t i = 0; i < archivesInData.size(); ++i) byLower.emplace(Lower(archivesInData[i]), i);
    std::vector<ArchiveLoad> out;
    std::set<size_t> loaded;
    auto claim = [&](const std::string& name, const char* by, const std::string& plugin) {
        auto it = byLower.find(Lower(name));
        if (it == byLower.end() || !loaded.insert(it->second).second) return;
        out.push_back({archivesInData[it->second], by, plugin, static_cast<int>(out.size())});
    };
    for (const auto& n : iniList) claim(n, "ini", "");
    for (const auto& p : loadOrder) {
        size_t dot = p.rfind('.');
        std::string base = dot == std::string::npos ? p : p.substr(0, dot);
        claim(base + ".bsa", "plugin", p);
        claim(base + " - Textures.bsa", "plugin", p);
    }
    return out;
}

}  // namespace assets

int BuildAssets(const AssetOptions& opt, std::string* error) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    auto say = [&](const char* fmt, ...) {
        if (opt.quiet) return;
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        fprintf(stderr, "[%6.2fs] ", s);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    };
    std::set<std::string> extras;
    for (const auto& e : kAssetExtras) extras.insert(e.name);
    for (const auto& s : opt.skip) {
        if (!extras.count(s)) {
            *error = "--skip: '" + s + "' is not an extra (spf build-assets --help lists them)";
            return 2;
        }
        extras.erase(s);
    }
    if (!opt.mo2.empty() && !opt.staging.empty()) {
        *error = "give --staging or --mo2, not both (MO2's mods folder is its staging)";
        return 2;
    }
    if (!opt.mo2.empty() && opt.profile.empty()) {
        *error = "--mo2 needs --profile";
        return 2;
    }
    const std::wstring data = FullDir(opt.data);
    if (!IsDir(data)) {
        *error = "no Data folder at " + Utf8(data);
        return 1;
    }

    // Roots: Data first, then staging folders (by name) or MO2 mods.
    std::vector<Root> roots;
    {
        Root d;
        d.kind = "data";
        d.name = "Data";
        d.path = data;
        roots.push_back(d);
    }
    Mo2Layout mo2;
    const bool isMo2 = !opt.mo2.empty();
    std::wstring stagingRoot;
    std::string manager;
    if (isMo2) {
        if (!ReadMo2(opt.mo2, opt.profile, &mo2, error)) return 1;
        stagingRoot = mo2.mods;
        manager = "mo2";
    } else if (!opt.staging.empty()) {
        stagingRoot = FullDir(opt.staging);
        if (!IsDir(stagingRoot)) {
            *error = "no staging folder at " + Utf8(stagingRoot);
            return 1;
        }
        if (GetFileAttributesW((stagingRoot + L"__vortex_staging_folder").c_str()) != INVALID_FILE_ATTRIBUTES)
            manager = "vortex";
    }
    if (!stagingRoot.empty()) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((stagingRoot + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != L"." && n != L"..") names.push_back(n);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        std::unordered_map<std::string, std::pair<int, int64_t>> order;  // lower name -> enabled, priority
        for (size_t i = 0; i < mo2.modlist.size(); ++i)
            order[Lower(mo2.modlist[i].first)] = {mo2.modlist[i].second,
                                                  static_cast<int64_t>(mo2.modlist.size() - i)};
        for (const auto& n : names) {
            Root r;
            r.kind = "staging";
            r.name = Utf8(n);
            r.path = stagingRoot + n + L"\\";
            r.manager = manager;
            if (isMo2) {
                auto it = order.find(Lower(r.name));
                r.enabled = it != order.end() ? it->second.first : 0;
                if (it != order.end()) r.priority = it->second.second, r.hasPriority = true;
            }
            roots.push_back(r);
        }
        if (isMo2 && IsDir(mo2.overwrite)) {
            Root o;
            o.kind = "overwrite";
            o.name = "Overwrite";
            o.path = mo2.overwrite;
            o.manager = "mo2";
            o.enabled = 1;
            o.priority = static_cast<int64_t>(mo2.modlist.size()) + 1;
            o.hasPriority = true;
            roots.push_back(o);
        }
    }
    for (size_t i = 0; i < roots.size(); ++i) roots[i].id = static_cast<int64_t>(i) + 1;
    const int threads = opt.threads > 0 ? opt.threads
                                        : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);

    // Phase 1: walk every root in parallel (biggest first: Data, then by
    // index, which keeps the pool busy enough for thousands of mod folders).
    std::vector<std::vector<Loose>> walked(roots.size());
    {
        std::atomic<size_t> next{0};
        std::atomic<int64_t> reparse{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < std::min<int>(threads, static_cast<int>(roots.size())); ++t) {
            pool.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (size_t w; (w = next++) < roots.size();) WalkRoot(&roots[w], &walked[w], static_cast<int>(w), &reparse);
            });
        }
        for (auto& t : pool) t.join();
        int64_t total = 0;
        for (const auto& w : walked) total += static_cast<int64_t>(w.size());
        say("walked %zu roots: %lld files%s", roots.size(), static_cast<long long>(total),
            reparse ? (", " + std::to_string(reparse.load()) + " linked folders not followed").c_str() : "");
        if (!roots[0].walkError.empty()) {
            *error = "cannot read the Data folder";
            return 1;
        }
    }

    // Phase 2: in_data. A staging file that is the same physical file as the
    // Data file at its path (hardlink: same volume and file ID) is stored once,
    // with its staging root. Unmatched Data files keep the Data root.
    int64_t hardlinks = 0;
    std::vector<char> claimed(walked[0].size(), 0);
    {
        std::unordered_map<std::string, size_t> dataIdx;
        dataIdx.reserve(walked[0].size());
        for (size_t i = 0; i < walked[0].size(); ++i) dataIdx.emplace(walked[0][i].path, i);
        for (size_t r = 1; r < roots.size(); ++r) {
            for (auto& f : walked[r]) {
                auto it = dataIdx.find(f.path);
                if (it == dataIdx.end() || claimed[it->second]) continue;
                const Loose& d = walked[0][it->second];
                if (d.fileId == f.fileId && roots[0].volume == roots[r].volume && d.fileId != 0) {
                    claimed[it->second] = 1;
                    f.inData = true;
                    ++hardlinks;
                }
            }
        }
        for (auto& f : walked[0]) f.inData = true;
    }
    // MO2 with nothing deployed: the virtual Data is the enabled roots by
    // priority over the real Data; the top copy of each path is in_data.
    const bool virtualData = isMo2 && hardlinks == 0;
    if (virtualData) {
        std::unordered_map<std::string, std::pair<int64_t, Loose*>> top;
        for (size_t r = 0; r < roots.size(); ++r) {
            if (r > 0 && roots[r].enabled != 1) continue;
            const int64_t pri = r == 0 ? LLONG_MIN : roots[r].hasPriority ? roots[r].priority : LLONG_MIN + 1;
            for (auto& f : walked[r]) {
                f.inData = false;
                auto& slot = top[f.path];
                if (!slot.second || pri > slot.first) slot = {pri, &f};
            }
        }
        for (auto& kv : top) kv.second.second->inData = true;
    } else if (isMo2) {
        say("warning: MO2 files are deployed in Data (%lld hardlinks): winners from Data, not the profile",
            static_cast<long long>(hardlinks));
    }

    // One list of every loose file stored, file ids 1..n in root order.
    std::vector<Loose> files;
    {
        size_t n = 0;
        for (const auto& w : walked) n += w.size();
        files.reserve(n);
        for (size_t r = 0; r < walked.size(); ++r) {
            for (size_t i = 0; i < walked[r].size(); ++i) {
                if (r == 0 && claimed[i]) continue;
                files.push_back(std::move(walked[r][i]));
            }
            std::vector<Loose>().swap(walked[r]);
        }
        for (size_t i = 0; i < files.size(); ++i) files[i].dbId = static_cast<int64_t>(i) + 1;
    }
    say("in_data: %lld staging files hardlinked to Data%s", static_cast<long long>(hardlinks),
        virtualData ? "; MO2 virtual Data" : "");

    // The load order (plugins in the visible Data) and which archives load.
    BuildOptions lo;
    lo.data = data;
    lo.loadOrderFile = opt.loadOrderFile;
    if (lo.loadOrderFile.empty() && isMo2 &&
        GetFileAttributesW((mo2.profileDir + L"plugins.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        lo.loadOrderFile = mo2.profileDir + L"plugins.txt";
    std::vector<std::pair<std::string, std::wstring>> present;
    std::vector<std::string> archiveNames;
    std::vector<size_t> archiveIdx;
    for (size_t i = 0; i < files.size(); ++i) {
        const Loose& f = files[i];
        if (!f.inData || f.topName.empty()) continue;
        const std::string ext = assets::SplitPath(f.path).ext;
        if (ext == "esm" || ext == "esp" || ext == "esl")
            present.push_back({f.topName, roots[f.root].path + Widen(f.topName)});
        if (ext == "bsa") {
            archiveNames.push_back(f.topName);
            archiveIdx.push_back(i);
        }
    }
    std::string loErr;
    std::vector<std::string> order = LoadOrder(lo, virtualData ? &present : nullptr, &loErr);
    if (!loErr.empty()) {
        *error = loErr;
        return 1;
    }
    std::vector<std::wstring> iniDirs;
    if (isMo2) iniDirs.push_back(mo2.profileDir);
    const std::wstring docs = DocumentsDir();
    if (!docs.empty()) iniDirs.push_back(docs + L"\\My Games\\Skyrim Special Edition\\");
    std::string iniSource;
    std::vector<std::string> iniList = IniArchives(iniDirs, &iniSource);
    std::unordered_map<size_t, ArchivePlanEntry> plan;
    {
        std::unordered_map<std::string, size_t> byName;
        for (size_t k = 0; k < archiveNames.size(); ++k) byName[Lower(archiveNames[k])] = archiveIdx[k];
        for (const auto& a : assets::PlanArchives(archiveNames, iniList, order))
            plan[byName.at(Lower(a.name))] = {a.loadedBy, a.plugin, a.rank};
    }
    say("load order: %zu plugins; %zu of %zu archives in Data load (archive lists: %s)", order.size(), plan.size(),
        archiveNames.size(), iniSource.c_str());

    // The database, written beside the target and moved over it at the end.
    std::wstring tmp = opt.out + L".building";
    if (!DeleteFileW(tmp.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION) {
        *error = Utf8(tmp) + " is in use: another build is writing this database";
        return 1;
    }
    int64_t fileRows = 0, errors = 0, pathCount = 0;
    {
        Db db;
        if (!db.Open(tmp, error)) return 1;
        if (!db.Exec("PRAGMA page_size=8192; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
                     "PRAGMA locking_mode=EXCLUSIVE; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-262144;",
                     error) ||
            !db.Exec(kSchema, error) || !db.Exec("BEGIN", error))
            return 1;
        Stmt insRoot{db.Prepare("INSERT INTO roots VALUES(?,?,?,?,?,?,?,?,?,?)", error)};
        Stmt insPath{db.Prepare("INSERT INTO paths VALUES(?,?,?,?,?,?)", error)};
        Stmt insFile{db.Prepare("INSERT INTO files VALUES(?,?,?,?,?,?,?,?,0,?,?)", error)};
        Stmt insVoice{db.Prepare("INSERT INTO voice VALUES(?,?,?,?,?)", error)};
        Stmt insFace{db.Prepare("INSERT INTO facegen VALUES(?,?,?,?)", error)};
        if (!insRoot.st || !insPath.st || !insFile.st || !insVoice.st || !insFace.st) return 1;
        std::vector<Stmt> insTable(kTableCount);
        for (int t = 0; t < kTableCount; ++t) {
            std::string sql = std::string("INSERT INTO ") + kTableNames[t] + " VALUES(";
            for (int c = 0; c < kTableColumns[t]; ++c) sql += c ? ",?" : "?";
            insTable[t].st = db.Prepare((sql + ")").c_str(), error);
            if (!insTable[t].st) return 1;
        }
        for (const auto& r : roots) {
            insRoot.Int(1, r.id).Text(2, r.kind).Text(3, r.name).Text(4, Utf8(r.path));
            if (r.manager.empty()) insRoot.Null(5);
            else insRoot.Text(5, r.manager);
            if (r.enabled < 0) insRoot.Null(6);
            else insRoot.Int(6, r.enabled);
            if (r.hasPriority) insRoot.Int(7, r.priority);
            else insRoot.Null(7);
            insRoot.Int(8, r.fileCount).Int(9, r.totalSize).Int(10, r.maxMtime);
            if (!insRoot.Run()) {
                *error = db.Message();
                return 1;
            }
        }

        // Writes one file row: its path (interned), and the facts its path
        // alone gives (voice, FaceGen).
        std::unordered_map<std::string, int64_t> pathIds;
        pathIds.reserve(files.size() + 1024);
        std::string firstError;
        auto writeFile = [&](const FileRow& f) {
            auto it = pathIds.find(f.path);
            const bool fresh = it == pathIds.end();
            if (fresh) {
                const int64_t id = static_cast<int64_t>(pathIds.size()) + 1;
                it = pathIds.emplace(f.path, id).first;
                assets::PathParts pp = assets::SplitPath(f.path);
                insPath.Int(1, id).Text(2, f.path).Text(3, pp.dir).Text(4, pp.name).Text(5, pp.ext)
                    .Text(6, assets::PathKind(f.path));
                if (!insPath.Run() && firstError.empty()) firstError = db.Message();
            }
            insFile.Int(1, f.id).Int(2, it->second).Int(3, f.root);
            if (f.archive < 0) insFile.Null(4);
            else insFile.Int(4, f.archive);
            insFile.Int(5, f.size);
            if (f.packed < 0) insFile.Null(6);
            else insFile.Int(6, f.packed);
            if (f.mtime < 0 || f.archive >= 0) insFile.Null(7);
            else insFile.Int(7, f.mtime);
            insFile.Int(8, f.inData);
            if (f.hash.empty()) insFile.Null(9);
            else insFile.Blob(9, f.hash);
            if (f.error.empty()) insFile.Null(10);
            else insFile.Text(10, f.error), ++errors;
            if (!insFile.Run() && firstError.empty()) firstError = db.Message();
            ++fileRows;
            if (f.path.compare(0, 12, "sound\\voice\\") == 0) {
                assets::VoicePath v;
                if (assets::ParseVoicePath(f.path, &v))
                    insVoice.Int(1, f.id).Text(2, v.plugin).Text(3, v.voiceType).Int(4, v.objectId).Int(5, v.response)
                        .Run();
            } else if (f.path.find("facegendata\\") != std::string::npos) {
                assets::FaceGenPath g;
                if (assets::ParseFaceGenPath(f.path, &g))
                    insFace.Int(1, f.id).Text(2, g.kind).Text(3, g.plugin).Int(4, g.objectId).Run();
            }
        };

        // Phase 3: read workers (largest first) -> this thread writes. Loose
        // files with no reader (and no hash wanted) need no work.
        std::vector<size_t> work;
        std::vector<char> isArchive(files.size(), 0);
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string ext = assets::SplitPath(files[i].path).ext;
            if (ext == "bsa") isArchive[i] = 1;
            if (isArchive[i] || opt.hash || BytesNeeded(ext, false)) {
                work.push_back(i);
            } else {
                FileRow row;
                row.id = files[i].dbId;
                row.path = files[i].path;
                row.root = roots[files[i].root].id;
                row.size = files[i].size;
                row.mtime = files[i].mtime;
                row.inData = files[i].inData;
                writeFile(row);
            }
        }
        std::sort(work.begin(), work.end(), [&](size_t a, size_t b) {
            if (isArchive[a] != isArchive[b]) return isArchive[a] > isArchive[b];
            return files[a].size > files[b].size;
        });
        std::atomic<int64_t> nextFileId{static_cast<int64_t>(files.size()) + 1};
        Shared sh{&roots, &files, &plan, opt.hash, extras.count("nif_textures") != 0, &nextFileId};
        const int workers = std::max(1, std::min<int>(threads, static_cast<int>(work.size())));
        AssetQueue queue;
        queue.SetProducers(workers);
        std::atomic<size_t> nextWork{0};
        std::atomic<size_t> archivesDone{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < workers; ++t) {
            pool.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                AssetBatch batch;
                for (size_t w; (w = nextWork++) < work.size();) {
                    const size_t i = work[w];
                    // A file that cannot be read is recorded with the reason;
                    // it never takes the build (or the process) down.
                    std::string failure;
                    try {
                        if (isArchive[i]) {
                            ProcessArchive(sh, i, &batch, &queue);
                            ++archivesDone;
                        } else {
                            ProcessLoose(sh, i, &batch);
                        }
                    } catch (const std::exception& e) {
                        failure = e.what();
                    } catch (...) {
                        failure = "unexpected error";
                    }
                    if (!failure.empty()) {
                        bool have = false;
                        for (auto& fr : batch.files)
                            if (fr.id == files[i].dbId) fr.error = "failed while reading: " + failure, have = true;
                        if (!have) {
                            FileRow row;
                            row.id = files[i].dbId;
                            row.path = files[i].path;
                            row.root = roots[files[i].root].id;
                            row.size = files[i].size;
                            row.mtime = files[i].mtime;
                            row.inData = files[i].inData;
                            row.error = "failed while reading: " + failure;
                            batch.files.push_back(std::move(row));
                        }
                    }
                    FlushIfFull(&batch, &queue);
                }
                if (batch.Size()) queue.Push(std::move(batch));
                queue.ProducerDone();
            });
        }
        say("reading %zu files (%zu archives) with %d threads", work.size(),
            static_cast<size_t>(std::count(isArchive.begin(), isArchive.end(), 1)), workers);
        AssetBatch b;
        int64_t detailRows = 0;
        auto lastReport = Clock::now();
        while (queue.Pop(&b)) {
            for (const auto& f : b.files) writeFile(f);
            for (const auto& r : b.rows) {
                if (!BindRun(insTable[r.table].st, r.cells) && firstError.empty())
                    firstError = std::string(kTableNames[r.table]) + ": " + db.Message();
                ++detailRows;
            }
            if (!opt.quiet && Clock::now() - lastReport > std::chrono::seconds(30)) {
                lastReport = Clock::now();
                say("  %lld file rows, %zu/%zu work items started", static_cast<long long>(fileRows),
                    std::min(nextWork.load(), work.size()), work.size());
            }
        }
        for (auto& t : pool) t.join();
        if (!firstError.empty()) {
            *error = firstError;
            return 1;
        }
        pathCount = static_cast<int64_t>(pathIds.size());
        say("wrote %lld file rows (%lld paths), %lld detail rows, %lld errors", static_cast<long long>(fileRows),
            static_cast<long long>(pathCount), static_cast<long long>(detailRows), static_cast<long long>(errors));

        std::string sql;
        for (const auto& e : kAssetExtras)
            if (e.sql && extras.count(e.name)) sql += e.sql;
        if (!sql.empty() && !db.Exec(sql, error)) return 1;
        say("indexes");
        if (!db.Exec(kWinners, error)) return 1;
        if (extras.count("nif_textures") &&
            !db.Exec("UPDATE nif_textures SET path_id = (SELECT p.path_id FROM paths p WHERE p.path = nif_textures.texture);",
                     error))
            return 1;
        if (!db.Exec(kViews, error)) return 1;
        say("winners");

        // The data dictionary.
        Stmt insT{db.Prepare("INSERT INTO st_tables VALUES(?,?,NULL,NULL,?,?)", error)};
        Stmt setC{db.Prepare("UPDATE st_columns SET description=?, refers_to=?, notes=? WHERE table_name=? AND column_name=?",
                             error)};
        if (!insT.st || !setC.st) return 1;
        for (const auto& t : kTableDocs) {
            insT.Text(1, t[0]).Text(2, t[1]);
            if (std::string(t[1]) == "extra") insT.Text(3, t[0]);
            else insT.Null(3);
            insT.Text(4, t[2]).Run();
        }
        if (!db.Exec("INSERT INTO st_columns SELECT m.table_name, p.name, p.cid, p.type, NULL, NULL, NULL, NULL, NULL, NULL "
                     "FROM st_tables m, pragma_table_info(m.table_name) p;"
                     "UPDATE st_columns SET description = 'the file', refers_to = 'files.file_id' "
                     "WHERE column_name = 'file_id' AND table_name NOT IN ('files', 'winning_files', 'conflicts');",
                     error))
            return 1;
        for (const auto& c : kColumnDocs) {
            setC.Text(1, c[2]);
            if (c[3]) setC.Text(2, c[3]);
            else setC.Null(2);
            if (c[4]) setC.Text(3, c[4]);
            else setC.Null(3);
            setC.Text(4, c[0]).Text(5, c[1]).Run();
        }

        Stmt meta{db.Prepare("INSERT INTO spf_meta VALUES(?,?)", error)};
        auto putMeta = [&](const char* k, const std::string& v) { meta.Text(1, k).Text(2, v).Run(); };
        putMeta("schema_version", std::to_string(kAssetSchemaVersion));
        putMeta("built_by", "spf build-assets");
        putMeta("built_at", NowIso());
        putMeta("sqlite_version", sqlite3_libversion());
        putMeta("data_dir", Utf8(data));
        putMeta("staging_root", isMo2 ? "" : Utf8(stagingRoot));
        putMeta("mo2_instance", isMo2 ? Utf8(FullDir(opt.mo2)) : "");
        putMeta("mo2_profile", isMo2 ? Utf8(opt.profile) : "");
        putMeta("mo2_virtual_data", virtualData ? "1" : "0");
        putMeta("load_order_source", !lo.loadOrderFile.empty() ? Utf8(lo.loadOrderFile) : "every plugin in Data");
        putMeta("load_order_count", std::to_string(order.size()));
        putMeta("archive_list_source", iniSource);
        putMeta("hashed", opt.hash ? "1" : "0");
        putMeta("hardlink_matches", std::to_string(hardlinks));
        putMeta("count_roots", std::to_string(roots.size()));
        putMeta("count_files", std::to_string(fileRows));
        putMeta("count_paths", std::to_string(pathCount));
        putMeta("count_errors", std::to_string(errors));
        std::string skipped;
        for (const auto& e : kAssetExtras)
            if (!extras.count(e.name)) skipped += (skipped.empty() ? "" : ",") + std::string(e.name);
        putMeta("modules_not_built", skipped);
        if (!db.Exec("COMMIT; ANALYZE;", error)) return 1;
        say("committed");
    }
    if (!MoveFileExW(tmp.c_str(), opt.out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = "cannot replace " + Utf8(opt.out) + " (open in another program?)";
        return 1;
    }
    say("done: %s", Utf8(opt.out).c_str());
    return 0;
}

}  // namespace spf
