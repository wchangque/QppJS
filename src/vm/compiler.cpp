#include "qppjs/vm/compiler.h"

#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/value.h"
#include "qppjs/vm/bytecode.h"
#include "qppjs/vm/opcode.h"

#include <algorithm>
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
        } else if (const auto* dd = std::get_if<DestructuringDeclaration>(&s.v)) {
            if (dd->kind == VarKind::Let || dd->kind == VarKind::Const) return true;
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
            if (for_of.pattern_binding) {
                hoist_vars_scan_pattern(*for_of.pattern_binding);
            } else {
                uint16_t idx = add_name(for_of.binding);
                bool found = false;
                for (uint16_t v : current_->var_decls) {
                    if (v == idx) { found = true; break; }
                }
                if (!found) current_->var_decls.push_back(idx);
            }
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
    } else if (std::holds_alternative<DestructuringDeclaration>(stmt.v)) {
        const auto& dd = std::get<DestructuringDeclaration>(stmt.v);
        if (dd.kind == VarKind::Var) {
            hoist_vars_scan_pattern(*dd.pattern);
        }
    }
    // Do NOT recurse into FunctionDeclaration/FunctionExpression bodies
}

void Compiler::hoist_vars_scan_pattern(const PatternNode& pat) {
    std::visit(overloaded{
        [this](const IdentifierPattern& ip) {
            uint16_t idx = add_name(ip.name);
            bool found = false;
            for (uint16_t v : current_->var_decls) {
                if (v == idx) { found = true; break; }
            }
            if (!found) current_->var_decls.push_back(idx);
        },
        [this](const ArrayPattern& ap) {
            for (const auto& elem_opt : ap.elements) {
                if (elem_opt.has_value()) {
                    hoist_vars_scan_pattern(*elem_opt->pattern);
                }
            }
            if (ap.rest) hoist_vars_scan_pattern(*ap.rest);
        },
        [this](const ObjectPattern& op) {
            for (const auto& prop : op.properties) {
                hoist_vars_scan_pattern(*prop.value_pattern);
            }
            if (op.rest) hoist_vars_scan_pattern(*op.rest);
        },
    }, pat.v);
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
                    if (prop.computed && prop.key_expr) hoist_vars_scan_expr(*prop.key_expr);
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
            [this](const DestructuringAssignmentExpression& e) {
                hoist_vars_scan_expr(*e.value);
            },
            [this](const OptionalChainExpression& e) {
                hoist_vars_scan_expr(*e.base);
                for (const auto& lnk : e.links) {
                    if (const auto* el = std::get_if<OptionalChainExpression::ElemLink>(&lnk)) {
                        hoist_vars_scan_expr(*el->key);
                    } else if (const auto* cl = std::get_if<OptionalChainExpression::CallLink>(&lnk)) {
                        for (const auto& arg : cl->args) {
                            hoist_vars_scan_expr(*arg);
                        }
                    }
                }
            },
            [this](const YieldExpression& e) {
                if (e.argument) hoist_vars_scan_expr(*e.argument);
            },
            [](const ClassExpression&) {},
            [this](const SuperCallExpression& e) {
                for (const auto& arg : e.arguments) hoist_vars_scan_expr(*arg);
            },
            [](const SuperMemberExpression&) {},
            [this](const TaggedTemplateExpression& e) {
                hoist_vars_scan_expr(*e.tag);
                for (const auto& ex : e.tmpl.expressions) hoist_vars_scan_expr(*ex);
            },
            [this](const PrivateMemberExpression& e) {
                hoist_vars_scan_expr(*e.object);
            },
            [this](const PrivateInExpression& e) {
                hoist_vars_scan_expr(*e.object);
            },
        },
        expr.v);
}

// ============================================================
// compile_function (core)
// ============================================================

