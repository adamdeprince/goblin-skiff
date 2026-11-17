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

Peers need nothing to take a zstd frame of a lower level, or a resent update
whose fragments keep their id.

Silkie's extensions add optional fields that older peers ignore: user
instructions `client_extensions` (14) and `ext_input` (15), host instructions
`ext_output` (13) and `ext_state` (14), the kitty image fields `vector`,
`vector_lineage`, `vector_version`, `hdr`, `tone_map` and `gamut` (10–15), and
`vector_patch` (5) in kitty state deltas. A server answers the extensions only
after a client describes its terminal, and accepts `f=200` images only when
that terminal does. `f=64` and `G` images are kept as sent and converted by
the client as it draws them, so a client from before that change passes them
to its terminal unconverted. A client from before Silkie support ignores these
fields, and an older server never receives a description, so it answers
nothing.

Kitty state deltas add `font` (20) and `delete_font` (21), of message
`SessionFont`: the fonts uploaded to the vector service. Older peers ignore
them, and a client without them cannot give a repainted terminal its fonts.

Pad labels are now part of `ext_state`, as `pad_label` (14), and no longer
travel as `ext_output`. A client from before this change, with a server from
after it, does not give its terminal the labels; a client from after it takes
labels either way.

Transport instructions also advertise `motion_records_supported` (field 14)
and `raw_frames_supported` (15). To a peer that advertised the first, a client
writes each run of `ext_input` reports in a user diff as one `motion_records`
(16) record, the binary form in `src/terminal/motioncodec.h`: the text comes
back byte for byte at the server, and the ink the terminal drew is sent as
the map it was rendered with. The record's context (its shapes, tracks and
last samples) is part of the user stream's state, kept the same at both ends
by noting every report that enters the stream, so a diff is written from the
context of the state the receiver has. The sender changes over only while
nothing is in flight, so a diff written again for a resend stays the same
bytes. To a peer
that advertised the second, an instruction that compression would only
lengthen is sent as it is behind a zero byte, which no zlib or zstd frame
begins with; a receiver takes such a frame whether or not it advertised.
Older peers see neither.

Notifications add `notices_supported` (field 16) to transport instructions,
and two bulk datagram types, `Notice` (11) and `NoticeAck` (12), described in
`src/network/noticechannel.h`. A build from before them does not know these
as datagrams at all, so neither end writes one until an instruction from its
peer has said it takes them. To an older client a server sends notifications
as `ext_output` frames, which that client writes to its terminal; to an older
server a client sends the terminal's `OSC 99` answers as keystrokes, as
before. `client_extensions` adds `notifications` (10) and `notification_routes`
(11), the terminal's answer to the `OSC 99` query; without them a server
answers no such query.

Silkie's packed image formats use the optional `packed` (17) and
`packed_format` (18) fields of image state only when the client advertises
`packed-state-v2` through `--image-codecs` and `MOSH_CLIENT_CAPS`, and zstd
inside `packed` only with `packed-zstd-v1` as well. Other clients receive the
same pixels expanded, in a lossless codec whatever `--lossy` says. Placements
add `depth` (21), `depth_pixels` (22) and `phase` (23), which an older client
ignores: it places the image on the plane of the terminal. The depth of text
crosses as `OSC 777 ; depth` frames in the host bytes, which an older client
ignores too.

## Toolchain

Goblin Skiff is C++23 (`AX_CXX_COMPILE_STDCXX([23])`): gcc 11, clang 14 and
Apple clang 15 or later. Older compilers fail at `configure`. The Debian, RPM,
Homebrew and Inkline recipes meet this; a host built by hand needs a compiler
of that age.

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

`packed-state-v2` uses the current Silkie format numbers (`301/302/304/308/516`)
with the same packed container. Clients advertising only the earlier
`packed-state-v1` receive expanded lossless pixels, avoiding its conflicting
`8/16` meanings. `native-image-v1` adds image fields `native` (19),
`native_format` (20) and `transfer` (21) for exact indexed, palette and
high precision source pixels. Those formats are refused with `ENOTSUP` unless
the client negotiates this encoding. Program Status Protocol (`OSC 7501`)
uses the existing notice channel; older clients receive it in the ordinary
ordered extension output stream.
