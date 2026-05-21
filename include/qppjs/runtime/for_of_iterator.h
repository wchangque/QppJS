#pragma once

#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <cstdint>

namespace qppjs {

// Generic for...of iterator: wraps a JS iterator object and its next() method.
// Used when the iterable is a generic object with Symbol.iterator.
class ForOfIterator : public RcObject {
public:
    ForOfIterator() : RcObject(ObjectKind::kForOfIterator) {}

    void TraceRefs(GcHeap& heap) override {
        if (iterator_.is_object()) heap.MarkPending(iterator_.as_object_raw());
        if (next_method_.is_object()) heap.MarkPending(next_method_.as_object_raw());
    }

    void ClearRefs() override {
        iterator_ = Value::undefined();
        next_method_ = Value::undefined();
    }

    Value iterator_;      // the JS iterator object
    Value next_method_;   // the iterator.next function
    bool done_ = false;
};

// Specialized iterator for arrays: avoids JS function calls per element.
class ArrayIterator : public RcObject {
public:
    ArrayIterator() : RcObject(ObjectKind::kArrayIterator) {}

    void TraceRefs(GcHeap& heap) override {
        if (array_ref_.is_object()) heap.MarkPending(array_ref_.as_object_raw());
    }

    void ClearRefs() override {
        array_ref_ = Value::undefined();
    }

    Value array_ref_;    // the JS array Value
    uint32_t index_ = 0;
    bool done_ = false;
};

// Specialized iterator for strings: iterates by Unicode code points (UTF-8 byte positions).
class StringIterator : public RcObject {
public:
    StringIterator() : RcObject(ObjectKind::kStringIterator) {}

    // string_val_ is a string Value — no object pointer to trace.
    void TraceRefs(GcHeap& /* heap */) override {}

    void ClearRefs() override {
        string_val_ = Value::undefined();
    }

    Value string_val_;       // the string Value being iterated
    uint32_t byte_pos_ = 0;  // current byte offset in UTF-8
    bool done_ = false;
};

}  // namespace qppjs
