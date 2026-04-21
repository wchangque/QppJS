#include "qppjs/frontend/ast_dump.h"
#include "qppjs/frontend/ast.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <string>

namespace qppjs {

namespace {

std::string ind(int indent) {
    return std::string(static_cast<size_t>(indent) * 2, ' ');
}

std::string format_number(double v) {
    // 整数值不显示小数点；超出 long long 范围则直接用 %g 格式
    if (std::isfinite(v) && std::floor(v) == v
        && v >= static_cast<double>(std::numeric_limits<long long>::min())
        && v <  static_cast<double>(std::numeric_limits<long long>::max()) + 1.0) {
        std::ostringstream ss;
        ss << static_cast<long long>(v);
        return ss.str();
    }
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

const char* unary_op_str(UnaryOp op) {
    switch (op) {
    case UnaryOp::Minus:  return "-";
    case UnaryOp::Plus:   return "+";
    case UnaryOp::Bang:   return "!";
    case UnaryOp::Typeof: return "typeof";
    case UnaryOp::Void:   return "void";
    }
    return "?";
}

const char* binary_op_str(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:     return "+";
    case BinaryOp::Sub:     return "-";
    case BinaryOp::Mul:     return "*";
    case BinaryOp::Div:     return "/";
    case BinaryOp::Mod:     return "%";
    case BinaryOp::Lt:      return "<";
    case BinaryOp::Gt:      return ">";
    case BinaryOp::LtEq:    return "<=";
    case BinaryOp::GtEq:    return ">=";
    case BinaryOp::EqEq:    return "==";
    case BinaryOp::NotEq:   return "!=";
    case BinaryOp::EqEqEq:  return "===";
    case BinaryOp::NotEqEq: return "!==";
    }
    return "?";
}

const char* logical_op_str(LogicalOp op) {
    switch (op) {
    case LogicalOp::And: return "&&";
    case LogicalOp::Or:  return "||";
    }
    return "?";
}

const char* assign_op_str(AssignOp op) {
    switch (op) {
    case AssignOp::Assign:    return "=";
    case AssignOp::AddAssign: return "+=";
    case AssignOp::SubAssign: return "-=";
    case AssignOp::MulAssign: return "*=";
    case AssignOp::DivAssign: return "/=";
    case AssignOp::ModAssign: return "%=";
    }
    return "?";
}

const char* var_kind_str(VarKind kind) {
    switch (kind) {
    case VarKind::Var:   return "var";
    case VarKind::Let:   return "let";
    case VarKind::Const: return "const";
    }
    return "?";
}

} // anonymous namespace

std::string dump_expr(const ExprNode& node, int indent) {
    std::string prefix = ind(indent);
    std::string result;

    std::visit(overloaded{
        [&](const NumberLiteral& n) {
            result = prefix + "NumberLiteral(" + format_number(n.value) + ")\n";
        },
        [&](const StringLiteral& s) {
            result = prefix + "StringLiteral(\"" + s.value + "\")\n";
        },
        [&](const BooleanLiteral& b) {
            result = prefix + "BooleanLiteral(" + (b.value ? "true" : "false") + ")\n";
        },
        [&](const NullLiteral&) {
            result = prefix + "NullLiteral\n";
        },
        [&](const Identifier& id) {
            result = prefix + "Identifier(" + id.name + ")\n";
        },
        [&](const UnaryExpression& ue) {
            result = prefix + "UnaryExpression(" + unary_op_str(ue.op) + ")\n";
            result += dump_expr(*ue.operand, indent + 1);
        },
        [&](const BinaryExpression& be) {
            result = prefix + "BinaryExpression(" + binary_op_str(be.op) + ")\n";
            result += dump_expr(*be.left, indent + 1);
            result += dump_expr(*be.right, indent + 1);
        },
        [&](const LogicalExpression& le) {
            result = prefix + "LogicalExpression(" + logical_op_str(le.op) + ")\n";
            result += dump_expr(*le.left, indent + 1);
            result += dump_expr(*le.right, indent + 1);
        },
        [&](const AssignmentExpression& ae) {
            result = prefix + "AssignmentExpression(" + assign_op_str(ae.op) + ")\n";
            result += ind(indent + 1) + "target: " + ae.target + "\n";
            result += ind(indent + 1) + "value:\n";
            result += dump_expr(*ae.value, indent + 2);
        },
    }, node.v);

    return result;
}

std::string dump_stmt(const StmtNode& node, int indent) {
    std::string prefix = ind(indent);
    std::string result;

    std::visit(overloaded{
        [&](const ExpressionStatement& es) {
            result = prefix + "ExpressionStatement\n";
            result += dump_expr(es.expr, indent + 1);
        },
        [&](const VariableDeclaration& vd) {
            result = prefix + "VariableDeclaration(" + var_kind_str(vd.kind) + ")\n";
            result += ind(indent + 1) + "name: " + vd.name + "\n";
            if (vd.init.has_value()) {
                result += ind(indent + 1) + "init:\n";
                result += dump_expr(*vd.init, indent + 2);
            } else {
                result += ind(indent + 1) + "init: (none)\n";
            }
        },
        [&](const BlockStatement& bs) {
            result = prefix + "BlockStatement\n";
            for (const auto& stmt : bs.body) {
                result += dump_stmt(stmt, indent + 1);
            }
        },
        [&](const IfStatement& is) {
            result = prefix + "IfStatement\n";
            result += ind(indent + 1) + "test:\n";
            result += dump_expr(is.test, indent + 2);
            result += ind(indent + 1) + "consequent:\n";
            result += dump_stmt(*is.consequent, indent + 2);
            if (is.alternate) {
                result += ind(indent + 1) + "alternate:\n";
                result += dump_stmt(*is.alternate, indent + 2);
            } else {
                result += ind(indent + 1) + "alternate: (none)\n";
            }
        },
        [&](const WhileStatement& ws) {
            result = prefix + "WhileStatement\n";
            result += ind(indent + 1) + "test:\n";
            result += dump_expr(ws.test, indent + 2);
            result += ind(indent + 1) + "body:\n";
            result += dump_stmt(*ws.body, indent + 2);
        },
        [&](const ReturnStatement& rs) {
            result = prefix + "ReturnStatement\n";
            if (rs.argument.has_value()) {
                result += ind(indent + 1) + "argument:\n";
                result += dump_expr(*rs.argument, indent + 2);
            } else {
                result += ind(indent + 1) + "argument: (none)\n";
            }
        },
    }, node.v);

    return result;
}

std::string dump_program(const Program& prog) {
    std::string result = "Program\n";
    for (const auto& stmt : prog.body) {
        result += dump_stmt(stmt, 1);
    }
    return result;
}

} // namespace qppjs
