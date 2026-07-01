cmake_minimum_required(VERSION 3.14...4.2)
project(nghttp2-corosio VERSION 1.0 LANGUAGES CXX)

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

add_executable(nghttp2-corosio main.cpp)
target_link_libraries(nghttp2-corosio PRIVATE Boost::corosio)

