<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff tests

## UDP jump relays

`udp-relay` checks outer encryption, independent keys, direction separation,
tampering/truncation, a 1024-packet replay window, one-to-four-hop nesting and
both endpoints' MTU budgets. It launches test-owned relay servers on IPv4 and
IPv6 loopback to check fixed-destination filtering, port roaming without
replay rollback, out-of-order delivery, authenticated close, and startup/idle
expiry. FIPS envelope roundtrips run only when a configured provider is present.

`udp-jump-wrapper.test` uses a fake SSH transport, never a real remote host,
to check `-J`, configured and nested `ProxyJump`, `--ssh -J`, SSH ports and
`-F`, first-hop NAT address selection, reverse-order relay setup, key order,
FIPS selection, and fail-closed preflight/MTU/suite negotiation.

`udp-relay-integration.test` runs real client/server PTYs with one and four
actual Goblin relay processes, plus an outer loss/reordering simulator. It
checks multi-packet screen updates, live typing, and recursive delta file
transfers in both directions while the menu is hidden/reopened. Normal client
exit must close every hop. All files and sockets belong to private fixtures;
these tests do not contact jump hosts or change installed binaries.

## Build-host clock isolation

Tests must see native filesystem metadata and time. If a packaging host uses
`libfaketime` for a process-only clock correction, set `NO_FAKE_STAT=1` for build
tools and remove the helper before running tests. For that build environment:

```sh
make -C src/tests check \
  TESTS_ENVIRONMENT='env -u LD_PRELOAD -u FAKETIME -u FAKETIME_SHARED -u FAKETIME_DONT_FAKE_MONOTONIC -u NO_FAKE_STAT'
```

Apply the same cleanup to installed-binary integration tests, and remove any
other `FAKETIME_*` variables configured by the build environment. Verify both
the test process's environment and its loaded libraries when checking isolation.
Interposed file timestamps can falsely trip the file-transfer test's
`Destination changed before signature` identity check. Separately, real
filesystems can give rapid directory changes identical timestamps; do not hide
that cache-invalidation bug by adding sleeps or skipping directory tests.

## Inline downloads

`download` covers the namespaced OSC 777 protocol, split/canceled escape
sequences, strict base64 and UTF-8 filenames, size/queue bounds, binary zstd-22
encoding, FEC loss/reordering/duplicate recovery, atomic no-overwrite saves,
symlink collisions, cancellation, partial-file cleanup and a stopped disk
worker. It also checks approval/rejection/expiry, out-of-order approvals, and
two nested Goblin FEC hops with final-recipient consent and no intermediate
filesystem writes. It never writes to the user's actual Downloads directory.

`download-forward` tests Goblin-first and Kitty-second discovery, optional
end-to-end approval, old Goblin parents, Kitty framing and collision handling,
reply-ID isolation, pasted/late replies, checksum failures and timeouts.

`download-integration.test` runs client and server through a lossy encrypted
UDP relay with real PTYs and a private download destination. It checks capability
negotiation and opt-out without graphics/clipboard support, the automatic
confirmation popup, Skiff-prefix/0 hide/reopen, two-stage approval and default rejection.
It continues typing and updating the screen while the Downloads worker is stopped, then verifies
the exact saved file and remote `saved` status after resuming it. Simulated
Goblin and Kitty parent terminals test actual client/server forwarding and
refusal over the same lossy relay; neither may create an intermediate file,
worker or local approval popup. These are protocol simulators, not GUI tests.
Requires Python 3, PTYs, loopback UDP and permission to inspect its child processes.

With Kitty installed, this optional headless check uses its actual transfer
parser, invalid-probe rejection, file writer and response encoding. It injects
only the accept/decline decision and maps destinations into a private fixture;
it does not open a GUI or touch the user's Downloads. Run from `src/tests`:

```sh
kitty +runpy 'import runpy, sys; sys.argv = ["download-integration.py", "--kitty-engine"]; runpy.run_path("download-integration.py", run_name="__main__")'
```

## control-panel and mascot

