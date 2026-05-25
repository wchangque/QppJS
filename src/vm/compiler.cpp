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

static constexpr const char* kReturnTempName = "$__qppjs_return_temp__";

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
        for (uint16_t vi : current_->function_decls) {
            if (vi == idx) { found = true; break; }
        }
        if (!found) current_->function_decls.push_back(idx);
    } else if (std::holds_alternative<AsyncFunctionDeclaration>(stmt.v)) {
        // P2-C: async function declarations are hoisted like regular function declarations
        const auto& afdecl = std::get<AsyncFunctionDeclaration>(stmt.v);
        uint16_t idx = add_name(afdecl.name);
        bool found = false;
        for (uint16_t vi : current_->function_decls) {
            if (vi == idx) { found = true; break; }
        }
        if (!found) current_->function_decls.push_back(idx);
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
    } else if (std::holds_alternative<ForInStatement>(stmt.v)) {
        const auto& for_in = std::get<ForInStatement>(stmt.v);
        if (for_in.has_decl && for_in.var_kind == VarKind::Var) {
            // Hoist var binding (same as a var declaration)
            uint16_t idx = add_name(for_in.binding);
            bool found = false;
            for (uint16_t v : current_->var_decls) {
                if (v == idx) { found = true; break; }
            }
            if (!found) current_->var_decls.push_back(idx);
        }
        hoist_vars_scan_expr(*for_in.right);
        if (for_in.body) hoist_vars_scan_stmt(*for_in.body);
    } else if (std::holds_alternative<ForOfStatement>(stmt.v)) {
        const auto& for_of = std::get<ForOfStatement>(stmt.v);
        if (for_of.has_decl && for_of.var_kind == VarKind::Var) {
            uint16_t idx = add_name(for_of.binding);
            bool found = false;
            for (uint16_t v : current_->var_decls) {
                if (v == idx) { found = true; break; }
            }
            if (!found) current_->var_decls.push_back(idx);
        }
        hoist_vars_scan_expr(*for_of.right);
        if (for_of.body) hoist_vars_scan_stmt(*for_of.body);
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
    } else if (std::holds_alternative<ExportNamedDeclaration>(stmt.v)) {
        const auto& exp = std::get<ExportNamedDeclaration>(stmt.v);
        if (exp.declaration) {
            hoist_vars_scan_stmt(*exp.declaration);
        }
    }
    // Do NOT recurse into FunctionDeclaration/FunctionExpression bodies
}

void Compiler::hoist_vars_scan_expr(const ExprNode& expr) {
    std::visit(
        overloaded{
            [](const NumberLiteral&) {},
            [](const StringLiteral&) {},
            [](const BooleanLiteral&) {},
            [](const NullLiteral&) {},
            [](const Identifier&) {},
            [this](const UnaryExpression& e) {
                hoist_vars_scan_expr(*e.operand);
            },
            [this](const BinaryExpression& e) {
                hoist_vars_scan_expr(*e.left);
                hoist_vars_scan_expr(*e.right);
            },
            [this](const LogicalExpression& e) {
                hoist_vars_scan_expr(*e.left);
                hoist_vars_scan_expr(*e.right);
            },
            [this](const AssignmentExpression& e) {
                hoist_vars_scan_expr(*e.value);
            },
            [this](const ObjectExpression& e) {
                for (const auto& prop : e.properties) {
                    hoist_vars_scan_expr(*prop.value);
                }
            },
            [this](const MemberExpression& e) {
                hoist_vars_scan_expr(*e.object);
                hoist_vars_scan_expr(*e.property);
            },
            [this](const MemberAssignmentExpression& e) {
                hoist_vars_scan_expr(*e.object);
                hoist_vars_scan_expr(*e.property);
                hoist_vars_scan_expr(*e.value);
            },
            [](const FunctionExpression&) {},
            [this](const CallExpression& e) {
                hoist_vars_scan_expr(*e.callee);
                for (const auto& arg : e.arguments) {
                    hoist_vars_scan_expr(*arg);
                }
            },
            [this](const NewExpression& e) {
                hoist_vars_scan_expr(*e.callee);
                for (const auto& arg : e.arguments) {
                    hoist_vars_scan_expr(*arg);
                }
            },
            [this](const ArrayExpression& e) {
                for (const auto& elem : e.elements) {
                    if (elem.has_value()) {
                        hoist_vars_scan_expr(**elem);
                    }
                }
            },
            [this](const AwaitExpression& e) {
                hoist_vars_scan_expr(*e.argument);
            },
            [this](const UpdateExpression& e) {
                hoist_vars_scan_expr(*e.operand);
            },
            [](const AsyncFunctionExpression&) {},
            [](const MetaProperty&) {},
            [this](const ImportCallExpression& e) {
                hoist_vars_scan_expr(*e.specifier);
            },
            [](const RegexLiteral&) {},
            [this](const TemplateLiteral& e) {
                for (const auto& part : e.expressions) {
                    hoist_vars_scan_expr(*part);
                }
            },
            [](const ArrowFunctionExpression&) {},
            [this](const ConditionalExpression& e) {
                hoist_vars_scan_expr(*e.condition);
                hoist_vars_scan_expr(*e.consequent);
                hoist_vars_scan_expr(*e.alternate);
            },
            [this](const SpreadElement& e) {
                hoist_vars_scan_expr(*e.argument);
            },
        },
        expr.v);
}

