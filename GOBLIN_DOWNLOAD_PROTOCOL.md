# Goblin inline downloads, version 1

Status: implemented in the development tree. Both endpoints must negotiate
`goblin-download-v2` (the on-terminal OSC format remains `v=1`). This document is the integration contract for terminal
applications such as Alpine; use the capability query before sending data.

This extension asks goblin-skiff to deliver an inline file toward the **user's
terminal**. It is not OSC 5522 clipboard data and does not replace
drag-and-drop input. Downloads use the background FEC path, below interactive
terminal traffic and socket forwarding, regardless of file size.

## Delivery and permission

The receiving client chooses one route per connection, in this order:

1. **Goblin parent:** if the enclosing terminal answers the Goblin capability
   query, forward begin/data/end to it, with locally allocated request IDs.
   This includes a client running inside another Goblin Skiff session. The
   intermediate client does not create a file or show a save prompt.
2. **Kitty parent:** otherwise, if the terminal supports the
   [Kitty file-transfer protocol](https://sw.kovidgoyal.net/kitty/file-transfer-protocol/),
   translate the transfer into OSC 5113. The terminal owns the permission prompt
   and final storage. File metadata follows session approval; payload follows
   the terminal's `STARTED` response. This is file transfer, not Kitty graphics.
3. **Local fallback:** if neither is detected, automatically open a prompt to
   save to the client's Downloads directory, showing the full filename, byte
   count and local destination. Press `y` to select Save, then Enter after the
   confirmation is displayed to approve just that file. Enter defaults to
   declining; `n` always declines. The Skiff command prefix followed by `0`
   (normally Ctrl-^ then `0`) hides or reopens the pending offer.
   The popup waits for an in-progress bracketed paste or split key sequence;
   pasted text cannot approve. No filesystem worker or staging file is created
   before approval. An unanswered local offer expires after two minutes.

Goblin discovery uses `op=query`; Kitty discovery uses a separate filename-free
session with a deliberately impossible `sha256:0` digest. Kitty's
[implementation](https://github.com/kovidgoyal/kitty/blob/master/kitty/file_transmission.py)
rejects that digest without a permission popup or file access. That rejection
detects the protocol; the probe is canceled. **Actual transfers contain no
authorization bypass.** Each probe has a five-second deadline, without blocking
the terminal event loop. Graphics flags such as `--no-kitty` do not disable
Kitty *file transfer*. `--no-downloads` disables every route, including probes.
The `pw` field is base64-encoded as Kitty's implementation requires
(`pw=c2hhMjU2OjA=`); the documentation's `safe_string` table differs here.

A refusal or failure of an actual forwarded transfer is returned to the
application. It never triggers another route or a second permission prompt.
Late capability replies cannot change a route already selected. Transfer and
probe replies are consumed locally by request ID, not sent as keystrokes to
the remote application. Queues and OSC frames are bounded.

New Goblin parents support propagating approval through nested sessions (see
`confirm=1` below), keeping payload on the originating server until the final
recipient agrees. Older parents without this option receive the stream and
manage their own buffering/permission; final completion still comes from them.

## Framing

`OSC` is the two bytes `ESC ]` (`\x1b]`). `ST` is `ESC \` (`\x1b\\`).
The spaces and newlines in the examples below are explanatory, not wire bytes.

```text
OSC 777;goblin-download;v=1:op=query:id=123 ST
OSC 777;goblin-download;v=1:op=begin:id=123:name=BASE64_FILENAME:size=DECODED_BYTE_COUNT ST
OSC 777;goblin-download;v=1:op=data:id=123;BASE64_CHUNK ST
OSC 777;goblin-download;v=1:op=end:id=123 ST
```

The metadata is colon-separated `key=value` pairs. Order is immaterial.
Payload, when present, follows the next semicolon. Use standard padded base64,
without line breaks. The filename is a UTF-8 basename, not a path. Absolute
paths, separators, NUL/control characters, `.` and `..` are invalid.

`id` is a nonzero unsigned 32-bit request identifier. Do not reuse an identifier
while its transfer is outstanding. `size` is the total number of decoded file
bytes, not the base64 length. Each data chunk contains at most **4096 decoded
bytes**. Empty files consist of begin and end, with size zero. Chunks for a file
must be sent in order. End is valid only after exactly the declared size has
arrived. To abandon a transfer:

```text
OSC 777;goblin-download;v=1:op=cancel:id=123 ST
```

The application emits plain file bytes encoded as base64; it does **not**
perform FEC or zstd compression. goblin-skiff decodes base64 before transmitting
binary file data, compressed at **zstd level 22**, over its authenticated FEC
channel. Neither the file nor its transfer progress belongs in screen state.

## Replies and completion

The query response, delivered on the application's terminal input, is:

```text
OSC 777;goblin-download;v=1:op=status:id=123:status=supported:max_chunk=4096 ST
```

An absent response means support is unknown. Do not infer support from `TERM`
or send a file without a positive response. Applications should wait
asynchronously with a bounded timeout and retain their normal save fallback.
The implementation also reports `max_size=67108864:approval=1`. Parse metadata by key
and ignore unknown keys, rather than comparing the whole reply string.
An explicit `status=unsupported` means the client disabled downloads, the
peer lacks the extension, or the session is in tmux control mode.

Append `:reply=1` to begin to request completion/error notifications:

```text
OSC 777;goblin-download;v=1:op=status:id=123:status=queued ST
OSC 777;goblin-download;v=1:op=status:id=123:status=saved:name=BASE64_FINAL_BASENAME ST
OSC 777;goblin-download;v=1:op=status:id=123:status=error:message=BASE64_UTF8_DESCRIPTION ST
```

Queued is emitted after a valid end: the complete stream was accepted for
transfer, **not approved or saved**. Only saved means the selected final
recipient has completed the file. Intermediate forwarding alone never reports
saved. Error descriptions are informational, not commands or paths. Without
reply=1, begin/data/end produce no terminal-input replies.

When a query reports `approval=1`, an application or forwarding client can add
`:reply=1:confirm=1` to begin and wait for this additional reply before sending
payload:

```text
OSC 777;goblin-download;v=1:op=status:id=123:status=approved ST
```

This is optional: existing applications may emit data immediately, and the
server buffers it within its limits while awaiting approval. Applications not
requesting `confirm=1` receive no new approval notification. Approval authorizes
only the identified transfer; it is not permission for future files.

The local fallback chooses a collision-free filename in Downloads and never
overwrites an existing file. Partial data is staged separately; it must not
appear under the final name before successful completion. These downloads
are permanent. The proposed 30-minute expiry for local-to-remote drag-and-drop
files under remote `/tmp` is not part of this extension.

For Kitty handoff, the destination is `~/Downloads/BASENAME` **on the parent
terminal's host**, not the intermediate client's configured path. The adapter
tries `.1`, `.2`, etc. when Kitty reports an existing destination before data
starts. Kitty owns filesystem behavior: its protocol has no atomic
exclusive-create guarantee, so the fallback's stronger no-overwrite and atomic
publication guarantees must not be claimed for Kitty. Goblin parents choose
their own final directory and report the final basename through the chain.

## Operational limits and safety

- Maximum file size and aggregate declared size of active downloads: 64 MiB.
- At most four active downloads per session. Each incoming filename may use
  up to 220 UTF-8 bytes; bidirectional and line-control characters are rejected.
- A name collision is resolved with `.1`, `.2`, etc. The `saved` reply reports
  the actual basename. Existing symlinks and directories also count as collisions.
- Local fallback files are created with mode 0600; Kitty metadata requests the
  same permissions. Normal cancellation and session closure of the fallback
  remove incomplete staging files. Completed downloads are left alone.
- An unfinished begin/data sequence expires after two minutes without another
  chunk. A fully queued transfer can wait for a slow/busy link without that
  timeout. Keep the Skiff session alive until `saved`; quitting it does not start
  an independent download daemon. Cancellation is best-effort if publication
  has already completed.
- Filesystem work runs in a separate process. A filesystem operation that does
  not respond within 30 seconds fails the transfer, without blocking the screen.
- The existing local destination directory defaults to `$HOME/Downloads`.
  `goblin-skiff --download-directory=DIR host` (or the local `GOBLIN_SKIFF_DOWNLOAD_DIR`
  environment variable) selects another directory. This path is never supplied
  by the remote program. These options affect only local fallback, not a parent
  terminal's filesystem. `--no-downloads` disables the feature for a connection.
- The extension does not grant blanket write permission. Local writes require
  per-file consent; a forwarding parent handles its own permission policy.
  No route launches, opens, or executes the downloaded content.
- A parent awaiting approval is given two minutes. A forwarded transfer with
  no data activity for five minutes fails and is canceled, without a local-save
  fallback. Keep nested Skiff sessions open until final completion.

Data and transfer control are separate from screen-state snapshots. The
background transport uses bounded ordered records, Reed-Solomon repair symbols
(one repair per eight source symbols, rounded up), acknowledgments and retries.
Acknowledgments/status reports can use the interactive path; file payloads
only use idle background opportunities below screen, keyboard and socket work.
Base64 is used only on the application's terminal, not for the network payload.

Run `make check` for the parser, filesystem safety, FEC loss/reordering and live
PTY regressions. `src/tests/download` uses private temporary directories, and
`src/tests/download` also exercises two nested FEC hops with final-recipient
consent. `src/tests/download-forward` covers parent negotiation and conversion.
`src/tests/download-integration.test` verifies local approval/rejection and
Goblin/Kitty handoff through an encrypted lossy UDP relay, including a stopped
local disk worker. Its normal parent terminals are protocol simulators.
An optional `--kitty-engine` mode, run under `kitty +runpy`, uses Kitty's actual
parser, permission-probe rejection, file writer and status serialization with
a private destination and an injected user decision; it does not open a GUI.
