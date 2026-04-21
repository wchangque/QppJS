#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/value.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace qppjs {

struct Binding {
    Value value;
    bool mutable_;      // false for const
    bool initialized;   // false means TDZ
};

class Environment {
public:
    explicit Environment(std::shared_ptr<Environment> outer);

    // Declare binding by VarKind:
    //   Var   -> initialized=true,  value=undefined, mutable=true
    //   Let   -> initialized=false, mutable=true  (TDZ)
    //   Const -> initialized=false, mutable=false (TDZ)
    void define(const std::string& name, VarKind kind);

    // Declare an already-initialized binding (for var hoisting); idempotent.
    void define_initialized(const std::string& name);

    // Walk the outer chain; returns nullptr if not found.
    Binding* lookup(const std::string& name);

    // Read variable value; checks TDZ and undefined-reference.
    EvalResult get(const std::string& name);

    // Write variable value; checks const and undefined-reference.
    EvalResult set(const std::string& name, Value value);

    // Initialize a TDZ binding (called when let/const declaration executes).
    EvalResult initialize(const std::string& name, Value value);

    std::shared_ptr<Environment> outer() const { return outer_; }

private:
    std::unordered_map<std::string, Binding> bindings_;
    std::shared_ptr<Environment> outer_;
};

}  // namespace qppjs
