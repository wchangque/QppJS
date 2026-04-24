#include "qppjs/vm/compiler.h"

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/opcode.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qppjs {

// ============================================================
// Helpers
// ============================================================

static bool has_block_scope_decl(const std::vector<StmtNode>& stmts) {
    for (const auto& s : stmts) {
        if (const auto* decl = std::get_if<VariableDeclaration>(&s.v)) {
            if (decl->kind == VarKind::Let || decl->kind == VarKind::Const) return true;
        } else if (std::holds_alternative<FunctionDeclaration>(s.v)) {
            return true;
        }
    }
    return false;
}

// ============================================================
// Emit helpers
// ============================================================

void Compiler::emit(Opcode op) {
    current_->code.push_back(static_cast<uint8_t>(op));
}

void Compiler::emit_u8(uint8_t v) {
    current_->code.push_back(v);
}

void Compiler::emit_u16(uint16_t v) {
    current_->code.push_back(static_cast<uint8_t>(v >> 8));
    current_->code.push_back(static_cast<uint8_t>(v & 0xFF));
}

void Compiler::emit_i32(int32_t v) {
    current_->code.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> 24) & 0xFF));
    current_->code.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> 16) & 0xFF));
    current_->code.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> 8) & 0xFF));
    current_->code.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) & 0xFF));
}

size_t Compiler::emit_jump(Opcode op) {
    emit(op);
    size_t pos = current_->code.size();
    emit_i32(0);  // placeholder
    return pos;
}

void Compiler::patch_jump(size_t pos) {
    int32_t offset = static_cast<int32_t>(current_->code.size()) - static_cast<int32_t>(pos + 4);
    uint32_t u = static_cast<uint32_t>(offset);
    current_->code[pos]     = static_cast<uint8_t>((u >> 24) & 0xFF);
    current_->code[pos + 1] = static_cast<uint8_t>((u >> 16) & 0xFF);
    current_->code[pos + 2] = static_cast<uint8_t>((u >> 8) & 0xFF);
    current_->code[pos + 3] = static_cast<uint8_t>(u & 0xFF);
}

void Compiler::patch_jump_to(size_t pos, size_t target) {
    // offset = target - (pos + 4), so that VM: pc = (pos+4) + offset = target
    int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(pos + 4);
    uint32_t u = static_cast<uint32_t>(offset);
    current_->code[pos]     = static_cast<uint8_t>((u >> 24) & 0xFF);
    current_->code[pos + 1] = static_cast<uint8_t>((u >> 16) & 0xFF);
    current_->code[pos + 2] = static_cast<uint8_t>((u >> 8) & 0xFF);
    current_->code[pos + 3] = static_cast<uint8_t>(u & 0xFF);
}

void Compiler::emit_jump_to(Opcode op, size_t target) {
    emit(op);
    size_t pos = current_->code.size();
    emit_i32(0);  // placeholder
    patch_jump_to(pos, target);
}

size_t Compiler::current_offset() const {
    return current_->code.size();
}

uint16_t Compiler::add_constant(Value v) {
    current_->constants.push_back(std::move(v));
    return static_cast<uint16_t>(current_->constants.size() - 1);
}

uint16_t Compiler::add_name(const std::string& name) {
    auto it = name_index_.find(name);
    if (it != name_index_.end()) {
        return it->second;
    }
    auto idx = static_cast<uint16_t>(current_->names.size());
    current_->names.push_back(name);
    name_index_.emplace(name, idx);
    return idx;
}

uint16_t Compiler::add_function(std::shared_ptr<BytecodeFunction> fn) {
    current_->functions.push_back(std::move(fn));
    return static_cast<uint16_t>(current_->functions.size() - 1);
}

// ============================================================
// Var hoisting pre-scan
// ============================================================

void Compiler::hoist_vars_scan(const std::vector<StmtNode>& body) {
    for (const auto& stmt : body) {
        hoist_vars_scan_stmt(stmt);
    }
}

