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

  Note that `adam-mosh-client` receives an AES session key as an environment
  variable.  If you are porting Mosh to a new operating system, please make
  sure that a running process's environment variables are not readable by other
  users.  We have confirmed that this is the case on GNU/Linux, OS X, and
  FreeBSD.

Usage
-----

  The `adam-mosh-client` binary must exist on the user's machine, and the
  `adam-mosh-server` binary on the remote host.

  The user runs:

    $ adam-mosh [user@]host

  Local forwarding follows the OpenSSH-style command line:

    $ adam-mosh -L 8080:127.0.0.1:80 [user@]host
    $ adam-mosh -R 2222:127.0.0.1:22 [user@]host
    $ adam-mosh -D 1080 [user@]host
    $ adam-mosh -A [user@]host
    $ adam-mosh -X [user@]host

  Forwarded stream bytes are coalesced before being sent and are rate
  limited separately from terminal updates.  The coalescing delay and stream
  payload cap can be tuned with `--stream-delay=MS` and
  `--stream-bandwidth=BPS`.  Terminal screen updates and keystrokes are
  scheduled ahead of forwarded streams.  X11 and SSH agent forwarding are
  medium priority with their own stream token bucket; TCP and SOCKS
  forwarding are low priority with a separate bucket.  Mosh emits at most one
  forwarded stream event per reliable send opportunity and caps stream chunks
  below the path-MTU payload budget.  `adam-moshcp` bulk datagrams are sent only
  when no reliable terminal or forwarded stream traffic is queued.

  `adam-moshcp` transfers files over an active Mosh session.  Start a receiver on
  one side of the session and a sender on the other:

    remote$ adam-moshcp receive .
    local$  adam-moshcp send --rate=2k --redundancy=20% ./file.bin

  `adam-moshcp` discovers the active session through `ADAM_MOSHCP_SOCK` or the
  latest `adam-moshcp.latest` control socket in the user's runtime directory.  This is intended
  for the normal "one active Mosh session" case; like SSH agent forwarding,
  it can point at the wrong session if a shell survives across tmux or
  reconnect handoffs.  The default FEC codec is Reed-Solomon.  Builds
  configured with `--with-libraptorq=DIR` can also use RFC 6330 RaptorQ with
  `adam-moshcp --fec=raptorq`.  `adam-moshcp send` accepts multiple sources, `-r` for
  recursive directories, `-p` to preserve modes and mtimes, and optional zstd
  compression with `-z --zstd-level=N`.  `adam-moshcp receive --multi` keeps one
  receiver open for several simultaneous transfer ids and prints lightweight
  status counters unless `--quiet` is used.

  Terminal state compression is a core part of this branch's low-bandwidth
  strategy.  After both peers advertise zstd support, every serialized state
  update uses zstd level 22.  There is no size threshold or zlib comparison.
  Initial packets remain zlib-compressed for compatibility with older Mosh
  peers.  For very slow links, you can train a session-specific zstd
  dictionary from received state samples:

    $ adam-mosh --state-sample-log=alpine.samples.zst host
    $ adam-mosh-compile-dictionary --input=alpine.samples.zst --output=alpine.dict
    $ adam-mosh --state-zstd-dict=alpine.dict host

  Static Kitty graphics sent as RGB, RGBA, or PNG are normalized once to
  lossless WebP and synchronized as image and placement state.  Placement-only
  changes do not resend pixels.  The client decodes WebP and emits standard
  Kitty RGBA commands to the local terminal.

  State sample logs contain the uncompressed terminal update stream and may
  include sensitive terminal contents.  A dictionary passed with
  `--state-zstd-dict=FILE` is stored as a zstd level-22 compressed file,
  uploaded over SSH during setup, decompressed by `adam-mosh-server`, used for
  that Mosh session only, and not cached by `adam-mosh-server`.

  If the `adam-mosh-client` or `adam-mosh-server` binaries live outside the user's
  `$PATH`, `adam-mosh` accepts the arguments `--client=PATH` and `--server=PATH` to
  select alternate locations. More options are documented in the adam-mosh(1) manual
  page.

  There are [more examples](https://mosh.org/#usage) and a
  [FAQ](https://mosh.org/#faq) on the Mosh web site.

How it works
------------

  The `adam-mosh` program will SSH to `user@host` to establish the connection.
  SSH may prompt the user for a password or use public-key
  authentication to log in.

  From this point, `adam-mosh` runs the `adam-mosh-server` process (as the user)
  on the server machine. The server process listens on a high UDP port
  and sends its port number and an AES-128 secret key back to the
  client over SSH. The SSH connection is then shut down and the
  terminal session begins over UDP.

  If the client changes IP addresses, the server will begin sending
  to the client on the new IP address within a few seconds.

  To function, Mosh requires UDP datagrams to be passed between client
  and server. By default, `adam-mosh` uses a port number between 60000 and
  61000, but the user can select a particular port with the -p option.
  Please note that the -p option has no effect on the port used by SSH.

  Forwarded streams, X11 forwarding, SSH agent forwarding, and `adam-moshcp` bulk
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
some platforms, it may make sense to have separate adam-mosh-server and
adam-mosh-client packages to allow adam-mosh-server usage without Perl.

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
