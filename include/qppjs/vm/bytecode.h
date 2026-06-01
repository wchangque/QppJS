#pragma once

#include "qppjs/frontend/ast.h"
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
    bool is_arrow = false;                                       // true for arrow functions
    bool is_method = false;                                      // true for object literal method shorthand
    bool is_generator = false;                                   // true for generator functions (function*)
    bool is_class_ctor = false;                                  // true for class constructor
    bool is_derived_ctor = false;                                // true for derived class constructor
    bool is_implicit_derived_ctor = false;                       // true for implicit derived ctor (auto super)
    std::optional<std::string> rest_param;                       // rest parameter name (...args)
    std::shared_ptr<std::vector<ParamDef>> param_defs;           // parameter default value definitions
    uint16_t length_count = 0;                                   // number of params before first default
    // If the last statement is a simple identifier expression, its name is stored here
    // (used by VM::exec() to re-read the value after DrainAll)
    std::optional<std::string> last_expr_name;
    // Instance field initializer function (only set for class constructors with instance fields)
    std::shared_ptr<BytecodeFunction> field_initializer;
};

}  // namespace qppjs
