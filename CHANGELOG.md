<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff changes

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

Earlier snapshots predate the compatibility baseline. The site's published
package table still identifies the 20260911.2 artifacts until a new package
release is built and published.
