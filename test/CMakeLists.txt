include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(nghttp2-corosio-tests
    test_server.cpp
)

target_link_libraries(nghttp2-corosio-tests
    PRIVATE
        nghttp2-corosio
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(nghttp2-corosio-tests)
