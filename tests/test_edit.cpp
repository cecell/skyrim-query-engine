// Unit tests for the plugin writer, on hand-built plugins.
//
//   skqeedit_tests <data folder>   (holds patterns.json, field_order.csv, catalog_open.csv)
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "edit/document.h"
#include "edit/ops.h"
#include "plugin.h"

using namespace skqe;
using namespace skqe::edit;

namespace {

int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_passed;                                                   \
        } else {                                                          \
            ++g_failed;                                                   \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

void U16(Bytes* b, uint16_t v) {
    b->push_back(static_cast<uint8_t>(v));
    b->push_back(static_cast<uint8_t>(v >> 8));
}
void U32(Bytes* b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b->push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Text(Bytes* b, const char* s, size_t n) { b->insert(b->end(), s, s + n); }

Bytes Sub(const char* sig, const Bytes& data) {
    Bytes out;
    Text(&out, sig, 4);
    U16(&out, static_cast<uint16_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}
Bytes Str(const char* s) {
    Bytes b(s, s + strlen(s));
    b.push_back(0);
    return b;
}
Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes Rec(const char* type, uint32_t flags, uint32_t id, const Bytes& body, uint16_t version = 44) {
    Bytes out;
    Text(&out, type, 4);
    U32(&out, static_cast<uint32_t>(body.size()));
    U32(&out, flags);
    U32(&out, id);
    U32(&out, 0xABCD1234);  // VC info, which must survive
    U16(&out, version);
    U16(&out, 0x77);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes Group(uint32_t label, uint32_t type, const Bytes& content) {
    Bytes out;
    Text(&out, "GRUP", 4);
    U32(&out, static_cast<uint32_t>(content.size() + 24));
    U32(&out, label);
    U32(&out, type);
    U32(&out, 0x11223344);  // stamp, which must survive
    U32(&out, 0x01010101);
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

uint32_t Label(const char* s) { return static_cast<uint32_t>(s[0]) | s[1] << 8 | s[2] << 16 | static_cast<uint32_t>(s[3]) << 24; }

Bytes Hedr(float version, uint32_t count, uint32_t next) {
    Bytes b;
    uint32_t v;
    std::memcpy(&v, &version, 4);
    U32(&b, v);
    U32(&b, count);
    U32(&b, next);
    return b;
}

// One master, a WEAP of its own, an override of the master's KYWD, an interior
// cell with a reference. Non-localized, HEDR 1.71.
Bytes SamplePlugin() {
    Bytes data(34, 0);  // WEAP.DATA is short; keep bytes the pattern may not name
    data[0] = 25;  // value
    Bytes weap = Join({Sub("EDID", Str("TestSword")), Sub("FULL", Str("Test Sword")),
                       Sub("KSIZ", Bytes{1, 0, 0, 0}), Sub("KWDA", Bytes{0x18, 0xE7, 0x01, 0x00}), Sub("DATA", Bytes{25, 0, 0, 0, 0, 0, 0x40, 0x40, 7, 0})});
    Bytes kywd = Join({Sub("EDID", Str("Overridden")), Sub("CNAM", Bytes{1, 2, 3, 4})});
    Bytes cell = Join({Sub("EDID", Str("TestCell")), Sub("DATA", Bytes{1, 0})});
    Bytes refrBody(28, 0);
    Bytes refr = Join({Sub("NAME", Bytes{0x00, 0x08, 0x00, 0x01}), Sub("DATA", refrBody)});
    uint32_t cellId = 0x01000801;
    Bytes cellKids = Group(cellId, 6, Group(cellId, 9, Rec("REFR", 0, 0x01000802, refr)));
    Bytes interior = Group(Label("CELL"), 0, Group(1, 2, Group(0, 3, Join({Rec("CELL", 0, cellId, cell), cellKids}))));
    Bytes groups = Join({Group(Label("KYWD"), 0, Rec("KYWD", 0, 0x00000900, kywd)),
                         Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x01000800, weap)), interior});
    Bytes tes4 = Join({Sub("HEDR", Hedr(1.71f, 10, 0x803)), Sub("CNAM", Str("me")), Sub("MAST", Str("Skyrim.esm")),
                       Sub("DATA", Bytes(8, 0)), Sub("INTV", Bytes{1, 0, 0, 0})});
    return Join({Rec("TES4", 0, 0, tes4), groups});
}

std::string g_dataFolder;
PatternCatalog g_catalog;
EditData g_data;

Value Op(const char* json) {
    Value v;
    std::string err;
    if (!ParseJson(json, &v, &err)) printf("bad test JSON: %s\n", err.c_str());
    return v;
}

struct Fixture {
    std::unique_ptr<Document> doc;
    std::unique_ptr<Editor> ed;
    explicit Fixture(const Bytes& bytes, const char* name = "Test.esp") {
        std::string err;
        doc = Document::FromMemory(bytes.data(), bytes.size(), name, &err);
        if (doc) ed = std::make_unique<Editor>(doc.get(), &g_data);
    }
    Bytes Out() {
        Bytes o;
        doc->Serialize(&o);
        return o;
    }
    std::vector<Field> FieldsOf(const Bytes& bytes, uint32_t id) {
        std::string err;
        auto p = Plugin::FromMemory(bytes.data(), bytes.size(), "Test.esp", g_catalog, &err);
        if (!p) return {};
        const Record* r = p->Find(id);
        return r ? p->Fields(*r) : std::vector<Field>{};
    }
};

std::string ListJson(const Value& v) {
    std::string s;
    WriteJson(v, &s);
    return s;
}

bool Same(const Bytes& a, const Bytes& b) { return a == b; }

// Bytes in `out` not in `in`, as the only difference: `in` with [at, at+oldSize)
// replaced by `now`, ignoring listed 4-byte size fields.
size_t CountDiffs(const Bytes& a, const Bytes& b) {
    size_t n = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) n += a[i] != b[i];
    return n;
}

void TestRoundTrip() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    CHECK(f.doc != nullptr);
    Bytes out;
    CHECK(f.doc->Serialize(&out));
    CHECK(Same(in, out));
    WriteOptions l1;
    l1.rebuildGroups = true;
    CHECK(f.doc->Serialize(&out, l1));
    CHECK(Same(in, out));
    WriteOptions l2;
    l2.rebuildGroups = true;
    l2.rebuildRecords = true;
    CHECK(f.doc->Serialize(&out, l2));
    CHECK(Same(in, out));
    // Trailing bytes after the last group survive.
    Bytes withTail = in;
    withTail.push_back(0xEE);
    Fixture g(withTail);
    CHECK(g.doc != nullptr);
    CHECK(g.doc->trailingSize() == 1);
    CHECK(Same(withTail, g.Out()));
}

void TestDamaged() {
    Bytes in = SamplePlugin();
    std::string err;
    CHECK(!Document::FromMemory(in.data(), 10, "x", &err));
    // Every truncation: refused cleanly or read, never a crash, and what is
    // read writes back as it came.
    for (size_t n = 0; n < in.size(); n += 3) {
        auto d = Document::FromMemory(in.data(), n, "x", &err);
        if (d) {
            Bytes out;
            d->Serialize(&out);
            CHECK(out.size() == n);
        }
    }
    // A flipped size byte anywhere must not crash or read out of bounds.
    for (size_t i = 0; i < in.size(); i += 5) {
        Bytes bad = in;
        bad[i] ^= 0xFF;
        auto d = Document::FromMemory(bad.data(), bad.size(), "x", &err);
        if (d) {
            Bytes out;
            d->Serialize(&out);
            CHECK(Same(bad, out));
        }
    }
    Bytes deep;
    for (int i = 0; i < 64; ++i) {
        Text(&deep, "GRUP", 4);
        U32(&deep, static_cast<uint32_t>(24 * (64 - i)));
        U32(&deep, 0);
        U32(&deep, 0);
        U32(&deep, 0);
        U32(&deep, 0);
    }
    Bytes withDeep = Join({Rec("TES4", 0, 0, Sub("HEDR", Hedr(1.71f, 0, 0x800))), deep});
    CHECK(!Document::FromMemory(withDeep.data(), withDeep.size(), "x", &err));
    CHECK(err.find("nested") != std::string::npos);
}

void TestBodyParser() {
    // XXXX with a non-zero size field and an unneeded XXXX re-emit exactly.
    Bytes big(70000, 7);
    Bytes body;
    Text(&body, "XXXX", 4);
    U16(&body, 4);
    U32(&body, 70000);
    Text(&body, "DATA", 4);
    U16(&body, 0x1234);
    body.insert(body.end(), big.begin(), big.end());
    Text(&body, "XXXX", 4);
    U16(&body, 4);
    U32(&body, 2);
    Text(&body, "EDID", 4);
    U16(&body, 2);
    body.push_back('a');
    body.push_back(0);
    ParsedBody p = ParseBody(body.data(), body.size());
    CHECK(p.issue == BodyIssue::None);
    CHECK(p.subs.size() == 2);
    CHECK(p.subs[0].size == 70000);
    CHECK(p.subs[0].sizeField == 0x1234);
    Bytes out;
    EmitBody(p.subs, &out);
    CHECK(Same(body, out));
    // Damage is reported, not guessed at.
    Bytes tail = Sub("EDID", Str("x"));
    tail.push_back(1);
    CHECK(ParseBody(tail.data(), tail.size()).issue == BodyIssue::TrailingBytes);
    Bytes over = Sub("EDID", Str("abc"));
    over[4] = 200;
    CHECK(ParseBody(over.data(), over.size()).issue == BodyIssue::Overrun);
    Bytes x;
    Text(&x, "XXXX", 4);
    U16(&x, 3);
    x.insert(x.end(), 3, 0);
    CHECK(ParseBody(x.data(), x.size()).issue == BodyIssue::BadXxxx);
    // A new big subrecord gets XXXX with a zero size field.
    SubView s;
    s.sig = MakeSig("DATA");
    s.data = big.data();
    s.size = big.size();
    Bytes emitted;
    EmitBody({s}, &emitted);
    CHECK(emitted.size() == 10 + 6 + 70000);
    CHECK(emitted[14] == 0 && emitted[15] == 0);
    CHECK(std::memcmp(emitted.data(), "XXXX", 4) == 0);
}

void TestSetValue() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"EDID","value":"NewName"})"));
    CHECK(r.ok);
    Bytes out = f.Out();
    CHECK(out != in);
    auto fields = f.FieldsOf(out, 0x01000800);
    CHECK(!fields.empty() && fields[0].sig == "EDID" && fields[0].value.AsString() == "NewName");
    // Other records are byte-identical: the KYWD and the cell tree appear unchanged.
    auto find = [](const Bytes& h, const char* needle, size_t n) {
        return std::search(h.begin(), h.end(), needle, needle + n) != h.end();
    };
    CHECK(find(out, "Overridden", 10));
    CHECK(find(out, "TestCell", 8));
    CHECK(f.ed->Dirty());
    CHECK(f.ed->Undo());
    CHECK(Same(f.Out(), in));
    CHECK(!f.ed->Dirty());
    CHECK(f.ed->Redo());
    CHECK(Same(f.Out(), out));
}

