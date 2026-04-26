#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

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
    // constructor_property_ is a raw (non-owning) pointer — weak reference semantics.
    void set_constructor_property(RcObject* value);
    bool has_own_property(const std::string& key) const;
    void clear_function_properties();
    std::vector<std::string> own_enumerable_string_keys() const;

    // Only used by kArray objects — sparse storage + explicit length
    std::unordered_map<uint32_t, Value> elements_;
    uint32_t array_length_ = 0;

private:
    void clear_function_properties(std::unordered_set<const JSObject*>& visited);

private:
    RcPtr<JSObject> proto_;
    struct PropertyEntry {
        std::string key;
        Value value;
    };
    std::vector<PropertyEntry> properties_;
    std::unordered_map<std::string, size_t> index_map_;
    // Raw pointer — weak reference, does not own. Caller must ensure lifetime.
    RcObject* constructor_property_ = nullptr;
    bool has_constructor_property_ = false;
};

}  // namespace qppjs