void Compiler::hoist_vars_scan_stmt(const StmtNode& stmt) {
    if (std::holds_alternative<VariableDeclaration>(stmt.v)) {
        const auto& decl = std::get<VariableDeclaration>(stmt.v);
        if (decl.kind == VarKind::Var) {
            uint16_t idx = add_name(decl.name);
            bool found = false;
            for (uint16_t vi : current_->var_decls) {
                if (vi == idx) { found = true; break; }
            }
            if (!found) current_->var_decls.push_back(idx);
        }
    } else if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
        const auto& fdecl = std::get<FunctionDeclaration>(stmt.v);
        uint16_t idx = add_name(fdecl.name);
        bool found = false;
        for (uint16_t vi : current_->var_decls) {
            if (vi == idx) { found = true; break; }
        }
        if (!found) current_->var_decls.push_back(idx);
    } else if (std::holds_alternative<BlockStatement>(stmt.v)) {
        hoist_vars_scan(std::get<BlockStatement>(stmt.v).body);
    } else if (std::holds_alternative<IfStatement>(stmt.v)) {
        const auto& if_stmt = std::get<IfStatement>(stmt.v);
        if (if_stmt.consequent) hoist_vars_scan_stmt(*if_stmt.consequent);
        if (if_stmt.alternate)  hoist_vars_scan_stmt(*if_stmt.alternate);
    } else if (std::holds_alternative<WhileStatement>(stmt.v)) {
        const auto& while_stmt = std::get<WhileStatement>(stmt.v);
        if (while_stmt.body) hoist_vars_scan_stmt(*while_stmt.body);
    } else if (std::holds_alternative<ForStatement>(stmt.v)) {
        const auto& for_stmt = std::get<ForStatement>(stmt.v);
        if (for_stmt.init.has_value()) hoist_vars_scan_stmt(**for_stmt.init);
        if (for_stmt.body) hoist_vars_scan_stmt(*for_stmt.body);
    } else if (std::holds_alternative<TryStatement>(stmt.v)) {
        const auto& try_stmt = std::get<TryStatement>(stmt.v);
        hoist_vars_scan(try_stmt.block.body);
        if (try_stmt.handler.has_value()) {
            hoist_vars_scan(try_stmt.handler->body.body);
        }
        if (try_stmt.finalizer.has_value()) {
            hoist_vars_scan(try_stmt.finalizer->body);
        }
    } else if (std::holds_alternative<LabeledStatement>(stmt.v)) {
        const auto& labeled = std::get<LabeledStatement>(stmt.v);
        if (labeled.body) hoist_vars_scan_stmt(*labeled.body);
    }
    // Do NOT recurse into FunctionDeclaration/FunctionExpression bodies
}

// ============================================================
// compile_function (core)
// ============================================================

std::shared_ptr<BytecodeFunction> Compiler::compile_function(
    std::optional<std::string> name,
    const std::vector<std::string>& params,
    const std::vector<StmtNode>& body) {

    auto fn = std::make_shared<BytecodeFunction>();
    fn->name = std::move(name);
    fn->params = params;

    // Save and switch context
    BytecodeFunction* saved = current_;
    auto saved_name_index = std::move(name_index_);
    name_index_.clear();
    current_ = fn.get();

    // Pre-scan for var declarations
    hoist_vars_scan(body);

    // Emit DefVar for all hoisted vars at function entry
    for (uint16_t idx : fn->var_decls) {
        emit(Opcode::kDefVar);
        emit_u16(idx);
    }

    // Hoist function declarations: emit MakeFunction + SetVar at entry
    for (const auto& stmt : body) {
        if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
            const auto& fdecl = std::get<FunctionDeclaration>(stmt.v);
            auto child = compile_function(fdecl.name, fdecl.params, *fdecl.body);
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(fdecl.name);
            emit(Opcode::kSetVar);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        }
    }

    // Compile all statements; leave the last value on stack for implicit return.
    if (body.empty()) {
        emit(Opcode::kReturnUndefined);
    } else {
        for (size_t i = 0; i < body.size() - 1; ++i) {
            compile_stmt(body[i]);
        }
        compile_stmt_last(body.back());
        emit(Opcode::kReturn);
    }

    current_ = saved;
    name_index_ = std::move(saved_name_index);
    return fn;
}

// ============================================================
// Public entry point
// ============================================================

std::shared_ptr<BytecodeFunction> Compiler::compile(const Program& program) {
    return compile_function(std::nullopt, {}, program.body);
}

// ============================================================
// Statement compilation
// ============================================================

