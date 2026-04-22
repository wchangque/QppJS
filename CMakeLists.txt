cmake_minimum_required(VERSION 3.20)

project(QppJS VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CompilerFlags)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QPPJS_BUILD_TESTS "Build QppJS tests" ON)

qppjs_setup_project_targets()

add_subdirectory(src)

if(QPPJS_BUILD_TESTS)
    enable_testing()
    include(Dependencies)
    qppjs_setup_googletest()
    add_subdirectory(tests)
endif()

qppjs_write_build_metadata()
