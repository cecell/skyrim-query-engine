#include "staging_find.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "pattern_value.h"

namespace skqe {
namespace {

// The manifest's fields come before its file list, which can run to 180 MB.
constexpr size_t kManifestHead = 64 * 1024;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Absolute, with one trailing backslash.
std::wstring FullDir(const std::wstring& d) {
    if (d.empty()) return {};
    std::wstring in = d;
    std::replace(in.begin(), in.end(), L'/', L'\\');
    DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (!n) return in;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
    out.resize(n);
    if (out.empty() || out.back() != L'\\') out.push_back(L'\\');
    return out;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool SameDir(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(FullDir(a).c_str(), -1, FullDir(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool ReadHead(const std::wstring& path, size_t limit, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out->resize(limit);
    DWORD got = 0;
    BOOL ok = ReadFile(f, out->data(), static_cast<DWORD>(limit), &got, nullptr);
    CloseHandle(f);
    out->resize(got);
    return ok != FALSE;
}

using Ini = std::map<std::string, std::map<std::string, std::string>>;

bool ReadIni(const std::wstring& path, Ini* ini) {
    std::string text;
    if (!ReadHead(path, 8 * 1024 * 1024, &text)) return false;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            section = Lower(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos) (*ini)[section][Lower(line.substr(0, eq))] = line.substr(eq + 1);
    }
    return true;
}

std::string IniGet(const Ini& ini, const char* section, const char* key) {
    auto s = ini.find(section);
    if (s == ini.end()) return {};
    auto k = s->second.find(key);
    return k == s->second.end() ? std::string() : QtIniDecode(k->second);
}

std::vector<std::wstring> SubDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((FullDir(dir) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
            out.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

// A top-level JSON string field, from the part of a file that was read.
bool JsonField(const std::string& text, const char* key, std::string* value, size_t* end = nullptr) {
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = text.find(quoted);
    if (at == std::string::npos) return false;
    size_t p = text.find_first_not_of(" \t\r\n", at + quoted.size());
    if (p == std::string::npos || text[p] != ':') return false;
    p = text.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || text[p] != '"') return false;
    size_t q = p + 1;
    while (q < text.size() && text[q] != '"') q += text[q] == '\\' ? 2 : 1;
    if (q >= text.size()) return false;
    Value v;
    std::string err;
    if (!ParseJson(std::string_view(text).substr(p, q - p + 1), &v, &err) || !v.IsString()) return false;
    *value = v.AsString();
    if (end) *end = q + 1;
    return true;
}

// "files":[] is a purged deployment.
bool ManifestHasFiles(const std::string& text) {
    size_t at = text.find("\"files\"");
    if (at == std::string::npos) return false;
    size_t p = text.find('[', at);
    if (p == std::string::npos) return false;
    p = text.find_first_not_of(" \t\r\n", p + 1);
    return p != std::string::npos && text[p] != ']';
}

void AddVortex(const std::wstring& data, const StagingEnv& env, std::vector<StagingCandidate>* out) {
    std::string head, path, game;
    bool manifest = false;
    if (ReadHead(data + L"vortex.deployment.json", kManifestHead, &head) && JsonField(head, "stagingPath", &path) &&
        (!JsonField(head, "gameId", &game) || Lower(game) == "skyrimse")) {
        const std::wstring staging = FullDir(Wide(path));
        if (IsDir(staging)) {
            StagingCandidate c;
            c.manager = "vortex";
            c.staging = staging;
            c.source = "Vortex's deployment manifest in Data";
            c.deployed = ManifestHasFiles(head);
            c.matchesData = true;
            out->push_back(std::move(c));
            manifest = true;
        }
    }
    if (manifest) return;
    // Nothing deployed yet: Vortex's default locations, per user and shared. The
    // marker alone doesn't count, because Vortex can leave it in an unused folder.
    for (const std::wstring& base : {env.appData + L"\\Vortex\\", env.programData + L"\\vortex\\"}) {
        const std::wstring dir = FullDir(base + L"skyrimse\\mods");
        if (IsFile(dir + L"__vortex_staging_folder") && !SubDirs(dir).empty()) {
            StagingCandidate c;
            c.manager = "vortex";
            c.staging = dir;
            c.source = "Vortex's default staging folder (nothing deployed to this Data)";
            c.matchesData = true;
            out->push_back(std::move(c));
        }
    }
}

void AddMo2Instance(const std::wstring& dir, const std::string& name, const std::wstring& data, const char* how,
                    std::vector<StagingCandidate>* out) {
    const std::wstring inst = FullDir(dir);
    Ini ini;
    if (!ReadIni(inst + L"ModOrganizer.ini", &ini)) return;
    if (Lower(IniGet(ini, "general", "gamename")) != "skyrim special edition") return;
    for (const auto& seen : *out)
        if (seen.manager == "mo2" && SameDir(seen.instance, inst)) return;
    const Mo2Folders f = ResolveMo2Folders(inst);
    if (!IsDir(f.mods)) return;
    StagingCandidate c;
    c.manager = "mo2";
    c.staging = f.mods;
    c.instance = inst;
    c.instanceName = name;
    c.source = how;
    const std::string game = IniGet(ini, "general", "gamepath");
    c.matchesData = !game.empty() && SameDir(FullDir(Wide(game)) + L"Data", data);
    std::string profile = IniGet(ini, "general", "selected_profile");
    if (profile.empty() || !IsDir(f.profiles + Wide(profile))) {
        const auto all = SubDirs(f.profiles);
        profile = all.size() == 1 ? Utf8(all[0]) : IsDir(f.profiles + L"Default") ? "Default" : "";
    }
    c.profile = profile;
    out->push_back(std::move(c));
}

void AddMo2(const std::wstring& data, const StagingEnv& env, std::vector<StagingCandidate>* out) {
    const std::wstring global = env.localAppData + L"\\ModOrganizer\\";
    for (const auto& name : SubDirs(global))
        AddMo2Instance(global + name, Utf8(name), data, "MO2's instance list in AppData", out);
    // Portable instances have no list of their own, but MO2 registers its
    // executable as a download handler.
    Ini handlers;
    if (ReadIni(global + L"nxmhandler.ini", &handlers)) {
        auto h = handlers.find("handlers");
        if (h != handlers.end()) {
            for (const auto& [key, value] : h->second) {
                if (key.size() < 11 || key.compare(key.size() - 11, 11, "\\executable")) continue;
                std::wstring exe = Wide(QtIniDecode(value));
                std::replace(exe.begin(), exe.end(), L'/', L'\\');
                size_t slash = exe.find_last_of(L'\\');
                if (slash == std::wstring::npos) continue;
                if (Lower(Utf8(exe.substr(slash + 1))) != "modorganizer.exe") continue;
                AddMo2Instance(exe.substr(0, slash), "", data, "a portable MO2 named in nxmhandler.ini", out);
            }
        }
    }
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

std::string Describe(const StagingCandidate& c) {
    if (c.manager == "vortex") return "vortex  " + Utf8(c.staging);
    return "mo2  " + Utf8(c.instance) + (c.matchesData ? "" : " (a different game folder)");
}

}  // namespace

std::string QtIniDecode(const std::string& raw) {
    std::string v = raw;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v.rfind("@ByteArray(", 0) == 0 && v.back() == ')') v = v.substr(11, v.size() - 12);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 >= v.size()) {
            out.push_back(v[i]);
            continue;
        }
        const char c = v[++i];
        if (c == 'x') {
            int n = 0, digits = 0;
            while (digits < 2 && i + 1 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1]))) {
                const char d = v[++i];
                n = n * 16 + (d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);
                ++digits;
            }
            out.push_back(static_cast<char>(n));
        } else {
            out.push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c);
        }
    }
    return out;
}

