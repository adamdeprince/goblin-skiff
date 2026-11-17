<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Goblin Skiff changes

## 1.4.0-goblin20261117.1 — 2026-11-17

- Deliver Program Status Protocol (OSC 7501) reports ahead of bulk traffic.
- Adopt Silkie's current packed format numbers and Kitty indexed colors.
- Preserve palette images, palette edits, high precision images and transfer
  functions through negotiated image state, with terminal rendering fallbacks.
- Handle graphics feature queries and unsigned 64-bit image file ranges.

- Relay desktop notifications, ahead of everything else. `OSC 9`, kitty's
  `OSC 99` and `OSC 777 ; notify` reach the client's terminal as the program
  wrote them, Silkie's return route (`r`) included, and what the terminal
  answers (activated, closed, still shown) returns to the program. They cross
  in datagrams of their own, reliable and ordered, which each end writes
  before the screen, the streams, the files and the rest of an update already
  on its way, and which may borrow one packet from the link's budget; the
  client writes one to its terminal the moment it arrives. In the end-to-end
  test, where the link budget starts at a few hundred bytes a second, a
  notification written after a 43 KB image reaches the terminal some 20 s
  before the image does. The server
  answers the `OSC 99` query from the terminal's own answer. `OSC 9 ; 4`
  progress is passed on in its turn. Nothing in the channel has a length
  limit, and nothing is dropped for a client that is away. Both ends
  advertise it (`notices_supported`); older peers get the frames the old way.
- Accept Silkie's packed image formats, `f=1`, `2`, `4` and `8` (grayscale)
  and `f=16` (RGB565), and keep them packed and exact: image state holds the
  bytes as sent, row filtered and compressed with zstd or zlib, never WebP
  and never `--lossy`. A terminal that lists the format is written the packed
  bytes, any other the same pixels as RGB. The listing has the formats
  whatever the terminal. Clients advertise `packed-state-v1` and
  `packed-zstd-v1`; older ones receive the pixels expanded and lossless.
- Relay Silkie's depth protocol (`depth.1`): the depth and vertical phase of
  text are kept with each cell, placements keep the depth they were made at,
  and the `depth`, `depth_px` and `phase` of vector records are checked as
  Silkie checks them. A terminal that lists the selector is given all of it.
- Pass on the channel widths and `precision` Silkie's display reports now carry.
- Smooth and thin everything a hand moves before it crosses the link: pens,
  fingers and mice of `tablet-pointer`, scrolls, gestures, xterm mouse motion
  and the Goblin pointer protocol. The client takes the sensor's noise out of
  positions with a fixed-lag Kalman smoother (fast-kalman, now in `src/kalman`,
  15 ms of lag) and merges consecutive reports of a contact, as Silkie's
  framing allows, for as long as the stroke stays within a quarter of a pixel
  of every report merged away and each measurement within its tolerance. Downs,
  ups, cancels, button changes, corrections, predictions and inked strokes go
  as they came, in order. `GOBLIN_SKIFF_HAND_SMOOTHING` and
  `GOBLIN_SKIFF_HAND_TOLERANCE` tune it; `GOBLIN_SKIFF_RECORD_INPUT` records
  the terminal's frames for `tools/hand-motion/evaluate.py`.
- Keep the pace of the hand. A moving hand is reported at least every 25 ms
  however straight its path (`GOBLIN_SKIFF_HAND_INTERVAL`, in milliseconds;
  `0` keeps every report, `--radio` makes it 250), where before a mouse or a
  hovering pen was reported once per frame of the link, 4 to 10 times a
  second. xterm mouse motion with a button held is a drag and keeps its bends,
  within a pixel in `?1016` and half a cell otherwise; without a button it is
  pointing and keeps only the pace. `--exact-mouse` (`GOBLIN_SKIFF_EXACT_MOUSE=1`)
  sends every report of a mouse, pen or finger as it came, nothing smoothed,
  merged or held.
- Send the reports of a hand as binary records. A run of extension reports in
  a user diff goes as one record: each key's next value is predicted from its
  last ones in integer arithmetic (positions from a constant acceleration, the
  rest from a constant velocity, scaled by the report's own time), and only
  what the prediction missed is written, as zigzag varints behind a mask. The
  server writes Silkie's text again byte for byte. The ink the terminal drew
  for a point travels as the map it was rendered with, once per stroke, and is
  rendered again at the server. Both ends advertise it
  (`motion_records_supported`); older peers get the text.
- Send an instruction that compression would only lengthen as it is, behind a
  zero byte, to a peer that takes one (`raw_frames_supported`). An ack or a
  keystroke no longer pays for a zstd frame.
- Build as C++23. gcc 11, clang 14 and later take it; the packaging recipes
  no longer set a standard of their own.
- Relay what Silkie has added since: the font protocol (`font.1`, SGR 10 to 19)
  with the selection kept in cells; vector text records, `box` clips, wide gamut
  and linear paints and `T=1`; the vector service (`a=v`), answered by the
  terminal; viewport placements (`F`). OSC 777 fields, pad labels and ink brushes
  no longer have caps of their own.
- Keep more of Silkie's extensions as terminal state, so that a repaint or a
  client that attaches later has them: pad labels; the fonts uploaded to the
  vector service, released ones while text still names them; and HDR and wide
  gamut images as sent, converted by the client as it draws them and sent again
  when its terminal turns out to take them.
- Follow Silkie's specifications more closely. Tablet pointer: cancels keep
  `src`, nothing of a stroke or gesture Skiff did not see begin is forwarded,
  finger frames only follow what was reported, the pad's own release of a key
  Skiff released is dropped, tools are described again when a program sets the
  mode it has, and a merged report keeps `valid` right. Modes and bits a
  terminal stops granting, ink included, read back as off and cancel what they
  covered. `latency` reports Skiff's normal waits, `RIS` stops continuous
  haptics, haptics are not queued for a client that is away, and SGR 10 is the
  default font role everywhere. Vector images: Silkie's color overflow, text
  checks and error words, `x g=0`, and an answer to every service request.
  Graphics: `a=f` and `a=c` on the root frame, kitty's checks of relative
  placements, which now move with their root, GPU surfaces held until their
  image is deleted, `G` asked of the terminal and kept on refreshed rectangles,
  PNG color profiles read without `png-state-v1`, unknown XTGETTCAP names
  answered, and SGR 2 kept. Error replies are written `CODE:message`, as kitty
  writes them.
- Bring the relay in line with Silkie where it differed: a soft reset (`DECSTR`)
  resets the extensions as `RIS` does; `caps` `op=reset` forgets the pad labels
  and needs no terminal, and the listing is answered for a terminal without one;
  pads are described again, like tools, to a program that sets the mode it has;
  a label's text is checked as UTF-8 and its device may be any 64-bit number;
  ink turned off during a stroke adds nothing more to it; a predicted sample
  does not set the terminal's clock; and SGR 221 and 222 clear bold and dim
  apart.
- Lower the zstd level of a state update only when the CPU cannot keep up with
  the wire. On a slow link, or until the link has been measured, it stays at 22.
- Resend an update of several fragments as the same bytes under the same id, so
  that each pass fills the gaps of the last. On a lossy link a large backlog,
  such as a long pen stroke, could otherwise never be delivered whole.

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
