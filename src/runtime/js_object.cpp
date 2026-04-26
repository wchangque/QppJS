#include "qppjs/runtime/js_object.h"

#include "qppjs/runtime/js_function.h"

#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace qppjs {

// Returns true and sets idx if key is a valid canonical array index (uint32, no leading zeros,
// value < 2^32 - 1).
static bool try_parse_array_index(const std::string& key, uint32_t& idx) {
    if (key.empty() || key[0] == '-') return false;
    if (key.size() > 1 && key[0] == '0') return false;
    char* end = nullptr;
    unsigned long long v = strtoull(key.c_str(), &end, 10);
    if (*end != '\0') return false;
    if (v >= 0xFFFFFFFFULL) return false;
    if (std::to_string(v) != key) return false;
    idx = static_cast<uint32_t>(v);
    return true;
}

Value JSObject::get_property(const std::string& key) const {
    if (object_kind() == ObjectKind::kArray) {
        if (key == "length") {
            return Value::number(static_cast<double>(array_length_));
        }
        uint32_t idx = 0;
        if (try_parse_array_index(key, idx)) {
            auto it = elements_.find(idx);
            return it != elements_.end() ? it->second : Value::undefined();
        }
        // Non-index keys fall through to prototype chain lookup below
    }

    const JSObject* cur = this;
    while (cur != nullptr) {
        auto it = cur->index_map_.find(key);
        if (it != cur->index_map_.end()) {
            return cur->properties_[it->second].value;
        }
        if (key == "constructor" && cur->has_constructor_property_) {
            if (cur->constructor_property_ != nullptr) {
                return Value::object(ObjectPtr(cur->constructor_property_));
            }
            return Value::undefined();
        }
        cur = cur->proto_.get();
    }
    return Value::undefined();
}

void JSObject::set_property(const std::string& key, Value value) {
    if (object_kind() == ObjectKind::kArray) {
        uint32_t idx = 0;
        if (try_parse_array_index(key, idx)) {
            elements_[idx] = std::move(value);
            if (idx + 1 > array_length_) array_length_ = idx + 1;
            return;
        }
        // "length" key and non-index keys fall through to properties_ dict
    }

    auto it = index_map_.find(key);
    if (it != index_map_.end()) {
        properties_[it->second].value = std::move(value);
    } else {
        size_t idx = properties_.size();
        properties_.push_back(PropertyEntry{key, std::move(value)});
        index_map_.emplace(key, idx);
    }
}

EvalResult JSObject::set_property_ex(const std::string& key, Value value) {
    if (object_kind() == ObjectKind::kArray && key == "length") {
        if (!value.is_number()) {
            return EvalResult::err(Error{ErrorKind::Runtime, "RangeError: Invalid array length"});
        }
        double d = value.as_number();
        double u32 = std::floor(d);
        if (u32 != d || u32 < 0.0 || u32 > 4294967295.0) {
            return EvalResult::err(Error{ErrorKind::Runtime, "RangeError: Invalid array length"});
        }
        uint32_t new_len = static_cast<uint32_t>(u32);
        // Truncate: remove elements with index >= new_len
        if (new_len < array_length_) {
            for (auto it = elements_.begin(); it != elements_.end();) {
                if (it->first >= new_len) it = elements_.erase(it);
                else ++it;
            }
        }
        array_length_ = new_len;
        return EvalResult::ok(Value::undefined());
    }
    set_property(key, std::move(value));
    return EvalResult::ok(Value::undefined());
}

void JSObject::set_constructor_property(RcObject* value) {
    constructor_property_ = value;
    has_constructor_property_ = true;
}

bool JSObject::has_own_property(const std::string& key) const {
    return key == "constructor" ? has_constructor_property_ || index_map_.contains(key)
                                : index_map_.contains(key);
}

void JSObject::clear_function_properties() {
    std::unordered_set<const JSObject*> visited;
    clear_function_properties(visited);
}

void JSObject::clear_function_properties(std::unordered_set<const JSObject*>& visited) {
    if (visited.contains(this)) {
        return;
    }
    visited.insert(this);

    if (proto_) {
        proto_->clear_function_properties(visited);
    }

    if (constructor_property_ != nullptr) {
        constructor_property_ = nullptr;
    }

    for (auto& property : properties_) {
        if (!property.value.is_object()) {
            continue;
        }
        RcObject* raw = property.value.as_object_raw();
        if (raw == nullptr) {
            continue;
        }
        ObjectPtr keep_alive(raw);
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* function = static_cast<JSFunction*>(raw);
            RcPtr<JSObject> prototype = function->prototype_obj();
            if (prototype) {
                prototype->clear_function_properties(visited);
            }
            property.value = Value::undefined();
            continue;
        }
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->clear_function_properties(visited);
        }
    }

    // Also clear function references held in array elements_
    if (object_kind() == ObjectKind::kArray) {
        for (auto& [k, elem] : elements_) {
            if (!elem.is_object()) continue;
            RcObject* raw = elem.as_object_raw();
            if (raw == nullptr) continue;
            ObjectPtr keep_alive(raw);
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* function = static_cast<JSFunction*>(raw);
                RcPtr<JSObject> prototype = function->prototype_obj();
                if (prototype) {
                    prototype->clear_function_properties(visited);
                }
                elem = Value::undefined();
            } else if (raw->object_kind() == ObjectKind::kOrdinary ||
                       raw->object_kind() == ObjectKind::kArray) {
                static_cast<JSObject*>(raw)->clear_function_properties(visited);
            }
        }
    }
}

std::vector<std::string> JSObject::own_enumerable_string_keys() const {
    std::vector<std::string> result;
    if (object_kind() == ObjectKind::kArray) {
        // Collect integer indices in sorted order, then non-index properties
        std::vector<uint32_t> indices;
        indices.reserve(elements_.size());
        for (const auto& [k, v] : elements_) {
            indices.push_back(k);
        }
        std::sort(indices.begin(), indices.end());
        for (uint32_t idx : indices) {
            result.push_back(std::to_string(idx));
        }
        for (const auto& entry : properties_) {
            result.push_back(entry.key);
        }
    } else {
        // kOrdinary or kFunction: insertion-order properties
        for (const auto& entry : properties_) {
            result.push_back(entry.key);
        }
    }
    return result;
}

}  // namespace qppjs
