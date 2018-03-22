#!/bin/bash

# Clones submodules from git.drwholdings.com:mirror instead of github to bypass
# issues with cloning from https URLs on old build servers.

# TODO: Handle recursive cloning of submodules

clone() {
    local DIR=$(echo "$1" | sed 's/path = \(.*\)/\1/')
    local URL=$(echo "$2" | sed 's/url = \(.*\)/\1/')
    local REPO=$(basename --suffix=.git "$URL")
    local NEW_URL="git@git.drwholdings.com:mirror/${REPO}.git"

    echo "DIR = $DIR   URL = $URL   NEW_URL = $NEW_URL"
    mkdir -p $DIR
    cd $DIR
    git clone "$NEW_URL" .
    cd -
}

while read SUBMOD; read DIR; read URL; do
    clone "$DIR" "$URL"
done
