#include "qppjs/runtime/environment.h"

#include "qppjs/base/error.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/js_function.h"

#include <unordered_set>

namespace qppjs {

namespace {

CellPtr MakeCell(Value value) {
    return RcPtr<Cell>::make(Cell{std::move(value)});
}

}  // namespace

Environment::Environment(RcPtr<Environment> outer)
    : RcObject(ObjectKind::kEnvironment), outer_(std::move(outer)) {}

void Environment::TraceRefs(GcHeap& heap) {
    if (outer_) heap.MarkPending(outer_.get());
    for (Binding* b : bindings_.binding_ptrs()) {
        if (b->cell && b->cell->value.is_object()) {
            RcObject* raw = b->cell->value.as_object_raw();
            if (raw) heap.MarkPending(raw);
        }
    }
}

void Environment::ClearRefs() {
    // Release outer_ normally: if outer_ is a non-GC object, its ref_count decrements
    // correctly; if it's also being swept (kGcSentinel), release() is a no-op.
    outer_ = RcPtr<Environment>();
    // Clear all cell values that hold object references.
    for (Binding* b : bindings_.binding_ptrs()) {
        if (b->cell) {
            b->cell->value = Value::undefined();
        }
    }
}

void Environment::define(const std::string& name, VarKind kind) {
    switch (kind) {
    case VarKind::Var:
        bindings_.insert_or_assign(name, Binding{MakeCell(Value::undefined()), true, true, false});
        break;
    case VarKind::Let:
        bindings_.insert_or_assign(name, Binding{MakeCell(Value::undefined()), true, false, false});
        break;
    case VarKind::Const:
        bindings_.insert_or_assign(name, Binding{MakeCell(Value::undefined()), false, false, false});
        break;
    }
}

void Environment::define_initialized(const std::string& name) {
    if (bindings_.count(name) != 0) {
        return;
    }
    bindings_.emplace(name, Binding{MakeCell(Value::undefined()), true, true, false});
}

void Environment::define_function(const std::string& name) {
    if (bindings_.count(name) != 0) {
        return;
    }
    bindings_.emplace(name, Binding{MakeCell(Value::undefined()), true, true, true});
}

Binding* Environment::lookup(const std::string& name) {
    Binding* b = bindings_.find(name);
    if (b != nullptr) {
        return b;
    }
    if (outer_) {
        return outer_->lookup(name);
    }
    return nullptr;
}

EvalResult Environment::get(const std::string& name) {
    Binding* b = lookup(name);
    if (b == nullptr) {
        return EvalResult::err(Error(ErrorKind::Runtime, "ReferenceError: " + name + " is not defined"));
    }
    if (!b->initialized) {
        return EvalResult::err(
            Error(ErrorKind::Runtime, "ReferenceError: Cannot access '" + name + "' before initialization"));
    }
    return EvalResult::ok(b->cell->value);
}

EvalResult Environment::set(const std::string& name, Value value) {
    Binding* b = lookup(name);
    if (b == nullptr) {
        return EvalResult::err(Error(ErrorKind::Runtime, "ReferenceError: " + name + " is not defined"));
    }
    if (!b->mutable_) {
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: Assignment to constant variable."));
    }
    b->cell->value = std::move(value);
    return EvalResult::ok(b->cell->value);
}

EvalResult Environment::initialize(const std::string& name, Value value) {
    Binding* b = lookup(name);
    if (b == nullptr) {
        return EvalResult::err(Error(ErrorKind::Runtime, "ReferenceError: " + name + " is not defined"));
    }
    b->initialized = true;
    b->cell->value = std::move(value);
    return EvalResult::ok(b->cell->value);
}

void Environment::clear_function_bindings() {
    std::unordered_set<const Environment*> visited;
    clear_function_bindings(visited);
}

void Environment::clear_function_bindings(std::unordered_set<const Environment*>& visited) {
    if (visited.contains(this)) {
        return;
    }
    RcPtr<Environment> self(this);
    visited.insert(this);

    for (Binding* binding : bindings_.binding_ptrs()) {
        if (!binding->initialized || !binding->cell->value.is_object()) {
            continue;
        }
        RcObject* raw = binding->cell->value.as_object_raw();
        if (raw == nullptr) {
            continue;
        }
        ObjectPtr keep_alive(raw);
        if (raw->object_kind() == ObjectKind::kFunction) {
            auto* function = static_cast<JSFunction*>(raw);
            RcPtr<JSObject> prototype = function->prototype_obj();
            RcPtr<Environment> closure_env = function->closure_env();
            if (closure_env) {
                closure_env->clear_function_bindings(visited);
            }
            if (prototype) {
                prototype->clear_function_properties();
            }
            binding->cell->value = Value::undefined();
            continue;
        }
        if (raw->object_kind() == ObjectKind::kOrdinary || raw->object_kind() == ObjectKind::kArray) {
            static_cast<JSObject*>(raw)->clear_function_properties();
        }
    }

    if (outer_) {
        outer_->clear_function_bindings(visited);
    }
}

}  // namespace qppjs
