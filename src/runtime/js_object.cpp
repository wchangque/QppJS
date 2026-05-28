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
        if (entry.getter.is_object()) heap.MarkPending(entry.getter.as_object_raw());
        if (entry.setter.is_object()) heap.MarkPending(entry.setter.as_object_raw());
    }
    for (const auto& [idx, val] : elements_) {
        if (val.is_object()) heap.MarkPending(val.as_object_raw());
    }
    if (symbol_props_) {
        for (const auto& entry : *symbol_props_) {
            if (entry.value.is_object()) heap.MarkPending(entry.value.as_object_raw());
            if (entry.getter.is_object()) heap.MarkPending(entry.getter.as_object_raw());
            if (entry.setter.is_object()) heap.MarkPending(entry.setter.as_object_raw());
        }
    }
    if (wrapped_value_.is_object()) heap.MarkPending(wrapped_value_.as_object_raw());
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
    wrapped_value_ = Value();
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
        // Preserve existing flags (don't downgrade a non-default property to default)
    } else {
        size_t idx = properties_.size();
        properties_.push_back(PropertyEntry{key, std::move(value), Value::undefined(), Value::undefined(), kPropDefault});
        index_map_.emplace(key, idx);
        ++active_count_;
    }
}

void JSObject::define_builtin_property(const std::string& key, Value value) {
    auto it = index_map_.find(key);
    if (it != index_map_.end()) {
        properties_[it->second].value = std::move(value);
        properties_[it->second].flags = 0x00;
    } else {
        size_t idx = properties_.size();
        properties_.push_back(PropertyEntry{key, std::move(value), Value::undefined(), Value::undefined(), 0x00});
        index_map_.emplace(key, idx);
        ++active_count_;
    }
}

JSObject::PropertyEntry* JSObject::get_own_entry(const std::string& key) {
    auto it = index_map_.find(key);
    if (it == index_map_.end()) return nullptr;
    return &properties_[it->second];
}

const JSObject::PropertyEntry* JSObject::get_own_entry(const std::string& key) const {
    auto it = index_map_.find(key);
    if (it == index_map_.end()) return nullptr;
    return &properties_[it->second];
}

// SameValue per spec: NaN==NaN, +0 != -0
static bool same_value(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) {
        double da = a.as_number();
        double db = b.as_number();
        if (std::isnan(da) && std::isnan(db)) return true;
        if (da == 0.0 && db == 0.0) {
            // distinguish +0 and -0 via sign bit
            return std::signbit(da) == std::signbit(db);
        }
        return da == db;
    }
    if (a.is_string() && b.is_string()) return a.sv() == b.sv();
    if (a.is_bool() && b.is_bool()) return a.as_bool() == b.as_bool();
    if (a.is_undefined() && b.is_undefined()) return true;
    if (a.is_null() && b.is_null()) return true;
    if (a.is_object() && b.is_object()) return a.as_object_raw() == b.as_object_raw();
    return false;
}

