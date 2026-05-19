#include "qppjs/runtime/js_object.h"

#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/js_function.h"

#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace qppjs {

void JSObject::TraceRefs(GcHeap& heap) {
    if (proto_) heap.MarkPending(proto_.get());
    for (const auto& entry : properties_) {
        if (entry.value.is_object()) heap.MarkPending(entry.value.as_object_raw());
    }
    for (const auto& [idx, val] : elements_) {
        if (val.is_object()) heap.MarkPending(val.as_object_raw());
    }
    if (symbol_props_) {
        for (const auto& entry : *symbol_props_) {
            if (entry.value.is_object()) heap.MarkPending(entry.value.as_object_raw());
        }
    }
    // constructor_property_ is a raw weak ref; do not trace — kept alive by JSFunction::prototype_
}

void JSObject::ClearRefs() {
    // Release normally: non-GC objects get their ref_count decremented;
    // GC-swept objects have kGcSentinel so release() is a no-op.
    proto_ = RcPtr<JSObject>();
    properties_.clear();
    index_map_.clear();
    active_count_ = 0;
    elements_.clear();
    if (symbol_props_) symbol_props_->clear();
    if (symbol_index_) symbol_index_->clear();
    constructor_property_ = nullptr;
    has_constructor_property_ = false;
}

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
        ++active_count_;
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

bool JSObject::delete_property(const std::string& key) {
    if (object_kind() == ObjectKind::kArray) {
        uint32_t idx = 0;
        if (try_parse_array_index(key, idx)) {
            elements_.erase(idx);
            // array_length_ is not changed (hole semantics)
            return true;
        }
        // Non-configurable built-in: length
        if (key == "length") {
            return false;
        }
    }
    // Non-configurable built-in: prototype on functions is handled by JSFunction,
    // but JSObject itself has no non-configurable named properties besides array length.
    auto it = index_map_.find(key);
    if (it == index_map_.end()) {
        return true;  // property doesn't exist — delete succeeds
    }
    size_t slot_idx = it->second;
    index_map_.erase(it);
    properties_[slot_idx].value = Value::undefined();
    --active_count_;
    return true;
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
        // Non-index keys: enumerate via index_map_ to skip deleted slots
        result.reserve(result.size() + active_count_);
        for (const auto& [key, slot] : index_map_) {
            result.push_back(key);
        }
    } else {
        // kOrdinary or kFunction: enumerate via index_map_ to skip deleted slots
        result.reserve(active_count_);
        // Preserve insertion order by iterating properties_ and checking index_map_.
        // Also verify the slot index matches to skip stale entries left by delete+re-insert.
        for (size_t i = 0; i < properties_.size(); ++i) {
            const auto& entry = properties_[i];
            auto it = index_map_.find(entry.key);
            if (it != index_map_.end() && it->second == i) {
                result.push_back(entry.key);
            }
        }
    }
    return result;
}

Value JSObject::get_property_by_symbol(uint64_t symbol_id) const {
    if (symbol_index_ && symbol_props_) {
        auto it = symbol_index_->find(symbol_id);
        if (it != symbol_index_->end()) {
            return (*symbol_props_)[it->second].value;
        }
    }
    // Walk prototype chain
    const JSObject* proto = proto_.get();
    while (proto != nullptr) {
        if (proto->symbol_index_ && proto->symbol_props_) {
            auto it = proto->symbol_index_->find(symbol_id);
            if (it != proto->symbol_index_->end()) {
                return (*proto->symbol_props_)[it->second].value;
            }
        }
        proto = proto->proto_.get();
    }
    return Value::undefined();
}

void JSObject::set_property_by_symbol(uint64_t symbol_id, Value val) {
    if (!symbol_props_) {
        symbol_props_ = std::make_unique<std::vector<SymbolPropertyEntry>>();
        symbol_index_ = std::make_unique<std::unordered_map<uint64_t, size_t>>();
    }
    auto it = symbol_index_->find(symbol_id);
    if (it != symbol_index_->end()) {
        (*symbol_props_)[it->second].value = std::move(val);
    } else {
        size_t idx = symbol_props_->size();
        symbol_props_->push_back({symbol_id, std::move(val)});
        (*symbol_index_)[symbol_id] = idx;
    }
}

bool JSObject::has_own_symbol(uint64_t symbol_id) const {
    if (!symbol_index_) return false;
    return symbol_index_->count(symbol_id) > 0;
}

}  // namespace qppjs
