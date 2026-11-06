// A plugin as an editable tree over its original bytes.
//
//   Bytes are the truth. A node nobody edited is written by copying its span
//   of the input, so unedited groups and records (compressed ones included)
//   come out byte-identical. An edit replaces the nodes on the path from the
//   root down to the record and leaves every other node shared, which also
//   makes undo a saved root pointer.
//
// The tree needs no pattern catalog. Field-level work is in ops.h.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "edit/record_body.h"

namespace spf::edit {

constexpr size_t kHeaderSize = 24;
constexpr uint32_t kFlagDeleted = 0x20;
constexpr uint32_t kFlagCompressed = 0x40000;

// Group nesting a real plugin never gets near; a hostile file is refused
// rather than recursed into.
constexpr size_t kMaxGroupDepth = 24;

using Bytes = std::vector<uint8_t>;

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
    bool isGroup = false;
    std::array<uint8_t, kHeaderSize> header{};  // size field refreshed when written

    // Unedited: this node's whole extent in the input (header included).
    // Null once it or anything below it has been edited.
    const uint8_t* span = nullptr;
    size_t spanSize = 0;

    // Record body as stored on disk (compressed when the flag says so). A view
    // of the input, or of `owned`.
    const uint8_t* stored = nullptr;
    size_t storedSize = 0;
    std::shared_ptr<const Bytes> owned;
    // An edited body, uncompressed; deflated at save if the flag says so.
    std::shared_ptr<const Bytes> plain;

    std::vector<NodePtr> children;  // groups and the root

    Sig sig() const { return {char(header[0]), char(header[1]), char(header[2]), char(header[3])}; }
    uint32_t U32At(size_t off) const {
        return uint32_t(header[off]) | uint32_t(header[off + 1]) << 8 | uint32_t(header[off + 2]) << 16 |
               uint32_t(header[off + 3]) << 24;
    }
    uint16_t U16At(size_t off) const { return static_cast<uint16_t>(header[off] | header[off + 1] << 8); }
    uint32_t flags() const { return U32At(8); }
    uint32_t formId() const { return U32At(12); }     // records
    uint32_t groupLabel() const { return U32At(8); }  // groups
    uint32_t groupType() const { return U32At(12); }  // groups
    uint16_t formVersion() const { return U16At(20); }
    bool compressed() const { return (flags() & kFlagCompressed) != 0; }
};

// Child indexes from the root to a node.
using Path = std::vector<uint32_t>;

void SetU32(Node* n, size_t off, uint32_t v);

struct WriteOptions {
    // Round-trip tests. Ignore the unedited spans: recompute every group size
    // and write each header from the parsed values.
    bool rebuildGroups = false;
    // Also split every record into subrecords and emit them again, deflating
    // nothing: a compressed record's stored bytes are kept once its inflated
    // body has been shown to re-emit exactly.
    bool rebuildRecords = false;
};

class Document {
public:
    // Maps the file. Refuses damaged structure with a reason.
    static std::unique_ptr<Document> Open(const std::wstring& path, std::string* error);
    // Copies the bytes.
    static std::unique_ptr<Document> FromMemory(const uint8_t* bytes, size_t size, std::string name,
                                                std::string* error);
    ~Document();

    const std::string& name() const { return name_; }  // file name, UTF-8
    const std::wstring& path() const { return path_; }  // empty for FromMemory
    const uint8_t* input() const { return data_; }
    size_t inputSize() const { return size_; }

    const Node& root() const { return *root_; }
    const NodePtr& rootPtr() const { return root_; }
    const NodePtr& tes4() const { return root_->children.front(); }
    const uint8_t* trailing() const { return trailing_; }  // bytes after the last node, too few for a header
    size_t trailingSize() const { return trailingSize_; }

    // Replaces the root (undo: keep the previous pointer; nodes are immutable).
    void SetRoot(NodePtr r) {
        root_ = std::move(r);
        InvalidateIndex();
    }
    // The tree's shape is unchanged: paths stay valid.
    void SetRootKeepIndex(NodePtr r) { root_ = std::move(r); }

    // A record's uncompressed body, inflating into `scratch` when needed.
    // False when a compressed body will not inflate.
    static bool PlainBody(const Node& record, Bytes* scratch, const uint8_t** body, size_t* size);

    // Whole file. False only if deflate fails.
    bool Serialize(Bytes* out, const WriteOptions& opt = {}, std::string* error = nullptr) const;

    // Node at a path, and the nodes on the way (root first, the node last).
    const Node* At(const Path& p, std::vector<const Node*>* chain = nullptr) const;

    // A copy of the root with the node at `path` replaced, or removed when
    // `node` is null. Ancestors become edited; all other nodes stay shared.
    NodePtr Replaced(const Path& path, NodePtr node) const;
    // A copy of the root with `node` inserted as child `index` of the group at `parent`.
    NodePtr Inserted(const Path& parent, size_t index, NodePtr node) const;

    // Paths of the records with a stored FormID. Usually one; a file can hold
    // duplicates. The TES4 header is not listed.
    const std::vector<Path>* Find(uint32_t formId) const;
    const std::unordered_map<uint32_t, std::vector<Path>>& Index() const;
    void InvalidateIndex() {
        indexValid_ = false;
        index_.clear();
    }
    // Number of groups and records below the root, not counting TES4: what the
    // HEDR count holds.
    uint32_t CountForHedr() const;

private:
    Document() = default;
    bool Build(std::string* error);

    std::string name_;
    std::wstring path_;
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    Bytes owned_;
    const uint8_t* trailing_ = nullptr;
    size_t trailingSize_ = 0;
    NodePtr root_;
    mutable bool indexValid_ = false;
    mutable std::unordered_map<uint32_t, std::vector<Path>> index_;
};

}  // namespace spf::edit
