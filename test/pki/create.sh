#!/bin/bash -e
# Generates a minimal root -> intermediate -> {server,client} certificate chain for TLS tests,
# using cfssl (all available in the devcontainer image). Adapted from anyhttp's pki/create.sh.
#
# Always regenerates the whole chain from scratch (cfssl is near-instant, so there's no real cost
# to that) rather than skipping files that already exist: CMake/Ninja may delete only a subset of
# this script's outputs before re-invoking it (whichever files test/CMakeLists.txt's
# add_custom_command() declares as OUTPUT), so a per-file existence check can leave a stale,
# no-longer-matching mix -- e.g. a fresh root.pem paired with an intermediate.pem still signed by
# the old one. A full rebuild every run avoids that class of bug entirely.
cd "$(dirname "${BASH_SOURCE[0]}")"
rm -rf out
mkdir -p out
cd out

cfssl gencert -initca ../root.json | cfssljson -bare root

cfssl gencert -initca ../intermediate.json | cfssljson -bare intermediate
cfssl sign -ca root.pem -ca-key root-key.pem -config ../config.json \
   -profile intermediate_ca intermediate.csr | cfssljson -bare intermediate

cfssl gencert -initca ../server.json | cfssljson -bare server
cfssl sign -ca intermediate.pem -ca-key intermediate-key.pem -config ../config.json \
   -profile server server.csr | cfssljson -bare server
# Full chain (leaf + intermediate) so a client trusting only the root can verify it.
cat server.pem intermediate.pem > server-fullchain.pem

cfssl gencert -initca ../client.json | cfssljson -bare client
cfssl sign -ca intermediate.pem -ca-key intermediate-key.pem -config ../config.json \
   -profile client client.csr | cfssljson -bare client
cat client.pem intermediate.pem > client-fullchain.pem