// ============================================================
// compile_function (core)
// ============================================================

std::shared_ptr<BytecodeFunction> Compiler::compile_function(
    std::optional<std::string> name,
    const std::vector<std::string>& params,
    const std::vector<StmtNode>& body,
    bool is_program,
    std::optional<std::string> rest_param) {

    auto fn = std::make_shared<BytecodeFunction>();
    fn->name = std::move(name);
    fn->params = params;
    fn->rest_param = std::move(rest_param);

    // Save and switch context
    BytecodeFunction* saved = current_;
    auto saved_name_index = std::move(name_index_);
    name_index_.clear();
    auto saved_loop_env_stack = std::move(loop_env_stack_);
    loop_env_stack_.clear();
    auto saved_finally_info_stack = std::move(finally_info_stack_);
    finally_info_stack_.clear();
    current_ = fn.get();

    // Pre-scan for var declarations
    hoist_vars_scan(body);

    uint16_t return_temp_idx = add_name(kReturnTempName);
    bool found_return_temp = false;
    for (uint16_t idx : fn->var_decls) {
        if (idx == return_temp_idx) {
            found_return_temp = true;
            break;
        }
    }
    if (!found_return_temp) fn->var_decls.push_back(return_temp_idx);

    // Emit DefVar for all hoisted vars at function entry
    for (uint16_t idx : fn->var_decls) {
        emit(Opcode::kDefVar);
        emit_u16(idx);
    }

    // Hoist function declarations: emit MakeFunction + SetVar at entry
    for (const auto& stmt : body) {
        const FunctionDeclaration* fdecl_ptr = nullptr;
        const AsyncFunctionDeclaration* afdecl_ptr = nullptr;
        if (std::holds_alternative<FunctionDeclaration>(stmt.v)) {
            fdecl_ptr = &std::get<FunctionDeclaration>(stmt.v);
        } else if (std::holds_alternative<AsyncFunctionDeclaration>(stmt.v)) {
            // P2-C: async function declarations are hoisted like regular function declarations
            afdecl_ptr = &std::get<AsyncFunctionDeclaration>(stmt.v);
        } else if (const auto* exp = std::get_if<ExportNamedDeclaration>(&stmt.v)) {
            if (exp->declaration && std::holds_alternative<FunctionDeclaration>(exp->declaration->v)) {
                fdecl_ptr = &std::get<FunctionDeclaration>(exp->declaration->v);
            } else if (exp->declaration &&
                       std::holds_alternative<AsyncFunctionDeclaration>(exp->declaration->v)) {
                afdecl_ptr = &std::get<AsyncFunctionDeclaration>(exp->declaration->v);
            }
        }
        if (fdecl_ptr) {
            auto child = compile_function(fdecl_ptr->name, fdecl_ptr->params, *fdecl_ptr->body,
                                          false, fdecl_ptr->rest_param);
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(fdecl_ptr->name);
            emit(Opcode::kSetVar);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        }
        if (afdecl_ptr) {
            auto child = compile_function(afdecl_ptr->name, afdecl_ptr->params, *afdecl_ptr->body,
                                          false, afdecl_ptr->rest_param);
            child->is_async = true;
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(afdecl_ptr->name);
            emit(Opcode::kSetVar);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        }
    }

    // Compile all statements.
    if (body.empty()) {
        emit(Opcode::kReturnUndefined);
    } else if (is_program) {
        // Top-level program: leave the last value on stack for implicit return (REPL semantics).
        for (size_t i = 0; i < body.size() - 1; ++i) {
            compile_stmt(body[i]);
        }
        compile_stmt_last(body.back());
        emit(Opcode::kReturn);
        // Record last expression identifier for post-DrainAll re-read
        if (const auto* es = std::get_if<ExpressionStatement>(&body.back().v)) {
            if (const auto* id = std::get_if<Identifier>(&es->expr.v)) {
                fn->last_expr_name = id->name;
            }
        }
    } else {
        // Function body: all statements executed normally; implicit return undefined.
        for (const auto& stmt : body) {
            compile_stmt(stmt);
        }
        emit(Opcode::kReturnUndefined);
    }

    current_ = saved;
    name_index_ = std::move(saved_name_index);
    loop_env_stack_ = std::move(saved_loop_env_stack);
    finally_info_stack_ = std::move(saved_finally_info_stack);
    return fn;
}

