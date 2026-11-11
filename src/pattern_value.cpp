#include "pattern_value.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace skqe {

void Value::Set(std::string key, Value v) {
    for (auto& m : map_) {
        if (m.first == key) {
            m.second = std::move(v);
            return;
        }
    }
    map_.emplace_back(std::move(key), std::move(v));
}

const Value* Value::Find(std::string_view key) const {
    for (const auto& m : map_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

bool Value::ToInt(int64_t* out) const {
    switch (kind_) {
        case Kind::Int:
        case Kind::Bool:
            *out = i_;
            return true;
        case Kind::Float:
            if (std::isfinite(f_) && f_ == std::trunc(f_)) {
                *out = static_cast<int64_t>(f_);
                return true;
            }
            return false;
        default:
            return false;
    }
}

std::string Value::LookupKey() const {
    switch (kind_) {
        case Kind::Null: return "None";
        case Kind::Bool: return i_ ? "True" : "False";
        case Kind::Int: return std::to_string(i_);
        case Kind::Float: {
            // Rust's f64 Display: shortest round-trip digits, never an exponent.
            if (std::isnan(f_)) return "NaN";
            if (std::isinf(f_)) return f_ < 0 ? "-inf" : "inf";
            char buf[400];
            auto r = std::to_chars(buf, buf + sizeof(buf), f_, std::chars_format::fixed);
            return std::string(buf, r.ptr);
        }
        case Kind::String: return s_;
        default: {
            std::string s;
            WriteJson(*this, &s);
            return s;
        }
    }
}

// ---- reader -----------------------------------------------------------------

namespace {

struct Reader {
    std::string_view t;
    size_t p = 0;
    std::string err;
    std::string* dup = nullptr;  // when set: the first key that repeats in one object

    void Ws() {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
    }
    bool Fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at offset " + std::to_string(p);
        return false;
    }
    static void PutUtf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t* cp) {
        if (p + 4 > t.size()) return Fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return Fail("bad \\u escape");
        }
        *cp = v;
        return true;
    }
    bool Str(std::string* out) {
        if (p >= t.size() || t[p] != '"') return Fail("expected string");
        ++p;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (p >= t.size()) break;
            char e = t[p++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!Hex4(&lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    PutUtf8(cp, out);
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Num(Value* out) {
        size_t s = p;
        bool isFloat = false;
        if (p < t.size() && t[p] == '-') ++p;
        while (p < t.size()) {
            char c = t[p];
            if (c >= '0' && c <= '9') { ++p; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { isFloat = true; ++p; continue; }
            break;
        }
        std::string_view tok = t.substr(s, p - s);
        if (!isFloat) {
            int64_t i;
            auto r = std::from_chars(tok.data(), tok.data() + tok.size(), i);
            if (r.ec == std::errc() && r.ptr == tok.data() + tok.size()) {
                *out = Value::Int(i);
                return true;
            }
        }
        double d;
        auto r = std::from_chars(tok.data(), tok.data() + tok.size(), d);
        if (r.ec != std::errc() || r.ptr != tok.data() + tok.size()) return Fail("bad number");
        *out = Value::Float(d);
        return true;
    }
    bool Parse(Value* out, int depth) {
        if (depth > 200) return Fail("nesting too deep");
        Ws();
        if (p >= t.size()) return Fail("unexpected end");
        char c = t[p];
        if (c == '{') {
            ++p;
            *out = Value::Map();
            Ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (!Str(&key)) return false;
                Ws();
                if (p >= t.size() || t[p] != ':') return Fail("expected ':'");
                ++p;
                Value v;
                if (!Parse(&v, depth + 1)) return false;
                if (dup && dup->empty() && out->Find(key)) *dup = key.empty() ? std::string("\"\"") : key;
                out->Set(std::move(key), std::move(v));
                Ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            *out = Value::List();
            Ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            for (;;) {
                Value v;
                if (!Parse(&v, depth + 1)) return false;
                out->Items().push_back(std::move(v));
                Ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!Str(&s)) return false;
            *out = Value::Str(std::move(s));
            return true;
        }
        if (t.substr(p, 4) == "true") { p += 4; *out = Value::Bool(true); return true; }
        if (t.substr(p, 5) == "false") { p += 5; *out = Value::Bool(false); return true; }
        if (t.substr(p, 4) == "null") { p += 4; *out = Value(); return true; }
        return Num(out);
    }
};

void WriteString(const std::string& s, std::string* out) {
    static const char kHex[] = "0123456789abcdef";
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\b': *out += "\\b"; break;
            case '\f': *out += "\\f"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (c < 0x20) {
                    *out += "\\u00";
                    out->push_back(kHex[c >> 4]);
                    out->push_back(kHex[c & 15]);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

}  // namespace

bool ParseJson(std::string_view text, Value* out, std::string* error) {
    return ParseJson(text, out, error, nullptr);
}

bool ParseJson(std::string_view text, Value* out, std::string* error, std::string* duplicateKey) {
    Reader r{text};
    r.dup = duplicateKey;
    if (!r.Parse(out, 0)) {
        if (error) *error = r.err;
        return false;
    }
    r.Ws();
    if (r.p != text.size()) {
        if (error) *error = "trailing characters at offset " + std::to_string(r.p);
        return false;
    }
    return true;
}

void WriteJson(const Value& v, std::string* out) {
    switch (v.kind()) {
        case Value::Kind::Null: *out += "null"; break;
        case Value::Kind::Bool: *out += v.AsBool() ? "true" : "false"; break;
        case Value::Kind::Int: *out += std::to_string(v.AsInt()); break;
        case Value::Kind::Float: {
            double f = v.AsFloat();
            if (!std::isfinite(f)) {
                *out += "null";
                break;
            }
            char buf[64];
            auto r = std::to_chars(buf, buf + sizeof(buf), f);
            std::string_view s(buf, r.ptr - buf);
            *out += s;
            // Keep it a JSON float, as serde does ("1.0", not "1").
            if (s.find_first_of(".eEn") == std::string_view::npos) *out += ".0";
            break;
        }
        case Value::Kind::String: WriteString(v.AsString(), out); break;
        case Value::Kind::List: {
            out->push_back('[');
            bool first = true;
            for (const auto& it : v.Items()) {
                if (!first) out->push_back(',');
                first = false;
                WriteJson(it, out);
            }
            out->push_back(']');
            break;
        }
        case Value::Kind::Map: {
            out->push_back('{');
            bool first = true;
            for (const auto& m : v.Members()) {
                if (!first) out->push_back(',');
                first = false;
                WriteString(m.first, out);
                out->push_back(':');
                WriteJson(m.second, out);
            }
            out->push_back('}');
            break;
        }
    }
}

const uint16_t kCp1252High[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

bool IsValidUtf8(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t b = p[i];
        if (b < 0x80) {
            ++i;
            continue;
        }
        size_t len;
        uint8_t lo = 0x80, hi = 0xBF;  // allowed range of the second byte
        if (b >= 0xC2 && b <= 0xDF) len = 2;
        else if (b == 0xE0) { len = 3; lo = 0xA0; }
        else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) len = 3;
        else if (b == 0xED) { len = 3; hi = 0x9F; }
        else if (b == 0xF0) { len = 4; lo = 0x90; }
        else if (b >= 0xF1 && b <= 0xF3) len = 4;
        else if (b == 0xF4) { len = 4; hi = 0x8F; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            uint8_t c = p[i + k];
            if (c < (k == 1 ? lo : 0x80) || c > (k == 1 ? hi : 0xBF)) return false;
        }
        i += len;
    }
    return true;
}

std::string TextToUtf8(const uint8_t* p, size_t n) {
    if (IsValidUtf8(p, n)) return std::string(reinterpret_cast<const char*>(p), n);
    std::string out;
    out.reserve(n + n / 8);
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = p[i] >= 0x80 && p[i] <= 0x9F ? kCp1252High[p[i] - 0x80] : p[i];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

}  // namespace skqe
