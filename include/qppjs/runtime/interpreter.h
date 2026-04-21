#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/value.h"

#include <memory>
#include <string>
#include <vector>

namespace qppjs {

class Interpreter {
public:
    Interpreter();

    EvalResult exec(const Program& program);

private:
    // Statement execution
    StmtResult eval_stmt(const StmtNode& stmt);
    StmtResult eval_expression_stmt(const ExpressionStatement& stmt);
    StmtResult eval_var_decl(const VariableDeclaration& decl);
    StmtResult eval_block_stmt(const BlockStatement& stmt);
    StmtResult eval_if_stmt(const IfStatement& stmt);
    StmtResult eval_while_stmt(const WhileStatement& stmt);
    StmtResult eval_return_stmt(const ReturnStatement& stmt);
    StmtResult eval_function_decl(const FunctionDeclaration& stmt);

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

    // Type conversions (static)
    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    static std::string to_string_val(const Value& v);

    // Hoist var declarations; var_target is the function-level env to receive var bindings.
    void hoist_vars(const std::vector<StmtNode>& stmts, Environment& var_target);

    // RAII scope switcher; optionally increments call_depth_ and restores on destruction.
    struct ScopeGuard {
        Interpreter& interp;
        std::shared_ptr<Environment> saved_env;
        std::shared_ptr<Environment> saved_var_env;
        bool owns_call_depth;
        ScopeGuard(Interpreter& i, std::shared_ptr<Environment> new_env,
                   std::shared_ptr<Environment> new_var_env, bool is_call = false);
        ~ScopeGuard();
    };

    std::shared_ptr<Environment> global_env_;
    std::shared_ptr<Environment> current_env_;
    std::shared_ptr<Environment> var_env_;  // current function-level var scope
    int call_depth_ = 0;
    static constexpr int kMaxCallDepth = 500;
};

}  // namespace qppjs
