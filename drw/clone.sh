#!/bin/bash

# Clones submodules from git.drwholdings.com:mirror instead of github to bypass
# issues with cloning from https URLs on old build servers.

set -x
ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

cd ${ROOT_DIR}

echo "If the update fails, make sure there's an up-to-date mirror at http://git.drwholdings.com/mirror"
env HOME=${ROOT_DIR}/drw git submodule init
env HOME=${ROOT_DIR}/drw git submodule update --force