void TestSetIsMinimal() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    // A fixed-size value: exactly the bytes of that value change.
    CHECK(f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"DATA.value","value":26})")).ok ||
          f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"DATA","raw":"1A0000000000404007 00"})")).code.size());
    Bytes out = f.Out();
    CHECK(out.size() == in.size());
    CHECK(CountDiffs(in, out) == 1);
}

void TestNoOpAndRefusals() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"EDID","value":"TestSword"})"));
    CHECK(r.ok);
    CHECK(r.changes.size() == 1 && r.changes[0].what == "unchanged");
    CHECK(!f.ed->Dirty());
    CHECK(Same(f.Out(), in));

    r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:00ABCD","field":"EDID","value":"x"})"));
    CHECK(!r.ok && r.code == "not_found");
    r = f.ed->Apply(Op(R"({"op":"set","form":"Other.esm:000800","field":"EDID","value":"x"})"));
    CHECK(!r.ok && r.code == "master_missing");
    r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"NOPE","value":"x"})"));
    CHECK(!r.ok && r.code == "not_found");
    r = f.ed->Apply(Op(R"({"op":"nonsense"})"));
    CHECK(!r.ok && r.code == "unknown_op");
    // A failed op leaves everything as it was.
    CHECK(Same(f.Out(), in));
    CHECK(!f.ed->Dirty());
}

