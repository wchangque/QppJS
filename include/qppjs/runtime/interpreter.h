#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/value.h"

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

    // Type conversions (static)
    static bool to_boolean(const Value& v);
    static EvalResult to_number(const Value& v);
    static std::string to_string_val(const Value& v);

    // Hoist var declarations from top-level stmts (does not recurse into blocks).
    void hoist_vars(const std::vector<StmtNode>& stmts);

    // RAII scope switcher
    struct ScopeGuard {
        Interpreter& interp;
        Environment* saved;
        ScopeGuard(Interpreter& i, Environment* new_env);
        ~ScopeGuard();
    };

    Environment global_env_;
    Environment* current_env_;
};

}  // namespace qppjs
