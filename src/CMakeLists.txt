add_library(qppjs_core
    base/error.cpp
    runtime/value.cpp
    debug/format.cpp
)

target_include_directories(qppjs_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(qppjs_core PUBLIC cxx_std_20)
qppjs_apply_compiler_flags(qppjs_core)

add_executable(qppjs
    main/main.cpp
)

target_link_libraries(qppjs PRIVATE qppjs_core)
qppjs_apply_compiler_flags(qppjs)
