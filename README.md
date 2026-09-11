[![ci](https://github.com/mobile-shell/mosh/actions/workflows/ci.yml/badge.svg)](https://github.com/mobile-shell/mosh/actions/workflows/ci.yml)

Mosh: the mobile shell
======================

Mosh is a remote terminal application that supports intermittent
connectivity, allows roaming, and provides speculative local echo
and line editing of user keystrokes.

It aims to support the typical interactive uses of SSH, plus:

   * Mosh keeps the session alive if the client goes to sleep and
     wakes up later, or temporarily loses its Internet connection.

   * Mosh allows the client and server to "roam" and change IP
     addresses, while keeping the connection alive. Unlike SSH, Mosh
     can be used while switching between Wi-Fi networks or from Wi-Fi
     to cellular data to wired Ethernet.

   * The Mosh client runs a predictive model of the server's behavior
     in the background and tries to guess intelligently how each
     keystroke will affect the screen state. When it is confident in
     its predictions, it will show them to the user while waiting for
     confirmation from the server. Most typing and uses of the left-
     and right-arrow keys can be echoed immediately.

     As a result, Mosh is usable on high-latency links, e.g. on a
     cellular data connection or spotty Wi-Fi. In distinction from
     previous attempts at local echo modes in other protocols, Mosh
     works properly with full-screen applications such as emacs, vi,
     alpine, and irssi, and automatically recovers from occasional
     prediction errors within an RTT. On high-latency links, Mosh
     underlines its predictions while they are outstanding and removes
     the underline when they are confirmed by the server.

Mosh does not support sshfs.  This version also includes experimental stream
forwarding for selected SSH-like uses: local TCP forwards (`-L`), remote TCP
forwards (`-R`), local SOCKS5 dynamic forwards (`-D`), SSH agent forwarding
(`-A`), and basic X11 forwarding (`-X`).
`-J` / `--jump` also relays the encrypted Mosh UDP session through up to four
Goblin Mosh jump hosts, including routes discovered from SSH `ProxyJump`.
It does not add arbitrary UDP port forwarding; `-L/-R/-D` remain TCP streams.

Other features
--------------

   * Mosh adjusts its frame rate so as not to fill up network queues
     on slow links, so "Control-C" always works within an RTT to halt
     a runaway process.

   * Mosh warns the user when it has not heard from the server
     in a while.

   * Mosh supports lossy links that lose a significant fraction
     of their packets.

   * Mosh handles some Unicode edge cases better than SSH and existing
     terminal emulators by themselves, but requires a UTF-8
     environment to run.

   * Mosh leverages SSH to set up the connection and authenticate
     users. Mosh does not contain any privileged (root) code.

Getting Mosh
------------

  [The Mosh web site](https://mosh.org/#getting) has information about
  packages for many operating systems, as well as instructions for building
  from source.

  Note that `goblin-mosh-client` receives an AES session key as an environment
  variable.  If you are porting Mosh to a new operating system, please make
  sure that a running process's environment variables are not readable by other
  users.  We have confirmed that this is the case on GNU/Linux, OS X, and
  FreeBSD.

Usage
-----

  The `goblin-mosh-client` binary must exist on the user's machine, and the
  `goblin-mosh-server` binary on the remote host.

  The user runs:

    $ goblin-mosh [user@]host

  To reach a destination through a jump host, for both setup and Mosh UDP:

    $ goblin-mosh -J [user@]jump destination
    $ goblin-mosh -J jump-a,jump-b destination

  Install the updated `goblin-mosh-server` on every jump and the destination.
  Each hop needs UDP reachability to the next, with UDP 60001–60999 permitted
  by default. SSH is used only for setup; terminal traffic remains encrypted
  end to end, with separately authenticated relay envelopes. `--jump-port`
  selects the jumps' UDP range; `-p` still selects the destination's port.
  See [UDP_RELAY.md](UDP_RELAY.md) for SSH configuration, NAT, relay expiry,
  FIPS behavior, and the 24-byte (36-byte FIPS) per-hop wire overhead.

  The client displays the bundled mascot while connecting, using Kitty,
  sixel, or ASCII according to local terminal capability replies. Its only
  stored asset is a 15,616-byte, 512-by-512 lossy WebP. Kitty uses about
  16 columns by 8 rows, sixel about 20 by 10, and ASCII 32 by 16, reduced
  for small windows. The image and capability probes stay on the local tty;
  they are not sent over the Mosh connection. Use `--no-mascot` to disable
  it, or `--mascot=kitty|sixel|ascii` to override detection. Preview without
  connecting with `goblin-mosh-client --show-mascot=ascii` (or kitty/sixel).
  The mascot is printed once, inline. On the default primary screen, the
  remote prompt starts below earlier output, reserving only the rows it needs.
  The mascot moves into native history as output grows, not all at once when
  the connection opens. Full-screen applications, mouse reporting, graphics,
  and explicit repaints release the remaining space. Graphics retention in
  history depends on the local terminal; `--alternate-screen` still uses an
  isolated screen.

  Use `--no-kitty` or `--no-sixel` to treat that local graphics protocol as
  unsupported. These suppress its mascot probes/output; `--no-kitty` also
  disables remote Kitty image rendering locally. Combine both for ASCII:

    $ goblin-mosh --no-kitty --no-sixel naamah

  The disable options also work on `goblin-mosh-client`, take precedence over
  forced mascot/preview formats (falling back to ASCII), and do not change the
  remote `TERM` value or send extra capability packets over the link.

  Remote sixel graphics are negotiated with `sixel-state-v1` and retained
  in terminal image state as lossless WebP. A sixel-capable local terminal
  receives sixel, even if it also supports Kitty. A Kitty-only local terminal
  receives client-side Kitty conversion; neither protocol is emitted to a
  client supporting neither. Capability discovery works independently of the
  mascot and honors `--no-kitty` / `--no-sixel`. See
  [SIXEL_STATE.md](SIXEL_STATE.md) for the integration, limits and tests.

  Applications can use the [Goblin download protocol](GOBLIN_DOWNLOAD_PROTOCOL.md)
  to deliver files toward the user's terminal. The client forwards to a Goblin
  parent if supported, otherwise uses Kitty file transfer, otherwise opens a
  confirmation to save to local Downloads (`y` selects Save, then Enter confirms;
  the Mosh prefix followed by `0` hides or reopens it). Intermediate clients do
  not save copies or ask again after a parent refuses. Terminal base64 is decoded
  before binary zstd-22/FEC transfer on the background path. Local fallback
  writes never overwrite existing files. Use `--no-downloads` to disable all
  routes or `--download-directory=DIR` to set the local fallback directory.

  The Mosh command prefix followed by `0` opens a local, two-pane directory
  browser over the live remote screen. By default, type Ctrl-^ (Ctrl-6 in
  Kitty, often Ctrl-Shift-6 elsewhere), release it, then press `0`. Repeat that
  sequence or press Escape to hide it; reopening resumes the same directories,
  selections, scroll positions, and pending requests. Tab or Shift-Tab changes
  pane; Left or Backspace opens the parent directory; Right enters a directory
  and does nothing on files. Up/Down moves selection, Page Down/Up (or n/p)
  scrolls a viewport, and r refreshes. The list scrolls only when selection
  reaches its top or bottom edge. There are no visible transport pages.
  The high-contrast panes have rounded Unicode borders and file sizes.
  The initial paths are the client and server
  processes' working directories, not the remote shell's current directory.
  This honors `MOSH_ESCAPE_KEY`, including the leading Enter required for
  printable prefixes. Both legacy control bytes and Kitty keyboard events
  are supported; no Option-as-Meta configuration is needed. Alt-0 is no
  longer intercepted. Bracketed paste cannot activate the shortcut, and it
  is bypassed during tmux -CC passthrough.

  **Enter, F5 or c** copies the selected file or directory into the other pane's
  directory immediately, without leaving the browser or asking for confirmation.
  Matching files are updated; x cancels the active transfer and queue. Bracketed
  paste and Kitty key-repeat/release events cannot start copies. Remote-initiated
  Downloads offers still require explicit permission.
  Folder copies include nested and empty directories, without buffering an
  entire archive in memory. They merge into a same-named destination folder;
  extra destination files are never deleted. Symlinks and special files are
  rejected, not followed. Completed files remain if a later file fails.

  Matching regular files use **librsync**: the destination sends its block
  signatures back, the source computes a delta, and the destination patches
  its original into a temporary file. Both signatures and deltas use the
  background FEC path, with zstd level 22 and loss recovery in both directions.
  A delta larger than the source falls back to sending the file. SHA-256 checks
  the reconstructed contents before atomic replacement; a detected concurrent
  destination change fails the copy. New-file name collisions never overwrite.
  Keyboard, screen and socket forwarding retain priority over file data.

  The popup shows the active job, queued count, completed file count, payload,
  signature and delta bytes. Up to eight jobs can wait behind the active job.
  **x** cancels the active job and queue, retaining completed files.
  Unsent data is discarded on cancel, but the bounded in-flight FEC window
  can still drain before another job starts. Hiding the popup preserves the
  queue; a temporary network disconnection retains the workers and pending
  FEC records. This is not persistent resume after session
  exit. Progress is streamed with bounded buffering, not a pre-scanned total.
  Transfer limits are 64 GiB per file, one million tree entries and 64 path
  components. Permissions for regular files are copied, excluding special bits;
  ownership, timestamps, hard-link relationships and directory modes are not.

  Menu copying negotiates `file-sync-v1` and requires librsync 2.3+ plus OpenSSL
  3 development files at build time on both ends. Use `--with-librsync` to require
  it or `--without-librsync` for a browsing-only build. Debian/Ubuntu build
  packages are `librsync-dev` and `libssl-dev`; on macOS install `librsync` and
  `openssl@3` with Homebrew. Pass its include/library directories via `CPPFLAGS`
  and `LDFLAGS` if needed. This feature does not change the separate
  `goblin-moshcp` CLI protocol. Live forwarding edits are not yet connected to
  this popup; UDP jump routes are selected at session startup with `-J`.

  The requested audio page (independent input/output codecs, on/off, volume,
  microphone meter and headphone test) is specified in [AUDIO.md](AUDIO.md)
  and is not implemented yet. See [THIRD_PARTY.md](THIRD_PARTY.md) for codec
  licensing and the source-distribution checklist.

  Directory access runs in a separate, killable helper on each endpoint.
  The terminal loop never calls readdir/stat or waits for the helper. Internal
  windows contain at most 64 entries and approximately 4 KiB including names,
  sizes and framing. Scrolling pulls only the next window as needed, with at
  most one window of lookahead. Eight windows are cached; backwards scrolling
  can refetch evicted windows using bounded, session-local directory cookies.
  Each host scans and alphabetically sorts its own directory names before
  returning the first window. ASCII case is ignored, with original-byte ties;
  non-ASCII ordering is bytewise, not locale collation. This full name scan
  stays inside the worker: a 100k-entry list is not sent across the link.
  Only the requested window's metadata is read. The sorted-name cache is
  limited to one million names or an estimated 64 MiB per worker.

  Both endpoints check the watched window every three seconds while browsing.
  Unchanged checks produce no network messages. Size changes send only changed
  metadata. Added/removed names or directory replacement invalidate the
  generation and renew the first bounded window, retaining the selected name
  if present. Deep scroll positions restart on membership changes rather than
  seeking an invalid cookie; the worker rebuilds its sorted name snapshot.
  This is a live view, not an atomic filesystem snapshot. Checks may be delayed
  by filesystem/network latency; hidden panes pause watching and resume on open.

  The wrapper negotiates `directory-v2` once at bootstrap, with read-only
  non-watching `directory-v1` fallback for older peers. Requests and
  responses use reserved reliable stream 0, not bulk file datagrams or
  terminal escape sequences. Only one 512-byte metadata chunk may be
  unacknowledged, with a minimum 25 ms interval; input and screen updates
  proceed independently. SST repairs loss without duplicating requests.
  New navigation has a new request ID, so a stale reply cannot replace it.
  Outbound metadata is capped at 16 KiB and pending navigation coalesces to
  the latest request. Queries time out after 30 seconds. A helper stuck in
  uninterruptible kernel I/O must be reaped before its replacement starts:
  browsing may remain unavailable, but the connection continues. Hiding
  the popup does not cancel an in-flight query or fetch additional pages.

  New peers also negotiate `link-budget-v1`. Each sender independently paces
  its own direction, starting at 240 IP bytes/s (1.92 kbit/s). Real packet
  delivery, loss and RTT feedback adjust that budget while traffic is active;
  no padding speed test or idle measurement traffic is generated. Feedback
  uses the existing authenticated packet sequence numbers, adding no headers
  to data packets. Reports are 32 payload bytes, coalesced for 50 ms–3 seconds
  (or 16 data packets at higher rates); IP/UDP/crypto overhead also consumes
  the pacing budget. A separate cap reserves at least three quarters of the
  outgoing budget for other traffic, delaying reports further on slow reverse
  links. Rates are estimates, not promises of physical capacity:
  sparse traffic cannot measure unused bandwidth and congestion probing can
  still lose packets. Long idle periods discard stale outgoing estimates.
  A clean, busy link can double its budget each feedback round (at least
  100 ms or one RTT); congestion switches to gentler growth. Clear evidence
  of a much slower path cuts directly toward its measured delivery rate.
  Unacknowledged background traffic is bounded to 512 packets so a sudden
  downgrade cannot keep flooding the link while feedback is delayed. Missing
  history outside a report's selective bitmap is not counted as packet loss.

  The browser's kbit/s line shows **traffic / send budget** separately for Up
  and Down, with outgoing delivery loss. The pacer covers terminal fragments,
  forwarding and bulk/FEC packets together; foreground traffic can borrow
  a bounded 128 bytes, and background data yields to interactive work. The
  file channel can use paced opportunities while a screen update is waiting
  for its next frame timer; a dirty screen alone does not block file replies.
  Due foreground work still runs before bulk data. The
  pacer allows at most 1 ms / 4 KiB of scheduling slack, without idle burst
  accumulation. The file menu bypasses its legacy 20 ms per-symbol delay
  when this pacer is active; old peers retain that fallback delay. The
  local bulk socket backpressures `goblin-moshcp --rate=0` instead of growing
  an unbounded queue. Forwarding uses its local directional estimate unless
  `--stream-bandwidth` specifies a manual ceiling. The moshcp `--rate` ceiling
  still applies (default 2048 bytes/s); `--rate=0` removes that extra ceiling,
  not the session pacer. Older peers retain the legacy transport behavior.

  For native tmux integration in a supporting terminal such as iTerm2:

    $ goblin-mosh --tmux-control [user@]host -- tmux -CC new-session -A -s main

  You can also connect with `--tmux-control` and run `tmux -CC` from the
  remote shell later. Both endpoints must support `tmux-control-v1`; the
  wrapper negotiates this once during setup and refuses an unsupported peer.
  The local terminal handles the [tmux control protocol](https://github.com/tmux/tmux/wiki/Control-Mode)
  and provides its native tabs and panes. This option supports the DCS-framed
  `-CC` integration, not the unframed `-C` testing interface.

  Control output and commands travel as ordered, reliable bytes through the
  existing encrypted, compressed UDP transport. They survive packet loss and
  roaming without being reduced to screen diffs. Unacknowledged queues apply
  backpressure at 64 KiB (plus at most one PTY read), so a stalled connection
  does not accumulate unlimited protocol history in Mosh. Because every byte
  must arrive, a busy tmux pane can cost more bandwidth and take longer to
  catch up than ordinary screen synchronization.

  While tmux control mode is active, Mosh suppresses screen redraws, prediction,
  status overlays, and its escape-key shortcuts so they cannot corrupt the
  protocol. Use the local terminal's tmux detach action to return to the remote
  shell; normal Mosh rendering then resumes. A new Mosh client process requires
  a fresh tmux attachment. Existing Mosh connections can still roam and recover
  from temporary network loss.

  Local forwarding follows the OpenSSH-style command line:

    $ goblin-mosh -L 8080:127.0.0.1:80 [user@]host
    $ goblin-mosh -R 2222:127.0.0.1:22 [user@]host
    $ goblin-mosh -D 1080 [user@]host
    $ goblin-mosh -A [user@]host
    $ goblin-mosh -X [user@]host
    $ goblin-mosh --fips-crypto [user@]host

  `--fips-crypto` is an explicit, fail-closed transport mode intended as a
  building block for deployments with FIPS requirements.  Client and server
  negotiate `aes128-gcm-v1` once during the SSH bootstrap, then use
  AES-128-GCM and HKDF-SHA-256 fetched only from an installed OpenSSL 3 FIPS
  provider.  Session keys, AES-GCM IVs, chaff, and X11 cookies use the
  provider's CTR-DRBG; separate client-to-server and server-to-client traffic
  keys are derived for each session.  The provider generates every GCM IV
  internally, adding 12 bytes to each UDP datagram compared with the default
  OCB framing.  If either endpoint cannot load and self-test the FIPS provider,
  or the remote endpoint does not confirm the suite, setup stops rather than
  falling back.

  This option does not claim that a goblin-mosh installation or complete
  system is FIPS compliant.  A deployment still needs an appropriate validated
  OpenSSL module and configuration, an approved system entropy source, a
  compliant SSH bootstrap and operating environment, and the required
  operational controls and validation evidence.  Normal sessions continue to
  use Mosh's existing AES-128-OCB transport.  Configure auto-detects OpenSSL 3;
  use `./configure --with-fips-crypto=yes` to require build support, or
  `--without-fips-crypto` to omit it.

  Forwarded stream bytes are coalesced before being sent and are rate
  limited separately from terminal updates.  The coalescing delay and stream
  payload cap can be tuned with `--stream-delay=MS` and
  `--stream-bandwidth=BPS`.  Terminal screen updates and keystrokes are
  scheduled ahead of forwarded streams.  X11 and SSH agent forwarding are
  medium priority with their own stream token bucket; TCP and SOCKS
  forwarding are low priority with a separate bucket.  Mosh emits at most one
  forwarded stream event per reliable send opportunity and caps stream chunks
  below the path-MTU payload budget.  `goblin-moshcp` bulk datagrams are sent only
  when no reliable terminal or forwarded stream traffic is queued.

  Sustained terminal state updates are paced between 10 frames per second on
  low-RTT paths and 4 frames per second on high-RTT paths.  The first update
  after an idle period still leaves promptly.  After 15 seconds with no
  traffic, clients and servers that negotiate `keepalive-v1` use an
  authenticated connection-layer ping and pong.  Continued inactivity backs
  the interval off through 15 seconds, 30 seconds, 1, 2, 4, 8, and 15 minutes;
  real terminal, forwarding, or bulk traffic resets it to 15 seconds.  These
  keepalives bypass terminal state serialization, compression, fragmentation,
  and chaff.  Mixed-version connections retain the legacy state heartbeat.

  Native terminal scrolling is enabled by default.  `goblin-mosh` stays on
  the client's primary screen and turns recognized full-screen upward motion
  into real terminal scroll operations, allowing Ghostty, xterm, Kitty, and
  similar terminals to retain the rows they receive in their own scrollback.
  Because Mosh synchronizes visible state, it cannot recover intermediate
  output that was skipped while a link was slow or disconnected.  Use
  `--alternate-screen` to restore the isolated, no-scrollback behavior of
  traditional Mosh; `--no-init` remains an alias for native scrolling.

  `goblin-moshcp` transfers files over an active Mosh session.  Start a receiver on
  one side of the session and a sender on the other:

    remote$ goblin-moshcp receive .
    local$  goblin-moshcp send --rate=2k --redundancy=20% ./file.bin

  `goblin-moshcp` discovers the active session through `GOBLIN_MOSHCP_SOCK` or the
  latest `goblin-moshcp.latest` control socket in the user's runtime directory.  This is intended
  for the normal "one active Mosh session" case; like SSH agent forwarding,
  it can point at the wrong session if a shell survives across tmux or
  reconnect handoffs. Prebuilt Debian/Ubuntu, RPM and Homebrew artifacts use
  the built-in Reed-Solomon FEC and exclude RaptorQ. Plain source builds also
  disable RaptorQ, even when its headers are installed. It requires an explicit
  custom build with `--with-libraptorq=DIR` (or `--with-libraptorq` for system
  headers), after reviewing the patent terms in [THIRD_PARTY.md](THIRD_PARTY.md).
  Only those custom builds can use `goblin-moshcp send --fec=raptorq`.
  `goblin-moshcp send` accepts multiple sources, `-r` for
  recursive directories, `-p` to preserve modes and mtimes, and optional zstd
  compression with `-z --zstd-level=N`.  `goblin-moshcp receive --multi` keeps one
  receiver open for several simultaneous transfer ids and prints lightweight
  status counters unless `--quiet` is used.

  Terminal state compression is a core part of this branch's low-bandwidth
  strategy.  After both peers advertise zstd support, every serialized state
  update uses zstd level 22.  There is no size threshold or zlib comparison.
  Initial packets remain zlib-compressed for compatibility with older Mosh
  peers.  For very slow links, you can train a session-specific zstd
  dictionary from received state samples:

    $ goblin-mosh --state-sample-log=alpine.samples.zst host
    $ goblin-mosh-compile-dictionary --input=alpine.samples.zst --output=alpine.dict
    $ goblin-mosh --state-zstd-dict=alpine.dict host

  Static Kitty graphics sent as RGB, RGBA, or PNG are normalized once to
  lossless WebP and synchronized as image and placement state.  Placement-only
  changes do not resend pixels.  The client decodes WebP and emits standard
  Kitty RGBA commands to the local terminal.  The wrapper also exposes the
  local terminal type to the remote login session, so a client running with
  `TERM=xterm-ghostty` advertises Ghostty's Kitty graphics support to remote
  applications.  The corresponding terminfo entry must be installed on the
  remote host.

  Incoming Kitty uploads support inline data, regular files, temporary files
  and POSIX shared memory on the server. References are resolved there, not
  on the client's filesystem. Resource limits still apply. OSC 66 text sizing
  and Kitty keyboard events (including configured Alt events) are retained;
  clients without text-sizing support receive ordinary text. Live integration
  tests cover 5,120-pixel-wide images through a lossy UDP relay, not every
  combination of GUI terminal and nested multiplexer.

  OSC 5522 MIME clipboard traffic uses separate authenticated FEC lanes,
  outside screen-state synchronization. Local-to-remote traffic, text/plain
  clipboard content and interactive replies use the high-priority lane.
  Large, volunteered remote non-text writes use the background lane. Set
  `MOSH_CLIPBOARD_FAST_THRESHOLD` in bytes on the client to change the default
  64 KiB cutoff. These terminal extensions do not imply support for every
  Kitty protocol extension.

  Pixel-sized Kitty images retain their original dimensions. Cursor movement
  uses the current attachment's cell dimensions, including when applications
  such as `gnuplot`'s `kittycairo` omit a `c,r` cell rectangle. Specifying just
  `c` or `r` preserves the source aspect ratio. Without usable pixel geometry,
  cursor placement uses an 8-by-16-pixel cell estimate; out-of-screen cursor
  movement is clamped to the screen/scroll-region edge. This does not rescale
  the image. To request a larger gnuplot plot, specify e.g.
  `set terminal kittycairo size 1280,960`.

  State sample logs contain the uncompressed terminal update stream and may
  include sensitive terminal contents.  A dictionary passed with
  `--state-zstd-dict=FILE` is stored as a zstd level-22 compressed file,
  uploaded over SSH during setup, decompressed by `goblin-mosh-server`, used for
  that Mosh session only, and not cached by `goblin-mosh-server`.

  If the `goblin-mosh-client` or `goblin-mosh-server` binaries live outside the user's
  `$PATH`, `goblin-mosh` accepts the arguments `--client=PATH` and `--server=PATH` to
  select alternate locations. More options are documented in the goblin-mosh(1) manual
  page.

  There are [more examples](https://mosh.org/#usage) and a
  [FAQ](https://mosh.org/#faq) on the Mosh web site.

How it works
------------

  The `goblin-mosh` program will SSH to `user@host` to establish the connection.
  SSH may prompt the user for a password or use public-key
  authentication to log in.

  From this point, `goblin-mosh` runs the `goblin-mosh-server` process (as the user)
  on the server machine. The server process listens on a high UDP port
  and sends its port number and an AES-128 secret key back to the
  client over SSH. The SSH connection is then shut down and the
  terminal session begins over UDP.

  If the client changes IP addresses, the server will begin sending
  to the client on the new IP address within a few seconds.

  To function, Mosh requires UDP datagrams to be passed between client
  and server. By default, `goblin-mosh` uses a port number between 60000 and
  61000, but the user can select a particular port with the -p option.
  Please note that the -p option has no effect on the port used by SSH.

  Forwarded streams, X11 forwarding, SSH agent forwarding, and `goblin-moshcp` bulk
  datagrams use the same encrypted and authenticated UDP session as terminal
  traffic.  Bulk data is scheduled conservatively so that interactive terminal
  updates remain responsive on slow or lossy links.  Oversized bulk datagrams
  are rejected instead of being handed to UDP fragmentation.

Advice to distributors
----------------------

A note on compiler flags: Mosh is security-sensitive code. When making
automated builds for a binary package, we recommend passing the option
`--enable-compile-warnings=error` to `./configure`. On GNU/Linux with
`g++` or `clang++`, the package should compile cleanly with
`-Werror`. Please report a bug if it doesn't.

Where available, Mosh builds with a variety of binary hardening flags
such as `-fstack-protector-all`, `-D_FORTIFY_SOURCE=2`, etc.  These
provide proactive security against the possibility of a memory
corruption bug in Mosh or one of the libraries it uses.  For a full
list of flags, search for `HARDEN` in `configure.ac`.  The `configure`
script detects which flags are supported by your compiler, and enables
them automatically.  To disable this detection, pass
`--disable-hardening` to `./configure`.  Please report a bug if you
have trouble with the default settings; we would like as many users as
possible to be running a configuration as secure as possible.

Mosh ships with a default optimization setting of `-O2`. Some
distributors have asked about changing this to `-Os` (which causes a
compiler to prefer space optimizations to time optimizations). We have
benchmarked with the included `src/examples/benchmark` program to test
this. The results are that `-O2` is 40% faster than `-Os` with g++ 4.6
on GNU/Linux, and 16% faster than `-Os` with clang++ 3.1 on Mac OS
X. In both cases, `-Os` did produce a smaller binary (by up to 40%,
saving almost 200 kilobytes on disk). While Mosh is not especially CPU
intensive and mostly sits idle when the user is not typing, we think
the results suggest that `-O2` (the default) is preferable.

Our Debian and Fedora packaging presents Mosh as a single package.
Mosh has a Perl dependency that is only required for client use.  For
some platforms, it may make sense to have separate goblin-mosh-server and
goblin-mosh-client packages to allow goblin-mosh-server usage without Perl.

Notes for developers
--------------------

To start contributing to Mosh, install the following dependencies:

Debian, Windows Subsystem for Linux:

```
$ sudo apt install -y build-essential protobuf-compiler \
    libprotobuf-dev pkg-config libutempter-dev zlib1g-dev libncurses5-dev \
    libssl-dev libpng-dev libwebp-dev libzstd-dev bash-completion tmux less
```

Fedora, RHEL:

```
$ sudo dnf group install development-tools
$ sudo dnf install automake protobuf-compiler protobuf-devel libutempter-devel \
    zlib-ng-compat-devel ncurses-devel openssl-devel libpng-devel libwebp-devel \
    libzstd-devel bash-completion tmux less perl-diagnostics
```

MacOS:

```
$ brew install protobuf automake libpng webp zstd
```

Once you have forked the repository, run the following to build and test Mosh:

```
$ ./autogen.sh
$ ./configure
$ make
$ make check
```

Mosh supports producing code coverage reports by tests, but this feature is
disabled by default. To enable it, make sure `lcov` is installed on your
system. Then, configure and run tests:

```
$ ./configure --enable-code-coverage
$ make check-code-coverage
```

This will run all tests and produce a coverage report in HTML form that can be
opened with your favorite browser. Ideally, newly added code should strive for
90% (or better) incremental test coverage.

More info
---------

  * Mosh Web site:

    <https://mosh.org>

  * `mosh-devel@mit.edu` mailing list:

    <https://mailman.mit.edu/mailman/listinfo/mosh-devel>

  * `mosh-users@mit.edu` mailing list:

    <https://mailman.mit.edu/mailman/listinfo/mosh-users>

  * `#mosh` channel on [Libera Chat](https://libera.chat/)

    https://web.libera.chat/#mosh
