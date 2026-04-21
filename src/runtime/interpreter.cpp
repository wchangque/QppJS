#include "qppjs/runtime/interpreter.h"

#include "qppjs/base/error.h"
#include "qppjs/frontend/ast.h"
#include "qppjs/runtime/completion.h"
#include "qppjs/runtime/environment.h"
#include "qppjs/runtime/js_object.h"
#include "qppjs/runtime/value.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

namespace qppjs {

// ============================================================
// Completion
// ============================================================

Completion Completion::normal(Value v) { return Completion{CompletionType::kNormal, std::move(v)}; }

Completion Completion::return_(Value v) { return Completion{CompletionType::kReturn, std::move(v)}; }

bool Completion::is_normal() const { return type == CompletionType::kNormal; }

bool Completion::is_return() const { return type == CompletionType::kReturn; }

// ============================================================
// EvalResult
// ============================================================

EvalResult EvalResult::ok(Value v) { return EvalResult{std::variant<Value, Error>(std::move(v))}; }

EvalResult EvalResult::err(Error e) { return EvalResult{std::variant<Value, Error>(std::move(e))}; }

bool EvalResult::is_ok() const { return std::holds_alternative<Value>(data); }

Value& EvalResult::value() { return std::get<Value>(data); }

const Value& EvalResult::value() const { return std::get<Value>(data); }

Error& EvalResult::error() { return std::get<Error>(data); }

const Error& EvalResult::error() const { return std::get<Error>(data); }

// ============================================================
// StmtResult
// ============================================================

StmtResult StmtResult::ok(Completion c) { return StmtResult{std::variant<Completion, Error>(std::move(c))}; }

StmtResult StmtResult::err(Error e) { return StmtResult{std::variant<Completion, Error>(std::move(e))}; }

bool StmtResult::is_ok() const { return std::holds_alternative<Completion>(data); }

Completion& StmtResult::completion() { return std::get<Completion>(data); }

const Completion& StmtResult::completion() const { return std::get<Completion>(data); }

Error& StmtResult::error() { return std::get<Error>(data); }

const Error& StmtResult::error() const { return std::get<Error>(data); }

// ============================================================
// ScopeGuard
// ============================================================

Interpreter::ScopeGuard::ScopeGuard(Interpreter& i, Environment* new_env) : interp(i), saved(i.current_env_) {
    interp.current_env_ = new_env;
}

Interpreter::ScopeGuard::~ScopeGuard() { interp.current_env_ = saved; }

// ============================================================
// Interpreter
// ============================================================

Interpreter::Interpreter() : global_env_(nullptr), current_env_(&global_env_) {}

// ---- Type conversions ----

bool Interpreter::to_boolean(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return false;
    case ValueKind::Null:
        return false;
    case ValueKind::Bool:
        return v.as_bool();
    case ValueKind::Number: {
        double n = v.as_number();
        return n != 0.0 && !std::isnan(n);
    }
    case ValueKind::String:
        return !v.as_string().empty();
    case ValueKind::Object:
        return true;
    }
    return false;
}

EvalResult Interpreter::to_number(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
    case ValueKind::Null:
        return EvalResult::ok(Value::number(0.0));
    case ValueKind::Bool:
        return EvalResult::ok(Value::number(v.as_bool() ? 1.0 : 0.0));
    case ValueKind::Number:
        return EvalResult::ok(v);
    case ValueKind::String: {
        const std::string& s = v.as_string();
        if (s.empty()) {
            return EvalResult::ok(Value::number(0.0));
        }
        // Use strtod to avoid exceptions
        char* end = nullptr;
        double result = std::strtod(s.c_str(), &end);
        // If end didn't advance to end-of-string, it's NaN
        if (end == s.c_str() || *end != '\0') {
            return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
        }
        return EvalResult::ok(Value::number(result));
    }
    case ValueKind::Object:
        return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
    }
    return EvalResult::ok(Value::number(std::numeric_limits<double>::quiet_NaN()));
}

