#include "catalog_check.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>

#include "plugin.h"

namespace skqe {
namespace {

class Sha256 {
public:
    Sha256() {
        static BCRYPT_ALG_HANDLE alg = [] {
            BCRYPT_ALG_HANDLE a = nullptr;
            if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) a = nullptr;
            return a;
        }();
        if (alg) BCryptCreateHash(alg, &h_, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (h_) BCryptDestroyHash(h_);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Add(const void* p, size_t n) {
        if (h_ && n) BCryptHashData(h_, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0);
    }
    void Add(const std::string& s) { Add(s.data(), s.size()); }
    std::string Hex() {
        uint8_t d[32] = {};
        if (h_) BCryptFinishHash(h_, d, 32, 0);
        static const char k[] = "0123456789abcdef";
        std::string s;
        for (uint8_t b : d) {
            s.push_back(k[b >> 4]);
            s.push_back(k[b & 15]);
        }
        return s;
    }

private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

std::string Hex(const uint8_t* b, size_t n) {
    static const char k[] = "0123456789ABCDEF";
    std::string s = "#";
    for (size_t i = 0; i < n; ++i) {
        s.push_back(k[b[i] >> 4]);
        s.push_back(k[b[i] & 15]);
    }
    return s;
}

struct KeyAcc {
    KeyStats st;
    std::unique_ptr<Sha256> sha = std::make_unique<Sha256>();
};

// One field: decode, count, hash.
void Visit(const PatternCatalog& cat, const std::string& rt, const std::string& fs, const uint8_t* data,
           size_t len, const DecodeContext& ctx, std::map<std::string, KeyAcc>* acc) {
    KeyAcc& a = (*acc)[rt + "." + fs];
    ++a.st.n;
    std::string text;
    const Pattern* pat = cat.Find(rt, fs);
    if (!pat) {
        ++a.st.nopat;
        text = Hex(data, len);
    } else {
        try {
            size_t used = 0;
            Value v = DecodeSubrecord(*pat, data, len, ctx, &used);
            if (used < len) ++a.st.left;
            WriteJson(v, &text);
        } catch (const std::exception&) {
            ++a.st.raw;
            text = Hex(data, len);
        }
    }
    text.push_back('\n');
    a.sha->Add(text);
}

PluginStats CheckOne(const PatternCatalog& cat, const std::wstring& path) {
    PluginStats out;
    std::string err;
    auto plugin = Plugin::Open(path, cat, &err);
    if (!plugin) {
        out.error = err.empty() ? "cannot read" : err;
        return out;
    }
    std::map<std::string, KeyAcc> acc;
    std::vector<uint8_t> scratch;
    for (const Record& r : plugin->records()) {
        ++out.records;
        DecodeContext ctx;
        ctx.rt = r.type;
        ctx.formVersion = r.formVersion;
        ctx.localized = plugin->localized();
        if (cat.Find(r.type, "Record Header") && r.offset + 24 <= plugin->size()) {
            ctx.fsig = "Record Header";
            Visit(cat, r.type, "Record Header", plugin->bytes() + r.offset, 24, ctx, &acc);
        }
        const uint8_t* body;
        size_t size;
        if (!plugin->Body(r, &scratch, &body, &size)) continue;
        int64_t lastEpft = 0;
        uint32_t big = 0;
        bool haveBig = false;
        for (size_t q = 0; q + 6 <= size;) {
            std::string fs(reinterpret_cast<const char*>(body + q), 4);
            size_t len = U16(body + q + 4);
            q += 6;
            if (haveBig) {
                len = big;
                haveBig = false;
            }
            if (q + len > size) break;
            const uint8_t* data = body + q;
            q += len;
            if (fs == "XXXX" && len >= 4) {
                big = U32(data);
                haveBig = true;
                continue;
            }
            if (r.type == "PERK") {
                if (fs == "PRKE") lastEpft = 0;
                else if (fs == "EPFT" && len) lastEpft = data[0];
                ctx.epft = lastEpft;
            }
            if (r.type == "GMST" && fs == "EDID" && len) ctx.edidPrefix = std::string(1, static_cast<char>(tolower(data[0])));
            if (r.type == "PACK" && fs == "ANAM") {
                ctx.lastAnam.assign(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), len));
            }
            ctx.fsig = fs;
            Visit(cat, r.type, fs, data, len, ctx, &acc);
        }
    }
    for (auto& [key, a] : acc) {
        a.st.sha = a.sha->Hex();
        out.keys[key] = a.st;
    }
    return out;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

CheckRun CheckPlugins(const PatternCatalog& cat, const std::wstring& folder, int threads) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
        if (!e.is_regular_file()) continue;
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext == L".esm" || ext == L".esp" || ext == L".esl") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<PluginStats> results(files.size());
    std::atomic<size_t> next{0};
    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < files.size();) results[i] = CheckOne(cat, files[i].wstring());
        });
    }
    for (auto& t : pool) t.join();
    CheckRun run;
    for (size_t i = 0; i < files.size(); ++i) run.plugins[Utf8(files[i].filename().wstring())] = std::move(results[i]);
    return run;
}