void Compiler::compile_stmt(const StmtNode& stmt) {
    std::visit(
        overloaded{
            [this](const ExpressionStatement& s) { compile_expr_stmt(s); },
            [this](const VariableDeclaration& s) { compile_var_decl(s); },
            [this](const BlockStatement& s) { compile_block_stmt(s); },
            [this](const IfStatement& s) { compile_if_stmt(s); },
            [this](const WhileStatement& s) { compile_while_stmt(s); },
            [this](const ReturnStatement& s) { compile_return_stmt(s); },
            [this](const FunctionDeclaration& s) { compile_function_decl(s); },
            [this](const ThrowStatement& s) { compile_throw_stmt(s); },
            [this](const TryStatement& s) { compile_try_stmt(s); },
            [this](const BreakStatement& s) { compile_break_stmt(s); },
            [this](const ContinueStatement& s) { compile_continue_stmt(s); },
            [this](const LabeledStatement& s) { compile_labeled_stmt(s); },
            [this](const ForStatement& s) { compile_for_stmt(s); },
        },
        stmt.v);
}

void Compiler::compile_expr_stmt(const ExpressionStatement& stmt) {
    compile_expr(stmt.expr);
    emit(Opcode::kPop);
}

void Compiler::compile_stmt_last(const StmtNode& stmt) {
    // For the last statement in a function/program body:
    // - ExpressionStatement: compile expr without Pop (leave value on stack)
    // - BlockStatement: compile preceding stmts normally, then compile_stmt_last on last inner stmt
    // - All other statements: compile normally, then push undefined as the "result"
    if (std::holds_alternative<ExpressionStatement>(stmt.v)) {
        compile_expr(std::get<ExpressionStatement>(stmt.v).expr);
    } else if (std::holds_alternative<BlockStatement>(stmt.v)) {
        const auto& block = std::get<BlockStatement>(stmt.v);
        bool need_scope = has_block_scope_decl(block.body);
        if (need_scope) emit(Opcode::kPushScope);
        if (block.body.empty()) {
            emit(Opcode::kLoadUndefined);
        } else {
            for (size_t i = 0; i < block.body.size() - 1; ++i) {
                compile_stmt(block.body[i]);
            }
            compile_stmt_last(block.body.back());
        }
        if (need_scope) emit(Opcode::kPopScope);
    } else {
        // Other statements don't produce a value; compile normally and push undefined
        compile_stmt(stmt);
        emit(Opcode::kLoadUndefined);
    }
}

void Compiler::compile_var_decl(const VariableDeclaration& decl) {
    if (decl.kind == VarKind::Var) {
        // var: binding already defined by DefVar at entry; just assign if initializer
        if (decl.init.has_value()) {
            compile_expr(decl.init.value());
            uint16_t idx = add_name(decl.name);
            emit(Opcode::kSetVar);
            emit_u16(idx);
            emit(Opcode::kPop);
        }
    } else {
        // let / const: define binding, then optionally initialize
        uint16_t idx = add_name(decl.name);
        if (decl.kind == VarKind::Let) {
            emit(Opcode::kDefLet);
            emit_u16(idx);
        } else {
            emit(Opcode::kDefConst);
            emit_u16(idx);
        }
        if (decl.init.has_value()) {
            compile_expr(decl.init.value());
        } else {
            emit(Opcode::kLoadUndefined);
        }
        emit(Opcode::kInitVar);
        emit_u16(idx);
        emit(Opcode::kPop);
    }
}

void Compiler::compile_block_stmt(const BlockStatement& stmt) {
    bool need_scope = has_block_scope_decl(stmt.body);
    if (need_scope) emit(Opcode::kPushScope);
    for (const auto& s : stmt.body) {
        compile_stmt(s);
    }
    if (need_scope) emit(Opcode::kPopScope);
}

void Compiler::compile_if_stmt(const IfStatement& stmt) {
    compile_expr(stmt.test);
    size_t patch1 = emit_jump(Opcode::kJumpIfFalse);

    compile_stmt(*stmt.consequent);

    if (stmt.alternate) {
        size_t patch2 = emit_jump(Opcode::kJump);
        patch_jump(patch1);
        compile_stmt(*stmt.alternate);
        patch_jump(patch2);
    } else {
        patch_jump(patch1);
    }
}

void Compiler::compile_while_stmt(const WhileStatement& stmt, std::optional<std::string> label) {
    size_t loop_start = current_offset();
    compile_expr(stmt.test);
    size_t exit_patch = emit_jump(Opcode::kJumpIfFalse);

    loop_env_stack_.push_back({label, 0, {}, {}, {}});

    compile_stmt(*stmt.body);

    // continue target = loop_start
    size_t continue_target = loop_start;
    loop_env_stack_.back().continue_target = continue_target;
    for (size_t p : loop_env_stack_.back().continue_patches) {
        patch_jump_to(p, continue_target);
    }

    emit_jump_to(Opcode::kJump, loop_start);

    size_t after_loop = current_offset();
    patch_jump_to(exit_patch, after_loop);
    for (size_t p : loop_env_stack_.back().break_patches) {
        patch_jump_to(p, after_loop);
    }

    loop_env_stack_.pop_back();
}

