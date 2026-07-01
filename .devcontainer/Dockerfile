FROM docker.io/psedoc/cpp-devcontainer:0.25

#
# Install capy (header + compiled library, installs cmake config files)
#
RUN git clone --depth 1 --branch develop https://github.com/cppalliance/capy.git capy && \
    cmake -S capy -B capy/build \
        -DBOOST_CAPY_BUILD_TESTS=OFF \
        -DBOOST_CAPY_BUILD_EXAMPLES=OFF \
        -DBOOST_CAPY_BUILD_BENCH=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build capy/build -j$(nproc) && \
    cmake --install capy/build && \
    rm -rf capy

#
# Install corosio (depends on capy)
#
# find_package(boost_capy) creates the namespaced Boost::capy target but not
# boost_capy. corosio_install() checks for boost_capy specifically, so we
# inject a bridge target via CMAKE_PROJECT_boost_corosio_INCLUDE.
#
RUN printf 'find_package(boost_capy REQUIRED)\nif(TARGET Boost::capy AND NOT TARGET boost_capy)\n  add_library(boost_capy INTERFACE IMPORTED GLOBAL)\n  set_target_properties(boost_capy PROPERTIES INTERFACE_LINK_LIBRARIES "Boost::capy")\nendif()\n' > /tmp/inject_capy.cmake && \
    git clone --depth 1 --branch develop https://github.com/cppalliance/corosio.git /tmp/corosio && \
    cmake -S /tmp/corosio -B /tmp/corosio/build \
        "-DCMAKE_PROJECT_boost_corosio_INCLUDE=/tmp/inject_capy.cmake" \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -DBOOST_COROSIO_BUILD_TESTS=OFF \
        -DBOOST_COROSIO_BUILD_EXAMPLES=OFF \
        -DBOOST_COROSIO_BUILD_PERF=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/corosio/build -j$(nproc) && \
    cmake --install /tmp/corosio/build && \
    rm -rf /tmp/corosio /tmp/inject_capy.cmake

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
