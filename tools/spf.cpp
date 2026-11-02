// spf: look inside a Skyrim SE plugin and export fields.
//
//   spf info   <plugin>                         masters and record counts by type
//   spf fields <plugin> <TYPE>                  which fields that type has here, with sub-fields
//   spf export <plugin> [options]               the chosen fields of the chosen forms
//       --type WEAP[,ARMO...]     only these record types
//       --form 00012EB7[,...]     only these forms (as stored, or Skyrim.esm:012EB7)
//       --fields EDID,DATA.damage only these fields ("*" or omitted = all);
//                                 a field is a subrecord, then a dotted path inside it
//       --csv | --json            output format (default json)
//       --out <file>              write there instead of the console
//   --patterns <file>             pattern catalog (default: patterns.json next to spf.exe)
//
// A field that occurs once is a value; one that repeats (SNAM, LVLO, CTDA...)
// is a list of its occurrences.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/plugin.h"

using namespace spf;

namespace {

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// "00012EB7" (as stored) or "Skyrim.esm:012EB7" (owner + object ID).
bool ParseFormSpec(const Plugin& p, const std::string& spec, uint32_t* out) {
    size_t colon = spec.rfind(':');
    if (colon == std::string::npos) {
        *out = static_cast<uint32_t>(strtoul(spec.c_str(), nullptr, 16));
        return true;
    }
    std::string owner = spec.substr(0, colon);
    uint32_t local = static_cast<uint32_t>(strtoul(spec.c_str() + colon + 1, nullptr, 16)) & 0xFFFFFF;
    auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
    if (same(owner, p.name())) {
        *out = (static_cast<uint32_t>(p.masters().size()) << 24) | local;
        return true;
    }
    for (size_t i = 0; i < p.masters().size(); ++i) {
        if (same(owner, p.masters()[i])) {
            *out = (static_cast<uint32_t>(i) << 24) | local;
            return true;
        }
    }
    return false;
}

// One requested field: the subrecord signature and the path inside it.
struct FieldSpec {
    std::string text, sig, path;
};

std::vector<FieldSpec> ParseFields(const std::string& s) {
    std::vector<FieldSpec> out;
    for (const auto& item : SplitList(s)) {
        FieldSpec f;
        f.text = item;
        size_t sep = item.find_first_of(".[");
        f.sig = item.substr(0, sep);
        f.path = sep == std::string::npos ? "" : item.substr(sep);
        out.push_back(f);
    }
    return out;
}

// Every occurrence of the field in a record, narrowed by the path; one value,
// a list for several, null for none.
Value Gather(const std::vector<Field>& fields, const FieldSpec& spec) {
    std::vector<Value> found;
    for (const auto& f : fields) {
        if (_stricmp(f.sig.c_str(), spec.sig.c_str()) != 0) continue;
        if (spec.path.empty()) found.push_back(f.value);
        else for (auto& v : Select(f.value, spec.path)) found.push_back(std::move(v));
    }
    if (found.empty()) return Value();
    if (found.size() == 1) return std::move(found[0]);
    return Value::List(std::move(found));
}

// All fields of a record as one object; repeated subrecords become lists.
Value AllFields(const std::vector<Field>& fields) {
    std::map<std::string, size_t> count;
    for (const auto& f : fields) ++count[f.sig];
    Value out = Value::Map();
    std::map<std::string, Value> lists;
    for (const auto& f : fields) {
        if (count[f.sig] == 1) {
            out.Set(f.sig, f.value);
        } else {
            if (!out.Find(f.sig)) out.Set(f.sig, Value::List());
            lists[f.sig].Items().push_back(f.value);
        }
    }
    for (auto& [sig, list] : lists) {
        list = Value::List(std::move(lists[sig].Items()));
        out.Set(sig, std::move(list));
    }
    return out;
}

std::string CsvCell(const Value& v) {
    std::string s;
    if (v.IsNull()) return "";
    if (v.IsString()) s = v.AsString();
    else WriteJson(v, &s);
    if (s.find_first_of(",\"\r\n") != std::string::npos) {
        std::string q = "\"";
        for (char c : s) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    }
    return s;
}

int Usage() {
    fprintf(stderr,
            "usage:\n"
            "  spf info   <plugin>\n"
            "  spf fields <plugin> <TYPE>\n"
            "  spf export <plugin> [--type T,..] [--form ID,..] [--fields F,..] [--csv|--json] [--out file]\n"
            "  (all commands: --patterns <file>)\n");
    return 2;
}

// Collects the shape of a value: "DATA", "DATA.damage", "DATA.flags[]"...
void Shape(const Value& v, const std::string& prefix, std::set<std::string>* out) {
    out->insert(prefix);
    if (v.kind() == Value::Kind::Map) {
        for (const auto& m : v.Members()) Shape(m.second, prefix + "." + m.first, out);
    } else if (v.kind() == Value::Kind::List) {
        for (const auto& it : v.Items())
            if (it.kind() == Value::Kind::Map) Shape(it, prefix, out);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return Usage();
    std::string cmd = Utf8(argv[1]);
    std::wstring pluginPath = argv[2];
    std::string types, forms, fieldList, outPath, positional;
    std::wstring patternsPath;
    bool csv = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = Utf8(argv[i]);
        auto next = [&](std::string* dst) {
            if (i + 1 < argc) *dst = Utf8(argv[++i]);
        };
        if (a == "--type") next(&types);
        else if (a == "--form") next(&forms);
        else if (a == "--fields") next(&fieldList);
        else if (a == "--out") next(&outPath);
        else if (a == "--csv") csv = true;
        else if (a == "--json") csv = false;
        else if (a == "--patterns" && i + 1 < argc) patternsPath = argv[++i];
        else positional = a;
    }
    if (patternsPath.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        patternsPath = exe;
        patternsPath = patternsPath.substr(0, patternsPath.find_last_of(L"\\/") + 1) + L"patterns.json";
    }
    std::ifstream pf(patternsPath, std::ios::binary);
    std::stringstream ss;
    ss << pf.rdbuf();
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(ss.str(), &err)) {
        fprintf(stderr, "patterns: %s\n", err.empty() ? "cannot read patterns.json" : err.c_str());
        return 2;
    }
    auto plugin = Plugin::Open(pluginPath, cat, &err);
    if (!plugin) {
        fprintf(stderr, "%s: %s\n", Utf8(pluginPath).c_str(), err.c_str());
        return 1;
    }

