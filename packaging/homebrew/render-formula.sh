#!/bin/sh
# Render the formula to stdout after producing the immutable source archive.
set -eu
test "$#" -eq 1
goblin_archive=$1
test -f "$goblin_archive"
goblin_scriptdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
goblin_hash=$(shasum -a 256 "$goblin_archive" | awk '{print $1}')
test "${#goblin_hash}" -eq 64
sed "s/@SOURCE_SHA256@/$goblin_hash/" "$goblin_scriptdir/goblin-mosh.rb.in"
