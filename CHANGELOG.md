<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff changes

## Unreleased

- Relay Silkie's OSC 777 extensions: capabilities, tablet pointer, scroll,
  gesture, frame presentation, low latency, display and haptic feedback, and
  terminal-side ink. The server answers queries from what the client learnt of
  its terminal, keeps the modes with terminal state, and cancels open strokes
  on a mode change, reset, prompt mark or a client that goes silent.
- Keep vector images (`f=200`) as record tables and send their edits as
  patches; keep HDR images (`f=64`), the `G` gamut key and `T=1`; read GPU
  surfaces (`t=g`, `a=r`) on the server.
- Linger a second after acknowledging a client's quit, re-sending the
  acknowledgement, so a lost datagram no longer makes the client exit with an
  unclean-shutdown error.
- Parse colon sub-parameters in SGR, and keep HDR brightness (`56`, `57`),
  RGB color-space ids and background alpha (`70`) in cells. Answer
  `kitty-query-hdr_headroom` and relay OSC 22 pointer shapes, image shapes
  included.
- Leave CSI sequences, OSC, APC and DCS payloads, and vector-group nesting
  uncapped unless `--csi-limit`, `--escape-limit` or `--vector-depth-limit`
  is set (`MOSH_CSI_LIMIT`, `MOSH_ESCAPE_LIMIT`, `MOSH_VECTOR_DEPTH_LIMIT`).
  `0` means no limit. A sequence past its bound is ignored, and a parent
  cycle is rejected either way.

## 1.4.0-goblin20260919.2 — 2026-09-19

- Name the product Goblin Skiff and install the `goblin-skiff` commands.
  Preserve upstream protocol framing, startup banners and server-facing
  environment variables for Mosh interoperability. Goblin protocol remains 1.
- Allow initial connection and proxy setup timeouts from 1 to 3600 seconds,
  with a 120-second default for slow links.
- Negotiate the optional radio profile with 128-byte encrypted datagrams,
  packet airtime pacing and adaptive retransmission waits.
- Rebuild APT, RPM and Homebrew packages with the new name. Keep Homebrew
  test sockets in its temporary directory and allow the established
  300-second observation window for lossy four-hop transfer tests. Keep
  startup-test runtime paths short in deeply nested RPM build directories.

## 1.4.0-goblin20260915.1 — 2026-09-15

First compatibility baseline: Goblin protocol 1, with separately negotiated
optional features and unchanged Mosh SST protocol 2.

- Record client/server release and build identities during setup and in
  authenticated session traffic. Show both releases on connection and reject
  incompatible protocols while allowing different compatible releases.
- Add `--socks5-proxy=HOST:PORT` for SSH setup and UDP sessions through an
  existing SOCKS5/userspace Tailscale proxy, including proxy-side DNS,
  reconnects and jump routes.
- Negotiate palette/DjVu image transport for images with up to 16 colors.
  Keep WebP fallback for older peers and lossless encoding by default.
  Add explicit `--lossy=0..100` WebP quality and independent `--djvu-lossy`
  two-color image encoding, plus DjVuLibre build/package dependencies.
- Expand image-codec, proxy, wrapper and terminal integration coverage.
- Track the product page and artwork in Git, and document release policy.

Earlier snapshots predate the compatibility baseline.
