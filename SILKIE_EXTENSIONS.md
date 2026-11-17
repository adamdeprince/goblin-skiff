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
client hears when the display changes. It also asks the query of the
notification protocol (OSC 99), with a return route, so the answer says
whether the terminal keeps one.

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
own reports, leaving out predicted samples, which are stamped for a time that
has not come, and sends it with what it forwards.

The `caps` listing is the intersection of what Skiff carries and what the
terminal listed. `latency` is always listed, because Skiff's own batching is
part of what a program waits for. Media are `d,f,t,s`, plus `g` where the
server can read GPU surfaces. Formats `64` and `200` appear only when the
terminal accepts them. The packed formats `301`, `302`, `304`, `308` and `516` are
always listed, because Skiff expands them for a terminal that does not take
them.

## Modes and reports

| Selector | What Skiff does |
| --- | --- |
| `tablet-pointer` | Keeps only the mode bits the terminal grants. Forwards events and device descriptions for the bits that are on. Drops serial and hardware ids unless bit 8 is on. Tracks open strokes and held pad keys, so that a mode change, `RIS`, `caps` reset, a prompt mark or a client that has gone cancels them. A cancel keeps the contact's `src`. The rest of a cancelled stroke is dropped, its corrections included, and so is the pad's own release of a key Skiff released. Nothing of a stroke Skiff did not see begin is forwarded, and a finger frame only follows a group of which something was. A program that sets the mode it already has is sent the descriptions of the tools in proximity, and of the pads, again, as a terminal whose mode changed would send them; a client that goes takes its tools and pads with it. Pad labels are terminal state: they are kept while pad reports are on, and the terminal is given them after the mode, again after every repaint. A label's text is checked as Silkie checks it, base64 of well-formed UTF-8 without control characters, and its device may be any 64-bit number. |
| (in progress) | While a stroke, a key or a gesture is in progress, the client sends a heartbeat every 2 s even if nothing moves. The server takes the client to have gone after `max(8 s, 4 × round trip + 4 s)` without hearing from it. |
| `scroll`, `gesture` | Forward reports while their modes are on. Gestures in progress are cancelled when the mode changes. An update or end of a gesture Skiff did not see begin is dropped. |
| `frame` | After drawing the program's output, the client writes a `frame` query as a marker. The terminal answers it once it has taken that output in. A frame report is forwarded only if it follows such an answer, so it is the frame that shows the output. Output that changed nothing on screen is reported at the terminal's next frame, as in Silkie. `seq` is renumbered from when the program turned the mode on. |
| `latency` | Turns on the terminal's mode, and also cuts Skiff's own batching (4 ms between packets, 1 ms collection delay) on both ends. `input` and `repaint` add Skiff's normal waits to the terminal's, whether or not the mode is on. |
| `display` | The server answers queries and sends events from the client's display, with every field the terminal gave, the channel widths (`red_bits`, `green_bits`, `blue_bits`, `gray_bits`) and `precision` among them. A change of display (for example, a different attached client) is reported. |
| `depth` | A text attribute, like the font: the server keeps the depth (`d`, in millidegrees, or `p`, in reference pixels) and the vertical phase (`y`) with each cell, whatever the terminal, and reads a frame as Silkie reads it, so one it would reject changes nothing. A terminal that lists `depth.1` is given them before the text they apply to; another is sent none, and the listing leaves the selector out. Fractions cross the link as the shortest text that reads back as the same 32-bit float. SGR 0, `op=reset`, `caps` `op=reset`, a prompt mark, `RIS` and `DECSTR` return to the plane of the terminal; colors, a font selection and a font reset leave the depth alone; cursor save and restore carry it. A placement keeps the depth and phase the text had when it was made, and the client's terminal is given them before the placement and the text's again after. The `depth`, `depth_px` and `phase` of a vector record are checked as numbers and left to the terminal, which draws the two eyes. Text and placements with a depth are drawn again when the terminal turns out to have the selector. |
| `haptic` | Valid requests reach the terminal once, on a reliable one-shot channel. They are not queued while the client is not heard from, and a client that receives one after it had lost contact with the server drops it rather than play it late. `RIS` stops continuous feedback. |
| `ink` | The brush goes to the terminal, which draws the stroke at the pen. The client works out the same records the terminal applied, from what that terminal shows and with the brush it holds. It sends them with the report, and the server applies them to its copy of the vector image; that copy is never written back to the terminal that drew it. A stroke the client cannot follow (no placement under the pen, for instance) loses its `ink` field: the program draws the stroke itself, and the terminal is told to delete its own copy. If the image is sent to the terminal whole mid-stroke, the terminal stops drawing that stroke, so the client writes the rest of it. Ink turned off during a stroke, by the mode or a reset, leaves what was drawn and adds nothing more to it at either end. A cancelled stroke is deleted at both ends. |
| `font` | The server answers `query` and `state`, keeps the role, weight and slant with each cell, and gives the terminal the selection before the text it applies to. An unknown role is the default role, as the terminal listed its roles. SGR 10 to 19 pick the default role and the first nine listed, keeping weight and slant; SGR 10, and a slot with nothing listed, is the default role whatever the terminal listed. SGR 0 and `RIS` reset it, `op=reset` leaves italic, and cursor save and restore carry it. `caps` `op=reset` leaves it alone, since it is a text attribute and not a mode. A terminal without `font.1` is sent none of this, and the listing leaves it out. |
| (consent) | When the client's terminal stops listing a selector or granting a bit (its user changed a setting, say), the mode, or the bit, reads back as off, and the contacts, keys and gestures it covered are cancelled. Ink is read back once the terminal has the brush, so a terminal that refuses ink turns it off here too. |
| `caps` `op=reset` and `OSC 133;A` | Turn every mode off, as Silkie does at a shell prompt, and forget the pad labels with the mode. Skiff never passes prompt marks on to the terminal. A reset needs no terminal; the listing is answered once the client has described its terminal, with `caps.1`, `ssh-agent.1`, and `latency.1` for a terminal that has no listing of its own. |
| `RIS` and `DECSTR` | Silkie resets its extensions on a soft reset (`CSI ! p`) as on `RIS`: every mode off with its cancels, the brush forgotten, both pointer shape stacks emptied and continuous feedback stopped. Skiff does the same. |

