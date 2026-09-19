<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Homebrew release packaging

The public tap is hosted directly at
`https://brew.goblinreactor.com/homebrew-tap.git`. It contains a formula for
`goblinreactor/tap/goblin-skiff`; stock Homebrew Mosh remains a separate package.
The web root on hail is `/mnt/distribution/brew.goblinreactor.com`.

Build the source distribution with `make distdir`, then archive its single
`goblin-skiff/` directory. Never include product HTML, Git metadata, credentials,
host configuration or signing keys in that snapshot. The formula template is
included in the source; its SHA-256 placeholder avoids a self-referential
archive hash. Render it after archiving with:

```sh
sh packaging/homebrew/render-formula.sh goblin-skiff-1.4.0-goblin20260919.2.tar.gz
```

The formula uses Homebrew's dynamically linked dependencies, runs 28 core
tests, installs the five goblin-prefixed tools and man pages, and supplies a
loopback encrypted file-transfer test for `brew test`. The test uses an
isolated runtime directory and never reuses a real session's sockets.

Bottles must use the bundled Reed-Solomon FEC, with RaptorQ excluded. The
formula passes `--without-libraptorq` and runs the executable policy gate during
installation, independently of optional tests. `brew test` repeats the gate
against the installed executable. RaptorQ requires explicit custom compilation
and patent review; see [THIRD_PARTY.md](../../THIRD_PARTY.md).

Build the bottle on its actual target macOS/architecture:

```sh
brew install --build-bottle --include-test goblinreactor/tap/goblin-skiff
brew test goblinreactor/tap/goblin-skiff
brew bottle --json --root-url=https://brew.goblinreactor.com/bottles \
  goblinreactor/tap/goblin-skiff
```

Keep existing installations and running sessions untouched while testing.
Do not use `brew link --overwrite` to erase an administrator's manual files.
Disable automatic cleanup, updates and dependent upgrades during release
verification on a shared workstation. Inspect the bottle's runtime dependency
versions, relocation results and actual platform tag; do not relabel a bottle
for an untested OS or CPU. Install the published bottle through HTTPS and rerun
the test, not only the source-built keg.

Publish the immutable source and bottle before publishing the formula commit
that references them. Keep the tap small: formula and Markdown documentation
only, never bottles, source tarballs or website HTML. Use the maintainer's
configured Git identity, with no assistant attribution. A bare Git repository
served over static HTTP needs `git update-server-info` after every push; test
both `brew tap` and later `git fetch` over the public HTTPS endpoint.

The first binary target is Apple Silicon on macOS 26 (Homebrew's arm64_tahoe
tag). Other macOS builds may use the source formula but are not verified by
this release. Linux users have the separately built APT and RPM packages.
Publish the supported inventory, setup/update/remove instructions, dependency
notes, matching source and verification artifacts on the Brew website. Sign
the public release catalog on hail with its existing private archive key;
never put that private key in the public directory.
