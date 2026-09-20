<!-- Created for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff for Inkline

Goblin Skiff is a fork of Mosh, optimized for bandwidth-constrained links.
The Inkline bundle targets ARMv7 hard float on reMarkable 2, firmware 3.27.
Install it through the [Inkline utility instructions](https://inkline.goblinreactor.com/install.html?utility=goblin-skiff#utilities).

Release `1.4.0+goblin20260919.2.rm2.1` uses source commit
`04f2e1d414f8b03d1ee0ec031453b4c67a3e4078`. The
[release directory](https://inkline.goblinreactor.com/downloads/utilities/2026-09-19/)
contains the bundle, checksums, manifest, validation logs, matching Skiff and
DjVuLibre source, and the exact cross compiler and installer recipes.

The build uses Zig 0.16.0, the reMarkable 5.7.119 target sysroot, native
protoc 25.8, static WebP 1.6.0, and static DjVuLibre 3.5.28 with Debian
`3.5.28-2.2~deb12u1` patches. The manifest records all input hashes. The
tablet's GNU C++ runtime is retained. A private Perl runtime and UTF-8
locale accompany the wrapper; their matching source remains in Inkline's
[shared Debian source archive](https://inkline.goblinreactor.com/downloads/utilities/2026-09-13/sources/debian-sources.tar).

To rebuild, extract the published recipe archive into a workspace. Populate
`source/` with the matching Skiff source, `webp-source/` with WebP, and
`djvu-source/` using `dpkg-source -x` on the published DjVuLibre `.dsc` and
its two accompanying archives. Place the extracted target sysroot at
`.cache/sdk/sysroot-5.7.119`, and the native protoc executable at
`protoc/bin/protoc`. The recipe archive supplies `scripts/` and `cmake/`;
the SDK extraction recipe is also included. Put Zig 0.16.0 on `PATH`, then:

```sh
SKIFF_RM2_WORKSPACE=/absolute/path/to/workspace ./build-rm2.sh
```

Copy the client, server, transfer and dictionary executables from the build
into the recipe's bundle template, together with the configured Perl wrapper
and `djvu-source/tools/cjb2`. Preserve the private runtime and locale recorded
in the manifest. Strip ARM executables with `arm-linux-gnueabihf-strip`, then
regenerate `SHA256SUMS` and the outer archive checksum. The published recipes
include the packaging script and the bundle's complete text files.

The encoder's compiled path is
`/home/root/.local/share/inkline-utilities/goblin-skiff/current/libexec/cjb2`.
The installer creates this layout and installs only Skiff command names.
Automatic downloads default to off. RaptorQ, librsync integration and the
optional FIPS provider are disabled in this tablet profile.

ARM verification under QEMU passed 26 core tests; two disabled-feature tests
were skipped. The private Perl/UTF-8 preflight and RaptorQ exclusion check
passed. Packaged ARM client and server sessions both passed against native
Skiff, exercising real PTYs, UDP, keyboard input and screen output. The
verification container uses a tmpfs for temporary files and mounts the
encoder at its installed path.

On a physical reMarkable 2 running firmware 3.27.3.0, installation, the
private runtime preflight, the complete graphics regression, and terminal
sessions all passed. Upstream Mosh 1.4.0 interoperability passed in both
directions on the tablet. A Mac-to-tablet session also passed over USB with
real SSH bootstrap, keyboard input and screen output. Incoming connections
to the tablet use `--no-ssh-pty`: its Dropbear SSH server can discard output
from short commands when an SSH terminal is allocated. Skiff still creates
the remote session's own terminal.

The superseded fork bundle and its source/checksum download files have been
removed from the Inkline publication. The separately offered upstream Mosh
package and its attribution are preserved.
