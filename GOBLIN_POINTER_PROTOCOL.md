# Goblin pointer protocol, version 1

This is an open, experimental terminal extension shared by Inkline, Goblin
Skiff and GoblinView. It is separate from USB HID and the Kitty graphics
protocol. Applications choose whether they want enriched pen/touch input.
An ordinary shell keeps local selection and tablet navigation. Existing
applications requesting xterm mouse reporting receive translated mouse events.

Frames use seven-bit OSC and ST (`ESC ]` and `ESC \\`). BEL is also accepted as
an OSC terminator. Each complete frame is at most 768 bytes. Fields are ASCII,
colon separated, case sensitive, and unordered. Integers are decimal, without
a leading `+`. Duplicate/unknown fields, unsupported versions, incomplete
reports and out-of-range values are rejected. A parser must not turn rejected
pointer reports into keystrokes. OSC cancellation follows ECMA-48.

```
ESC ] 777;goblin-pointer;v=1:op=query:id=42 ST
ESC ] 777;goblin-pointer;v=1:op=cap:id=42:caps=7:w=1200:h=800:cw=12:ch=24 ST
ESC ] 777;goblin-pointer;v=1:op=mode:mode=1 ST
ESC ] 777;goblin-pointer;v=1:op=mode:mode=0 ST
```

`query` does not change mode. `id` is an application-chosen nonnegative signed
32-bit integer, echoed in the reply. Capability bits are pen=1, touch=2,
two-contact gesture=4. They describe supported report types, not attached
hardware. `w,h` are the terminal content size in pixels (1–65535), `cw,ch`
the character cell size (1–4096). Query again after resize. An unsupported
terminal ignores the request; applications must time out and use standard
mouse input. There is no UI setting that overrides the application's choice.

Mode 1 gives pen and touch precedence over legacy mouse reporting. A physical
mouse continues to use the application's xterm mouse mode. Mode 0 restores
the normal path. Mode is local to each terminal/window and resets on RIS.
Applications must disable it on exit, suspend and error, and re-enable after
resume. Multiplexers retain it with terminal state, restore it on reconnect,
and disable it when detaching. A mode change cancels active contacts.

An input report looks like this (one physical line):

```
ESC ] 777;goblin-pointer;v=1:op=event:kind=1:phase=1:device=2:contact=0:x=25600:y=51200:buttons=1:mods=0:tool=1:valid=3:p=32000:tx=-1200:ty=600 ST
```

| Field | Meaning |
| --- | --- |
| `kind` | 1 pen, 2 finger, 3 gesture |
| `phase` | 0 hover, 1 down/begin, 2 move/update, 3 up/end, 4 cancel |
| `device,contact` | Nonnegative 31-bit IDs, stable for the contact lifetime; pen contact is normally 0 |
| `x,y` | Signed **zero-based 1/256 pixel** position relative to the terminal content origin; range ±1073741823 |
| `buttons` | Held buttons: tip/left=1, barrel/right=2, middle=4, back=8, forward=16 |
| `mods` | Shift=1, Alt=2, Control=4, Super=8 |
| `tool` | 0 finger/unknown, 1 pen tip, 2 eraser end |
| `valid` | Available measurements: pressure=1, tilt=2, barrel rotation=4, tangential pressure=8, Z=16 |
| `p` | Pressure 0–65535, required when pressure is valid |
| `tx,ty` | Tilt in hundredths of a degree, −9000…9000; positive toward content right/bottom |
| `twist` | Barrel rotation in hundredths of a degree, 0…35999 clockwise |
| `tangent` | Tangential pressure −65535…65535 |
| `z` | Device-reported Z value −1000000…1000000 (device units) |

Every event requires `kind,phase,device,contact,x,y`. Other non-measurement
fields default to the values in the reference codec. A missing validity bit
means **unavailable**, not a zero measurement. In particular reMarkable 2
does not measure pen barrel rotation. A terminal must not manufacture it.
Eraser is a tool end, not a pressure or button convention.

Gesture events require `peer` (second contact ID), `dx,dy` (total translation
from the beginning, in 1/256 pixel), `scale` (10000 = original distance,
range 1–1000000), and `angle` (total clockwise rotation in hundredths of a
degree, −36000…36000). `contact` identifies the first contact. `x,y` is the
centroid. Raw finger events continue during gestures; apps choose one stream
for an operation. A gesture ends when either member is released or cancelled.

Capture starts at down, and lasts through up/cancel even outside the original
window. Coordinates may be negative during capture; do not clamp them into a
different window. A multiplexer translates positions by the viewport origin,
window content origin (including borders), and content scroll offset. It
preserves pressure, tool, tilt and gesture deltas. A gesture is delivered only
when both contacts belong to the same window. Contacts cancel on resize,
terminal focus/session changes or device removal. Cancellation has buttons=0 and p=0.
Consumers must tolerate lost hover, repeated cancellation and a fresh down
after disconnect. A new down replaces an unfinished contact of the same ID.

The MIT reference codec is `protocol/goblin_pointer.hpp` in Inkline,
`src/terminal/goblin_pointer.hpp` in Skiff and `src/goblin_pointer.hpp` in View.
It is usable without Qt. The three copies must remain byte-identical.

Related standards: [xterm mouse reporting](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html),
[Kitty graphics](https://sw.kovidgoyal.net/kitty/graphics-protocol/).
