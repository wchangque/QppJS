#pragma once

#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace qppjs {

class JSObject;
class JSFunction;

struct ExceptionHandler {
    size_t catch_target;  // bytecode absolute offset of the catch/finally handler
    size_t stack_depth;   // operand stack size at EnterTry time
    size_t scope_depth;   // scope depth at EnterTry time
};

struct CallFrame {
    const BytecodeFunction* bytecode = nullptr;
    size_t pc = 0;
    std::vector<Value> stack;
    std::shared_ptr<Environment> env;
    Value this_val = Value::undefined();
    bool is_new_call = false;                  // true if created by NewCall
    Value new_instance = Value::undefined();   // the new instance (only valid if is_new_call)

    // Phase 7: exception handling
    std::vector<ExceptionHandler> handler_stack;
    std::optional<Value> pending_throw;
    std::optional<Value> caught_exception;  // set by exception_handler for GetException to consume
    std::vector<size_t> finally_return_stack;  // gosub return address stack
    size_t scope_depth = 0;
};

class VM {
public:
    VM();

    EvalResult exec(std::shared_ptr<BytecodeFunction> bytecode);

private:
    void init_global_env();

    // Main dispatch loop. Runs until call_stack_.size() == exit_depth.
    EvalResult run(size_t exit_depth = 0);

    // Push a new CallFrame. Returns error if call depth exceeded.
    EvalResult push_call_frame(RcPtr<JSFunction> fn, Value this_val, std::span<Value> args,
                               bool is_new = false, Value new_instance = Value::undefined());

    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    static std::string to_string_val(const Value& v);
    static bool abstract_eq(const Value& a, const Value& b);

    // deque: push_back does not invalidate references to existing elements
    std::deque<CallFrame> call_stack_;
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;

    RcPtr<JSObject> object_prototype_;
    std::shared_ptr<Environment> global_env_;
};

}  // namespace qppjs