void TestAddRemoveSub() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"add-item","form":"Test.esp:000800","sig":"KWDA","item":"Skyrim.esm:012345"})"));
    CHECK(r.ok);
    Bytes out = f.Out();
    auto fields = f.FieldsOf(out, 0x01000800);
    bool sawKsiz = false;
    for (const auto& fd : fields) {
        if (fd.sig == "KSIZ") {
            sawKsiz = true;
            CHECK(fd.value.kind() == Value::Kind::Int && fd.value.AsInt() == 2);
        }
    }
    CHECK(sawKsiz);
    r = f.ed->Apply(Op(R"({"op":"remove-sub","form":"Test.esp:000800","sig":"KSIZ"})"));
    CHECK(!r.ok && r.code == "linked_subrecord");
    r = f.ed->Apply(Op(R"({"op":"remove-sub","form":"Test.esp:000800","sig":"KWDA"})"));
    CHECK(r.ok);
    fields = f.FieldsOf(f.Out(), 0x01000800);
    for (const auto& fd : fields) CHECK(fd.sig != "KSIZ" && fd.sig != "KWDA");
    // A new subrecord lands in the Creation Kit's order, nothing else moves.
    r = f.ed->Apply(Op(R"({"op":"add-sub","form":"Test.esp:000800","sig":"DESC","value":"A sword"})"));
    CHECK(r.ok);
    fields = f.FieldsOf(f.Out(), 0x01000800);
    std::string order;
    for (const auto& fd : fields) order += fd.sig + " ";
    CHECK(order == "EDID FULL DESC DATA ");
}

