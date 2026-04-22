option(QPPJS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(QPPJS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(QPPJS_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(QPPJS_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(QPPJS_ENABLE_COVERAGE AND QPPJS_ENABLE_ASAN)
    message(FATAL_ERROR "QPPJS_ENABLE_COVERAGE and QPPJS_ENABLE_ASAN cannot both be ON")
endif()

function(qppjs_set_build_metadata name value)
    set(${name} "${value}" CACHE INTERNAL "QppJS build metadata")
endfunction()

function(qppjs_setup_project_targets)
    if(TARGET qppjs_project_options)
        return()
    endif()

    add_library(qppjs_project_options INTERFACE)
    add_library(qppjs_project_warnings INTERFACE)

    target_compile_features(qppjs_project_options INTERFACE cxx_std_20)

    if(WIN32)
        qppjs_set_build_metadata(QPPJS_PLATFORM windows)
    elseif(APPLE)
        qppjs_set_build_metadata(QPPJS_PLATFORM macos)
    else()
        qppjs_set_build_metadata(QPPJS_PLATFORM linux)
    endif()

    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(is_multi_config)
        qppjs_set_build_metadata(QPPJS_IS_MULTI_CONFIG true)
    else()
        qppjs_set_build_metadata(QPPJS_IS_MULTI_CONFIG false)
    endif()
    qppjs_set_build_metadata(QPPJS_GENERATOR_NAME "${CMAKE_GENERATOR}")

    if(MSVC)
        qppjs_set_build_metadata(QPPJS_COMPILER_FAMILY msvc)
        qppjs_set_build_metadata(QPPJS_COVERAGE_BACKEND unsupported)

        target_compile_options(qppjs_project_warnings INTERFACE /W4 /utf-8)
        if(QPPJS_WARNINGS_AS_ERRORS)
            target_compile_options(qppjs_project_warnings INTERFACE /WX)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            qppjs_set_build_metadata(QPPJS_COMPILER_FAMILY gcc)
            qppjs_set_build_metadata(QPPJS_COVERAGE_BACKEND gcov)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            qppjs_set_build_metadata(QPPJS_COMPILER_FAMILY appleclang)
            qppjs_set_build_metadata(QPPJS_COVERAGE_BACKEND llvm-cov)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            qppjs_set_build_metadata(QPPJS_COMPILER_FAMILY clang)
            qppjs_set_build_metadata(QPPJS_COVERAGE_BACKEND llvm-cov)
        else()
            qppjs_set_build_metadata(QPPJS_COMPILER_FAMILY "${CMAKE_CXX_COMPILER_ID}")
            qppjs_set_build_metadata(QPPJS_COVERAGE_BACKEND unsupported)
        endif()

        target_compile_options(qppjs_project_warnings INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
        )
        if(QPPJS_WARNINGS_AS_ERRORS)
            target_compile_options(qppjs_project_warnings INTERFACE -Werror)
        endif()

        if(QPPJS_ENABLE_ASAN)
            target_compile_options(qppjs_project_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(qppjs_project_options INTERFACE -fsanitize=address)
        endif()
        if(QPPJS_ENABLE_UBSAN)
            target_compile_options(qppjs_project_options INTERFACE -fsanitize=undefined)
            target_link_options(qppjs_project_options INTERFACE -fsanitize=undefined)
        endif()
        if(QPPJS_ENABLE_COVERAGE)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(qppjs_project_options INTERFACE --coverage -O0 -g)
                target_link_options(qppjs_project_options INTERFACE --coverage)
            elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
                target_compile_options(qppjs_project_options INTERFACE -fprofile-instr-generate -fcoverage-mapping -O0 -g)
                target_link_options(qppjs_project_options INTERFACE -fprofile-instr-generate -fcoverage-mapping)
            else()
                message(FATAL_ERROR "QPPJS_ENABLE_COVERAGE is unsupported for compiler ${CMAKE_CXX_COMPILER_ID}")
            endif()
        endif()
    endif()
endfunction()

function(qppjs_configure_project_target target)
    target_link_libraries(${target}
        PUBLIC qppjs_project_options
        PRIVATE qppjs_project_warnings
    )
endfunction()

function(qppjs_write_build_metadata)
    if(NOT DEFINED QPPJS_PLATFORM)
        return()
    endif()

    set(metadata_path "${CMAKE_BINARY_DIR}/qppjs-build-meta.json")
    set(test_binary "")
    if(TARGET qppjs_unit_tests)
        set(test_binary "$<TARGET_FILE:qppjs_unit_tests>")
    endif()

    file(GENERATE OUTPUT "${metadata_path}" CONTENT
"{\n  \"platform\": \"${QPPJS_PLATFORM}\",\n  \"compiler_family\": \"${QPPJS_COMPILER_FAMILY}\",\n  \"generator\": \"${QPPJS_GENERATOR_NAME}\",\n  \"is_multi_config\": ${QPPJS_IS_MULTI_CONFIG},\n  \"coverage_enabled\": $<IF:$<BOOL:${QPPJS_ENABLE_COVERAGE}>,true,false>,\n  \"coverage_backend\": \"${QPPJS_COVERAGE_BACKEND}\",\n  \"test_binary\": \"${test_binary}\",\n  \"source_dir\": \"${PROJECT_SOURCE_DIR}\",\n  \"build_dir\": \"${CMAKE_BINARY_DIR}\"\n}\n")
endfunction()
