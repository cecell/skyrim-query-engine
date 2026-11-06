// Localized plugin strings (.STRINGS / .DLSTRINGS / .ILSTRINGS): the text a
// localized plugin's name and description fields point at by number.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace skqe {

class StringTables {
public:
    // Looks for <plugin>_<language>.<ext> in <dir>\Strings, then inside
    // <plugin>.bsa, then "Skyrim - Interface.bsa" (where Bethesda's are;
    // BSAs through bsa-reader, compressed ones too). Missing or damaged
    // tables are fine: lookups just fail.
    void Load(const std::wstring& dir, const std::string& pluginBaseName, const std::string& language = "english");

    // Parses one strings file already in memory into table `c` (0 strings,
    // 1 dlstrings, 2 ilstrings). Entries pointing outside the file are
    // skipped. Returns false when the file's own sizes do not fit.
    bool Parse(int c, const uint8_t* data, size_t size);

    // The text for an ID, trying the field's own table first ("strings",
    // "dlstrings" or "ilstrings"; empty = strings).
    const std::string* Find(uint32_t id, const std::string& cls) const;
    // One table: 0 strings, 1 dlstrings, 2 ilstrings.
    const std::unordered_map<uint32_t, std::string>& table(int c) const { return tables_[c]; }
    bool empty() const { return tables_[0].empty() && tables_[1].empty() && tables_[2].empty(); }

private:
    std::unordered_map<uint32_t, std::string> tables_[3];  // strings, dlstrings, ilstrings
};

}  // namespace skqe