## Pens and mice

Everything a hand moves is smoothed and thinned on the client before it crosses
the link, and nothing else is: a down, an up, a cancel, a change of buttons, a
correction (`seq`, `pred`, `est`), a description, a frame and a stroke the
terminal is inking (`ink`) go as they came, in order. What a program receives
of a pen, an eraser, a brush, a finger or a mouse is the hand's motion within
what the hand can control and a quarter of a pixel, not the sensor's every
sample.

**Smoothing.** A sensor is never silent, and a hand moves at a few hertz where
a tablet samples at a few hundred. Positions are passed through a Kalman filter
whose model is a hand (position, velocity and acceleration per axis), from
[fast-kalman](https://fast-kalman.dev) in `src/kalman`. A report is held back
for a short lag, 15 ms by default, and the reports that arrive meanwhile refine
it with a Rauch–Tung–Striebel pass, so a bend is not rounded off as a filter
alone would round it. Hovers, corrections and predictions are not smoothed. The
ink record of a report is rendered again from the smoothed point with the
placement the client used. Only the client smooths, so nothing about it needs
to agree between machines. `GOBLIN_SKIFF_HAND_SMOOTHING` sets the lag in
milliseconds; `0` turns smoothing off.

**Thinning.** Consecutive `move` and `hover` reports of one contact are merged,
as the framing lets a relay merge them, for as long as the stroke the program
will see stays within a tolerance of every report merged away, and each
measurement within its own tolerance of the straight line between the reports
kept: a quarter of a pixel for position, 2 percent for pressure, 2 degrees for
tilt, 5 degrees for twist and the shape of a contact. The bends of the hand are
kept; its straight runs are not. The merged report has the absolute values of
the last, and the sum of `wheel` and `wclicks`. `GOBLIN_SKIFF_HAND_TOLERANCE`
sets the position tolerance in 1/256 of a pixel; `0` merges nothing but what
lies exactly on the line. The same holds for

- `scroll` reports of one source in the middle of a phase (`0`, `2` or `5`),
  with `dx` and `dy` summed and the other fields of the last;
- `update` reports of a gesture, with their scale, rotation and translation
  composed;
- xterm mouse motion (`?1002`, `?1003`) in any encoding, with the same button
  code. A mouse with a button held is dragging, and its path is kept within a
  pixel in `?1016` and within half a cell otherwise; one with no button is
  pointing, and only the pace of the hand is kept, the last position of each
  interval. Presses, releases and wheel clicks are never held, nor is anything
  inside bracketed paste;
- and Skiff's own [Goblin pointer protocol](GOBLIN_POINTER_PROTOCOL.md).

**The pace of the hand.** Skiff sends a frame every 100 to 250 ms, and a link
packs reports well: the second and later reports in a frame cost a few bytes
each where the first costs the frame. So thinning does not go as far as the
frame rate. However straight its path, a hand in motion is reported at least
every 25 ms (`GOBLIN_SKIFF_HAND_INTERVAL`, in milliseconds), give or take a
sample: as a point merged away stays within the tolerance of the path kept, a
report merged away stays within half of the interval of a report kept, the
one before it or the one it merged into. That is about forty reports a second,
twice what the hand's own bandwidth needs, where one position per frame would
be four to ten. A program that follows the pointer, or judges from the pace
and shape of a motion whether a hand made it, sees the motion and not a
position per frame. `0` keeps every report; `--radio` makes it 250 unless it
is set. The first position after a pause goes at once.
After that, what is held goes once per send interval, and waits while the
link has not taken what it was already given. Anything that may not be merged
lets what is held go first, so nothing is reordered. No press, release, change
of buttons or change of phase is merged.

`--exact-mouse` (`GOBLIN_SKIFF_EXACT_MOUSE=1`) sends every report of a mouse,
pen or finger as it came: nothing smoothed, nothing merged, nothing held. It
is for a program that needs the sensor's every sample, and it costs what the
sensor's rate costs.

What a frame carries reaches the program together. A `tablet-pointer` report
has its own `t`, so a program that reads it sees the motion's timing however
the reports were packed; an xterm mouse report and a frame of the Goblin
pointer protocol carry no time, and arrive at the program at the pace of the
link's frames.

xterm mouse reports do not say what made them. A program that wants every bend
of a pen turns on `tablet-pointer` tool reports, and then a pen is reported as
a pen, with its pressure and tilt kept within their tolerances.

**On the wire.** A report is a hundred and some bytes of text, most of it the
same as the report before. Between a client and a server that both advertise
`motion_records_supported`, each run of reports in a user diff goes as one
binary record (`src/terminal/motioncodec.h`). A shape, defined once per run,
is a selector and its keys in order, each a number or text; a track is one
contact. Each number's next value is predicted from its last ones in integer
arithmetic that comes out the same on every machine, a position from a
constant acceleration and anything else from a constant velocity, scaled by
the report's own `t`, and only what the prediction missed is written, as
zigzag varints behind a mask of which keys changed. The server writes the
text again, byte for byte, in the order the terminal wrote it. A report the
record cannot say exactly, one whose numbers are not written the plain way or
one carrying a service reply, goes in it as text. The ink the terminal drew
for a point goes as the map from the point to the stroke's coordinates, once
per stroke, and the server renders the record from the point. The shapes, the
tracks and their last samples are part of the user stream's state at both
ends, so a shape is defined once per session and the first report of a packet
is predicted from the last of the one before. An instruction that
compression would only lengthen, such as an ack or a keystroke, goes as it is
behind a zero byte to a peer that advertised `raw_frames_supported`.

`GOBLIN_SKIFF_RECORD_INPUT` names a file the client appends every frame of its
terminal to, as `<ms>\t<kind>\t<text>` with ESC written `\e`, so that the
smoothing and thinning can be tuned on a real tablet with
`tools/hand-motion/evaluate.py`, which also prints what the reports cost on
the wire as text and as records.

## Notifications

A program's desktop notifications reach the client's terminal as the program
wrote them: `OSC 9 ; message`, kitty's `OSC 99`, and `OSC 777 ; notify ; title
; body`, in UTF-8, ended by BEL or ST. A notification is neither drawn nor
kept. It is not part of the screen, a client that attaches later is not shown
it again, and each one is written to the terminal once.

**Ahead of everything else.** A notification tells a person who is looking
elsewhere that something happened, and is worth nothing behind a screen
update that a slow link takes a minute to carry. So notifications do not ride
the state the two ends synchronize. They cross in datagrams of their own on
the same authenticated connection (`src/network/noticechannel.h`), and each
end writes them before anything else it has to send: before the screen, the
forwarded sockets, the clipboard and the files, and before the rest of an
update that is already on its way. The link's budget still counts their
bytes, but a notification may borrow one packet from it, so it leaves at once
even behind a full packet of the screen; a radio's airtime lends nothing, and
there it takes the next slot. The client writes a notification to its
terminal the moment it arrives, whatever the screen is waiting for.

The channel is reliable and ordered. Every notification arrives once, whole,
and in the order the program wrote them, whatever the link loses, repeats or
reorders, so the fragments of a multipart `OSC 99` stay in order. Nothing in
it has a length limit: a long one, an icon say, crosses in as many datagrams
as the path needs, each acknowledged by the bytes it carried, so a path whose
packets grow smaller mid-way loses nothing. A client that is not heard from
for 15 s is not written to; its notifications are kept, none dropped, and go
as soon as it is heard from again.

**Answers.** What the terminal says of a notification (it was activated, a
button was chosen, it was closed, it is still shown) is taken out of the
client's input and returns to the program the same way, ahead of the
keystrokes, as the terminal wrote it. Only a whole `OSC 99` in ASCII is
handed to a program.

**The query.** `OSC 99` with `p=?` is answered by the server at once, from
the terminal's own answer to the client's query, and never reaches the
terminal as a notification. The terminal is asked again for the next program.
A terminal that gave no answer has none given for it, which is what kitty
tells a program by saying nothing. An `OSC 99` whose metadata cannot be read
is dropped, as Silkie drops it.

**Return routes.** Silkie's `r` key names the windows a notification crossed,
so that a click restores them (`docs/silkie/notification-routing.rst` in its
repository). Skiff passes the route through untouched in both directions. It
has one window and nothing to restore, so it adds no hop of its own, and the
terminal's answer names every hop beyond it, for the multiplexers inside the
session to act on. A query with a route is answered with the route and
`r=1` when the terminal keeps routes, and as kitty would answer it otherwise.

**Progress.** `OSC 9 ; 4` reports progress and notifies nobody. It goes to
the terminal once, in its turn with the screen, not ahead of it.

A client or server from before this channel is not sent its datagrams: both
ends say in their instructions whether they take them. A newer server gives
an older client its notifications with the other frames it writes to its
terminal once, and a newer client gives an older server the terminal's
answers as the input they would have been.

## SGR

CSI parameters now accept colon sub-parameters (ITU-T T.416). Only SGR uses
them; any other sequence with sub-parameters is ignored, as it was before.
Cells keep:

- **Foreground and background brightness:** `56:` and `57:`, in quarter steps
  up to 1675%.
- **Color-space id:** for RGB colors, `38:2:<id>:r:g:b`.
- **Background opacity:** `70:`, in even percentages.
- **Dim:** SGR 2, independent of brightness. SGR 22 clears bold and dim;
  kitty's SGR 221 and 222 clear them one at a time.

These are sent to a terminal that answered the `caps` listing, in an SGR
sequence of their own. Other terminals get the plain colors, with a Display P3
or Rec. 2020 color converted to the nearest sRGB color. Cells drawn before the
listing arrived are drawn again once it does. The colon form of the
256-color and RGB SGRs, and `4:0` to turn underline off, are accepted too.

Error replies to graphics commands are written as kitty writes them,
`CODE:message`, with no space.

`XTGETTCAP kitty-query-hdr_headroom` is answered with the display's headroom,
`1` before anything is known. Other names get `DCS 0+r`.

## Graphics

- **Vector images (`f=200`)** are kept as a table of records. A patch
  (`a=e`) crosses the link as the records it contained. A client that is too
  far behind gets the whole scene again. Records are checked exactly as Silkie
  checks them, so `EINVAL:Line N: …` comes from the server, in Silkie's words.
  That includes the `box` clip of a group, the `srgb:`, `p3:`, `rec2020:` and
  `linear:` colors, and a color that overflows Silkie's single-precision
  floats. `x g=0` deletes everything below the root. `T=1` is kept. A text
  record (`t`) is read as Silkie reads it: strict base64, Python's JSON, and
  every check of its layout that needs no font file, including that an `@id`
  names a session font and that its axes are in that font's range. Only what
  the system's fonts can tell is left to the terminal: a layout that asks for a
  font or an axis the terminal does not have is not an error here, and the
  terminal drops that patch, whose command was quiet by then. A program that
  wants to know measures it first.
- **The vector service (`a=v,f=200`)** is carried to the terminal, which has the
  fonts, and its answer comes back under the program's `i`, with `q` as usual.
  This covers `fonts`, `inspect`, `measure`, `upload` and `release`. A font
  reaches the terminal before any frame that follows it, so a program may use it
  at once. A terminal without the service gets nothing and the program gets
  `EINVAL`, as does a request made before the client has described its
  terminal, or still unanswered when the client goes away.
- **Uploaded fonts are terminal state.** The server keeps each upload, takes
  it back if the terminal refuses it, and answers `release` itself. A released
  font is kept while any text record names it. A new client, a repaint, or a
  scene sent whole gives the terminal the fonts its text names first (quietly,
  `q=2`) and releases again after it those the program had released.
- **Viewport placements (the `F` key)** are kept with placements and sent to a
  terminal that lists `viewport.1`, and only there: another would drop the whole
  command. For such a terminal a placement with `F` gets `EINVAL`, as do unknown
  bits, and `F` with `U=1`. A placement placed in pixels is left alone by
  scrolling and resizing. Deleting by position finds it in the cells it is
  visible in, and ink starts only on its visible part. Removing a placement
  removes the ones below it, with or without `F`.
- **Packed images (`f=301`, `302`, `304`, `308` and `516`)**, grayscale of one, two, four or eight
  bits a pixel and RGB565, are accepted whatever the terminal, with `o=z` or
  without, and as the data of a frame (`a=f`). A program packed its pixels to
  spend few bytes, so they stay packed and exact: image state holds the bytes
  as they were sent, behind a row filter (PNG's, or the exclusive or with the
  row above for sub-byte pixels; RGB565 as a byte a channel) and zstd, or zlib
  for a peer without it. Several filterings are tried and the smallest kept,
  with less effort for a larger image, so that four megapixels of eight-bit
  gray take about a tenth of a second to pack.
  `--lossy` never applies to them, nor does `--djvu-lossy`. The client writes
  a terminal that lists the format the same packed bytes, and any other the
  same pixels as RGB (`f=24`). `G` is kept for RGB565 and ignored for
  grayscale, as in Silkie. A client that does not advertise `packed-state-v2`
  is sent the pixels expanded, in a lossless codec.
- **Indexed, palette and precision images** keep their original payload with
  `native-image-v1`. Kitty's `f=8` (index) and `f=16` (index and alpha) refer
  to the terminal's live colors and are offered when that terminal lists them.
  Silkie's `401/402/404/408` carry an RGBA palette and packed indices; `a=e`
  replaces a range of palette slots atomically. `316` is little-endian 16-bit
  gray, `632` RGB10A2 and `664` 16-bit RGBA. They retain every source bit,
  their gamut, transfer function (`E=8/13/16/18`) and tone-mapping setting.
  A supporting terminal gets those bytes; another gets linear HDR half floats
  or SDR RGBA at rendering time. Indexed colors require the terminal's palette.
  These images refuse animation and composition as Silkie does. Older Skiff
  clients are told `ENOTSUP` for formats requiring this new state encoding.
  The old grayscale numbers `1/2/4` are refused; `8/16` have only Kitty's
  indexed meaning. `516` replaces the briefly used RGB565 number `316`.
- **Graphics feature queries (`a=q,e=...`)** report the relay's supported
  formats and keys without uploading pixels or disturbing a chunked upload.
  Image file size `S` and offset `O` are unsigned 64-bit values; ranges that
  overflow a file offset fail with `EOVERFLOW` before opening the file.
- **HDR images (`f=64`)** are kept as sent, half floats and `T=1`, whatever
  the terminal. A terminal without HDR answers a query for `f=64` with
  `EINVAL`. The client converts an image sent anyway as it draws it (clipped,
  or compressed with `T=1`), and sends it again whole if the terminal later
  lists `64`.
- **The `G` gamut key** is kept with RGB and surface pixels as sent. The client
  asks its terminal with a one-pixel `G=1` query, bounded by DA1, and converts
  to the nearest sRGB colors as it draws for a terminal that refuses it.
  Refreshed rectangles of a surface carry `G` too.
- **PNGs with an ICC profile or a `cICP` chunk** reach the terminal as PNG,
  for it to read their colors itself, where both ends support PNG image state
  (`png-state-v1`). Otherwise a PNG is read as Silkie reads it: `cICP` Display
  P3 or Rec. 2020 with the sRGB curve, or an ICC profile with those primaries,
  keeps its gamut, with `G`; other profiles are converted to sRGB. Profiles
  made of lookup tables are read as sRGB.
- **Frames (`a=f`, `a=c`, `a=a`).** An image has only its root frame. `a=f`
  with `r=1` edits it and `a=c` composes within it, as kitty does, in linear
  light for `f=64`. A new frame gets `EINVAL`, `a=a` only answers `ENOENT`,
  and surface images refuse both. The chunks of an `a=f` are one upload.
- **Relative placements (`P`, `Q`)** are refused as kitty refuses them
  (`ENOPARENT`, `ECYCLE`, `ETOODEEP`); `a=T` answers for its transmission, as
  kitty does, and makes no such placement. A relative placement sits at its
  root plus `H` and `V`, or at a virtual parent's placeholders, moves with its
  root, and is found by position in every cell it covers. A virtual parent is
  matched by image id only.
- **GPU surfaces (`t=g`)** are read on the server, where the program is, and
  their pixels are sent on. The surface is taken at the first `t=g` and held
  (its descriptors, or a reference to the IOSurface) until the image is
  deleted, so the program may close its own at once:
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

- **Animation frames** beyond the root frame, and DECRQSS, which Skiff does not
  answer at all.
- **Records for other pointers.** Skiff's own pointer protocol and xterm mouse
  motion are smoothed and thinned, but still cross the link as the bytes they
  are, among the keystrokes.
- **tmux control mode.** The extensions are off there, as the Goblin pointer
  protocol is.
- **Progress as state.** `OSC 9 ; 4` is passed on once. A client that attaches
  later does not show the progress a program reported before.
- **Stereo presentation.** Depth is kept and relayed; drawing the two eyes'
  views is the terminal's.
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

## SSH agents across reconnects

The server exports a stable `SSH_AUTH_SOCK`. Each agent packet queries the
current terminal provider and carries its generation, rather than reusing an
SSH login's old socket path. Silkie's `ssh_agent_forwarding yes` enables that
provider; otherwise `-A` explicitly authorizes the client's local agent as a
fallback. Switching clients changes available keys on the next connection.
Existing connections fail across provider changes so session bindings cannot
silently disappear.

Agent frames use the reliable one-shot extension channel, outside snapshots,
input prediction, motion coalescing, and diagnostic input recordings. Replies
return through nested multiplexers. Requests preserve backend agent connection
identity, are not retried against another provider, and expire before they can
begin late. [Protocol, permissions and lifetime](docs/SSH_AGENT_PROTOCOL.md).

Program Status Protocol reports (`OSC 7501`, revision 0.4) use the reliable,
ordered notice channel, ahead of screen updates, images and file transfers.
Reports are one-shot events and are never replayed by a screen repaint. Their
fields are interpreted by the terminal. Reports and fields have no imposed
size limit; a report is queued only after its BEL or ST terminator is complete.
Status discovery replies return on the priority channel too. A remote shell
prompt removes transient statuses using `process-exited`, and a hard reset
clears all statuses; both precede subsequent reports on the same ordered channel.