void Compiler::compile_return_stmt(const ReturnStatement& stmt) {
    if (!finally_info_stack_.empty()) {
        // There are active finally blocks that must run before the function exits.
        // Push the return value onto the stack first (kReturn always pops one value).
        if (stmt.argument.has_value()) {
            compile_expr(stmt.argument.value());
        } else {
            emit(Opcode::kLoadUndefined);
        }
        // For each active finally (innermost first), emit LeaveTry + Gosub.
        // The Gosub placeholder is recorded in gosub_patches; compile_try_stmt patches it
        // once the finally subroutine label is known.
        for (auto fi_it = finally_info_stack_.rbegin(); fi_it != finally_info_stack_.rend(); ++fi_it) {
            emit(Opcode::kLeaveTry);
            size_t gosub_pos = emit_jump(Opcode::kGosub);
            fi_it->gosub_patches.push_back(gosub_pos);
        }
        emit(Opcode::kReturn);
    } else if (stmt.argument.has_value()) {
        compile_expr(stmt.argument.value());
        emit(Opcode::kReturn);
    } else {
        emit(Opcode::kReturnUndefined);
    }
}

void Compiler::compile_function_decl(const FunctionDeclaration& stmt) {
    // Function declarations are already hoisted at function entry; skip here.
    // (Nothing to emit at the declaration site.)
}

// ============================================================
// Expression compilation
// ============================================================

void Compiler::compile_expr(const ExprNode& expr) {
    std::visit(
        overloaded{
            [this](const NumberLiteral& e) {
                uint16_t idx = add_constant(Value::number(e.value));
                emit(Opcode::kLoadNumber);
                emit_u16(idx);
            },
            [this](const StringLiteral& e) {
                uint16_t idx = add_constant(Value::string(e.value));
                emit(Opcode::kLoadString);
                emit_u16(idx);
            },
            [this](const BooleanLiteral& e) {
                emit(e.value ? Opcode::kLoadTrue : Opcode::kLoadFalse);
            },
            [this](const NullLiteral&) {
                emit(Opcode::kLoadNull);
            },
            [this](const Identifier& e) {
                if (e.name == "this") {
                    emit(Opcode::kLoadThis);
                } else {
                    uint16_t idx = add_name(e.name);
                    emit(Opcode::kGetVar);
                    emit_u16(idx);
                }
            },
            [this](const UnaryExpression& e) { compile_unary(e); },
            [this](const BinaryExpression& e) { compile_binary(e); },
            [this](const LogicalExpression& e) { compile_logical(e); },
            [this](const AssignmentExpression& e) { compile_assignment(e); },
            [this](const ObjectExpression& e) { compile_object_expr(e); },
            [this](const MemberExpression& e) { compile_member_expr(e); },
            [this](const MemberAssignmentExpression& e) { compile_member_assign(e); },
            [this](const FunctionExpression& e) { compile_function_expr(e); },
            [this](const CallExpression& e) { compile_call_expr(e); },
            [this](const NewExpression& e) { compile_new_expr(e); },
        },
        expr.v);
}

void Compiler::compile_unary(const UnaryExpression& expr) {
    switch (expr.op) {
    case UnaryOp::Minus:
        compile_expr(*expr.operand);
        emit(Opcode::kNeg);
        break;
    case UnaryOp::Plus:
        compile_expr(*expr.operand);
        emit(Opcode::kPos);
        break;
    case UnaryOp::Bang:
        compile_expr(*expr.operand);
        emit(Opcode::kNot);
        break;
    case UnaryOp::Typeof:
        // Special case: typeof identifier → TypeofVar (no ReferenceError for undeclared)
        if (std::holds_alternative<Identifier>(expr.operand->v)) {
            const auto& id = std::get<Identifier>(expr.operand->v);
            if (id.name != "this") {
                uint16_t idx = add_name(id.name);
                emit(Opcode::kTypeofVar);
                emit_u16(idx);
                break;
            }
        }
        compile_expr(*expr.operand);
        emit(Opcode::kTypeof);
        break;
    case UnaryOp::Void:
        compile_expr(*expr.operand);
        emit(Opcode::kPop);
        emit(Opcode::kLoadUndefined);
        break;
    }
}

