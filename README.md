# nghttp2-corosio

This project is an attempt to re-implement [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio)
on top of the [capy](https://github.com/cppalliance/capy) / [corosio](https://github.com/cppalliance/corosio)
async framework.
But instead of going for simple callbacks and custom interfaces as in nghttp2-asio, nghttp2-corosio
is a coroutine-native implementation wrapping [nghttp2](https://github.com/nghttp2/nghttp2).

## Project Setup

This project is meant to be used in a C++ devcontainer ([pgit/cpp-devcontainer](https://github.com/pgit/cpp-devcontainer)).

## References

* [capy](https://github.com/cppalliance/capy)
* [corosio](https://github.com/cppalliance/corosio)
* [nghttp2](https://github.com/nghttp2/nghttp2)
* [nghttp2-asio](https://github.com/nghttp2/nghttp2-asio) (abandoned)