// ============================================================
// Public entry point
// ============================================================

std::shared_ptr<BytecodeFunction> Compiler::compile(const Program& program) {
    return compile_function(std::nullopt, {}, program.body, /*is_program=*/true);
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
            [this](const AsyncFunctionDeclaration& s) { compile_async_function_decl(s); },
            [this](const ThrowStatement& s) { compile_throw_stmt(s); },
            [this](const TryStatement& s) { compile_try_stmt(s); },
            [this](const BreakStatement& s) { compile_break_stmt(s); },
            [this](const ContinueStatement& s) { compile_continue_stmt(s); },
            [this](const LabeledStatement& s) { compile_labeled_stmt(s); },
            [this](const ForStatement& s) { compile_for_stmt(s); },
            [this](const ForInStatement& s) { compile_for_in_stmt(s); },
            [this](const ForOfStatement& s) { compile_for_of_stmt(s); },
            [](const ImportDeclaration&) {
                // Link 阶段已处理，编译时 no-op
            },
            [this](const ExportNamedDeclaration& s) {
                if (s.source.has_value()) {
                    // re-export：no-op（Link 阶段已处理）
                    return;
                }
                if (s.declaration) {
                    // export let/const/var/function：正常编译声明
                    compile_stmt(*s.declaration);
                }
                // export { x, y }（无 declaration）：no-op
                // Link 阶段已将导出 Cell 注入 module_env Binding，
                // 执行时 SetVar 即可写入（Cell 共享）
            },
            [this](const ExportDefaultDeclaration& s) {
                // 编译 expression，再 emit SetExportDefault（从栈顶取值写入 default Cell）
                compile_expr(*s.expression);
                // export default function foo() {}：同时在模块作用域绑定 foo
                if (s.local_name.has_value()) {
                    emit(Opcode::kDup);
                    uint16_t idx = add_name(*s.local_name);
                    emit(Opcode::kSetVar);
                    emit_u16(idx);
                }
                emit(Opcode::kSetExportDefault);
            },
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

    loop_env_stack_.push_back({label, 0, {}, {}, {}, false, false, false, false, finally_info_stack_.size()});

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
    uint16_t return_temp_idx = add_name(kReturnTempName);
    if (stmt.argument.has_value()) {
        compile_expr(stmt.argument.value());
    } else {
        emit(Opcode::kLoadUndefined);
    }
    emit(Opcode::kSetVar);
    emit_u16(return_temp_idx);
    emit(Opcode::kPop);

    size_t unwind_finally_depth = finally_info_stack_.size();
    for (auto it = loop_env_stack_.rbegin(); it != loop_env_stack_.rend(); ++it) {
        for (size_t i = unwind_finally_depth; i > it->finally_depth_at_entry; --i) {
            emit(Opcode::kLeaveTry);
            size_t gosub_pos = emit_jump(Opcode::kGosub);
            finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
        }
        unwind_finally_depth = it->finally_depth_at_entry;
        if (it->is_for_in) {
            if (it->for_in_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kPop);
        }
        if (it->is_for_of) {
            if (it->for_of_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kLeaveTry);
            emit(Opcode::kIteratorClose);
        }
    }

    for (size_t i = unwind_finally_depth; i > 0; --i) {
        emit(Opcode::kLeaveTry);
        size_t gosub_pos = emit_jump(Opcode::kGosub);
        finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
    }
    emit(Opcode::kGetVar);
    emit_u16(return_temp_idx);
    // Clear the temp binding before returning to break potential reference cycles
    // (e.g. a returned closure capturing its own call env via $__qppjs_return_temp__).
    emit(Opcode::kLoadUndefined);
    emit(Opcode::kSetVar);
    emit_u16(return_temp_idx);
    emit(Opcode::kPop);
    emit(Opcode::kReturn);
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
            [this](const ArrayExpression& e) { compile_array_expr(e); },
            [this](const AsyncFunctionExpression& e) { compile_async_function_expr(e); },
            [this](const UpdateExpression& e) { compile_update_expr(e); },
            [this](const AwaitExpression& e) {
                compile_expr(*e.argument);
                emit(Opcode::kAwait);
            },
            [this](const MetaProperty& /*e*/) {
                emit(Opcode::kMetaProperty);
            },
            [this](const ImportCallExpression& e) { compile_import_call(e); },
            [this](const RegexLiteral& e) {
                uint16_t pattern_idx = add_constant(Value::string(e.pattern));
                uint16_t flags_idx = add_constant(Value::string(e.flags));
                emit(Opcode::kNewRegExp);
                emit_u16(pattern_idx);
                emit_u16(flags_idx);
            },
            [this](const TemplateLiteral& e) { compile_template_literal(e); },
            [this](const ArrowFunctionExpression& e) { compile_arrow_function_expr(e); },
            [this](const ConditionalExpression& e) { compile_conditional_expr(e); },
            [this](const SpreadElement& /*e*/) {
                // SpreadElement in a non-spread context is a syntax error at runtime.
                uint16_t idx = add_constant(Value::string("SyntaxError: invalid use of spread element"));
                emit(Opcode::kLoadString);
                emit_u16(idx);
                emit(Opcode::kThrow);
            },
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
    case UnaryOp::Delete:
        if (std::holds_alternative<MemberExpression>(expr.operand->v)) {
            const auto& mem = std::get<MemberExpression>(expr.operand->v);
            compile_expr(*mem.object);
            if (mem.computed) {
                compile_expr(*mem.property);
                emit(Opcode::kDeleteElem);
            } else {
                const auto& prop_lit = std::get<StringLiteral>(mem.property->v);
                uint16_t idx = add_name(prop_lit.value);
                emit(Opcode::kDeleteProp);
                emit_u16(idx);
            }
        } else if (std::holds_alternative<Identifier>(expr.operand->v)) {
            // TODO: strict mode Early Error (SyntaxError for delete of unqualified identifier)
            const auto& id = std::get<Identifier>(expr.operand->v);
            uint16_t idx = add_name(id.name);
            emit(Opcode::kDeleteVar);
            emit_u16(idx);
        } else {
            // Other expr: eval for side effects, discard, push true
            compile_expr(*expr.operand);
            emit(Opcode::kPop);
            emit(Opcode::kLoadTrue);
        }
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
    case BinaryOp::EqEqEq:    emit(Opcode::kStrictEq);  break;
    case BinaryOp::NotEqEq:   emit(Opcode::kStrictNEq); break;
    case BinaryOp::Instanceof: emit(Opcode::kInstanceof); break;
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

void Compiler::compile_conditional_expr(const ConditionalExpression& expr) {
    compile_expr(*expr.condition);
    size_t patch1 = emit_jump(Opcode::kJumpIfFalse);
    compile_expr(*expr.consequent);
    size_t patch2 = emit_jump(Opcode::kJump);
    patch_jump(patch1);
    compile_expr(*expr.alternate);
    patch_jump(patch2);
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
    auto child = compile_function(expr.name, expr.params, *expr.body, false, expr.rest_param);
    child->is_named_expr = expr.name.has_value();
    uint16_t fn_idx = add_function(std::move(child));
    emit(Opcode::kMakeFunction);
    emit_u16(fn_idx);
}

void Compiler::compile_async_function_expr(const AsyncFunctionExpression& expr) {
    auto child = compile_function(expr.name, expr.params, *expr.body, false, expr.rest_param);
    child->is_async = true;
    // P2-D: named async function expressions need self-reference binding inside the body
    child->is_named_expr = expr.name.has_value();
    uint16_t fn_idx = add_function(std::move(child));
    emit(Opcode::kMakeFunction);
    emit_u16(fn_idx);
}

void Compiler::compile_async_function_decl(const AsyncFunctionDeclaration& /*stmt*/) {
    // P2-C: async function declarations are hoisted at function entry; skip here.
    // (Nothing to emit at the declaration site.)
}

void Compiler::compile_arrow_function_expr(const ArrowFunctionExpression& expr) {
    auto child = compile_function(std::nullopt, expr.params, *expr.body_stmts, false, expr.rest_param);
    child->is_arrow = true;
    uint16_t fn_idx = add_function(std::move(child));
    emit(Opcode::kMakeFunction);
    emit_u16(fn_idx);
}

void Compiler::compile_call_expr(const CallExpression& expr) {
    bool has_spread = std::any_of(expr.arguments.begin(), expr.arguments.end(),
        [](const auto& a) { return std::holds_alternative<SpreadElement>(a->v); });

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
        if (!has_spread) {
            for (const auto& arg : expr.arguments) {
                compile_expr(*arg);
            }
            // Stack: [obj(receiver) | method | arg0 ... argN-1]
            emit(Opcode::kCallMethod);
            emit_u8(static_cast<uint8_t>(expr.arguments.size()));
        } else {
            // Stack: [obj, method] → swap → [method, obj]
            emit(Opcode::kSwap);
            emit(Opcode::kNewArray);  // [method, obj, []]
            for (const auto& arg : expr.arguments) {
                emit(Opcode::kDup);
                if (std::holds_alternative<SpreadElement>(arg->v)) {
                    compile_expr(*std::get<SpreadElement>(arg->v).argument);
                    emit(Opcode::kSpreadAppend);
                } else {
                    compile_expr(*arg);
                    emit(Opcode::kArrayAppend);
                }
            }
            // Stack: [method, obj, args_array]
            emit(Opcode::kApplyArgs);
            emit_u8(0);  // normal call
        }
    } else {
        compile_expr(*expr.callee);
        if (!has_spread) {
            for (const auto& arg : expr.arguments) {
                compile_expr(*arg);
            }
            emit(Opcode::kCall);
            emit_u8(static_cast<uint8_t>(expr.arguments.size()));
        } else {
            emit(Opcode::kLoadUndefined);  // this = undefined; stack: [func, undefined]
            emit(Opcode::kNewArray);       // stack: [func, undefined, []]
            for (const auto& arg : expr.arguments) {
                emit(Opcode::kDup);
                if (std::holds_alternative<SpreadElement>(arg->v)) {
                    compile_expr(*std::get<SpreadElement>(arg->v).argument);
                    emit(Opcode::kSpreadAppend);
                } else {
                    compile_expr(*arg);
                    emit(Opcode::kArrayAppend);
                }
            }
            // Stack: [func, undefined, args_array]
            emit(Opcode::kApplyArgs);
            emit_u8(0);  // normal call
        }
    }
}