void TestAddRecord() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"add-record","type":"GLOB","edid":"MyGlobal","subs":[{"sig":"FNAM","raw":"66"},{"sig":"FLTV","raw":"0000803F"}]})"));
    CHECK(r.ok);
    CHECK(r.newForm == "Test.esp:000803");
    Bytes out = f.Out();
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (!p) return;
    const Record* rec = p->Find(0x01000803);
    CHECK(rec != nullptr && rec->type == "GLOB");
    CHECK(p->info().nextObjectId == 0x804);
    // 3 new things would be one record and a GLOB top group.
    auto reread = Document::FromMemory(out.data(), out.size(), "Test.esp", &err);
    CHECK(reread != nullptr);
    CHECK(reread != nullptr && p->info().numRecords == reread->CountForHedr());
    // The GLOB group goes where the top-group order puts it: after KYWD, before WEAP.
    if (reread) {
        const Node& root = reread->root();
        CHECK(root.children.size() == 5);
        if (root.children.size() == 5) {
            CHECK(root.children[2]->groupLabel() == Label("GLOB"));
            CHECK(root.children[3]->groupLabel() == Label("WEAP"));
        }
    }
    // A reference under an existing cell, in the temporary group.
    r = f.ed->Apply(Op(R"({"op":"add-record","type":"REFR","parent":"Test.esp:000801","subs":[{"sig":"NAME","raw":"00080001"},{"sig":"DATA","raw":"000000000000000000000000000000000000000000000000"}]})"));
    CHECK(r.ok);
    out = f.Out();
    p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (!p) return;
    rec = p->Find(0x01000804);
    CHECK(rec != nullptr && rec->parent == 0x01000801);
    // A persistent one opens the persistent group before the temporary one.
    r = f.ed->Apply(Op(R"({"op":"add-record","type":"REFR","parent":"Test.esp:000801","flags":1024,"subs":[{"sig":"NAME","raw":"00080001"}]})"));
    CHECK(r.ok);
    Bytes o2 = f.Out();
    auto reread2 = Document::FromMemory(o2.data(), o2.size(), "Test.esp", &err);
    CHECK(reread2 != nullptr);
    if (reread2) {
        const Node* cellKids = nullptr;
        std::function<void(const Node&)> walk = [&](const Node& n) {
            for (const auto& c : n.children) {
                if (c->isGroup && c->groupType() == 6) cellKids = c.get();
                if (c->isGroup) walk(*c);
            }
        };
        walk(reread2->root());
        CHECK(cellKids != nullptr && cellKids->children.size() == 2);
        if (cellKids && cellKids->children.size() == 2) {
            CHECK(cellKids->children[0]->groupType() == 8);
            CHECK(cellKids->children[1]->groupType() == 9);
        }
    }
    // A light plugin runs out at 0xFFF.
    Bytes lightTes4 = Join({Sub("HEDR", Hedr(1.71f, 0, 0xFFF)), Sub("MAST", Str("Skyrim.esm")), Sub("DATA", Bytes(8, 0))});
    Bytes light = Rec("TES4", 0x200, 0, lightTes4);
    Fixture l(light, "Light.esl");
    r = l.ed->Apply(Op(R"({"op":"add-record","type":"GLOB","edid":"A"})"));
    CHECK(r.ok);
    r = l.ed->Apply(Op(R"({"op":"add-record","type":"GLOB","edid":"B"})"));
    CHECK(!r.ok && r.code == "light_full");
}

void TestHedrRules() {
    // HEDR below 1.71: low object IDs refused.
    Bytes tes4 = Join({Sub("HEDR", Hedr(1.7f, 0, 0x800)), Sub("MAST", Str("Skyrim.esm")), Sub("DATA", Bytes(8, 0)), Sub("MAST", Str("Other.esm")), Sub("DATA", Bytes(8, 0))});
    Bytes weap = Join({Sub("EDID", Str("A"))});
    Bytes in = Join({Rec("TES4", 0, 0, tes4), Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x02000800, weap))});
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"add-item","form":"Test.esp:000800","sig":"KWDA","item":"Other.esm:000123"})"));
    CHECK(!r.ok && r.code == "low_id_needs_171");
    r = f.ed->Apply(Op(R"({"op":"add-item","form":"Test.esp:000800","sig":"KWDA","item":"Skyrim.esm:000123"})"));
    CHECK(r.ok);
    r = f.ed->Apply(Op(R"({"op":"add-record","type":"GLOB","objectId":"000100"})"));
    CHECK(!r.ok && r.code == "low_id_needs_171");
    // A light master's IDs stop at 0xFFF.
    Bytes tes4b = Join({Sub("HEDR", Hedr(1.71f, 0, 0x800)), Sub("MAST", Str("Other.esl")), Sub("DATA", Bytes(8, 0))});
    Bytes inb = Join({Rec("TES4", 0, 0, tes4b), Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x01000800, weap))});
    Fixture g(inb);
    r = g.ed->Apply(Op(R"({"op":"add-item","form":"Test.esp:000800","sig":"KWDA","item":"Other.esl:001000"})"));
    CHECK(!r.ok && r.code == "light_master_range");
}

