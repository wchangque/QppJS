#include "qppjs/runtime/environment.h"

#include "qppjs/base/error.h"

namespace qppjs {

Environment::Environment(std::shared_ptr<Environment> outer) : outer_(std::move(outer)) {}

void Environment::define(const std::string& name, VarKind kind) {
    switch (kind) {
    case VarKind::Var:
        bindings_.insert_or_assign(name, Binding{Value::undefined(), true, true});
        break;
    case VarKind::Let:
        bindings_.insert_or_assign(name, Binding{Value::undefined(), true, false});
        break;
    case VarKind::Const:
        bindings_.insert_or_assign(name, Binding{Value::undefined(), false, false});
        break;
    }
}

void Environment::define_initialized(const std::string& name) {
    if (bindings_.count(name) != 0) {
        return;
    }
    bindings_.emplace(name, Binding{Value::undefined(), true, true});
}

Binding* Environment::lookup(const std::string& name) {
    auto it = bindings_.find(name);
    if (it != bindings_.end()) {
        return &it->second;
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
    return EvalResult::ok(b->value);
}

EvalResult Environment::set(const std::string& name, Value value) {
    Binding* b = lookup(name);
    if (b == nullptr) {
        return EvalResult::err(Error(ErrorKind::Runtime, "ReferenceError: " + name + " is not defined"));
    }
    if (!b->mutable_) {
        return EvalResult::err(Error(ErrorKind::Runtime, "TypeError: Assignment to constant variable."));
    }
    b->value = std::move(value);
    return EvalResult::ok(b->value);
}

EvalResult Environment::initialize(const std::string& name, Value value) {
    Binding* b = lookup(name);
    if (b == nullptr) {
        return EvalResult::err(Error(ErrorKind::Runtime, "ReferenceError: " + name + " is not defined"));
    }
    b->initialized = true;
    b->value = std::move(value);
    return EvalResult::ok(b->value);
}

}  // namespace qppjs
