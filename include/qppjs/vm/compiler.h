#pragma once

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/opcode.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qppjs {

struct LoopEnv {
    std::optional<std::string> label;
    size_t continue_target = 0;               // where continue jumps (update or loop_start)
    std::vector<size_t> break_patches;        // Jump placeholders waiting for after-loop offset
    std::vector<size_t> continue_patches;     // Jump placeholders waiting for continue_target offset
    std::vector<size_t> finally_labels;       // active finally subroutine offsets crossed by this loop
};

// Tracks an active finally block being compiled.
struct FinallyInfo {
    size_t label = 0;                    // absolute offset of the finally subroutine (0 = not yet known)
    std::vector<size_t> gosub_patches;   // Gosub i32 placeholders that need patching to label
};

class Compiler {
public:
    // Compile a top-level Program into a BytecodeFunction (like a top-level thunk).
    std::shared_ptr<BytecodeFunction> compile(const Program& program);

private:
    // Current function being compiled (stack for nested functions)
    BytecodeFunction* current_ = nullptr;

    // Reverse index for add_name deduplication: name -> index in current_->names
    std::unordered_map<std::string, uint16_t> name_index_;

    // Phase 7: loop and finally tracking
    std::vector<LoopEnv> loop_env_stack_;
    std::vector<FinallyInfo> finally_info_stack_;  // active finally blocks being compiled (innermost last)

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
    void compile_while_stmt(const WhileStatement& stmt, std::optional<std::string> label = std::nullopt);
    void compile_return_stmt(const ReturnStatement& stmt);
    void compile_function_decl(const FunctionDeclaration& stmt);
    void compile_throw_stmt(const ThrowStatement& stmt);
    void compile_try_stmt(const TryStatement& stmt);
    void compile_break_stmt(const BreakStatement& stmt);
    void compile_continue_stmt(const ContinueStatement& stmt);
    void compile_labeled_stmt(const LabeledStatement& stmt);
    void compile_for_stmt(const ForStatement& stmt, std::optional<std::string> label = std::nullopt);

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
    // Patches the i32 at pos to jump to absolute target offset.
    void patch_jump_to(size_t pos, size_t target);
    // Emits a jump to an already-known absolute target offset.
    void emit_jump_to(Opcode op, size_t target);

    // Returns the current bytecode offset (after last emitted byte).
    size_t current_offset() const;

    // Pool helpers – return index (capped at uint16_t max)
    uint16_t add_constant(Value v);
    uint16_t add_name(const std::string& name);
    uint16_t add_function(std::shared_ptr<BytecodeFunction> fn);
};

}  // namespace qppjs