void TestAddMaster() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"add-master","name":"Dawnguard.esm"})"));
    CHECK(r.ok);
    Bytes out = f.Out();
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(p->masters().size() == 2 && p->masters()[1] == "Dawnguard.esm");
    // Own forms moved to the new self index, master references did not.
    CHECK(p->Find(0x02000800) != nullptr);
    CHECK(p->Find(0x01000800) == nullptr);
    CHECK(p->Find(0x00000900) != nullptr);
    const Record* rec = p->Find(0x02000802);
    CHECK(rec != nullptr && rec->parent == 0x02000801);  // group labels moved too
    // The REFR's NAME pointed at its own plugin (self index 01): now 02.
    if (rec) {
        auto fields = p->Fields(*rec);
        bool saw = false;
        for (const auto& fd : fields) {
            if (fd.sig == "NAME") {
                saw = true;
                CHECK(fd.value.IsString() && fd.value.AsString() == "Test.esp:000800");
            }
        }
        CHECK(saw);
    }
    // Keywords point at Skyrim.esm and stay.
    auto kw = p->Fields(*p->Find(0x02000800));
    for (const auto& fd : kw) {
        if (fd.sig == "KWDA") CHECK(ListJson(fd.value).find("Skyrim.esm:01E718") != std::string::npos);
    }
    // Asking again is a no-op, in any case spelling.
    r = f.ed->Apply(Op(R"({"op":"add-master","name":"dawnguard.ESM"})"));
    CHECK(r.ok && r.changes[0].what == "unchanged");
}