void Compiler::compile_new_expr(const NewExpression& expr) {
    bool has_spread = std::any_of(expr.arguments.begin(), expr.arguments.end(),
        [](const auto& a) { return std::holds_alternative<SpreadElement>(a->v); });

    compile_expr(*expr.callee);
    if (!has_spread) {
        for (const auto& arg : expr.arguments) {
            compile_expr(*arg);
        }
        emit(Opcode::kNewCall);
        emit_u8(static_cast<uint8_t>(expr.arguments.size()));
    } else {
        emit(Opcode::kLoadUndefined);  // this placeholder; stack: [ctor, undefined]
        emit(Opcode::kNewArray);       // stack: [ctor, undefined, []]
        for (const auto& arg : expr.arguments) {
            emit(Opcode::kDup);
            if (std::holds_alternative<SpreadElement>(arg->v)) {
                compile_expr(*std::get<SpreadElement>(arg->v).argument);
                emit(Opcode::kSpreadAppend);
            } else {
                compile_expr(*arg);
                emit(Opcode::kArrayAppend);
            }
        }
        // Stack: [ctor, undefined, args_array]
        emit(Opcode::kApplyArgs);
        emit_u8(1);  // new call
    }
}

void Compiler::compile_array_expr(const ArrayExpression& expr) {
    // Detect if any element is a SpreadElement; if so use append mode for all elements.
    bool has_spread = false;
    for (const auto& e : expr.elements) {
        if (e.has_value() && std::holds_alternative<SpreadElement>((*e)->v)) {
            has_spread = true;
            break;
        }
    }

    emit(Opcode::kNewArray);

    if (!has_spread) {
        // Original path using SetElem with fixed indices.
        for (size_t i = 0; i < expr.elements.size(); ++i) {
            const auto& elem_opt = expr.elements[i];
            if (!elem_opt.has_value()) {
                emit(Opcode::kDup);
                emit(Opcode::kArrayHole);
            } else {
                emit(Opcode::kDup);
                compile_expr(**elem_opt);
                uint16_t idx_const = add_constant(Value::number(static_cast<double>(i)));
                emit(Opcode::kLoadNumber);
                emit_u16(idx_const);
                emit(Opcode::kSetElem);
                emit(Opcode::kPop);
            }
        }
    } else {
        // Append path: use ArrayAppend / SpreadAppend for all elements.
        for (const auto& elem_opt : expr.elements) {
            if (!elem_opt.has_value()) {
                emit(Opcode::kDup);
                emit(Opcode::kArrayHole);
            } else if (std::holds_alternative<SpreadElement>((*elem_opt)->v)) {
                const auto& sp = std::get<SpreadElement>((*elem_opt)->v);
                emit(Opcode::kDup);
                compile_expr(*sp.argument);
                emit(Opcode::kSpreadAppend);
            } else {
                emit(Opcode::kDup);
                compile_expr(**elem_opt);
                emit(Opcode::kArrayAppend);
            }
        }
    }
    // stack: arr
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
            compile_block_stmt(stmt.handler->body);
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
        compile_block_stmt(stmt.handler->body);
        emit(Opcode::kPopScope);

        // [after_catch]
        size_t after_catch = current_offset();
        patch_jump_to(jump_after, after_catch);
    }
}

