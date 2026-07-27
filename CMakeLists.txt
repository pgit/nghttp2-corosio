cmake_minimum_required(VERSION 3.14...4.2)
project(nghttp2-corosio VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_VALGRIND "Build with valgrind-friendly instrumentation (debug info, no optimization)" OFF)

if (ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

if (ENABLE_VALGRIND AND (ENABLE_ASAN OR ENABLE_TSAN))
    message(FATAL_ERROR "ENABLE_VALGRIND is mutually exclusive with ENABLE_ASAN/ENABLE_TSAN "
                         "(sanitizers and valgrind both intercept malloc)")
endif()

if (ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if (ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if (ENABLE_VALGRIND)
    # -O0 keeps memcheck's stack traces and uninitialized-value reports accurate; optimized code
    # can reorder/elide reads in ways valgrind mistakes for real bugs. -g is needed regardless of
    # build type for valgrind to resolve source lines.
    add_compile_options(-g -O0)
endif()

# Note: when using GIT_TAG with a commit hash, GIT_SHALLOW must be set to FALSE.
include(FetchContent)
FetchContent_Declare(capy
    GIT_REPOSITORY https://github.com/cppalliance/capy.git
    GIT_TAG develop
    GIT_SHALLOW TRUE)
FetchContent_Declare(corosio
    GIT_REPOSITORY https://github.com/cppalliance/corosio.git
    GIT_TAG develop
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(capy corosio)

add_subdirectory(src)

add_executable(echo_server main.cpp)
target_link_libraries(echo_server PRIVATE Boost::corosio)

add_executable(server_main server_main.cpp echo_handler.cpp)
target_link_libraries(server_main PRIVATE nghttp2-corosio)

enable_testing()
add_subdirectory(test)

