#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/job_queue.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_generator.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/js_regexp.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/promise.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/symbol_table.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"

#include <array>
#include <deque>
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

    // Generator: owning generator object (non-owning raw pointer, GC managed).
    // Set when this frame belongs to an executing generator body.
    // JSGeneratorObject is forward-declared here; vm.cpp includes js_generator.h.
    class JSGeneratorObject* owning_generator = nullptr;

    // Class constructor state
    Value new_target_val = Value::undefined();  // new.target value for this frame
    bool derived_this_initialized = false;       // true after super() called in derived ctor
    RcPtr<JSFunction> current_fn_holder;         // keeps current function alive for GC safety
    JSFunction* current_fn = nullptr;            // non-owning pointer to current function (valid as long as current_fn_holder is held)
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

    // Promise helpers
    RcPtr<JSPromise> vm_promise_resolve(Value value);
    void vm_execute_reaction_job(ReactionJob job);
    void vm_drain_job_queue();

    // Async body result handler: fulfill/reject outer_promise based on body_result.
    // Also handles nested suspension (multiple awaits).
    void vm_handle_async_result(EvalResult body_result, RcPtr<JSPromise> outer_promise);

    // Generator helpers: resume a suspended generator frame
    EvalResult vm_generator_next(RcPtr<JSGeneratorObject> gen, Value resume_val);
    EvalResult vm_generator_return(RcPtr<JSGeneratorObject> gen, Value return_val);
    EvalResult vm_generator_throw(RcPtr<JSGeneratorObject> gen, Value throw_val);
    // Execute the suspended generator frame; handles yield/complete/throw
    EvalResult vm_generator_resume(RcPtr<JSGeneratorObject> gen);

    // Create a JSRegExp from pattern/flags. Returns error on invalid flags or pattern.
    EvalResult vm_make_regexp(const std::string& pattern, const std::string& flags);

    // Execute RegExp exec() on input string. Returns result array or null.
    EvalResult vm_regexp_exec(JSRegExp* rx, const std::string& input);

    GcHeap gc_heap_;
    ModuleLoader module_loader_;
    JobQueue job_queue_;
    SymbolTable symbol_table_;

    // Pending throw value for native functions that need to re-throw
    // (used when call_stack_ may be empty, e.g., during job queue drain).
    std::optional<Value> native_pending_throw_;

    // Async suspension state: set by kAwait when suspending a frame.
    // kAsyncSuspendSentinel is the string value used as the error message.
    static constexpr const char* kAsyncSuspendSentinel = "__qppjs_async_suspend__";
    bool vm_async_suspended_ = false;
    std::optional<RcPtr<JSPromise>> vm_pending_inner_promise_;
    std::optional<CallFrame> vm_suspended_frame_;

    // Generator yield state: set by kYield when a generator frame suspends.
    static constexpr const char* kGeneratorYieldSentinel = "__qppjs_generator_yield__";
    bool vm_generator_yielded_ = false;
    std::optional<Value> vm_generator_yield_value_;

    // deque: push_back does not invalidate references to existing elements
    std::deque<CallFrame> call_stack_;
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;

    RcPtr<JSObject> object_prototype_;
    RcPtr<JSObject> array_prototype_;
    RcPtr<JSObject> function_prototype_; // Function.prototype (call/apply/bind)
    RcPtr<JSObject> promise_prototype_;  // Promise.prototype (then/catch/finally)
    RcPtr<JSObject> boolean_prototype_;  // Boolean.prototype (valueOf/toString)
    RcPtr<JSObject> string_prototype_;   // String.prototype (indexOf/slice/trim/...)
    RcPtr<JSObject> math_obj_;           // Math object
    RcPtr<JSObject> number_prototype_;   // Number.prototype
    RcPtr<JSObject> regexp_prototype_;   // RegExp.prototype
    RcPtr<JSObject> symbol_prototype_;   // Symbol.prototype (toString/valueOf)
    RcPtr<JSObject> generator_prototype_;  // Generator prototype (.next/.return/.throw/[Symbol.iterator])
    RcPtr<JSFunction> object_constructor_;  // global Object function
    RcPtr<JSFunction> number_constructor_;  // global Number function
    RcPtr<JSFunction> boolean_constructor_;  // global Boolean function
    RcPtr<JSFunction> string_constructor_;  // global String function
    RcPtr<JSFunction> regexp_constructor_;  // global RegExp function
    RcPtr<JSFunction> symbol_constructor_;  // global Symbol function
    uint64_t math_random_state_ = 1;    // xorshift64* PRNG state
    RcPtr<Environment> global_env_;

    // Error prototype cache: indexed by NativeErrorType
    std::array<RcPtr<JSObject>, static_cast<size_t>(NativeErrorType::kCount)> error_protos_;
};

}  // namespace qppjs