std::string Interpreter::to_string_val(const Value& v) {
    switch (v.kind()) {
    case ValueKind::Undefined:
        return "undefined";
    case ValueKind::Null:
        return "null";
    case ValueKind::Bool:
        return v.as_bool() ? "true" : "false";
    case ValueKind::Number: {
        double n = v.as_number();
        if (std::isnan(n)) {
            return "NaN";
        }
        if (std::isinf(n)) {
            return n > 0 ? "Infinity" : "-Infinity";
        }
        // Show integer values without decimal point
        if (n == static_cast<double>(static_cast<long long>(n)) && std::abs(n) < 1e15) {
            std::ostringstream oss;
            oss << static_cast<long long>(n);
            return oss.str();
        }
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    case ValueKind::String:
        return v.as_string();
    case ValueKind::Object:
        return "[object Object]";
    }
    return "undefined";
}

// ---- Var hoisting ----

void Interpreter::hoist_vars(const std::vector<StmtNode>& stmts) {
    for (const auto& stmt : stmts) {
        if (std::holds_alternative<VariableDeclaration>(stmt.v)) {
            const auto& decl = std::get<VariableDeclaration>(stmt.v);
            if (decl.kind == VarKind::Var) {
                global_env_.define_initialized(decl.name);
            } else {
                // let / const: pre-declare TDZ binding so access before the declaration throws ReferenceError
                current_env_->define(decl.name, decl.kind);
            }
        }
        // Do not recurse into BlockStatement for var hoisting at top-level
    }
}

// ---- exec ----

EvalResult Interpreter::exec(const Program& program) {
    hoist_vars(program.body);

    Value last = Value::undefined();
    for (const auto& stmt : program.body) {
        auto result = eval_stmt(stmt);
        if (!result.is_ok()) {
            return EvalResult::err(result.error());
        }
        const Completion& c = result.completion();
        if (c.is_return()) {
            return EvalResult::ok(c.value);
        }
        if (c.is_normal()) {
            last = c.value;
        }
    }
    return EvalResult::ok(last);
}

// ---- Statement dispatch ----

StmtResult Interpreter::eval_stmt(const StmtNode& stmt) {
    return std::visit(
        overloaded{
            [this](const ExpressionStatement& s) { return eval_expression_stmt(s); },
            [this](const VariableDeclaration& s) { return eval_var_decl(s); },
            [this](const BlockStatement& s) { return eval_block_stmt(s); },
            [this](const IfStatement& s) { return eval_if_stmt(s); },
            [this](const WhileStatement& s) { return eval_while_stmt(s); },
            [this](const ReturnStatement& s) { return eval_return_stmt(s); },
        },
        stmt.v);
}

StmtResult Interpreter::eval_expression_stmt(const ExpressionStatement& stmt) {
    auto result = eval_expr(stmt.expr);
    if (!result.is_ok()) {
        return StmtResult::err(result.error());
    }
    return StmtResult::ok(Completion::normal(result.value()));
}

StmtResult Interpreter::eval_var_decl(const VariableDeclaration& decl) {
    if (decl.kind == VarKind::Var) {
        // var: binding already hoisted; just assign if there is an initializer
        if (decl.init.has_value()) {
            auto init_result = eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            auto set_result = current_env_->set(decl.name, init_result.value());
            if (!set_result.is_ok()) {
                return StmtResult::err(set_result.error());
            }
        }
    } else {
        // let / const: create TDZ binding in current scope, then initialize
        current_env_->define(decl.name, decl.kind);
        if (decl.init.has_value()) {
            auto init_result = eval_expr(decl.init.value());
            if (!init_result.is_ok()) {
                return StmtResult::err(init_result.error());
            }
            auto init_env_result = current_env_->initialize(decl.name, init_result.value());
            if (!init_env_result.is_ok()) {
                return StmtResult::err(init_env_result.error());
            }
        } else {
            // No initializer: immediately initialize to undefined, TDZ ends (ECMAScript §14.3.1.1 step 3.b.i)
            current_env_->initialize(decl.name, Value::undefined());
        }
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_block_stmt(const BlockStatement& stmt) {
    Environment block_env(current_env_);
    ScopeGuard guard(*this, &block_env);

    // Hoist var declarations inside the block to the global env;
    // pre-declare let/const bindings as TDZ so access before declaration throws ReferenceError.
    for (const auto& s : stmt.body) {
        if (std::holds_alternative<VariableDeclaration>(s.v)) {
            const auto& decl = std::get<VariableDeclaration>(s.v);
            if (decl.kind == VarKind::Var) {
                global_env_.define_initialized(decl.name);
            } else {
                block_env.define(decl.name, decl.kind);
            }
        }
    }

    Value last = Value::undefined();
    for (const auto& s : stmt.body) {
        auto result = eval_stmt(s);
        if (!result.is_ok()) {
            return result;
        }
        const Completion& c = result.completion();
        if (c.is_return()) {
            return result;  // propagate return upward
        }
        last = c.value;
    }
    return StmtResult::ok(Completion::normal(last));
}

StmtResult Interpreter::eval_if_stmt(const IfStatement& stmt) {
    auto test_result = eval_expr(stmt.test);
    if (!test_result.is_ok()) {
        return StmtResult::err(test_result.error());
    }
    bool cond = to_boolean(test_result.value());
    if (cond) {
        auto result = eval_stmt(*stmt.consequent);
        if (!result.is_ok() || result.completion().is_return()) {
            return result;
        }
        return result;
    }
    if (stmt.alternate != nullptr) {
        auto result = eval_stmt(*stmt.alternate);
        if (!result.is_ok() || result.completion().is_return()) {
            return result;
        }
        return result;
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_while_stmt(const WhileStatement& stmt) {
    while (true) {
        auto test_result = eval_expr(stmt.test);
        if (!test_result.is_ok()) {
            return StmtResult::err(test_result.error());
        }
        if (!to_boolean(test_result.value())) {
            break;
        }
        auto body_result = eval_stmt(*stmt.body);
        if (!body_result.is_ok()) {
            return body_result;
        }
        if (body_result.completion().is_return()) {
            return body_result;
        }
    }
    return StmtResult::ok(Completion::normal(Value::undefined()));
}

StmtResult Interpreter::eval_return_stmt(const ReturnStatement& stmt) {
    if (stmt.argument.has_value()) {
        auto result = eval_expr(stmt.argument.value());
        if (!result.is_ok()) {
            return StmtResult::err(result.error());
        }
        return StmtResult::ok(Completion::return_(result.value()));
    }
    return StmtResult::ok(Completion::return_(Value::undefined()));
}

// ---- Expression dispatch ----

EvalResult Interpreter::eval_expr(const ExprNode& expr) {
    return std::visit(
        overloaded{
            [](const NumberLiteral& e) { return EvalResult::ok(Value::number(e.value)); },
            [](const StringLiteral& e) { return EvalResult::ok(Value::string(e.value)); },
            [](const BooleanLiteral& e) { return EvalResult::ok(Value::boolean(e.value)); },
            [](const NullLiteral&) { return EvalResult::ok(Value::null()); },
            [this](const Identifier& e) { return eval_identifier(e); },
            [this](const UnaryExpression& e) { return eval_unary(e); },
            [this](const BinaryExpression& e) { return eval_binary(e); },
            [this](const LogicalExpression& e) { return eval_logical(e); },
            [this](const AssignmentExpression& e) { return eval_assignment(e); },
            [this](const ObjectExpression& e) { return eval_object_expr(e); },
            [this](const MemberExpression& e) { return eval_member_expr(e); },
            [this](const MemberAssignmentExpression& e) { return eval_member_assign(e); },
        },
        expr.v);
}

EvalResult Interpreter::eval_identifier(const Identifier& expr) {
    // "undefined" is a global identifier that evaluates to undefined
    if (expr.name == "undefined") {
        return EvalResult::ok(Value::undefined());
    }
    return current_env_->get(expr.name);
}

EvalResult Interpreter::eval_unary(const UnaryExpression& expr) {
    // typeof special case: must not throw for undeclared identifiers
    if (expr.op == UnaryOp::Typeof) {
        if (std::holds_alternative<Identifier>(expr.operand->v)) {
            const auto& id = std::get<Identifier>(expr.operand->v);
            // "undefined" identifier
            if (id.name == "undefined") {
                return EvalResult::ok(Value::string("undefined"));
            }
            Binding* b = current_env_->lookup(id.name);
            if (b == nullptr) {
                // Undeclared variable: typeof special rule, return "undefined" without throwing
                return EvalResult::ok(Value::string("undefined"));
            }
            if (!b->initialized) {
                // TDZ: typeof still throws ReferenceError (ECMAScript §13.5.3.1)
                return EvalResult::err(Error(ErrorKind::Runtime,
                    "ReferenceError: Cannot access '" + id.name + "' before initialization"));
            }
        }
        // Otherwise fall through to normal evaluation
        auto operand_result = eval_expr(*expr.operand);
        if (!operand_result.is_ok()) {
            return operand_result;
        }
        const Value& val = operand_result.value();
        switch (val.kind()) {
        case ValueKind::Undefined:
            return EvalResult::ok(Value::string("undefined"));
        case ValueKind::Null:
            return EvalResult::ok(Value::string("object"));
        case ValueKind::Bool:
            return EvalResult::ok(Value::string("boolean"));
        case ValueKind::Number:
            return EvalResult::ok(Value::string("number"));
        case ValueKind::String:
            return EvalResult::ok(Value::string("string"));
        case ValueKind::Object:
            return EvalResult::ok(Value::string("object"));
        }
        return EvalResult::ok(Value::string("undefined"));
    }

    if (expr.op == UnaryOp::Void) {
        auto operand_result = eval_expr(*expr.operand);
        if (!operand_result.is_ok()) {
            return operand_result;
        }
        return EvalResult::ok(Value::undefined());
    }

    auto operand_result = eval_expr(*expr.operand);
    if (!operand_result.is_ok()) {
        return operand_result;
    }
    const Value& val = operand_result.value();

    switch (expr.op) {
    case UnaryOp::Minus: {
        auto num_result = to_number(val);
        if (!num_result.is_ok()) {
            return num_result;
        }
        return EvalResult::ok(Value::number(-num_result.value().as_number()));
    }
    case UnaryOp::Plus: {
        return to_number(val);
    }
    case UnaryOp::Bang:
        return EvalResult::ok(Value::boolean(!to_boolean(val)));
    default:
        break;
    }
    return EvalResult::ok(Value::undefined());
}

// Strict equality (===)
static bool strict_eq(const Value& a, const Value& b) {
    if (a.kind() != b.kind()) {
        return false;
    }
    switch (a.kind()) {
    case ValueKind::Undefined:
        return true;
    case ValueKind::Null:
        return true;
    case ValueKind::Bool:
        return a.as_bool() == b.as_bool();
    case ValueKind::Number: {
        double na = a.as_number();
        double nb = b.as_number();
        if (std::isnan(na) || std::isnan(nb)) {
            return false;
        }
        return na == nb;
    }
    case ValueKind::String:
        return a.as_string() == b.as_string();
    case ValueKind::Object:
        return a.as_object() == b.as_object();
    }
    return false;
}

// Abstract equality (==) — only primitive subset
static bool abstract_eq(const Value& a, const Value& b) {
    // Same type: use strict equality rules
    if (a.kind() == b.kind()) {
        return strict_eq(a, b);
    }
    // null == undefined  /  undefined == null
    bool a_nullish = a.is_null() || a.is_undefined();
    bool b_nullish = b.is_null() || b.is_undefined();
    if (a_nullish && b_nullish) {
        return true;
    }
    if (a_nullish || b_nullish) {
        return false;
    }
    // Boolean: convert to number, recurse
    if (a.is_bool()) {
        return abstract_eq(Value::number(a.as_bool() ? 1.0 : 0.0), b);
    }
    if (b.is_bool()) {
        return abstract_eq(a, Value::number(b.as_bool() ? 1.0 : 0.0));
    }
    // String == Number: convert string to number, recurse
    if (a.is_string() && b.is_number()) {
        char* end = nullptr;
        const std::string& s = a.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0')) {
            n = std::numeric_limits<double>::quiet_NaN();
        }
        return abstract_eq(Value::number(n), b);
    }
    if (a.is_number() && b.is_string()) {
        char* end = nullptr;
        const std::string& s = b.as_string();
        double n = s.empty() ? 0.0 : std::strtod(s.c_str(), &end);
        if (!s.empty() && (end == s.c_str() || *end != '\0')) {
            n = std::numeric_limits<double>::quiet_NaN();
        }
        return abstract_eq(a, Value::number(n));
    }
    return false;
}

EvalResult Interpreter::eval_binary(const BinaryExpression& expr) {
    auto left_result = eval_expr(*expr.left);
    if (!left_result.is_ok()) {
        return left_result;
    }
    auto right_result = eval_expr(*expr.right);
    if (!right_result.is_ok()) {
        return right_result;
    }

    const Value& lv = left_result.value();
    const Value& rv = right_result.value();

    switch (expr.op) {
    case BinaryOp::Add: {
        // If either side is String, concatenate
        if (lv.is_string() || rv.is_string()) {
            return EvalResult::ok(Value::string(to_string_val(lv) + to_string_val(rv)));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() + rn.value().as_number()));
    }
    case BinaryOp::Sub: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() - rn.value().as_number()));
    }
    case BinaryOp::Mul: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() * rn.value().as_number()));
    }
    case BinaryOp::Div: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(ln.value().as_number() / rn.value().as_number()));
    }
    case BinaryOp::Mod: {
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        return EvalResult::ok(Value::number(std::fmod(ln.value().as_number(), rn.value().as_number())));
    }
    case BinaryOp::Lt: {
        // Both strings: lexicographic comparison (ECMAScript §13.11 AbstractRelationalComparison)
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() < rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum < rnum));
    }
    case BinaryOp::Gt: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() > rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum > rnum));
    }
    case BinaryOp::LtEq: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() <= rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum <= rnum));
    }
    case BinaryOp::GtEq: {
        if (lv.is_string() && rv.is_string()) {
            return EvalResult::ok(Value::boolean(lv.as_string() >= rv.as_string()));
        }
        auto ln = to_number(lv);
        if (!ln.is_ok()) {
            return ln;
        }
        auto rn = to_number(rv);
        if (!rn.is_ok()) {
            return rn;
        }
        double lnum = ln.value().as_number();
        double rnum = rn.value().as_number();
        if (std::isnan(lnum) || std::isnan(rnum)) {
            return EvalResult::ok(Value::boolean(false));
        }
        return EvalResult::ok(Value::boolean(lnum >= rnum));
    }
    case BinaryOp::EqEqEq:
        return EvalResult::ok(Value::boolean(strict_eq(lv, rv)));
    case BinaryOp::NotEqEq:
        return EvalResult::ok(Value::boolean(!strict_eq(lv, rv)));
    case BinaryOp::EqEq:
        return EvalResult::ok(Value::boolean(abstract_eq(lv, rv)));
    case BinaryOp::NotEq:
        return EvalResult::ok(Value::boolean(!abstract_eq(lv, rv)));
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_logical(const LogicalExpression& expr) {
    auto left_result = eval_expr(*expr.left);
    if (!left_result.is_ok()) {
        return left_result;
    }
    const Value& lv = left_result.value();

    switch (expr.op) {
    case LogicalOp::And:
        if (!to_boolean(lv)) {
            return left_result;
        }
        return eval_expr(*expr.right);
    case LogicalOp::Or:
        if (to_boolean(lv)) {
            return left_result;
        }
        return eval_expr(*expr.right);
    }
    return EvalResult::ok(Value::undefined());
}

