add_executable(qppjs_unit_tests
    unit/error_test.cpp
    unit/value_test.cpp
    unit/token_test.cpp
    unit/lexer_test.cpp
    unit/ast_test.cpp
    unit/parser_test.cpp
    unit/ast_dump_test.cpp
    unit/interpreter_test.cpp
    unit/object_test.cpp
    unit/function_test.cpp
    unit/proto_test.cpp
    unit/vm_test.cpp
    unit/interpreter_control_flow_test.cpp
    unit/vm_control_flow_test.cpp
    unit/interpreter_control_flow_edge_test.cpp
    unit/vm_control_flow_edge_test.cpp
    unit/value_nanboxing_test.cpp
    unit/vm_error_test.cpp
    unit/interpreter_error_test.cpp
    unit/console_test.cpp
    unit/array_test.cpp
    unit/object_builtin_test.cpp
    unit/function_builtin_test.cpp
    unit/env_rc_lifecycle_test.cpp
    unit/gc_heap_test.cpp
    unit/parser_module_test.cpp
    unit/module_test.cpp
    unit/promise_test.cpp
    unit/async_order_test.cpp
    unit/string_test.cpp
    unit/number_math_test.cpp
    unit/tla_test.cpp
    unit/update_expression_test.cpp
    # unit/global_stubs_test.cpp  # FIXME: pre-existing compilation error (is_err not a member of EvalResult)
)

target_link_libraries(qppjs_unit_tests PRIVATE qppjs_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(qppjs_unit_tests)

add_test(NAME qppjs_cli_help COMMAND qppjs --help)
set_tests_properties(qppjs_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: qppjs"
)

add_test(NAME qppjs_cli_eval COMMAND qppjs -e "1+2")
set_tests_properties(qppjs_cli_eval PROPERTIES
    PASS_REGULAR_EXPRESSION "3"
)