`control-panel` covers bounded framing, ACK pacing, SST loss recovery,
stale navigation replies, persistent hide/reopen, input filtering, escaped
filenames, and a stopped filesystem worker with nonblocking timeout/recovery.
Download permission tests cover automatic popup wakeup, paste/split-key
deferral, deliberate two-stage confirmation, default rejection, and cancellation.
Rapid create/rename/delete tests exercise native directory notifications and
polling-only fallback without sleeps to advance filesystem timestamps. A
missing file must invalidate the sorted-name cache rather than remain as an
UNKNOWN entry; unchanged membership must not produce wire traffic. Native
notifications are armed before enumeration, and periodic name-only checks
cover remote filesystem changes that do not produce local notifications. All
enumeration and notification handling stays in the killable filesystem worker.
It creates and removes 1,024 files in its own temporary directory. Run
`src/tests/control-panel --stress` from the build root to repeat with 100,000
files and print first-page latency and framed bytes (local filesystem, not a
NAS throughput benchmark).

`control-panel-integration.test` runs real client/server PTYs through a local
UDP relay that drops every seventh packet and reorders some others. It stops
only its own remote directory helper, checks that the underlying screen and
keyboard remain live, and resumes the same pending page after Skiff-prefix/0 reopen.
It checks legacy and Kitty command keys (including press-only mode, repeats,
releases, alternate codes and lock modifiers), custom/disabled escape prefixes,
paste protection, literal/unknown commands and quitting with the popup open.
Alt-0 is verified to pass through to the remote application. Set
`GOBLIN_SKIFF_TEST_CLIENT` and `GOBLIN_SKIFF_TEST_SERVER` to test installed binaries.
It needs Python 3, loopback UDP, PTYs, and permission to inspect its child
processes with `ps`.

`mascot` checks the bundled lossy WebP size/dimensions, rendering envelopes,
capability replies split at every boundary, fallback/opt-out, and preservation
of ordinary input. The image and terminal capability probes are client-local.

`startup-screen.test` uses real local client/server PTYs to test all combinations
of graphics-disable flags, probe filtering, forced-format/preview overrides,
remote Kitty suppression, and clean exit without screen erasure. An isolated
tmux capture also verifies that pre-existing output and the ASCII chicken
survive the first remote frame in native scrollback and that the chicken stays
visible above a short remote session (this subtest is skipped when tmux is
unavailable). When Kitty is installed, its headless terminal parser also checks
fresh, partially used, and bottom-of-window layouts: remote text starts directly
after the banner and a fresh window does not scroll unnecessarily. These checks
do not open or change any GUI windows. Run only this subtest with
`python3 startup-screen.py --kitty-layout` from the tests directory.
The test needs Python 3, loopback UDP, and PTYs. `terminal-display` also checks
every starting row in several window sizes, incremental startup scrolling,
and full-viewport takeover for full-screen output, mouse reporting, scrolling
regions, repaint and resize.
The client-local source/license notice must appear exactly once and remain
in the startup history ahead of the mascot; it must not enter remote input.
The generic emulation comparisons explicitly use `--no-mascot --alternate-screen`
so their physical row coordinates match the direct-terminal reference.
To test an installed build, set `GOBLIN_SKIFF_TEST_CLIENT`,
`GOBLIN_SKIFF_TEST_SERVER`, and `GOBLIN_SKIFF_TEST_WRAPPER` to its executable paths
and run `python3 startup-screen.py` from the tests directory.

## sixel-state

`sixel-state` tests the sixel codec and synchronized image-state
representation. It covers native/WebP roundtrips,
RGB/HLS palettes, transparency, aspect ratios, malformed input and resource
quotas, cumulative state recovery after a skipped update, and reset. Renderer
selection prefers native sixel, including on dual-capable clients; Kitty is
used only when sixel is unavailable. Existing Kitty image messages retain
their wire format. `graphics-integration.test` separately exercises live PTYs
and encrypted, lossy UDP with simulated terminal capability replies. It checks
native sixel, Kitty-only conversion, no-graphics fallback, 5,120-pixel images,
unchanged-image retention, OSC 66 sizing/fallback, Alt events and clean exit.
See `SIXEL_STATE.md` for integration details and limits.

## Kitty pixel layout

`kitty-graphics` checks pixel-sized PNG placements, crop rectangles, cell
offsets, one-axis aspect-ratio sizing, cursor hold, virtual/relative placements,
bounded cursor arithmetic, and changed attachment geometry. A synthetic
`kittycairo` stream checks unchanged 640-by-480 dimensions through PNG/WebP/
RGBA conversion and cursor positioning through state synchronization.
`kittycairo.test` additionally pipes a real gnuplot plot through the same check;
it skips when gnuplot or its `kittycairo` terminal is unavailable.

