cmake_minimum_required(VERSION 3.14...4.2)
project(nghttp2-corosio VERSION 1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(boost_corosio REQUIRED)

add_executable(nghttp2-corosio main.cpp)
target_link_libraries(nghttp2-corosio PRIVATE Boost::corosio)

