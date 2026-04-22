add_library(qppjs_core
    base/error.cpp
    runtime/value.cpp
    runtime/environment.cpp
    runtime/js_object.cpp
    runtime/interpreter.cpp
    debug/format.cpp
    frontend/token.cpp
    frontend/lexer.cpp
    frontend/parser.cpp
    frontend/ast_dump.cpp
    vm/compiler.cpp
    vm/vm.cpp
)

target_include_directories(qppjs_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
qppjs_configure_project_target(qppjs_core)

add_executable(qppjs
    main/main.cpp
)

target_link_libraries(qppjs PRIVATE qppjs_core)
qppjs_configure_project_target(qppjs)