## ocb-aes

This is a unit test for the OCB-AES encryption used in Skiff, including
Rogaway's OCB implementation and some of Skiff's surrounding C++
support code.

## encrypt-decrypt

This is a simple functional test of Skiff's implementation of encrypted
messages.

## base64

This tests the inherited upstream Mosh homegrown base64 functionality.  The associated
`genbase64.pl` script is used to independently generate validated test
vectors.

## e2e-test

This is a test framework for end-to-end testing of Skiff.  It uses tmux
to invoke Skiff in a nicely stable interactive pty, and also uses
tmux's `capture-pane` command to get a dump of the terminal screen
that goblin-skiff-client has drawn, neatly getting around Skiff's somewhat
non-deterministic display redraw.

There are four essential parts to the framework:

* your test script
* `e2e-test`
* `e2e-test-server`
* `e2e-test-subrs`

The test script has two roles: when invoked without arguments, it is a
wrapper script for the overall test, and when invoked with an
argument, it performs a testing-related action.  In wrapper mode, it
invokes e2e-test with action arguments, which are used to invoke the
test script for actions at appropriate points by e2e-test.  These
provide a suite of behaviors that you can use to test various Skiff
behaviors.

`e2e-test` is the heart of the framework.  It runs actions as
requested, logs their output, compares and/or validates their results,
and generates the final result (exitstatus, mostly) for the Automake
testing framework used by the Skiff build.  For test execution, it runs
an action in an interactive session, in a tmux `screen`, to exercise
some behavior.  The action can optionally be run in a Skiff session, or
directly in tmux (doing both and comparing the result is a useful way
to test complex terminal emulation behaviors).  The action generally
writes some output to the terminal that can later be verified by
another action.  Optionally, a client action can generate tty input or
otherwise exercise Skiff in some fashion (this capability is untested,
but it's a useful place to use `expect` or other interactive
simulations).  The action is run by `e2e-test-server`, which is a
relatively small wrapper script to capture errors, and capture the
tmux screen.

There are several different categories of actions:

### Execution

`baseline` is an action that almost all tests will use.  This invokes
the test script inside Skiff, where it can generate some output, and
then captures the client-side tmux display with `tmux capture-pane`.

`direct` is the same as the above, except that Skiff is not used--
`e2e-wrapper-script` and the test script are invoked directly inside
tmux.

`variant` can be used to provide a slightly different action from
`baseline`.

### Verification

`verify` compares captures from the `baseline` and `direct` test
actions, which are expected to be identical.

`same` compares captures from the `baseline` and `variant` test
actions, which are expected to be identical.

`different` compares captures from the `baseline` and `variant` test
actions, which are expected to be different.

`post` is a catchall script hook which allows custom verification
acions to be coded.

### Client wrappers

`tmux` injects a wrapper command into the test command before tmux.
If this is not run, a default command called `hold-stdin` is run
instead.  These commands are expected to hold tmux's stdin open,
possibly injecting tmux commands, while the test runs.  See
`window-resize.test` for an example of this that manipulates tmux
state.  Alternately, this could use expect or something similar.

`client` simply injects a wrapper command into the (long) test command
between tmux and Skiff.  It's expected to interact with its wrapped
command line as `expect` might do.  This is not actually tested yet.

### Flags

Upstream’s `mosh-args` action, `client-args` and `server-args` inject extra arguments
into the invocations of the respective commands.

## Logging and error reporting

Each execution action is run, and recorded in
`<testname>.test.d/<action>.*`. `<action>.exitstatus` is the
exitstatus from the server wrapper.  `<action>.tmux.log` is the output
of tmux for the entire test run for that action; `<action>.capture` is
a capture of the Skiff client screen after the test action is complete,
generated with `tmux capture-pane`.

In accordance with GNU Automake's test framework, the test should
return these exit status values:

* 0 test success
* 1 test failure
* 77 test skipped (tmux or ssh is unavailable if needed)
* 99 hard error

These values are also used internally between the various scripts;
errors are conveyed out to the build test framework.


## Sample tests

A few tests have been implemented so far to test the framework itself,
and to provide examples for further development.

`e2e-success` is a simple test that executes `baseline` and `direct`
with the same stimulus (simply clearing the screen), and expects to
see identical results.

