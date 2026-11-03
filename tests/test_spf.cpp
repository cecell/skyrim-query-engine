// Unit tests: hand-built plugins and strings files, valid and damaged. Every
// damaged input must be refused or skipped cleanly, never crash or allocate
// what the file claims.
//
//   spf_tests [patterns.json]   (default: patterns.json next to the exe)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "inflate.h"
#include "plugin.h"
#include "strings.h"

using namespace spf;

namespace {

int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_passed;                                                   \
        } else {                                                          \
            ++g_failed;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// Little-endian byte builder for plugin structures.
struct Bytes {
    std::vector<uint8_t> b;
    Bytes& raw(const void* p, size_t n) {
        b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return *this;
    }
    Bytes& sig(const char* s) { return raw(s, 4); }
    Bytes& u8(uint8_t v) { return raw(&v, 1); }
    Bytes& u16(uint16_t v) { return raw(&v, 2); }
    Bytes& u32(uint32_t v) { return raw(&v, 4); }
    Bytes& f32(float v) { return raw(&v, 4); }
    Bytes& add(const Bytes& o) { return raw(o.b.data(), o.b.size()); }
    size_t size() const { return b.size(); }
};

Bytes Sub(const char* sig, const Bytes& data) {
    Bytes s;
    s.sig(sig).u16(static_cast<uint16_t>(data.size())).add(data);
    return s;
}

Bytes Zstring(const char* text) {
    Bytes s;
    return s.raw(text, strlen(text) + 1);
}

Bytes Rec(const char* type, uint32_t formId, const Bytes& body, uint32_t flags = 0) {
    Bytes r;
    r.sig(type).u32(static_cast<uint32_t>(body.size())).u32(flags).u32(formId).u32(0).u16(44).u16(0).add(body);
    return r;
}

Bytes Group(const char* label, uint32_t type, const Bytes& content, uint32_t sizeOverride = 0) {
    Bytes g;
    g.sig("GRUP").u32(sizeOverride ? sizeOverride : static_cast<uint32_t>(24 + content.size())).raw(label, 4).u32(type);
    g.u32(0).u32(0).add(content);
    return g;
}

Bytes Header(const std::vector<const char*>& masters) {
    Bytes hedr;
    hedr.f32(1.71f).u32(1).u32(0x801);
    Bytes body = Sub("HEDR", hedr);
    for (const char* m : masters) body.add(Sub("MAST", Zstring(m))).add(Sub("DATA", Bytes().u32(0).u32(0)));
    return Rec("TES4", 0, body);
}

Bytes Weapon(uint32_t formId, const char* edid, uint16_t damage) {
    Bytes data;
    data.u32(25).f32(9.0f).u16(damage);
    return Rec("WEAP", formId, Sub("EDID", Zstring(edid)).add(Sub("DATA", data)));
}

std::unique_ptr<Plugin> Load(const Bytes& b, const PatternCatalog& cat, std::string* err) {
    err->clear();
    return Plugin::FromMemory(b.b.data(), b.size(), "Test.esp", cat, err);
}

const Field* FindField(const std::vector<Field>& fs, const char* sig) {
    for (const auto& f : fs)
        if (f.sig == sig) return &f;
    return nullptr;
}

// zlib stream with one stored (uncompressed) block, which the inflater must
// accept like any other.
Bytes StoredZlib(const Bytes& data) {
    Bytes z;
    z.u8(0x78).u8(0x01).u8(0x01).u16(static_cast<uint16_t>(data.size())).u16(static_cast<uint16_t>(~data.size()));
    z.add(data);
    uint32_t a = 1, b = 0;
    for (uint8_t c : data.b) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    return z.u8(adler >> 24).u8((adler >> 16) & 0xFF).u8((adler >> 8) & 0xFF).u8(adler & 0xFF);
}

void TestValidPlugin(const PatternCatalog& cat) {
    Bytes p = Header({"Skyrim.esm"});
    p.add(Group("WEAP", 0, Weapon(0x01000800, "TestSword", 7).add(Weapon(0x00012EB7, "IronSword", 9))));
    std::string err;
    auto plugin = Load(p, cat, &err);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    CHECK(plugin->masters().size() == 1 && plugin->masters()[0] == "Skyrim.esm");
    CHECK(plugin->records().size() == 3);  // TES4 + 2
    const Record* r = plugin->Find(0x01000800);
    CHECK(r != nullptr);
    if (!r) return;
    auto fields = plugin->Fields(*r);
    const Field* edid = FindField(fields, "EDID");
    const Field* data = FindField(fields, "DATA");
    CHECK(edid && edid->decoded && edid->value.AsString() == "TestSword");
    CHECK(data && data->decoded);
    if (data && data->decoded) {
        const Value* dmg = data->value.Find("damage");
        CHECK(dmg && dmg->AsInt() == 7);
    }
    CHECK(plugin->FormKey(0x00012EB7) == "Skyrim.esm:012EB7");
    CHECK(plugin->FormKey(0x01000800) == "Test.esp:000800");
}

void TestDamagedStructure(const PatternCatalog& cat) {
    std::string err;
    Bytes good = Header({}).add(Group("WEAP", 0, Weapon(0x800, "A", 1)));

    // Not a plugin / truncated header.
    CHECK(Load(Bytes().sig("ABCD").u32(0).u32(0).u32(0).u32(0).u32(0), cat, &err) == nullptr);
    Bytes shortHdr = Header({});
    shortHdr.b.resize(30);
    CHECK(Load(shortHdr, cat, &err) == nullptr);

    // A record cut off at the end of the file.
    Bytes cut = good;
    cut.b.resize(cut.size() - 3);
    CHECK(Load(cut, cat, &err) == nullptr && err.find("damaged") != std::string::npos);

    // Group size smaller than its own header, and larger than the file.
    CHECK(Load(Header({}).add(Group("WEAP", 0, Weapon(0x800, "A", 1), 10)), cat, &err) == nullptr);
    CHECK(Load(Header({}).add(Group("WEAP", 0, Weapon(0x800, "A", 1), 0x7FFFFFFF)), cat, &err) == nullptr);

    // A nested group that claims more than its parent holds.
    Bytes inner = Group("CELL", 6, Weapon(0x800, "A", 1));
    Bytes outerContent = inner;
    uint32_t badInner = static_cast<uint32_t>(inner.size() + 100);
    memcpy(outerContent.b.data() + 4, &badInner, 4);
    Bytes outer = Group("CELL", 0, outerContent);
    outer.add(Weapon(0x900, "Pad", 1)).add(Weapon(0x901, "Pad2", 1)).add(Weapon(0x902, "Pad3", 1));
    CHECK(Load(Header({}).add(outer), cat, &err) == nullptr);

    // Header MAST running past the header must not be read as a master.
    Bytes mastBody = Sub("HEDR", Bytes().f32(1.71f).u32(0).u32(0x800));
    mastBody.sig("MAST").u16(200).raw("Skyrim.esm", 10);
    Bytes mast = Rec("TES4", 0, mastBody).add(Group("WEAP", 0, Weapon(0x800, "A", 1)));
    auto pm = Load(mast, cat, &err);
    CHECK(pm == nullptr || pm->masters().empty());
}

void TestDamagedFields(const PatternCatalog& cat) {
    std::string err;
    // Compressed record claiming a 4 GB body.
    Bytes huge;
    huge.u32(0xFFFFFFF0u).add(StoredZlib(Sub("EDID", Zstring("X"))));
    Bytes p1 = Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, huge, 0x00040000)));
    auto plugin = Load(p1, cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) CHECK(plugin->Fields(*plugin->Find(0x800)).empty());

    // Compressed record with the right size decodes.
    Bytes inner = Sub("EDID", Zstring("Packed"));
    Bytes ok;
    ok.u32(static_cast<uint32_t>(inner.size())).add(StoredZlib(inner));
    Bytes p2 = Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, ok, 0x00040000)));
    plugin = Load(p2, cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) {
        auto fs = plugin->Fields(*plugin->Find(0x800));
        const Field* edid = FindField(fs, "EDID");
        CHECK(edid && edid->value.AsString() == "Packed");
    }

    // Compressed record with garbage instead of a zlib stream.
    Bytes junk;
    junk.u32(100).u32(0xDEADBEEF).u32(0x12345678);
    plugin = Load(Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, junk, 0x00040000))), cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) CHECK(plugin->Fields(*plugin->Find(0x800)).empty());

    // XXXX announcing a subrecord larger than the record.
    Bytes xxxx = Sub("EDID", Zstring("A"));
    xxxx.add(Sub("XXXX", Bytes().u32(0x7FFFFFFF))).sig("DATA").u16(0);
    plugin = Load(Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, xxxx))), cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) {
        auto fs = plugin->Fields(*plugin->Find(0x800));
        CHECK(fs.size() == 1 && fs[0].sig == "EDID");
    }

    // A subrecord length running past the record.
    Bytes over = Sub("EDID", Zstring("A"));
    over.sig("DATA").u16(5000).u32(1);
    plugin = Load(Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, over))), cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) CHECK(plugin->Fields(*plugin->Find(0x800)).size() == 1);

    // A list count (n_pfx) of 4 billion over 8 bytes: not decoded, kept raw.
    Bytes mo2s;
    mo2s.u32(0xFFFFFFFFu).u32(0);
    plugin = Load(Header({}).add(Group("ARMO", 0, Rec("ARMO", 0x800, Sub("MO2S", mo2s)))), cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) {
        auto fs = plugin->Fields(*plugin->Find(0x800));
        CHECK(fs.size() == 1 && !fs[0].decoded && fs[0].value.AsString()[0] == '#');
    }

    // Data too short for the pattern: raw hex, not a crash.
    plugin = Load(Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, Sub("DATA", Bytes().u8(1))))), cat, &err);
    CHECK(plugin != nullptr);
    if (plugin) {
        auto fs = plugin->Fields(*plugin->Find(0x800));
        CHECK(fs.size() == 1 && !fs[0].decoded);
    }
}

