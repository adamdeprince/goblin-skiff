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