Value BaselineJson(const CheckRun& run) {
    Value doc = Value::Map();
    doc.Set("format", Value::Str("skqe-catalog-baseline/1"));
    Value plugins = Value::Map();
    std::map<std::string, KeyStats> totals;
    for (const auto& [name, p] : run.plugins) {
        Value pv = Value::Map();
        if (!p.error.empty()) pv.Set("error", Value::Str(p.error));
        pv.Set("records", Value::Int(static_cast<int64_t>(p.records)));
        Value keys = Value::Map();
        for (const auto& [key, k] : p.keys) {
            Value kv = Value::Map();
            kv.Set("n", Value::Int(static_cast<int64_t>(k.n)));
            kv.Set("raw", Value::Int(static_cast<int64_t>(k.raw)));
            kv.Set("left", Value::Int(static_cast<int64_t>(k.left)));
            kv.Set("nopat", Value::Int(static_cast<int64_t>(k.nopat)));
            kv.Set("sha", Value::Str(k.sha));
            keys.Set(key, std::move(kv));
            KeyStats& t = totals[key];
            t.n += k.n;
            t.raw += k.raw;
            t.left += k.left;
            t.nopat += k.nopat;
        }
        pv.Set("keys", std::move(keys));
        plugins.Set(name, std::move(pv));
    }
    Value tv = Value::Map();
    for (const auto& [key, t] : totals) {
        Value kv = Value::Map();
        kv.Set("n", Value::Int(static_cast<int64_t>(t.n)));
        kv.Set("raw", Value::Int(static_cast<int64_t>(t.raw)));
        kv.Set("left", Value::Int(static_cast<int64_t>(t.left)));
        kv.Set("nopat", Value::Int(static_cast<int64_t>(t.nopat)));
        tv.Set(key, std::move(kv));
    }
    doc.Set("keys", std::move(tv));
    doc.Set("plugins", std::move(plugins));
    return doc;
}

bool BaselineFromJson(const Value& doc, CheckRun* run, std::string* error) {
    const Value* f = doc.Find("format");
    if (!f || !f->IsString() || f->AsString() != "skqe-catalog-baseline/1") {
        *error = "not a catalog check baseline";
        return false;
    }
    const Value* plugins = doc.Find("plugins");
    if (!plugins || plugins->kind() != Value::Kind::Map) {
        *error = "baseline has no plugins";
        return false;
    }
    auto num = [](const Value& v, const char* k) {
        const Value* x = v.Find(k);
        int64_t i = 0;
        if (x) x->ToInt(&i);
        return static_cast<uint64_t>(i);
    };
    for (const auto& [name, pv] : plugins->Members()) {
        PluginStats& p = run->plugins[name];
        if (const Value* e = pv.Find("error"); e && e->IsString()) p.error = e->AsString();
        p.records = num(pv, "records");
        if (const Value* keys = pv.Find("keys")) {
            for (const auto& [key, kv] : keys->Members()) {
                KeyStats& k = p.keys[key];
                k.n = num(kv, "n");
                k.raw = num(kv, "raw");
                k.left = num(kv, "left");
                k.nopat = num(kv, "nopat");
                if (const Value* s = kv.Find("sha"); s && s->IsString()) k.sha = s->AsString();
            }
        }
    }
    return true;
}

std::vector<std::string> CompareBaseline(const CheckRun& base, const CheckRun& now,
                                         const std::vector<std::string>& expectChanged) {
    std::vector<std::string> out;
    std::set<std::string> expect(expectChanged.begin(), expectChanged.end());
    std::set<std::string> changed;
    std::map<std::string, std::pair<uint64_t, uint64_t>> raw;  // key -> (base, now)
    std::set<std::string> names;
    for (const auto& p : base.plugins) names.insert(p.first);
    for (const auto& p : now.plugins) names.insert(p.first);
    static const PluginStats kNone;
    static const KeyStats kNoKey;
    for (const std::string& name : names) {
        auto bi = base.plugins.find(name);
        auto ni = now.plugins.find(name);
        if (bi == base.plugins.end()) {
            out.push_back(name + ": not in the baseline");
            continue;
        }
        if (ni == now.plugins.end()) {
            out.push_back(name + ": in the baseline, not read now");
            continue;
        }
        const PluginStats& b = bi->second;
        const PluginStats& n = ni->second;
        if (b.error != n.error) out.push_back(name + ": read error \"" + b.error + "\" -> \"" + n.error + "\"");
        std::set<std::string> keys;
        for (const auto& k : b.keys) keys.insert(k.first);
        for (const auto& k : n.keys) keys.insert(k.first);
        for (const std::string& key : keys) {
            auto bk = b.keys.find(key);
            auto nk = n.keys.find(key);
            const KeyStats& x = bk == b.keys.end() ? kNoKey : bk->second;
            const KeyStats& y = nk == n.keys.end() ? kNoKey : nk->second;
            raw[key].first += x.raw;
            raw[key].second += y.raw;
            if (x.sha != y.sha || x.n != y.n || x.nopat != y.nopat) {
                changed.insert(key);
                if (!expect.count(key)) out.push_back(name + ": " + key + ": decoded values changed");
            }
        }
    }
    for (const auto& [key, r] : raw) {
        if (r.second > r.first) {
            out.push_back(key + ": raw fallbacks rose from " + std::to_string(r.first) + " to " + std::to_string(r.second));
        }
    }
    for (const std::string& key : expect) {
        if (!changed.count(key)) out.push_back(key + ": listed in --expect-changed but did not change");
    }
    return out;
}

}  // namespace skqe