std::shared_ptr<BytecodeFunction> Compiler::compile_function(
    std::optional<std::string> name,
    const std::vector<ParamDef>& params,
    const std::vector<StmtNode>& body,
    bool is_program,
    std::optional<std::string> rest_param) {

    // 提取参数名列表，计算 length_count
    std::vector<std::string> param_names;
    param_names.reserve(params.size());
    for (const auto& pd : params) param_names.push_back(pd.name);

    uint16_t length_count = static_cast<uint16_t>(params.size());
    for (uint16_t i = 0; i < static_cast<uint16_t>(params.size()); ++i) {
        if (params[i].default_init != nullptr) { length_count = i; break; }
    }

    auto fn = std::make_shared<BytecodeFunction>();
    fn->name = std::move(name);
    fn->params = param_names;
    fn->param_defs = std::make_shared<std::vector<ParamDef>>(params);
    fn->length_count = length_count;
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

    // Emit default param prologue（仅对有默认值的参数）
    // 序列：kGetVar name; kLoadUndefined; kStrictEq; kJumpIfFalse skip; [default_expr]; kSetVar name; kPop; label_skip:
    // 必须在 kDefVar（body var 提升）之前执行，确保 body var 在默认值求值时不可见（规范要求）
    if (!is_program) {
        for (const auto& pd : params) {
            if (pd.default_init == nullptr) continue;
            uint16_t name_idx = add_name(pd.name);
            emit(Opcode::kGetVar);
            emit_u16(name_idx);
            emit(Opcode::kLoadUndefined);
            emit(Opcode::kStrictEq);
            size_t skip_patch = emit_jump(Opcode::kJumpIfFalse);  // if not undefined, skip
            // param is undefined: compile default_init
            compile_expr(*pd.default_init);
            emit(Opcode::kSetVar);
            emit_u16(name_idx);
            emit(Opcode::kPop);
            patch_jump(skip_patch);  // label_skip
        }
    }

    // Emit DefVar for all hoisted vars after prologue（prologue 之后才提升 body 中的 var 声明）
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
            child->is_generator = fdecl_ptr->is_generator;
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
            if (afdecl_ptr->is_generator) {
                child->is_async_generator = true;
                child->is_generator = true;  // so push_call_frame creates generator object
            } else {
                child->is_async = true;
            }
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
            [this](const DestructuringDeclaration& s) { compile_destructuring_decl(s); },
            [this](const ClassDeclaration& s) { compile_class_decl(s); },
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

// ============================================================
// Destructuring compilation helpers
// ============================================================

// compile_bind_pattern: 假设 rhs 已在栈顶
// 消耗 rhs，将 pattern 中每个标识符绑定到对应值
// kind: 声明种类（Var/Let/Const），is_assign=true 时用 SetVar（写已有变量）
void Compiler::compile_bind_pattern(const PatternNode& pat, VarKind kind, bool is_assign) {
    std::visit(overloaded{
        [&](const IdentifierPattern& ip) {
            uint16_t idx = add_name(ip.name);
            if (is_assign || kind == VarKind::Var) {
                emit(Opcode::kSetVar);
                emit_u16(idx);
                emit(Opcode::kPop);
            } else {
                emit(kind == VarKind::Let ? Opcode::kDefLet : Opcode::kDefConst);
                emit_u16(idx);
                emit(Opcode::kInitVar);
                emit_u16(idx);
                emit(Opcode::kPop);
            }
        },
        [&](const ObjectPattern& op) {
            // 统一使用临时变量存储 rhs（简化 rest/无 rest 两种路径）
            static int dest_counter = 0;
            std::string tmp_name = "$__qppjs_obj_dest_" + std::to_string(dest_counter++) + "__";
            uint16_t tmp_idx = add_name(tmp_name);
            // DefLet $tmp; InitVar $tmp (pop rhs) → stack: []
            emit(Opcode::kDefLet);
            emit_u16(tmp_idx);
            emit(Opcode::kInitVar);
            emit_u16(tmp_idx);
            emit(Opcode::kPop);
            // 每个属性
            std::vector<std::string> named_keys;
            // For rest exclusion: computed key temps (only allocated when op.rest != nullptr)
            bool has_rest = (op.rest != nullptr);
            std::vector<uint16_t> computed_key_tmp_indices;
            static int ckey_counter = 0;
            for (const auto& prop : op.properties) {
                if (prop.computed && prop.key_expr != nullptr) {
                    if (has_rest) {
                        // Save computed key to temp so it can be reused for CopyDataProperties exclusion
                        std::string ckey_tmp = "$__qppjs_ckey_" + std::to_string(ckey_counter++) + "__";
                        uint16_t ckey_idx = add_name(ckey_tmp);
                        emit(Opcode::kDefLet);
                        emit_u16(ckey_idx);
                        compile_expr(*prop.key_expr);
                        emit(Opcode::kInitVar);
                        emit_u16(ckey_idx);
                        emit(Opcode::kPop);
                        computed_key_tmp_indices.push_back(ckey_idx);
                        // Load obj and saved key, then GetElem
                        emit(Opcode::kGetVar);
                        emit_u16(tmp_idx);
                        emit(Opcode::kGetVar);
                        emit_u16(ckey_idx);
                        emit(Opcode::kGetElem);
                    } else {
                        // No rest: no need to save key separately
                        emit(Opcode::kGetVar);
                        emit_u16(tmp_idx);
                        compile_expr(*prop.key_expr);
                        emit(Opcode::kGetElem);
                    }
                } else {
                    named_keys.push_back(prop.key);
                    emit(Opcode::kGetVar);
                    emit_u16(tmp_idx);
                    uint16_t key_idx = add_name(prop.key);
                    emit(Opcode::kGetProp);
                    emit_u16(key_idx);
                }
                // 默认值处理：如果 val === undefined，使用 default
                if (prop.default_value.has_value()) {
                    emit(Opcode::kDup);
                    emit(Opcode::kLoadUndefined);
                    emit(Opcode::kStrictEq);
                    size_t skip_default = emit_jump(Opcode::kJumpIfFalse);
                    emit(Opcode::kPop);
                    compile_expr(**prop.default_value);
                    patch_jump(skip_default);
                }
                compile_bind_pattern(*prop.value_pattern, kind, is_assign);
            }
            // rest 处理
            if (op.rest != nullptr) {
                emit(Opcode::kGetVar);
                emit_u16(tmp_idx);
                for (const auto& k : named_keys) {
                    uint16_t ki = add_constant(Value::string(k));
                    emit(Opcode::kLoadString);
                    emit_u16(ki);
                }
                // Push computed key values for dynamic exclusion
                for (uint16_t ckey_idx : computed_key_tmp_indices) {
                    emit(Opcode::kGetVar);
                    emit_u16(ckey_idx);
                }
                uint8_t total_excluded = static_cast<uint8_t>(named_keys.size() + computed_key_tmp_indices.size());
                emit(Opcode::kCopyDataProperties);
                emit_u8(total_excluded);
                compile_bind_pattern(*op.rest, kind, is_assign);
            }
        },
        [&](const ArrayPattern& ap) {
            // 数组解构：使用 ForOfStart/ForOfNext/IteratorClose
            // Stack before: [rhs]
            emit(Opcode::kForOfStart);
            // Stack: [iter]
            // Try-catch for iterator close on exception
            size_t enter_try_pos = emit_jump(Opcode::kEnterTry);

            // 每个普通元素
            for (const auto& elem_opt : ap.elements) {
                if (!elem_opt.has_value()) {
                    // elision hole: consume one value
                    emit(Opcode::kForOfNext);
                    // stack: [iter, value, done]
                    emit(Opcode::kPop);  // pop done
                    emit(Opcode::kPop);  // pop value (discarded)
                    continue;
                }
                const auto& elem = *elem_opt;
                emit(Opcode::kForOfNext);
                // stack: [iter, value, done]
                emit(Opcode::kPop);  // pop done (simplified: ignore done flag)
                // stack: [iter, value]
                // 默认值处理
                if (elem.default_value.has_value()) {
                    emit(Opcode::kDup);
                    emit(Opcode::kLoadUndefined);
                    emit(Opcode::kStrictEq);
                    size_t skip_default = emit_jump(Opcode::kJumpIfFalse);
                    emit(Opcode::kPop);
                    compile_expr(**elem.default_value);
                    patch_jump(skip_default);
                }
                // bind consumes value, stack: [iter]
                compile_bind_pattern(*elem.pattern, kind, is_assign);
            }

            // rest 处理：将剩余元素收集到新数组
            // 使用临时变量存储 iter，然后用 SpreadAppend 一次性收集剩余元素
            if (ap.rest != nullptr) {
                static int iter_counter = 0;
                std::string iter_tmp = "$__qppjs_arr_iter_" + std::to_string(iter_counter++) + "__";
                uint16_t iter_tmp_idx = add_name(iter_tmp);
                // 存储 iter: DefLet $iter_tmp; InitVar $iter_tmp; Pop → stack: []
                emit(Opcode::kDefLet);
                emit_u16(iter_tmp_idx);
                emit(Opcode::kInitVar);
                emit_u16(iter_tmp_idx);
                emit(Opcode::kPop);
                // 创建新数组, dup 它，然后 SpreadAppend iter
                emit(Opcode::kNewArray);      // [new_arr]
                emit(Opcode::kDup);           // [new_arr, new_arr_dup]
                emit(Opcode::kGetVar);        // [new_arr, new_arr_dup, iter]
                emit_u16(iter_tmp_idx);
                emit(Opcode::kSpreadAppend);  // [new_arr]
                // 绑定 rest 模式
                compile_bind_pattern(*ap.rest, kind, is_assign);
                // 恢复 iter 到栈上（用于 IteratorClose）
                emit(Opcode::kGetVar);
                emit_u16(iter_tmp_idx);
            }

            // Close iterator (normal path)
            emit(Opcode::kLeaveTry);
            emit(Opcode::kIteratorClose);
            size_t after_close = emit_jump(Opcode::kJump);

            // Exception handler
            size_t exc_handler = current_offset();
            patch_jump_to(enter_try_pos, exc_handler);
            emit(Opcode::kGetException);
            emit(Opcode::kIteratorCloseAbnormal);
            emit(Opcode::kThrow);
            patch_jump(after_close);
        },
    }, pat.v);
}

void Compiler::compile_destructuring_decl(const DestructuringDeclaration& decl) {
    if (decl.init) {
        compile_expr(*decl.init);
    } else {
        emit(Opcode::kLoadUndefined);
    }
    compile_bind_pattern(*decl.pattern, decl.kind, false);
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
            [this](const MetaProperty& e) {
                if (e.kind == MetaPropertyKind::kNewTarget) {
                    emit(Opcode::kGetNewTarget);
                } else {
                    emit(Opcode::kMetaProperty);
                }
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
            [this](const DestructuringAssignmentExpression& e) {
                // Compile rhs first
                compile_expr(*e.value);
                // Dup so the expression also produces a value (= expression result = rhs)
                emit(Opcode::kDup);
                // compile_bind_pattern consumes rhs (the dup'd copy)
                // Actually: rhs is on stack, we need rhs to remain for expression value.
                // Strategy: compile rhs, dup it, bind pattern from dup'd copy.
                // But compile_bind_pattern pops the rhs.
                // So: stack before bind = [rhs_orig, rhs_dup]; bind consumes rhs_dup; leaves rhs_orig.
                compile_bind_pattern(*e.pattern, VarKind::Var /* unused for assign */, true);
            },
            [this](const OptionalChainExpression& e) { compile_optional_chain(e); },
            [this](const ClassExpression& e) { compile_class_expr(e); },
            [this](const SuperCallExpression& e) {
                // emit args, then kSuperCall argc
                for (const auto& arg : e.arguments) {
                    if (std::holds_alternative<SpreadElement>(arg->v)) {
                        compile_expr(*std::get<SpreadElement>(arg->v).argument);
                        emit(Opcode::kSpreadAppend);
                    } else {
                        compile_expr(*arg);
                    }
                }
                emit(Opcode::kSuperCall);
                emit_u8(static_cast<uint8_t>(e.arguments.size()));
            },
            [this](const SuperMemberExpression& e) {
                if (e.computed) {
                    compile_expr(*e.key_expr);
                    emit(Opcode::kSuperGetElem);
                } else {
                    uint16_t idx = add_name(e.property);
                    emit(Opcode::kSuperGetProp);
                    emit_u16(idx);
                }
            },
            [this](const YieldExpression& e) {
                if (e.is_delegate) {
                    // yield* iterable
                    // Stack progression: [] → [iter] → loop: [iter, val, done] → yield val → [iter]
                    compile_expr(*e.argument);
                    emit(Opcode::kForOfStart);    // [iter]
                    size_t loop_top = current_->code.size();
                    emit(Opcode::kForOfNext);     // [iter, value, done]
                    // done=false (falsy) → jump to loop_body; done=true → fall through to exit
                    size_t jmp_to_body = emit_jump(Opcode::kJumpIfFalse);  // pops done
                    // done=true path: stack is [iter, value]
                    emit(Opcode::kPop);           // discard return value; stack: [iter]
                    emit(Opcode::kIteratorClose); // close and pop iter; stack: []
                    emit(Opcode::kLoadUndefined); // yield* result = undefined (basic impl)
                    size_t jmp_end = emit_jump(Opcode::kJump);
                    // done=false path: stack is [iter, value]
                    patch_jump(jmp_to_body);
                    // value is TOS; kYield pops TOS as yield value; iter remains below
                    emit(Opcode::kYield);         // yields value; after resume: [iter, resume_val]
                    emit(Opcode::kPop);           // discard resume_val (not forwarded, basic impl)
                    emit_jump_to(Opcode::kJump, loop_top);
                    patch_jump(jmp_end);
                } else {
                    if (e.argument) {
                        compile_expr(*e.argument);
                    } else {
                        emit(Opcode::kLoadUndefined);
                    }
                    emit(Opcode::kYield);
                }
            },
            [this](const TaggedTemplateExpression& e) { compile_tagged_template_expr(e); },
            [this](const PrivateMemberExpression& pme) {
                // Look up sym_id from private_fields_stack_
                uint64_t sym_id = 0;
                for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
                    auto fit = it->find(pme.field_name);
                    if (fit != it->end()) { sym_id = fit->second; break; }
                }
                if (sym_id == 0) {
                    // Unknown private field: emit a throw
                    uint16_t msg_idx = add_constant(Value::string(
                        "Private field '" + pme.field_name + "' not found"));
                    emit(Opcode::kLoadString);
                    emit_u16(msg_idx);
                    emit(Opcode::kThrow);
                    return;
                }
                compile_expr(*pme.object);                    // push obj
                uint16_t sym_idx = add_constant(Value::symbol(sym_id));  // symbol value in constants
                emit(Opcode::kLoadString);                    // kLoadString reuses constants pool
                emit_u16(sym_idx);
                emit(Opcode::kGetElem);                       // obj[symbol]
            },
            [this](const PrivateInExpression& pie) {
                uint64_t sym_id = 0;
                for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
                    auto fit = it->find(pie.field_name);
                    if (fit != it->end()) { sym_id = fit->second; break; }
                }
                if (sym_id == 0) {
                    emit(Opcode::kLoadFalse);
                    return;
                }
                uint16_t sym_idx = add_constant(Value::symbol(sym_id));
                emit(Opcode::kLoadString);
                emit_u16(sym_idx);
                compile_expr(*pie.object);                    // push obj
                emit(Opcode::kIn);                            // symbol in obj
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
        } else if (std::holds_alternative<OptionalChainExpression>(expr.operand->v)) {
            const auto& oc = std::get<OptionalChainExpression>(expr.operand->v);
            compile_optional_chain(oc, /*delete_mode=*/true);
        } else {
            // Other expr: eval for side effects, discard, push true
            compile_expr(*expr.operand);
            emit(Opcode::kPop);
            emit(Opcode::kLoadTrue);
        }
        break;
    case UnaryOp::BitNot:
        compile_expr(*expr.operand);
        emit(Opcode::kBitNot);
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
    case BinaryOp::Pow:     emit(Opcode::kPow);       break;
    case BinaryOp::Lt:      emit(Opcode::kLt);        break;
    case BinaryOp::LtEq:    emit(Opcode::kLtEq);      break;
    case BinaryOp::Gt:      emit(Opcode::kGt);        break;
    case BinaryOp::GtEq:    emit(Opcode::kGtEq);      break;
    case BinaryOp::EqEq:    emit(Opcode::kEq);        break;
    case BinaryOp::NotEq:   emit(Opcode::kNEq);       break;
    case BinaryOp::EqEqEq:    emit(Opcode::kStrictEq);  break;
    case BinaryOp::NotEqEq:   emit(Opcode::kStrictNEq); break;
    case BinaryOp::Instanceof: emit(Opcode::kInstanceof); break;
    case BinaryOp::In:         emit(Opcode::kIn);         break;
    case BinaryOp::BitAnd:     emit(Opcode::kBitAnd);     break;
    case BinaryOp::BitOr:      emit(Opcode::kBitOr);      break;
    case BinaryOp::BitXor:     emit(Opcode::kBitXor);     break;
    case BinaryOp::Shl:        emit(Opcode::kShl);        break;
    case BinaryOp::Sar:        emit(Opcode::kSar);        break;
    case BinaryOp::Shr:        emit(Opcode::kShr);        break;
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
    } else if (expr.op == LogicalOp::Or) {
        size_t patch = emit_jump(Opcode::kJumpIfTrue);
        emit(Opcode::kPop);
        compile_expr(*expr.right);
        patch_jump(patch);
    } else {
        // Nullish: LHS is non-nullish → jump (keep LHS), else pop LHS and eval RHS
        size_t patch = emit_jump(Opcode::kJumpIfNotNullish);
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
    } else if (expr.op == AssignOp::LogicalAndAssign || expr.op == AssignOp::LogicalOrAssign ||
               expr.op == AssignOp::NullishAssign) {
        // Logical assignment short-circuit pattern:
        //   kGetVar → kDup → kJumpIfXxx skip → kPop → [rhs] → kSetVar → skip:
        //
        // kJumpIfXxx pops the dup'd value.
        // If taken (short-circuit): original lhs stays on stack as result.
        // If not taken: kPop removes lhs, RHS compiled, kSetVar writes+pushes result.
        emit(Opcode::kGetVar);
        emit_u16(idx);
        emit(Opcode::kDup);
        Opcode jump_op;
        if (expr.op == AssignOp::LogicalAndAssign) {
            jump_op = Opcode::kJumpIfFalse;        // LHS falsy → short-circuit
        } else if (expr.op == AssignOp::LogicalOrAssign) {
            jump_op = Opcode::kJumpIfTrue;         // LHS truthy → short-circuit
        } else {
            jump_op = Opcode::kJumpIfNotNullish;   // LHS non-nullish → short-circuit
        }
        size_t skip_patch = emit_jump(jump_op);
        // Not short-circuiting: pop lhs, eval RHS, write back
        emit(Opcode::kPop);
        compile_expr(*expr.value);
        emit(Opcode::kSetVar);
        emit_u16(idx);
        // Patch jump to here (skip lands with lhs on stack, SetVar already pushed rhs)
        patch_jump(skip_patch);
        return;
    } else {
        // Compound assignment: read current value, compute, write back
        emit(Opcode::kGetVar);
        emit_u16(idx);
        compile_expr(*expr.value);
        switch (expr.op) {
        case AssignOp::AddAssign:     emit(Opcode::kAdd);    break;
        case AssignOp::SubAssign:     emit(Opcode::kSub);    break;
        case AssignOp::MulAssign:     emit(Opcode::kMul);    break;
        case AssignOp::DivAssign:     emit(Opcode::kDiv);    break;
        case AssignOp::ModAssign:     emit(Opcode::kMod);    break;
        case AssignOp::PowAssign:     emit(Opcode::kPow);    break;
        case AssignOp::BitAndAssign:  emit(Opcode::kBitAnd); break;
        case AssignOp::BitOrAssign:   emit(Opcode::kBitOr);  break;
        case AssignOp::BitXorAssign:  emit(Opcode::kBitXor); break;
        case AssignOp::ShlAssign:     emit(Opcode::kShl);    break;
        case AssignOp::SarAssign:     emit(Opcode::kSar);    break;
        case AssignOp::ShrAssign:     emit(Opcode::kShr);    break;
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
        if (std::holds_alternative<SpreadElement>(prop.value->v)) {
            uint16_t msg_idx = add_constant(Value::string("Object spread not supported"));
            emit(Opcode::kLoadString);
            emit_u16(msg_idx);
            emit(Opcode::kThrow);
            return;
        }
        if (std::holds_alternative<AssignmentExpression>(prop.value->v)) {
            uint16_t msg_idx = add_constant(Value::string("Invalid shorthand property initializer"));
            emit(Opcode::kLoadString);
            emit_u16(msg_idx);
            emit(Opcode::kThrow);
            return;
        }

        // 计算键属性：栈布局 [... obj] → kDup → [... obj obj] → compile key → [... obj obj key]
        //   → compile val/fn  → [... obj obj key val/fn] → kSetComputedProp/kDefineComputed* → [... obj val/fn]
        //   → kPop → [... obj]
        if (prop.computed && prop.key_expr != nullptr) {
            if (prop.method_kind == MethodKind::kData) {
                emit(Opcode::kDup);
                compile_expr(*prop.key_expr);
                compile_expr(*prop.value);
                emit(Opcode::kSetComputedProp);
                emit(Opcode::kPop);
            } else if (prop.method_kind == MethodKind::kMethod ||
                       prop.method_kind == MethodKind::kGenerator) {
                const auto& fe = std::get<FunctionExpression>(prop.value->v);
                auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
                child->is_method = true;
                child->is_generator = (prop.method_kind == MethodKind::kGenerator);
                uint16_t fn_idx = add_function(std::move(child));
                emit(Opcode::kDup);
                compile_expr(*prop.key_expr);
                emit(Opcode::kMakeFunction);
                emit_u16(fn_idx);
                emit(Opcode::kSetComputedProp);
                emit(Opcode::kPop);
            } else if (prop.method_kind == MethodKind::kAsyncMethod) {
                const auto& afe = std::get<AsyncFunctionExpression>(prop.value->v);
                auto child = compile_function(afe.name, afe.params, *afe.body, false, afe.rest_param);
                child->is_async = true;
                child->is_method = true;
                uint16_t fn_idx = add_function(std::move(child));
                emit(Opcode::kDup);
                compile_expr(*prop.key_expr);
                emit(Opcode::kMakeFunction);
                emit_u16(fn_idx);
                emit(Opcode::kSetComputedProp);
                emit(Opcode::kPop);
            } else if (prop.method_kind == MethodKind::kGetter) {
                const auto& fe = std::get<FunctionExpression>(prop.value->v);
                auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
                child->is_method = true;
                uint16_t fn_idx = add_function(std::move(child));
                emit(Opcode::kDup);
                compile_expr(*prop.key_expr);
                emit(Opcode::kMakeFunction);
                emit_u16(fn_idx);
                emit(Opcode::kDefineComputedGetter);
                emit(Opcode::kPop);
            } else if (prop.method_kind == MethodKind::kSetter) {
                const auto& fe = std::get<FunctionExpression>(prop.value->v);
                auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
                child->is_method = true;
                uint16_t fn_idx = add_function(std::move(child));
                emit(Opcode::kDup);
                compile_expr(*prop.key_expr);
                emit(Opcode::kMakeFunction);
                emit_u16(fn_idx);
                emit(Opcode::kDefineComputedSetter);
                emit(Opcode::kPop);
            }
            continue;
        }

        if (prop.method_kind == MethodKind::kData) {
            // 普通数据属性
            emit(Opcode::kDup);
            compile_expr(*prop.value);
            uint16_t name_idx = add_name(prop.key);
            emit(Opcode::kSetProp);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        } else if (prop.method_kind == MethodKind::kMethod ||
                   prop.method_kind == MethodKind::kGenerator) {
            // 普通方法简写 / generator 方法
            const auto& fe = std::get<FunctionExpression>(prop.value->v);
            auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
            child->is_method = true;
            child->is_generator = (prop.method_kind == MethodKind::kGenerator);
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kDup);
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(prop.key);
            emit(Opcode::kSetProp);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        } else if (prop.method_kind == MethodKind::kAsyncMethod) {
            // async 方法
            const auto& afe = std::get<AsyncFunctionExpression>(prop.value->v);
            auto child = compile_function(afe.name, afe.params, *afe.body, false, afe.rest_param);
            child->is_async = true;
            child->is_method = true;
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kDup);
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(prop.key);
            emit(Opcode::kSetProp);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        } else if (prop.method_kind == MethodKind::kGetter) {
            // getter 方法
            const auto& fe = std::get<FunctionExpression>(prop.value->v);
            auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
            child->is_method = true;
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kDup);
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(prop.key);
            emit(Opcode::kDefineGetter);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        } else if (prop.method_kind == MethodKind::kSetter) {
            // setter 方法
            const auto& fe = std::get<FunctionExpression>(prop.value->v);
            auto child = compile_function(fe.name, fe.params, *fe.body, false, fe.rest_param);
            child->is_method = true;
            uint16_t fn_idx = add_function(std::move(child));
            emit(Opcode::kDup);
            emit(Opcode::kMakeFunction);
            emit_u16(fn_idx);
            uint16_t name_idx = add_name(prop.key);
            emit(Opcode::kDefineSetter);
            emit_u16(name_idx);
            emit(Opcode::kPop);
        }
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
    // Private field assignment: property is a synthetic Identifier with #name (from Parser)
    if (!expr.computed && std::holds_alternative<Identifier>(expr.property->v)) {
        const auto& id = std::get<Identifier>(expr.property->v);
        if (!id.name.empty() && id.name[0] == '#') {
            uint64_t sym_id = 0;
            for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
                auto fit = it->find(id.name);
                if (fit != it->end()) { sym_id = fit->second; break; }
            }
            if (sym_id != 0) {
                uint16_t sym_idx = add_constant(Value::symbol(sym_id));
                if (expr.op == AssignOp::Assign) {
                    compile_expr(*expr.object);   // [obj]
                    compile_expr(*expr.value);    // [obj, val]
                    emit(Opcode::kLoadString);    // [obj, val, sym]
                    emit_u16(sym_idx);
                    emit(Opcode::kSetElem);       // [val]
                } else {
                    // Compound assignment: [obj] → Dup → sym → GetElem → cur_val
                    compile_expr(*expr.object);   // [obj]
                    emit(Opcode::kDup);           // [obj, obj]
                    emit(Opcode::kLoadString);    // [obj, obj, sym]
                    emit_u16(sym_idx);
                    emit(Opcode::kGetElem);       // [obj, cur_val]
                    compile_expr(*expr.value);    // [obj, cur_val, rhs]
                    switch (expr.op) {
                    case AssignOp::AddAssign:    emit(Opcode::kAdd);    break;
                    case AssignOp::SubAssign:    emit(Opcode::kSub);    break;
                    case AssignOp::MulAssign:    emit(Opcode::kMul);    break;
                    case AssignOp::DivAssign:    emit(Opcode::kDiv);    break;
                    case AssignOp::ModAssign:    emit(Opcode::kMod);    break;
                    case AssignOp::PowAssign:    emit(Opcode::kPow);    break;
                    case AssignOp::BitAndAssign: emit(Opcode::kBitAnd); break;
                    case AssignOp::BitOrAssign:  emit(Opcode::kBitOr);  break;
                    case AssignOp::BitXorAssign: emit(Opcode::kBitXor); break;
                    case AssignOp::ShlAssign:    emit(Opcode::kShl);    break;
                    case AssignOp::SarAssign:    emit(Opcode::kSar);    break;
                    case AssignOp::ShrAssign:    emit(Opcode::kShr);    break;
                    default: break;
                    }
                    // Stack: [obj, new_val] → [obj, new_val, sym] → kSetElem → [new_val]
                    emit(Opcode::kLoadString);
                    emit_u16(sym_idx);
                    emit(Opcode::kSetElem);
                }
                return;
            }
        }
    }

    if (expr.op == AssignOp::LogicalAndAssign || expr.op == AssignOp::LogicalOrAssign ||
        expr.op == AssignOp::NullishAssign) {
        // Logical member assignment (short-circuit):
        if (!expr.computed) {
            // obj.prop &&= rhs pattern:
            //   compile obj      → [obj]
            //   kDup             → [obj, obj]
            //   kGetProp         → [obj, prop_val]
            //   kDup             → [obj, prop_val, prop_val_dup]
            //   kJumpIfXxx skip  → pops prop_val_dup; if short-circuit: [obj, prop_val]
            //   (not SC) kPop    → [obj]
            //   (not SC) compile RHS → [obj, rhs]
            //   (not SC) kSetProp → [rhs]
            //   kJump end
            //   skip: kSwap → kPop → [prop_val]
            //   end:
            const auto& prop_str = std::get<StringLiteral>(expr.property->v);
            uint16_t pidx = add_name(prop_str.value);
            compile_expr(*expr.object);
            emit(Opcode::kDup);
            emit(Opcode::kGetProp);
            emit_u16(pidx);
            emit(Opcode::kDup);
            Opcode jump_op = (expr.op == AssignOp::LogicalAndAssign) ? Opcode::kJumpIfFalse
                           : (expr.op == AssignOp::LogicalOrAssign)  ? Opcode::kJumpIfTrue
                                                                      : Opcode::kJumpIfNotNullish;
            size_t skip_patch = emit_jump(jump_op);
            // Not short-circuiting:
            emit(Opcode::kPop);
            compile_expr(*expr.value);
            emit(Opcode::kSetProp);
            emit_u16(pidx);
            size_t end_patch = emit_jump(Opcode::kJump);
            // Short-circuit path:
            patch_jump(skip_patch);
            emit(Opcode::kSwap);
            emit(Opcode::kPop);
            patch_jump(end_patch);
            return;
        }
        // Computed logical member assign: simplified, no short-circuit in VM
        compile_expr(*expr.object);
        compile_expr(*expr.value);
        compile_expr(*expr.property);
        emit(Opcode::kSetElem);
        return;
    }

    // Arithmetic/bitwise compound member assignment: read-modify-write
    if (expr.op != AssignOp::Assign) {
        if (!expr.computed) {
            // obj.prop op= rhs:
            //   compile obj   → [obj]
            //   kDup          → [obj, obj]
            //   kGetProp      → [obj, current_val]
            //   compile rhs   → [obj, current_val, rhs]
            //   kOp           → [obj, new_val]
            //   kSetProp      → [new_val]
            const auto& prop_str2 = std::get<StringLiteral>(expr.property->v);
            uint16_t pidx2 = add_name(prop_str2.value);
            compile_expr(*expr.object);
            emit(Opcode::kDup);
            emit(Opcode::kGetProp);
            emit_u16(pidx2);
            compile_expr(*expr.value);
            // Emit operation
            switch (expr.op) {
            case AssignOp::AddAssign:    emit(Opcode::kAdd);    break;
            case AssignOp::SubAssign:    emit(Opcode::kSub);    break;
            case AssignOp::MulAssign:    emit(Opcode::kMul);    break;
            case AssignOp::DivAssign:    emit(Opcode::kDiv);    break;
            case AssignOp::ModAssign:    emit(Opcode::kMod);    break;
            case AssignOp::PowAssign:    emit(Opcode::kPow);    break;
            case AssignOp::BitAndAssign: emit(Opcode::kBitAnd); break;
            case AssignOp::BitOrAssign:  emit(Opcode::kBitOr);  break;
            case AssignOp::BitXorAssign: emit(Opcode::kBitXor); break;
            case AssignOp::ShlAssign:    emit(Opcode::kShl);    break;
            case AssignOp::SarAssign:    emit(Opcode::kSar);    break;
            case AssignOp::ShrAssign:    emit(Opcode::kShr);    break;
            default: break;
            }
            emit(Opcode::kSetProp);
            emit_u16(pidx2);
        } else {
            // Computed: obj[key] op= rhs (simplified: no true read-modify-write in current VM)
            compile_expr(*expr.object);
            compile_expr(*expr.value);
            compile_expr(*expr.property);
            emit(Opcode::kSetElem);
        }
        return;
    }

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
    child->is_generator = expr.is_generator;
    uint16_t fn_idx = add_function(std::move(child));
    emit(Opcode::kMakeFunction);
    emit_u16(fn_idx);
}