void Compiler::compile_break_stmt(const BreakStatement& stmt) {
    size_t unwind_finally_depth = finally_info_stack_.size();
    for (auto it = loop_env_stack_.rbegin(); it != loop_env_stack_.rend(); ++it) {
        bool matches = !stmt.label.has_value() || (it->label == stmt.label);
        if (matches) {
            for (size_t i = unwind_finally_depth; i > it->finally_depth_at_entry; --i) {
                emit(Opcode::kLeaveTry);
                size_t gosub_pos = emit_jump(Opcode::kGosub);
                finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
            }
            unwind_finally_depth = it->finally_depth_at_entry;
            // If this is a lexical for-in, pop the per-iteration scope before breaking.
            if (it->is_for_in && it->for_in_has_scope) {
                emit(Opcode::kPopScope);
            }
            if (it->is_for_of) {
                if (it->for_of_has_scope) {
                    emit(Opcode::kPopScope);
                }
                emit(Opcode::kLeaveTry);
                emit(Opcode::kIteratorClose);
            }
            size_t patch_pos = emit_jump(Opcode::kJump);
            it->break_patches.push_back(patch_pos);
            return;
        }
        for (size_t i = unwind_finally_depth; i > it->finally_depth_at_entry; --i) {
            emit(Opcode::kLeaveTry);
            size_t gosub_pos = emit_jump(Opcode::kGosub);
            finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
        }
        unwind_finally_depth = it->finally_depth_at_entry;
        if (it->is_for_in) {
            if (it->for_in_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kPop);
        }
        if (it->is_for_of) {
            if (it->for_of_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kLeaveTry);
            emit(Opcode::kIteratorClose);
        }
    }
    assert(false && "break: no matching loop");
}

