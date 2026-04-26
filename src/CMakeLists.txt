add_library(qppjs_core
    base/error.cpp
    runtime/value.cpp
    runtime/environment.cpp
    runtime/gc_heap.cpp
    runtime/js_function.cpp
    runtime/js_object.cpp
    runtime/native_errors.cpp
    runtime/module_record.cpp
    runtime/module_loader.cpp
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

add_executable(qppjs
    main/main.cpp
)

target_link_libraries(qppjs PRIVATE qppjs_core)
