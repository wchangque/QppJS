include(FetchContent)

function(qppjs_setup_googletest)
    find_package(GTest CONFIG QUIET)
    if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
        return()
    endif()

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endfunction()
