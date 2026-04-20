#include "qppjs/runtime/value.h"

#include <cassert>
#include <utility>

namespace qppjs {

Value Value::undefined() {
    return Value(Undefined {});
}

Value Value::null() {
    return Value(Null {});
}

Value Value::boolean(bool value) {
    return Value(value);
}

Value Value::number(double value) {
    return Value(value);
}

Value Value::string(std::string value) {
    return Value(std::move(value));
}

Value Value::object(ObjectPtr value) {
    return Value(std::move(value));
}

ValueKind Value::kind() const {
    if (std::holds_alternative<Undefined>(storage_)) {
        return ValueKind::Undefined;
    }
    if (std::holds_alternative<Null>(storage_)) {
        return ValueKind::Null;
    }
    if (std::holds_alternative<bool>(storage_)) {
        return ValueKind::Bool;
    }
    if (std::holds_alternative<double>(storage_)) {
        return ValueKind::Number;
    }
    if (std::holds_alternative<std::string>(storage_)) {
        return ValueKind::String;
    }
    return ValueKind::Object;
}

bool Value::is_undefined() const {
    return std::holds_alternative<Undefined>(storage_);
}

bool Value::is_null() const {
    return std::holds_alternative<Null>(storage_);
}

bool Value::is_bool() const {
    return std::holds_alternative<bool>(storage_);
}

bool Value::is_number() const {
    return std::holds_alternative<double>(storage_);
}

bool Value::is_string() const {
    return std::holds_alternative<std::string>(storage_);
}

bool Value::is_object() const {
    return std::holds_alternative<ObjectPtr>(storage_);
}

bool Value::as_bool() const {
    assert(is_bool());
    return std::get<bool>(storage_);
}

double Value::as_number() const {
    assert(is_number());
    return std::get<double>(storage_);
}

const std::string& Value::as_string() const {
    assert(is_string());
    return std::get<std::string>(storage_);
}

const ObjectPtr& Value::as_object() const {
    assert(is_object());
    return std::get<ObjectPtr>(storage_);
}

Value::Value(Storage storage)
    : storage_(std::move(storage)) {}

} // namespace qppjs
