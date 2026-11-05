#include "plugin.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "inflate.h"

namespace spf {
namespace {

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Header text and file names: UTF-8 when the bytes already are, otherwise
// Windows-1252 (what the CK writes) converted to UTF-8.
std::string ToUtf8(const std::string& bytes) {
    if (bytes.empty()) return bytes;
    int n = static_cast<int>(bytes.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), n, nullptr, 0) > 0) return bytes;
    int wn = MultiByteToWideChar(1252, 0, bytes.data(), n, nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(1252, 0, bytes.data(), n, w.data(), wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), un, nullptr, nullptr);
    return out;
}

std::string Narrow1252(const std::wstring& w) {
    int n = WideCharToMultiByte(1252, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(1252, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ParseHex8(const std::string& s, size_t start, uint32_t* out) {
    uint32_t v = 0;
    for (size_t i = start; i < start + 8; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return false;
    }
    *out = v;
    return true;
}

}  // namespace

Plugin::~Plugin() {
    if (mapped_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

std::unique_ptr<Plugin> Plugin::Open(const std::wstring& path, const PatternCatalog& catalog, std::string* error,
                                     const std::string& language) {
    std::unique_ptr<Plugin> p(new Plugin());
    p->catalog_ = &catalog;
    size_t slash = path.find_last_of(L"\\/");
    p->dir_ = path.substr(0, slash + 1);
    p->name_ = Narrow1252(path.substr(slash + 1));

    p->file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (p->file_ == INVALID_HANDLE_VALUE) {
        *error = "cannot open the file";
        return nullptr;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(p->file_, &size);
    p->size_ = static_cast<size_t>(size.QuadPart);
    if (p->size_ < 24) {
        *error = "not a plugin";
        return nullptr;
    }
    p->mapping_ = CreateFileMappingW(p->file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (p->mapping_) p->data_ = static_cast<const uint8_t*>(MapViewOfFile(p->mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!p->data_) {
        *error = "cannot read the file";
        return nullptr;
    }
    p->mapped_ = true;
    if (!p->Parse(error)) return nullptr;
    if (p->localized_) {
        std::string base = p->name_.substr(0, p->name_.find_last_of('.'));
        p->strings_.Load(p->dir_, base, language);
    }
    return p;
}

std::unique_ptr<Plugin> Plugin::FromMemory(const uint8_t* bytes, size_t size, const std::string& name,
                                           const PatternCatalog& catalog, std::string* error) {
    std::unique_ptr<Plugin> p(new Plugin());
    p->catalog_ = &catalog;
    p->name_ = name;
    p->owned_.assign(bytes, bytes + size);
    p->data_ = p->owned_.data();
    p->size_ = size;
    if (!p->Parse(error)) return nullptr;
    return p;
}

// Reads the header and indexes every record. Every size comes from the file
// and is checked before it is used.
bool Plugin::Parse(std::string* error) {
    Plugin* p = this;
    if (size_ < 24 || memcmp(data_, "TES4", 4) != 0) {
        *error = "not a plugin";
        return false;
    }
    const uint8_t* d = p->data_;
    p->localized_ = (U32(d + 8) & 0x80) != 0;
    size_t hdrSize = U32(d + 4);
    if (24 + hdrSize > p->size_) {
        *error = "truncated header";
        return false;
    }
    PluginHeader& info = p->info_;
    info.flags = U32(d + 8);
    info.esm = (info.flags & 0x1) != 0;
    info.esl = (info.flags & 0x200) != 0;
    info.localized = p->localized_;
    std::string ext = p->name_.size() >= 4 ? p->name_.substr(p->name_.size() - 4) : "";
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    info.master = info.esm || ext == ".esm" || ext == ".esl";
    info.light = info.esl || ext == ".esl";
    size_t bigSize = 0;  // from XXXX: the real size of the next subrecord (a large ONAM)
    for (size_t q = 24; q + 6 <= 24 + hdrSize;) {
        size_t n = bigSize ? bigSize : U16(d + q + 4);
        bigSize = 0;
        if (q + 6 + n > 24 + hdrSize) break;  // runs past the header
        const char* s = reinterpret_cast<const char*>(d + q + 6);
        if (memcmp(d + q, "XXXX", 4) == 0 && n == 4) {
            bigSize = U32(d + q + 6);
        } else if (memcmp(d + q, "MAST", 4) == 0) {
            p->masters_.emplace_back(s, strnlen(s, n));
            info.masterSizes.push_back(0);
        } else if (memcmp(d + q, "DATA", 4) == 0 && n >= 8 && !info.masterSizes.empty()) {
            memcpy(&info.masterSizes.back(), s, 8);
        } else if (memcmp(d + q, "ONAM", 4) == 0) {
            info.overrideCount = static_cast<uint32_t>(n / 4);
        } else if (memcmp(d + q, "INTV", 4) == 0 && n >= 4) {
            info.intv = U32(d + q + 6);
        } else if (memcmp(d + q, "INCC", 4) == 0 && n >= 4) {
            info.incc = U32(d + q + 6);
        } else if (memcmp(d + q, "HEDR", 4) == 0 && n >= 12) {
            memcpy(&info.version, s, 4);
            info.numRecords = U32(d + q + 10);
            info.nextObjectId = U32(d + q + 14);
        } else if (memcmp(d + q, "CNAM", 4) == 0) {
            info.author = ToUtf8(std::string(s, strnlen(s, n)));
        } else if (memcmp(d + q, "SNAM", 4) == 0) {
            info.description = ToUtf8(std::string(s, strnlen(s, n)));
        }
        q += 6 + n;
    }
    p->keyOwners_ = p->mastersUtf8();
    p->keyOwners_.push_back(ToUtf8(p->name_));
    p->records_.push_back({"TES4", 0, U32(d + 8), U16(d + 20), 0, 0});

    // Every record, with the WRLD / CELL / DIAL it sits in.
    struct Group {
        size_t end;
        uint32_t type, label;
    };
    std::vector<Group> groups;
    for (size_t q = 24 + hdrSize; q + 24 <= p->size_;) {
        while (!groups.empty() && q >= groups.back().end) groups.pop_back();
        const uint8_t* r = d + q;
        if (memcmp(r, "GRUP", 4) == 0) {
            // A group's size counts its own header and must stay inside its
            // parent group and the file; otherwise records would be placed in
            // the wrong WRLD / CELL / DIAL.
            size_t gsize = U32(r + 4);
            size_t limit = groups.empty() ? p->size_ : groups.back().end;
            if (gsize < 24 || q + gsize > limit) {
                *error = "damaged plugin: a group at offset " + std::to_string(q) + " has an impossible size";
                return false;
            }
            groups.push_back({q + gsize, U32(r + 12), U32(r + 8)});
            q += 24;
            continue;
        }
        size_t len = U32(r + 4);
        size_t limit = groups.empty() ? p->size_ : groups.back().end;
        if (q + 24 + len > limit) {
            *error = "damaged plugin: the record at offset " + std::to_string(q) + " runs past its group or the file";
            return false;
        }
        uint32_t parent = 0;
        for (auto g = groups.rbegin(); g != groups.rend(); ++g) {
            if (g->type == 1 || g->type == 6 || g->type == 7) {
                parent = g->label;
                break;
            }
        }
        p->records_.push_back(
            {std::string(reinterpret_cast<const char*>(r), 4), U32(r + 12), U32(r + 8), U16(r + 20), parent, q});
        q += 24 + len;
    }
    for (size_t i = 1; i < p->records_.size(); ++i) p->byFormId_.emplace_back(p->records_[i].formId, i);
    std::stable_sort(p->byFormId_.begin(), p->byFormId_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

const Record* Plugin::Find(uint32_t formId) const {
    auto it = std::lower_bound(byFormId_.begin(), byFormId_.end(), formId,
                               [](const auto& e, uint32_t f) { return e.first < f; });
    return it == byFormId_.end() || it->first != formId ? nullptr : &records_[it->second];
}

std::vector<const Record*> Plugin::OfType(const std::string& type) const {
    std::vector<const Record*> out;
    for (const auto& r : records_)
        if (r.type == type) out.push_back(&r);
    return out;
}

std::vector<std::string> Plugin::mastersUtf8() const {
    std::vector<std::string> out;
    for (const auto& m : masters_) out.push_back(ToUtf8(m));
    return out;
}

std::string Plugin::FormKey(uint32_t formId) const {
    size_t hb = formId >> 24;
    const std::string& owner = hb < masters_.size() ? keyOwners_[hb] : keyOwners_.back();
    char buf[16];
    snprintf(buf, sizeof(buf), ":%06X", formId & 0xFFFFFF);
    return owner + buf;
}

void Plugin::Resolve(const std::string& rt, const std::string& fs, const FormIdMap* formIds, Value* v,
                     std::string* path) const {
    switch (v->kind()) {
        case Value::Kind::String: {
            const std::string& s = v->AsString();
            uint32_t n;
            if (s.size() == 10 && s[0] == '[' && s[9] == ']' && ParseHex8(s, 1, &n)) {
                // [00000000] = no form
                *v = n == 0 ? Value() : formIds ? (*formIds)(n, *path) : Value::Str(FormKey(n));
            } else if (s.size() == 20 && s.rfind("[LSTRING:0x", 0) == 0 && ParseHex8(s, 11, &n)) {
                if (n == 0) *v = Value::Str("");
                else if (const std::string* text = strings_.Find(n, catalog_->LstringClass(rt, fs))) *v = Value::Str(*text);
            }
            break;
        }
        case Value::Kind::List:  // list positions are not part of the path
            for (auto& it : v->Items()) Resolve(rt, fs, formIds, &it, path);
            break;
        case Value::Kind::Map: {
            Value out = Value::Map();
            const size_t len = path->size();
            for (const auto& m : v->Members()) {
                Value child = m.second;
                if (formIds) path->append(".").append(m.first);
                Resolve(rt, fs, formIds, &child, path);
                path->resize(len);
                out.Set(m.first, std::move(child));
            }
            *v = std::move(out);
            break;
        }
        default:
            break;
    }
}

bool Plugin::Body(const Record& r, std::vector<uint8_t>* scratch, const uint8_t** body, size_t* size) const {
    const uint8_t* rec = data_ + r.offset;
    *size = U32(rec + 4);
    *body = rec + 24;
    if (r.flags & 0x00040000) {
        if (*size < 4 || !ZlibInflate(*body + 4, *size - 4, U32(*body), scratch)) return false;
        *body = scratch->data();
        *size = scratch->size();
    }
    return true;
}

std::vector<Field> Plugin::Fields(const Record& r, const FormIdMap* formIds, const SigFilter* wanted) const {
    std::vector<Field> out;
    std::vector<uint8_t> inflated;
    const uint8_t* body;
    size_t size;
    if (!Body(r, &inflated, &body, &size)) return out;
    DecodeContext ctx;
    ctx.rt = r.type;
    ctx.formVersion = r.formVersion;
    ctx.localized = localized_;
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
        if (fs == "XXXX" && len >= 4) {  // the next subrecord is longer than 65535 bytes
            big = U32(data);
            haveBig = true;
            continue;
        }
        if (r.type == "PERK") {
            if (fs == "PRKE") lastEpft = 0;
            else if (fs == "EPFT" && len) lastEpft = data[0];
            ctx.epft = lastEpft;
        }
        if (wanted && !(*wanted)(fs)) continue;
        Field f;
        f.sig = fs;
        if (const Pattern* pat = catalog_->Find(r.type, fs)) {
            ctx.fsig = fs;
            try {
                f.value = DecodeSubrecord(*pat, data, len, ctx);
                std::string path = fs;
                Resolve(r.type, fs, formIds, &f.value, &path);
                f.decoded = true;
            } catch (const std::exception&) {  // bytes that do not fit the pattern (or a failed allocation)
            }
        }
        if (!f.decoded) {
            static const char kHex[] = "0123456789ABCDEF";
            std::string hex = "#";
            for (size_t i = 0; i < len; ++i) {
                hex.push_back(kHex[data[i] >> 4]);
                hex.push_back(kHex[data[i] & 15]);
            }
            f.value = Value::Str(hex);
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<Value> Select(const Value& v, const std::string& path) {
    std::vector<Value> cur{v};
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        std::vector<Value> next;
        if (path[i] == '[') {  // [n]: one element of a list
            size_t close = path.find(']', i);
            if (close == std::string::npos) return {};
            size_t idx = static_cast<size_t>(atoi(path.c_str() + i + 1));
            for (const auto& c : cur)
                if (c.kind() == Value::Kind::List && idx < c.Items().size()) next.push_back(c.Items()[idx]);
            i = close + 1;
        } else {  // a name: a map member, applied to each element of a list
            size_t end = path.find_first_of(".[", i);
            std::string name = path.substr(i, end == std::string::npos ? std::string::npos : end - i);
            i = end == std::string::npos ? path.size() : end;
            std::vector<const Value*> stack;
            for (const auto& c : cur) stack.push_back(&c);
            while (!stack.empty()) {
                const Value* c = stack.back();
                stack.pop_back();
                if (c->kind() == Value::Kind::Map) {
                    if (const Value* m = c->Find(name)) next.push_back(*m);
                } else if (c->kind() == Value::Kind::List) {
                    for (auto it = c->Items().rbegin(); it != c->Items().rend(); ++it) stack.push_back(&*it);
                }
            }
        }
        cur = std::move(next);
    }
    return cur;
}

}  // namespace spf