void Compiler::compile_binary(const BinaryExpression& expr) {
    compile_expr(*expr.left);
    compile_expr(*expr.right);
    switch (expr.op) {
    case BinaryOp::Add:     emit(Opcode::kAdd);       break;
    case BinaryOp::Sub:     emit(Opcode::kSub);       break;
    case BinaryOp::Mul:     emit(Opcode::kMul);       break;
    case BinaryOp::Div:     emit(Opcode::kDiv);       break;
    case BinaryOp::Mod:     emit(Opcode::kMod);       break;
    case BinaryOp::Lt:      emit(Opcode::kLt);        break;
    case BinaryOp::LtEq:    emit(Opcode::kLtEq);      break;
    case BinaryOp::Gt:      emit(Opcode::kGt);        break;
    case BinaryOp::GtEq:    emit(Opcode::kGtEq);      break;
    case BinaryOp::EqEq:    emit(Opcode::kEq);        break;
    case BinaryOp::NotEq:   emit(Opcode::kNEq);       break;
    case BinaryOp::EqEqEq:  emit(Opcode::kStrictEq);  break;
    case BinaryOp::NotEqEq: emit(Opcode::kStrictNEq); break;
    }
}

void Compiler::compile_logical(const LogicalExpression& expr) {
    compile_expr(*expr.left);
    emit(Opcode::kDup);
    if (expr.op == LogicalOp::And) {
        size_t patch = emit_jump(Opcode::kJumpIfFalse);
        emit(Opcode::kPop);
        compile_expr(*expr.right);
        patch_jump(patch);
    } else {
        // Or
        size_t patch = emit_jump(Opcode::kJumpIfTrue);
        emit(Opcode::kPop);
        compile_expr(*expr.right);
        patch_jump(patch);
    }
}

void Compiler::compile_assignment(const AssignmentExpression& expr) {
    uint16_t idx = add_name(expr.target);

    if (expr.op == AssignOp::Assign) {
        compile_expr(*expr.value);
    } else {
        // Compound assignment: read current value, compute, write back
        emit(Opcode::kGetVar);
        emit_u16(idx);
        compile_expr(*expr.value);
        switch (expr.op) {
        case AssignOp::AddAssign: emit(Opcode::kAdd); break;
        case AssignOp::SubAssign: emit(Opcode::kSub); break;
        case AssignOp::MulAssign: emit(Opcode::kMul); break;
        case AssignOp::DivAssign: emit(Opcode::kDiv); break;
        case AssignOp::ModAssign: emit(Opcode::kMod); break;
        default: break;
        }
    }
    // SetVar: pop value, write to env, push value back
    emit(Opcode::kSetVar);
    emit_u16(idx);
}

void Compiler::compile_object_expr(const ObjectExpression& expr) {
    emit(Opcode::kNewObject);
    for (const auto& prop : expr.properties) {
        emit(Opcode::kDup);  // dup obj reference
        compile_expr(*prop.value);
        uint16_t name_idx = add_name(prop.key);
        emit(Opcode::kSetProp);
        emit_u16(name_idx);
        // SetProp pops val and obj, pushes val back. But we need the obj.
        // Wait — design says: SetProp pops val+obj, writes, pushes val back.
        // That means after SetProp we have val on stack (not obj).
        // But we need obj for next property. So we must pop val, leaving nothing...
        // Actually we need to rethink the object construction pattern.
        // Let's use: Dup obj → push obj on stack, then:
        //   compile_value → stack: obj | val
        //   SetProp(key)  → pops val+obj, pushes val
        //   Pop           → discard val, obj is gone too
        // But we Dup'd obj first, so we have the original obj below.
        // Actually: after Dup, stack is: [obj, obj]. After compile_value: [obj, obj, val].
        // SetProp pops val+obj, pushes val. Stack becomes: [obj, val].
        // Pop: [obj]. Good — the original obj reference remains.
        emit(Opcode::kPop);
    }
    // stack: obj (the constructed object)
}

void Compiler::compile_member_expr(const MemberExpression& expr) {
    compile_expr(*expr.object);
    if (expr.computed) {
        compile_expr(*expr.property);
        emit(Opcode::kGetElem);
    } else {
        // Property is a StringLiteral (dot access)
        const auto& prop_str = std::get<StringLiteral>(expr.property->v);
        uint16_t idx = add_name(prop_str.value);
        emit(Opcode::kGetProp);
        emit_u16(idx);
    }
}

