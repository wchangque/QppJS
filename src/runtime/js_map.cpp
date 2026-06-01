#include "qppjs/runtime/js_map.h"

#include "qppjs/runtime/gc_heap.h"

#include <cmath>

namespace qppjs {

// ---- SameValueZero ----

bool same_value_zero(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) {
        double da = a.as_number();
        double db = b.as_number();
        if (std::isnan(da) && std::isnan(db)) return true;
        // +0 === -0 in SameValueZero
        return da == db;
    }
    if (a.is_string() && b.is_string()) {
        return a.sv() == b.sv();
    }
    if (a.is_bool() && b.is_bool()) {
        return a.as_bool() == b.as_bool();
    }
    if (a.is_object() && b.is_object()) {
        return a.as_object_raw() == b.as_object_raw();
    }
    if (a.is_undefined() && b.is_undefined()) return true;
    if (a.is_null() && b.is_null()) return true;
    if (a.is_symbol() && b.is_symbol()) {
        return a.as_symbol_id() == b.as_symbol_id();
    }
    return false;
}

// ---- JSMap ----

size_t JSMap::find_key(const Value& key) const {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (same_value_zero(entries_[i].first, key)) return i;
    }
    return kNotFound;
}

void JSMap::TraceRefs(GcHeap& heap) {
    JSObject::TraceRefs(heap);
    for (auto& [k, v] : entries_) {
        if (k.is_object()) heap.MarkPending(k.as_object_raw());
        if (v.is_object()) heap.MarkPending(v.as_object_raw());
    }
}

void JSMap::ClearRefs() {
    JSObject::ClearRefs();
    entries_.clear();
}

// ---- JSSet ----

size_t JSSet::find_value(const Value& val) const {
    for (size_t i = 0; i < values_.size(); ++i) {
        if (same_value_zero(values_[i], val)) return i;
    }
    return kNotFound;
}

void JSSet::TraceRefs(GcHeap& heap) {
    JSObject::TraceRefs(heap);
    for (auto& v : values_) {
        if (v.is_object()) heap.MarkPending(v.as_object_raw());
    }
}

void JSSet::ClearRefs() {
    JSObject::ClearRefs();
    values_.clear();
}

// ---- JSWeakMap ----

void JSWeakMap::TraceRefs(GcHeap& heap) {
    JSObject::TraceRefs(heap);
    // Trace values (keys are weak — not traced)
    for (auto& [k, v] : table_) {
        if (v.is_object()) heap.MarkPending(v.as_object_raw());
    }
}

void JSWeakMap::ClearRefs() {
    JSObject::ClearRefs();
    table_.clear();
}

// ---- JSWeakSet ----

void JSWeakSet::TraceRefs(GcHeap& heap) {
    JSObject::TraceRefs(heap);
    // Keys are weak — not traced
    (void)heap;
}

void JSWeakSet::ClearRefs() {
    JSObject::ClearRefs();
    table_.clear();
}

// ---- JSMapIterator ----

void JSMapIterator::TraceRefs(GcHeap& heap) {
    if (map_) heap.MarkPending(map_.get());
}

void JSMapIterator::ClearRefs() {
    map_.reset_no_release();
}

// ---- JSSetIterator ----

void JSSetIterator::TraceRefs(GcHeap& heap) {
    if (set_) heap.MarkPending(set_.get());
}

void JSSetIterator::ClearRefs() {
    set_.reset_no_release();
}

}  // namespace qppjs
