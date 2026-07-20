# nghttp2-corosio

**This project is fully experimental** — an attempt to re-implement
[nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) on top of the
[capy](https://github.com/cppalliance/capy) / [corosio](https://github.com/cppalliance/corosio)
async framework. But instead of going for simple callbacks and custom interfaces as in nghttp2-asio,
nghttp2-corosio is a coroutine-native implementation wrapping
[nghttp2](https://github.com/nghttp2/nghttp2). Both capy and corosio are themselves pre-1.0 and
evolving, and the API surface here (`Server`/`Session`/`Client`) is expected to change significantly
as HTTP/2 support grows beyond the current echo handler.

## Project Setup

This project is meant to be used in a C++ devcontainer
([pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)).

It also doubles as an experiment in using [Claude Code](https://claude.com/claude-code) to drive most
of the day-to-day implementation work — expect commit history and code comments that reflect an
AI-assisted workflow.

## Build

```sh
cmake -S . -B build
cmake --build build
```

capy and corosio are pulled in automatically via CMake's `FetchContent`, so this first configure
clones and builds them as part of the project — no pre-installed system packages required.

This also builds and runs a small GoogleTest suite (`test/`); run it directly via
`./build/test/nghttp2-corosio-tests` or through `ctest --test-dir build`.

Configure with `-DENABLE_VALGRIND=ON` for a debug-info, unoptimized build tuned for running under
[valgrind](https://valgrind.org/) (mutually exclusive with `-DENABLE_ASAN`/`-DENABLE_TSAN`, which
also intercept `malloc` and don't mix with it):

```sh
cmake -S . -B build -DENABLE_VALGRIND=ON
cmake --build build
```

This also registers a `ValgrindMemcheck` CTest test (when `valgrind` is found) that runs the whole
test suite under memcheck:

```sh
ctest --test-dir build -R ValgrindMemcheck --output-on-failure
```

Every `Server` the tests construct is destroyed through a real, structured teardown (see
`Server::~Server()`'s docs) rather than being leaked, and no thread is spawned, so the whole run is
expected to be genuinely leak-free end to end -- this test fails on any `definitely lost`,
`indirectly lost`, or `possibly lost` block, not just real leaks; see the comment above the test in
`test/CMakeLists.txt` for details.

## Running

```sh
./build/server_main [port] [log-level]
```

`server_main` registers a single streaming echo handler at `/echo` (there's no routing beyond that
yet): it reads the request body and writes it straight back. `log-level` is one of
`debug`/`info`/`warn`/`error`/`off`. Since there's no TLS/ALPN negotiation to negotiate the protocol
yet, use curl's `--http2-prior-knowledge` flag to speak HTTP/2 directly over plain TCP (h2c) instead
of falling back to HTTP/1.1:

```sh
curl --http2-prior-knowledge --data-binary @somefile http://localhost:8080/echo
```

## Benchmarking

Throughput can be measured against `server_main` with
[h2load](https://nghttp2.org/documentation/h2load.1.html), nghttp2's own load-testing tool. For a
quick before/after comparison, use a **Release** build (`build`'s default Debug config makes
nghttp2-corosio itself 10-15x slower) and a payload from `test/data/` (`1k`, `64kminus1`):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/server_main 18080 warn &
h2load http://localhost:18080/echo -n 10000 -m 4 -c 3 -d test/data/64kminus1
```

For a full grid sweep over `-c`/`-m` values rendered as a heatmap/line chart, see
[`benchmark/`](benchmark/README.md).

## References

* [capy](https://github.com/cppalliance/capy)
* [corosio](https://github.com/cppalliance/corosio)
* [nghttp2](https://github.com/nghttp2/nghttp2)
* [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) (abandoned)
* [anyhttp](https://github.com/pgit/anyhttp) — the pattern source for this project's `Server`/`Session`
  API design
