cmake_minimum_required(VERSION 3.20)
project(QppJS VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(Options)
include(CompilerFlags)
include(Dependencies)


# 全局编译选项和链接选项，不能移动
qppjs_setup_project_configuration()

add_subdirectory(src)

if(QPPJS_BUILD_TESTS)
    enable_testing()
    qppjs_setup_googletest()
    add_subdirectory(tests)
endif()

qppjs_write_build_metadata()
