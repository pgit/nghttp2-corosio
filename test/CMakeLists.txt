include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(nghttp2-corosio-tests
    request_handlers.cpp
    test_client.cpp
    test_client_async.cpp
    test_server.cpp
    utils.cpp
)

target_link_libraries(nghttp2-corosio-tests
    PRIVATE
        nghttp2-corosio
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(nghttp2-corosio-tests)

# =================================================================================================
#
# Runs the whole test binary under valgrind's memcheck, registered as its own CTest test (rather
# than folded into gtest_discover_tests() above) so the plain, fast run stays the default and this
# one is opt-in-by-selection when valgrind's overhead is worth it (e.g. CI, or chasing a
# regression).
#
# Every Server the test suite constructs is a real, structured value now: ~Server() cancels and
# joins its accept loop, every session, and every request handler synchronously before its
# io_context is destroyed (see detail::TaskGroup, src/task_group.hpp, and the comment on
# Server::~Server() in server.hpp) -- nothing is deliberately leaked anymore, and no thread is
# spawned either. So unlike when this test only had to tolerate "still reachable"/"possibly lost"
# noise from a leaked Server and detached worker threads, the whole suite is now expected to be
# genuinely leak-free end to end (confirmed: "All heap blocks were freed -- no leaks are
# possible"), and --errors-for-leak-kinds is deliberately strict about it.
#
# Configure with -DENABLE_VALGRIND=ON for the most accurate stack traces (see top-level
# CMakeLists.txt); this test itself works with any build.
#
# =================================================================================================

find_program(VALGRIND_EXECUTABLE valgrind)
if (VALGRIND_EXECUTABLE)
    add_test(
        NAME ValgrindMemcheck
        COMMAND ${VALGRIND_EXECUTABLE}
            --error-exitcode=1
            --leak-check=full
            --show-leak-kinds=definite,indirect,possible
            --errors-for-leak-kinds=definite,indirect,possible
            --track-origins=yes
            $<TARGET_FILE:nghttp2-corosio-tests>
    )
else()
    message(STATUS "valgrind not found -- ValgrindMemcheck test will not be registered")
endif()
