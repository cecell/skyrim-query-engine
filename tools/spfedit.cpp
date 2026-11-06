// spfedit: writes and edits Skyrim SE plugins. `spfedit help` lists the commands.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "edit/ops.h"
#include "edit/roundtrip.h"
#include "plugin.h"

using namespace spf::edit;
namespace fs = std::filesystem;

namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += '"';
        q += c;
    }
    return q + "\"";
}

bool IsPlugin(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    std::transform(e.begin(), e.end(), e.begin(), towlower);
    return e == L".esm" || e == L".esp" || e == L".esl";
}

void Collect(const std::wstring& arg, std::vector<std::wstring>* files) {
    std::error_code ec;
    if (fs::is_directory(arg, ec)) {
        for (fs::recursive_directory_iterator it(arg, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && IsPlugin(it->path())) files->push_back(it->path().wstring());
        }
    } else {
        files->push_back(arg);
    }
}

int CmdRoundTrip(const std::vector<std::wstring>& args) {
    std::vector<std::wstring> files;
    std::wstring csvPath;
    RoundTripOptions opt;
    unsigned threads = std::thread::hardware_concurrency();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--csv" && i + 1 < args.size()) csvPath = args[++i];
        else if (args[i] == L"--threads" && i + 1 < args.size()) threads = static_cast<unsigned>(_wtoi(args[++i].c_str()));
        else if (args[i] == L"--recompress") opt.recompress = true;
        else Collect(args[i], &files);
    }
    if (files.empty()) {
        fprintf(stderr, "roundtrip: no plugins given\n");
        return 2;
    }
    LARGE_INTEGER start, stop, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    std::vector<RoundTripResult> results(files.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next++) < files.size();) results[i] = RoundTrip(files[i], opt);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    QueryPerformanceCounter(&stop);
    double seconds = double(stop.QuadPart - start.QuadPart) / freq.QuadPart;

    if (!csvPath.empty()) {
        std::ofstream csv(fs::path(csvPath), std::ios::binary);
        csv << "path,size,status,failure,ms,records,groups,compressed,subrecords,xxxx,xxxx_odd,opaque,opaque_reasons,"
               "first_opaque,inflate_failures,trailing_bytes,duplicate_formids,hedr_count,computed_count,"
               "recompress_same,recompress_different,recompress_not_smaller\n";
        for (const auto& r : results) {
            std::string status = !r.opened ? "refused" : r.l0 && r.l1 && r.l2 ? "ok" : "FAILED";
            std::string reasons;
            for (const auto& e : r.opaqueBy) reasons += (reasons.empty() ? "" : ";") + e.first + "=" + std::to_string(e.second);
            csv << CsvField(Narrow(r.path)) << ',' << r.size << ',' << status << ','
                << CsvField(r.opened ? r.failure : r.refused) << ',' << r.ms << ',' << r.records << ',' << r.groups << ','
                << r.compressed << ',' << r.subrecords << ',' << r.xxxx << ',' << r.xxxxOddSize << ',' << r.opaque << ','
                << CsvField(reasons) << ',' << CsvField(r.firstOpaque) << ',' << r.inflateFailures << ','
                << r.trailingBytes << ',' << r.duplicateFormIds << ',' << r.hedrCount << ',' << r.computedCount << ','
                << r.recompressSame << ',' << r.recompressDifferent << ',' << r.recompressNotSmaller << '\n';
        }
    }

    uint64_t ok = 0, refused = 0, failed = 0, bytes = 0, records = 0, groups = 0, compressed = 0, subs = 0, opaque = 0,
             opaquePlugins = 0, trailing = 0, dups = 0, hedrOff = 0, same = 0, diff = 0, notSmaller = 0, xxxx = 0, odd = 0;
    std::map<std::string, uint64_t> reasons;
    for (const auto& r : results) {
        if (!r.opened) {
            ++refused;
            printf("REFUSED  %s: %s\n", Narrow(r.path).c_str(), r.refused.c_str());
            continue;
        }
        if (r.l0 && r.l1 && r.l2) {
            ++ok;
        } else {
            ++failed;
            printf("FAILED   %s: %s\n", Narrow(r.path).c_str(), r.failure.c_str());
        }
        bytes += r.size;
        records += r.records;
        groups += r.groups;
        compressed += r.compressed;
        subs += r.subrecords;
        opaque += r.opaque;
        opaquePlugins += r.opaque != 0;
        trailing += r.trailingBytes != 0;
        dups += r.duplicateFormIds != 0;
        hedrOff += !r.hedrCountMatches;
        same += r.recompressSame;
        diff += r.recompressDifferent;
        notSmaller += r.recompressNotSmaller;
        xxxx += r.xxxx;
        odd += r.xxxxOddSize;
        for (const auto& e : r.opaqueBy) reasons[e.first] += e.second;
    }
    printf("plugins %zu: ok %llu, refused %llu, failed %llu\n", files.size(), ok, refused, failed);
    printf("bytes %llu, records %llu, groups %llu, compressed %llu, subrecords %llu, xxxx %llu (odd %llu)\n", bytes, records,
           groups, compressed, subs, xxxx, odd);
    printf("opaque records %llu in %llu plugins", opaque, opaquePlugins);
    for (const auto& e : reasons) printf(", %s %llu", e.first.c_str(), e.second);
    printf("\nplugins with trailing bytes %llu, with duplicate FormIDs %llu, HEDR count differs %llu\n", trailing, dups, hedrOff);
    if (opt.recompress) printf("recompress level 9: same %llu, different %llu, not smaller %llu\n", same, diff, notSmaller);
    printf("%.2f s on %u threads\n", seconds, threads);
    return failed ? 1 : 0;
}

