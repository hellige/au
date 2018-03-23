#!/bin/bash

# Clones submodules from git.drwholdings.com:mirror instead of github to bypass
# issues with cloning from https URLs on old build servers.

set -x
ROOT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

manualClone() {
    local DIR=$(echo "$1" | sed 's/path = \(.*\)/\1/')
    local URL=$(echo "$2" | sed 's/url = \(.*\)/\1/')
    #local REPO=$(basename --suffix=.git "$URL")
    local REPO=$(basename "$URL")
    REPO=${REPO%.git}
    local NEW_URL="git@git.drwholdings.com:mirror/${REPO}.git"

    echo "DIR = $DIR   URL = $URL   NEW_URL = $NEW_URL"
    mkdir -p $DIR
    pushd $DIR
    if [ -z "$(ls -A .)" ]; then
        git clone "$NEW_URL" .
    else
        git fetch origin
    fi
    if [ -f .gitmodules ]; then
        $(${SCRIPT})
    fi
    popd
}

parseGitModules() {
    cat .gitmodules | while read SUBMOD; read DIR; read URL; do
        manualClone "$DIR" "$URL"
    done
}

#parseGitModules
env HOME=${ROOT_DIR}/drw git submodule init
env HOME=${ROOT_DIR}/drw git submodule update --force
