#include "core/util/Json.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace mapgen {

namespace {
const Json kNull;
const std::string kEmpty;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
}  // namespace

std::string formatNumber(double v) {
    if (!std::isfinite(v)) return "null";
    if (v == std::floor(v) && std::fabs(v) < 9.007199254740992e15) {
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(v));
        return std::string(buf, r.ptr);
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);  // shortest round-trip representation
    return std::string(buf, r.ptr);
}

const std::string& Json::asString() const { return type_ == Type::String ? str_ : kEmpty; }

void Json::ensureMutable() {
    if (type_ == Type::Array && arr_ && arr_.use_count() > 1) arr_ = std::make_shared<Array>(*arr_);
    if (type_ == Type::Object && obj_ && obj_.use_count() > 1) obj_ = std::make_shared<Object>(*obj_);
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_->size();
    if (type_ == Type::Object) return obj_->size();
    return 0;
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_->size()) return kNull;
    return (*arr_)[i];
}

Json& Json::operator[](size_t i) {
    if (type_ != Type::Array) throw std::runtime_error("JSON: not an array");
    ensureMutable();
    return arr_->at(i);
}

void Json::push(Json v) {
    if (type_ == Type::Null) { type_ = Type::Array; arr_ = std::make_shared<Array>(); }
    if (type_ != Type::Array) throw std::runtime_error("JSON: push to non-array");
    ensureMutable();
    arr_->push_back(std::move(v));
}

const Json::Array& Json::items() const { return type_ == Type::Array ? *arr_ : kEmptyArray; }
const Json::Object& Json::members() const { return type_ == Type::Object ? *obj_ : kEmptyObject; }

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (auto& kv : *obj_) if (kv.first == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    for (auto& kv : *obj_) if (kv.first == key) return kv.second;
    return kNull;
}

Json& Json::operator[](const std::string& key) {
    if (type_ == Type::Null) { type_ = Type::Object; obj_ = std::make_shared<Object>(); }
    if (type_ != Type::Object) throw std::runtime_error("JSON: not an object");
    ensureMutable();
    for (auto& kv : *obj_) if (kv.first == key) return kv.second;
    obj_->emplace_back(key, Json());
    return obj_->back().second;
}

static void escapeString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));  // UTF-8 passes through
                }
        }
    }
    out.push_back('"');
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto nl = [&](int lvl) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: out += formatNumber(num_); break;
        case Type::String: escapeString(out, str_); break;
        case Type::Array: {
            if (arr_->empty()) { out += "[]"; break; }
            out.push_back('[');
            bool first = true;
            for (auto& v : *arr_) {
                if (!first) out.push_back(',');
                first = false;
                nl(level + 1);
                v.dumpTo(out, indent, level + 1);
            }
            nl(level);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (obj_->empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (auto& kv : *obj_) {
                if (!first) out.push_back(',');
                first = false;
                nl(level + 1);
                escapeString(out, kv.first);
                out += indent > 0 ? ": " : ":";
                kv.second.dumpTo(out, indent, level + 1);
            }
            nl(level);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON parse error: ") + what);
    }
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if (static_cast<size_t>(end - p) >= n && std::memcmp(p, s, n) == 0) { p += n; return true; }
        return false;
    }
    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    uint32_t hex4() {
        if (end - p < 4) fail("bad \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }
    std::string str() {
        if (p >= end || *p != '"') fail("expected string");
        ++p;
        std::string s;
        while (true) {
            if (p >= end) fail("unterminated string");
            char c = *p++;
            if (c == '"') break;
            if (c == '\\') {
                if (p >= end) fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            uint32_t lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(s, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                s.push_back(c);
            }
        }
        return s;
    }
    Json value() {
        if (++depth > 256) fail("nesting too deep");
        ws();
        if (p >= end) fail("unexpected end");
        Json result;
        char c = *p;
        if (c == '{') {
            ++p;
            Json o = Json::object();
            ws();
            if (p < end && *p == '}') { ++p; --depth; return o; }
            while (true) {
                ws();
                std::string k = str();
                ws();
                if (p >= end || *p != ':') fail("expected ':'");
                ++p;
                o[k] = value();
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                fail("expected ',' or '}'");
            }
            result = std::move(o);
        } else if (c == '[') {
            ++p;
            Json a = Json::array();
            ws();
            if (p < end && *p == ']') { ++p; --depth; return a; }
            while (true) {
                a.push(value());
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                fail("expected ',' or ']'");
            }
            result = std::move(a);
        } else if (c == '"') {
            result = Json(str());
        } else if (lit("true")) {
            result = Json(true);
        } else if (lit("false")) {
            result = Json(false);
        } else if (lit("null")) {
            result = Json();
        } else {
            double v = 0;
            auto r = std::from_chars(p, end, v);
            if (r.ec != std::errc() || r.ptr == p) fail("bad number");
            p = r.ptr;
            result = Json(v);
        }
        --depth;
        return result;
    }
};

}  // namespace

Json Json::parse(const std::string& text) {
    Parser ps{text.data(), text.data() + text.size()};
    // Skip UTF-8 BOM.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        ps.p += 3;
    Json v = ps.value();
    ps.ws();
    if (ps.p != ps.end) ps.fail("trailing characters");
    return v;
}

}  // namespace mapgen