Mo2Folders ResolveMo2Folders(const std::wstring& instance) {
    const std::wstring inst = FullDir(instance);
    Ini ini;
    ReadIni(inst + L"ModOrganizer.ini", &ini);
    std::string base = IniGet(ini, "settings", "base_directory");
    if (base.empty()) base = Utf8(inst.substr(0, inst.size() - 1));
    auto dir = [&](const char* key, const wchar_t* fallback) {
        std::string v = IniGet(ini, "settings", key);
        if (v.empty()) return FullDir(Wide(base) + L"\\" + fallback);
        size_t k = v.find("%BASE_DIR%");
        if (k != std::string::npos) v.replace(k, 10, base);
        return FullDir(Wide(v));
    };
    return {dir("mod_directory", L"mods"), dir("overwrite_directory", L"overwrite"),
            dir("profiles_directory", L"profiles")};
}

StagingEnv CurrentStagingEnv() {
    StagingEnv e;
    e.appData = EnvVar(L"APPDATA");
    e.localAppData = EnvVar(L"LOCALAPPDATA");
    e.programData = EnvVar(L"PROGRAMDATA");
    if (!e.localAppData.empty()) e.settingsFile = e.localAppData + L"\\skqe\\settings.json";
    e.mo2CurrentInstance = Utf8(RegString(HKEY_CURRENT_USER, L"Software\\Mod Organizer Team\\Mod Organizer", L"CurrentInstance"));
    return e;
}

