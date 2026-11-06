// A small JSON-shaped value type, a JSON reader and a JSON writer. Used for
// the subrecord pattern definitions (working_patterns.json) and for the
// values the pattern engine decodes from them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skqe {

class Value {
public:
    enum class Kind : uint8_t { Null, Bool, Int, Float, String, List, Map };
    using Member = std::pair<std::string, Value>;

    Value() = default;
    static Value Bool(bool b) { Value v(Kind::Bool); v.i_ = b; return v; }
    static Value Int(int64_t i) { Value v(Kind::Int); v.i_ = i; return v; }
    static Value Float(double f) { Value v(Kind::Float); v.f_ = f; return v; }
    static Value Str(std::string s) { Value v(Kind::String); v.s_ = std::move(s); return v; }
    static Value List(std::vector<Value> items = {}) { Value v(Kind::List); v.list_ = std::move(items); return v; }
    static Value Map() { return Value(Kind::Map); }

    Kind kind() const { return kind_; }
    bool IsNull() const { return kind_ == Kind::Null; }
    bool IsString() const { return kind_ == Kind::String; }
    bool AsBool() const { return i_ != 0; }
    int64_t AsInt() const { return i_; }
    double AsFloat() const { return f_; }
    const std::string& AsString() const { return s_; }
    const std::vector<Value>& Items() const { return list_; }
    std::vector<Value>& Items() { return list_; }
    const std::vector<Member>& Members() const { return map_; }

    // Map insert with IndexMap semantics: a repeated key keeps its first
    // position and takes the new value.
    void Set(std::string key, Value v);
    const Value* Find(std::string_view key) const;

    // Integer view used for counts and lengths: Int, Bool, or a Float with
    // no fractional part. Returns false otherwise.
    bool ToInt(int64_t* out) const;
    // The string a union looks its cases up by (Python str() of the value).
    std::string LookupKey() const;

private:
    explicit Value(Kind k) : kind_(k) {}
    Kind kind_ = Kind::Null;
    int64_t i_ = 0;
    double f_ = 0;
    std::string s_;
    std::vector<Value> list_;
    std::vector<Member> map_;
};

// Parses a complete JSON text. Integers that fit int64 become Int, other
// numbers Float. Returns false (with *error set) on malformed input.
bool ParseJson(std::string_view text, Value* out, std::string* error);

// Appends compact JSON. Non-finite floats are written as null, as
// serde_json does.
void WriteJson(const Value& v, std::string* out);

// Bytes -> UTF-8, replacing invalid sequences with U+FFFD the way Rust's
// String::from_utf8_lossy does (one replacement per maximal invalid prefix).
std::string Utf8Lossy(const uint8_t* p, size_t n);

}  // namespace skqe
