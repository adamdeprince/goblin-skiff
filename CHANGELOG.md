<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff changes

## 1.4.0-goblin20260919.1 — 2026-09-19

- Name the product Goblin Skiff and install the `goblin-skiff` commands.
  Preserve upstream protocol framing, startup banners and server-facing
  environment variables for Mosh interoperability. Goblin protocol remains 1.
- Allow initial connection and proxy setup timeouts from 1 to 3600 seconds,
  with a 120-second default for slow links.
- Negotiate the optional radio profile with 128-byte encrypted datagrams,
  packet airtime pacing and adaptive retransmission waits.
- Rebuild APT, RPM and Homebrew packages with the new name. Keep Homebrew
  test sockets in its temporary directory and allow the established
  300-second observation window for lossy four-hop transfer tests.

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
