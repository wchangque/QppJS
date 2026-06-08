#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/gc_heap.h"
#include "qppjs/runtime/job_queue.h"
#include "qppjs/runtime/js_function.h"
#include "qppjs/runtime/js_generator.h"
#include "qppjs/runtime/js_map.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/js_regexp.h"
#include "qppjs/runtime/module_loader.h"
#include "qppjs/runtime/native_errors.h"
#include "qppjs/runtime/promise.h"
#include "qppjs/runtime/rc_object.h"
#include "qppjs/runtime/symbol_table.h"

#include <array>
#include <memory>
#include <optional>
#include <set>
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
    StmtResult eval_do_while_stmt(const DoWhileStatement& stmt,
                                  std::optional<std::string> label = std::nullopt);
    StmtResult eval_return_stmt(const ReturnStatement& stmt);
    StmtResult eval_function_decl(const FunctionDeclaration& stmt);
    StmtResult eval_throw_stmt(const ThrowStatement& stmt);
    StmtResult eval_try_stmt(const TryStatement& stmt);
    StmtResult eval_switch_stmt(const SwitchStatement& stmt);
    StmtResult eval_break_stmt(const BreakStatement& stmt);
    StmtResult eval_continue_stmt(const ContinueStatement& stmt);
    StmtResult eval_labeled_stmt(const LabeledStatement& stmt);
    StmtResult eval_for_stmt(const ForStatement& stmt,
                             std::optional<std::string> label = std::nullopt);
    StmtResult eval_for_in_stmt(const ForInStatement& stmt,
                                std::optional<std::string> label = std::nullopt);
    StmtResult eval_for_of_stmt(const ForOfStatement& stmt,
                                std::optional<std::string> label = std::nullopt);
    StmtResult exec_catch(const CatchClause& handler, Value thrown_val);
    StmtResult eval_destructuring_decl(const DestructuringDeclaration& decl);

    // Bind a destructuring pattern to a value.
    // kind: var kind for define; is_assign: true = write to existing bindings, not define new
    // Returns StmtResult (ok or throw)
    StmtResult bind_pattern(const PatternNode& pattern, Value rhs,
                            VarKind kind, bool is_assign);

    // Expression evaluation
    EvalResult eval_expr(const ExprNode& expr);
    EvalResult eval_identifier(const Identifier& expr);
    EvalResult eval_unary(const UnaryExpression& expr);
    EvalResult eval_binary(const BinaryExpression& expr);
    EvalResult eval_logical(const LogicalExpression& expr);
    EvalResult eval_conditional_expr(const ConditionalExpression& expr);
    EvalResult eval_assignment(const AssignmentExpression& expr);
    EvalResult eval_object_expr(const ObjectExpression& expr);
    EvalResult eval_member_expr(const MemberExpression& expr);
    EvalResult eval_member_assign(const MemberAssignmentExpression& expr);
    EvalResult eval_function_expr(const FunctionExpression& expr);
    EvalResult eval_arrow_function_expr(const ArrowFunctionExpression& expr);
    EvalResult eval_call_expr(const CallExpression& expr);
    EvalResult eval_new_expr(const NewExpression& expr);
    EvalResult eval_array_expr(const ArrayExpression& expr);
    EvalResult eval_async_function_expr(const AsyncFunctionExpression& expr);
    EvalResult eval_await_expr(const AwaitExpression& expr);
    EvalResult eval_update_expr(const UpdateExpression& expr);
    EvalResult eval_import_call(const ImportCallExpression& expr);
    EvalResult eval_template_literal(const TemplateLiteral& expr);
    EvalResult eval_tagged_template_expr(const TaggedTemplateExpression& expr);
    EvalResult eval_regex_literal(const RegexLiteral& expr);
    EvalResult eval_optional_chain(const OptionalChainExpression& expr);
    EvalResult eval_yield_expr(const YieldExpression& expr);
    // Class support
    EvalResult eval_class_common(const std::optional<std::unique_ptr<ExprNode>>& super_class,
                                 const std::vector<ClassMethod>& methods,
                                 const std::vector<ClassField>& fields,
                                 const std::optional<std::string>& class_name);
    EvalResult eval_class_expr(const ClassExpression& expr);
    EvalResult eval_class_decl(const ClassDeclaration& stmt);

    // Initialize instance fields on this_val; called at constructor entry (base) or after super() (derived)
    EvalResult init_instance_fields(JSFunction* ctor_fn, Value& this_val);
    EvalResult eval_super_call(const SuperCallExpression& expr);
    EvalResult eval_super_member(const SuperMemberExpression& expr);
    // Helper: property access on a pre-evaluated object value
    EvalResult eval_get_property_of(const Value& obj, const Value& key_val);

    // Create a JSRegExp from pattern/flags. Returns error on invalid flags or pattern.
    EvalResult make_regexp(const std::string& pattern, const std::string& flags);

    // Execute RegExp exec() on input string. Returns result array or null.
    EvalResult regexp_exec(JSRegExp* rx, const std::string& input);

    // Type conversions (static)
    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    // Returns empty string on Symbol (caller must handle TypeError separately if needed).
    static std::string to_string_val(const Value& v);
    // Returns symbol description string for toString() ("Symbol(desc)").
    static std::string symbol_to_string(uint64_t id, const SymbolTable& table);

    // Statement execution for async functions
    StmtResult eval_async_function_decl(const AsyncFunctionDeclaration& stmt);

    // Generator helpers: execute generator body from suspended state
    EvalResult generator_next(RcPtr<JSGeneratorObject> gen, Value resume_val);
    EvalResult generator_return(RcPtr<JSGeneratorObject> gen, Value return_val);
    EvalResult generator_throw(RcPtr<JSGeneratorObject> gen, Value throw_val);

    // Execute one iteration of generator body, returning {value, done} result object.
    EvalResult generator_resume(RcPtr<JSGeneratorObject> gen);

    // Async generator: resume the generator body, resolving outer_promise when yield/done is reached.
    // When await is encountered, sets up resume callback and returns (outer_promise stays pending).
    void ag_resume(RcPtr<JSGeneratorObject> gen, RcPtr<JSPromise> outer_promise);

    // Hoist var declarations; var_target is the function-level env to receive var bindings.
    void hoist_vars(const std::vector<StmtNode>& stmts, Environment& var_target);
    void hoist_vars_stmt(const StmtNode& stmt, Environment& var_target);

    // Module-specific hoisting: skip exported let/const (already defined by Link phase).
    void hoist_module_vars(const std::vector<StmtNode>& stmts, Environment& module_env);

    // Create a JSFunction value with eager prototype initialization.
    Value make_function_value(std::optional<std::string> name, const std::vector<ParamDef>& params,
                              std::shared_ptr<std::vector<StmtNode>> body,
                              RcPtr<Environment> closure_env,
                              bool is_named_expr = false,
                              std::optional<std::string> rest_param = std::nullopt);

    // Create an async JSFunction value (wraps call in Promise).
    Value make_async_function_value(std::optional<std::string> name, const std::vector<ParamDef>& params,
                                    std::shared_ptr<std::vector<StmtNode>> body,
                                    RcPtr<Environment> closure_env,
                                    std::optional<std::string> rest_param = std::nullopt);

    Value make_async_generator_value(std::optional<std::string> name, const std::vector<ParamDef>& params,
                                     std::shared_ptr<std::vector<StmtNode>> body,
                                     RcPtr<Environment> closure_env,
                                     std::optional<std::string> rest_param = std::nullopt);

    // Spread all elements of `iterable` into `out`. Returns true on success;
    // on failure, sets pending_throw_ and returns false.
    bool spread_into(const Value& iterable, std::vector<Value>& out);

    // JSON helpers
    // Returns false if value should be omitted (undefined/function/symbol).
    // Sets pending_throw_ on circular reference error.
    bool json_stringify_value(const Value& val, std::string& out, std::set<RcObject*>& seen);
    EvalResult json_parse_value(const std::string& text, size_t& pos);

    // Execute async body from stmt_index. On suspend, enqueues resume/reject via PerformThen.
    // On completion, fulfills/rejects outer_promise.
    void run_async_body(std::shared_ptr<std::vector<StmtNode>> body, size_t stmt_index,
                        RcPtr<Environment> fn_env, Value this_val,
                        RcPtr<JSPromise> outer_promise);

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
    SymbolTable symbol_table_;

    RcPtr<Environment> global_env_;
    RcPtr<Environment> current_env_;
    RcPtr<Environment> var_env_;  // current function-level var scope
    Value current_this_;                    // current this binding
    RcPtr<JSObject> object_prototype_;   // global Object.prototype
    RcPtr<JSObject> array_prototype_;    // Array.prototype
    RcPtr<JSObject> function_prototype_; // Function.prototype (call/apply/bind)
    RcPtr<JSObject> promise_prototype_;  // Promise.prototype (then/catch/finally)
    RcPtr<JSObject> boolean_prototype_;  // Boolean.prototype (valueOf/toString)
    RcPtr<JSObject> string_prototype_;   // String.prototype (indexOf/slice/trim/...)
    RcPtr<JSObject> math_obj_;           // Math object
    RcPtr<JSObject> number_prototype_;   // Number.prototype
    RcPtr<JSObject> regexp_prototype_;   // RegExp.prototype
    RcPtr<JSObject> symbol_prototype_;   // Symbol.prototype (toString/valueOf)
    RcPtr<JSObject> generator_prototype_;  // Generator prototype (.next/.return/.throw/[Symbol.iterator])
    RcPtr<JSObject> map_prototype_;        // Map.prototype
    RcPtr<JSObject> set_prototype_;        // Set.prototype
    RcPtr<JSObject> weakmap_prototype_;    // WeakMap.prototype
    RcPtr<JSObject> weakset_prototype_;    // WeakSet.prototype
    RcPtr<JSFunction> object_constructor_;  // global Object function
    RcPtr<JSFunction> number_constructor_;  // global Number function
    RcPtr<JSFunction> boolean_constructor_;  // global Boolean function
    RcPtr<JSFunction> string_constructor_;  // global String function
    RcPtr<JSFunction> regexp_constructor_;  // global RegExp function
    RcPtr<JSFunction> symbol_constructor_;  // global Symbol function
    uint64_t math_random_state_ = 1;    // xorshift64* PRNG state
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;
    ModuleRecord* current_module_ = nullptr;  // 当前正在执行的模块（非拥有指针）
    JSFunction* current_function_ = nullptr;  // 当前正在执行的函数（非拥有指针，用于 import.meta 词法绑定）

    // Class constructor state: new.target and derived-this-initialized tracking
    Value current_new_target_ = Value::undefined();  // new.target value
    bool derived_this_initialized_ = false;           // true after super() called in derived ctor
    Value last_new_this_ = Value::undefined();        // set by eval_super_call, read by eval_new_expr


    // Error prototype cache: indexed by NativeErrorType
    std::array<RcPtr<JSObject>, static_cast<size_t>(NativeErrorType::kCount)> error_protos_;

    // When a JS-level throw crosses an EvalResult boundary (e.g., from call_function),
    // the thrown Value is stashed here and the error message is set to kPendingThrowSentinel.
    // eval_try_stmt checks this sentinel before interpreting any EvalResult error.
    std::optional<Value> pending_throw_;
    static constexpr const char* kPendingThrowSentinel = "__qppjs_pending_throw__";

    // Sentinel returned by eval_await_expr when the async function suspends.
    // All intermediate eval_* layers propagate this upward without extra processing.
    static constexpr const char* kAsyncSuspendSentinel = "__qppjs_async_suspend__";

    // Generator yield state: set by eval_yield_expr when the generator body yields.
    static constexpr const char* kGeneratorYieldSentinel = "__qppjs_generator_yield__";
    // When true, eval_yield_expr returns resume_value_ instead of yielding.
    bool in_generator_resume_mode_ = false;
    std::optional<Value> pending_generator_resume_value_;
    std::optional<Value> pending_generator_yield_value_;
    // When true (alongside in_generator_resume_mode_), eval_yield_expr injects a throw.
    bool in_generator_throw_mode_ = false;
    std::optional<Value> pending_generator_throw_value_;
    // Live delegate iterator for an in-progress yield* (restored from gen->yield_delegate_iter_).
    Value current_yield_delegate_iter_;
    Value current_yield_delegate_next_;

    // When an async function resumes after an await, the fulfilled value is stored here
    // so that eval_await_expr can return it without re-suspending.
    std::optional<Value> pending_await_result_;

    // Set by eval_await_expr when suspending: the inner promise to await on.
    // Read by the async body runner to set up resume/reject callbacks.
    std::optional<RcPtr<JSPromise>> pending_inner_promise_;

    // Current async function context (non-owning, only valid during async body execution)
    JSPromise* current_async_promise_ = nullptr;  // outer promise for current async function
    bool in_async_body_ = false;  // true when executing inside an async function body
};

}  // namespace qppjs
