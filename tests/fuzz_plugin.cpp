// libFuzzer target: every input is tried as a plugin (index + decode every
// field of every record), a strings file, and a zlib stream. Built only with
// -DSPF_FUZZ=ON (MSVC /fsanitize=fuzzer,address); see README.
//
//   spf_fuzz corpus_dir [-max_total_time=600]
// patterns.json is read from next to the exe.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "inflate.h"
#include "plugin.h"
#include "strings.h"

using namespace spf;

namespace {

const PatternCatalog& Catalog() {
    static PatternCatalog cat = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"patterns.json";
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        PatternCatalog c;
        std::string err;
        if (!c.Load(ss.str(), &err)) abort();  // a fuzzer without patterns tests nothing
        return c;
    }();
    return cat;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    if (auto plugin = Plugin::FromMemory(data, size, "Fuzz.esp", Catalog(), &err)) {
        const Plugin::FormIdMap formIds = [](uint32_t stored, const std::string&) {
            return Value::Int(stored);
        };
        for (const auto& r : plugin->records()) {
            for (const auto& f : plugin->Fields(r, &formIds)) {
                std::string json;
                WriteJson(f.value, &json);
            }
        }
    }
    if (size > 1) {
        StringTables t;
        t.Parse(data[0] % 3, data + 1, size - 1);
    }
    if (size > 4) {
        std::vector<uint8_t> out;
        uint32_t claim = data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
        ZlibInflate(data + 4, size - 4, claim, &out);
    }
    return 0;
}