void Compiler::compile_member_assign(const MemberAssignmentExpression& expr) {
    compile_expr(*expr.object);
    compile_expr(*expr.value);
    if (expr.computed) {
        compile_expr(*expr.property);
        emit(Opcode::kSetElem);
    } else {
        const auto& prop_str = std::get<StringLiteral>(expr.property->v);
        uint16_t idx = add_name(prop_str.value);
        emit(Opcode::kSetProp);
        emit_u16(idx);
    }
    // SetProp/SetElem pops val+obj, pushes val. Result (val) stays on stack.
}

void Compiler::compile_function_expr(const FunctionExpression& expr) {
    auto child = compile_function(expr.name, expr.params, *expr.body);
    uint16_t fn_idx = add_function(std::move(child));
    emit(Opcode::kMakeFunction);
    emit_u16(fn_idx);
}

void Compiler::compile_call_expr(const CallExpression& expr) {
    // Check if callee is a MemberExpression (method call)
    if (std::holds_alternative<MemberExpression>(expr.callee->v)) {
        const auto& mem = std::get<MemberExpression>(expr.callee->v);
        compile_expr(*mem.object);   // push obj (receiver)
        emit(Opcode::kDup);          // dup obj; stack: [obj, obj]
        if (mem.computed) {
            compile_expr(*mem.property);
            emit(Opcode::kGetElem);  // pops dup'd obj + key, pushes method; stack: [obj, method]
        } else {
            const auto& prop_str = std::get<StringLiteral>(mem.property->v);
            uint16_t idx = add_name(prop_str.value);
            emit(Opcode::kGetProp);  // pops dup'd obj, pushes method; stack: [obj, method]
            emit_u16(idx);
        }
        for (const auto& arg : expr.arguments) {
            compile_expr(*arg);
        }
        // Stack: [obj(receiver) | method | arg0 ... argN-1]
        emit(Opcode::kCallMethod);
        emit_u8(static_cast<uint8_t>(expr.arguments.size()));
    } else {
        compile_expr(*expr.callee);
        for (const auto& arg : expr.arguments) {
            compile_expr(*arg);
        }
        emit(Opcode::kCall);
        emit_u8(static_cast<uint8_t>(expr.arguments.size()));
    }
}

void Compiler::compile_new_expr(const NewExpression& expr) {
    compile_expr(*expr.callee);
    for (const auto& arg : expr.arguments) {
        compile_expr(*arg);
    }
    emit(Opcode::kNewCall);
    emit_u8(static_cast<uint8_t>(expr.arguments.size()));
}

// ============================================================
// Phase 7: throw / try / break / continue / labeled / for
// ============================================================

void Compiler::compile_throw_stmt(const ThrowStatement& stmt) {
    compile_expr(stmt.argument);
    emit(Opcode::kThrow);
}

