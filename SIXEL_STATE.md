# Sixel state implementation

Remote sixel is integrated into live sessions in the current source tree.
It is negotiated with `sixel-state-v1`; local capability discovery is separate
from the client-local startup mascot. Published packages are versioned snapshots,
not automatic deployments of every change to this document.

## Agreed behavior

Sixel images belong in the normal terminal state alongside character cells.
Use the existing state/delta transport, not a separate graphics channel or a
reliable log of drawing commands. Store pixels as WebP or palette/DjVu with
image origin and placement. Do not put client capabilities or cell geometry
in the replicated framebuffer.

For sixel-origin images, the client uses native sixel whenever it supports
sixel, including when it also supports Kitty. Only a Kitty-capable client
without sixel gets client-side Kitty conversion. A client supporting neither
must not receive unsupported graphics escape sequences.

## Implementation

- `src/terminal/sixel.cc`: complete-DCS decoder, RGB/HLS palettes, repeat
  runs, raster dimensions, integer vertical aspect ratios, transparency,
  native sixel encoder, and client renderer selection/encoding helpers.
- Input/output byte caps, dimension/pixel caps, palette caps and a drawing
  work budget. The decoder validates layout before allocating pixels.
- Sixel origin carried in the existing image-state protobuf; default Kitty
  origin adds no bytes to existing Kitty image messages.
- Normal image/placement deltas preserve sixel origin. Unchanged image
  content is not resent merely because text changes. Ordinary Kitty output
  explicitly excludes sixel-origin images.
- Tests cover native sixel and WebP roundtrips, client-only Kitty fallback,
  malformed input/quotas, state recovery after a skipped update, and reset.

The terminal DCS parser now feeds bounded image ingestion and placement.
The server waits for attachment capabilities before enabling sixel responses;
`--no-kitty` and `--no-sixel` also mask the live renderer. The compositor handles
image coverage alongside text, scrolling and repaint. Native sixel has no
delete-by-ID, so changed coverage is erased and repainted as needed. Shared and
private palette state and sixel display/cursor modes are handled by the parser.

## Limits and current verification

The shared graphics limits are 16,383 pixels per dimension and 32 million
pixels per image, with additional encoded-byte, resident-image and drawing-work
quotas. Native output uses square pixels and RGB percentages; palette rounding
can change an 8-bit channel by about one unit. This is static image-state
support, not a claim of every historical sixel terminal behavior or every
Kitty protocol extension.

`graphics-integration.test` exercises real client/server PTYs and encrypted,
lossy UDP with simulated sixel-only, Kitty-only, dual-capable and non-graphics
terminals. It checks a 5,120-pixel-wide image, unchanged-image retention,
client-side conversion, OSC 66 fallback, Alt keyboard events and clean exit.
`sixel-state` covers codec/state behavior; `terminal-extensions` covers sized
text and keyboard state. These are automated protocol checks, not exhaustive
GUI or nested-terminal compatibility tests.

## Historical verification before live integration (2026-09-06)

The current build was installed under `/usr/local` locally and on naamah,
including inline startup scrollback preservation and `--no-kitty`/`--no-sixel`.
Installed loopback sessions on both hosts and a Mac-to-naamah session passed,
including clean exit, disabled graphics output, and no startup screen erase.
HTML was excluded, and the previous binaries and man pages were backed up.

The development tree passed `make -j8 check` on macOS and in an isolated
Linux build on naamah: 50 tests, 47 passes, two expected failures, and the
optional unconfigured FIPS-provider test skipped. Linux used
`CXXFLAGS="-g -O2 -Wno-error=effc++"` to keep pre-existing advisory ownership
warnings visible without treating them as errors.

An additional local AddressSanitizer/UndefinedBehaviorSanitizer run passed
for the new codec and test translation units. Its existing linked libraries
were not instrumented. Mixing those objects with Homebrew Protobuf 36 required
`-DPROTOBUF_MESSAGE_GLOBALS_TEMPORARY_OPTOUT` to match the prebuilt protobuf
default-instance representation; without it the mixed-build check crashed
in protobuf extension allocation.

Protocol references: [XTerm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h3-Sixel-Graphics),
[DEC VT330/VT340 pocket guide](https://vt100.net/dec/ek-vt3xx-hr-002.pdf),
and [Windows Terminal's sixel parser](https://github.com/microsoft/terminal/blob/main/src/terminal/adapter/SixelParser.cpp).