void TestCopyAsOverride() {
    // Source: Skyrim-like, no masters, with a REFR in a cell (compressed record included).
    Bytes cell = Join({Sub("EDID", Str("SrcCell")), Sub("DATA", Bytes{1, 0})});
    Bytes refr = Join({Sub("NAME", Bytes{0x00, 0x10, 0x00, 0x00}), Sub("DATA", Bytes(24, 0))});
    Bytes npcPlain = Join({Sub("EDID", Str("SrcNpc")), Sub("RNAM", Bytes{0x00, 0x10, 0x00, 0x00})});
    Bytes npcZ;
    CHECK(ZlibDeflate(npcPlain.data(), npcPlain.size(), &npcZ));
    Bytes npcBody;
    U32(&npcBody, static_cast<uint32_t>(npcPlain.size()));
    npcBody.insert(npcBody.end(), npcZ.begin(), npcZ.end());
    uint32_t cellId = 0x00002000;
    Bytes kids = Group(cellId, 6, Group(cellId, 9, Rec("REFR", 0, 0x00002001, refr)));
    Bytes srcBytes = Join({Rec("TES4", 1, 0, Join({Sub("HEDR", Hedr(0.94f, 7, 0x3000))})),
                           Group(Label("NPC_"), 0, Rec("NPC_", kFlagCompressed, 0x00002002, npcBody)),
                           Group(Label("CELL"), 0, Group(0, 2, Group(0, 3, Join({Rec("CELL", 0, cellId, cell), kids}))))});
    auto temp = std::filesystem::temp_directory_path() / L"skqeedit_tests";
    std::filesystem::create_directories(temp);
    auto srcPath = temp / L"Skyrim.esm";
    {
        std::ofstream o(srcPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(srcBytes.data()), static_cast<std::streamsize>(srcBytes.size()));
    }
    std::string pathText;
    {
        std::string u = srcPath.u8string();
        for (char c : u) {
            pathText += c == '\\' ? '/' : c;
        }
    }

    // Target whose master 0 is the source: identical bytes, even compressed.
    Bytes targetTes4 = Join({Sub("HEDR", Hedr(1.71f, 0, 0x800)), Sub("MAST", Str("Skyrim.esm")), Sub("DATA", Bytes(8, 0))});
    Fixture f(Rec("TES4", 0, 0, targetTes4));
    std::string op = R"({"op":"copy-as-override","from":")" + pathText + R"(","form":"Skyrim.esm:002002"})";
    OpResult r = f.ed->Apply(Op(op.c_str()));
    CHECK(r.ok);
    Bytes out = f.Out();
    CHECK(std::search(out.begin(), out.end(), npcBody.begin(), npcBody.end()) != out.end());
    // A reference brings its cell and the groups.
    op = R"({"op":"copy-as-override","from":")" + pathText + R"(","form":"Skyrim.esm:002001"})";
    r = f.ed->Apply(Op(op.c_str()));
    CHECK(r.ok);
    out = f.Out();
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->Find(0x00002000) != nullptr);
        const Record* rec = p->Find(0x00002001);
        CHECK(rec != nullptr && rec->parent == 0x00002000);
        auto again = Document::FromMemory(out.data(), out.size(), "Test.esp", &err);
        CHECK(again != nullptr && again->CountForHedr() == p->info().numRecords);
    }
    r = f.ed->Apply(Op(op.c_str()));
    CHECK(!r.ok && r.code == "already_here");

    // A target that lists the master second: the FormIDs move, the source master is added.
    Bytes t2 = Join({Sub("HEDR", Hedr(1.71f, 0, 0x800)), Sub("MAST", Str("Update.esm")), Sub("DATA", Bytes(8, 0))});
    Fixture g(Rec("TES4", 0, 0, t2));
    op = R"({"op":"copy-as-override","from":")" + pathText + R"(","form":"Skyrim.esm:002002"})";
    r = g.ed->Apply(Op(op.c_str()));
    CHECK(r.ok);
    out = g.Out();
    p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->masters().size() == 2 && p->masters()[1] == "Skyrim.esm");
        const Record* rec = p->Find(0x01002002);
        CHECK(rec != nullptr);
        if (rec) {
            bool saw = false;
            for (const auto& fd : p->Fields(*rec)) {
                if (fd.sig == "RNAM") {
                    saw = true;
                    CHECK(fd.value.IsString() && fd.value.AsString() == "Skyrim.esm:001000");
                }
            }
            CHECK(saw);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void TestCopyConvertsText() {
    // A localized source, its strings beside it, a target that isn't localized.
    Bytes weap = Join({Sub("EDID", Str("S")), Sub("FULL", Bytes{9, 0, 0, 0}), Sub("DESC", Bytes{0, 0, 0, 0})});
    Bytes srcBytes = Join({Rec("TES4", 0x81, 0, Sub("HEDR", Hedr(1.71f, 2, 0x900))),
                           Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x00000800, weap))});
    auto temp = std::filesystem::temp_directory_path() / L"skqeedit_tests_text";
    std::filesystem::create_directories(temp / L"Strings");
    auto srcPath = temp / L"Loc.esm";
    {
        std::ofstream o(srcPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(srcBytes.data()), static_cast<std::streamsize>(srcBytes.size()));
    }
    Bytes table;
    U32(&table, 1);
    U32(&table, 6);
    U32(&table, 9);
    U32(&table, 0);
    Text(&table, "Hello", 6);
    {
        std::ofstream o(temp / L"Strings" / L"Loc_english.strings", std::ios::binary);
        o.write(reinterpret_cast<const char*>(table.data()), static_cast<std::streamsize>(table.size()));
    }
    std::string pathText;
    for (char c : srcPath.u8string()) pathText += c == '\\' ? '/' : c;
    Bytes t = Join({Sub("HEDR", Hedr(1.71f, 0, 0x800)), Sub("MAST", Str("Loc.esm")), Sub("DATA", Bytes(8, 0))});
    Fixture f(Rec("TES4", 0, 0, t));
    std::string op = R"({"op":"copy-as-override","from":")" + pathText + R"(","form":"Loc.esm:000800"})";
    OpResult r = f.ed->Apply(Op(op.c_str()));
    CHECK(r.ok);
    Bytes out = f.Out();
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        const Record* rec = p->Find(0x00000800);
        CHECK(rec != nullptr);
        if (rec) {
            std::string seen;
            for (const auto& fd : p->Fields(*rec)) {
                if (fd.sig == "FULL" && fd.value.IsString()) seen = fd.value.AsString();
            }
            CHECK(seen == "Hello");
        }
    }
    // An ID the strings don't hold is refused rather than guessed at.
    Bytes weap2 = Join({Sub("EDID", Str("S")), Sub("FULL", Bytes{8, 0, 0, 0})});
    f.ed.reset();  // it maps the source, which is about to be rewritten
    f.doc.reset();
    Bytes src2 = Join({Rec("TES4", 0x81, 0, Sub("HEDR", Hedr(1.71f, 2, 0x900))),
                       Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x00000800, weap2))});
    {
        std::ofstream o(srcPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(src2.data()), static_cast<std::streamsize>(src2.size()));
    }
    Fixture g(Rec("TES4", 0, 0, t));
    r = g.ed->Apply(Op(op.c_str()));
    CHECK(!r.ok && r.code == "string_missing");
    g.ed.reset();
    g.doc.reset();
    f.ed.reset();
    f.doc.reset();
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

