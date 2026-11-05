// A request for `spf export`: which forms of which plugins.
//
//   [Skyrim.esm]
//   Skyrim.esm:0132AA, Skyrim.esm:01367B
//   # comments and blank lines are skipped; a list can span lines
//   Skyrim.esm:01367C
//   [C:\mods\MyMod.esp]
//   MyMod.esp:000800 00012EB7
//
// One [plugin] line, then its forms, separated by commas, spaces or line ends.
// A plugin with no forms means every form it has (narrowed by --type).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace spf {

struct ExportTarget {
    std::string plugin;              // as written: a name or a path, UTF-8
    std::vector<std::string> forms;  // empty = every form
};

inline bool ParseExportRequest(std::string_view text, std::vector<ExportTarget>* out, std::string* error) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    size_t lineNo = 0;
    while (!text.empty()) {
        size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        ++lineNo;
        const char* blank = " \t\r";
        size_t a = line.find_first_not_of(blank);
        if (a == std::string_view::npos || line[a] == '#') continue;
        line = line.substr(a, line.find_last_not_of(blank) - a + 1);
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3) {
                *error = "line " + std::to_string(lineNo) + ": expected [plugin]";
                return false;
            }
            out->push_back({std::string(line.substr(1, line.size() - 2)), {}});
            continue;
        }
        if (out->empty()) {
            *error = "line " + std::to_string(lineNo) + ": forms before any [plugin] line";
            return false;
        }
        // Owner names may hold spaces ("My Mod.esp:000800"), so a space only
        // separates forms where the piece before it is complete: it has a colon
        // or is all hex.
        const char* seps = ", \t";
        std::string item;
        auto flush = [&] {
            if (!item.empty()) out->back().forms.push_back(item);
            item.clear();
        };
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == ',') {
                flush();
            } else if (c == ' ' || c == '\t') {
                bool complete = item.find(':') != std::string::npos ||
                                (!item.empty() && item.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
                size_t after = line.find_first_not_of(seps, i);
                bool nextIsComma = after != std::string_view::npos && line.find(',', i) == after;
                if (complete || nextIsComma) flush();
                else if (!item.empty()) item += c;
            } else {
                item += c;
            }
        }
        flush();
    }
    if (out->empty()) {
        *error = "the request names no plugin";
        return false;
    }
    return true;
}

}  // namespace spf
