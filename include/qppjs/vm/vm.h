#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace qppjs {

class JSObject;
class JSFunction;
class ModuleRecord;

struct ExceptionHandler {
    size_t catch_target;  // bytecode absolute offset of the catch/finally handler
    size_t stack_depth;   // operand stack size at EnterTry time
    size_t scope_depth;   // scope depth at EnterTry time
};

struct CallFrame {
    const BytecodeFunction* bytecode = nullptr;
    size_t pc = 0;
    std::vector<Value> stack;
    RcPtr<Environment> env;
    Value this_val = Value::undefined();
    bool is_new_call = false;                  // true if created by NewCall
    Value new_instance = Value::undefined();   // the new instance (only valid if is_new_call)

    // Phase 7: exception handling
    std::vector<ExceptionHandler> handler_stack;
    std::optional<Value> pending_throw;
    std::optional<Value> caught_exception;  // set by exception_handler for GetException to consume
    std::vector<size_t> finally_return_stack;  // gosub return address stack
    size_t scope_depth = 0;

    // Phase 10.2: 当前模块（非拥有指针，仅模块顶层帧有效）
    ModuleRecord* current_module = nullptr;
};

class VM {
public:
    VM();

    EvalResult exec(std::shared_ptr<BytecodeFunction> bytecode);

    // 执行入口模块文件（ESM）
    EvalResult exec_module(const std::string& entry_path);

private:
    void init_global_env();
    // Main dispatch loop. Runs until call_stack_.size() == exit_depth.
    EvalResult run(size_t exit_depth = 0);

    // Module: Link 阶段（DFS）
    EvalResult link_module(ModuleRecord& mod);
    // Module: Evaluate 阶段（DFS）
    EvalResult evaluate_module(ModuleRecord& mod);
    // Module: 执行模块体（编译 + 运行）
    EvalResult exec_module_body(ModuleRecord& mod);

    // Push a new CallFrame. Returns error if call depth exceeded.
    EvalResult push_call_frame(RcPtr<JSFunction> fn, Value this_val, std::span<Value> args,
                               bool is_new = false, Value new_instance = Value::undefined());

    // Call a JS or native function value from within a NativeFn (e.g., forEach callback).
    // Runs synchronously by entering a nested run() loop.
    EvalResult call_function_val(Value fn_val, Value this_val, std::span<Value> args);

    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    static std::string to_string_val(const Value& v);
    static bool abstract_eq(const Value& a, const Value& b);

    Value make_error_value(NativeErrorType type, const std::string& message);

    GcHeap gc_heap_;
    ModuleLoader module_loader_;

    // deque: push_back does not invalidate references to existing elements
    std::deque<CallFrame> call_stack_;
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;

    RcPtr<JSObject> object_prototype_;
    RcPtr<JSObject> array_prototype_;
    RcPtr<JSObject> function_prototype_; // Function.prototype (call/apply/bind)
    RcPtr<JSFunction> object_constructor_;  // global Object function
    RcPtr<Environment> global_env_;

    // Error prototype cache: indexed by NativeErrorType
    std::array<RcPtr<JSObject>, static_cast<size_t>(NativeErrorType::kCount)> error_protos_;
};

}  // namespace qppjs