void Compiler::compile_async_function_expr(const AsyncFunctionExpression& expr) {
    auto child = compile_function(expr.name, expr.params, *expr.body, false, expr.rest_param);
    if (expr.is_generator) {
        child->is_async_generator = true;
        child->is_generator = true;  // so push_call_frame creates generator object
    } else {
        child->is_async = true;
        // P2-D: named async function expressions need self-reference binding inside the body
        child->is_named_expr = expr.name.has_value();
    }
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

    // super.method(...args) — get method from H.__proto__, call with current this
    if (std::holds_alternative<SuperMemberExpression>(expr.callee->v)) {
        const auto& smem = std::get<SuperMemberExpression>(expr.callee->v);
        emit(Opcode::kLoadThis);   // receiver = current 'this'
        // Get method from super
        if (smem.computed) {
            compile_expr(*smem.key_expr);
            emit(Opcode::kSuperGetElem);
        } else {
            uint16_t idx = add_name(smem.property);
            emit(Opcode::kSuperGetProp);
            emit_u16(idx);
        }
        // Stack: [this_receiver, method]
        for (const auto& arg : expr.arguments) {
            if (std::holds_alternative<SpreadElement>(arg->v)) {
                compile_expr(*std::get<SpreadElement>(arg->v).argument);
                emit(Opcode::kSpreadAppend);
            } else {
                compile_expr(*arg);
            }
        }
        emit(Opcode::kCallMethod);
        emit_u8(static_cast<uint8_t>(expr.arguments.size()));
        return;
    }

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
    // pattern_binding: use destructuring; otherwise simple binding
    bool has_pattern = stmt.pattern_binding != nullptr;
    uint16_t name_idx = has_pattern ? 0 : add_name(stmt.binding);

    compile_expr(*stmt.right);
    emit(Opcode::kForOfStart);
    size_t enter_try_pos = emit_jump(Opcode::kEnterTry);
    size_t jump_to_check = emit_jump(Opcode::kJump);

    size_t label_body_start = current_offset();

    if (has_pattern) {
        // 解构模式绑定：value on top of [iter, value]
        // stack at body_start: [iter, value]
        if (need_scope) emit(Opcode::kPushScope);
        // compile_bind_pattern consumes value, leaves stack: [iter]
        compile_bind_pattern(*stmt.pattern_binding, stmt.var_kind, !stmt.has_decl);
    } else if (need_scope) {
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
    if (std::holds_alternative<OptionalChainExpression>(expr.operand->v)) {
        // a?.b++ is a SyntaxError at runtime
        uint16_t idx = add_constant(Value::string(
            "SyntaxError: invalid left-hand side: optional chain is not a valid assignment target"));
        emit(Opcode::kLoadString);
        emit_u16(idx);
        emit(Opcode::kThrow);
        return;
    }
    // PrivateMemberExpression: ++this.#x / this.#x++ → inline read-modify-write via symbol key
    if (std::holds_alternative<PrivateMemberExpression>(expr.operand->v)) {
        const auto& pme = std::get<PrivateMemberExpression>(expr.operand->v);
        uint64_t sym_id = 0;
        for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
            auto fit = it->find(pme.field_name);
            if (fit != it->end()) { sym_id = fit->second; break; }
        }
        if (sym_id == 0) {
            uint16_t msg_idx = add_constant(Value::string("Private field '" + pme.field_name + "' not found"));
            emit(Opcode::kLoadString); emit_u16(msg_idx);
            emit(Opcode::kThrow);
            return;
        }
        uint16_t sym_idx = add_constant(Value::symbol(sym_id));
        uint16_t one_idx = add_constant(Value::number(1.0));

        // prefix: obj → Dup → sym → GetElem(old_num) → 1 → op → [obj, new_val]
        //         → [obj, new_val, sym] → SetElem → [new_val]
        // postfix: obj → Dup → sym → GetElem(old_num) → Dup(old_num_copy) → 1 → op
        //         → [obj, old_num, new_val, sym] → SetElem → [old_num, new_val_result=new_val]
        //         → Pop(new_val) → [old_num]
        // Wait: postfix needs to return old_num AFTER the SetElem.
        // Let me trace postfix carefully:
        // [obj] → Dup → [obj, obj] → sym → GetElem → [obj, old_num]
        // → Pos(ToNumber) → [obj, old_num]
        // → Dup → [obj, old_num, old_num_copy]
        // → 1 → [obj, old_num, old_num_copy, 1]
        // → Add/Sub → [obj, old_num, new_val]
        // → LoadStr sym → [obj, old_num, new_val, sym]
        // → SetElem: pops sym(key), new_val(val), old_num(obj=WRONG!)
        // SetElem uses TOS-2 as obj. TOS-2 = old_num, not obj. Wrong!
        //
        // Correct layout for SetElem: [..., obj, new_val, sym]
        // For postfix: we need [old_num] on stack after SetElem([obj, new_val, sym])
        // So we need: [..., old_num, obj, new_val, sym] — old_num under obj
        // But old_num comes from obj[sym], so we'd need to compute it first, save it,
        // then re-read obj... This is circular.
        //
        // SIMPLER: for postfix, compute:
        // [obj] → Dup → sym → GetElem → Pos(old_num) → Dup → 1 → op(new_val) → Swap
        // After Swap: [obj, new_val, old_num] ... still obj is buried.
        //
        // Alternative: compile object once as a local var expression (not possible here).
        //
        // PRAGMATIC: For private field update expressions in class methods,
        // use the assignment trick: evaluate as assignment expression.
        // ++this.#x === (this.#x = this.#x + 1)
        // this.#x++ === ((old = this.#x), this.#x = old + 1, old)
        //
        // The cleanest approach: for prefix, emit exactly:
        //   obj → Dup → sym → GetElem → Pos → 1 → op → [obj, new_val] → LoadStr sym → SetElem
        // For postfix, we can't easily do it without a temp variable.
        // Use Swap to move old_num to safe location:
        //   obj → Dup → sym → GetElem → Pos → [obj, old_num] → Swap → [old_num, obj]
        //   → Dup → [old_num, obj, obj] → LoadStr sym → GetElem → Pos → 1 → op
        //   → [old_num, obj, new_val] → LoadStr sym → SetElem
        //   SetElem: pops sym(key), new_val(val), obj(obj) → pushes new_val
        //   Stack: [old_num, new_val] → Pop → [old_num]  ✓

        if (expr.prefix) {
            compile_expr(*pme.object);                       // [obj]
            emit(Opcode::kDup);                              // [obj, obj]
            emit(Opcode::kLoadString); emit_u16(sym_idx);   // [obj, obj, sym]
            emit(Opcode::kGetElem);                          // [obj, old_num]
            emit(Opcode::kPos);                              // [obj, old_num] (ToNumber)
            emit(Opcode::kLoadNumber); emit_u16(one_idx);   // [obj, old_num, 1]
            emit(expr.op == UpdateOp::Inc ? Opcode::kAdd : Opcode::kSub);  // [obj, new_val]
            emit(Opcode::kLoadString); emit_u16(sym_idx);   // [obj, new_val, sym]
            emit(Opcode::kSetElem);                          // [new_val]
        } else {
            // postfix: return old_num
            compile_expr(*pme.object);                       // [obj]
            emit(Opcode::kDup);                              // [obj, obj]
            emit(Opcode::kLoadString); emit_u16(sym_idx);   // [obj, obj, sym]
            emit(Opcode::kGetElem);                          // [obj, old_num]
            emit(Opcode::kPos);                              // [obj, old_num] (ToNumber)
            emit(Opcode::kSwap);                             // [old_num, obj]
            emit(Opcode::kDup);                              // [old_num, obj, obj]
            emit(Opcode::kLoadString); emit_u16(sym_idx);   // [old_num, obj, obj, sym]
            emit(Opcode::kGetElem);                          // [old_num, obj, old_num2]
            emit(Opcode::kPos);                              // [old_num, obj, old_num2]
            emit(Opcode::kLoadNumber); emit_u16(one_idx);   // [old_num, obj, old_num2, 1]
            emit(expr.op == UpdateOp::Inc ? Opcode::kAdd : Opcode::kSub);  // [old_num, obj, new_val]
            emit(Opcode::kLoadString); emit_u16(sym_idx);   // [old_num, obj, new_val, sym]
            emit(Opcode::kSetElem);                          // [old_num, new_val]
            emit(Opcode::kPop);                              // [old_num]
        }
        return;
    }

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

void Compiler::compile_tagged_template_expr(const TaggedTemplateExpression& expr) {
    const TemplateLiteral& tmpl = expr.tmpl;

    // kCallMethod(argc) expects stack: [receiver, callee, arg0, arg1, ...]
    // For plain tag call: receiver=undefined, callee=tag
    // For method call (tag is MemberExpression): receiver=obj, callee=method

    if (auto* mem = std::get_if<MemberExpression>(&expr.tag->v)) {
        // obj.method`...` — receiver=obj, callee=method
        compile_expr(*mem->object);   // [obj]
        emit(Opcode::kDup);           // [obj, obj]
        if (mem->computed) {
            compile_expr(*mem->property);
            emit(Opcode::kGetElem);   // [obj, method]  (pops dup'd obj)
        } else {
            uint16_t pidx = add_name(std::get<StringLiteral>(mem->property->v).value);
            emit(Opcode::kGetProp);
            emit_u16(pidx);           // [obj, method]
        }
        // Stack: [obj(receiver), method(callee)]
    } else {
        // plain tag`...` — receiver=undefined
        emit(Opcode::kLoadUndefined); // [undefined]
        compile_expr(*expr.tag);      // [undefined, tag]
    }
    // Stack: [receiver, callee]

    // Build strings array with .raw property:
    //   kNewArray                     → [receiver, callee, strings_arr]
    //   for each quasi: LoadString+ArrayAppend
    //   kDup                          → [receiver, callee, strings_arr, strings_arr_dup]
    //   kNewArray                     → [receiver, callee, strings_arr, strings_arr_dup, raw_arr]
    //   for each quasi: LoadString+ArrayAppend
    //   kSetProp "raw"                → pops (raw_arr, strings_arr_dup), sets .raw, pushes raw_arr
    //                                 → [receiver, callee, strings_arr, raw_arr]
    //   kPop                          → [receiver, callee, strings_arr]

    emit(Opcode::kNewArray);
    for (size_t i = 0; i < tmpl.quasis.size(); ++i) {
        emit(Opcode::kDup);  // [strings_arr, strings_arr_dup]
        uint16_t sidx = add_constant(Value::string(tmpl.quasis[i].cooked));
        emit(Opcode::kLoadString);
        emit_u16(sidx);
        emit(Opcode::kArrayAppend);  // pops (str + dup), appends to arr
    }
    // [receiver, callee, strings_arr]
    emit(Opcode::kDup);  // [receiver, callee, strings_arr, strings_arr_dup]

    emit(Opcode::kNewArray);
    for (size_t i = 0; i < tmpl.quasis.size(); ++i) {
        emit(Opcode::kDup);  // [raw_arr, raw_arr_dup]
        uint16_t ridx = add_constant(Value::string(tmpl.quasis[i].raw));
        emit(Opcode::kLoadString);
        emit_u16(ridx);
        emit(Opcode::kArrayAppend);
    }
    // [receiver, callee, strings_arr, strings_arr_dup, raw_arr]
    // kSetProp "raw": pops TOS(raw_arr) as val, pops TOS-1(strings_arr_dup) as obj,
    //   sets obj.raw = val, pushes val(raw_arr)
    // → [receiver, callee, strings_arr, raw_arr]
    uint16_t raw_idx = add_name("raw");
    emit(Opcode::kSetProp);
    emit_u16(raw_idx);
    // [receiver, callee, strings_arr, raw_arr(result of SetProp)]
    emit(Opcode::kPop);
    // [receiver, callee, strings_arr]

    // Compile expressions
    for (const auto& e : tmpl.expressions) {
        compile_expr(*e);
    }
    // [receiver, callee, strings_arr, expr0, expr1, ...]

    // argc = 1 (strings_arr) + expressions.size()
    uint8_t argc = static_cast<uint8_t>(1 + tmpl.expressions.size());
    emit(Opcode::kCallMethod);
    emit_u8(argc);
}

void Compiler::compile_optional_chain(const OptionalChainExpression& expr, bool delete_mode) {
    std::vector<size_t> chain_end_patches;

    // Compile base expression
    compile_expr(*expr.base);

    const size_t n = expr.links.size();

    for (size_t i = 0; i < n; ++i) {
        const auto& lnk = expr.links[i];

        bool next_is_call = (i + 1 < n) &&
            std::holds_alternative<OptionalChainExpression::CallLink>(expr.links[i + 1]);
        bool prev_is_member = (i > 0) &&
            (std::holds_alternative<OptionalChainExpression::PropLink>(expr.links[i - 1]) ||
             std::holds_alternative<OptionalChainExpression::ElemLink>(expr.links[i - 1]));

        bool optional = std::visit([](const auto& l) { return l.optional; }, lnk);

        if (optional) {
            emit(Opcode::kDup);  // null-check copy
            size_t patch_nn = emit_jump(Opcode::kJumpIfNotNullish);
            // null/undefined path: clean up stack and jump to chain end
            if (std::holds_alternative<OptionalChainExpression::CallLink>(lnk) && prev_is_member) {
                emit(Opcode::kPop);  // pop method
                emit(Opcode::kPop);  // pop receiver
            } else {
                emit(Opcode::kPop);  // pop base/current
            }
            if (delete_mode) {
                emit(Opcode::kLoadTrue);
            } else {
                emit(Opcode::kLoadUndefined);
            }
            chain_end_patches.push_back(emit_jump(Opcode::kJump));
            patch_jump(patch_nn);  // non-null path continues here
        }

        // Determine if this is the last link and we're in delete mode targeting it
        bool is_last_delete = delete_mode && (i + 1 == n);

        if (const auto* prop = std::get_if<OptionalChainExpression::PropLink>(&lnk)) {
            uint16_t idx = add_name(prop->name);
            if (is_last_delete) {
                emit(Opcode::kDeleteProp);
                emit_u16(idx);
            } else if (next_is_call) {
                emit(Opcode::kDup);          // receiver copy
                emit(Opcode::kGetProp);      // pops dup'd copy, pushes method: [receiver, method]
                emit_u16(idx);
            } else {
                emit(Opcode::kGetProp);
                emit_u16(idx);
            }
        } else if (const auto* elem = std::get_if<OptionalChainExpression::ElemLink>(&lnk)) {
            if (is_last_delete) {
                compile_expr(*elem->key);
                emit(Opcode::kDeleteElem);
            } else if (next_is_call) {
                emit(Opcode::kDup);           // receiver copy
                compile_expr(*elem->key);
                emit(Opcode::kGetElem);       // [receiver, elem]
            } else {
                compile_expr(*elem->key);
                emit(Opcode::kGetElem);
            }
        } else if (const auto* call = std::get_if<OptionalChainExpression::CallLink>(&lnk)) {
            if (prev_is_member) {
                // Stack: [receiver, method, arg0 ... argN-1]
                for (const auto& arg : call->args) {
                    compile_expr(*arg);
                }
                emit(Opcode::kCallMethod);
                emit_u8(static_cast<uint8_t>(call->args.size()));
            } else {
                // Stack: [func, arg0 ... argN-1]
                for (const auto& arg : call->args) {
                    compile_expr(*arg);
                }
                emit(Opcode::kCall);
                emit_u8(static_cast<uint8_t>(call->args.size()));
            }
            if (is_last_delete) {
                emit(Opcode::kPop);
                emit(Opcode::kLoadTrue);
            }
        }
    }

    // Patch all chain_end jumps to here
    for (size_t patch : chain_end_patches) {
        patch_jump(patch);
    }
}

// ============================================================
// class 编译
// ============================================================

// Build a BytecodeFunction that initializes instance fields on 'this'.
// For each field: kLoadThis → (val or kLoadUndefined) → kSetProp/kSetElem → kPop
std::shared_ptr<BytecodeFunction> Compiler::compile_field_initializer(
    const std::vector<ClassField>& fields) {
    auto fi = std::make_shared<BytecodeFunction>();
    fi->name = std::string("$__field_init__");
    // Temporarily switch compiler context to fi
    auto* outer = current_;
    auto saved_name_index = std::move(name_index_);
    auto saved_loop_env = std::move(loop_env_stack_);
    auto saved_finally_info = std::move(finally_info_stack_);
    name_index_.clear();
    loop_env_stack_.clear();
    finally_info_stack_.clear();
    current_ = fi.get();

    for (const auto& f : fields) {
        if (f.is_static) continue;
        if (f.is_private) {
            // Private instance field: use symbol key
            // stack: kLoadThis → init_or_undef → symbol → kSetElem → kPop
            uint64_t sym_id = 0;
            for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
                auto fit = it->find(f.key);
                if (fit != it->end()) { sym_id = fit->second; break; }
            }
            if (sym_id != 0) {
                emit(Opcode::kLoadThis);
                if (f.initializer != nullptr) {
                    compile_expr(*f.initializer);
                } else {
                    emit(Opcode::kLoadUndefined);
                }
                uint16_t sym_idx = add_constant(Value::symbol(sym_id));
                emit(Opcode::kLoadString);
                emit_u16(sym_idx);
                emit(Opcode::kSetElem);
                emit(Opcode::kPop);
            }
        } else if (f.computed) {
            // stack: kLoadThis(obj) → init_or_undef(val) → key_expr(key) → kSetElem → kPop
            emit(Opcode::kLoadThis);
            if (f.initializer != nullptr) {
                compile_expr(*f.initializer);
            } else {
                emit(Opcode::kLoadUndefined);
            }
            compile_expr(*f.key_expr);
            emit(Opcode::kSetElem);
            emit(Opcode::kPop);
        } else {
            // stack: kLoadThis(obj) → init_or_undef(val) → kSetProp(key) → kPop
            emit(Opcode::kLoadThis);
            if (f.initializer != nullptr) {
                compile_expr(*f.initializer);
            } else {
                emit(Opcode::kLoadUndefined);
            }
            uint16_t key_idx = add_name(f.key);
            emit(Opcode::kSetProp);
            emit_u16(key_idx);
            emit(Opcode::kPop);
        }
    }
    emit(Opcode::kReturnUndefined);

    current_ = outer;
    name_index_ = std::move(saved_name_index);
    loop_env_stack_ = std::move(saved_loop_env);
    finally_info_stack_ = std::move(saved_finally_info);
    return fi;
}

