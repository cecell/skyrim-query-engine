// Unit tests: hand-built plugins and strings files, valid and damaged. Every
// damaged input must be refused or skipped cleanly, never crash or allocate
// what the file claims. build-assets: its readers on synthetic bytes (every
// truncation), path rules, and winners on a fixture tree (a Data folder with
// hardlinks, staging folders, BSAs, an MO2 instance) in %TEMP%.
//
//   skqe_tests [patterns.json]   (default: patterns.json next to the exe)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "asset_build.h"
#include "asset_readers.h"
#include "asset_search.h"
#include "db_schema.h"
#include "export_request.h"
#include "inflate.h"
#include "plugin.h"
#include "sqlite3.h"
#include "staging_find.h"
#include "strings.h"

using namespace skqe;

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

// TES4 header facts, including an ONAM too large for a 16-bit size (XXXX), with
// INTV and INCC after it.
void TestTextDecoding() {
    auto text = [](const std::string& s) {
        return TextToUtf8(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    CHECK(text("") == "");
    CHECK(text("Iron Sword") == "Iron Sword");
    CHECK(text("T\xC3\xA9st \xE2\x82\xAC") == "T\xC3\xA9st \xE2\x82\xAC");  // valid UTF-8 stays
    CHECK(text("\xDCnicode \xC4\xD6") == "\xC3\x9Cnicode \xC3\x84\xC3\x96");  // Windows-1252
    CHECK(text("\x80 \x93hi\x94 \x85") == "\xE2\x82\xAC \xE2\x80\x9Chi\xE2\x80\x9D \xE2\x80\xA6");
    CHECK(text("\x81") == "\xC2\x81");  // undefined in Windows-1252, but not lost
    CHECK(text("caf\xC3") == "caf\xC3\x83");  // cut off UTF-8 isn't UTF-8
    CHECK(text("\xC3\xA9 \xE9") == "\xC3\x83\xC2\xA9 \xC3\xA9");  // one bad byte: the whole string is 1252
    CHECK(text("\xED\xA0\x80").find("\xEF\xBF\xBD") == std::string::npos);  // no replacement characters
    for (int b = 0x80; b <= 0xFF; ++b) {
        const std::string one(1, static_cast<char>(b));
        CHECK(IsValidUtf8(reinterpret_cast<const uint8_t*>(text(one).data()), text(one).size()));
    }
}

void TestHeaderFacts(const PatternCatalog& cat) {
    Bytes hedr;
    hedr.f32(1.70f).u32(5).u32(0x1234);
    Bytes onam;
    for (uint32_t i = 0; i < 20000; ++i) onam.u32(i);
    Bytes body = Sub("HEDR", hedr).add(Sub("CNAM", Zstring("Someone")));
    body.add(Sub("MAST", Zstring("Skyrim.esm"))).add(Sub("DATA", Bytes().u32(0x89ABCDEF).u32(1)));
    body.add(Sub("MAST", Zstring("Update.esm"))).add(Sub("DATA", Bytes().u32(0).u32(0)));
    body.add(Sub("XXXX", Bytes().u32(static_cast<uint32_t>(onam.size()))));
    body.sig("ONAM").u16(0).add(onam);
    body.add(Sub("INTV", Bytes().u32(7))).add(Sub("INCC", Bytes().u32(9)));
    std::string err;
    {
        // A plugin name outside ASCII is UTF-8 in FormKeys (JSON must stay valid).
        Bytes own = Header({});
        auto named = Plugin::FromMemory(own.b.data(), own.size(), "T\xE9st.esp", cat, &err);
        CHECK(named != nullptr);
        if (named) {
            CHECK(named->nameUtf8() == "T\xC3\xA9st.esp");
            CHECK(named->FormKey(0x000800) == "T\xC3\xA9st.esp:000800");
        }
    }
    auto plugin = Load(Rec("TES4", 0, body, 0x281), cat, &err);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    const PluginHeader& h = plugin->info();
    CHECK(h.version > 1.69f && h.version < 1.71f);
    CHECK(h.flags == 0x281 && h.esm && h.esl && h.localized);
    CHECK(h.nextObjectId == 0x1234 && h.numRecords == 5);
    CHECK(plugin->header().formVersion == 44);
    CHECK(h.author == "Someone");
    CHECK(plugin->masters().size() == 2 && h.masterSizes.size() == 2);
    CHECK(h.masterSizes[0] == 0x189ABCDEFull && h.masterSizes[1] == 0);
    CHECK(h.overrideCount == 20000);
    CHECK(h.intv && *h.intv == 7);
    CHECK(h.incc && *h.incc == 9);

    // A header without them, and an XXXX claiming more than the header holds.
    auto plain = Load(Header({}), cat, &err);
    CHECK(plain != nullptr);
    if (plain) CHECK(plain->info().overrideCount == 0 && !plain->info().intv && !plain->info().incc);
    Bytes lie = Sub("HEDR", hedr).add(Sub("XXXX", Bytes().u32(0xFFFFFFFF)));
    lie.sig("ONAM").u16(0);
    auto bad = Load(Rec("TES4", 0, lie), cat, &err);
    if (bad) CHECK(bad->info().overrideCount == 0);
}

void TestExportRequest() {
    std::vector<ExportTarget> t;
    std::string err;
    std::string text =
        "\xEF\xBB\xBF# a comment\r\n\r\n[Skyrim.esm]\r\nSkyrim.esm:0132AA, Skyrim.esm:01367B\r\n  00012EB7 000800\r\n"
        "[C:\\mods\\My Mod [v2].esp]\nMy Mod [v2].esp:000800 , Skyrim.esm:01,\n[Empty.esp]\n";
    CHECK(ParseExportRequest(text, &t, &err));
    CHECK(t.size() == 3);
    if (t.size() == 3) {
        CHECK(t[0].plugin == "Skyrim.esm" && t[0].forms.size() == 4);
        if (t[0].forms.size() == 4)
            CHECK(t[0].forms[1] == "Skyrim.esm:01367B" && t[0].forms[2] == "00012EB7" && t[0].forms[3] == "000800");
        CHECK(t[1].plugin == "C:\\mods\\My Mod [v2].esp" && t[1].forms.size() == 2);
        if (t[1].forms.size() == 2) CHECK(t[1].forms[0] == "My Mod [v2].esp:000800");
        CHECK(t[2].plugin == "Empty.esp" && t[2].forms.empty());
    }
    std::vector<ExportTarget> none;
    CHECK(!ParseExportRequest("000800\n[A.esp]\n", &none, &err));
    none.clear();
    CHECK(!ParseExportRequest("[A.esp\n", &none, &err));
    none.clear();
    CHECK(!ParseExportRequest("# nothing\n", &none, &err));
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

    // Header facts.
    const PluginHeader& h = plugin->info();
    CHECK(h.version > 1.70f && h.version < 1.72f);
    CHECK(h.numRecords == 1 && h.nextObjectId == 0x801);
    CHECK(!h.esm && !h.esl && !h.localized && !h.master && !h.light);

    // A Windows-1252 master name comes out as UTF-8 too.
    Bytes accented = Header({"Caf\xE9.esm"});
    accented.add(Group("WEAP", 0, Weapon(0x01000800, "A", 1)));
    auto pa = Load(accented, cat, &err);
    CHECK(pa && pa->masters()[0] == "Caf\xE9.esm" && pa->mastersUtf8()[0] == "Caf\xC3\xA9.esm");
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

// ---- build-assets: readers, path rules, winners ----

// Big-endian builder (Papyrus).
struct BeBytes {
    std::vector<uint8_t> b;
    BeBytes& u8(uint8_t v) {
        b.push_back(v);
        return *this;
    }
    BeBytes& u16(uint16_t v) { return u8(static_cast<uint8_t>(v >> 8)).u8(static_cast<uint8_t>(v)); }
    BeBytes& u32(uint32_t v) { return u16(static_cast<uint16_t>(v >> 16)).u16(static_cast<uint16_t>(v)); }
    BeBytes& str(const char* s) {
        u16(static_cast<uint16_t>(strlen(s)));
        b.insert(b.end(), s, s + strlen(s));
        return *this;
    }
};

void TestAssetPaths() {
    using namespace assets;
    CHECK(NormalizePath("Textures/Armor/X.DDS") == "textures\\armor\\x.dds");
    CHECK(NormalizePath("\\Meshes\\A.nif") == "meshes\\a.nif");
    CHECK(NormalizePath("Textures/\xC3\x84rmel.dds") == "textures\\\xC3\xA4rmel.dds");  // Ä -> ä
    CHECK(NormalizeTexturePath("Armor\\Iron\\Cuirass.dds") == "textures\\armor\\iron\\cuirass.dds");
    CHECK(NormalizeTexturePath("Data\\Textures\\A.dds") == "textures\\a.dds");
    CHECK(NormalizeTexturePath("C:\\Skyrim\\Data\\textures\\b.dds") == "textures\\b.dds");
    CHECK(NormalizeTexturePath("  ") == "");
    CHECK(NormalizeAssetPath("Clutter\\Key01.NIF", "meshes\\") == "meshes\\clutter\\key01.nif");
    CHECK(NormalizeAssetPath("Meshes/Clutter/Key01.nif", "meshes\\") == "meshes\\clutter\\key01.nif");
    CHECK(NormalizeAssetPath("Sound\\FX\\a.wav", "sound\\") == "sound\\fx\\a.wav");
    CHECK(NormalizeAssetPath("C:\\Games\\Data\\Music\\x.wav", "music\\") == "music\\x.wav");
    // some plugins store a path with a trailing CR or CRLF (or a tab)
    CHECK(NormalizeAssetPath("Armor\\Circlets\\a.nif\r", "meshes\\") == "meshes\\armor\\circlets\\a.nif");
    CHECK(NormalizeAssetPath("Sound\\FX\\b.wav\r\n", "sound\\") == "sound\\fx\\b.wav");
    CHECK(NormalizeAssetPath("\tc.nif ", "meshes\\") == "meshes\\c.nif");
    PathParts p = SplitPath("textures\\armor\\x.dds");
    CHECK(p.dir == "textures\\armor" && p.name == "x.dds" && p.ext == "dds");
    CHECK(SplitPath("readme").ext == "" && SplitPath("readme").dir == "");
    CHECK(std::string(PathKind("textures\\a.dds")) == "texture");
    CHECK(std::string(PathKind("meshes\\a.nif")) == "mesh");
    CHECK(std::string(PathKind("meshes\\actors\\character\\animations\\a.hkx")) == "animation");
    CHECK(std::string(PathKind("sound\\voice\\skyrim.esm\\malenord\\a_0001_1.fuz")) == "voice");
    CHECK(std::string(PathKind("sound\\fx\\a.wav")) == "audio");
    CHECK(std::string(PathKind("scripts\\a.pex")) == "script");
    CHECK(std::string(PathKind("skyrim.esm")) == "plugin");
    CHECK(std::string(PathKind("skyrim - misc.bsa")) == "archive");
    CHECK(std::string(PathKind("meshes\\actors\\character\\facegendata\\facegeom\\a.esp\\00000800.nif")) == "facegen");
    CHECK(std::string(PathKind("calientetools\\bodyslide\\sliderpresets\\a.xml")) == "bodyslide");
    CHECK(std::string(PathKind("skse\\plugins\\a.dll")) == "other");

    VoicePath v;
    CHECK(ParseVoicePath("sound\\voice\\skyrim.esm\\malenord\\dialoguege_dialoguegener_00013ec5_1.fuz", &v));
    CHECK(v.plugin == "skyrim.esm" && v.voiceType == "malenord" && v.objectId == 0x013EC5 && v.response == 1);
    CHECK(ParseVoicePath("sound\\voice\\a.esp\\femaleyoung\\x_y_fe012345_12.lip", &v) && v.objectId == 0x012345 &&
          v.response == 12);
    CHECK(!ParseVoicePath("sound\\voice\\a.esp\\femaleyoung\\readme.txt", &v));
    CHECK(!ParseVoicePath("sound\\voice\\a.esp\\x_00013ec5_1.fuz", &v));          // no voice type folder
    CHECK(!ParseVoicePath("sound\\voice\\a.esp\\vt\\x_0001zec5_1.fuz", &v));      // not hex
    FaceGenPath g;
    CHECK(ParseFaceGenPath("meshes\\actors\\character\\facegendata\\facegeom\\skyrim.esm\\00013bb8.nif", &g));
    CHECK(g.kind == "geom" && g.plugin == "skyrim.esm" && g.objectId == 0x013BB8);
    CHECK(ParseFaceGenPath("textures\\actors\\character\\facegendata\\facetint\\a.esp\\fe000d64.dds", &g));
    CHECK(g.kind == "tint" && g.objectId == 0x000D64);
    CHECK(!ParseFaceGenPath("textures\\actors\\character\\facegendata\\facetint\\a.esp\\00000d64.tga", &g));
    CHECK(!ParseFaceGenPath("meshes\\actors\\character\\facegendata\\facegeom\\a.esp\\x.nif", &g));
    auto list = SplitArchiveList(" Skyrim - Misc.bsa,Skyrim - Shaders.bsa , ,x.bsa");
    CHECK(list.size() == 3 && list[0] == "Skyrim - Misc.bsa" && list[1] == "Skyrim - Shaders.bsa" && list[2] == "x.bsa");
}

void TestPlanArchives() {
    using assets::PlanArchives;
    // In Data: two ini archives (listed out of directory order), plugin
    // archives, one archive nobody loads, one claimed twice.
    std::vector<std::string> inData = {"Skyrim - Misc.bsa", "Skyrim - Textures0.bsa", "Mod.bsa", "Mod - Textures.bsa",
                                       "Orphan.bsa", "Late.BSA", "Dawnguard.bsa"};
    auto plan = PlanArchives(inData, {"skyrim - textures0.bsa", "Skyrim - Misc.bsa", "Missing.bsa", "Dawnguard.bsa"},
                             {"Skyrim.esm", "Dawnguard.esm", "Mod.esp", "late.esl"});
    CHECK(plan.size() == 6);
    auto find = [&](const char* n) -> const assets::ArchiveLoad* {
        for (const auto& a : plan)
            if (a.name == n) return &a;
        return nullptr;
    };
    CHECK(find("Skyrim - Textures0.bsa") && find("Skyrim - Textures0.bsa")->rank == 0);
    CHECK(find("Skyrim - Misc.bsa") && find("Skyrim - Misc.bsa")->rank == 1);
    CHECK(find("Dawnguard.bsa") && find("Dawnguard.bsa")->loadedBy == "ini" && find("Dawnguard.bsa")->rank == 2);
    CHECK(find("Mod.bsa") && find("Mod.bsa")->loadedBy == "plugin" && find("Mod.bsa")->plugin == "Mod.esp");
    CHECK(find("Mod - Textures.bsa") && find("Mod - Textures.bsa")->rank == find("Mod.bsa")->rank + 1);
    CHECK(find("Late.BSA") && find("Late.BSA")->rank == 5);
    CHECK(!find("Orphan.bsa"));
}

BeBytes Pex(const char* name, const char* parent, bool debug) {
    BeBytes p;
    p.u32(0xFA57C0DE).u8(3).u8(2).u16(1).u32(0).u32(0x5F000000).str("Test.psc").str("user").str("machine");
    p.u16(3).str(name).str(parent).str("GetState");
    p.u8(debug ? 1 : 0);
    if (debug) p.u32(0).u32(0).u16(1).u16(0).u16(0).u16(2).u8(0).u16(2).u16(10).u16(11);
    p.u16(1).u16(2).u8(0);              // one user flag
    p.u16(1).u16(0).u32(64).u16(1).u16(0);  // object: name, size, parent, docstring
    p.u32(0).u16(0);
    return p;
}

void TestPex() {
    assets::PexInfo i;
    std::string err;
    BeBytes p = Pex("MyQuestScript", "Quest", true);
    CHECK(assets::ReadPex(p.b.data(), p.b.size(), &i, &err));
    CHECK(i.scriptName == "MyQuestScript" && i.parentScript == "Quest" && i.gameId == 1 && err.empty());
    BeBytes q = Pex("Base", "", false);
    assets::PexInfo j;
    CHECK(assets::ReadPex(q.b.data(), q.b.size(), &j, &err) && j.scriptName == "Base" && j.parentScript.empty());
    // Every truncation is refused, never read past.
    int refused = 0;
    for (size_t n = 0; n + 8 < p.b.size(); ++n) {  // the last 8 bytes are not needed
        assets::PexInfo t;
        std::string e;
        std::vector<uint8_t> cut(p.b.begin(), p.b.begin() + n);
        if (!assets::ReadPex(cut.data(), cut.size(), &t, &e)) ++refused;
    }
    CHECK(refused == static_cast<int>(p.b.size()) - 8);
    // A string index past the table.
    BeBytes bad = Pex("A", "B", false);
    bad.b[bad.b.size() - 16] = 0x7F;  // object name index high byte
    assets::PexInfo k;
    CHECK(!assets::ReadPex(bad.b.data(), bad.b.size(), &k, &err));
    uint8_t le[] = {0xDE, 0xC0, 0x57, 0xFA, 3, 9};
    CHECK(!assets::ReadPex(le, sizeof(le), &k, &err) && err.find("little-endian") != std::string::npos);
}

Bytes FaceGenTri() {
    Bytes t;
    const uint32_t v = 3, tris = 1, x = 3, md = 2, ms = 1, k = 2;
    t.raw("FRTRI003", 8).u32(v).u32(tris).u32(0).u32(0).u32(0).u32(x).u32(1).u32(md).u32(ms).u32(k);
    for (int i = 0; i < 4; ++i) t.u32(0);
    for (uint32_t i = 0; i < (v + k) * 3; ++i) t.f32(0.5f);
    t.u32(0).u32(1).u32(2);                                 // triangle
    for (uint32_t i = 0; i < x * 2; ++i) t.f32(0.25f);      // texture coordinates
    t.u32(0).u32(1).u32(2);                                 // texture triangle
    const char* morphs[] = {"Aah", "BigAah"};
    for (const char* m : morphs) {
        t.u32(static_cast<uint32_t>(strlen(m) + 1)).raw(m, strlen(m) + 1).f32(0.001f);
        for (uint32_t i = 0; i < v * 3; ++i) t.u16(1);
    }
    t.u32(6).raw("Blink", 6).u32(2).u32(3).u32(4);
    return t;
}

void TestTri() {
    Bytes t = FaceGenTri();
    assets::TriInfo i;
    std::string err;
    CHECK(assets::ReadTri(t.b.data(), t.size(), &i, &err) && err.empty());
    CHECK(i.version == "FRTRI003" && i.vertexCount == 3 && i.triangleCount == 1 && i.uvCount == 3);
    CHECK(i.morphs.size() == 2 && i.morphs[1] == "BigAah" && i.modifiers.size() == 1 && i.modifiers[0] == "Blink");
    // Truncated bodies keep the header counts, drop the names, set an error.
    int ok = 0;
    for (size_t n = 64; n < t.size(); ++n) {
        assets::TriInfo c;
        std::string e;
        if (assets::ReadTri(t.b.data(), n, &c, &e) && c.morphs.empty() && c.modifiers.empty() && !e.empty() &&
            c.morphCount == 2)
            ++ok;
    }
    CHECK(ok == static_cast<int>(t.size()) - 64);
    assets::TriInfo h;
    CHECK(!assets::ReadTri(t.b.data(), 63, &h, &err));
    // A name length the file cannot hold.
    Bytes bad = FaceGenTri();
    size_t morph0 = 64 + 5 * 12 + 12 + 3 * 8 + 12;
    bad.b[morph0 + 3] = 0x7F;
    assets::TriInfo b;
    err.clear();
    CHECK(assets::ReadTri(bad.b.data(), bad.size(), &b, &err) && b.morphs.empty() && !err.empty());
    // BodySlide morphs.
    Bytes p;
    p.raw("PIRT", 4).u16(1).u8(3).raw("3BA", 3).u16(2);
    p.u8(4).raw("Arms", 4).f32(0.01f).u16(1).u16(5).u16(1).u16(2).u16(3);
    p.u8(4).raw("Legs", 4).f32(0.01f).u16(0);
    p.u16(0);
    assets::TriInfo s;
    err.clear();
    CHECK(assets::ReadTri(p.b.data(), p.size(), &s, &err) && s.bodySlide && s.morphCount == 2 &&
          s.morphs[0] == "3BA:Arms" && err.empty());
    for (size_t n = 0; n < p.size(); ++n) {  // no crash on any prefix
        assets::TriInfo c;
        std::string e;
        assets::ReadTri(p.b.data(), n, &c, &e);
    }
}


// ---- End to end: a Data folder, staging folders, an MO2 instance ----

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, ' ');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string LowerPathForTest(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring FullPathForTest(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    return std::wstring(buf, n);
}

// Removes a fixture folder and everything in it.
void RemoveFixture(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void WriteFile(const std::wstring& path, const std::string& bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string Dds(uint32_t w, uint32_t h) {
    Bytes d;
    d.raw("DDS ", 4).u32(124).u32(0x1007).u32(h).u32(w).u32(w * h / 2).u32(0).u32(1);
    for (int i = 0; i < 11; ++i) d.u32(0);
    d.u32(32).u32(4).raw("DXT1", 4).u32(0).u32(0).u32(0).u32(0).u32(0);
    d.u32(0x1000).u32(0).u32(0).u32(0).u32(0);
    d.raw(std::string(w * h / 2, '\0').data(), w * h / 2);
    return std::string(d.b.begin(), d.b.end());
}

// A v105 BSA, uncompressed, one folder.
std::string Bsa(const char* folder, const std::vector<std::pair<std::string, std::string>>& files) {
    uint32_t namesLen = 0;
    for (const auto& f : files) namesLen += static_cast<uint32_t>(f.first.size() + 1);
    const uint32_t folderLen = static_cast<uint32_t>(strlen(folder) + 1);
    const uint32_t block = 36 + 24;
    const uint32_t dataStart = block + 1 + folderLen + 16 * static_cast<uint32_t>(files.size()) + namesLen;
    Bytes b;
    b.raw("BSA\0", 4).u32(105).u32(36).u32(3).u32(1).u32(static_cast<uint32_t>(files.size())).u32(folderLen)
        .u32(namesLen).u32(2);
    b.u32(1).u32(0).u32(static_cast<uint32_t>(files.size())).u32(0).u32(block + namesLen).u32(0);
    b.u8(static_cast<uint8_t>(folderLen)).raw(folder, folderLen);
    uint32_t off = dataStart;
    for (size_t i = 0; i < files.size(); ++i) {
        b.u32(static_cast<uint32_t>(i + 1)).u32(0).u32(static_cast<uint32_t>(files[i].second.size())).u32(off);
        off += static_cast<uint32_t>(files[i].second.size());
    }
    for (const auto& f : files) b.raw(f.first.c_str(), f.first.size() + 1);
    for (const auto& f : files) b.raw(f.second.data(), f.second.size());
    return std::string(b.b.begin(), b.b.end());
}

// One value from a query (first column of the first row), as text; "" = none.
std::string Query(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_text(st, 0))
        out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (!out.size() && st == nullptr) out = std::string("SQL ERROR: ") + sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return out;
}

// "root in_data is_winner" of the copy of `path` in `root` (archive name, or the root's name).
std::string Copy(sqlite3* db, const std::string& path, const std::string& where) {
    return Query(db, "SELECT in_data || ' ' || is_winner FROM conflicts_all WHERE path = '" + path +
                         "' AND src = '" + where + "'");
}

// The fixture lives in %TEMP%\skqe_tests_assets and is rewritten on every
// run (nothing is deleted).
std::wstring FixtureDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"skqe_tests_assets\\";
}

sqlite3* BuildFixture(AssetOptions opt, const std::wstring& out) {
    opt.out = out;
    opt.quiet = true;
    opt.threads = 3;
    std::string err;
    int rc = BuildAssets(opt, &err);
    if (rc) fprintf(stderr, "build-assets: %s\n", err.c_str());
    CHECK(rc == 0);
    sqlite3* db = nullptr;
    sqlite3_open16(out.c_str(), &db);
    char* msg = nullptr;
    sqlite3_exec(db,
                 "CREATE TEMP VIEW conflicts_all AS SELECT p.path, coalesce(ap.name, r.name) AS src, f.in_data, "
                 "f.is_winner FROM files f JOIN paths p USING(path_id) JOIN roots r USING(root_id) "
                 "LEFT JOIN files af ON af.file_id = f.archive_id LEFT JOIN paths ap ON ap.path_id = af.path_id",
                 nullptr, nullptr, &msg);
    sqlite3_free(msg);
    return db;
}

void TestBuildAssetsStaging() {
    const std::wstring root = FixtureDir() + L"vortex\\";
    const std::wstring data = root + L"Data\\", staging = root + L"staging\\";
    const std::string ddsA = Dds(4, 4), ddsB = Dds(8, 8);
    // ModA's a.dds is deployed (hardlinked) into Data; ModB has a different
    // a.dds (not deployed) and a b.dds only it has.
    WriteFile(staging + L"ModA\\textures\\a.dds", ddsA);
    WriteFile(staging + L"ModB\\textures\\a.dds", ddsB);
    WriteFile(staging + L"ModB\\textures\\b.dds", ddsB);
    std::filesystem::create_directories(data + L"textures");
    if (!CreateHardLinkW((data + L"textures\\a.dds").c_str(), (staging + L"ModA\\textures\\a.dds").c_str(), nullptr))
        CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
    WriteFile(data + L"meshes\\only.nif", "not a mesh");
    WriteFile(data + L"textures\\d.dds", ddsA);  // loose, also in Test.bsa: loose wins
    WriteFile(data + L"abc.txt", "abc");
    // Test.esp loads Test.bsa; Other.bsa has no plugin.
    WriteFile(data + L"Test.esp", "TES4");
    WriteFile(data + L"Test.bsa", Bsa("textures", {{"b.dds", ddsA}, {"c.dds", ddsA}, {"d.dds", ddsA}}));
    WriteFile(data + L"Other.bsa", Bsa("textures", {{"c.dds", ddsB}}));
    WriteFile(root + L"plugins.txt", "*Test.esp\r\n");

    AssetOptions opt;
    opt.data = data;
    opt.staging = staging;
    opt.loadOrderFile = root + L"plugins.txt";
    opt.hash = true;
    sqlite3* db = BuildFixture(opt, root + L"assets.db");
    // Hardlink: stored once, with ModA, in_data.
    CHECK(Query(db, "SELECT count(*) FROM files f JOIN paths p USING(path_id) WHERE path = 'textures\\a.dds'") == "2");
    CHECK(Copy(db, "textures\\a.dds", "ModA") == "1 1");
    CHECK(Copy(db, "textures\\a.dds", "ModB") == "0 0");
    CHECK(Query(db, "SELECT value FROM skqe_meta WHERE key = 'hardlink_matches'") == "1");
    // Data only: the Data root.
    CHECK(Copy(db, "meshes\\only.nif", "Data") == "1 1");
    CHECK(Query(db, "SELECT error FROM files JOIN paths USING(path_id) WHERE path = 'meshes\\only.nif'").find("nif") == 0);
    // No loose copy in Data: the loaded archive wins; staging-only is not seen.
    CHECK(Copy(db, "textures\\b.dds", "test.bsa") == "1 1");
    CHECK(Copy(db, "textures\\b.dds", "ModB") == "0 0");
    CHECK(Copy(db, "textures\\c.dds", "test.bsa") == "1 1");
    CHECK(Copy(db, "textures\\c.dds", "other.bsa") == "1 0");
    CHECK(Copy(db, "textures\\d.dds", "Data") == "1 1");
    CHECK(Copy(db, "textures\\d.dds", "test.bsa") == "1 0");
    CHECK(Query(db, "SELECT loaded_by || ' ' || plugin FROM archives JOIN files USING(file_id) JOIN paths USING(path_id) "
                    "WHERE path = 'test.bsa'") == "plugin Test.esp");
    CHECK(Query(db, "SELECT count(*) FROM archives WHERE loaded_by IS NULL") == "1");
    // Exactly one winner per path the game sees.
    CHECK(Query(db, "SELECT count(*) FROM (SELECT path_id FROM files GROUP BY path_id "
                    "HAVING sum(is_winner) <> (max(in_data AND archive_id IS NULL) OR "
                    "max(in_data AND archive_id IN (SELECT file_id FROM archives WHERE load_rank IS NOT NULL))))") == "0");
    // Headers inside the BSA; hashes.
    CHECK(Query(db, "SELECT count(*) FROM dds JOIN files USING(file_id) WHERE archive_id IS NOT NULL") == "4");
    CHECK(Query(db, "SELECT d.width FROM dds d JOIN files f USING(file_id) JOIN paths p USING(path_id) "
                    "JOIN roots r USING(root_id) WHERE p.path = 'textures\\b.dds' AND r.name = 'ModB'") == "8");
    CHECK(Query(db, "SELECT lower(hex(hash)) FROM files JOIN paths USING(path_id) WHERE path = 'abc.txt'") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Query(db, "SELECT count(*) FROM winning_files") == Query(db, "SELECT count(*) FROM files WHERE is_winner"));
    sqlite3_close(db);
}

void TestBuildAssetsMo2() {
    const std::wstring root = FixtureDir() + L"mo2\\";
    const std::wstring data = root + L"Data\\", inst = root + L"instance\\";
    WriteFile(inst + L"ModOrganizer.ini", "[General]\r\ngameName=Skyrim Special Edition\r\n");
    WriteFile(inst + L"profiles\\Default\\modlist.txt", "# order\r\n+ModY\r\n+ModX\r\n-ModZ\r\n");
    WriteFile(inst + L"profiles\\Default\\plugins.txt", "*X.esp\r\n");
    WriteFile(data + L"textures\\t.dds", Dds(4, 4));
    WriteFile(data + L"textures\\real.dds", Dds(4, 4));
    WriteFile(inst + L"mods\\ModX\\textures\\t.dds", Dds(8, 8));
    WriteFile(inst + L"mods\\ModX\\textures\\u.dds", Dds(8, 8));
    WriteFile(inst + L"mods\\ModY\\textures\\u.dds", Dds(16, 16));
    WriteFile(inst + L"mods\\ModZ\\textures\\u.dds", Dds(16, 16));
    WriteFile(inst + L"mods\\ModZ\\textures\\z.dds", Dds(16, 16));
    WriteFile(inst + L"overwrite\\textures\\t.dds", Dds(16, 16));
    WriteFile(inst + L"mods\\ModX\\X.esp", "TES4");
    WriteFile(inst + L"mods\\ModX\\X.bsa", Bsa("textures", {{"x.dds", Dds(4, 4)}}));

    AssetOptions opt;
    opt.data = data;
    opt.mo2 = inst;
    opt.profile = L"Default";
    sqlite3* db = BuildFixture(opt, root + L"assets.db");
    CHECK(Query(db, "SELECT value FROM skqe_meta WHERE key = 'mo2_virtual_data'") == "1");
    CHECK(Query(db, "SELECT priority FROM roots WHERE name = 'ModY'") == "3");
    CHECK(Query(db, "SELECT priority FROM roots WHERE kind = 'overwrite'") == "4");
    CHECK(Query(db, "SELECT enabled FROM roots WHERE name = 'ModZ'") == "0");
    CHECK(Copy(db, "textures\\t.dds", "Overwrite") == "1 1");  // overwrite over mods over Data
    CHECK(Copy(db, "textures\\t.dds", "ModX") == "0 0");
    CHECK(Copy(db, "textures\\t.dds", "Data") == "0 0");
    CHECK(Copy(db, "textures\\u.dds", "ModY") == "1 1");  // higher in modlist.txt
    CHECK(Copy(db, "textures\\u.dds", "ModX") == "0 0");
    CHECK(Copy(db, "textures\\u.dds", "ModZ") == "0 0");  // disabled
    CHECK(Copy(db, "textures\\z.dds", "ModZ") == "0 0");
    CHECK(Copy(db, "textures\\real.dds", "Data") == "1 1");
    CHECK(Copy(db, "textures\\x.dds", "x.bsa") == "1 1");  // the profile's plugin loads its mod's archive
    CHECK(Query(db, "SELECT manager FROM roots WHERE name = 'ModX'") == "mo2");
    sqlite3_close(db);
}

}  // namespace

// Writes valid hand-built inputs as fuzzing seeds (CI cannot ship real
// plugins): plain, compressed and nested-group plugins, a strings file and a
// zlib stream.
int WriteSeeds(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // parents too
    Bytes plain = Header({"Skyrim.esm"});
    plain.add(Group("WEAP", 0, Weapon(0x01000800, "TestSword", 7).add(Weapon(0x00012EB7, "IronSword", 9))));
    Bytes inner = Sub("EDID", Zstring("Packed")).add(Sub("DATA", Bytes().u32(25).f32(9.0f).u16(7)));
    Bytes packed;
    packed.u32(static_cast<uint32_t>(inner.size())).add(StoredZlib(inner));
    Bytes compressed = Header({}).add(Group("WEAP", 0, Rec("WEAP", 0x800, packed, 0x00040000)));
    Bytes refr = Rec("REFR", 0x801, Sub("NAME", Bytes().u32(0x800)).add(Sub("DATA", Bytes().f32(1).f32(2).f32(3).f32(0).f32(0).f32(0))));
    Bytes cell = Rec("CELL", 0x802, Sub("EDID", Zstring("TestCell")));
    Bytes cellKids = Group("\x02\x08\x00\x00", 6, Group("\x02\x08\x00\x00", 9, refr));
    Bytes nested = Header({}).add(Group("CELL", 0, Group("\x00\x00\x00\x00", 2, Group("\x00\x00\x00\x00", 3, cell.add(cellKids)))));
    Bytes armo = Header({}).add(Group("ARMO", 0, Rec("ARMO", 0x800, Sub("MO2S", Bytes().u32(1).u32(4).raw("Name", 4).u32(0x800).u32(0)))));
    Bytes strings;
    strings.u32(2).u32(12).u32(1).u32(0).u32(2).u32(6).raw("Hello", 6).raw("World", 6);
    Bytes zlib = Bytes().u32(17).add(StoredZlib(Bytes().raw("hello hello hello", 17)));
    const std::pair<const wchar_t*, const Bytes*> seeds[] = {
        {L"plain.esp", &plain}, {L"compressed.esp", &compressed}, {L"nested.esp", &nested},
        {L"armo.esp", &armo},   {L"strings.bin", &strings},       {L"zlib.bin", &zlib},
    };
    for (const auto& s : seeds) {
        std::ofstream out(dir + L"\\" + s.first, std::ios::binary);
        out.write(reinterpret_cast<const char*>(s.second->b.data()), static_cast<std::streamsize>(s.second->size()));
        if (!out) {
            fprintf(stderr, "cannot write seed %ls\n", (dir + L"\\" + s.first).c_str());
            return 1;
        }
    }
    printf("%zu seeds written\n", std::size(seeds));
    return 0;
}

void TestFindStaging() {
    CHECK(QtIniDecode("@ByteArray(D:\\\\Games\\\\Skyrim)") == "D:\\Games\\Skyrim");
    CHECK(QtIniDecode("\"C:/a b\"") == "C:/a b");
    CHECK(QtIniDecode("@ByteArray(Caf\\xc3\\xa9)") == "Caf\xC3\xA9");
    CHECK(QtIniDecode("plain") == "plain");

    const std::wstring root = FixtureDir() + L"find\\";
    RemoveFixture(root);  // a previous run's instances would be found too
    const std::wstring game = root + L"Game\\", data = game + L"Data\\";
    const std::string rootA = Utf8(root);
    auto esc = [](std::string s) {
        std::string o;
        for (char c : s) {
            if (c == '\\') o += '\\';
            o += c;
        }
        return o;
    };
    const std::wstring custom = root + L"Custom\\Vortex Mods\\";
    WriteFile(data + L"Skyrim.esm", "TES4");
    WriteFile(custom + L"__vortex_staging_folder", "");
    WriteFile(custom + L"ModA\\a.esp", "x");
    auto manifest = [&](const std::string& files, const std::string& game_ = "skyrimse", const std::wstring& at = L"") {
        WriteFile(data + L"vortex.deployment.json",
                  "{\n \"instance\": \"x\",\n \"version\": 1,\n \"gameId\": \"" + game_ + "\",\n \"stagingPath\": \"" +
                      esc(Utf8(at.empty() ? custom.substr(0, custom.size() - 1) : at)) + "\",\n \"files\": " + files + "\n}\n");
    };
    StagingEnv env;
    env.appData = root + L"Roaming";
    env.localAppData = root + L"Local";
    env.programData = root + L"ProgramData";
    auto find = [&](const std::string& manager, StagingResult* r, std::string* err) {
        *r = StagingResult();
        err->clear();
        return FindStaging(data, manager, env, r, err);
    };
    StagingResult r;
    std::string err;

    // Nothing at all.
    CHECK(!find("", &r, &err) && err.find("no Vortex or MO2") != std::string::npos);

    // The manifest beats a default folder that has the marker and even a mod.
    WriteFile(env.appData + L"\\Vortex\\skyrimse\\mods\\__vortex_staging_folder", "");
    WriteFile(env.appData + L"\\Vortex\\skyrimse\\mods\\Old\\o.esp", "x");
    manifest("[{\"relPath\":\"a.esp\"}]");
    CHECK(find("", &r, &err) && r.chosen.manager == "vortex" && r.chosen.staging == custom && r.chosen.deployed);
    CHECK(r.all.size() == 1);

    // A manifest for another game, or pointing nowhere, isn't used; the default is.
    manifest("[]", "fallout4");
    CHECK(find("", &r, &err) && r.chosen.staging == FullPathForTest(env.appData + L"\\Vortex\\skyrimse\\mods\\"));
    manifest("[]", "skyrimse", root + L"Gone");
    CHECK(find("", &r, &err) && !r.chosen.deployed && r.chosen.staging != custom);

    // The marker with no mods doesn't count.
    DeleteFileW((env.appData + L"\\Vortex\\skyrimse\\mods\\Old\\o.esp").c_str());
    RemoveDirectoryW((env.appData + L"\\Vortex\\skyrimse\\mods\\Old").c_str());
    CHECK(!find("vortex", &r, &err));

    // MO2: a global instance, stored the way Qt writes it, with a custom mods folder.
    const std::wstring inst = env.localAppData + L"\\ModOrganizer\\Skyrim Special Edition\\";
    auto ini = [&](const std::wstring& at, const std::wstring& gamePath, const std::string& extra = "") {
        WriteFile(at + L"ModOrganizer.ini", "[General]\r\ngameName=Skyrim Special Edition\r\nselected_profile=@ByteArray(Mine)\r\ngamePath=@ByteArray(" +
                                                esc(Utf8(gamePath)) + ")\r\n\r\n[Settings]\r\n" + extra);
        WriteFile(at + L"profiles\\Mine\\modlist.txt", "+A\r\n");
        WriteFile(at + L"profiles\\Other\\modlist.txt", "+A\r\n");
    };
    ini(inst, game.substr(0, game.size() - 1), "mod_directory=%BASE_DIR%/elsewhere\r\n");
    WriteFile(inst + L"elsewhere\\A\\a.esp", "x");
    CHECK(find("", &r, &err) && r.chosen.manager == "mo2" && r.chosen.profile == "Mine" && r.chosen.staging == inst + L"elsewhere\\");

    // Vortex with files deployed beats MO2, unless MO2 is asked for; a purged Vortex doesn't.
    manifest("[{\"relPath\":\"a.esp\"}]");
    CHECK(find("", &r, &err) && r.chosen.manager == "vortex" && r.all.size() == 2);
    CHECK(find("mo2", &r, &err) && r.chosen.manager == "mo2" && r.all.size() == 1);
    manifest("[ ]");
    CHECK(find("", &r, &err) && r.chosen.manager == "mo2");
    CHECK(find("vortex", &r, &err) && r.chosen.manager == "vortex" && !r.chosen.deployed);
    DeleteFileW((data + L"vortex.deployment.json").c_str());

    // Another instance of the same game: can't tell, until MO2's current one says.
    const std::wstring second = env.localAppData + L"\\ModOrganizer\\Second\\";
    ini(second, game.substr(0, game.size() - 1));
    WriteFile(second + L"mods\\A\\a.esp", "x");
    CHECK(!find("", &r, &err) && err.find("more than one") != std::string::npos && r.all.size() == 2);
    env.mo2CurrentInstance = "second";
    CHECK(find("", &r, &err) && r.chosen.instanceName == "Second");
    env.mo2CurrentInstance.clear();
    RemoveFixture(second);

    // A portable instance, known only from nxmhandler.ini.
    RemoveFixture(inst);
    const std::wstring portable = root + L"Portable MO2\\";
    ini(portable, game.substr(0, game.size() - 1));
    WriteFile(portable + L"mods\\A\\a.esp", "x");
    WriteFile(env.localAppData + L"\\ModOrganizer\\nxmhandler.ini",
              "[handlers]\r\nsize=2\r\n1\\executable=" + esc(Utf8(portable)) + "ModOrganizer.exe\r\n2\\executable=C:\\\\Vortex\\\\Vortex.exe\r\n");
    CHECK(find("", &r, &err) && r.chosen.manager == "mo2" && r.chosen.instanceName.empty() && r.chosen.staging == portable + L"mods\\");

    // An instance of this game in another folder isn't chosen, but is reported.
    ini(portable, root.substr(0, root.size() - 1) + L"\\OtherGame");
    CHECK(!find("", &r, &err) && err.find("not for this Data") != std::string::npos && r.all.size() == 1 && !r.all[0].matchesData);
}

void TestSearch() {
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "iron"));
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "a\\iron"));
    CHECK(!AssetPathMatches("textures\\a\\iron.dds", "steel"));
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "*.dds"));
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "ir?n.*"));
    CHECK(!AssetPathMatches("textures\\a\\iron.dds", "textures*"));  // no \: file name only
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "textures\\*\\iron.dds"));
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "a\\iron*"));               // from any folder down
    CHECK(AssetPathMatches("textures\\a\\iron.dds", "textures\\a\\ir*.dds"));
    CHECK(!AssetPathMatches("textures\\a\\iron.dds", "b\\iron*"));
    CHECK(!AssetPathMatches("textures\\a\\iron.dds", "ures\\a\\iron*"));      // whole folder names only

    const std::wstring root = FixtureDir() + L"search\\";
    RemoveFixture(root);
    const std::wstring data = root + L"Data\\", staging = root + L"Staging\\";
    WriteFile(data + L"Textures\\Iron.dds", Dds(4, 4));
    WriteFile(staging + L"ModA\\Textures\\keep.dds", Dds(4, 4));
    WriteFile(staging + L"ModB\\textures\\iron.dds", Dds(4, 4));  // the same bytes, a separate file
    WriteFile(staging + L"__vortex_staging_folder", "");
    CreateDirectoryW((staging + L"ModA\\Textures").c_str(), nullptr);
    CHECK(CreateHardLinkW((staging + L"ModA\\Textures\\Iron.dds").c_str(), (data + L"Textures\\Iron.dds").c_str(), nullptr));
    WriteFile(data + L"Pack.bsa", Bsa("textures", {{"iron.dds", Dds(4, 4)}, {"other.dds", Dds(4, 4)}}));
    WriteFile(staging + L"ModC\\Other.bsa", Bsa("textures", {{"iron.dds", Dds(4, 4)}}));
    CreateDirectoryW((staging + L"ModA").c_str(), nullptr);
    CHECK(CreateHardLinkW((staging + L"ModA\\Pack.bsa").c_str(), (data + L"Pack.bsa").c_str(), nullptr));

    AssetSearchOptions opt;
    opt.threads = 3;
    opt.pattern = "iron.dds";
    opt.roots.push_back({data, "Data"});
    AssetSearchResult r;
    std::string err;
    CHECK(SearchAssets(opt, &r, &err));
    CHECK(r.files.size() == 1);
    if (r.files.size() == 1) {
        CHECK(r.files[0].path == "Textures\\Iron.dds");  // the loose file's spelling
        CHECK(r.files[0].copies.size() == 2);
        CHECK(r.files[0].copies[0].archive.empty() && r.files[0].copies[0].size > 0);
        CHECK(r.files[0].copies[1].archive == "Pack.bsa" && r.files[0].copies[1].in == std::vector<std::string>{"Data"});
    }
    CHECK(r.archivesRead == 1 && r.hardlinksMerged == 0);

    auto mods = StagingRoots(staging);
    CHECK(mods.size() == 3 && mods[0].name == "ModA");
    opt.roots.insert(opt.roots.end(), mods.begin(), mods.end());
    r = AssetSearchResult();
    CHECK(SearchAssets(opt, &r, &err));
    CHECK(r.files.size() == 1 && r.archivesRead == 2 && r.hardlinksMerged == 2);
    if (r.files.size() == 1) {
        const auto& c = r.files[0].copies;
        CHECK(c.size() == 4);  // Data+ModA hardlink, ModB, Pack.bsa (Data+ModA), ModC's archive
        if (c.size() == 4) {
            CHECK((c[0].in == std::vector<std::string>{"Data", "ModA"}) && c[0].archive.empty());
            CHECK((c[1].in == std::vector<std::string>{"ModB"}) && c[1].archive.empty());
            CHECK(c[2].archive == "Pack.bsa" && (c[2].in == std::vector<std::string>{"Data", "ModA"}));
            CHECK(c[3].archive == "Other.bsa" && (c[3].in == std::vector<std::string>{"ModC"}));
        }
    }

    // Wildcards reach into archives; a damaged archive warns and the rest still works.
    WriteFile(data + L"Broken.bsa", "not a bsa at all");
    opt.pattern = "oth*.dds";
    r = AssetSearchResult();
    CHECK(SearchAssets(opt, &r, &err));
    CHECK(r.files.size() == 1 && r.files[0].path == "textures\\other.dds" && r.warnings.size() == 1);

    // A regex: the whole path, any case, / between folders, classes intact.
    auto regexHits = [&](const char* p) {
        opt.pattern = p;
        opt.regex = true;
        r = AssetSearchResult();
        const bool ok = SearchAssets(opt, &r, &err);
        opt.regex = false;
        return ok ? static_cast<int>(r.files.size()) : -1;
    };
    CHECK(regexHits("TEXTURES/IRON\\.DDS$") == 1);
    CHECK(regexHits("textures/o.*\\.dds$") == 1);
    CHECK(regexHits("^iron") == 0);
    CHECK(regexHits("\\D+\\.dds$") >= 1);                                       // \D must not become \d
    CHECK(regexHits("[unclosed") == -1 && err.find("bad regex") != std::string::npos);

    opt.pattern = "";
    CHECK(!SearchAssets(opt, &r, &err));
    opt.pattern = "x";
    opt.roots.push_back({root + L"Nowhere", "Nowhere"});
    CHECK(!SearchAssets(opt, &r, &err));

    // Saved folders beat detection, and a saved staging folder gone missing is an error.
    StagingEnv env;
    env.appData = root + L"R";
    env.localAppData = root + L"L";
    env.programData = root + L"P";
    env.settingsFile = root + L"L\\skqe\\settings.json";
    CreateDirectoryW((root + L"L").c_str(), nullptr);
    CHECK(LoadStagingSettings(env).data.empty());
    StagingSettings saved;
    saved.data = data;
    saved.staging = staging;
    CHECK(SaveStagingSettings(env, saved, &err));
    CHECK(LoadStagingSettings(env).staging == staging);
    StagingResult found;
    CHECK(FindStaging(L"", "", env, &found, &err) && found.chosen.manager == "manual" && found.chosen.staging == staging && found.data == data);
    CHECK(!FindStaging(L"", "vortex", env, &found, &err));  // asking for a manager skips the saved folder
    saved.staging = root + L"Gone";
    CHECK(SaveStagingSettings(env, saved, &err));
    found = StagingResult();
    CHECK(!FindStaging(L"", "", env, &found, &err) && err.find("saved staging") != std::string::npos);
}

