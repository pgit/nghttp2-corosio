# nghttp2-corosio

This project is an attempt to re-implement nghttp2-asio (https://github.com/nghttp2/nghttp2-asio) to the capy/corosio async framework (https://github.com/cppalliance/capy) and (github.com/cppalliance/corosio).
But instead of going for simple callbacks and custom interfaces as in nghttp2-asio, nghttp2-corosio is a coroutine-native implementation wrapping nhttps://github.com/nghttp2/nghttp2.

## Project Setup

This project is mean to  be used in a C++ devcontainer (https://github.com/pgit/cpp-devcontainer).

## References

* capy
* corosio
* nghttp2
* nghttp2-asio (abandoned)