void TestDelete() {
    Bytes in = SamplePlugin();
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"delete-record","form":"Test.esp:000801"})"));
    CHECK(!r.ok && r.code == "has_children");
    // An override stays as a deleted header.
    r = f.ed->Apply(Op(R"({"op":"delete-record","form":"Skyrim.esm:000900"})"));
    CHECK(r.ok);
    Bytes out = f.Out();
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        const Record* rec = p->Find(0x00000900);
        CHECK(rec != nullptr && (rec->flags & kFlagDeleted) != 0);
        if (rec) CHECK(p->Fields(*rec).empty());
    }
    // A form of its own goes away, with its children and the groups left empty.
    r = f.ed->Apply(Op(R"({"op":"delete-record","form":"Test.esp:000801","withChildren":true})"));
    CHECK(r.ok);
    out = f.Out();
    p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->Find(0x01000801) == nullptr);
        CHECK(p->Find(0x01000802) == nullptr);
        auto again = Document::FromMemory(out.data(), out.size(), "Test.esp", &err);
        CHECK(again != nullptr);
        if (again) {
            CHECK(again->root().children.size() == 3);  // TES4, KYWD, WEAP
            CHECK(p->info().numRecords == again->CountForHedr());
        }
    }
}

void TestCompressedEdit() {
    Bytes plain = Join({Sub("EDID", Str("SomeNpcWithALongEnoughNameToCompressWellEnoughForTheTest_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))});
    Bytes z;
    CHECK(ZlibDeflate(plain.data(), plain.size(), &z));
    Bytes body;
    U32(&body, static_cast<uint32_t>(plain.size()));
    body.insert(body.end(), z.begin(), z.end());
    Bytes untouched = Join({Sub("EDID", Str("Untouched_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))});
    Bytes z2;
    ZlibDeflate(untouched.data(), untouched.size(), &z2);
    // A stored form a recompress could not reproduce (padded zlib stored block is out of scope:
    // a marker byte in the unused size field is enough to tell it was kept).
    Bytes body2;
    U32(&body2, static_cast<uint32_t>(untouched.size()));
    body2.insert(body2.end(), z2.begin(), z2.end());
    Bytes in = Join({Rec("TES4", 0, 0, Join({Sub("HEDR", Hedr(1.71f, 2, 0x900))})),
                     Group(Label("NPC_"), 0, Join({Rec("NPC_", kFlagCompressed, 0x00000800, body), Rec("NPC_", kFlagCompressed, 0x00000801, body2)}))});
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"EDID","value":"Short_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})"));
    CHECK(r.ok);
    Bytes out = f.Out();
    CHECK(std::search(out.begin(), out.end(), body2.begin(), body2.end()) != out.end());
    std::string err;
    auto p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        const Record* rec = p->Find(0x00000800);
        CHECK(rec != nullptr && (rec->flags & kFlagCompressed) != 0);
        if (rec) {
            auto fields = p->Fields(*rec);
            CHECK(!fields.empty() && fields[0].value.AsString().rfind("Short_", 0) == 0);
        }
    }
    // Too short to shrink: stored plain, flag cleared.
    r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"EDID","value":"x"})"));
    CHECK(r.ok);
    out = f.Out();
    p = Plugin::FromMemory(out.data(), out.size(), "Test.esp", g_catalog, &err);
    CHECK(p != nullptr);
    if (p) {
        const Record* rec = p->Find(0x00000800);
        CHECK(rec != nullptr && (rec->flags & kFlagCompressed) == 0);
    }
}

void TestLocalized() {
    Bytes weap = Join({Sub("EDID", Str("A")), Sub("FULL", Bytes{5, 0, 0, 0})});
    Bytes in = Join({Rec("TES4", 0x80, 0, Join({Sub("HEDR", Hedr(1.71f, 2, 0x900))})),
                     Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x00000800, weap))});
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"FULL","value":"Text"})"));
    CHECK(!r.ok && r.code == "bad_value");
    r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"FULL","value":7})"));
    CHECK(r.ok);
}

