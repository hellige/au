#!/bin/bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"

fig_exec() {
    fig --no-file --log-level=warn --suppress-cleanup-of-retrieves --update-if-missing --suppress-retrieves cmake/3.9.1 -- "$@"
}

mkdir -p ${DIR}/out/test
cd ${DIR}/out/test
fig_exec cmake -DCMAKE_CXX_COMPILER=${DIR}/drw/g++ -DCMAKE_C_COMPILER=${DIR}/drw/gcc -DSTATIC=ON ../..
fig_exec cmake --build . -- all -j$(nproc)
fig_exec ctest . -T Test -V
