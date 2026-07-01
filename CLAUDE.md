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

This produces the `nghttp2-corosio` static library at `build/src/libnghttp2-corosio.a` and the
`echo_server` demo executable at `build/echo_server`. `CMAKE_EXPORT_COMPILE_COMMANDS` is on, so
`build/compile_commands.json` is regenerated on every configure (clangd is set up via
`.devcontainer/devcontainer.json` to read it from the `build` directory).

capy and corosio (both `develop` branch) are pulled in via `FetchContent` in `CMakeLists.txt`, so the
first `cmake -S . -B build` configure clones and builds them as part of this project — no
pre-installed system packages required. If capy/corosio APIs seem missing or different than expected,
check the cloned sources under `build/_deps/{capy,corosio}-src` rather than assuming upstream docs
are current — both libraries are pre-1.0 and evolving.

## Environment

This project is meant to be used in the devcontainer defined here (built from
`docker.io/psedoc/cpp-devcontainer`, see [pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)).

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
  external executor like anyhttp's `Server`), and exposes `run()` to drive it synchronously.
- The `*_impl.hpp` headers (`Server::Impl`, `Session::Impl`) live under `src/`, not `include/`, since
  no other translation unit needs them yet — anyhttp keeps its equivalents in `include/` because many
  `.cpp` files (nghttp2 session, beast session, etc.) reach into them. Move them to `include/` if/when
  the same need arises here.

`Server::Impl` currently just runs an accept loop that logs each connection's source endpoint; it does
not yet construct a `Session` per connection (`Session::Impl` is an empty stub). `main.cpp`/`echo_server`
is an unrelated raw-corosio demo that predates the library and does not use it — don't assume the two
are connected.
