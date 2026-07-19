# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

nghttp2-corosio re-implements [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) on top of the
[capy](https://github.com/cppalliance/capy) / [corosio](https://github.com/cppalliance/corosio) async
framework. Unlike nghttp2-asio's callback-based design, this is a coroutine-native implementation
wrapping [nghttp2](https://github.com/nghttp2/nghttp2).

The `nghttp2-corosio` library (headers in `include/nghttp2-corosio`, sources in `src/`) is where the
real implementation lives; `main.cpp` is a standalone `echo_server` demo of raw corosio I/O and does
not depend on the library. Expect the library's structure to change significantly as HTTP/2 support
is added.

## Build

```sh
cmake -S . -B build
cmake --build build
```

This produces the `nghttp2-corosio` static library at `build/src/libnghttp2-corosio.a`, the
`echo_server` demo executable at `build/echo_server`, and a GoogleTest binary at
`build/test/nghttp2-corosio-tests` (run directly, or via `ctest --test-dir build`).
`CMAKE_EXPORT_COMPILE_COMMANDS` is on, so `build/compile_commands.json` is regenerated on every
configure (clangd is set up via `.devcontainer/devcontainer.json` to read it from the `build`
directory).

capy and corosio (both `develop` branch) are pulled in via `FetchContent` in `CMakeLists.txt`, so the
first `cmake -S . -B build` configure clones and builds them as part of this project — no
pre-installed system packages required. If capy/corosio APIs seem missing or different than expected,
check the cloned sources under `build/_deps/{capy,corosio}-src` rather than assuming upstream docs
are current — both libraries are pre-1.0 and evolving.

## Environment

This project is meant to be used in the devcontainer defined here (built from
`docker.io/psedoc/cpp-devcontainer`, see [pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)).

## Benchmarking

For a quick before/after throughput comparison (as opposed to the full `-c`/`-m` grid sweep in
`benchmark/`, see its own README), configure a **Release** build in a separate directory --
`build`'s default `Debug` config makes `nghttp2-corosio` itself 10-15x slower, which swamps any
signal from a source change:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Then run `server_main` and hit it with h2load using a payload from `test/data/` (`1k`, `64kminus1`):

```sh
./build-release/server_main 18080 warn &
h2load http://localhost:18080/echo -n 10000 -m 4 -c 3 -d test/data/64kminus1
```

**The system's `libnghttp2` matters too.** It's built from source into the devcontainer image
(`/usr/local/lib/libnghttp2.*`), not just `nghttp2-corosio`'s own Debug/Release setting -- if that
image was built without its own Release/optimization flags, every measurement is dominated by
nghttp2 itself (observed ~10x slower end-to-end, independent of anything in this repo) regardless
of how `nghttp2-corosio` is configured. If absolute throughput looks implausibly low, rebuild the
devcontainer image before trusting the numbers, rather than chasing a regression that isn't there.

**This environment is noisy.** Back-to-back runs of the *identical* binary have been observed to
vary ~40-90% (shared/virtualized host, no CPU pinning). A sequential "build before, benchmark;
build after, benchmark" comparison can show a fake regression or improvement driven entirely by
time-varying background load. Instead, keep both binaries around (e.g. `cp build-release/server_main
/tmp/server_main_<label>`) and interleave short runs across them on different ports, several rounds
each, then compare per-label averages -- a real effect should show up as a gap that doesn't
disappear when you shuffle the run order.

## Architecture

corosio provides coroutine-based async I/O primitives (`corosio::io_context`, `tcp_acceptor`,
`tcp_socket`, TLS streams, etc.) on top of capy, which supplies the coroutine task type
(`capy::task<>`), buffer/stream concepts, and the executor/scheduling machinery (`capy::run_async`).

The server pattern used throughout: one coroutine runs an accept loop on a `tcp_acceptor`, and for
each accepted `tcp_socket` it spawns a new detached session coroutine via
`capy::run_async(ioc.get_executor())(...)`. Session coroutines own their socket by value and read/write
using `co_await socket.read_some(...)` / `co_await capy::write(socket, ...)`, looping until an error
(including peer disconnect) breaks the loop.

### Library structure

`nghttp2-corosio`'s public API (`Server`, `Session`, both in namespace `nghttp2_corosio`) follows the
PIMPL shape of [anyhttp](https://github.com/pgit/anyhttp)'s `server.hpp`/`session.hpp` — a thin
value-type wrapper (movable, non-copyable) around a `std::shared_ptr<Impl>` — but is built on
capy/corosio rather than ASIO, with some deliberate deviations from anyhttp:

- `Server` owns its `corosio::io_context` outright (constructed from a `Config`, not handed an
  external executor like anyhttp's `Server`), and exposes `run()` to drive it synchronously. `Client`
  instead takes an external `any_executor`, matching anyhttp — it's meant to connect from within an
  application that already has its own execution context (e.g. the same one driving a `Server`).
- The `*_impl.hpp` headers (`Server::Impl`, `Session::Impl`, `Client::Impl`, `Stream`) live under
  `src/`, not `include/`, since no other translation unit needs them yet — anyhttp keeps its
  equivalents in `include/` because many `.cpp` files (nghttp2 session, beast session, etc.) reach
  into them. Move them to `include/` if/when the same need arises here.
- Unlike anyhttp, which templates its nghttp2 session on the stream type so server/client sessions can
  share `send_loop()`/`recv_loop()`, `Session::Impl` type-erases its stream to `any_stream`, so those
  loops are already shared as ordinary member functions; a `Session::Impl::Role` enum picks
  `nghttp2_session_server_new2()` vs `_client_new2()`, the one thing that actually differs.

`main.cpp`/`echo_server` is an unrelated raw-corosio demo that predates the library and does not use
it — don't assume the two are connected.

`Server`/`Client` sessions run a full HTTP/2 handshake and drive real request/response bodies through
`Session::Reader`/`Session::Writer` (type aliases for capy's `any_read_source`/`any_write_sink` — see
https://develop.capy.cpp.al/capy/6.streams/6.intro.html), bridged from nghttp2's synchronous callbacks
via `Stream` (`src/stream_impl.hpp`/`stream.cpp`) using `capy::async_event`. There's no pluggable
request-handler API yet: every server-side request is handled by a single hardcoded echo handler
(`Session::Impl::handle_request()`), and the client has one method, `Session::submit_request(path)`,
with fixed pseudo-headers (`POST`, `http`, a hardcoded `:authority`). Headers beyond routing by stream
ID/category aren't captured anywhere yet.

**`Session::Response::submit()` timing matters for throughput.** `submit(status, headers)` sends
the response HEADERS frame immediately -- it's an explicit, single call rather than a lazy
accumulator triggered on first write (see the class's doc comment in `session.hpp`). For a
streaming handler, call it once the first body chunk is actually in hand, not up front before
reading anything: submitting before nghttp2 has any data to send forces it to flush a standalone
HEADERS frame and defer the data provider (`NGHTTP2_ERR_DEFERRED`) until the first `write()`
resumes it -- an extra round trip through `Session::Impl::send_loop()`'s wait/wake cycle on every
single request. Measured ~40% lower h2load throughput (large request/response bodies, see
"Benchmarking" above) for submitting eagerly vs. deferring to just before the first write, where
nghttp2 can fold the HEADERS and first DATA frame into one `send_loop()` pass instead. See
`server_main.cpp`'s `echo()` for the deferred pattern. (anyhttp's own reference handlers submit
eagerly, up front -- don't copy that shape here without checking whether it still matters once
anyhttp's send-side implementation is understood, since this project's `send_loop()` batches
differently.)

**Structured shutdown**: `Server` doesn't leak on destruction. `~Server()` performs synchronous,
ungraceful cancellation — no draining of in-flight requests, since a destructor can't `co_await` —
of everything running on its `io_context`: the accept loop, every session it accepted, every
in-flight request handler, and (since `Session::Impl`/`Client::Impl::connect()` register into the
same per-context service) any `Client` session sharing that executor too. See
`detail::TaskGroup` (`src/task_group.hpp`) for the mechanism: a `capy::execution_context::service`
that tracks spawned coroutines via a shared `std::stop_source`, and `Server::Impl::~Impl()` for how
it's driven — `request_stop()` then `poll()`/`run_one()` in a loop until every tracked coroutine has
actually unwound, all *before* any member (in particular `io_context`) starts destructing. Destroying
an `io_context` while a coroutine frame is still suspended on it is what used to corrupt memory (a
double-free inside the frame's `std::stop_state`/`std::stop_callback` teardown); draining first means
nothing is ever destroyed mid-flight.

This replaced an earlier, less principled state: `recv_loop()`'s final `start_write()` (waking a
parked `send_loop()` to flush the closing GOAWAY after the peer disconnects) was occasionally
missed — apparently a corosio scheduler edge case around same-thread wakeups posted right before the
scheduler would otherwise go idle — leaving that coroutine (and its socket) stuck forever, and tests
sidestepped it by deliberately leaking every `Server` instead of tearing one down. The structured
shutdown above no longer depends on that particular wakeup: `request_stop()` happens from `~Impl()`,
outside of any active `io_context::run()`/`poll()` pass, so the specific "posted right as the
scheduler goes idle" race window it needed doesn't apply to it. Whether the underlying corosio
scheduler behavior is itself fixed is untested here; what's known is that `Server` teardown no longer
hangs or corrupts under it — verified with repeated (50x) shuffled test runs and full-suite valgrind
sweeps (`--leak-check=full`, zero leaks, zero errors) rather than just the one previously-`EchoLoop`-
scoped check.

### Coding Conventions

#### Formatting

Mandatory `clang-format`. Editor width: 100. Occasional horizontal rulers. Doxygen comments. Prefer `/** ... */` for longer explanations. Always use `//` inside code, even for multiple lines.