// Forward: defined below.
std::wstring ExeFolder();
bool ReadFileText(const std::wstring& path, std::string* out);

// The tracer against the decoder, every leaf encoded back, group placement
// against the writer's rules, across whole plugins.
int CmdSelfCheck(const std::vector<std::wstring>& args) {
    std::vector<std::wstring> files;
    unsigned threads = std::thread::hardware_concurrency();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--threads" && i + 1 < args.size()) threads = static_cast<unsigned>(_wtoi(args[++i].c_str()));
        else Collect(args[i], &files);
    }
    spf::PatternCatalog cat;
    std::string text, err;
    if (!ReadFileText(ExeFolder() + L"patterns.json", &text) || !cat.Load(text, &err)) {
        fprintf(stderr, "patterns.json: %s\n", err.c_str());
        return 2;
    }
    LARGE_INTEGER start, stop, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    std::vector<SelfCheck> results(files.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next++) < files.size();) results[i] = RunSelfCheck(files[i], cat);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    QueryPerformanceCounter(&stop);
    SelfCheck sum;
    uint64_t refused = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const SelfCheck& r = results[i];
        if (!r.refused.empty()) {
            ++refused;
            continue;
        }
        sum.records += r.records;
        sum.subs += r.subs;
        sum.leaves += r.leaves;
        sum.undecoded += r.undecoded;
        sum.traceDiffers += r.traceDiffers;
        sum.leafOutOfRange += r.leafOutOfRange;
        sum.leafPathWrong += r.leafPathWrong;
        sum.topOutOfOrder += r.topOutOfOrder;
        sum.cellsChecked += r.cellsChecked;
        sum.cellGroupWrong += r.cellGroupWrong;
        sum.childGroupWrong += r.childGroupWrong;
        sum.orphanGroups += r.orphanGroups;
        if (sum.firstProblem.empty() && !r.firstProblem.empty()) sum.firstProblem = Narrow(files[i]) + ": " + r.firstProblem;
        if (sum.firstPlacement.empty() && !r.firstPlacement.empty()) sum.firstPlacement = Narrow(files[i]) + ": " + r.firstPlacement;
        for (const auto& e : r.encode) {
            auto& s = sum.encode[e.first];
            s.total += e.second.total;
            s.identical += e.second.identical;
            s.sameDisplay += e.second.sameDisplay;
            s.differs += e.second.differs;
            s.refused += e.second.refused;
            if (s.example.empty()) s.example = e.second.example;
        }
    }
    printf("plugins %zu (refused %llu), records %llu, subrecords %llu, values %llu, subrecords not decoded %llu\n", files.size(), refused,
           sum.records, sum.subs, sum.leaves, sum.undecoded);
    printf("trace differs from decode %llu, leaf out of range %llu, leaf path wrong %llu\n", sum.traceDiffers, sum.leafOutOfRange, sum.leafPathWrong);
    if (!sum.firstProblem.empty()) printf("  first: %s\n", sum.firstProblem.c_str());
    printf("top groups out of order %llu, cells checked %llu, cell group wrong %llu, child group wrong %llu, owned group misplaced %llu\n",
           sum.topOutOfOrder, sum.cellsChecked, sum.cellGroupWrong, sum.childGroupWrong, sum.orphanGroups);
    if (!sum.firstPlacement.empty()) printf("  first: %s\n", sum.firstPlacement.c_str());
    printf("%-22s %12s %12s %10s %10s %8s\n", "leaf type", "values", "identical", "same show", "differs", "refused");
    for (const auto& e : sum.encode) {
        printf("%-22s %12llu %12llu %10llu %10llu %8llu  %s\n", e.first.c_str(), e.second.total, e.second.identical, e.second.sameDisplay,
               e.second.differs, e.second.refused, e.second.differs || e.second.refused ? e.second.example.c_str() : "");
    }
    printf("%.2f s on %u threads\n", double(stop.QuadPart - start.QuadPart) / freq.QuadPart, threads);
    return sum.traceDiffers || sum.leafOutOfRange || sum.leafPathWrong ? 1 : 0;
}

