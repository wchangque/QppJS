#include "qppjs/runtime/gc_heap.h"

#include "qppjs/runtime/rc_object.h"

#include <vector>

namespace qppjs {

void GcHeap::Register(RcObject* obj) {
    if (!obj) return;
    obj->gc_heap_ = this;
    objects_.insert(obj);
}

void GcHeap::Unregister(RcObject* obj) {
    if (!obj) return;
    objects_.erase(obj);
    // Do not clear gc_heap_ here: the destructor path calls Unregister via the
    // private RcObject::Unregister() shim, which already runs in the dtor.
}

void GcHeap::MarkPending(RcObject* obj) {
    if (!obj || obj->gc_mark_) return;
    obj->gc_mark_ = true;
    worklist_.push_back(obj);
}

void GcHeap::Collect(const std::vector<RcObject*>& roots) {
    // Phase 1: Reset all marks
    for (RcObject* obj : objects_) {
        obj->gc_mark_ = false;
    }
    // Also reset roots that are not registered (long-lived prototype objects, etc.)
    for (RcObject* root : roots) {
        if (root) root->gc_mark_ = false;
    }

    // Phase 2: Mark from roots
    for (RcObject* root : roots) {
        MarkPending(root);
    }
    DrainWorklist();

    // Phase 3: Sweep unreachable objects
    Sweep();
}

void GcHeap::DrainWorklist() {
    while (!worklist_.empty()) {
        RcObject* obj = worklist_.back();
        worklist_.pop_back();
        obj->TraceRefs(*this);
    }
}

void GcHeap::Sweep() {
    std::vector<RcObject*> to_delete;
    for (RcObject* obj : objects_) {
        if (!obj->gc_mark_) {
            to_delete.push_back(obj);
        }
    }
    // Phase A: remove from registry and set sentinel on all unreachable objects FIRST.
    // This ensures that when ClearRefs() calls release() on sibling GC objects,
    // the release() is a no-op (kGcSentinel), preventing premature deletion.
    for (RcObject* obj : to_delete) {
        objects_.erase(obj);
        obj->gc_heap_ = nullptr;
        obj->set_gc_sentinel();
    }
    // Phase B: clear cross-references. Now safe because all GC-swept objects have
    // kGcSentinel, so release() on them is a no-op. Non-GC objects get their
    // ref_count decremented normally (fixing the ref_count imbalance).
    for (RcObject* obj : to_delete) {
        obj->ClearRefs();
    }
    // Phase C: delete all swept objects. Their RcPtr members are already cleared.
    for (RcObject* obj : to_delete) {
        delete obj;
    }
}

}  // namespace qppjs

// RcObject::Unregister() 的定义放在此处，避免 gc_heap.h 的循环包含问题
namespace qppjs {

void RcObject::Unregister() {
    gc_heap_->Unregister(this);
    gc_heap_ = nullptr;
}

}  // namespace qppjs
