#include "strings.h"

#include <windows.h>

#include <bsa/reader.hpp>

#include <cstring>
#include <vector>

#include "pattern_value.h"

namespace skqe {
namespace {

// Bethesda's largest strings file is a few MB; anything this size is damage.
constexpr size_t kMaxStringsFile = size_t{64} << 20;

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) <= kMaxStringsFile;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out->empty() ||
             (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

// One file from a BSA, through bsa-reader: open, read, close (a mapping held
// open would stop other tools rewriting the archive). Any failure is a miss.
bool ReadFromBsa(const std::wstring& bsaPath, const std::string& path, std::vector<uint8_t>* out) {
    bsa::Reader r;
    if (r.open(bsaPath) != bsa::Status::Ok) return false;
    bsa::Entry e;
    return r.find(path, e) == bsa::Status::Ok && r.read(e, *out, kMaxStringsFile) == bsa::Status::Ok;
}

}  // namespace

bool StringTables::Parse(int c, const uint8_t* data, size_t size) {
    if (c < 0 || c > 2 || size < 8) return false;
    uint64_t count = U32(data), dataSize = U32(data + 4);
    uint64_t dirEnd = 8 + count * 8;
    if (dirEnd + dataSize > size) return false;
    const uint8_t* blob = data + dirEnd;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t id = U32(data + 8 + i * 8), off = U32(data + 12 + i * 8);
        if (off >= dataSize) continue;
        const uint8_t* s = blob + off;
        size_t len;
        if (c == 0) {  // zero-terminated
            len = strnlen(reinterpret_cast<const char*>(s), static_cast<size_t>(dataSize - off));
        } else {  // dl / il: length-prefixed
            if (off + 4 > dataSize) continue;
            len = U32(s);
            s += 4;
            if (off + 4 + static_cast<uint64_t>(len) > dataSize) continue;
            while (len && s[len - 1] == 0) --len;
        }
        tables_[c][id] = Utf8Lossy(s, len);
    }
    return true;
}

void StringTables::Load(const std::wstring& dir, const std::string& pluginBaseName, const std::string& language) {
    static const char* kExt[3] = {"strings", "dlstrings", "ilstrings"};
    std::string base;
    for (char ch : pluginBaseName) base += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    std::wstring wbase(base.begin(), base.end());
    for (int c = 0; c < 3; ++c) {
        std::string fn = base + "_" + language + "." + kExt[c];
        std::wstring wfn(fn.begin(), fn.end());
        std::vector<uint8_t> data;
        if (!ReadWholeFile(dir + L"Strings\\" + wfn, &data) &&
            !ReadFromBsa(dir + wbase + L".bsa", "strings\\" + fn, &data) &&
            !ReadFromBsa(dir + L"Skyrim - Interface.bsa", "strings\\" + fn, &data)) {
            continue;
        }
        Parse(c, data.data(), data.size());
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

}  // namespace skqe
