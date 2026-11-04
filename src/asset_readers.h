// Header facts of the asset types beth-asset-metadata does not read (Papyrus
// .pex, Havok .hkx, FaceGen .tri), the texture paths a .nif expects, and the
// path conventions build-assets relies on (normalized paths, kinds, voice and
// FaceGen file names, ini archive lists).
//
// Every reader works on bytes the caller holds and checks each size and count
// against them before use: a damaged file is a false return with a reason,
// never a crash or a large allocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spf {
namespace assets {

// ---- Paths ----

// Lower case (invariant), backslashes, no leading separator: the form every
// path takes in assets.db ("Textures/Armor/X.DDS" -> "textures\armor\x.dds").
std::string NormalizePath(const std::string& utf8);

// The texture a mesh names, as the game looks it up: normalized, without a
// leading "data\", under "textures\" (prefixed when missing). "" when empty.
std::string NormalizeTexturePath(const std::string& raw);

struct PathParts {
    std::string dir, name, ext;  // ext without the dot, '' when none
};
PathParts SplitPath(const std::string& path);

// texture mesh audio voice script animation plugin archive strings interface
// facegen bodyslide other.
const char* PathKind(const std::string& path);

// sound\voice\<plugin>\<voicetype>\<prefix>_<formid8>_<n>.<ext>
struct VoicePath {
    std::string plugin, voiceType;
    uint32_t objectId = 0;  // FormID in the file name, low 24 bits
    int response = 0;
};
bool ParseVoicePath(const std::string& path, VoicePath* out);

// meshes\actors\character\facegendata\facegeom\<plugin>\<formid>.nif
// textures\actors\character\facegendata\facetint\<plugin>\<formid>.dds
struct FaceGenPath {
    std::string kind;  // geom | tint
    std::string plugin;
    uint32_t objectId = 0;
};
bool ParseFaceGenPath(const std::string& path, FaceGenPath* out);

// "Skyrim - Misc.bsa, Skyrim - Shaders.bsa" -> the names, trimmed.
std::vector<std::string> SplitArchiveList(const std::string& value);

// ---- Papyrus .pex (Skyrim: big-endian) ----

struct PexInfo {
    std::string scriptName, parentScript;  // parent '' when none
    int major = 0, minor = 0, gameId = 0;
};
constexpr size_t kPexMaxBytes = size_t{16} << 20;
bool ReadPex(const uint8_t* d, size_t n, PexInfo* out, std::string* error);

// ---- Havok .hkx packfile ----

struct HkxInfo {
    std::string havokVersion;  // contents version: "hk_2010.2.0-r1"
    int pointerSize = 0;       // 4 (LE) or 8 (SE)
    bool valid = false;        // a binary packfile the game can load
};
constexpr size_t kHkxProbeBytes = 64;
bool ReadHkx(const uint8_t* d, size_t n, HkxInfo* out, std::string* error);

// ---- .tri: FaceGen (FRTRI003) and BodySlide (PIRT) morphs ----

struct TriInfo {
    std::string version;  // "FRTRI003" | "PIRT"
    bool bodySlide = false;  // PIRT: no geometry counts; morphs named "<shape>:<morph>"
    uint32_t vertexCount = 0, triangleCount = 0, uvCount = 0;
    uint32_t morphCount = 0, modifierCount = 0;
    std::vector<std::string> morphs, modifiers;  // names
};
constexpr size_t kTriMaxBytes = size_t{64} << 20;
// The counts come from the 64-byte header; the names need the whole file.
// False when the header is unreadable; names missing (with `error` set) when
// the body does not fit the counts.
bool ReadTri(const uint8_t* d, size_t n, TriInfo* out, std::string* error);

// ---- NIF texture paths ----

struct NifTexture {
    std::string block;  // BSShaderTextureSet | BSEffectShaderProperty
    int slot = 0;       // position in the texture set (effect shader: 0 source, 1 greyscale)
    std::string path;   // as stored
};
constexpr size_t kNifMaxBytes = size_t{64} << 20;
// Skyrim NIFs (20.2.0.7, little-endian) only; the whole file. Walks the
// header to the block sizes and reads the texture-set blocks.
bool ReadNifTextures(const uint8_t* d, size_t n, std::vector<NifTexture>* out, std::string* error);

}  // namespace assets
}  // namespace spf