void Compiler::compile_try_stmt(const TryStatement& stmt) {
    bool has_catch = stmt.handler.has_value();
    bool has_finally = stmt.finalizer.has_value();

    if (has_finally) {
        // Compile the finally subroutine after the main try/catch body.
        // We use a forward jump to skip over the finally body during normal execution,
        // then Gosub to invoke it.

        if (has_catch) {
            // try + catch + finally
            //
            // EnterTry [catch_label]
            // <try block>
            // LeaveTry
            // Gosub [finally_label]
            // Jump [after_finally]
            //
            // [catch_label]:
            //   EnterTry [catch_rethrow_label]
            //   PushScope
            //   GetException
            //   DefLet [param]
            //   InitVar [param]
            //   <catch body>
            //   PopScope
            //   LeaveTry
            //   Gosub [finally_label]
            //   Jump [after_finally]
            //
            // [catch_rethrow_label]:
            //   Gosub [finally_label]
            //   Throw
            //
            // [finally_label]:
            //   <finally body>
            //   Ret
            //
            // [after_finally]:

            size_t enter_try_pos = emit_jump(Opcode::kEnterTry);
            finally_info_stack_.push_back(FinallyInfo{});
            // try block
            for (const auto& s : stmt.block.body) {
                compile_stmt(s);
            }
            // Capture gosub_patches before popping (break/continue inside try block may have
            // emitted Gosub placeholders that need patching once finally_label is known).
            std::vector<size_t> try_gosub_patches = std::move(finally_info_stack_.back().gosub_patches);
            finally_info_stack_.pop_back();

            emit(Opcode::kLeaveTry);
            size_t gosub_finally_1 = emit_jump(Opcode::kGosub);
            size_t jump_after_1 = emit_jump(Opcode::kJump);

            // [catch_label]
            size_t catch_label = current_offset();
            patch_jump_to(enter_try_pos, catch_label);

            // inner EnterTry for catch body (protect with finally on exception in catch)
            size_t enter_try_catch_pos = emit_jump(Opcode::kEnterTry);
            finally_info_stack_.push_back(FinallyInfo{});
            emit(Opcode::kPushScope);
            emit(Opcode::kGetException);
            uint16_t param_idx = add_name(stmt.handler->param);
            emit(Opcode::kDefLet);
            emit_u16(param_idx);
            emit(Opcode::kInitVar);
            emit_u16(param_idx);
            emit(Opcode::kPop);
            for (const auto& s : stmt.handler->body.body) {
                compile_stmt(s);
            }
            emit(Opcode::kPopScope);
            std::vector<size_t> catch_gosub_patches = std::move(finally_info_stack_.back().gosub_patches);
            finally_info_stack_.pop_back();
            emit(Opcode::kLeaveTry);
            size_t gosub_finally_2 = emit_jump(Opcode::kGosub);
            size_t jump_after_2 = emit_jump(Opcode::kJump);

            // [catch_rethrow_label]
            size_t catch_rethrow_label = current_offset();
            patch_jump_to(enter_try_catch_pos, catch_rethrow_label);
            size_t gosub_finally_3 = emit_jump(Opcode::kGosub);
            // After Gosub, finally_return_stack is empty — kRet will restore pending_throw.
            emit(Opcode::kRet);

            // [finally_label] — the subroutine
            size_t finally_label = current_offset();
            patch_jump_to(gosub_finally_1, finally_label);
            patch_jump_to(gosub_finally_2, finally_label);
            patch_jump_to(gosub_finally_3, finally_label);
            // Patch Gosub placeholders from break/continue inside try and catch blocks
            for (size_t p : try_gosub_patches) patch_jump_to(p, finally_label);
            for (size_t p : catch_gosub_patches) patch_jump_to(p, finally_label);

            for (const auto& s : stmt.finalizer->body) {
                compile_stmt(s);
            }
            emit(Opcode::kRet);

            // [after_finally]
            size_t after_finally = current_offset();
            patch_jump_to(jump_after_1, after_finally);
            patch_jump_to(jump_after_2, after_finally);

        } else {
            // try + finally (no catch)
            //
            // EnterTry [finally_handler_label]
            // <try block>
            // LeaveTry
            // Gosub [finally_label]
            // Jump [after_finally]
            //
            // [finally_handler_label]:
            //   Gosub [finally_label]
            //   Throw
            //
            // [finally_label]:
            //   <finally body>
            //   Ret
            //
            // [after_finally]:

            size_t enter_try_pos = emit_jump(Opcode::kEnterTry);
            finally_info_stack_.push_back(FinallyInfo{});
            for (const auto& s : stmt.block.body) {
                compile_stmt(s);
            }
            std::vector<size_t> try_gosub_patches = std::move(finally_info_stack_.back().gosub_patches);
            finally_info_stack_.pop_back();
            emit(Opcode::kLeaveTry);
            size_t gosub_finally_1 = emit_jump(Opcode::kGosub);
            size_t jump_after = emit_jump(Opcode::kJump);

            // [finally_handler_label]
            size_t finally_handler_label = current_offset();
            patch_jump_to(enter_try_pos, finally_handler_label);
            size_t gosub_finally_2 = emit_jump(Opcode::kGosub);
            // After Gosub, finally_return_stack is empty — kRet will restore pending_throw.
            emit(Opcode::kRet);

            // [finally_label]
            size_t finally_label = current_offset();
            patch_jump_to(gosub_finally_1, finally_label);
            patch_jump_to(gosub_finally_2, finally_label);
            for (size_t p : try_gosub_patches) patch_jump_to(p, finally_label);

            for (const auto& s : stmt.finalizer->body) {
                compile_stmt(s);
            }
            emit(Opcode::kRet);

            // [after_finally]
            size_t after_finally = current_offset();
            patch_jump_to(jump_after, after_finally);
        }

    } else {
        // try + catch (no finally)
        //
        // EnterTry [catch_label]
        // <try block>
        // LeaveTry
        // Jump [after_catch]
        //
        // [catch_label]:
        //   PushScope
        //   GetException
        //   DefLet [param]
        //   InitVar [param]
        //   <catch body>
        //   PopScope
        //
        // [after_catch]:

        size_t enter_try_pos = emit_jump(Opcode::kEnterTry);
        for (const auto& s : stmt.block.body) {
            compile_stmt(s);
        }
        emit(Opcode::kLeaveTry);
        size_t jump_after = emit_jump(Opcode::kJump);

        // [catch_label]
        size_t catch_label = current_offset();
        patch_jump_to(enter_try_pos, catch_label);
        emit(Opcode::kPushScope);
        emit(Opcode::kGetException);
        uint16_t param_idx = add_name(stmt.handler->param);
        emit(Opcode::kDefLet);
        emit_u16(param_idx);
        emit(Opcode::kInitVar);
        emit_u16(param_idx);
        emit(Opcode::kPop);
        for (const auto& s : stmt.handler->body.body) {
            compile_stmt(s);
        }
        emit(Opcode::kPopScope);

        // [after_catch]
        size_t after_catch = current_offset();
        patch_jump_to(jump_after, after_catch);
    }
}

