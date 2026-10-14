#pragma once
// Minimal JSON value, parser and writer (UTF-8). Enough for the launcher <-> UI bridge and its settings file.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int n) : type_(Type::Number), n_(n) {}
    Value(unsigned n) : type_(Type::Number), n_(n) {}
    Value(double n) : type_(Type::Number), n_(n) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
    static Value Array() { Value v; v.type_ = Type::Array; return v; }
    static Value Object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }

    bool asBool(bool d = false) const { return type_ == Type::Bool ? b_ : d; }
    double asNumber(double d = 0) const { return type_ == Type::Number ? n_ : d; }
    int asInt(int d = 0) const { return type_ == Type::Number ? static_cast<int>(n_) : d; }
    const std::string& asString() const { static const std::string empty; return type_ == Type::String ? s_ : empty; }
    std::string str(const char* key, const std::string& d = "") const { auto& v = (*this)[key]; return v.isString() ? v.s_ : d; }

    // Object access; operator[] const returns a null value for missing keys.
    const Value& operator[](const std::string& key) const {
        static const Value none;
        if (type_ != Type::Object) return none;
        auto it = o_.find(key); return it == o_.end() ? none : it->second;
    }
    Value& set(const std::string& key, Value v) { if (type_ != Type::Object) { *this = Object(); } o_[key] = std::move(v); return *this; }
    void erase(const std::string& key) { o_.erase(key); }
    const std::map<std::string, Value>& items() const { return o_; }

    // Array access
    size_t size() const { return type_ == Type::Array ? a_.size() : type_ == Type::Object ? o_.size() : 0; }
    const Value& operator[](size_t i) const { static const Value none; return type_ == Type::Array && i < a_.size() ? a_[i] : none; }
    Value& push(Value v) { if (type_ != Type::Array) { *this = Array(); } a_.push_back(std::move(v)); return *this; }
    const std::vector<Value>& elements() const { return a_; }
    std::vector<Value>& elements() { return a_; }

    std::string dump() const { std::string out; Write(out); return out; }

    static bool Parse(const std::string& text, Value& out) {
        size_t i = 0;
        if (!ParseValue(text, i, out, 0)) return false;
        Skip(text, i); return i == text.size();
    }

private:
    static void Escape(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += static_cast<char>(c);
            }
        }
        out += '"';
    }
    void Write(std::string& out) const {
        switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: {
            char b[32];
            if (n_ == static_cast<double>(static_cast<long long>(n_)) && n_ < 1e15 && n_ > -1e15) std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(n_));
            else std::snprintf(b, sizeof(b), "%.9g", n_);
            out += b; break;
        }
        case Type::String: Escape(out, s_); break;
        case Type::Array: {
            out += '[';
            for (size_t i = 0; i < a_.size(); ++i) { if (i) out += ','; a_[i].Write(out); }
            out += ']'; break;
        }
        case Type::Object: {
            out += '{'; bool first = true;
            for (auto& [k, v] : o_) { if (!first) out += ','; first = false; Escape(out, k); out += ':'; v.Write(out); }
            out += '}'; break;
        }
        }
    }
    static void Skip(const std::string& t, size_t& i) { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
    static void Utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    static bool Hex4(const std::string& t, size_t i, uint32_t& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (size_t k = i; k < i + 4; ++k) {
            const char c = t[k]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
        }
        return true;
    }
    static bool ParseString(const std::string& t, size_t& i, std::string& out) {
        if (t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            const char e = t[i++];
            switch (e) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp; if (!Hex4(t, i, cp)) return false; i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    uint32_t lo; if (Hex4(t, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                Utf8(out, cp); break;
            }
            default: return false;
            }
        }
        return false;
    }
    static bool ParseValue(const std::string& t, size_t& i, Value& out, int depth) {
        if (depth > 64) return false;
        Skip(t, i);
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '{') {
            out = Object(); ++i; Skip(t, i);
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            for (;;) {
                Skip(t, i); std::string key;
                if (i >= t.size() || !ParseString(t, i, key)) return false;
                Skip(t, i); if (i >= t.size() || t[i] != ':') return false; ++i;
                Value v; if (!ParseValue(t, i, v, depth + 1)) return false;
                out.o_[key] = std::move(v);
                Skip(t, i); if (i >= t.size()) return false;
                if (t[i] == ',') { ++i; continue; }
                if (t[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            out = Array(); ++i; Skip(t, i);
            if (i < t.size() && t[i] == ']') { ++i; return true; }
            for (;;) {
                Value v; if (!ParseValue(t, i, v, depth + 1)) return false;
                out.a_.push_back(std::move(v));
                Skip(t, i); if (i >= t.size()) return false;
                if (t[i] == ',') { ++i; continue; }
                if (t[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') { std::string s; if (!ParseString(t, i, s)) return false; out = Value(std::move(s)); return true; }
        if (t.compare(i, 4, "true") == 0) { i += 4; out = Value(true); return true; }
        if (t.compare(i, 5, "false") == 0) { i += 5; out = Value(false); return true; }
        if (t.compare(i, 4, "null") == 0) { i += 4; out = Value(); return true; }
        char* end = nullptr;
        const double n = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return false;
        i = static_cast<size_t>(end - t.c_str()); out = Value(n); return true;
    }

    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::map<std::string, Value> o_;
};

}  // namespace json