void Compiler::compile_continue_stmt(const ContinueStatement& stmt) {
    size_t unwind_finally_depth = finally_info_stack_.size();
    for (auto it = loop_env_stack_.rbegin(); it != loop_env_stack_.rend(); ++it) {
        bool matches = !stmt.label.has_value() || (it->label == stmt.label);
        if (matches) {
            for (size_t i = unwind_finally_depth; i > it->finally_depth_at_entry; --i) {
                emit(Opcode::kLeaveTry);
                size_t gosub_pos = emit_jump(Opcode::kGosub);
                finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
            }
            unwind_finally_depth = it->finally_depth_at_entry;
            // For a lexical for-in continue, label_continue already emits kPopScope before
            // jumping to label_check — no extra emit needed here.
            if (it->is_for_of && it->for_of_has_scope) {
                emit(Opcode::kPopScope);
            }
            size_t patch_pos = emit_jump(Opcode::kJump);
            it->continue_patches.push_back(patch_pos);
            return;
        }
        for (size_t i = unwind_finally_depth; i > it->finally_depth_at_entry; --i) {
            emit(Opcode::kLeaveTry);
            size_t gosub_pos = emit_jump(Opcode::kGosub);
            finally_info_stack_[i - 1].gosub_patches.push_back(gosub_pos);
        }
        unwind_finally_depth = it->finally_depth_at_entry;
        if (it->is_for_in) {
            if (it->for_in_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kPop);
        }
        if (it->is_for_of) {
            if (it->for_of_has_scope) {
                emit(Opcode::kPopScope);
            }
            emit(Opcode::kLeaveTry);
            emit(Opcode::kIteratorClose);
        }
    }
    assert(false && "continue: no matching loop");
}