void TestInflate() {
    std::vector<uint8_t> out;
    Bytes data;
    data.raw("hello hello hello", 17);
    Bytes z = StoredZlib(data);
    CHECK(ZlibInflate(z.b.data(), z.size(), 17, &out) && out.size() == 17 && memcmp(out.data(), "hello", 5) == 0);
    CHECK(!ZlibInflate(z.b.data(), z.size(), 16, &out));           // wrong size
    CHECK(!ZlibInflate(z.b.data(), z.size(), 0xFFFFFFFFu, &out));  // impossible claim, refused before allocating
    CHECK(!ZlibInflate(z.b.data(), 1, 17, &out));                  // truncated
    Bytes bad = z;
    bad.b[0] = 0x00;  // not a zlib header
    CHECK(!ZlibInflate(bad.b.data(), bad.size(), 17, &out));
}

void TestStrings() {
    // Two entries: one fine, one pointing past the data.
    Bytes s;
    s.u32(2).u32(6).u32(1).u32(0).u32(2).u32(100).raw("Hello", 6);
    StringTables t;
    CHECK(t.Parse(0, s.b.data(), s.size()));
    const std::string* h = t.Find(1, "strings");
    CHECK(h && *h == "Hello");
    CHECK(t.Find(2, "strings") == nullptr);

    // A directory count that does not fit the file.
    Bytes huge;
    huge.u32(0x40000000).u32(0).u32(1).u32(0);
    StringTables t2;
    CHECK(!t2.Parse(0, huge.b.data(), huge.size()));

    // dlstrings entry whose length prefix runs past the data.
    Bytes dl;
    dl.u32(1).u32(8).u32(7).u32(0).u32(1000).raw("abcd", 4);
    StringTables t3;
    CHECK(t3.Parse(1, dl.b.data(), dl.size()));
    CHECK(t3.Find(7, "dlstrings") == nullptr);

    // Too short, wrong table index.
    StringTables t4;
    CHECK(!t4.Parse(0, s.b.data(), 4));
    CHECK(!t4.Parse(5, s.b.data(), s.size()));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring patterns;
    if (argc > 1) {
        patterns = argv[1];
    } else {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        patterns = exe;
        patterns = patterns.substr(0, patterns.find_last_of(L"\\/") + 1) + L"patterns.json";
    }
    std::ifstream f(patterns, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(ss.str(), &err)) {
        fprintf(stderr, "cannot load patterns: %s\n", err.c_str());
        return 2;
    }
    TestValidPlugin(cat);
    TestDamagedStructure(cat);
    TestDamagedFields(cat);
    TestInflate();
    TestStrings();
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
