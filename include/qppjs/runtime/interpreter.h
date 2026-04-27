#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/job_queue.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/promise.h"
#include "qppjs/runtime/rc_object.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace qppjs {

class Compiler;
class JSObject;
class JSFunction;
class ModuleRecord;

class Interpreter {
public:
    Interpreter();

    EvalResult exec(const Program& program);

    // 执行入口模块文件（ESM）
    EvalResult exec_module(const std::string& entry_path);

private:
    void init_runtime();

    // Statement execution
    StmtResult eval_stmt(const StmtNode& stmt);
    StmtResult eval_expression_stmt(const ExpressionStatement& stmt);
    StmtResult eval_var_decl(const VariableDeclaration& decl);
    StmtResult eval_block_stmt(const BlockStatement& stmt);
    StmtResult eval_if_stmt(const IfStatement& stmt);
    StmtResult eval_while_stmt(const WhileStatement& stmt,
                               std::optional<std::string> label = std::nullopt);
    StmtResult eval_return_stmt(const ReturnStatement& stmt);
    StmtResult eval_function_decl(const FunctionDeclaration& stmt);
    StmtResult eval_throw_stmt(const ThrowStatement& stmt);
    StmtResult eval_try_stmt(const TryStatement& stmt);
    StmtResult eval_break_stmt(const BreakStatement& stmt);
    StmtResult eval_continue_stmt(const ContinueStatement& stmt);
    StmtResult eval_labeled_stmt(const LabeledStatement& stmt);
    StmtResult eval_for_stmt(const ForStatement& stmt,
                             std::optional<std::string> label = std::nullopt);
    StmtResult exec_catch(const CatchClause& handler, Value thrown_val);

    // Expression evaluation
    EvalResult eval_expr(const ExprNode& expr);
    EvalResult eval_identifier(const Identifier& expr);
    EvalResult eval_unary(const UnaryExpression& expr);
    EvalResult eval_binary(const BinaryExpression& expr);
    EvalResult eval_logical(const LogicalExpression& expr);
    EvalResult eval_assignment(const AssignmentExpression& expr);
    EvalResult eval_object_expr(const ObjectExpression& expr);
    EvalResult eval_member_expr(const MemberExpression& expr);
    EvalResult eval_member_assign(const MemberAssignmentExpression& expr);
    EvalResult eval_function_expr(const FunctionExpression& expr);
    EvalResult eval_call_expr(const CallExpression& expr);
    EvalResult eval_new_expr(const NewExpression& expr);
    EvalResult eval_array_expr(const ArrayExpression& expr);
    EvalResult eval_async_function_expr(const AsyncFunctionExpression& expr);
    EvalResult eval_await_expr(const AwaitExpression& expr);

    // Type conversions (static)
    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    static std::string to_string_val(const Value& v);

    // Statement execution for async functions
    StmtResult eval_async_function_decl(const AsyncFunctionDeclaration& stmt);

    // Hoist var declarations; var_target is the function-level env to receive var bindings.
    void hoist_vars(const std::vector<StmtNode>& stmts, Environment& var_target);
    void hoist_vars_stmt(const StmtNode& stmt, Environment& var_target);

    // Module-specific hoisting: skip exported let/const (already defined by Link phase).
    void hoist_module_vars(const std::vector<StmtNode>& stmts, Environment& module_env);

    // Create a JSFunction value with eager prototype initialization.
    Value make_function_value(std::optional<std::string> name, std::vector<std::string> params,
                              std::shared_ptr<std::vector<StmtNode>> body,
                              RcPtr<Environment> closure_env,
                              bool is_named_expr = false);

    // Create an async JSFunction value (wraps call in Promise).
    Value make_async_function_value(std::optional<std::string> name, std::vector<std::string> params,
                                    std::shared_ptr<std::vector<StmtNode>> body,
                                    RcPtr<Environment> closure_env);

    // Promise.resolve(value): wraps non-Promise values in a fulfilled Promise.
    RcPtr<JSPromise> promise_resolve(Value value);

    // Execute a reaction job from the job queue.
    void execute_reaction_job(ReactionJob job);

    // Drain all pending microtasks.
    void drain_job_queue();

    Value make_error_value(NativeErrorType type, const std::string& message);

    // Execute a function with given this_val and args.
    // Returns StmtResult so callers can distinguish explicit return from natural completion.
    StmtResult call_function(RcPtr<JSFunction> fn, Value this_val, std::vector<Value> args,
                             bool is_new_call = false);

    // Overload accepting span (used by forEach NativeFn to avoid heap allocation).
    EvalResult call_function_val(Value fn_val, Value this_val, std::span<Value> args);

    // RAII scope switcher; optionally increments call_depth_ and restores on destruction.
    struct ScopeGuard {
        Interpreter& interp;
        RcPtr<Environment> saved_env;
        RcPtr<Environment> saved_var_env;
        Value saved_this;
        bool owns_call_depth;
        ScopeGuard(Interpreter& i, RcPtr<Environment> new_env,
                   RcPtr<Environment> new_var_env, Value new_this,
                   bool is_call = false);
        ~ScopeGuard();
    };

    // Module: Link 阶段（DFS）
    EvalResult link_module(ModuleRecord& mod);
    // Module: Evaluate 阶段（DFS）
    EvalResult evaluate_module(ModuleRecord& mod);
    // Module: 执行模块体
    EvalResult exec_module_body(ModuleRecord& mod);

    GcHeap gc_heap_;
    ModuleLoader module_loader_;
    JobQueue job_queue_;

    RcPtr<Environment> global_env_;
    RcPtr<Environment> current_env_;
    RcPtr<Environment> var_env_;  // current function-level var scope
    Value current_this_;                    // current this binding
    RcPtr<JSObject> object_prototype_;   // global Object.prototype
    RcPtr<JSObject> array_prototype_;    // Array.prototype
    RcPtr<JSObject> function_prototype_; // Function.prototype (call/apply/bind)
    RcPtr<JSObject> promise_prototype_;  // Promise.prototype (then/catch/finally)
    RcPtr<JSFunction> object_constructor_;  // global Object function
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;
    ModuleRecord* current_module_ = nullptr;  // 当前正在执行的模块（非拥有指针）

    // Error prototype cache: indexed by NativeErrorType
    std::array<RcPtr<JSObject>, static_cast<size_t>(NativeErrorType::kCount)> error_protos_;

    // When a JS-level throw crosses an EvalResult boundary (e.g., from call_function),
    // the thrown Value is stashed here and the error message is set to kPendingThrowSentinel.
    // eval_try_stmt checks this sentinel before interpreting any EvalResult error.
    std::optional<Value> pending_throw_;
    static constexpr const char* kPendingThrowSentinel = "__qppjs_pending_throw__";

    // Current async function context (non-owning, only valid during async body execution)
    JSPromise* current_async_promise_ = nullptr;  // outer promise for current async function
    bool in_async_body_ = false;  // true when executing inside an async function body
};

}  // namespace qppjs
