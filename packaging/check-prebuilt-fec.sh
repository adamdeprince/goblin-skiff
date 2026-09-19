#!/bin/sh
# Distributed under the GNU GPL, version 3 or later.
# Check the actual executable, not just configure flags or shared-library lists:
# libRaptorQ is header-only and could otherwise be compiled into an artifact.
set -eu
if test "$#" -ne 1; then
    printf 'Usage: sh check-prebuilt-fec.sh /path/to/goblin-skiffcp\n' >&2
    exit 2
fi
goblin_skiffcp=$1
test -x "$goblin_skiffcp"
umask 077
goblin_probe=$(mktemp -d "${TMPDIR:-/tmp}/goblin-fec-policy.XXXXXX")
trap 'rmdir "$goblin_probe"' EXIT
trap 'exit 1' HUP INT TERM

# Both paths deliberately do not exist inside our private, empty directory.
# A disabled codec fails before inspecting the source or opening any socket.
# An enabled codec fails on the missing source, which MUST fail this gate.
# Never discover or contact a real session, even when the artifact is wrong.
goblin_status=0
goblin_result=$("$goblin_skiffcp" send --fec=raptorq \
    --socket="$goblin_probe/no-session.sock" "$goblin_probe/no-source" 2>&1) || goblin_status=$?
if test "$goblin_status" -ne 1 || \
   test "$goblin_result" != 'goblin-skiffcp: raptorq is not available in this build'; then
    printf 'FAIL: prebuilt artifact must exclude RaptorQ (custom compilation only).\n' >&2
    printf 'Probe exit: %s; output: %s\n' "$goblin_status" "$goblin_result" >&2
    exit 1
fi
printf 'PASS: prebuilt FEC policy; RaptorQ unavailable in %s\n' "$goblin_skiffcp"
