add_executable(qppjs_unit_tests
    unit/error_test.cpp
    unit/value_test.cpp
    unit/token_test.cpp
    unit/lexer_test.cpp
    unit/ast_test.cpp
    unit/parser_test.cpp
    unit/ast_dump_test.cpp
)

target_link_libraries(qppjs_unit_tests PRIVATE qppjs_core GTest::gtest_main)
qppjs_apply_compiler_flags(qppjs_unit_tests)

include(GoogleTest)
gtest_discover_tests(qppjs_unit_tests)

add_test(NAME qppjs_cli_without_args COMMAND qppjs)
set_tests_properties(qppjs_cli_without_args PROPERTIES
    PASS_REGULAR_EXPRESSION "UsageError: usage: qppjs <source>"
)

add_test(NAME qppjs_cli_with_source COMMAND qppjs "1+2")
set_tests_properties(qppjs_cli_with_source PROPERTIES
    PASS_REGULAR_EXPRESSION "source: 1\\+2"
)