EvalResult JSObject::define_property(const std::string& key, const PropDesc& desc) {
    PropertyEntry* existing = get_own_entry(key);

    if (existing == nullptr) {
        // Property doesn't exist
        if (!extensible_) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Cannot define property " + key + ", object is not extensible"});
        }
        // Create new property — missing fields default to false/undefined
        bool is_accessor = desc.getter.has_value() || desc.setter.has_value();
        uint8_t flags = 0x00;
        if (desc.writable.value_or(false)) flags |= kPropWritable;
        if (desc.enumerable.value_or(false)) flags |= kPropEnumerable;
        if (desc.configurable.value_or(false)) flags |= kPropConfigurable;
        if (is_accessor) flags |= kPropIsAccessor;
        Value val = desc.value.value_or(Value::undefined());
        Value getter = desc.getter.value_or(Value::undefined());
        Value setter = desc.setter.value_or(Value::undefined());
        size_t idx = properties_.size();
        properties_.push_back(PropertyEntry{key, std::move(val), std::move(getter), std::move(setter), flags});
        index_map_.emplace(key, idx);
        ++active_count_;
        return EvalResult::ok(Value::undefined());
    }

    // Property exists — validate changes
    bool configurable = (existing->flags & kPropConfigurable) != 0;
    bool writable = (existing->flags & kPropWritable) != 0;
    bool is_accessor = (existing->flags & kPropIsAccessor) != 0;

    // If descriptor is empty, no-op
    if (!desc.value.has_value() && !desc.writable.has_value() && !desc.getter.has_value() &&
        !desc.setter.has_value() && !desc.enumerable.has_value() && !desc.configurable.has_value()) {
        return EvalResult::ok(Value::undefined());
    }

    bool desc_has_accessor = desc.getter.has_value() || desc.setter.has_value();

    if (!configurable) {
        // configurable: false -> can't change configurable to true
        if (desc.configurable.has_value() && desc.configurable.value()) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Cannot redefine property: " + key});
        }
        // Can't change enumerable
        if (desc.enumerable.has_value()) {
            bool cur_enum = (existing->flags & kPropEnumerable) != 0;
            if (desc.enumerable.value() != cur_enum) {
                return EvalResult::err(Error{ErrorKind::Runtime,
                    "TypeError: Cannot redefine property: " + key});
            }
        }
        // Can't switch data <-> accessor
        if (desc_has_accessor && !is_accessor) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Cannot redefine property: " + key});
        }
        if (desc.value.has_value() && !desc_has_accessor && is_accessor) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Cannot redefine property: " + key});
        }
        if (!is_accessor) {
            // data property checks
            if (!writable) {
                if (desc.writable.has_value() && desc.writable.value()) {
                    return EvalResult::err(Error{ErrorKind::Runtime,
                        "TypeError: Cannot redefine property: " + key});
                }
                if (desc.value.has_value() && !same_value(desc.value.value(), existing->value)) {
                    return EvalResult::err(Error{ErrorKind::Runtime,
                        "TypeError: Cannot redefine property: " + key});
                }
            }
        } else {
            // accessor property: non-configurable cannot change get or set
            if (desc.getter.has_value() && !same_value(desc.getter.value(), existing->getter)) {
                return EvalResult::err(Error{ErrorKind::Runtime,
                    "TypeError: Cannot redefine property: " + key});
            }
            if (desc.setter.has_value() && !same_value(desc.setter.value(), existing->setter)) {
                return EvalResult::err(Error{ErrorKind::Runtime,
                    "TypeError: Cannot redefine property: " + key});
            }
        }
    }

    // Apply the descriptor fields that are present
    if (desc.value.has_value()) existing->value = desc.value.value();
    if (desc.getter.has_value()) existing->getter = desc.getter.value();
    if (desc.setter.has_value()) existing->setter = desc.setter.value();
    if (desc.writable.has_value()) {
        if (desc.writable.value()) existing->flags |= kPropWritable;
        else existing->flags &= static_cast<uint8_t>(~kPropWritable);
    }
    if (desc.enumerable.has_value()) {
        if (desc.enumerable.value()) existing->flags |= kPropEnumerable;
        else existing->flags &= static_cast<uint8_t>(~kPropEnumerable);
    }
    if (desc.configurable.has_value()) {
        if (desc.configurable.value()) existing->flags |= kPropConfigurable;
        else existing->flags &= static_cast<uint8_t>(~kPropConfigurable);
    }
    if (desc_has_accessor) {
        existing->flags |= kPropIsAccessor;
        // Clear value for accessor
        existing->value = Value::undefined();
    } else if (desc.value.has_value()) {
        // Becoming data property
        existing->flags &= static_cast<uint8_t>(~kPropIsAccessor);
        existing->getter = Value::undefined();
        existing->setter = Value::undefined();
    }

    return EvalResult::ok(Value::undefined());
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

    // Check existing property for writable/accessor
    PropertyEntry* entry = get_own_entry(key);
    if (entry != nullptr) {
        if (entry->flags & kPropIsAccessor) {
            // Accessor: caller should handle setter separately; here we return ok (set via setter path)
            return EvalResult::ok(Value::undefined());
        }
        if (!(entry->flags & kPropWritable)) {
            // Sloppy mode: silently ignore assignment to non-writable property
            return EvalResult::ok(Value::undefined());
        }
        entry->value = std::move(value);
        return EvalResult::ok(Value::undefined());
    }

    // New property: check extensible. Sloppy mode: silently ignore.
    if (!extensible_) {
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
    auto it = index_map_.find(key);
    if (it == index_map_.end()) {
        return true;  // property doesn't exist — delete succeeds
    }
    size_t slot_idx = it->second;
    // Check configurable flag
    if (!(properties_[slot_idx].flags & kPropConfigurable)) {
        return false;
    }
    index_map_.erase(it);
    properties_[slot_idx].value = Value::undefined();
    properties_[slot_idx].getter = Value::undefined();
    properties_[slot_idx].setter = Value::undefined();
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
        // Clear accessor getter/setter
        if (property.getter.is_object()) {
            property.getter = Value::undefined();
        }
        if (property.setter.is_object()) {
            property.setter = Value::undefined();
        }
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

    // Clear function references held in symbol properties (e.g. Symbol.iterator closures).
    if (symbol_props_) {
        for (auto& entry : *symbol_props_) {
            // Clear accessor getter/setter (mirrors string-key property handling above)
            if (entry.getter.is_object()) entry.getter = Value::undefined();
            if (entry.setter.is_object()) entry.setter = Value::undefined();
            // Also release heap-allocated strings (e.g. Symbol.toStringTag = "Generator").
            // Strings can't create reference cycles, so clearing them is always safe.
            if (entry.value.is_string()) entry.value = Value::undefined();
            if (!entry.value.is_object()) continue;
            RcObject* raw = entry.value.as_object_raw();
            if (raw == nullptr) continue;
            ObjectPtr keep_alive(raw);
            if (raw->object_kind() == ObjectKind::kFunction) {
                auto* function = static_cast<JSFunction*>(raw);
                RcPtr<JSObject> prototype = function->prototype_obj();
                if (prototype) {
                    prototype->clear_function_properties(visited);
                }
                entry.value = Value::undefined();
            } else if (raw->object_kind() == ObjectKind::kOrdinary ||
                       raw->object_kind() == ObjectKind::kArray) {
                static_cast<JSObject*>(raw)->clear_function_properties(visited);
            }
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
            if (properties_[slot].flags & kPropEnumerable) {
                result.push_back(key);
            }
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
                if (entry.flags & kPropEnumerable) {
                    result.push_back(entry.key);
                }
            }
        }
    }
    return result;
}

