<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Debian and Ubuntu package builds

The package is `goblin-skiff`; its commands are installed in `/usr/bin` and
coexist with distribution Mosh. Do not install files in `/usr/local` from a
Debian maintainer script or silently delete an administrator's manual build.

Build separately for Ubuntu 22.04 (`jammy`), 24.04 (`noble`), 26.04
(`resolute`), Debian 12 (`bookworm`) and Debian 13 (`trixie`). The release
suffix is part of the package version. Do not put different distributions'
builds in a common APT suite: their library ABIs and package names differ.

Use the official container image for that release, pinning its resolved
digest in the build invocation and saving that digest with the artifacts.
`Containerfile` expects `snapshot.tar.gz` containing one `goblin-skiff/`
directory with the distribution source, `debian/`, and these build recipes.
It also expects a copy of `build-release.sh` next to the Containerfile.

Example, from that prepared container build context:

```sh
podman build --build-arg BASE_IMAGE=docker.io/library/debian:12 \
  -f Containerfile -t goblin-skiff-build:bookworm .
mkdir -p artifacts/bookworm
podman run --rm -v "$PWD/artifacts/bookworm:/out" \
  goblin-skiff-build:bookworm bookworm '1.4.0+goblin20260919.1+bookworm1'
```

The container builds the binary and complete matching native source package,
runs 28 core tests, installs its package, and runs an encrypted UDP/PTY
file-transfer integration test. `/out` contains the `.deb`, `.dsc`, source
archive, build information, changes, and verification logs. No HTML product
page, Git metadata, signing key, or host configuration belongs in the source
snapshot. Dependencies remain dynamically linked system libraries.

From a downloaded source package, install its build dependencies and use
`dpkg-buildpackage -us -uc -b` (or `./build-package.sh`). Configure requires
zstd, librsync and OpenSSL 3; the optional FIPS-provider mode is built but no
validated FIPS module is included. No audio codecs are vendored in this build.

Prebuilt artifacts must use the bundled Reed-Solomon FEC, with RaptorQ excluded.
`debian/rules` passes `--without-libraptorq` and checks the built executable even
when `DEB_BUILD_OPTIONS=nocheck` is set. Installed-package verification repeats
`sh packaging/check-prebuilt-fec.sh /usr/bin/goblin-skiffcp` and saves
`prebuilt-fec.log`. RaptorQ requires explicit custom compilation and patent
review; see [THIRD_PARTY.md](../../THIRD_PARTY.md).

Publish each `.deb` and matching `.dsc` into its release-named suite using
the existing private reprepro configuration on hail. Keep private signing
material outside the public distribution root. The web documentation and
package catalog must list only artifacts that have actually been published
and tested through APT, with architecture and distribution compatibility.