void Compiler::compile_break_stmt(const BreakStatement& stmt) {
    for (auto it = loop_env_stack_.rbegin(); it != loop_env_stack_.rend(); ++it) {
        bool matches = !stmt.label.has_value() || (it->label == stmt.label);
        if (matches) {
            // For each active finally being crossed (innermost first), emit:
            //   LeaveTry (clear the EnterTry handler)
            //   Gosub [finally_label] (run the finally subroutine)
            for (auto fi_it = finally_info_stack_.rbegin(); fi_it != finally_info_stack_.rend(); ++fi_it) {
                emit(Opcode::kLeaveTry);
                size_t gosub_pos = emit_jump(Opcode::kGosub);
                fi_it->gosub_patches.push_back(gosub_pos);
            }
            size_t patch_pos = emit_jump(Opcode::kJump);
            it->break_patches.push_back(patch_pos);
            return;
        }
    }
    assert(false && "break: no matching loop");
}

void Compiler::compile_continue_stmt(const ContinueStatement& stmt) {
    for (auto it = loop_env_stack_.rbegin(); it != loop_env_stack_.rend(); ++it) {
        bool matches = !stmt.label.has_value() || (it->label == stmt.label);
        if (matches) {
            // Same LeaveTry + Gosub pattern as break.
            for (auto fi_it = finally_info_stack_.rbegin(); fi_it != finally_info_stack_.rend(); ++fi_it) {
                emit(Opcode::kLeaveTry);
                size_t gosub_pos = emit_jump(Opcode::kGosub);
                fi_it->gosub_patches.push_back(gosub_pos);
            }
            size_t patch_pos = emit_jump(Opcode::kJump);
            it->continue_patches.push_back(patch_pos);
            return;
        }
    }
    assert(false && "continue: no matching loop");
}

void Compiler::compile_labeled_stmt(const LabeledStatement& stmt) {
    if (std::holds_alternative<ForStatement>(stmt.body->v)) {
        compile_for_stmt(std::get<ForStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<WhileStatement>(stmt.body->v)) {
        compile_while_stmt(std::get<WhileStatement>(stmt.body->v), stmt.label);
    } else {
        compile_stmt(*stmt.body);
    }
}

void Compiler::compile_for_stmt(const ForStatement& stmt, std::optional<std::string> label) {
    emit(Opcode::kPushScope);

    if (stmt.init.has_value()) {
        compile_stmt(*stmt.init.value());
    }

    size_t loop_start = current_offset();

    size_t exit_patch = 0;
    bool has_test = stmt.test.has_value();
    if (has_test) {
        compile_expr(*stmt.test);
        exit_patch = emit_jump(Opcode::kJumpIfFalse);
    }

    loop_env_stack_.push_back({label, 0, {}, {}, {}});

    compile_stmt(*stmt.body);

    // continue target = update expression start (or loop_start if no update)
    size_t continue_target = current_offset();
    loop_env_stack_.back().continue_target = continue_target;
    for (size_t p : loop_env_stack_.back().continue_patches) {
        patch_jump_to(p, continue_target);
    }

    if (stmt.update.has_value()) {
        compile_expr(*stmt.update);
        emit(Opcode::kPop);
    }

    emit_jump_to(Opcode::kJump, loop_start);

    size_t after_loop = current_offset();
    if (has_test) {
        patch_jump_to(exit_patch, after_loop);
    }
    for (size_t p : loop_env_stack_.back().break_patches) {
        patch_jump_to(p, after_loop);
    }

    loop_env_stack_.pop_back();

    emit(Opcode::kPopScope);
}

}  // namespace qppjs
