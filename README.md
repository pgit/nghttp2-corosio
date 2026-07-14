# nghttp2-corosio

This project is an attempt to re-implement [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio)
on top of the [capy](https://github.com/cppalliance/capy) / [corosio](https://github.com/cppalliance/corosio)
async framework.
But instead of going for simple callbacks and custom interfaces as in nghttp2-asio, nghttp2-corosio
is a coroutine-native implementation wrapping [nghttp2](https://github.com/nghttp2/nghttp2).

## Project Setup

This project is meant to be used in a C++ devcontainer ([pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)).

## Build

```sh
cmake -S . -B build
cmake --build build
```

This also builds and runs a small GoogleTest suite (`test/`); run it directly via
`./build/test/nghttp2-corosio-tests` or through `ctest --test-dir build`.

Configure with `-DENABLE_VALGRIND=ON` for a debug-info, unoptimized build tuned for running under
[valgrind](https://valgrind.org/) (mutually exclusive with `-DENABLE_ASAN`/`-DENABLE_TSAN`, which
also intercept `malloc` and don't mix with it):

```sh
cmake -S . -B build -DENABLE_VALGRIND=ON
cmake --build build
```

This also registers an `EchoLoopValgrind` CTest test (when `valgrind` is found) that runs a real
HTTP/2 client/server echo round trip (`ClientAsync.PostData_ReceivesEcho`) under memcheck:

```sh
ctest --test-dir build -R EchoLoopValgrind --output-on-failure
```

It only fails on `definitely lost`/`indirectly lost` blocks -- real leaks. `possibly lost` and
`still reachable` blocks show up on every run (deliberately: this fixture leaks its `Server`, and
detached pthreads/coroutine-frame pooling are common sources of valgrind's conservative "possibly
lost" false positives) but aren't treated as failures; see the comment above the test in
`test/CMakeLists.txt` for details.

## Running

```sh
./build/server_main [port]
```

Request handling isn't implemented yet, so the server won't send back a response, but it does
complete the HTTP/2 connection preface and SETTINGS handshake and will log every frame it sends and
receives. Since there's no TLS/ALPN negotiation to negotiate the protocol yet, use curl's
`--http2-prior-knowledge` flag to speak HTTP/2 directly over plain TCP (h2c) instead of falling back
to HTTP/1.1:

```sh
curl --http2-prior-knowledge http://localhost:8080/
```

## References

* [capy](https://github.com/cppalliance/capy)
* [corosio](https://github.com/cppalliance/corosio)
* [nghttp2](https://github.com/nghttp2/nghttp2)
* [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) (abandoned)