StagingSettings LoadStagingSettings(const StagingEnv& env) {
    StagingSettings s;
    std::string text;
    Value v;
    std::string err;
    if (env.settingsFile.empty() || !ReadHead(env.settingsFile, 1024 * 1024, &text) || !ParseJson(text, &v, &err)) return s;
    if (const Value* d = v.Find("data"); d && d->IsString()) s.data = Wide(d->AsString());
    if (const Value* d = v.Find("staging"); d && d->IsString()) s.staging = Wide(d->AsString());
    return s;
}

bool SaveStagingSettings(const StagingEnv& env, const StagingSettings& settings, std::string* error) {
    if (env.settingsFile.empty()) {
        *error = "nowhere to save settings: LOCALAPPDATA isn't set";
        return false;
    }
    Value v = Value::Map();
    if (!settings.data.empty()) v.Set("data", Value::Str(Utf8(settings.data)));
    if (!settings.staging.empty()) v.Set("staging", Value::Str(Utf8(settings.staging)));
    std::string text;
    WriteJson(v, &text);
    text += "\n";
    size_t slash = env.settingsFile.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(env.settingsFile.substr(0, slash).c_str(), nullptr);
    const std::wstring temp = env.settingsFile + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        f << text;
        if (!f.good()) {
            *error = "cannot write " + Utf8(temp);
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), env.settingsFile.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = "cannot write " + Utf8(env.settingsFile);
        return false;
    }
    return true;
}

bool ResolveDataFolder(const std::wstring& dataIn, const StagingEnv& env, std::wstring* out, std::string* error) {
    std::wstring data = dataIn;
    if (data.empty()) data = LoadStagingSettings(env).data;
    if (data.empty()) {
        const std::wstring game = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition",
                                            L"installed path");
        if (game.empty()) {
            *error = "the game isn't in the registry: give its folder with --data, or save it with skqe staging --set-data <folder>";
            return false;
        }
        data = FullDir(game) + L"Data";
    }
    data = FullDir(data);
    if (!IsDir(data)) {
        *error = "no Data folder at " + Utf8(data);
        return false;
    }
    *out = data;
    return true;
}

bool FindStaging(const std::wstring& dataIn, const std::string& manager, const StagingEnv& env, StagingResult* result,
                 std::string* error) {
    std::wstring data;
    if (!ResolveDataFolder(dataIn, env, &data, error)) return false;
    result->data = data;
    auto& all = result->all;
    all.clear();
    const StagingSettings saved = LoadStagingSettings(env);
    if (manager.empty() && !saved.staging.empty()) {
        const std::wstring dir = FullDir(saved.staging);
        if (!IsDir(dir)) {
            *error = "your saved staging folder doesn't exist: " + Utf8(dir) + "\nChange it with skqe staging --set-staging <folder>, or remove it with --clear";
            return false;
        }
        StagingCandidate c;
        c.manager = "manual";
        c.staging = dir;
        c.source = "your saved setting";
        c.matchesData = true;
        all.push_back(c);
        result->chosen = std::move(c);
        return true;
    }
    if (manager != "mo2") AddVortex(data, env, &all);
    if (manager != "vortex") AddMo2(data, env, &all);

    const StagingCandidate* deployed = nullptr;
    const StagingCandidate* vortex = nullptr;
    std::vector<const StagingCandidate*> mo2;
    for (const auto& c : all) {
        if (c.manager == "vortex") {
            if (!vortex) vortex = &c;
            if (c.deployed) deployed = &c;
        } else if (c.matchesData) {
            mo2.push_back(&c);
        }
    }
    const StagingCandidate* pick = deployed;
    if (!pick && mo2.size() == 1) pick = mo2[0];
    if (!pick && mo2.size() > 1) {
        for (const auto* c : mo2)
            if (!env.mo2CurrentInstance.empty() && Lower(c->instanceName) == Lower(env.mo2CurrentInstance)) pick = c;
        if (!pick) {
            *error = "more than one MO2 instance manages this game, and none is MO2's current one: give --mo2 <instance>";
            for (const auto* c : mo2) *error += "\n  " + Describe(*c);
            return false;
        }
    }
    if (!pick) pick = vortex;
    if (!pick) {
        *error = "no " + (manager.empty() ? std::string("Vortex or MO2") : manager) + " staging folder found for " + Utf8(data);
        for (const auto& c : all) *error += "\n  found, but not for this Data folder: " + Describe(c);
        *error += "\nGive the folder with --staging <folder> or --mo2 <instance>";
        return false;
    }
    result->chosen = *pick;
    return true;
}

}  // namespace skqe
