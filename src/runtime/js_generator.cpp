#include "qppjs/runtime/js_generator.h"

#include "qppjs/runtime/gc_heap.h"
#include "qppjs/vm/vm.h"  // for complete CallFrame definition (unique_ptr destructor)

namespace qppjs {

JSGeneratorObject::JSGeneratorObject() : JSObject(ObjectKind::kGenerator) {}

JSGeneratorObject::~JSGeneratorObject() = default;

void JSGeneratorObject::TraceRefs(GcHeap& heap) {
    JSObject::TraceRefs(heap);
    if (gen_env_) heap.MarkPending(gen_env_.get());
    if (gen_this_val_.is_object()) heap.MarkPending(gen_this_val_.as_object_raw());
    if (resume_value_.has_value() && resume_value_->is_object()) {
        heap.MarkPending(resume_value_->as_object_raw());
    }
    if (yield_delegate_iter_.is_object()) heap.MarkPending(yield_delegate_iter_.as_object_raw());
    if (yield_delegate_next_.is_object()) heap.MarkPending(yield_delegate_next_.as_object_raw());
    // Trace the suspended VM frame so GC does not collect objects referenced from it.
    if (suspended_frame_) {
        if (suspended_frame_->env) heap.MarkPending(suspended_frame_->env.get());
        auto trace_val = [&](const Value& v) {
            if (v.is_object()) {
                RcObject* raw = v.as_object_raw();
                if (raw) heap.MarkPending(raw);
            }
        };
        trace_val(suspended_frame_->this_val);
        trace_val(suspended_frame_->new_instance);
        for (const auto& v : suspended_frame_->stack) trace_val(v);
        if (suspended_frame_->pending_throw.has_value()) trace_val(*suspended_frame_->pending_throw);
        if (suspended_frame_->caught_exception.has_value()) trace_val(*suspended_frame_->caught_exception);
    }
}

void JSGeneratorObject::ClearRefs() {
    suspended_frame_.reset();
    gen_env_ = RcPtr<Environment>();
    gen_body_.reset();
    gen_this_val_ = Value::undefined();
    resume_value_.reset();
    yield_delegate_iter_ = Value::undefined();
    yield_delegate_next_ = Value::undefined();
    JSObject::ClearRefs();
}

}  // namespace qppjs