std::vector<std::string> JSObject::enumerate_properties() const {
    // P1 fast path: no prototype chain, just return own keys directly.
    if (proto_ == nullptr) {
        return own_enumerable_string_keys();
    }
    // Walk the prototype chain, collecting enumerable string keys and deduplicating.
    // Shadowing rule (ES spec): an own property (even non-enumerable) must prevent an
    // inherited property with the same name from appearing in the result.
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    const JSObject* cur = this;
    while (cur != nullptr) {
        // For arrays: enumerate integer indices from elements_ (sorted), then named properties.
        // Without this, arrays with a prototype (i.e. all normal arrays) skip their elements_.
        if (cur->object_kind() == ObjectKind::kArray) {
            std::vector<uint32_t> indices;
            indices.reserve(cur->elements_.size());
            for (const auto& [k, v] : cur->elements_) {
                indices.push_back(k);
            }
            std::sort(indices.begin(), indices.end());
            for (uint32_t idx : indices) {
                std::string key = std::to_string(idx);
                bool is_new = seen.insert(key).second;
                if (is_new) {
                    result.push_back(key);
                }
            }
        }
        // Iterate all own string property entries (preserves insertion order via properties_).
        for (size_t i = 0; i < cur->properties_.size(); ++i) {
            const auto& entry = cur->properties_[i];
            auto it = cur->index_map_.find(entry.key);
            if (it == cur->index_map_.end() || it->second != i) continue;  // stale/deleted slot
            // Always insert into seen so that non-enumerable own keys shadow inherited ones.
            bool is_new = seen.insert(entry.key).second;
            if (is_new && (entry.flags & kPropEnumerable)) {
                result.push_back(entry.key);
            }
        }
        cur = cur->proto_.get();
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
        symbol_props_->push_back({symbol_id, std::move(val), Value::undefined(), Value::undefined(), false});
        (*symbol_index_)[symbol_id] = idx;
    }
}

