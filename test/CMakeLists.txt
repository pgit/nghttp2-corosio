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
# Runs the HTTP/2-level echo round trip (ClientAsync.PostData_ReceivesEcho) under valgrind's
# memcheck, registered as its own CTest test so it doesn't pay valgrind's overhead for the whole
# suite. Reuses the real client/server/nghttp2-session code path rather than a synthetic
# reproducer, since the point is to exercise exactly the code the "Known issue" in CLAUDE.md
# describes (a leaked coroutine/socket after a scheduler edge case) under a tool that can actually
# tell a real leak/corruption apart from noise.
#
# ClientAsync deliberately leaks its Server (`new`, never `delete`d -- see test_client_async.cpp)
# to sidestep that known issue instead of triggering it. That leaked Server stays reachable via
# the fixture's `server_` pointer and the detached server thread's captured copy, so valgrind
# reports it as "still reachable", which --errors-for-leak-kinds below doesn't treat as an error.
#
# --errors-for-leak-kinds is narrowed to definite,indirect (memcheck's default also includes
# "possible"): both the detached server/worker threads (glibc's pthread TLS bookkeeping is a
# well-known source of "possibly lost" false positives for detached threads) and capy's
# recycling_memory_resource (coroutine frames/buffers are reached through interior pointers --
# e.g. a coroutine_handle -- that valgrind's conservative scanner can't always prove point to the
# start of a block) produce "possibly lost" blocks on every run that aren't real leaks.
# --show-leak-kinds still includes "possible" so they're visible for manual inspection.
#
# Configure with -DENABLE_VALGRIND=ON for the most accurate stack traces (see top-level
# CMakeLists.txt); this test itself works with any build.
#
# =================================================================================================

find_program(VALGRIND_EXECUTABLE valgrind)
if (VALGRIND_EXECUTABLE)
    add_test(
        NAME EchoLoopValgrind
        COMMAND ${VALGRIND_EXECUTABLE}
            --error-exitcode=1
            --leak-check=full
            --show-leak-kinds=definite,indirect,possible
            --errors-for-leak-kinds=definite,indirect
            --track-origins=yes
            $<TARGET_FILE:nghttp2-corosio-tests> --gtest_filter=ClientAsync.PostData_ReceivesEcho
    )
else()
    message(STATUS "valgrind not found -- EchoLoopValgrind test will not be registered")
endif()
