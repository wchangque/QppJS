#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qppjs {

class JSObject;
struct BytecodeFunction;

// Native function signature: receives this_val, evaluated args; is_new_call is true when called via new.
using NativeFn = std::function<EvalResult(Value this_val, std::vector<Value> args, bool is_new_call)>;

class JSFunction : public RcObject {
public:
    JSFunction() : RcObject(ObjectKind::kFunction) {}

    void TraceRefs(GcHeap& heap) override;
    void ClearRefs() override;

    const std::optional<std::string>& name() const { return name_; }
    const std::vector<std::string>& params() const { return params_; }
    const std::shared_ptr<std::vector<StmtNode>>& body() const { return body_; }
    const RcPtr<Environment>& closure_env() const { return closure_env_; }
    const RcPtr<JSObject>& prototype_obj() const { return prototype_; }
    const std::shared_ptr<BytecodeFunction>& bytecode() const { return bytecode_; }

    void set_name(std::optional<std::string> v) { name_ = std::move(v); }
    void set_params(std::vector<std::string> v) { params_ = std::move(v); }
    void set_body(std::shared_ptr<std::vector<StmtNode>> v) { body_ = std::move(v); }
    void set_closure_env(RcPtr<Environment> v) { closure_env_ = std::move(v); }
    void set_prototype_obj(RcPtr<JSObject> v) { prototype_ = std::move(v); }
    void set_bytecode(std::shared_ptr<BytecodeFunction> v) { bytecode_ = std::move(v); }
    void set_native_fn(NativeFn fn) { native_fn_ = std::move(fn); }
    void set_is_named_expr(bool v) { is_named_expr_ = v; }

    bool is_native() const { return native_fn_.has_value(); }
    bool is_named_expr() const { return is_named_expr_; }
    const NativeFn& native_fn() const { return *native_fn_; }

    // Bound function fields (populated by Function.prototype.bind)
    bool is_bound() const { return is_bound_; }
    void set_bound(Value target, Value this_val, std::vector<Value> args) {
        is_bound_ = true;
        bound_target_ = std::move(target);
        bound_this_ = std::move(this_val);
        bound_args_ = std::move(args);
    }
    const Value& bound_target() const { return bound_target_; }
    const Value& bound_this_val() const { return bound_this_; }
    const std::vector<Value>& bound_args() const { return bound_args_; }

    // Static properties on the function object itself (e.g., Object.keys, Array.isArray).
    void set_property(const std::string& key, Value value);
    Value get_property(const std::string& key) const;
    void clear_own_properties();

private:
    std::optional<std::string> name_;
    std::vector<std::string> params_;
    std::shared_ptr<std::vector<StmtNode>> body_;
    RcPtr<Environment> closure_env_;
    RcPtr<JSObject> prototype_;  // F.prototype (not [[Prototype]] of the function itself)
    std::shared_ptr<BytecodeFunction> bytecode_;
    std::optional<NativeFn> native_fn_;
    bool is_named_expr_ = false;
    // Own properties (e.g., Object.keys, Object.assign, Object.create)
    std::unordered_map<std::string, Value> own_properties_;

    // Bound function data (set_bound populates these)
    bool is_bound_ = false;
    Value bound_target_;
    Value bound_this_;
    std::vector<Value> bound_args_;
};

}  // namespace qppjs