std::wstring ExeFolder() {
    wchar_t buf[4096];
    GetModuleFileNameW(nullptr, buf, 4096);
    std::wstring p(buf);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool ReadFileText(const std::wstring& path, std::string* out) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct Session {
    spf::PatternCatalog catalog;
    EditData data;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Editor> editor;
};

bool OpenSession(const std::wstring& plugin, Session* s) {
    std::string text, err;
    if (!ReadFileText(ExeFolder() + L"patterns.json", &text) || !s->catalog.Load(text, &err)) {
        fprintf(stderr, "patterns.json: %s\n", err.empty() ? "cannot read it next to spfedit.exe" : err.c_str());
        return false;
    }
    if (!LoadEditData(ExeFolder(), &s->catalog, &s->data, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    s->doc = Document::Open(plugin, &err);
    if (!s->doc) {
        fprintf(stderr, "%s: %s\n", Narrow(plugin).c_str(), err.c_str());
        return false;
    }
    s->editor = std::make_unique<Editor>(s->doc.get(), &s->data);
    return true;
}

struct OutputArgs {
    std::wstring out;
    bool overwrite = false;
    bool dryRun = false;
};

// Applies ops in order; the first failure stops it and nothing is saved.
int RunOps(const std::wstring& plugin, const std::vector<spf::Value>& ops, const OutputArgs& o) {
    if (o.out.empty() && !o.dryRun) {
        fprintf(stderr, "give --out <file>, or --dry-run\n");
        return 2;
    }
    LARGE_INTEGER start, stop, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    Session s;
    if (!OpenSession(plugin, &s)) return 2;
    std::string json = "{\"results\":[";
    bool ok = true;
    for (size_t i = 0; i < ops.size(); ++i) {
        OpResult r = s.editor->Apply(ops[i]);
        if (i) json += ",";
        json += ResultJson(r);
        if (!r.ok) {
            ok = false;
            break;
        }
    }
    json += "]";
    std::string saveError;
    if (ok && !o.dryRun) {
        SaveOptions so;
        so.overwrite = o.overwrite;
        if (!s.editor->Save(o.out, so, &saveError)) ok = false;
    }
    QueryPerformanceCounter(&stop);
    json += ",\"ok\":" + std::string(ok ? "true" : "false");
    if (!saveError.empty()) {
        std::string quoted;
        spf::WriteJson(spf::Value::Str(saveError), &quoted);
        json += ",\"save_error\":" + quoted;
    }
    if (ok && !o.dryRun) {
        std::string quoted;
        spf::WriteJson(spf::Value::Str(Narrow(o.out)), &quoted);
        json += ",\"saved\":" + quoted;
    }
    json += ",\"ms\":" + std::to_string(static_cast<long long>(1000.0 * (stop.QuadPart - start.QuadPart) / freq.QuadPart));
    json += "}";
    puts(json.c_str());
    return ok ? 0 : 1;
}

int CmdApply(const std::vector<std::wstring>& args) {
    std::wstring plugin, opsPath;
    OutputArgs o;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--ops" && i + 1 < args.size()) opsPath = args[++i];
        else if (args[i] == L"--out" && i + 1 < args.size()) o.out = args[++i];
        else if (args[i] == L"--overwrite") o.overwrite = true;
        else if (args[i] == L"--dry-run") o.dryRun = true;
        else plugin = args[i];
    }
    std::string text, err;
    if (plugin.empty() || opsPath.empty() || !ReadFileText(opsPath, &text)) {
        fprintf(stderr, "usage: spfedit apply <plugin> --ops ops.json (--out <file> | --dry-run) [--overwrite]\n");
        return 2;
    }
    spf::Value list;
    if (!spf::ParseJson(text, &list, &err) || list.kind() != spf::Value::Kind::List) {
        fprintf(stderr, "%ls: %s\n", opsPath.c_str(), err.empty() ? "expected a JSON list of ops" : err.c_str());
        return 2;
    }
    return RunOps(plugin, list.Items(), o);
}

// spfedit <op> <plugin> --key value ...: one op built from flags.
int CmdOneOp(const std::wstring& opName, const std::vector<std::wstring>& args) {
    std::wstring plugin;
    OutputArgs o;
    spf::Value op = spf::Value::Map();
    op.Set("op", spf::Value::Str(Narrow(opName)));
    // These are never numbers, whatever they look like.
    static const char* const kText[] = {"form", "field", "sig", "from", "name", "item", "raw", "edid", "parent", "type",
                                        "after", "before", "author", "description", "objectId"};
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--out" && i + 1 < args.size()) {
            o.out = args[++i];
        } else if (a == L"--overwrite") {
            o.overwrite = true;
        } else if (a == L"--dry-run") {
            o.dryRun = true;
        } else if (a.rfind(L"--", 0) == 0) {
            std::string key = Narrow(a.substr(2));
            bool nextIsValue = i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0;
            if (!nextIsValue) {
                op.Set(key, spf::Value::Bool(true));
                continue;
            }
            std::string val = Narrow(args[++i]);
            if (key == "text") {
                op.Set("value", spf::Value::Str(val));
                continue;
            }
            spf::Value parsed;
            std::string err;
            bool text = std::any_of(std::begin(kText), std::end(kText), [&](const char* k) { return key == k; });
            if (text || !spf::ParseJson(val, &parsed, &err)) parsed = spf::Value::Str(val);
            op.Set(key, std::move(parsed));
        } else {
            plugin = a;
        }
    }
    if (plugin.empty()) {
        fprintf(stderr, "give the plugin (spfedit help)\n");
        return 2;
    }
    return RunOps(plugin, {op}, o);
}

