// Finds where a user's mod manager keeps its mods for a Skyrim SE install.
//
// Vortex writes the real staging folder into a manifest in the folder it deploys
// to (Data\vortex.deployment.json, "stagingPath"), which follows a custom
// location and survives a purge. Its default folder is only a fallback for
// nothing deployed yet: on a machine that moved the staging folder, the default
// can still exist with Vortex's marker in it. MO2 instances are the global ones
// in %LOCALAPPDATA%\ModOrganizer and portable ones next to an executable named
// in nxmhandler.ini.
#pragma once

#include <string>
#include <vector>

namespace skqe {

struct StagingCandidate {
    std::string manager;       // "vortex" or "mo2"
    std::wstring staging;      // the folder holding one subfolder per mod
    std::wstring instance;     // MO2: the folder with ModOrganizer.ini
    std::string instanceName;  // MO2: as in the registry; empty = portable
    std::string profile;       // MO2: the selected profile
    std::string source;        // how it was found, for people
    bool deployed = false;     // Vortex: the manifest lists files
    bool matchesData = false;  // this install's Data, not another game copy
};

// Where to look. CurrentStagingEnv() reads the real machine; tests pass their own.
struct StagingEnv {
    std::wstring appData, localAppData, programData;
    std::string mo2CurrentInstance;  // HKCU; empty = none set, or portable
};
StagingEnv CurrentStagingEnv();

struct StagingResult {
    std::wstring data;                  // the Data folder looked at
    StagingCandidate chosen;
    std::vector<StagingCandidate> all;  // everything found, chosen included
};

// data: empty = the installed game's, from the registry. manager: "", "vortex"
// or "mo2". With both managers set up for this game, Vortex wins when it has
// files deployed, because MO2 leaves nothing in Data. False with *error when
// nothing is found or two MO2 instances can't be told apart; result->all still
// lists what was found.
bool FindStaging(const std::wstring& data, const std::string& manager, const StagingEnv& env, StagingResult* result,
                 std::string* error);

// One ModOrganizer.ini value as Qt stores it: @ByteArray(..), quotes, \\ and \xHH.
std::string QtIniDecode(const std::string& value);

// An MO2 instance's folders, honouring its custom directories and %BASE_DIR%.
struct Mo2Folders {
    std::wstring mods, overwrite, profiles;
};
Mo2Folders ResolveMo2Folders(const std::wstring& instance);

}  // namespace skqe
