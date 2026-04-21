#pragma once

#include "qppjs/base/result.h"
#include "qppjs/frontend/ast.h"

#include <string_view>

namespace qppjs {

ParseResult<Program> parse_program(std::string_view source);

}  // namespace qppjs