void Compiler::compile_labeled_stmt(const LabeledStatement& stmt) {
    if (std::holds_alternative<ForStatement>(stmt.body->v)) {
        compile_for_stmt(std::get<ForStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<ForInStatement>(stmt.body->v)) {
        compile_for_in_stmt(std::get<ForInStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<ForOfStatement>(stmt.body->v)) {
        compile_for_of_stmt(std::get<ForOfStatement>(stmt.body->v), stmt.label);
    } else if (std::holds_alternative<WhileStatement>(stmt.body->v)) {
        compile_while_stmt(std::get<WhileStatement>(stmt.body->v), stmt.label);
    } else {
        loop_env_stack_.push_back(LoopEnv{stmt.label, 0, {}, {}, {}, false, false, false, false,
                                          finally_info_stack_.size()});
        compile_stmt(*stmt.body);
        size_t after_block = current_offset();
        for (size_t p : loop_env_stack_.back().break_patches) {
            patch_jump_to(p, after_block);
        }
        loop_env_stack_.pop_back();
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

    loop_env_stack_.push_back({label, 0, {}, {}, {}, false, false, false, false, finally_info_stack_.size()});

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

void Compiler::compile_for_in_stmt(const ForInStatement& stmt, std::optional<std::string> label) {
    bool need_scope = stmt.has_decl && stmt.var_kind != VarKind::Var;
    uint16_t name_idx = add_name(stmt.binding);

    // Evaluate RHS in outer scope and build iterator.
    compile_expr(*stmt.right);
    emit(Opcode::kForInStart);
    // Jump forward to label_check (first condition test).
    size_t jump_to_check = emit_jump(Opcode::kJump);

    // label_body_start: entered each iteration when done=false.
    // Stack on entry: [iter, key]
    size_t label_body_start = current_offset();

    if (need_scope) {
        // Per-iteration scope: each iteration gets its own environment for let/const.
        emit(Opcode::kPushScope);
        emit(stmt.var_kind == VarKind::Let ? Opcode::kDefLet : Opcode::kDefConst);
        emit_u16(name_idx);
        emit(Opcode::kInitVar);
        emit_u16(name_idx);
        emit(Opcode::kPop);  // kInitVar pushes the value back; discard it
    } else {
        // var or no_decl: assign key to binding.
        emit(Opcode::kSetVar);
        emit_u16(name_idx);
        emit(Opcode::kPop);
    }

    // Push LoopEnv; break/continue handlers consult for_in_has_scope to emit kPopScope.
    loop_env_stack_.push_back({label, 0, {}, {}, {}, /*is_for_in=*/true, /*for_in_has_scope=*/need_scope,
                               /*is_for_of=*/false, /*for_of_has_scope=*/false,
                               /*finally_depth_at_entry=*/finally_info_stack_.size()});
    compile_stmt(*stmt.body);

    // Normal end of iteration: pop per-iteration scope and jump to condition check.
    if (need_scope) {
        emit(Opcode::kPopScope);
    }
    size_t jump_body_to_check = emit_jump(Opcode::kJump);  // → label_check (patched below)

    // label_continue: continue patches target here.
    // Must pop the per-iteration scope before re-testing the condition.
    size_t label_continue = current_offset();
    loop_env_stack_.back().continue_target = label_continue;
    for (size_t p : loop_env_stack_.back().continue_patches) {
        patch_jump_to(p, label_continue);
    }
    if (need_scope) {
        emit(Opcode::kPopScope);
    }
    size_t jump_cont_to_check = emit_jump(Opcode::kJump);  // → label_check (patched below)

    // label_check: test whether iteration is done.
    // Stack on entry: [iter]
    size_t label_check = current_offset();
    patch_jump_to(jump_to_check, label_check);
    patch_jump_to(jump_body_to_check, label_check);
    patch_jump_to(jump_cont_to_check, label_check);

    emit(Opcode::kForInNext);
    // done=false → jump back to label_body_start; stack leaves as [iter, key]
    emit_jump_to(Opcode::kJumpIfFalse, label_body_start);
    // done=true: pop undefined key; stack: [iter]
    emit(Opcode::kPop);

    // label_break: break patches (and done=true fall-through) land here.
    // Stack on entry: [iter]
    size_t label_break = current_offset();
    for (size_t p : loop_env_stack_.back().break_patches) {
        patch_jump_to(p, label_break);
    }
    loop_env_stack_.pop_back();

    // Pop the iterator.
    emit(Opcode::kPop);
}

void Compiler::compile_for_of_stmt(const ForOfStatement& stmt, std::optional<std::string> label) {
    bool need_scope = stmt.has_decl && stmt.var_kind != VarKind::Var;
    uint16_t name_idx = add_name(stmt.binding);

    compile_expr(*stmt.right);
    emit(Opcode::kForOfStart);
    size_t enter_try_pos = emit_jump(Opcode::kEnterTry);
    size_t jump_to_check = emit_jump(Opcode::kJump);

    size_t label_body_start = current_offset();

    if (need_scope) {
        emit(Opcode::kPushScope);
        emit(stmt.var_kind == VarKind::Let ? Opcode::kDefLet : Opcode::kDefConst);
        emit_u16(name_idx);
        emit(Opcode::kInitVar);
        emit_u16(name_idx);
        emit(Opcode::kPop);
    } else {
        emit(Opcode::kSetVar);
        emit_u16(name_idx);
        emit(Opcode::kPop);
    }

    loop_env_stack_.push_back(
        {label, 0, {}, {}, {}, /*is_for_in=*/false, /*for_in_has_scope=*/false,
         /*is_for_of=*/true, /*for_of_has_scope=*/need_scope,
         /*finally_depth_at_entry=*/finally_info_stack_.size()});
    compile_stmt(*stmt.body);

    if (need_scope) {
        emit(Opcode::kPopScope);
    }
    size_t jump_body_to_check = emit_jump(Opcode::kJump);

    size_t label_continue = current_offset();
    loop_env_stack_.back().continue_target = label_continue;
    for (size_t p : loop_env_stack_.back().continue_patches) {
        patch_jump_to(p, label_continue);
    }
    size_t jump_cont_to_check = emit_jump(Opcode::kJump);

    size_t label_check = current_offset();
    patch_jump_to(jump_to_check, label_check);
    patch_jump_to(jump_body_to_check, label_check);
    patch_jump_to(jump_cont_to_check, label_check);

    emit(Opcode::kForOfNext);
    emit_jump_to(Opcode::kJumpIfFalse, label_body_start);
    emit(Opcode::kPop);
    emit(Opcode::kLeaveTry);
    emit(Opcode::kIteratorClose);
    size_t jump_after_loop = emit_jump(Opcode::kJump);

    size_t exception_handler = current_offset();
    patch_jump_to(enter_try_pos, exception_handler);
    emit(Opcode::kGetException);
    emit(Opcode::kIteratorCloseAbnormal);
    emit(Opcode::kThrow);

    size_t after_loop = current_offset();
    patch_jump_to(jump_after_loop, after_loop);
    for (size_t p : loop_env_stack_.back().break_patches) {
        patch_jump_to(p, after_loop);
    }
    loop_env_stack_.pop_back();
}

void Compiler::compile_update_expr(const UpdateExpression& expr) {
    auto is_member = std::holds_alternative<MemberExpression>(expr.operand->v);

    if (is_member) {
        const auto& member = std::get<MemberExpression>(expr.operand->v);

        if (member.computed) {
            // Element update: compile object, compile key
            compile_expr(*member.object);
            compile_expr(*member.property);
            // Emit appropriate elem opcode
            if (expr.op == UpdateOp::Inc) {
                emit(expr.prefix ? Opcode::kElemPreInc : Opcode::kElemPostInc);
            } else {
                emit(expr.prefix ? Opcode::kElemPreDec : Opcode::kElemPostDec);
            }
        } else {
            // Property update: compile object, emit prop opcode with name_idx
            compile_expr(*member.object);
            const auto& prop = std::get<StringLiteral>(member.property->v);
            uint16_t name_idx = add_name(prop.value);
            if (expr.op == UpdateOp::Inc) {
                emit(expr.prefix ? Opcode::kPropPreInc : Opcode::kPropPostInc);
            } else {
                emit(expr.prefix ? Opcode::kPropPreDec : Opcode::kPropPostDec);
            }
            emit_u16(name_idx);
        }
    } else {
        // Variable update: emit var opcode with name_idx
        const auto& ident = std::get<Identifier>(expr.operand->v);
        uint16_t name_idx = add_name(ident.name);
        if (expr.op == UpdateOp::Inc) {
            emit(expr.prefix ? Opcode::kVarPreInc : Opcode::kVarPostInc);
        } else {
            emit(expr.prefix ? Opcode::kVarPreDec : Opcode::kVarPostDec);
        }
        emit_u16(name_idx);
    }
}

void Compiler::compile_import_call(const ImportCallExpression& expr) {
    compile_expr(*expr.specifier);  // push specifier string onto stack
    emit(Opcode::kImportCall);      // pops specifier, pushes Promise
}

void Compiler::compile_template_literal(const TemplateLiteral& expr) {
    const auto& quasis = expr.quasis;
    const auto& expressions = expr.expressions;

    if (expressions.empty()) {
        // 无插值快路径：直接加载字符串
        uint16_t idx = add_constant(Value::string(quasis[0].cooked));
        emit(Opcode::kLoadString);
        emit_u16(idx);
        return;
    }

    // 有插值：LoadString(quasis[0]) + 对每个表达式 compile_expr + kToString + kAdd + LoadString(quasis[i+1]) + kAdd
    {
        uint16_t idx = add_constant(Value::string(quasis[0].cooked));
        emit(Opcode::kLoadString);
        emit_u16(idx);
    }
    for (size_t i = 0; i < expressions.size(); ++i) {
        compile_expr(*expressions[i]);
        emit(Opcode::kToString);
        emit(Opcode::kAdd);
        uint16_t idx = add_constant(Value::string(quasis[i + 1].cooked));
        emit(Opcode::kLoadString);
        emit_u16(idx);
        emit(Opcode::kAdd);
    }
}

}  // namespace qppjs
