#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qppjs {

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
    // length setter may throw RangeError; array index writes auto-extend elements_
    EvalResult set_property_ex(const std::string& key, Value value);
    // Returns true if deleted (or property didn't exist); false if non-configurable.
    bool delete_property(const std::string& key);
    // constructor_property_ is a raw (non-owning) pointer — weak reference semantics.
    void set_constructor_property(RcObject* value);
    bool has_own_property(const std::string& key) const;
    void clear_function_properties();
    std::vector<std::string> own_enumerable_string_keys() const;

    // Symbol-keyed property access (lazy-initialized storage).
    Value get_property_by_symbol(uint64_t symbol_id) const;
    void set_property_by_symbol(uint64_t symbol_id, Value val);
    bool has_own_symbol(uint64_t symbol_id) const;

    // Only used by kArray objects — sparse storage + explicit length
    std::unordered_map<uint32_t, Value> elements_;
    uint32_t array_length_ = 0;

private:
    void clear_function_properties(std::unordered_set<const JSObject*>& visited);

    struct SymbolPropertyEntry {
        uint64_t symbol_id;
        Value value;
    };
    // Lazily initialized symbol-keyed property storage.
    std::unique_ptr<std::vector<SymbolPropertyEntry>> symbol_props_;
    std::unique_ptr<std::unordered_map<uint64_t, size_t>> symbol_index_;

private:
    RcPtr<JSObject> proto_;
    struct PropertyEntry {
        std::string key;
        Value value;
    };
    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
    // Tracks the number of live entries in index_map_ (for own_enumerable_string_keys pre-alloc).
    size_t active_count_ = 0;
    // Raw pointer — weak reference, does not own. Caller must ensure lifetime.
    RcObject* constructor_property_ = nullptr;
    bool has_constructor_property_ = false;
};

}  // namespace qppjs