void Compiler::compile_class_common(
    const std::optional<std::unique_ptr<ExprNode>>& super_class,
    const std::vector<ClassMethod>& methods,
    const std::vector<ClassField>& fields,
    const std::optional<std::string>& class_name) {

    // Collect private fields/methods, allocate compile-time symbols
    std::unordered_map<std::string, uint64_t> class_private_fields;
    for (const auto& f : fields) {
        if (f.is_private) {
            class_private_fields[f.key] = symbol_table_.NewSymbol(f.key);
        }
    }
    for (const auto& m : methods) {
        if (m.is_private) {
            class_private_fields[m.key] = symbol_table_.NewSymbol(m.key);
        }
    }
    private_fields_stack_.push_back(class_private_fields);

    bool has_super = super_class.has_value();

    // 1. 若有 extends，先求 super class；否则 push undefined
    if (has_super) {
        compile_expr(*super_class->get());
    } else {
        emit(Opcode::kLoadUndefined);
    }

    // 2. 找 constructor 方法（method_kind == kData 表示 constructor）
    const ClassMethod* ctor_method = nullptr;
    for (const auto& m : methods) {
        if (!m.computed && m.key == "constructor" && !m.is_static &&
            m.method_kind == MethodKind::kData) {
            ctor_method = &m;
            break;
        }
    }

    // 2b. 编译 instance field initializer（如有）
    bool has_instance_fields = false;
    for (const auto& f : fields) {
        if (!f.is_static) { has_instance_fields = true; break; }
    }
    std::shared_ptr<BytecodeFunction> field_init_bc;
    if (has_instance_fields) {
        field_init_bc = compile_field_initializer(fields);
    }

    // 3. 编译 constructor 函数体，产生 BytecodeFunction
    uint16_t fn_idx;
    if (ctor_method != nullptr) {
        const auto& fe = std::get<FunctionExpression>(ctor_method->fn_expr->v);
        auto child = compile_function(class_name, fe.params, *fe.body, false, fe.rest_param);
        child->is_class_ctor = true;
        child->is_derived_ctor = has_super;
        if (field_init_bc) child->field_initializer = field_init_bc;
        fn_idx = add_function(std::move(child));
    } else if (has_super) {
        auto child = compile_function(class_name, {}, {}, false,
                                      std::optional<std::string>{"$__class_impl_args__"});
        child->is_class_ctor = true;
        child->is_derived_ctor = true;
        child->is_implicit_derived_ctor = true;
        if (field_init_bc) child->field_initializer = field_init_bc;
        fn_idx = add_function(std::move(child));
    } else {
        // Base class with implicit constructor: empty body
        auto child = compile_function(class_name, {}, {}, false, std::nullopt);
        child->is_class_ctor = true;
        child->is_derived_ctor = false;
        if (field_init_bc) child->field_initializer = field_init_bc;
        fn_idx = add_function(std::move(child));
    }

    // 4. kMakeClass fn_idx：消耗 stack[super_or_undef]，产生 stack[ctor]
    emit(Opcode::kMakeClass);
    emit_u16(fn_idx);
    // Stack: [ctor]

    // ---- 辅助 lambda：编译一个方法的函数表达式并 push 到栈 ----
    auto emit_method_fn = [&](const ClassMethod& m) {
        if (std::holds_alternative<FunctionExpression>(m.fn_expr->v)) {
            const auto& fe = std::get<FunctionExpression>(m.fn_expr->v);
            auto child = compile_function(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                fe.params, *fe.body, false, fe.rest_param);
            child->is_method = true;
            child->is_generator = fe.is_generator;
            uint16_t mfn_idx = add_function(std::move(child));
            emit(Opcode::kMakeFunction);
            emit_u16(mfn_idx);
        } else if (std::holds_alternative<AsyncFunctionExpression>(m.fn_expr->v)) {
            const auto& afe = std::get<AsyncFunctionExpression>(m.fn_expr->v);
            auto child = compile_function(
                m.computed ? std::nullopt : std::optional<std::string>{m.key},
                afe.params, *afe.body, false, afe.rest_param);
            child->is_method = true;
            child->is_async = true;
            uint16_t mfn_idx = add_function(std::move(child));
            emit(Opcode::kMakeFunction);
            emit_u16(mfn_idx);
        } else {
            emit(Opcode::kLoadUndefined);
        }
    };

    // 5. 挂载非 static 方法到 ctor.prototype
    //    Stack pattern per method: [ctor] → Dup → GetProp "prototype" → [ctor, proto]
    //    Then per non-static method:
    //      - non-computed: [ctor, proto, fn] → SetProp/DefineGetter/DefineSetter → [ctor, proto]
    //      - computed: [ctor, proto] → compile key → [ctor, proto, key] → fn → [ctor, proto, key, fn] → SetComputedProp → [ctor, proto]

    bool has_instance_methods = false;
    for (const auto& m : methods) {
        if (!m.is_static && m.method_kind != MethodKind::kData) {
            has_instance_methods = true; break;
        }
    }

    if (has_instance_methods) {
        uint16_t proto_name_idx = add_name("prototype");
        emit(Opcode::kDup);           // [ctor, ctor]
        emit(Opcode::kGetProp);       // [ctor, proto]
        emit_u16(proto_name_idx);

        for (const auto& m : methods) {
            if (m.is_static || m.method_kind == MethodKind::kData) continue;

            if (m.computed) {
                // [ctor, proto] → Dup → [ctor, proto, proto] → key → fn → SetComputedProp → Pop
                emit(Opcode::kDup);
                compile_expr(*m.key_expr);
                emit_method_fn(m);
                // [ctor, proto, proto, key, fn] → SetHomeObject: fn.home = proto (TOS-3 = proto)
                emit(Opcode::kSetHomeObject);
                if (m.method_kind == MethodKind::kGetter) {
                    // Class getter: enumerable=false (use existing kDefineComputedGetter but need to patch)
                    emit(Opcode::kDefineComputedGetter);
                } else if (m.method_kind == MethodKind::kSetter) {
                    emit(Opcode::kDefineComputedSetter);
                } else {
                    emit(Opcode::kDefineComputedClassMethod);
                }
                emit(Opcode::kPop);  // pop leftover fn/val
            } else {
                // [ctor, proto] → Dup → [ctor, proto, proto] → fn → [ctor, proto, proto, fn]
                emit(Opcode::kDup);
                emit_method_fn(m);
                // SetHomeObject: fn.home = proto (TOS-1 = proto_dup)
                emit(Opcode::kSetHomeObject);
                // [ctor, proto, proto_dup, fn] → SetProp/DefineGetter/DefineSetter → [ctor, proto, fn] → Pop → [ctor, proto]
                if (m.method_kind == MethodKind::kGetter) {
                    // Class getter: use same as object getter (enumerable=false for class)
                    // Actually object kDefineGetter uses enumerable=true. For class, override:
                    uint16_t key_idx = add_name(m.key);
                    emit(Opcode::kDefineGetter);
                    emit_u16(key_idx);
                } else if (m.method_kind == MethodKind::kSetter) {
                    uint16_t key_idx = add_name(m.key);
                    emit(Opcode::kDefineSetter);
                    emit_u16(key_idx);
                } else {
                    // Class method: enumerable=false, writable=true, configurable=true
                    uint16_t key_idx = add_name(m.key);
                    emit(Opcode::kDefineClassMethod);
                    emit_u16(key_idx);
                }
                emit(Opcode::kPop);
            }
        }
        // Pop proto
        emit(Opcode::kPop);
        // Stack: [ctor]
    }

    // 6. 挂载 static 方法到 ctor 本身
    for (const auto& m : methods) {
        if (!m.is_static) continue;

        if (m.computed) {
            emit(Opcode::kDup);
            compile_expr(*m.key_expr);
            emit_method_fn(m);
            emit(Opcode::kSetHomeObjectStatic);
            if (m.method_kind == MethodKind::kGetter) {
                emit(Opcode::kDefineComputedGetter);
            } else if (m.method_kind == MethodKind::kSetter) {
                emit(Opcode::kDefineComputedSetter);
            } else {
                emit(Opcode::kSetComputedProp);
            }
            emit(Opcode::kPop);
        } else {
            // [ctor] → Dup → [ctor, ctor] → fn → [ctor, ctor, fn]
            emit(Opcode::kDup);
            emit_method_fn(m);
            emit(Opcode::kSetHomeObjectStatic);
            // [ctor, ctor, fn] → SetProp → [ctor, fn] → Pop → [ctor]
            if (m.method_kind == MethodKind::kGetter) {
                uint16_t key_idx = add_name(m.key);
                emit(Opcode::kDefineGetter);
                emit_u16(key_idx);
            } else if (m.method_kind == MethodKind::kSetter) {
                uint16_t key_idx = add_name(m.key);
                emit(Opcode::kDefineSetter);
                emit_u16(key_idx);
            } else {
                uint16_t key_idx = add_name(m.key);
                emit(Opcode::kSetProp);
                emit_u16(key_idx);
            }
            emit(Opcode::kPop);
        }
    }

    // 7. static fields: [ctor] → Dup → init_val → kSetProp/kSetElem → Pop → [ctor]
    for (const auto& f : fields) {
        if (!f.is_static) continue;
        if (f.is_private) {
            // Static private field: use symbol key via kSetElem
            // kSetElem on kFunction with symbol key stores as "__pfsym_<id>__" in own_properties_
            uint64_t sym_id = 0;
            for (auto it = private_fields_stack_.rbegin(); it != private_fields_stack_.rend(); ++it) {
                auto fit = it->find(f.key);
                if (fit != it->end()) { sym_id = fit->second; break; }
            }
            if (sym_id != 0) {
                emit(Opcode::kDup);
                if (f.initializer != nullptr) {
                    compile_expr(*f.initializer);
                } else {
                    emit(Opcode::kLoadUndefined);
                }
                uint16_t sym_idx = add_constant(Value::symbol(sym_id));
                emit(Opcode::kLoadString);
                emit_u16(sym_idx);
                emit(Opcode::kSetElem);
                emit(Opcode::kPop);
            }
        } else if (f.computed) {
            // [ctor] → Dup → key_expr → init_or_undef → SetElem → Pop
            emit(Opcode::kDup);
            if (f.initializer != nullptr) {
                compile_expr(*f.initializer);
            } else {
                emit(Opcode::kLoadUndefined);
            }
            compile_expr(*f.key_expr);
            emit(Opcode::kSetElem);
            emit(Opcode::kPop);
        } else {
            // [ctor] → Dup → init_or_undef → kSetProp key → Pop
            emit(Opcode::kDup);
            if (f.initializer != nullptr) {
                compile_expr(*f.initializer);
            } else {
                emit(Opcode::kLoadUndefined);
            }
            uint16_t key_idx = add_name(f.key);
            emit(Opcode::kSetProp);
            emit_u16(key_idx);
            emit(Opcode::kPop);
        }
    }
    // Stack: [ctor]
    private_fields_stack_.pop_back();
}

