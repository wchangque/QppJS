#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qppjs {

class JSObject;
struct BytecodeFunction;

// Native function signature: receives this_val, evaluated args; is_new_call is true when called via new.
using NativeFn = std::function<EvalResult(Value this_val, std::vector<Value> args, bool is_new_call)>;

class JSFunction : public RcObject {
public:
    JSFunction() : RcObject(ObjectKind::kFunction) {}

    const std::optional<std::string>& name() const { return name_; }
    const std::vector<std::string>& params() const { return params_; }
    const std::shared_ptr<std::vector<StmtNode>>& body() const { return body_; }
    const std::shared_ptr<Environment>& closure_env() const { return closure_env_; }
    const RcPtr<JSObject>& prototype_obj() const { return prototype_; }
    const std::shared_ptr<BytecodeFunction>& bytecode() const { return bytecode_; }

    void set_name(std::optional<std::string> v) { name_ = std::move(v); }
    void set_params(std::vector<std::string> v) { params_ = std::move(v); }
    void set_body(std::shared_ptr<std::vector<StmtNode>> v) { body_ = std::move(v); }
    void set_closure_env(std::shared_ptr<Environment> v) { closure_env_ = std::move(v); }
    void set_prototype_obj(RcPtr<JSObject> v) { prototype_ = std::move(v); }
    void set_bytecode(std::shared_ptr<BytecodeFunction> v) { bytecode_ = std::move(v); }
    void set_native_fn(NativeFn fn) { native_fn_ = std::move(fn); }

    bool is_native() const { return native_fn_.has_value(); }
    const NativeFn& native_fn() const { return *native_fn_; }

private:
    std::optional<std::string> name_;
    std::vector<std::string> params_;
    std::shared_ptr<std::vector<StmtNode>> body_;
    std::shared_ptr<Environment> closure_env_;
    RcPtr<JSObject> prototype_;  // F.prototype (not [[Prototype]] of the function itself)
    std::shared_ptr<BytecodeFunction> bytecode_;
    std::optional<NativeFn> native_fn_;
};

}  // namespace qppjs
