#include "edit/document.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <thread>

#include "inflate.h"

namespace skqe::edit {
namespace {

uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

void SetU32(Node* n, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) n->header[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

std::unique_ptr<Document> Document::Open(const std::wstring& path, std::string* error) {
    std::unique_ptr<Document> d(new Document());
    size_t slash = path.find_last_of(L"\\/");
    d->name_ = Utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
    d->path_ = path;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *error = "cannot open file (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    d->file_ = f;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    d->size_ = static_cast<size_t>(sz.QuadPart);
    if (d->size_ == 0) {
        *error = "empty file";
        return nullptr;
    }
    d->mapping_ = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!d->mapping_) {
        *error = "cannot map file (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    d->data_ = static_cast<const uint8_t*>(MapViewOfFile(d->mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!d->data_) {
        *error = "cannot map file (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    d->mapped_ = true;
    if (!d->Build(error)) return nullptr;
    return d;
}

std::unique_ptr<Document> Document::FromMemory(const uint8_t* bytes, size_t size, std::string name,
                                               std::string* error) {
    std::unique_ptr<Document> d(new Document());
    d->name_ = std::move(name);
    d->owned_.assign(bytes, bytes + size);
    d->data_ = d->owned_.data();
    d->size_ = size;
    if (!d->Build(error)) return nullptr;
    return d;
}

Document::~Document() {
    // Nodes can outlive nothing here: everything viewing the map is ours.
    root_.reset();
    if (mapped_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_) CloseHandle(file_);
}

// The same structural checks as skqe's Plugin::Parse: a group's size counts its
// header and stays inside its parent and the file; a record stays inside its
// group.
bool Document::Build(std::string* error) {
    if (size_ < kHeaderSize || std::memcmp(data_, "TES4", 4) != 0) {
        *error = "not a plugin";
        return false;
    }
    struct Open {
        std::shared_ptr<Node> node;
        size_t end;
    };
    auto makeRecord = [&](size_t q) {
        auto n = std::make_shared<Node>();
        std::memcpy(n->header.data(), data_ + q, kHeaderSize);
        size_t len = U32(data_ + q + 4);
        n->span = data_ + q;
        n->spanSize = kHeaderSize + len;
        n->stored = data_ + q + kHeaderSize;
        n->storedSize = len;
        return n;
    };
    auto root = std::make_shared<Node>();
    root->isGroup = true;
    size_t tes4Len = U32(data_ + 4);
    if (kHeaderSize + tes4Len > size_) {
        *error = "truncated header";
        return false;
    }
    root->children.push_back(makeRecord(0));
    std::vector<Open> open;
    size_t q = kHeaderSize + tes4Len;
    auto closeDone = [&]() {
        while (!open.empty() && q >= open.back().end) open.pop_back();
    };
    while (size_ - q >= kHeaderSize) {
        closeDone();
        Node* parent = open.empty() ? root.get() : open.back().node.get();
        size_t limit = open.empty() ? size_ : open.back().end;
        const uint8_t* r = data_ + q;
        if (std::memcmp(r, "GRUP", 4) == 0) {
            size_t gsize = U32(r + 4);
            if (gsize < kHeaderSize || gsize > limit - q) {
                *error = "damaged plugin: a group at offset " + std::to_string(q) + " has an impossible size";
                return false;
            }
            if (open.size() >= kMaxGroupDepth) {
                *error = "damaged plugin: groups nested too deep at offset " + std::to_string(q);
                return false;
            }
            auto g = std::make_shared<Node>();
            g->isGroup = true;
            std::memcpy(g->header.data(), r, kHeaderSize);
            g->span = r;
            g->spanSize = gsize;
            parent->children.push_back(g);
            open.push_back({g, q + gsize});
            q += kHeaderSize;
            continue;
        }
        size_t len = U32(r + 4);
        if (limit - q < kHeaderSize || len > limit - q - kHeaderSize) {
            *error = "damaged plugin: the record at offset " + std::to_string(q) + " runs past its group or the file";
            return false;
        }
        parent->children.push_back(makeRecord(q));
        q += kHeaderSize + len;
    }
    closeDone();
    if (!open.empty()) {
        *error = "damaged plugin: a group runs past the end of the file";
        return false;
    }
    trailing_ = data_ + q;
    trailingSize_ = size_ - q;
    root_ = root;
    return true;
}

bool Document::PlainBody(const Node& r, Bytes* scratch, const uint8_t** body, size_t* size) {
    if (r.plain) {
        *body = r.plain->data();
        *size = r.plain->size();
        return true;
    }
    if (r.compressed()) {
        if (r.storedSize < 4 || !ZlibInflate(r.stored + 4, r.storedSize - 4, U32(r.stored), scratch)) return false;
        *body = scratch->data();
        *size = scratch->size();
        return true;
    }
    *body = r.stored;
    *size = r.storedSize;
    return true;
}

namespace {

void CollectPlain(const Node& n, std::vector<const Node*>* out) {
    if (n.span) return;  // unedited, and nothing below it is either
    if (n.isGroup) {
        for (const auto& c : n.children) CollectPlain(*c, out);
    } else if (n.plain && n.compressed()) {
        out->push_back(&n);
    }
}

// The body a compressed, edited record gets, or nullptr to store it plain.
struct Deflated {
    Bytes bytes;  // u32 size + zlib stream
    bool smaller = false;
    bool failed = false;
};

void DeflateAll(const std::vector<const Node*>& nodes, std::map<const Node*, Deflated>* out) {
    std::vector<Deflated> results(nodes.size());
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i; (i = next++) < nodes.size();) {
            const Bytes& plain = *nodes[i]->plain;
            Bytes z;
            if (!ZlibDeflate(plain.data(), plain.size(), &z)) {
                results[i].failed = true;
                continue;
            }
            // The Creation Kit's rule: keep it only when it is smaller.
            if (z.size() + 4 >= plain.size()) continue;
            uint32_t n = static_cast<uint32_t>(plain.size());
            results[i].bytes.resize(4 + z.size());
            std::memcpy(results[i].bytes.data(), &n, 4);
            std::memcpy(results[i].bytes.data() + 4, z.data(), z.size());
            results[i].smaller = true;
        }
    };
    unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(),
                                                       static_cast<unsigned>(nodes.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    for (size_t i = 0; i < nodes.size(); ++i) (*out)[nodes[i]] = std::move(results[i]);
}

struct Writer {
    Bytes* out;
    WriteOptions opt;
    const std::map<const Node*, Deflated>* deflated;
    std::string* error;
    bool ok = true;

    void Append(const uint8_t* p, size_t n) {
        if (n) out->insert(out->end(), p, p + n);
    }

    void Header(const Node& n, uint32_t size, uint32_t flags) {
        size_t at = out->size();
        Append(n.header.data(), kHeaderSize);
        std::memcpy(out->data() + at + 4, &size, 4);
        if (!n.isGroup) std::memcpy(out->data() + at + 8, &flags, 4);
    }

    // Splits the record and writes it back. For the round-trip test: a body
    // that re-emits differently is a model bug, so it fails the save.
    void RebuiltRecord(const Node& n) {
        Bytes scratch, emitted;
        const uint8_t* body;
        size_t size;
        if (!Document::PlainBody(n, &scratch, &body, &size)) {
            // Cannot be parsed; the stored bytes pass through (counted by the
            // caller's per-record scan).
            Header(n, static_cast<uint32_t>(n.storedSize), n.flags());
            Append(n.stored, n.storedSize);
            return;
        }
        ParsedBody parsed = ParseBody(body, size);
        size_t usable = parsed.issue == BodyIssue::None ? size : parsed.issueOffset;
        EmitBody(parsed.subs, &emitted);
        if (emitted.size() != usable || (usable && std::memcmp(emitted.data(), body, usable) != 0)) {
            ok = false;
            if (error) {
                char id[16];
                snprintf(id, sizeof(id), "%08X", n.formId());
                *error = "record " + std::string(id) + " does not re-emit its subrecords exactly";
            }
        }
        if (n.compressed()) {
            Header(n, static_cast<uint32_t>(n.storedSize), n.flags());
            Append(n.stored, n.storedSize);
            return;
        }
        emitted.insert(emitted.end(), body + usable, body + size);
        Header(n, static_cast<uint32_t>(emitted.size()), n.flags());
        Append(emitted.data(), emitted.size());
    }

    void Write(const Node& n) {
        if (n.span && !opt.rebuildGroups && !opt.rebuildRecords) {
            Append(n.span, n.spanSize);
            return;
        }
        if (n.isGroup) {
            size_t at = out->size();
            Header(n, 0, 0);
            for (const auto& c : n.children) Write(*c);
            uint32_t size = static_cast<uint32_t>(out->size() - at);
            std::memcpy(out->data() + at + 4, &size, 4);
            return;
        }
        if (n.plain) {
            if (n.compressed()) {
                const Deflated& d = deflated->at(&n);
                if (d.failed) {
                    ok = false;
                    if (error) *error = "deflate failed";
                    return;
                }
                if (d.smaller) {
                    Header(n, static_cast<uint32_t>(d.bytes.size()), n.flags());
                    Append(d.bytes.data(), d.bytes.size());
                    return;
                }
                Header(n, static_cast<uint32_t>(n.plain->size()), n.flags() & ~kFlagCompressed);
            } else {
                Header(n, static_cast<uint32_t>(n.plain->size()), n.flags());
            }
            Append(n.plain->data(), n.plain->size());
            return;
        }
        if (opt.rebuildRecords) {
            RebuiltRecord(n);
            return;
        }
        Header(n, static_cast<uint32_t>(n.storedSize), n.flags());
        Append(n.stored, n.storedSize);
    }
};

}  // namespace

bool Document::Serialize(Bytes* out, const WriteOptions& opt, std::string* error) const {
    std::vector<const Node*> pending;
    for (const auto& c : root_->children) CollectPlain(*c, &pending);
    std::map<const Node*, Deflated> deflated;
    if (!pending.empty()) DeflateAll(pending, &deflated);
    out->clear();
    out->reserve(size_ + 64 * 1024);
    Writer w{out, opt, &deflated, error};
    for (const auto& c : root_->children) w.Write(*c);
    w.Append(trailing_, trailingSize_);
    return w.ok;
}

const Node* Document::At(const Path& p, std::vector<const Node*>* chain) const {
    const Node* n = root_.get();
    if (chain) {
        chain->clear();
        chain->push_back(n);
    }
    for (uint32_t i : p) {
        if (i >= n->children.size()) return nullptr;
        n = n->children[i].get();
        if (chain) chain->push_back(n);
    }
    return n;
}

namespace {

// A copy of a group, marked as edited.
std::shared_ptr<Node> EditedCopy(const Node& n) {
    auto c = std::make_shared<Node>(n);
    c->span = nullptr;
    c->spanSize = 0;
    return c;
}

NodePtr ReplaceRec(const Node& n, const Path& p, size_t depth, const NodePtr& node) {
    auto c = EditedCopy(n);
    uint32_t i = p[depth];
    if (depth + 1 == p.size()) {
        if (node) c->children[i] = node;
        else c->children.erase(c->children.begin() + i);
    } else {
        c->children[i] = ReplaceRec(*n.children[i], p, depth + 1, node);
    }
    return c;
}

NodePtr InsertRec(const Node& n, const Path& p, size_t depth, size_t index, const NodePtr& node) {
    auto c = EditedCopy(n);
    if (depth == p.size()) {
        c->children.insert(c->children.begin() + index, node);
    } else {
        uint32_t i = p[depth];
        c->children[i] = InsertRec(*n.children[i], p, depth + 1, index, node);
    }
    return c;
}

void IndexRec(const Node& n, Path* path, std::unordered_map<uint32_t, std::vector<Path>>* out) {
    for (uint32_t i = 0; i < n.children.size(); ++i) {
        const Node& c = *n.children[i];
        path->push_back(i);
        if (c.isGroup) IndexRec(c, path, out);
        else if (!(path->size() == 1 && i == 0)) (*out)[c.formId()].push_back(*path);
        path->pop_back();
    }
}

uint32_t CountRec(const Node& n) {
    uint32_t count = 0;
    for (const auto& c : n.children) count += 1 + (c->isGroup ? CountRec(*c) : 0);
    return count;
}

}  // namespace

NodePtr Document::Replaced(const Path& path, NodePtr node) const { return ReplaceRec(*root_, path, 0, node); }

NodePtr Document::Inserted(const Path& parent, size_t index, NodePtr node) const {
    return InsertRec(*root_, parent, 0, index, node);
}

const std::unordered_map<uint32_t, std::vector<Path>>& Document::Index() const {
    if (!indexValid_) {
        index_.clear();
        Path p;
        IndexRec(*root_, &p, &index_);
        indexValid_ = true;
    }
    return index_;
}

const std::vector<Path>* Document::Find(uint32_t formId) const {
    const auto& idx = Index();
    auto it = idx.find(formId);
    return it == idx.end() ? nullptr : &it->second;
}

uint32_t Document::CountForHedr() const { return CountRec(*root_) - 1; }

}  // namespace skqe::edit
