#pragma once

#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <vector>

namespace qppjs {

// ForInIterator holds the pre-computed keys for a for...in loop.
// Registered with GcHeap and pushed onto the operand stack as a Value::object().
class ForInIterator : public RcObject {
public:
    ForInIterator() : RcObject(ObjectKind::kForInIterator) {}

    // keys are string Values — JSString uses its own refcounting, not GC.
    void TraceRefs(GcHeap& /* heap */) override {}

    // Clear keys to release JSString refcounts before GC sweep.
    void ClearRefs() override { keys_.clear(); }

    std::vector<Value> keys_;
    size_t index_ = 0;
};

}  // namespace qppjs
