#pragma once

#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qppjs {

// Map: ES6 ordered key-value collection with SameValueZero key comparison.
// Inherits JSObject so prototype method lookup works via get_property.
class JSMap : public JSObject {
public:
    JSMap() : JSObject(ObjectKind::kMap) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    // Find index of key using SameValueZero. Returns npos if not found.
    static constexpr size_t kNotFound = static_cast<size_t>(-1);
    size_t find_key(const Value& key) const;

    // Map entries in insertion order.
    std::vector<std::pair<Value, Value>> entries_;
};

// Set: ES6 ordered value collection with SameValueZero deduplication.
class JSSet : public JSObject {
public:
    JSSet() : JSObject(ObjectKind::kSet) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    size_t find_value(const Value& val) const;
    static constexpr size_t kNotFound = static_cast<size_t>(-1);

    std::vector<Value> values_;
};

// WeakMap: object-keyed map. Uses raw pointer as key (weak semantics, no true GC).
// QppJS does not implement true weak references; this uses raw RcObject* keys.
class JSWeakMap : public JSObject {
public:
    JSWeakMap() : JSObject(ObjectKind::kWeakMap) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    // Values must be traced; keys are weak (raw pointers).
    std::unordered_map<RcObject*, Value> table_;
};

// WeakSet: object-only set. Uses raw pointer (weak semantics).
class JSWeakSet : public JSObject {
public:
    JSWeakSet() : JSObject(ObjectKind::kWeakSet) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    std::unordered_set<RcObject*> table_;
};

// Iterator mode for Map and Set iterators.
enum class CollectionIterMode { kKeys, kValues, kEntries };

// Iterator over JSMap entries.
class JSMapIterator : public RcObject {
public:
    JSMapIterator() : RcObject(ObjectKind::kMapIterator) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    RcPtr<JSMap> map_;
    size_t index_ = 0;
    CollectionIterMode mode_ = CollectionIterMode::kEntries;
};

// Iterator over JSSet values.
class JSSetIterator : public RcObject {
public:
    JSSetIterator() : RcObject(ObjectKind::kSetIterator) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    RcPtr<JSSet> set_;
    size_t index_ = 0;
    CollectionIterMode mode_ = CollectionIterMode::kValues;
};

}  // namespace qppjs
