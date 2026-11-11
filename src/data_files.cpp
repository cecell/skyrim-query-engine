#include "data_files.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace skqe {

// From the generated embedded_data.cpp (SKQE_EMBED_DATA=ON) or the stub that
// has nothing (OFF).
bool EmbeddedDataFile(const std::wstring& name, const unsigned char** data, size_t* size);

namespace {

bool ReadWhole(const std::wstring& path, std::string* text) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
    text->clear();
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), fp)) > 0;) text->append(buf, n);
    fclose(fp);
    return true;
}

bool IsFile(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The places on disk, in order: SKQE_DATA, then next to the exe.
std::wstring FindOnDisk(const std::wstring& name) {
    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"SKQE_DATA", env, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring dir = env;
        if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        if (IsFile(dir + name)) return dir + name;
    }
    wchar_t exe[MAX_PATH * 2];
    const DWORD e = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    if (e > 0 && e < MAX_PATH * 2) {
        std::wstring p = exe;
        p = p.substr(0, p.find_last_of(L"\\/") + 1) + name;
        if (IsFile(p)) return p;
    }
    return {};
}

}  // namespace

bool ReadDataFile(const std::wstring& name, std::string* text, std::wstring* source) {
    const std::wstring path = FindOnDisk(name);
    if (!path.empty() && ReadWhole(path, text)) {
        if (source) *source = path;
        return true;
    }
    const unsigned char* data = nullptr;
    size_t size = 0;
    if (EmbeddedDataFile(name, &data, &size)) {
        text->assign(reinterpret_cast<const char*>(data), size);
        if (source) *source = L"embedded";
        return true;
    }
    return false;
}

std::wstring DataFileSource(const std::wstring& name) {
    const std::wstring path = FindOnDisk(name);
    if (!path.empty()) return path;
    const unsigned char* data = nullptr;
    size_t size = 0;
    return EmbeddedDataFile(name, &data, &size) ? L"embedded" : L"";
}

bool DataFilesEmbedded() {
    const unsigned char* data = nullptr;
    size_t size = 0;
    return EmbeddedDataFile(L"patterns.json", &data, &size);
}

}  // namespace skqe