`e2e-failure` is similar to `e2e-success`, but expects to see
different results from `baseline` and `variant`.  Since it uses the
same stimulus for the two execution action, it fails.  A more
realistic test might be to have `variant` execute some escape sequence
that is absent from `baseline`; this would verify that the escape
sequence actually does something.

`emulation-back-tab` tests an escape sequence that Skiff does not
support.  It expects the test to produce the output that would be
generated if the escape sequence were implemented.  If it gets output
as expected when the escape sequence is *not* implemented, the test
fails.  But if the output does not match one of these two cases, the
test returns an error.  This is an example of error handling within
the test framework.

`unicode-later-combining` demonstrates Skiff's handling of a Unicode
edge case, a combining character drawn without a printing character in
the same cell.  It verifies the output in the `post` action; since
there are a couple of different Unicode renderings that are reasonable
in this case, a regex that covers both is used.  It also implements an
unused `variant` action that draws blank-space+combiner in a correct
fashion.

## Notes

The shell command `printf` is generally used in place of
`echo` in this framework, because of its more precisely-specified and
portable behavior.  But beware, even `printf` varies between systems--
GNU printf, for example, implements `\e`, which is a non-POSIX
extension unavailable in BSD implementations

It's fairly simple to test each of these scripts independently, but
the entire chain is a bit prone to behaving oddly in hard-to-debug
ways.  `set -x` is your friend here.

The test scripts are a bit fragile about timeouts.  They will
generally run correctly on an unloaded machine without the `make -j`
flag.  Using `make -j` is obviously very convenient for development,
and it works fine on faster machines, but I don't recommend it for
automated testing.
The `link-budget` test simulates independent 88/22 and 2.4/2.4 kbit/s queues
with bounded buffering and satellite-like propagation delay. It also checks
wire-byte pacing, duplicate feedback, idle reset and no idle probing. These
are synthetic controller checks, not measurements on a satellite service.
`python3 control-panel-integration.py --adaptive` exercises real encrypted
UDP and PTYs with pacing enabled over the lossy relay. The regular panel
tests cover live metadata deltas, silent unchanged watches, generation
invalidation, parent-directory sorting, Tab/Shift-Tab and Left/Right bindings,
viewport-edge scrolling (including resize and cache eviction), direct Enter
transfers without leaving the browser, and a stopped-filesystem helper.

`control-panel --stress` creates 100,000 private fixture entries, verifies
alphabetical ordering across bounded windows and tests a stalled worker.
The first-window timing includes the host-side name scan and sort.

`file-transfer` exercises real forked workers and the file-menu FEC channel:
librsync signatures and deltas, recursive upload/download, empty files and
directories, queued work, network interruptions, duplicate/reordered/dropped
packets in both directions, bounded backpressure, cancellation and symlink
safety. It skips when configured without librsync. Fixture paths are private
temporary directories; it does not copy into the user's Downloads folder.
`file-transfer-integration.test` drives the actual two-pane menu using PTYs
and encrypted loopback UDP with packet loss/reordering. It checks Enter, F5,
Shift-Tab and Kitty keys, direct transfers and paste safety, recursive delta
copies in both directions, adaptive pacing, a hidden/reopened transfer queue
and live keyboard input. Remote-download consent is tested separately.

`python3 control-panel-integration.py --file-speed` measures a 512 KiB
incompressible upload and download through real encrypted loopback UDP,
including cold-start transfer setup. This is not a 10 GbE hardware benchmark.
Use `--file-speed-confirm` with `GOBLIN_SKIFF_TEST_CLIENT`/`GOBLIN_SKIFF_TEST_SERVER` to
compare an older installed version that still has the copy confirmation.
`link-budget` also simulates clean fast links, an abrupt downgrade to
2.4 kbit/s, and continuous foreground traffic with random loss/reordering
on an unconstrained path. Its simulated throughput is not an end-to-end
file speed claim.

## Connection version compatibility

`session-version` exercises release/build identity over encrypted loopback
traffic, compatible mixed releases, legacy peers, lost startup metadata,
removal of version overhead after acknowledgment, and incompatible protocol
rejection before state application. `udp-jump-wrapper.test` checks setup
reporting, the selected client's version, malformed and duplicate metadata,
and rejection before starting jump relays. See `COMPATIBILITY.md` for the
baseline and the release compatibility policy.
