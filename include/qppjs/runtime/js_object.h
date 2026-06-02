#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qppjs {

// Property attribute flags (stored in PropertyEntry.flags)
constexpr uint8_t kPropWritable     = 0x01;
constexpr uint8_t kPropEnumerable   = 0x02;
constexpr uint8_t kPropConfigurable = 0x04;
constexpr uint8_t kPropIsAccessor   = 0x08;
// Default flags for user-level set_property (writable + enumerable + configurable)
constexpr uint8_t kPropDefault      = 0x07;

// Property descriptor for Object.defineProperty (stack-only, not stored)
struct PropDesc {
    std::optional<Value> value;
    std::optional<bool> writable;
    std::optional<Value> getter;
    std::optional<Value> setter;
    std::optional<bool> enumerable;
    std::optional<bool> configurable;
};

class JSObject : public RcObject {
public:
    JSObject() : RcObject(ObjectKind::kOrdinary) {}
    explicit JSObject(ObjectKind kind) : RcObject(kind) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    void set_proto(RcPtr<JSObject> proto) { proto_ = std::move(proto); }
    const RcPtr<JSObject>& proto() const { return proto_; }

    Value get_property(const std::string& key) const;
    void set_property(const std::string& key, Value value);
    // Internal initialization: flags = 0x00 (non-writable, non-enumerable, non-configurable).
    void define_builtin_property(const std::string& key, Value value);
    // length setter may throw RangeError; array index writes auto-extend elements_
    // Also checks writable/extensible for descriptor-aware properties.
    EvalResult set_property_ex(const std::string& key, Value value);
    // Returns true if deleted (or property didn't exist); false if non-configurable.
    bool delete_property(const std::string& key);
    // [[DefineOwnProperty]] per spec: ValidateAndApplyPropertyDescriptor.
    // Returns ok(undefined) on success, err(TypeError) on violation.
    EvalResult define_property(const std::string& key, const PropDesc& desc);
    // constructor_property_ is a raw (non-owning) pointer — weak reference semantics.
    void set_constructor_property(RcObject* value);
    bool has_own_property(const std::string& key) const;
    // Check whether key exists anywhere in this object's prototype chain.
    bool has_property(const std::string& key) const;
    // Check whether symbol_id exists anywhere in this object's prototype chain.
    bool has_property_by_symbol(uint64_t symbol_id) const;
    void clear_function_properties();
    std::vector<std::string> own_enumerable_string_keys() const;
    // All own string property names (including non-enumerable), excluding Symbol keys.
    // Used by Object.getOwnPropertyNames.
    std::vector<std::string> own_all_string_keys() const;
    // Enumerate all string keys reachable via this object and its prototype chain,
    // including only enumerable properties, deduplicating by name.
    std::vector<std::string> enumerate_properties() const;

    bool extensible() const { return extensible_; }
    void set_extensible(bool v) { extensible_ = v; }

    // Wrapped primitive value for kStringObject / kBooleanObject.
    const Value& wrapped_value() const { return wrapped_value_; }
    void set_wrapped_value(Value v) { wrapped_value_ = std::move(v); }

    struct PropertyEntry {
        std::string key;
        Value value;
        Value getter;   // kPropIsAccessor: valid; data: undefined
        Value setter;   // kPropIsAccessor: valid; data: undefined
        uint8_t flags = kPropDefault;
    };

    // Returns nullptr if key not found in own properties.
    PropertyEntry* get_own_entry(const std::string& key);
    const PropertyEntry* get_own_entry(const std::string& key) const;

    // Symbol-keyed property entry (public for accessor handling at call sites).
    struct SymbolPropertyEntry {
        uint64_t symbol_id;
        Value value;
        Value getter;       // valid when is_accessor=true
        Value setter;       // valid when is_accessor=true
        bool is_accessor = false;
    };

    // Symbol-keyed property access (lazy-initialized storage).
    Value get_property_by_symbol(uint64_t symbol_id) const;
    void set_property_by_symbol(uint64_t symbol_id, Value val);
    bool has_own_symbol(uint64_t symbol_id) const;
    // [[DefineOwnProperty]] for Symbol keys. Supports data and accessor descriptors.
    EvalResult define_property_by_symbol(uint64_t symbol_id, const PropDesc& desc);
    // Traverse prototype chain and return the SymbolPropertyEntry for symbol_id, or nullptr.
    const SymbolPropertyEntry* find_symbol_entry(uint64_t symbol_id) const;

    // Only used by kArray objects — sparse storage + explicit length
    std::unordered_map<uint32_t, Value> elements_;
    uint32_t array_length_ = 0;

private:
    void clear_function_properties(std::unordered_set<const JSObject*>& visited);

    // Lazily initialized symbol-keyed property storage.
    std::unique_ptr<std::vector<SymbolPropertyEntry>> symbol_props_;
    std::unique_ptr<std::unordered_map<uint64_t, size_t>> symbol_index_;

private:
    RcPtr<JSObject> proto_;
    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
    // Tracks the number of live entries in index_map_ (for own_enumerable_string_keys pre-alloc).
    size_t active_count_ = 0;
    // Raw pointer — weak reference, does not own. Caller must ensure lifetime.
    RcObject* constructor_property_ = nullptr;
    bool has_constructor_property_ = false;
    bool extensible_ = true;
    Value wrapped_value_;  // kStringObject: string primitive; kBooleanObject: bool primitive
};

}  // namespace qppjs