    std::string out;
    if (cmd == "info") {
        std::map<std::string, size_t> counts;
        for (const auto& r : plugin->records()) ++counts[r.type];
        out += "plugin: " + plugin->name() + (plugin->localized() ? " (localized)" : "") + "\n";
        for (size_t i = 0; i < plugin->masters().size(); ++i) {
            char buf[16];
            snprintf(buf, sizeof(buf), "  [%02zX] ", i);
            out += buf + plugin->masters()[i] + "\n";
        }
        out += "records: " + std::to_string(plugin->records().size() - 1) + "\n";
        for (const auto& [t, n] : counts)
            if (t != "TES4") out += "  " + t + " " + std::to_string(n) + "\n";
    } else if (cmd == "fields") {
        std::string type = Upper(positional);
        if (type.empty()) return Usage();
        std::map<std::string, std::pair<size_t, std::set<std::string>>> seen;  // sig -> (records, shapes)
        auto recs = plugin->OfType(type);
        for (const Record* r : recs) {
            std::set<std::string> inThis;
            for (const auto& f : plugin->Fields(*r)) {
                Shape(f.value, f.sig, &seen[f.sig].second);
                if (inThis.insert(f.sig).second) ++seen[f.sig].first;
            }
        }
        out += type + ": " + std::to_string(recs.size()) + " records\n";
        for (const auto& [sig, info] : seen) {
            out += "  " + sig + "  (in " + std::to_string(info.first) + ")\n";
            for (const auto& s : info.second)
                if (s != sig) out += "    " + s + "\n";
        }
    } else if (cmd == "export") {
        std::set<std::string> typeSet;
        for (const auto& t : SplitList(types)) typeSet.insert(Upper(t));
        std::vector<const Record*> recs;
        if (!forms.empty()) {
            for (const auto& spec : SplitList(forms)) {
                uint32_t fid;
                const Record* r = ParseFormSpec(*plugin, spec, &fid) ? plugin->Find(fid) : nullptr;
                if (!r) fprintf(stderr, "no such form here: %s\n", spec.c_str());
                else if (typeSet.empty() || typeSet.count(r->type)) recs.push_back(r);
            }
        } else {
            for (const auto& r : plugin->records())
                if (r.type != "TES4" && (typeSet.empty() || typeSet.count(r.type))) recs.push_back(&r);
        }
        std::vector<FieldSpec> specs = fieldList.empty() || fieldList == "*" ? std::vector<FieldSpec>{}
                                                                              : ParseFields(fieldList);
        if (csv) {
            if (specs.empty()) {
                fprintf(stderr, "--csv needs --fields (a fixed set of columns)\n");
                return 2;
            }
            out += "form,type,in";
            for (const auto& s : specs) out += "," + CsvCell(Value::Str(s.text));
            out += "\n";
        } else {
            out += "[\n";
        }
        bool first = true;
        for (const Record* r : recs) {
            auto fields = plugin->Fields(*r);
            Value form = Value::Str(plugin->FormKey(r->formId));
            Value parent = r->parent ? Value::Str(plugin->FormKey(r->parent)) : Value();
            if (csv) {
                out += CsvCell(form) + "," + r->type + "," + CsvCell(parent);
                for (const auto& s : specs) out += "," + CsvCell(Gather(fields, s));
                out += "\n";
            } else {
                Value obj = Value::Map();
                obj.Set("form", form);
                obj.Set("type", Value::Str(r->type));
                if (!parent.IsNull()) obj.Set("in", parent);
                if (r->flags & 0x20) obj.Set("deleted", Value::Bool(true));
                if (specs.empty()) {
                    obj.Set("fields", AllFields(fields));
                } else {
                    Value sel = Value::Map();
                    for (const auto& s : specs) sel.Set(s.text, Gather(fields, s));
                    obj.Set("fields", std::move(sel));
                }
                if (!first) out += ",\n";
                first = false;
                WriteJson(obj, &out);
            }
        }
        if (!csv) out += "\n]\n";
    } else {
        return Usage();
    }

    if (!outPath.empty()) {
        std::ofstream f(outPath, std::ios::binary);
        f << out;
    } else {
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}