int CmdShow(const std::vector<std::wstring>& args) {
    std::wstring plugin;
    std::string form = "TES4";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--form" && i + 1 < args.size()) form = Narrow(args[++i]);
        else plugin = args[i];
    }
    if (plugin.empty()) {
        fprintf(stderr, "usage: spfedit show <plugin> [--form F]\n");
        return 2;
    }
    Session s;
    if (!OpenSession(plugin, &s)) return 2;
    std::string json;
    OpResult r = s.editor->Describe(form, &json);
    if (!r.ok) {
        puts(ResultJson(r).c_str());
        return 1;
    }
    puts(json.c_str());
    return 0;
}

void Help() {
    puts(R"(spfedit: writes and edits Skyrim SE plugins. Never writes into a Skyrim
Special Edition\Data folder, never replaces its input, and leaves every byte it
was not asked to change as it was.

  spfedit apply <plugin> --ops ops.json (--out <file> | --dry-run) [--overwrite]
      Run a list of ops. The first failure stops it and nothing is saved.
  spfedit <op> <plugin> --key value ... (--out <file> | --dry-run) [--overwrite]
      One op from flags: spfedit set X.esp --form X.esp:000800 --field DATA.damage
      --value 9 --out Y.esp. --text gives a value that is text even if it looks
      like a number. Numbers and JSON parse as JSON.
  spfedit show <plugin> [--form F]
      A record (default TES4): every value, its path, byte offset and size.
  spfedit ops
      The ops and their arguments.
  spfedit selfcheck <plugin|folder>... [--threads n]
      Read-only. The tracer against the decoder on every subrecord, every value
      encoded back and compared, group placement against the writer's rules.
  spfedit roundtrip <plugin|folder>... [--csv out.csv] [--recompress] [--threads n]
      Read each plugin, write it back in memory at three levels, compare with the
      input. Writes nothing except --csv.

Exit codes: 0 done, 1 an op or check failed, 2 bad usage or unreadable input.)");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || std::wstring(argv[1]) == L"help" || std::wstring(argv[1]) == L"--help") {
        Help();
        return argc < 2 ? 2 : 0;
    }
    std::wstring cmd = argv[1];
    std::vector<std::wstring> args(argv + 2, argv + argc);
    if (cmd == L"roundtrip") return CmdRoundTrip(args);
    if (cmd == L"selfcheck") return CmdSelfCheck(args);
    if (cmd == L"apply") return CmdApply(args);
    if (cmd == L"show") return CmdShow(args);
    if (cmd == L"ops") {
        puts(OpHelp());
        return 0;
    }
    for (const wchar_t* op : {L"set", L"add-sub", L"remove-sub", L"add-item", L"remove-item", L"add-record",
                              L"copy-as-override", L"delete-record", L"add-master", L"set-flags", L"set-header"}) {
        if (cmd == op) return CmdOneOp(cmd, args);
    }
    fprintf(stderr, "unknown command: %ls (try: spfedit help)\n", cmd.c_str());
    return 2;
}
