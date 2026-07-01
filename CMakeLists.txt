cmake_minimum_required(VERSION 3.14...4.2)
project(nghttp2-corosio VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_executable(server_main server_main.cpp)
target_link_libraries(server_main PRIVATE nghttp2-corosio)

