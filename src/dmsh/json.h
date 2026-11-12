// JSON as Python's json module reads and writes it (internal to nxmac::dmsh): the shader build's reflection files are
// json.dump(value, ensure_ascii=False, indent=1) of dicts built in a fixed order, and nxnative's host and tools read
// them, so the port writes the same bytes. An object keeps its members in the order they were put, as a dict does.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::dmsh {

class Json {
public:
    enum class Kind { kNull, kBool, kInt, kFloat, kString, kArray, kObject };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;  // null (Python's None)
    Json(std::nullptr_t) {}                                      // NOLINT(google-explicit-constructor)
    Json(bool v) : kind_(Kind::kBool), int_(v) {}                // NOLINT(google-explicit-constructor)
    Json(int v) : kind_(Kind::kInt), int_(v) {}                  // NOLINT(google-explicit-constructor)
    Json(unsigned v) : kind_(Kind::kInt), int_(v) {}             // NOLINT(google-explicit-constructor)
    Json(long v) : kind_(Kind::kInt), int_(v) {}                 // NOLINT(google-explicit-constructor)
    Json(long long v) : kind_(Kind::kInt), int_(v) {}            // NOLINT(google-explicit-constructor)
    Json(unsigned long v);                                       // NOLINT(google-explicit-constructor)
    Json(unsigned long long v);                                  // NOLINT(google-explicit-constructor)
    Json(double v) : kind_(Kind::kFloat), float_(v) {}           // NOLINT(google-explicit-constructor)
    Json(std::string v) : kind_(Kind::kString), string_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
    Json(std::string_view v) : kind_(Kind::kString), string_(v) {}        // NOLINT(google-explicit-constructor)
    Json(const char* v) : kind_(Kind::kString), string_(v) {}             // NOLINT(google-explicit-constructor)
    Json(Array v) : kind_(Kind::kArray), array_(std::move(v)) {}          // NOLINT(google-explicit-constructor)
    Json(Object v) : kind_(Kind::kObject), object_(std::move(v)) {}       // NOLINT(google-explicit-constructor)

    static Json MakeArray() { return Json(Array{}); }
    static Json MakeObject() { return Json(Object{}); }

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::kNull; }
    bool is_bool() const { return kind_ == Kind::kBool; }
    bool is_int() const { return kind_ == Kind::kInt; }
    bool is_number() const { return kind_ == Kind::kInt || kind_ == Kind::kFloat; }
    bool is_string() const { return kind_ == Kind::kString; }
    bool is_array() const { return kind_ == Kind::kArray; }
    bool is_object() const { return kind_ == Kind::kObject; }

    // The value as its kind; std::logic_error when it is another (a mistake in the port, or a file of another shape)
    bool as_bool() const;
    long long as_int() const;
    double as_float() const;  // an int too
    const std::string& as_string() const;
    const Array& array() const;
    Array& array();
    const Object& object() const;
    Object& object();

    // Python's truth: null, false, 0, "", [] and {} are false
    bool truthy() const;

    // dict operations
    const Json* find(std::string_view key) const;  // d.get(key): nullptr when absent
    Json* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    const Json& at(std::string_view key) const;      // d[key]: std::out_of_range (KeyError) when absent
    Json& operator[](std::string_view key);          // d[key] = ...: a new key goes last, an old one keeps its place
    void set(std::string_view key, Json value) { (*this)[key] = std::move(value); }
    bool erase(std::string_view key);                // d.pop(key, None)
    void update(const Json& other);                  // d.update(other)
    // list operations
    void push_back(Json value);
    std::size_t size() const;  // len() of a list, dict or string

    friend bool operator==(const Json& a, const Json& b);

private:
    Kind kind_ = Kind::kNull;
    long long int_ = 0;
    double float_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

// json.dumps(value, ensure_ascii=False, indent=1): what json.dump(..., indent=1) writes into a reflection file
std::string JsonIndent1(const Json& value);
// json.dumps(value): one line, ", " and ": " between things, everything but ASCII escaped
std::string JsonLine(const Json& value);
// json.dumps(value, sort_keys=True, separators=(',', ':'))
std::string JsonSortedCompact(const Json& value);
// json.loads; InvalidArgument for text that is not JSON
absl::StatusOr<Json> JsonParse(std::string_view text);

// repr(float) / str(float)
std::string PyFloatRepr(double v);

}  // namespace nxmac::dmsh
