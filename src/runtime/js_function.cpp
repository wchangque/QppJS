#include "qppjs/runtime/js_function.h"

#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/js_object.h"

namespace qppjs {

void JSFunction::TraceRefs(GcHeap& heap) {
    if (closure_env_) heap.MarkPending(closure_env_.get());
    if (prototype_) heap.MarkPending(prototype_.get());
    for (auto& [key, val] : own_properties_) {
        if (val.is_object()) heap.MarkPending(val.as_object_raw());
    }
    if (is_bound_) {
        if (bound_target_.is_object()) heap.MarkPending(bound_target_.as_object_raw());
        if (bound_this_.is_object()) heap.MarkPending(bound_this_.as_object_raw());
        for (const auto& v : bound_args_) {
            if (v.is_object()) heap.MarkPending(v.as_object_raw());
        }
    }
    if (is_arrow_ && lexical_this_.is_object()) {
        heap.MarkPending(lexical_this_.as_object_raw());
    }
}

void JSFunction::ClearRefs() {
    // Release normally: non-GC objects get their ref_count decremented;
    // GC-swept objects have kGcSentinel so release() is a no-op.
    native_fn_.reset();  // destroy closure; any RcPtr captures call release() which is safe (sentinel)
    closure_env_ = RcPtr<Environment>();
    prototype_ = RcPtr<JSObject>();
    own_properties_.clear();
    if (is_bound_) {
        bound_target_ = Value::undefined();
        bound_this_ = Value::undefined();
        bound_args_.clear();
    }
    if (is_arrow_) {
        lexical_this_ = Value::undefined();
    }
    param_defs_.reset();
    instance_fields_.reset();
    field_initializer_.reset();
}

void JSFunction::set_property(const std::string& key, Value value) {
    own_properties_[key] = std::move(value);
}

Value JSFunction::get_property(const std::string& key) const {
    auto it = own_properties_.find(key);
    if (it != own_properties_.end()) {
        return it->second;
    }
    return Value::undefined();
}

bool JSFunction::has_property(const std::string& key) const {
    return own_properties_.count(key) > 0;
}

void JSFunction::clear_own_properties() {
    own_properties_.clear();
}

}  // namespace qppjs
