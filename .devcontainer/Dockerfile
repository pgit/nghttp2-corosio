FROM docker.io/psedoc/cpp-devcontainer:0.25

#
# capy and corosio are fetched and built by CMake (see FetchContent in
# CMakeLists.txt), so no image-level install step is needed for them.
#

#
# GitHub Copilot CLI (copilot-cli) installation
#
RUN (type -p wget >/dev/null || (sudo apt update && sudo apt install wget -y)) \
	&& sudo mkdir -p -m 755 /etc/apt/keyrings \
	&& out=$(mktemp) && wget -nv -O$out https://cli.github.com/packages/githubcli-archive-keyring.gpg \
	&& cat $out | sudo tee /etc/apt/keyrings/githubcli-archive-keyring.gpg > /dev/null \
	&& sudo chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
	&& sudo mkdir -p -m 755 /etc/apt/sources.list.d \
	&& echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" | sudo tee /etc/apt/sources.list.d/github-cli.list > /dev/null \
	&& sudo apt update \
	&& sudo apt install gh -y

#
# Persistent bash history across container recreations
# (mount a volume to /commandhistory in devcontainer.json)
#
ARG USERNAME=vscode
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    && mkdir /commandhistory \
    && touch /commandhistory/.bash_history \
    && chown -R $USERNAME /commandhistory \
    && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

ARG USERNAME=vscode
COPY .bash_aliases /home/${USERNAME}/.bash_aliases
RUN chown $USERNAME:$USERNAME /home/${USERNAME}/.bash_aliases

# ==================================================================================================

#
# install some more libs and tools, remove *-dev packages that might interfere with custom builds
#
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive && \
    apt-get -y install libev-dev golang-cfssl valgrind && \
    apt-get -y remove libnghttp3-dev libnghttp2-dev && \
    apt-get -y autoremove && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

#
# spdlog
#
# https://github.com/gabime/spdlog
#
RUN cd opt && \
    git clone https://github.com/gabime/spdlog.git && \
    cd spdlog && mkdir build && cd build && \
    cmake .. && make -j$(nproc) && make install && \
    cd ../.. && rm -rf spdlog

# --------------------------------------------------------------------------------------------------

#
# TODO: consider rpath
#
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/local.conf && ldconfig
ENV LD_LIBRARY_PATH=/usr/local/lib

#
# boringssl / nghttp3 / ngtcp2 (using boringssl) / urlparse
#
# NOTE: don't install BoringSSL to /usr/local, we don't want it to interfere with OpenSSL
# NOTE: actually, we are using aws-lc instead of boringssl, but to ngtcp2, this is the same
#
# With debian trixie, we now have OpenSSL 3 with QUIC support. Whether or not that is any good,
# seems to be up for debate: https://www.haproxy.com/blog/state-of-ssl-stacks
#
ARG AWS_LC_VERSION=v5.1.0
RUN git clone --depth 1 -b ${AWS_LC_VERSION} https://github.com/aws/aws-lc && \
    cd aws-lc && \
    cmake -B build -DDISABLE_GO=ON --install-prefix=/opt/boringssl && \
    make -j$(nproc) -C build && \
    cmake --install build && \
    cd .. && \
    rm -rf aws-lc

ARG NGHTTP3_VERSION=v1.17.0
RUN git clone --recursive --depth 1 --branch ${NGHTTP3_VERSION} https://github.com/ngtcp2/nghttp3 && \
    cd nghttp3 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local --enable-lib-only && \
    make -j$(nproc) check && make install && \
    cd .. && rm -rf nghttp3

#
# nghtcp2 is built with both 'boringssl' (actually, aws-lc) and 'ossl' support
#
ARG NGTCP2_VERSION=v1.24.0
RUN git clone --recursive --depth 1 --branch ${NGTCP2_VERSION} https://github.com/ngtcp2/ngtcp2 && \
    cd ngtcp2 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local \
       BORINGSSL_CFLAGS="-I/opt/boringssl/include" \
       BORINGSSL_LIBS="-L/opt/boringssl/lib -lssl -lcrypto" \
       --with-boringssl --with-ossl && \
    make -j$(nproc) check && make install && \
    cd third-party/urlparse && \
    autoreconf -i && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && make install && \
    cd ../.. && \
    cd .. && rm -rf ngctp2
  
# ==================================================================================================

RUN git clone --depth 1 -b v1.7.0 https://github.com/libbpf/libbpf && \
    cd libbpf && \
    PREFIX=/usr/local LIBDIR=/usr/local/lib make -j$(nproc) -C src install && \
    cd .. && rm -rf libbpf

#
# nghttp2 with HTTP/3 support (based on OpenSSL)
#
ARG NGHTTP2_VERSION=v1.69.0
RUN git clone --recursive --depth 1 --branch ${NGHTTP2_VERSION} https://github.com/nghttp2/nghttp2.git && \
    cd nghttp2 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local --enable-http3 --with-libbpf && \
    make -j$(nproc) && make install && \
    cd .. && rm -rf nghttp2

#    ./configure --prefix=/usr/local --with-mruby --enable-http3 --with-libbpf \
#        LIBNGTCP2_CRYPTO_BORINGSSL_CFLAGS="-I/opt/boringssl/include" \
#        LIBNGTCP2_CRYPTO_BORINGSSL_LIBS="-L/opt/boringssl/lib -lssl -lcrypto" && \

# ==================================================================================================
