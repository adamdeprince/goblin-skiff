#!/bin/sh

set -eu
exec dpkg-buildpackage -us -uc -b "$@"
