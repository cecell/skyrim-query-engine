#include "asset_readers.h"

#include <windows.h>

#include <cstring>

#include "pattern_value.h"

namespace spf {
namespace assets {
namespace {

// Little-endian / big-endian cursor over a byte range. Every read checks the
// bytes left; a failed read sets `bad` and returns 0, so a parse can run to
// its end and check once.
struct Cursor {
    const uint8_t* d;
    size_t n, pos = 0;
    bool big = false;
    bool bad = false;

    bool Has(size_t k) {
        if (bad || k > n - pos) {
            bad = true;
            return false;
        }
        return true;
    }
    uint8_t U8() { return Has(1) ? d[pos++] : 0; }
    uint16_t U16() {
        if (!Has(2)) return 0;
        const uint8_t* p = d + pos;
        pos += 2;
        return big ? static_cast<uint16_t>(p[0] << 8 | p[1]) : static_cast<uint16_t>(p[0] | p[1] << 8);
    }
    uint32_t U32() {
        if (!Has(4)) return 0;
        const uint8_t* p = d + pos;
        pos += 4;
        return big ? (static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3])
                   : (p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24);
    }
    void Skip(uint64_t k) {
        if (k > n - pos) bad = true;
        if (!bad) pos += static_cast<size_t>(k);
    }
    // `len` bytes as text (Windows-1252 / UTF-8 to UTF-8), NULs trimmed.
    std::string Text(size_t len) {
        if (!Has(len)) return {};
        const uint8_t* p = d + pos;
        pos += len;
        return Utf8Lossy(p, strnlen(reinterpret_cast<const char*>(p), len));
    }
};

bool StartsWith(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

bool IsHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Path components split on '\'.
std::vector<std::string> Components(const std::string& path) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= path.size()) {
        size_t q = path.find('\\', p);
        if (q == std::string::npos) q = path.size();
        out.push_back(path.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

}  // namespace

std::string NormalizePath(const std::string& utf8) {
    std::string s;
    bool ascii = true;
    for (char c : utf8) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    if (ascii) {
        s.reserve(utf8.size());
        for (char c : utf8) s.push_back(c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c))));
    } else {
        // Invariant lower case through the wide form, as NTFS folds names.
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        if (n > 0)
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), n, w.data(), n, nullptr, nullptr, 0);
        for (auto& c : w)
            if (c == L'/') c = L'\\';
        int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
        s.assign(m, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), n, s.data(), m, nullptr, nullptr);
    }
    size_t lead = 0;
    while (lead < s.size() && s[lead] == '\\') ++lead;
    return s.substr(lead);
}

std::string NormalizeTexturePath(const std::string& raw) {
    std::string s = NormalizePath(raw);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\\')) s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && (s[lead] == ' ' || s[lead] == '\\')) ++lead;
    s.erase(0, lead);
    if (s.empty()) return s;
    // Absolute or Data-relative paths left by exporters: keep from textures\.
    size_t t = s.find("\\textures\\");
    if (!StartsWith(s, "textures\\") && t != std::string::npos) s.erase(0, t + 1);
    if (StartsWith(s, "data\\")) s.erase(0, 5);
    if (!StartsWith(s, "textures\\")) s = "textures\\" + s;
    return s;
}

PathParts SplitPath(const std::string& path) {
    PathParts p;
    size_t slash = path.rfind('\\');
    p.dir = slash == std::string::npos ? "" : path.substr(0, slash);
    p.name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = p.name.rfind('.');
    p.ext = dot == std::string::npos || dot == 0 ? "" : p.name.substr(dot + 1);
    return p;
}

const char* PathKind(const std::string& path) {
    const std::string ext = SplitPath(path).ext;
    if (ext == "esm" || ext == "esp" || ext == "esl") return "plugin";
    if (ext == "bsa" || ext == "ba2") return "archive";
    if (path.find("facegendata\\") != std::string::npos) return "facegen";
    if (StartsWith(path, "sound\\voice\\")) return "voice";
    if (StartsWith(path, "calientetools\\bodyslide\\")) return "bodyslide";
    if (ext == "hkx" || (StartsWith(path, "meshes\\") && path.find("\\animations\\") != std::string::npos))
        return "animation";
    if (StartsWith(path, "textures\\") || ext == "dds" || ext == "tga") return "texture";
    if (StartsWith(path, "meshes\\") || ext == "nif" || ext == "tri") return "mesh";
    if (StartsWith(path, "scripts\\") || StartsWith(path, "source\\scripts\\") || ext == "pex" || ext == "psc")
        return "script";
    if (StartsWith(path, "sound\\") || StartsWith(path, "music\\") || ext == "wav" || ext == "xwm" || ext == "fuz" ||
        ext == "lip")
        return "audio";
    if (StartsWith(path, "strings\\") || ext == "strings" || ext == "dlstrings" || ext == "ilstrings") return "strings";
    if (StartsWith(path, "interface\\") || ext == "swf") return "interface";
    return "other";
}

