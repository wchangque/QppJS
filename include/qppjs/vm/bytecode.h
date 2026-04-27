#pragma once

#include "qppjs/runtime/value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qppjs {

struct BytecodeFunction {
    std::vector<uint8_t> code;                                  // instruction stream
    std::vector<Value> constants;                               // constant pool (number/string)
    std::vector<std::string> names;                             // name pool (variable/property names)
    std::vector<std::shared_ptr<BytecodeFunction>> functions;   // nested function pool
    std::vector<std::string> params;                            // parameter name list
    std::optional<std::string> name;                            // function name (debug)
    std::vector<uint16_t> var_decls;                            // names indices for var declarations
    std::vector<uint16_t> function_decls;                       // names indices for function declarations
    bool is_named_expr = false;                                  // true for named function expressions
    bool is_async = false;                                       // true for async functions
    // If the last statement is a simple identifier expression, its name is stored here
    // (used by VM::exec() to re-read the value after DrainAll)
    std::optional<std::string> last_expr_name;
};

}  // namespace qppjs