void Compiler::compile_class_expr(const ClassExpression& expr) {
    // 若有类名，创建类名作用域（命名类表达式）
    bool is_named = expr.name.has_value();
    if (is_named) {
        emit(Opcode::kPushScope);
        uint16_t name_idx = add_name(*expr.name);
        emit(Opcode::kDefConst);
        emit_u16(name_idx);
        compile_class_common(expr.super_class, expr.methods, expr.fields, expr.name);
        // Stack: [ctor]. Initialize the name binding (InitVar works for both let and const)
        emit(Opcode::kDup);
        emit(Opcode::kInitVar);
        emit_u16(name_idx);
        emit(Opcode::kPop);  // InitVar pushes val back; discard
        emit(Opcode::kPopScope);
        // Stack: [ctor]
    } else {
        compile_class_common(expr.super_class, expr.methods, expr.fields, expr.name);
    }
}

void Compiler::compile_class_decl(const ClassDeclaration& stmt) {
    // Class declaration: create a let-like binding (TDZ), then initialize it
    uint16_t name_idx = add_name(stmt.name);
    emit(Opcode::kDefLet);
    emit_u16(name_idx);
    compile_class_common(stmt.super_class, stmt.methods, stmt.fields, std::optional<std::string>{stmt.name});
    // Stack: [ctor]
    emit(Opcode::kInitVar);
    emit_u16(name_idx);
    emit(Opcode::kPop);
}

}  // namespace qppjs
