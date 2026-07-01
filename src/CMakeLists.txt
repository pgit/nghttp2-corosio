find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

add_library(nghttp2-corosio
    server.cpp
    server_impl.hpp
    session.cpp
    session_impl.hpp
)

target_include_directories(nghttp2-corosio
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(nghttp2-corosio
    PUBLIC
        Boost::corosio
    PRIVATE
        PkgConfig::NGHTTP2
)
