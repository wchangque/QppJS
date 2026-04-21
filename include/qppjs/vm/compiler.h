#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/opcode.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qppjs {

class Compiler {
public:
    // Compile a top-level Program into a BytecodeFunction (like a top-level thunk).
    std::shared_ptr<BytecodeFunction> compile(const Program& program);

private:
    // Current function being compiled (stack for nested functions)
    BytecodeFunction* current_ = nullptr;

    // Compile a function body (params + statements). Creates a new BytecodeFunction.
    std::shared_ptr<BytecodeFunction> compile_function(std::optional<std::string> name,
                                                        const std::vector<std::string>& params,
                                                        const std::vector<StmtNode>& body);

    // Pre-scan body for var declarations and function declarations (non-recursive into nested fns).
    void hoist_vars_scan(const std::vector<StmtNode>& body);
    void hoist_vars_scan_stmt(const StmtNode& stmt);

    // Statement compilation
    void compile_stmt(const StmtNode& stmt);
    // Compile last statement leaving its value on stack for implicit return.
    void compile_stmt_last(const StmtNode& stmt);
    void compile_expr_stmt(const ExpressionStatement& stmt);
    void compile_var_decl(const VariableDeclaration& decl);
    void compile_block_stmt(const BlockStatement& stmt);
    void compile_if_stmt(const IfStatement& stmt);
    void compile_while_stmt(const WhileStatement& stmt);
    void compile_return_stmt(const ReturnStatement& stmt);
    void compile_function_decl(const FunctionDeclaration& stmt);

    // Expression compilation; always leaves exactly one value on stack
    void compile_expr(const ExprNode& expr);
    void compile_unary(const UnaryExpression& expr);
    void compile_binary(const BinaryExpression& expr);
    void compile_logical(const LogicalExpression& expr);
    void compile_assignment(const AssignmentExpression& expr);
    void compile_object_expr(const ObjectExpression& expr);
    void compile_member_expr(const MemberExpression& expr);
    void compile_member_assign(const MemberAssignmentExpression& expr);
    void compile_function_expr(const FunctionExpression& expr);
    void compile_call_expr(const CallExpression& expr);
    void compile_new_expr(const NewExpression& expr);

    // Emit helpers
    void emit(Opcode op);
    void emit_u8(uint8_t v);
    void emit_u16(uint16_t v);
    void emit_i32(int32_t v);

    // Returns the byte position of the 4-byte placeholder (for patch_jump).
    size_t emit_jump(Opcode op);
    // Patches the i32 at pos to be relative from (pos+4) to current code size.
    void patch_jump(size_t pos);

    // Pool helpers – return index (capped at uint16_t max)
    uint16_t add_constant(Value v);
    uint16_t add_name(const std::string& name);
    uint16_t add_function(std::shared_ptr<BytecodeFunction> fn);
};

}  // namespace qppjs
