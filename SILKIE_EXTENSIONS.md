# Silkie's terminal extensions in Goblin Skiff

[Silkie](https://github.com/adamdeprince/silkie/tree/silkie) is a fork of the
kitty terminal. It adds a family of OSC 777 extensions plus a few additions to
SGR and to the kitty graphics protocol. Skiff passes all of them to a program
running behind `goblin-skiff-server`. The specifications are Silkie's (see
`docs/` in its repository). This page covers only what changes when Skiff
stands between the program and the terminal.

Skiff redraws the screen itself, so it acts as the terminal from the program's
point of view. The server answers every query itself and keeps each mode with
the rest of the terminal state. Meanwhile the client turns the same modes on in
the terminal it is attached to. Reports from that terminal travel to the server
on a channel of their own, never as keystrokes, and from there to the program.

## Discovery

At startup the client asks its terminal about every selector. For each mode, it
turns every bit on, reads the mode back and turns it off again; this shows
which bits the terminal's user allows. Display reports are left on, so the
client hears when the display changes.

The client then sends the server what it learned. Until that arrives, and for
any selector the terminal did not list, the server answers nothing, so programs
time out and fall back as the specifications ask.

What a terminal can do may change while it runs: a pen is paired, or its user
changes a setting. So the client also reads back every mode it sets later, and
whenever a program asks about an extension the server answers at once from
what it knows and has the client ask the terminal again. The next program to
ask gets the fresh answer.

Times the server makes up itself, such as those of the cancels it sends, are
on the terminal's clock. The client works that clock out from the terminal's
own reports and sends it with what it forwards.

The `caps` listing is the intersection of what Skiff carries and what the
terminal listed. `latency` is always listed, because Skiff's own batching is
part of what a program waits for. Media are `d,f,t,s`, plus `g` where the
server can read GPU surfaces. Formats `64` and `200` appear only when the
terminal accepts them.

## Modes and reports

| Selector | What Skiff does |
| --- | --- |
| `tablet-pointer` | Keeps only the mode bits the terminal grants. Forwards events and device descriptions for the bits that are on. Drops serial and hardware ids unless bit 8 is on. Tracks open strokes and held pad keys, so that a mode change, `RIS`, `caps` reset, a prompt mark or a client that has gone cancels them. The rest of a cancelled stroke is dropped. Labels go to the terminal when pad reports are on. |
| (in progress) | While a stroke, a key or a gesture is in progress, the client sends a heartbeat every 2 s even if nothing moves. The server takes the client to have gone after `max(8 s, 4 × round trip + 4 s)` without hearing from it. |
| `scroll`, `gesture` | Forward reports while their modes are on. Gestures in progress are cancelled when the mode changes. |
| `frame` | After drawing the program's output, the client writes a `frame` query as a marker. The terminal answers it once it has taken that output in. A frame report is forwarded only if it follows such an answer, so it is the frame that shows the output. Output that changed nothing on screen is reported at the terminal's next frame, as in Silkie. `seq` is renumbered from when the program turned the mode on. |
| `latency` | Turns on the terminal's mode, and also cuts Skiff's own batching (4 ms between packets, 1 ms collection delay) on both ends. `input` and `repaint` add Skiff's waits to the terminal's. |
| `display` | The server answers queries and sends events from the client's display. A change of display (for example, a different attached client) is reported. |
| `haptic` | Valid requests reach the terminal once, on a reliable one-shot channel. A client that receives one after it had lost contact with the server drops it rather than play it late. |
| `ink` | The brush goes to the terminal, which draws the stroke at the pen. The client works out the same records the terminal applied, from what that terminal shows and with the brush it holds. It sends them with the report, and the server applies them to its copy of the vector image; that copy is never written back to the terminal that drew it. A stroke the client cannot follow (no placement under the pen, for instance) loses its `ink` field: the program draws the stroke itself, and the terminal is told to delete its own copy. If the image is sent to the terminal whole mid-stroke, the terminal stops drawing that stroke, so the client writes the rest of it. A cancelled stroke is deleted at both ends. |
| `caps` `op=reset` and `OSC 133;A` | Turn every mode off, as Silkie does at a shell prompt. Skiff never passes prompt marks on to the terminal. |

