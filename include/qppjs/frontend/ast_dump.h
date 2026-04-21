#pragma once

#include <string>

namespace qppjs {

struct ExprNode;
struct StmtNode;
struct Program;

[[nodiscard]] std::string dump_expr(const ExprNode& node, int indent = 0);
[[nodiscard]] std::string dump_stmt(const StmtNode& node, int indent = 0);
[[nodiscard]] std::string dump_program(const Program& prog);

}  // namespace qppjs