bool JSObject::has_own_symbol(uint64_t symbol_id) const {
    if (!symbol_index_) return false;
    return symbol_index_->count(symbol_id) > 0;
}

const JSObject::SymbolPropertyEntry* JSObject::find_symbol_entry(uint64_t symbol_id) const {
    const JSObject* cur = this;
    while (cur != nullptr) {
        if (cur->symbol_index_ && cur->symbol_props_) {
            auto it = cur->symbol_index_->find(symbol_id);
            if (it != cur->symbol_index_->end()) {
                return &(*cur->symbol_props_)[it->second];
            }
        }
        cur = cur->proto_.get();
    }
    return nullptr;
}

EvalResult JSObject::define_property_by_symbol(uint64_t symbol_id, const PropDesc& desc) {
    if (!symbol_props_) {
        symbol_props_ = std::make_unique<std::vector<SymbolPropertyEntry>>();
        symbol_index_ = std::make_unique<std::unordered_map<uint64_t, size_t>>();
    }
    auto it = symbol_index_->find(symbol_id);
    bool is_new = (it == symbol_index_->end());
    bool desc_has_accessor = desc.getter.has_value() || desc.setter.has_value();

    if (is_new) {
        if (!extensible_) {
            return EvalResult::err(Error{ErrorKind::Runtime,
                "TypeError: Cannot define Symbol property, object is not extensible"});
        }
        SymbolPropertyEntry entry;
        entry.symbol_id = symbol_id;
        if (desc_has_accessor) {
            entry.is_accessor = true;
            entry.getter = desc.getter.value_or(Value::undefined());
            entry.setter = desc.setter.value_or(Value::undefined());
        } else {
            entry.value = desc.value.value_or(Value::undefined());
        }
        size_t idx = symbol_props_->size();
        symbol_props_->push_back(std::move(entry));
        (*symbol_index_)[symbol_id] = idx;
        return EvalResult::ok(Value::undefined());
    }

    // Existing entry: merge accessor descriptors (getter/setter can be set independently)
    SymbolPropertyEntry& existing = (*symbol_props_)[it->second];
    if (desc_has_accessor) {
        existing.is_accessor = true;
        if (desc.getter.has_value()) existing.getter = desc.getter.value();
        if (desc.setter.has_value()) existing.setter = desc.setter.value();
    } else if (desc.value.has_value()) {
        existing.is_accessor = false;
        existing.value = desc.value.value();
    }
    return EvalResult::ok(Value::undefined());
}

bool JSObject::has_property(const std::string& key) const {
    const JSObject* cur = this;
    while (cur != nullptr) {
        if (cur->object_kind() == ObjectKind::kArray) {
            if (key == "length") return true;
            uint32_t idx = 0;
            if (try_parse_array_index(key, idx)) {
                if (cur->elements_.count(idx) > 0) return true;
                // Fall through: non-index keys checked in properties_ below
                cur = cur->proto_.get();
                continue;
            }
        }
        if (cur->index_map_.count(key) > 0) return true;
        if (key == "constructor" && cur->has_constructor_property_) return true;
        cur = cur->proto_.get();
    }
    return false;
}

bool JSObject::has_property_by_symbol(uint64_t symbol_id) const {
    const JSObject* cur = this;
    while (cur != nullptr) {
        if (cur->symbol_index_ && cur->symbol_index_->count(symbol_id) > 0) return true;
        cur = cur->proto_.get();
    }
    return false;
}

}  // namespace qppjs