bool ParseVoicePath(const std::string& path, VoicePath* out) {
    auto c = Components(path);
    if (c.size() != 5 || c[0] != "sound" || c[1] != "voice" || c[2].empty() || c[3].empty()) return false;
    PathParts p = SplitPath(path);
    if (p.ext != "fuz" && p.ext != "wav" && p.ext != "xwm" && p.ext != "lip") return false;
    std::string stem = p.name.substr(0, p.name.size() - p.ext.size() - 1);
    size_t last = stem.rfind('_');
    if (last == std::string::npos || last < 9) return false;
    std::string n = stem.substr(last + 1), id = stem.substr(last - 8, 8);
    if (n.empty() || n.size() > 4 || stem[last - 9] != '_' || !IsHex(id)) return false;
    for (char ch : n)
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    out->plugin = c[2];
    out->voiceType = c[3];
    out->objectId = static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 16)) & 0xFFFFFF;
    out->response = atoi(n.c_str());
    return true;
}

bool ParseFaceGenPath(const std::string& path, FaceGenPath* out) {
    auto c = Components(path);
    if (c.size() != 7 || c[1] != "actors" || c[2] != "character" || c[3] != "facegendata" || c[5].empty())
        return false;
    PathParts p = SplitPath(path);
    std::string stem = p.name.substr(0, p.name.size() - (p.ext.empty() ? 0 : p.ext.size() + 1));
    if (stem.size() != 8 || !IsHex(stem)) return false;
    if (c[0] == "meshes" && c[4] == "facegeom" && p.ext == "nif") out->kind = "geom";
    else if (c[0] == "textures" && c[4] == "facetint" && p.ext == "dds") out->kind = "tint";
    else return false;
    out->plugin = c[5];
    out->objectId = static_cast<uint32_t>(strtoul(stem.c_str(), nullptr, 16)) & 0xFFFFFF;
    return true;
}

std::vector<std::string> SplitArchiveList(const std::string& value) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= value.size()) {
        size_t q = value.find(',', p);
        if (q == std::string::npos) q = value.size();
        std::string item = value.substr(p, q - p);
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
        p = q + 1;
    }
    return out;
}

// ---- Papyrus ----

bool ReadPex(const uint8_t* d, size_t n, PexInfo* out, std::string* error) {
    Cursor c{d, n};
    c.big = true;
    uint32_t magic = c.U32();
    if (c.bad || magic != 0xFA57C0DE) {
        *error = magic == 0xDEC057FA ? "pex: little-endian (Fallout 4), not a Skyrim script" : "pex: not a compiled script";
        return false;
    }
    out->major = c.U8();
    out->minor = c.U8();
    out->gameId = c.U16();
    c.Skip(8);                                          // compilation time
    for (int i = 0; i < 3; ++i) c.Skip(c.U16());        // source file, user, machine
    const uint16_t count = c.U16();
    std::vector<std::string> strings;
    strings.reserve(count);
    for (uint16_t i = 0; i < count && !c.bad; ++i) strings.push_back(c.Text(c.U16()));
    if (c.U8()) {  // debug info
        c.Skip(8);
        const uint16_t functions = c.U16();
        for (uint16_t i = 0; i < functions && !c.bad; ++i) {
            c.Skip(7);  // object, state, function name; type
            c.Skip(uint64_t{c.U16()} * 2);  // line numbers
        }
    }
    const uint16_t flags = c.U16();
    c.Skip(uint64_t{flags} * 3);
    const uint16_t objects = c.U16();
    const uint16_t name = c.U16();
    c.Skip(4);  // object size
    const uint16_t parent = c.U16();
    if (c.bad || objects == 0) {
        *error = "pex: truncated or damaged";
        return false;
    }
    if (name >= strings.size() || parent >= strings.size()) {
        *error = "pex: object names outside the string table";
        return false;
    }
    out->scriptName = strings[name];
    out->parentScript = strings[parent];
    if (out->gameId != 1) *error = "pex: game id " + std::to_string(out->gameId) + " (Skyrim is 1)";
    return true;
}

// ---- Havok ----

