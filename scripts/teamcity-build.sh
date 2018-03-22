#!/bin/bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"

fig_exec() {
    fig --no-file --log-level=warn --suppress-cleanup-of-retrieves --update-if-missing --suppress-retrieves cmake/3.9.1 -- "$@"
}

mkdir -p ${DIR}/out/test
cd ${DIR}/out/test
fig_exec cmake ../..
fig_exec cmake --build . -- all all-tests -j$(nproc)
fig_exec ctest . -T Test -V
