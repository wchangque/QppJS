#pragma once

#include <memory>
#include <string>
#include <variant>

namespace qppjs {

struct Object {
    virtual ~Object() = default;
};

using ObjectPtr = std::shared_ptr<Object>;

struct Undefined final {};
struct Null final {};

enum class ValueKind {
    Undefined,
    Null,
    Bool,
    Number,
    String,
    Object,
};

class Value {
public:
    static Value undefined();
    static Value null();
    static Value boolean(bool value);
    static Value number(double value);
    static Value string(std::string value);
    static Value object(ObjectPtr value);

    [[nodiscard]] ValueKind kind() const;
    [[nodiscard]] bool is_undefined() const;
    [[nodiscard]] bool is_null() const;
    [[nodiscard]] bool is_bool() const;
    [[nodiscard]] bool is_number() const;
    [[nodiscard]] bool is_string() const;
    [[nodiscard]] bool is_object() const;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const ObjectPtr& as_object() const;

private:
    using Storage = std::variant<Undefined, Null, bool, double, std::string, ObjectPtr>;

    explicit Value(Storage storage);

    Storage storage_;
};

}  // namespace qppjs