void TestSearchFilters() {
    const std::string rules =
        "# comment\nplugins, data\npex, scripts\npsc, scripts;source\nxml, a;b\n*_SUM.ini, seasons\n"
        "regex: _[A-Z]{3,5}\\.ini, data\n\nexclude: __folder_managed_by_vortex\n";
    SearchFilters f;
    std::string err;
    CHECK(f.Parse(rules, &err));
    CHECK(f.RuleCount() == 8 && f.ExcludeCount() == 1);
    CHECK(f.Allows("Skyrim.esm") && f.Allows("X.ESL"));
    CHECK(!f.Allows("sub\\X.esp"));
    CHECK(f.Allows("Scripts\\a.pex") && f.Allows("scripts\\deep\\a.pex"));
    CHECK(!f.Allows("textures\\a.pex") && !f.Allows("a.pex"));
    CHECK(f.Allows("source\\scripts\\a.psc") && f.Allows("scripts\\a.psc") && !f.Allows("other\\a.psc"));
    CHECK(f.Allows("anything\\a.wav") && f.Allows("a.wav"));                      // no rule: everywhere
    CHECK(f.Allows("Seasons\\Foo_SUM.ini") && !f.Allows("Foo_SUM.ini"));          // the earlier line wins over the regex
    CHECK(f.Allows("Foo_DISTR.ini") && !f.Allows("sub\\Foo_DISTR.ini"));          // regex: Data's top folder only
    CHECK(f.Allows("sub\\foo_distr.ini") && f.Allows("sub\\Foo_Toolong.ini"));    // the regex matches case, and 3 to 5 letters
    CHECK(!f.Allows("textures\\a\\__folder_managed_by_vortex"));

    std::vector<std::string> folders;
    bool root = false;
    CHECK(f.Narrow("*.pex", &folders, &root) && folders == std::vector<std::string>{"scripts"} && !root);
    CHECK(f.Narrow("x.xml", &folders, &root) && folders.size() == 2);
    CHECK(f.Narrow("*.esp", &folders, &root) && folders.empty() && root);
    CHECK(f.Narrow("*_SUM.ini", &folders, &root) && folders == std::vector<std::string>{"seasons"});
    CHECK(f.Narrow("*_DISTR.ini", &folders, &root) && folders.empty() && root);
    CHECK(!f.Narrow("barenziah", &folders, &root));
    CHECK(!f.Narrow("*.wav", &folders, &root));
    CHECK(!f.Narrow("textures\\iron*", &folders, &root));

    SearchFilters bad;
    CHECK(!bad.Parse("pex\n", &err) && err.find("line 1") != std::string::npos);
    CHECK(!bad.Parse("regex: [, data\n", &err) && err.find("bad regex") != std::string::npos);
    CHECK(!bad.Parse("pex, a;;b\n", &err));

    // The file that ships: any case fits the last rule, while seasons' lines win over it.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring shipped = exe;
    shipped = shipped.substr(0, shipped.find_last_of(L"\\") + 1) + L"search_filters.txt";
    SearchFilters ship;
    CHECK(ship.LoadFile(shipped, &err));
    CHECK(ship.Allows("Foo_DISTR.ini") && ship.Allows("foo_distr.ini"));
    CHECK(!ship.Allows("sub\\Foo_DISTR.ini") && !ship.Allows("sub\\foo_distr.ini"));
    CHECK(ship.Allows("seasons\\foo_sum.ini") && !ship.Allows("foo_sum.ini"));
    CHECK(ship.Narrow("*_distr.ini", &folders, &root) && folders.empty() && root);

    // On real files: the rules, then the bypasses, then winners.
    const std::wstring root2 = FixtureDir() + L"filters\\";
    RemoveFixture(root2);
    const std::wstring data = root2 + L"Data\\";
    WriteFile(data + L"scripts\\good.pex", "x");
    WriteFile(data + L"textures\\stray.pex", "x");
    WriteFile(data + L"textures\\__folder_managed_by_vortex", "");
    WriteFile(data + L"meshes\\a.nif", "x");
    WriteFile(data + L"meshes\\both.nif", "x");
    WriteFile(data + L"A.bsa", Bsa("meshes", {{"both.nif", "aaa"}, {"packed.nif", "aaa"}, {"only.nif", "aaa"}}));
    WriteFile(data + L"B.bsa", Bsa("meshes", {{"packed.nif", "bbb"}}));
    WriteFile(data + L"Unloaded.bsa", Bsa("meshes", {{"unloaded.nif", "ccc"}, {"only.nif", "ccc"}}));
    WriteFile(data + L"Stray.bsa", Bsa("textures", {{"packed.pex", "ddd"}}));

    AssetSearchOptions opt;
    opt.threads = 3;
    opt.roots.push_back({data, "Data"});
    opt.filters = &f;
    auto run = [&](const char* pattern, AssetSearchResult* r) {
        *r = AssetSearchResult();
        opt.pattern = pattern;
        return SearchAssets(opt, r, &err);
    };
    AssetSearchResult r;
    CHECK(run("*.pex", &r) && r.files.size() == 1 && r.files[0].path == "scripts\\good.pex");
    opt.regex = true;
    CHECK(run("\\.pex$", &r) && r.files.size() == 1 && r.narrowedTo == std::vector<std::string>{"scripts"});  // narrows too
    CHECK(run("(a|good)\\.pex$", &r) && r.narrowedTo.empty() && r.files.size() == 1);                          // can't tell: walks all
    opt.regex = false;
    CHECK(run("*.pex", &r) && r.files.size() == 1);
    CHECK(r.narrowedTo == std::vector<std::string>{"scripts"});
    CHECK(r.looseScanned < 9);                               // textures and meshes weren't walked
    CHECK(run("vortex", &r) && r.files.empty());             // excluded
    opt.filters = nullptr;
    CHECK(run("*.pex", &r) && r.files.size() == 3);          // --anywhere: both loose and the packed one
    CHECK(run("vortex", &r) && r.files.size() == 1);
    opt.bsaOnly = true;
    CHECK(run("*.pex", &r) && r.files.size() == 1 && r.files[0].path == "textures\\packed.pex");
    CHECK(r.looseScanned < 7);                               // only Data's top folder was listed
    opt.bsaOnly = false;
    opt.filters = &f;

    std::unordered_map<std::string, int> ranks{{"a.bsa", 1}, {"b.bsa", 2}};
    opt.winnerRanks = &ranks;
    CHECK(run("*.nif", &r));
    auto copyOf = [&](const char* path) {
        for (const auto& file : r.files)
            if (LowerPathForTest(file.path) == path) return file.copies;
        return std::vector<AssetSearchCopy>();
    };
    CHECK(r.archivesRead == 2);                              // the unloaded archive wasn't read
    CHECK(copyOf("meshes\\both.nif").size() == 1 && copyOf("meshes\\both.nif")[0].archive.empty());  // loose first
    CHECK(copyOf("meshes\\packed.nif").size() == 1 && copyOf("meshes\\packed.nif")[0].archive == "B.bsa");  // later archive
    CHECK(copyOf("meshes\\only.nif").size() == 1 && copyOf("meshes\\only.nif")[0].archive == "A.bsa");
    CHECK(copyOf("meshes\\unloaded.nif").empty());           // the game doesn't load it
    opt.winnerRanks = nullptr;
}

void TestSchema(const PatternCatalog& cat) {
    // Raw byte runs become BLOB columns, and nothing else does.
    Schema schema;
    schema.Build(cat, [](const std::string&) { return true; });
    int bytes = 0, mismatched = 0;
    for (const auto& t : schema.tables())
        for (const auto& c : t.columns) {
            if (c.kind == "bytes") ++bytes;
            if ((c.kind == "bytes") != (c.sqlType == "BLOB")) ++mismatched;
        }
    CHECK(bytes > 0);
    CHECK(mismatched == 0);
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 2 && std::wstring(argv[1]) == L"--seeds") return WriteSeeds(argv[2]);
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
    TestTextDecoding();
    TestHeaderFacts(cat);
    TestExportRequest();
    TestValidPlugin(cat);
    TestDamagedStructure(cat);
    TestDamagedFields(cat);
    TestInflate();
    TestStrings();
    TestSchema(cat);
    TestAssetPaths();
    TestPlanArchives();
    TestPex();
    TestTri();
    TestBuildAssetsStaging();
    TestBuildAssetsMo2();
    TestFindStaging();
    TestSearch();
    TestSearchFilters();
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
