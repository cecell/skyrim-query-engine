// Identical To Master (ITM) detection, the way xEdit's Quick Auto Clean
// decides it: the plugin is loaded with its masters only, and an override is
// ITM when every compared field equals the defining master's value and no
// other master changed it in between. Fields are decoded with the pattern
// engine; xEdit's comparison rules (ignored fields, sorted arrays, float
// precision, FormIDs by target) are applied on top.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pattern_engine.h"

namespace skqe {

struct ItmRecord {
    uint32_t formId = 0;     // as stored in the plugin
    std::string type;        // record signature
};

struct ItmResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> missingMasters;  // masters not found next to the plugin
    uint32_t overrides = 0;  // override records examined
    std::vector<ItmRecord> itms;
};

// Masters are looked up in the plugin's own folder.
ItmResult FindItms(const std::wstring& pluginPath, const PatternCatalog& catalog);

// Test hook: look for masters in this folder instead of next to the plugin.
void SetItmMastersFolder(const std::wstring& folder);

// Debug aid: why a single override is or is not ITM (one line per finding).
std::vector<std::string> ExplainItm(const std::wstring& pluginPath, const PatternCatalog& catalog, uint32_t formId);

}  // namespace skqe