EvalResult Interpreter::eval_assignment(const AssignmentExpression& expr) {
    if (expr.op == AssignOp::Assign) {
        auto rhs = eval_expr(*expr.value);
        if (!rhs.is_ok()) {
            return rhs;
        }
        auto set_result = current_env_->set(expr.target, rhs.value());
        if (!set_result.is_ok()) {
            return set_result;
        }
        return rhs;
    }

    // Compound assignment: read current value, compute, write back
    auto current_result = current_env_->get(expr.target);
    if (!current_result.is_ok()) {
        return current_result;
    }
    auto rhs = eval_expr(*expr.value);
    if (!rhs.is_ok()) {
        return rhs;
    }

    Value new_val = Value::undefined();
    switch (expr.op) {
    case AssignOp::AddAssign: {
        const Value& lv = current_result.value();
        const Value& rv = rhs.value();
        if (lv.is_string() || rv.is_string()) {
            new_val = Value::string(to_string_val(lv) + to_string_val(rv));
        } else {
            auto ln = to_number(lv);
            auto rn = to_number(rv);
            if (!ln.is_ok()) {
                return ln;
            }
            if (!rn.is_ok()) {
                return rn;
            }
            new_val = Value::number(ln.value().as_number() + rn.value().as_number());
        }
        break;
    }
    case AssignOp::SubAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() - rn.value().as_number());
        break;
    }
    case AssignOp::MulAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() * rn.value().as_number());
        break;
    }
    case AssignOp::DivAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(ln.value().as_number() / rn.value().as_number());
        break;
    }
    case AssignOp::ModAssign: {
        auto ln = to_number(current_result.value());
        auto rn = to_number(rhs.value());
        if (!ln.is_ok()) {
            return ln;
        }
        if (!rn.is_ok()) {
            return rn;
        }
        new_val = Value::number(std::fmod(ln.value().as_number(), rn.value().as_number()));
        break;
    }
    default:
        break;
    }

    auto set_result = current_env_->set(expr.target, new_val);
    if (!set_result.is_ok()) {
        return set_result;
    }
    return EvalResult::ok(new_val);
}

