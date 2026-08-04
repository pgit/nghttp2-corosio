include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

# test_external.cpp is the only translation unit in this project that uses ASIO/Boost.Process.V2
# (see its top comment for why) -- everything else drives I/O through capy/corosio. Boost itself
# isn't FetchContent'd like capy/corosio are; it's expected to already be installed system-wide
# (see the devcontainer image), so this just links against it.
find_package(boost_process 1.86 CONFIG REQUIRED)

add_executable(nghttp2-corosio-tests
    ${CMAKE_SOURCE_DIR}/echo_handler.cpp
    issue_tls_full_duplex.cpp
    request_handlers.cpp
    test_any_read_stream_timeout_bug.cpp
    test_client.cpp
    test_client_async.cpp
    test_external.cpp
    test_server.cpp
    test_tls.cpp
    utils.cpp
)

target_link_libraries(nghttp2-corosio-tests
    PRIVATE
        nghttp2-corosio
        GTest::gtest_main
        Boost::process
        Boost::corosio_openssl
)

# =================================================================================================
#
# TLS tests need a real certificate chain to hand to nghttp2_corosio::Config::tls /
# tls_context::use_certificate_chain_file. pki/create.sh (cfssl, present in the devcontainer
# image, same as anyhttp's own pki/ setup) generates a root -> intermediate -> {server,client}
# chain into pki/out/, gitignored since it's regenerated here rather than committed. Always
# regenerates the whole chain from scratch (see create.sh's own comment for why a per-file
# idempotency check is actively wrong here -- CMake/Ninja can delete a subset of this command's
# declared OUTPUT files out from under it).
#
# =================================================================================================

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/root.pem
        ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/server-fullchain.pem
        ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/server-key.pem
        ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/client-fullchain.pem
        ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/client-key.pem
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/pki/create.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/pki
    COMMENT "Generating test/pki/out/ certificate chain (cfssl)"
    VERBATIM
)
add_custom_target(test_pki_certs DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/root.pem
    ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/server-fullchain.pem
    ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/server-key.pem
    ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/client-fullchain.pem
    ${CMAKE_CURRENT_SOURCE_DIR}/pki/out/client-key.pem
)
add_dependencies(nghttp2-corosio-tests test_pki_certs)

include(GoogleTest)
# WORKING_DIRECTORY matches anyhttp's own test/CMakeLists.txt: External's tests reference test
# fixtures (test/data/64kminus1) and a payload file (this project's own CMakeLists.txt) by a path
# relative to the repo root, not wherever ctest happens to invoke the test binary from.
gtest_discover_tests(nghttp2-corosio-tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
    # Same reason as gtest_discover_tests()'s WORKING_DIRECTORY above: without it, External's
    # fixture-construction-time file_size("CMakeLists.txt") throws before any test body runs
    # (ctest's default working directory for a plain add_test() is this file's own directory, not
    # the repo root), failing all of External's tests -- not a memcheck finding, just a wrong CWD.
    set_tests_properties(ValgrindMemcheck PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    message(STATUS "valgrind not found -- ValgrindMemcheck test will not be registered")
endif()

# =================================================================================================
#
# test/data/1m is a 1 MiB random payload used as a larger h2load body (benchmark/h2load_matrix.sh's
# DATA env var) than the checked-in test/data/1k and test/data/64kminus1. Unlike those, it's not
# committed -- it's undiffable binary noise with no reason to sit in git history -- and is instead
# generated on demand by this target. Not a dependency of any other target, so plain builds/tests
# never pay for it; run it explicitly (`cmake --build build --target test_data_1m`) before a
# benchmark sweep that needs it.
#
# =================================================================================================

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/data/1m
    COMMAND head -c 1048576 /dev/urandom > ${CMAKE_CURRENT_SOURCE_DIR}/data/1m
    COMMENT "Generating test/data/1m (1 MiB random payload)"
    VERBATIM
)
add_custom_target(test_data_1m DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/1m)