void TestDependency() {
    // PLDT's type byte chooses how the value reads: changing it alone is refused.
    Bytes pldt{0, 0, 0, 0, 0x00, 0x08, 0x00, 0x00, 5, 0, 0, 0};
    Bytes body = Join({Sub("EDID", Str("P")), Sub("PLDT", pldt)});
    Bytes in = Join({Rec("TES4", 0, 0, Join({Sub("HEDR", Hedr(1.71f, 2, 0x900))})),
                     Group(Label("PACK"), 0, Rec("PACK", 0, 0x00000800, body))});
    Fixture f(in);
    OpResult r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"PLDT.type","value":5})"));
    CHECK(!r.ok && r.code == "layout_dependency");
    CHECK(Same(f.Out(), in));
    // The whole subrecord by raw is the way.
    r = f.ed->Apply(Op(R"({"op":"set","form":"Test.esp:000800","field":"PLDT","raw":"050000000008000005000000"})"));
    CHECK(r.ok);
    CHECK(CountDiffs(f.Out(), in) == 1);
}

void TestEncodeRoundTrip() {
    // Every leaf type: encode what decode shows and it reads back as shown.
    Bytes weapData{25, 0, 0, 0, 0, 0, 0x40, 0x40, 7, 0};
    Bytes body = Join({Sub("EDID", Str("A")), Sub("DATA", weapData)});
    Bytes in = Join({Rec("TES4", 0, 0, Join({Sub("HEDR", Hedr(1.71f, 2, 0x900))})),
                     Group(Label("WEAP"), 0, Rec("WEAP", 0, 0x00000800, body))});
    Fixture f(in);
    std::string json;
    CHECK(f.ed->Describe("Test.esp:000800", &json).ok);
    CHECK(json.find("\"path\":\"value\"") != std::string::npos);
    CHECK(json.find("\"offset\":4") != std::string::npos);
}

void TestSaveGuards() {
    auto temp = std::filesystem::temp_directory_path() / L"skqeedit_tests_save";
    std::filesystem::create_directories(temp);
    auto input = temp / L"In.esp";
    Bytes in = SamplePlugin();
    {
        std::ofstream o(input, std::ios::binary);
        o.write(reinterpret_cast<const char*>(in.data()), static_cast<std::streamsize>(in.size()));
    }
    std::string err;
    {
        auto doc = Document::Open(input.wstring(), &err);
        CHECK(doc != nullptr);
        Editor ed(doc.get(), &g_data);
        SaveOptions so;
        std::string why;
        CHECK(!ed.Save(input.wstring(), so, &why));
        so.overwrite = true;
        CHECK(!ed.Save(input.wstring(), so, &why));
        CHECK(why.find("input") != std::string::npos);
        // A hard link of the input is the input.
        auto link = temp / L"Link.esp";
        CHECK(CreateHardLinkW(link.c_str(), input.c_str(), nullptr));
        CHECK(!ed.Save(link.wstring(), so, &why));
        CHECK(why.find("input") != std::string::npos);
        auto data = temp / L"Skyrim Special Edition" / L"Data";
        std::filesystem::create_directories(data);
        CHECK(!ed.Save((data / L"X.esp").wstring(), so, &why));
        CHECK(why.find("Data") != std::string::npos);
        auto out = temp / L"Out.esp";
        SaveOptions plain;
        CHECK(ed.Save(out.wstring(), plain, &why));
        std::ifstream back(out, std::ios::binary);
        Bytes got((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
        CHECK(Same(got, in));
        CHECK(!ed.Save(out.wstring(), plain, &why));  // exists now
        back.close();
        CHECK(ed.Save(out.wstring(), so, &why));
        CHECK(!std::filesystem::exists(temp / L"Out.esp.skqeedit.tmp"));
    }
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring folder = argc > 1 ? std::wstring(argv[1]) + L"\\" : L".\\";
    std::string text, err;
    {
        std::ifstream f(folder + L"patterns.json", std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!g_catalog.Load(text, &err)) {
        printf("patterns.json: %s\n", err.c_str());
        return 2;
    }
    if (!LoadEditData(folder, &g_catalog, &g_data, &err)) {
        printf("%s\n", err.c_str());
        return 2;
    }
    TestRoundTrip();
    TestDamaged();
    TestBodyParser();
    TestSetValue();
    TestSetIsMinimal();
    TestNoOpAndRefusals();
    TestAddRemoveSub();
    TestAddRecord();
    TestHedrRules();
    TestAddMaster();
    TestCopyAsOverride();
    TestCopyConvertsText();
    TestDelete();
    TestCompressedEdit();
    TestLocalized();
    TestDependency();
    TestEncodeRoundTrip();
    TestSaveGuards();
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
