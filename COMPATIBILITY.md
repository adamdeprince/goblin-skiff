<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff version compatibility

`1.4.0-goblin20260915.1` is the first compatibility baseline. Its Goblin
protocol is **1**; the underlying Mosh SST protocol remains **2**. The tracked
`GOBLIN_VERSION` file identifies the source release, independently of the Git
build identifier and distribution-specific package revisions.

From this baseline, compatible releases should work in both directions:
new client with baseline server, and baseline client with new server. Keep
Goblin protocol 1 for compatible changes. Negotiate optional functionality
with capabilities, preserve existing meanings and protobuf field numbers,
and fall back when a peer lacks a feature. Do not infer feature availability
from a release date or require identical releases/builds. An unavoidable
breaking change requires an explicit protocol bump and migration notes.

Kitty images outside WebP's dimension range use the optional `png` field in
image state only when the selected client advertises `png-state-v1` through
`--image-codecs` and `MOSH_CLIENT_CAPS`. Existing WebP and palette/DjVu state is
unchanged. Transport instructions advertise `large_fragments_supported`
(field 13). After receiving that capability, a sender may use a 16-byte
fragment header for updates needing more than 32768 packets: the high bit of
the instruction ID marks the extended header, followed by a 64-bit fragment
index whose high bit marks the final packet. Smaller updates and traffic to
older peers retain the original 10-byte header. Older peers cannot receive
snapshots exceeding their original fragment range.

Silkie's extensions add optional fields that older peers ignore: user
instructions `client_extensions` (14) and `ext_input` (15), host instructions
`ext_output` (13) and `ext_state` (14), the kitty image fields `vector`,
`vector_lineage`, `vector_version`, `hdr`, `tone_map` and `gamut` (10–15), and
`vector_patch` (5) in kitty state deltas. A server answers the extensions only
after a client describes its terminal, and accepts `f=200` and `f=64` images
only when that terminal does. An older client therefore sees nothing new, and
an older server never receives a description, so it answers nothing.

## Connection identification

`goblin-skiff --version`, `goblin-skiff-client --version` and
`goblin-skiff-server --version` report the release and build. The wrapper
queries the selected client binary with `--connection-version`; the server
emits the same metadata before its unchanged `MOSH CONNECT` message:

```
MOSH VERSION <goblin-protocol> <release> <build>
```

The wrapper displays both releases when connecting and rejects different
Goblin protocols before starting the client or any jump relays. It compares
the selected client, not the wrapper's own version. An older endpoint that
does not report a version is labelled `unversioned`: existing feature
fallbacks remain available, but pre-baseline builds have no compatibility
guarantee. Relay framing retains its separate `MOSH RELAY 1` version.

Both endpoints also exchange release, build and Goblin protocol inside
authenticated SST instructions (fields 10–12). This covers direct binary
connections as well as wrapper connections. Metadata repeats until the
first nonzero state is acknowledged, so loss and reordering cannot lose the
identification; later updates and compact idle pings pay no version overhead.
Each endpoint retains the peer identity for the session and reports it with
`-v`. Incompatible protocols, partial/invalid metadata and identity changes
are rejected before state is applied or acknowledged. Missing metadata
never erases an identity already learned.

## Making a release

1. Update `GOBLIN_VERSION` using `1.4.0-goblinYYYYMMDD.N` (the `1.4.0`
   component identifies the upstream base). Add an entry in `CHANGELOG.md`
   and update `html/index.html` with source changes and compatibility notes.
2. Keep protocol 1 unless compatibility actually breaks. Add a new
   capability for new optional wire behavior and test its fallback.
3. Run `make check`, including `session-version`, `udp-jump-wrapper.test`
   and the real startup/graphics/session checks. Before publishing future
   releases, also exercise new-client/baseline-server and
   baseline-client/new-server combinations against this baseline build.
4. Update the packaging recipes' explicit package versions/source filenames
   and build immutable source and binary packages with corresponding package
   revisions and source checksums. The site's package table describes
   published artifacts; a source commit alone does not update those packages.

`html/index.html` and its `goblin.png` artwork are tracked alongside the code
so changes to the product page can be reviewed in Git.