## SGR

CSI parameters now accept colon sub-parameters (ITU-T T.416). Only SGR uses
them; any other sequence with sub-parameters is ignored, as it was before.
Cells keep:

- **Foreground and background brightness:** `56:` and `57:`, in quarter steps
  up to 1675%.
- **Color-space id:** for RGB colors, `38:2:<id>:r:g:b`.
- **Background opacity:** `70:`, in even percentages.

These are sent to a terminal that answered the `caps` listing, in an SGR
sequence of their own. Other terminals get the plain colors, with a Display P3
or Rec. 2020 color converted to the nearest sRGB color. The colon form of the
256-color and RGB SGRs, and `4:0` to turn underline off, are accepted too.

`XTGETTCAP kitty-query-hdr_headroom` is answered with the display's headroom.

## Graphics

- **Vector images (`f=200`)** are kept as a table of records. A patch
  (`a=e`) crosses the link as the records it contained. A client that is too
  far behind gets the whole scene again. Records are checked exactly as Silkie
  checks them, so `EINVAL: Line N: …` comes from the server.
- **HDR images (`f=64`)** keep their half floats, along with `T=1`. A terminal
  without HDR answers a query for `f=64` with `EINVAL`, but an image sent
  anyway is converted and shown at SDR (clipped, or compressed with `T=1`).
- **The `G` gamut key** is kept with RGB pixels, for a terminal that accepts
  it. For any other terminal the pixels are converted to the nearest sRGB
  colors.
- **PNGs with an ICC profile or a `cICP` chunk** reach the terminal as PNG,
  for it to read their colors itself. This needs both ends to support PNG
  image state (`png-state-v1`).
- **GPU surfaces (`t=g`)** are read on the server, where the program is, and
  their pixels are sent on:
  - On macOS, the surface is a global IOSurface.
  - On Linux, it is a dma-buf owned by a process in the shell's session, taken
    with `pidfd_getfd` (kernel 5.6 or later; older kernels do not list `g`). A
    linear buffer is read directly; a tiled one, or one whose layout the driver
    keeps to itself, is read through GBM, loaded at run time. If the driver's
    own GBM backend cannot map the buffer (NVIDIA's cannot), Mesa's is tried.
  - `a=r` copies the surface again and keeps its placements. If nothing
    changed, nothing is sent. Otherwise only the changed rectangle crosses the
    link, and the client writes it over the image in its terminal (`a=f`).
- **Pointer shapes (`OSC 22`)** follow kitty's rules, with separate stacks
  for the main and alternate screens:
  - a set replaces the top of the stack;
  - names the terminal does not take are ignored;
  - image names are written out in full (`image:<id>:<x>:<y>`);
  - an image entry whose image does not exist passes to the shape below it.

  The terminal is sent the shape in effect. Image ids are the same at both
  ends. Support queries are answered from the terminal's own answers.

Numbers in records, brushes and ink points are always read and written with a
decimal point, whatever the locale of the server or client.

## Not done

- **Report merging.** Skiff never merges reports, though the framing allows a
  relay to under load.
- **tmux control mode.** The extensions are off there, as the Goblin pointer
  protocol is.
- **Limits.** Nothing in a CSI, an OSC, an APC, a DCS or a vector scene has a
  fixed cap. `0` (the default) means no limit. `--csi-limit`, `--escape-limit`
  and `--vector-depth-limit`, or `MOSH_CSI_LIMIT`, `MOSH_ESCAPE_LIMIT` and
  `MOSH_VECTOR_DEPTH_LIMIT`, set an optional bound. The same names, with
  underscores, are Silkie's `csi_limit`, `escape_limit` and
  `vector_depth_limit`. A CSI or string past its bound is ignored. A vector
  nested deeper than `vector-depth-limit` is not followed. A cycle in a
  group's parents is rejected either way. Silkie also has `vector_dash_limit`,
  `vector_raster_limit` and `vector_curve_limit` for rasterizing, which Skiff
  does not do.