bool ReadHkx(const uint8_t* d, size_t n, HkxInfo* out, std::string* error) {
    Cursor c{d, n};
    uint32_t m0 = c.U32(), m1 = c.U32();
    if (c.bad) {
        *error = "hkx: truncated";
        return false;
    }
    if (m0 == 0x57E0E057 && m1 == 0x10C0C010) {  // binary packfile
        if (n < kHkxProbeBytes) {
            *error = "hkx: truncated packfile header";
            return false;
        }
        out->pointerSize = d[16];
        const bool little = d[17] == 1;
        out->havokVersion = Utf8Lossy(d + 40, strnlen(reinterpret_cast<const char*>(d + 40), 16));
        out->valid = little && (out->pointerSize == 4 || out->pointerSize == 8) && !out->havokVersion.empty();
        if (!out->valid) *error = "hkx: unexpected packfile layout";
        return true;
    }
    if ((m0 == 0xCAB00D1E && m1 == 0xD011FACE) || (m0 == 0x1E0DB0CA)) {
        *error = "hkx: tagfile (the game loads packfiles)";
        return true;
    }
    if (n >= 5 && (memcmp(d, "<?xml", 5) == 0 || memcmp(d, "\xEF\xBB\xBF<?x", 6) == 0)) {
        *error = "hkx: XML (the game loads binary packfiles)";
        return true;
    }
    *error = "hkx: not a Havok file";
    return false;
}

// ---- FaceGen TRI ----

bool ReadTri(const uint8_t* d, size_t n, TriInfo* out, std::string* error) {
    if (n >= 6 && memcmp(d, "PIRT", 4) == 0) {
        // BodySlide / Outfit Studio morphs: per shape, named morphs of sparse
        // vertex offsets (u16 index + 3 x int16). A UV-morph section may
        // follow; it is not read.
        Cursor c{d, n};
        c.Skip(4);
        out->version = "PIRT";
        out->bodySlide = true;
        const uint16_t shapes = c.U16();
        for (uint16_t s = 0; s < shapes && !c.bad; ++s) {
            std::string shape = c.Text(c.U8());
            const uint16_t morphs = c.U16();
            for (uint16_t m = 0; m < morphs && !c.bad; ++m) {
                std::string morph = c.Text(c.U8());
                c.Skip(4);                      // multiplier
                c.Skip(uint64_t{c.U16()} * 8);  // vertex offsets
                if (!c.bad) out->morphs.push_back(shape + ":" + morph);
            }
        }
        out->morphCount = static_cast<uint32_t>(out->morphs.size());
        if (c.bad) *error = "tri: BodySlide morphs run past the end of the file";
        return true;
    }
    if (n < 64 || memcmp(d, "FRTRI", 5) != 0) {
        *error = "tri: not a FaceGen or BodySlide TRI";
        return false;
    }
    Cursor c{d, n};
    out->version = c.Text(8);
    const uint32_t v = c.U32(), t = c.U32(), q = c.U32(), lv = c.U32(), ls = c.U32(), x = c.U32(), ext = c.U32();
    const uint32_t md = c.U32(), ms = c.U32(), k = c.U32();
    c.Skip(16);
    out->vertexCount = v;
    out->triangleCount = t;
    out->uvCount = x;
    out->morphCount = md;
    out->modifierCount = ms;
    // The body: vertices (with the stat morphs' extra ones), faces, labels,
    // texture coordinates, then the named morphs. 64-bit sums: counts come
    // from the file.
    c.Skip((uint64_t{v} + k) * 12);
    c.Skip(uint64_t{t} * 12 + uint64_t{q} * 16);
    for (uint32_t i = 0; i < lv && !c.bad; ++i) {
        c.Skip(4);
        c.Skip(c.U32());
    }
    for (uint32_t i = 0; i < ls && !c.bad; ++i) {
        c.Skip(12);
        c.Skip(c.U32());
    }
    // Texture coordinates, then per-face texture indices whenever there are
    // coordinates (every file in the corpus: ext bit 0 does not decide it).
    (void)ext;
    if (x > 0) c.Skip(uint64_t{x} * 8 + uint64_t{t} * 12 + uint64_t{q} * 16);
    auto name = [&] {
        uint32_t len = c.U32();
        return len > 4096 ? (c.bad = true, std::string()) : c.Text(len);
    };
    for (uint32_t i = 0; i < md && !c.bad; ++i) {
        out->morphs.push_back(name());
        c.Skip(4 + uint64_t{v} * 6);  // scale, 3 int16 per vertex
    }
    for (uint32_t i = 0; i < ms && !c.bad; ++i) {
        out->modifiers.push_back(name());
        c.Skip(uint64_t{c.U32()} * 4);
    }
    if (c.bad) {
        out->morphs.clear();
        out->modifiers.clear();
        *error = "tri: body does not fit the header's counts (names not read)";
    }
    return true;
}

}  // namespace assets
}  // namespace spf
