#pragma once

// Minimal JSON value with a strict parser and deterministic serializer.
// Objects keep insertion order so that manifests are stable byte-for-byte.

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mapgen {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(uint64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(unsigned v) : type_(Type::Number), num_(v) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : def; }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return type_ == Type::String ? str_ : def; }

    // Array access.
    size_t size() const;
    const Json& operator[](size_t i) const;
    Json& operator[](size_t i);
    const Json& operator[](int i) const { return (*this)[static_cast<size_t>(i)]; }
    Json& operator[](int i) { return (*this)[static_cast<size_t>(i)]; }
    void push(Json v);
    const Array& items() const;

    // Object access.
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;  // returns null if absent
    Json& operator[](const std::string& key);              // inserts if absent
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    const Object& members() const;
    void set(const std::string& key, Json v) { (*this)[key] = std::move(v); }

    std::string dump(int indent = 2) const;
    static Json parse(const std::string& text);  // throws std::runtime_error

private:
    void dumpTo(std::string& out, int indent, int level) const;
    void ensureMutable();

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

std::string formatNumber(double v);

}  // namespace mapgen
