#include "strings.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "pattern_value.h"

namespace spf {
namespace {

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    bool ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    CloseHandle(h);
    return ok;
}

// One file from a BSA (v104 Oldrim / v105 SSE). Only uncompressed entries,
// which is how Bethesda stores strings.
bool ReadFromBsa(const std::wstring& bsaPath, const std::string& folder, const std::string& file,
                 std::vector<uint8_t>* out) {
    std::vector<uint8_t> bsa;
    HANDLE h = CreateFileW(bsaPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto readAt = [h](uint64_t off, void* buf, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
    };
    bool ok = false;
    uint8_t hdr[36];
    if (readAt(0, hdr, 36) && memcmp(hdr, "BSA\0", 4) == 0) {
        uint32_t version = U32(hdr + 4), flags = U32(hdr + 12), folders = U32(hdr + 16);
        uint32_t fileNamesLen = U32(hdr + 28);
        size_t recSize = version == 105 ? 24 : 16;
        std::vector<uint8_t> recs(folders * recSize);
        auto folderOffset = [&](const uint8_t* r) -> uint64_t {
            return (version == 105 ? (U32(r + 16) | (static_cast<uint64_t>(U32(r + 20)) << 32)) : U32(r + 12)) -
                   fileNamesLen;
        };
        if ((version == 104 || version == 105) && (flags & 3) == 3 &&
            readAt(36, recs.data(), static_cast<DWORD>(recs.size()))) {
            // File names follow all the folder blocks (name + file records).
            uint64_t blocks = 0;
            for (uint32_t k = 0; k < folders; ++k) {
                const uint8_t* rk = recs.data() + k * recSize;
                uint8_t nl = 0;
                readAt(folderOffset(rk), &nl, 1);
                blocks += 1 + nl + 16ull * U32(rk + 8);
            }
            std::vector<char> names(fileNamesLen);
            readAt(36 + recs.size() + blocks, names.data(), fileNamesLen);
            uint64_t before = 0;
            for (uint32_t i = 0; i < folders && !ok; ++i) {
                const uint8_t* r = recs.data() + i * recSize;
                uint32_t count = U32(r + 8);
                uint64_t off = folderOffset(r);
                uint8_t nameLen = 0;
                readAt(off, &nameLen, 1);
                std::string name(nameLen, '\0');
                readAt(off + 1, name.data(), nameLen);
                if (Lower(std::string(name.c_str())) == folder) {
                    std::vector<uint8_t> files(count * 16);
                    readAt(off + 1 + nameLen, files.data(), static_cast<DWORD>(files.size()));
                    size_t pos = 0;
                    for (uint64_t k = 0; k < before && pos < names.size(); ++k) pos += strlen(names.data() + pos) + 1;
                    for (uint32_t k = 0; k < count && pos < names.size(); ++k) {
                        std::string fn = Lower(std::string(names.data() + pos));
                        pos += fn.size() + 1;
                        if (fn != file) continue;
                        uint32_t size = U32(files.data() + k * 16 + 8);
                        uint32_t dataOff = U32(files.data() + k * 16 + 12);
                        bool compressed = ((flags & 4) != 0) != ((size & 0x40000000) != 0);
                        size &= 0x3FFFFFFF;
                        if (compressed) break;
                        uint64_t at = dataOff;
                        if (flags & 0x100) {  // embedded file name before the data
                            uint8_t bl = 0;
                            readAt(at, &bl, 1);
                            at += 1 + bl;
                            size -= 1 + bl;
                        }
                        out->resize(size);
                        ok = readAt(at, out->data(), size);
                        break;
                    }
                    break;
                }
                before += count;
            }
        }
    }
    CloseHandle(h);
    return ok;
}

}  // namespace

void StringTables::Load(const std::wstring& dir, const std::string& pluginBaseName, const std::string& language) {
    static const char* kExt[3] = {"strings", "dlstrings", "ilstrings"};
    std::string base = Lower(pluginBaseName);
    std::wstring wbase(base.begin(), base.end());
    for (int c = 0; c < 3; ++c) {
        std::string fn = base + "_" + language + "." + kExt[c];
        std::wstring wfn(fn.begin(), fn.end());
        std::vector<uint8_t> data;
        if (!ReadWholeFile(dir + L"Strings\\" + wfn, &data) &&
            !ReadFromBsa(dir + wbase + L".bsa", "strings", fn, &data) &&
            !ReadFromBsa(dir + L"Skyrim - Interface.bsa", "strings", fn, &data)) {
            continue;
        }
        if (data.size() < 8) continue;
        uint32_t count = U32(data.data()), dataSize = U32(data.data() + 4);
        size_t dirEnd = 8 + static_cast<size_t>(count) * 8;
        if (dirEnd + dataSize > data.size()) continue;
        const uint8_t* blob = data.data() + dirEnd;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = U32(data.data() + 8 + i * 8), off = U32(data.data() + 12 + i * 8);
            if (off >= dataSize) continue;
            const uint8_t* s = blob + off;
            size_t len;
            if (c == 0) {
                len = strnlen(reinterpret_cast<const char*>(s), dataSize - off);
            } else {  // dl/il: length-prefixed
                if (off + 4 > dataSize) continue;
                len = U32(s);
                s += 4;
                if (off + 4 + len > dataSize) continue;
                while (len && s[len - 1] == 0) --len;
            }
            tables_[c][id] = Utf8Lossy(s, len);
        }
    }
}

const std::string* StringTables::Find(uint32_t id, const std::string& cls) const {
    int first = cls == "dlstrings" ? 1 : cls == "ilstrings" ? 2 : 0;
    for (int k = 0; k < 3; ++k) {
        int c = k == 0 ? first : (k <= first ? k - 1 : k);
        auto it = tables_[c].find(id);
        if (it != tables_[c].end()) return &it->second;
    }
    return nullptr;
}

}  // namespace spf
