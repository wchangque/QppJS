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
class ModuleRecord;
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

    // Arrow function fields
    bool is_arrow() const { return is_arrow_; }
    const Value& lexical_this() const { return lexical_this_; }
    void set_arrow(bool v) { is_arrow_ = v; }
    void set_lexical_this(Value v) { lexical_this_ = std::move(v); }

    // Method shorthand flag: true for object literal method/getter/setter/async-method
    bool is_method() const { return is_method_; }
    void set_is_method(bool v) { is_method_ = v; }

    // Generator function flag
    bool is_generator() const { return is_generator_; }
    void set_is_generator(bool v) { is_generator_ = v; }

    // Async generator function flag (async function*)
    bool is_async_generator() const { return is_async_generator_; }
    void set_is_async_generator(bool v) { is_async_generator_ = v; }

    // Class constructor flags
    bool is_class_ctor() const { return is_class_ctor_; }
    bool is_derived_ctor() const { return is_derived_ctor_; }
    void set_is_class_ctor(bool v) { is_class_ctor_ = v; }
    void set_is_derived_ctor(bool v) { is_derived_ctor_ = v; }

    // [[HomeObject]]: the object on which the method is defined (weak, not RC-tracked)
    JSObject* home_object() const { return home_object_; }
    void set_home_object(JSObject* obj) { home_object_ = obj; }

    // Parent class constructor (weak pointer, kept alive via closure env / global env)
    JSFunction* fn_ctor_proto() const { return fn_ctor_proto_; }
    void set_fn_ctor_proto(JSFunction* fn) { fn_ctor_proto_ = fn; }

    // import.meta 词法绑定：记录函数是在哪个模块中创建的
    ModuleRecord* defining_module() const { return defining_module_; }
    void set_defining_module(ModuleRecord* mod) { defining_module_ = mod; }

    // rest 参数
    const std::optional<std::string>& rest_param() const { return rest_param_; }
    void set_rest_param(std::optional<std::string> v) { rest_param_ = std::move(v); }

    // 参数默认值定义（nullptr = native 函数或无默认值参数）
    const std::shared_ptr<std::vector<ParamDef>>& param_defs() const { return param_defs_; }
    void set_param_defs(std::shared_ptr<std::vector<ParamDef>> v) { param_defs_ = std::move(v); }

    // Instance field definitions for class constructors (Interpreter path)
    const std::shared_ptr<std::vector<ClassField>>& instance_fields() const { return instance_fields_; }
    void set_instance_fields(std::shared_ptr<std::vector<ClassField>> v) { instance_fields_ = std::move(v); }

    // Field initializer bytecode function for class constructors (VM path)
    const std::shared_ptr<BytecodeFunction>& field_initializer() const { return field_initializer_; }
    void set_field_initializer(std::shared_ptr<BytecodeFunction> v) { field_initializer_ = std::move(v); }

    // Private field name → symbol id mapping (only set on class constructors)
    // Key is "#name" (with # prefix), value is the symbol id from SymbolTable.
    const std::unordered_map<std::string, uint64_t>& private_fields() const { return private_fields_; }
    void set_private_field(const std::string& name, uint64_t sym_id) { private_fields_[name] = sym_id; }
    bool has_private_field(const std::string& name) const {
        return private_fields_.find(name) != private_fields_.end();
    }
    uint64_t get_private_field_sym(const std::string& name) const {
        auto it = private_fields_.find(name);
        if (it != private_fields_.end()) return it->second;
        return 0;
    }

    // Static properties on the function object itself (e.g., Object.keys, Array.isArray).
    void set_property(const std::string& key, Value value);
    Value get_property(const std::string& key) const;
    bool has_property(const std::string& key) const;
    void clear_own_properties();
    const std::unordered_map<std::string, Value>& own_properties() const { return own_properties_; }

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

    // Arrow function data
    bool is_arrow_ = false;
    Value lexical_this_;

    // Method shorthand data
    bool is_method_ = false;

    // Generator function data
    bool is_generator_ = false;
    bool is_async_generator_ = false;

    // Class constructor data
    bool is_class_ctor_ = false;
    bool is_derived_ctor_ = false;
    JSObject* home_object_ = nullptr;   // [[HomeObject]], weak ref, not RC-tracked
    JSFunction* fn_ctor_proto_ = nullptr;  // parent class ctor, weak ref

    // import.meta 词法绑定：函数定义时所在的模块（非拥有指针）
    ModuleRecord* defining_module_ = nullptr;

    // rest 参数名
    std::optional<std::string> rest_param_;

    // 参数默认值定义（nullptr = native 函数或无默认值参数）
    std::shared_ptr<std::vector<ParamDef>> param_defs_;

    // Instance field definitions (only set on class constructors, Interpreter path)
    std::shared_ptr<std::vector<ClassField>> instance_fields_;

    // Field initializer bytecode function for class constructors (VM path)
    std::shared_ptr<BytecodeFunction> field_initializer_;

    // Private field name → symbol id mapping (set at class creation time)
    std::unordered_map<std::string, uint64_t> private_fields_;
};

}  // namespace qppjs