EvalResult Interpreter::eval_object_expr(const ObjectExpression& expr) {
    auto obj = std::make_shared<JSObject>();
    for (const auto& prop : expr.properties) {
        auto val = eval_expr(*prop.value);
        if (!val.is_ok()) {
            return val;
        }
        obj->set_property(prop.key, val.value());
    }
    return EvalResult::ok(Value::object(obj));
}

EvalResult Interpreter::eval_member_expr(const MemberExpression& expr) {
    auto obj_result = eval_expr(*expr.object);
    if (!obj_result.is_ok()) {
        return obj_result;
    }
    const Value& obj_val = obj_result.value();

    if (obj_val.is_undefined() || obj_val.is_null()) {
        return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Cannot read properties of " + to_string_val(obj_val)));
    }

    // 非对象：Phase 3 返回 undefined（Phase 5 补原始值包装）
    if (!obj_val.is_object()) {
        return EvalResult::ok(Value::undefined());
    }

    auto key_result = eval_expr(*expr.property);
    if (!key_result.is_ok()) {
        return key_result;
    }
    std::string key = to_string_val(key_result.value());

    assert(obj_val.as_object()->object_kind() == ObjectKind::kOrdinary);
    auto* js_obj = static_cast<JSObject*>(obj_val.as_object().get());
    return EvalResult::ok(js_obj->get_property(key));
}

EvalResult Interpreter::eval_member_assign(const MemberAssignmentExpression& expr) {
    auto obj_result = eval_expr(*expr.object);
    if (!obj_result.is_ok()) {
        return obj_result;
    }
    const Value& obj_val = obj_result.value();

    if (obj_val.is_undefined() || obj_val.is_null()) {
        return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Cannot set properties of " + to_string_val(obj_val)));
    }
    if (!obj_val.is_object()) {
        return EvalResult::err(Error(ErrorKind::Runtime,
                "TypeError: Cannot set properties of non-object"));
    }

    auto key_result = eval_expr(*expr.property);
    if (!key_result.is_ok()) {
        return key_result;
    }
    std::string key = to_string_val(key_result.value());

    auto val_result = eval_expr(*expr.value);
    if (!val_result.is_ok()) {
        return val_result;
    }

    assert(obj_val.as_object()->object_kind() == ObjectKind::kOrdinary);
    auto* js_obj = static_cast<JSObject*>(obj_val.as_object().get());
    js_obj->set_property(key, val_result.value());
    return EvalResult::ok(val_result.value());
}

}  // namespace qppjs
