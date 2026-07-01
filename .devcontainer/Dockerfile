FROM docker.io/psedoc/cpp-devcontainer:0.25

#
# capy and corosio are fetched and built by CMake (see FetchContent in
# CMakeLists.txt), so no image-level install step is needed for them.
#

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
