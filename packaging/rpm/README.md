# RPM package builds

Build separately in official Fedora 44, Rocky Linux 9 and Rocky Linux 10
containers. Record each resolved base-image digest. This is a third-party
repository, not a Fedora, Rocky or Red Hat endorsed package. The binaries use
each release's system libraries; do not mix Fedora and Enterprise Linux RPMs.
Rocky builds need EPEL and CRB for dependencies. Only x86_64 is built here.

Prepare a source distribution with `make distdir`. Exclude the untracked
product HTML, Git metadata, signing material and host configuration. Create
`snapshot.tar.gz` with a single `goblin-skiff/` directory, including this spec
and the build recipes. Copy `Containerfile`, `goblin-skiff.spec` and
`build-release.sh` beside the snapshot, then:

```sh
podman build --build-arg BASE_IMAGE=registry.fedoraproject.org/fedora:44 \
  -f Containerfile -t goblin-skiff-build:fedora44 .
mkdir -p artifacts/fedora44
podman run --rm -v "$PWD/artifacts/fedora44:/out" \
  goblin-skiff-build:fedora44 fedora44
```

Pin the resolved image digest for a release build. The build produces an RPM
and its complete matching SRPM, runs 24 core tests, installs its RPM, and
exercises encrypted UDP/PTY file transfers with a private runtime directory.
The optional FIPS-provider test skips when the OS has no FIPS provider; the
package is not a validated FIPS module. Audio codecs are not vendored.

Prebuilt artifacts must use the bundled Reed-Solomon FEC, with RaptorQ excluded.
The spec passes `--without-libraptorq` and runs the executable policy gate in
`%build`, so skipping `%check` cannot omit it. Installed-package verification
repeats that gate and saves `prebuilt-fec.log`. RaptorQ requires explicit custom
compilation and patent review; see [THIRD_PARTY.md](../../THIRD_PARTY.md).

The package installs only goblin-prefixed commands under `/usr/bin`; it does
not replace stock Mosh or remove administrator-managed `/usr/local` files.

Before publishing, sign the binary RPM and SRPM on hail using the existing
private Goblin Reactor archive key. Run `createrepo_c` and sign `repomd.xml`
with the same key. Publish below `https://apt.goblinreactor.com/rpm/`, separated
by distribution and release. Client configuration must enable `gpgcheck=1`
and `repo_gpgcheck=1`. Verify a clean public-HTTPS DNF installation, not just a
local RPM installation, and link the matching SRPM in the public catalog.
