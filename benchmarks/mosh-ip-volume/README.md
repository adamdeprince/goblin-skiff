# Mosh IP-volume benchmark

This harness compares the downlink IP volume of two Mosh installations while
they carry the same deterministic terminal activity. It is aimed at measuring
the branch against stock Mosh over a real low-bandwidth path.

## Quick start

Both Mosh variants and their corresponding servers must already be installed.
By default the harness runs `/usr/local/bin/adam-mosh` against `mosh`. It needs
Python 3 on the client and server, plus `tcpdump` on the client.

The patched wrapper invokes `adam-mosh-server`; the stock wrapper invokes
`mosh-server`. Both server commands therefore need to be on the remote `PATH`.

```sh
cd benchmarks/mosh-ip-volume
sudo -v
./run.py adam@naamah --duration 15 --hz 10 --repetitions 5
```

If route discovery is ambiguous, specify the UDP destination and capture
interface explicitly:

```sh
./run.py adam@naamah \
  --server-ip 192.0.2.10 \
  --interface en0 \
  --variant patched=/usr/local/bin/adam-mosh \
  --variant system=/opt/homebrew/bin/mosh
```

The default capture command is `sudo -n tcpdump`, so `sudo -v` obtains the
credential before the unattended run. Use `--tcpdump-command` for a different
capture wrapper, or `--no-capture` for a quick protocol smoke test. `--dry-run`
prints every Mosh command without connecting.

The capture interface is inferred from the route (`route get` on macOS and
`ip route get` on Linux); `--interface` overrides it. If a VPN or Network
Extension path is not visible to BPF, use a directly routed server address for
exact inner-IP accounting. The harness rejects a run with no active downlink
packets instead of reporting a misleading zero.

## What happens in each run

The harness uploads `workload.py` to a private cache directory on the server,
starts Mosh on a unique UDP port, and captures only UDP traffic for that server
and port. Tcpdump runs in immediate mode so short runs are not left in libpcap's
kernel buffer at shutdown. The remote workload prints a READY marker and waits.
When the local runner sees it, it timestamps and sends a one-byte start signal.
The workload then emits a fixed terminal byte stream and finishes with a DONE
marker.

Two accounting windows are recorded:

- `active_*` covers the start signal through receipt of DONE. This is the main
  comparison and includes packets needed to deliver the complete workload.
- `session_*` covers all captured UDP traffic, including Mosh startup and
  shutdown.

`*_ip_bytes` is the IPv4 total length or the IPv6 fixed header plus payload
length. It includes IP and UDP headers, retransmissions, acknowledgments, and
Mosh protocol overhead, while excluding link-layer framing and the separate
SSH startup connection. Packet counts are recorded alongside byte counts.

Every matched run must report the same update count and SHA-256 of the intended
terminal stream. A mismatch fails the run instead of comparing different
workloads. This validates the generated input, not pixel-for-pixel rendering.

## Workloads

| Workload | Terminal activity represented |
| --- | --- |
| `idle` | Connected session with no changing application output |
| `dashboard` | Small in-place status and progress-field updates |
| `scroll-repetitive` | Compressible logs scrolling one line at a time |
| `scroll-entropy` | Hard-to-compress alphanumeric lines |
| `full-redraw` | Alternating, highly compressible full-screen states |
| `kitty-place` | One small Kitty image followed by placement moves |

The first five run by default. Add `--workload kitty-place` explicitly when
testing the branch's Kitty state support. It is a feature benchmark: stock Mosh
may discard unsupported Kitty graphics, so inspect the terminal logs and do not
treat lower stock traffic as equivalent rendering.

The runner fixes the PTY at 80x24 by default, sets `TERM=xterm-256color`, turns
client prediction off, keeps matched variants in adjacent pairs, randomizes
both pair order and which variant goes first with a recorded seed, and uses a
different UDP port for every run. Increase repetitions before drawing a
conclusion; the printed table uses medians and the percentage is the median of
the paired savings.

## Results

Each invocation creates an ignored directory under `results/` containing:

- `metadata.json`: commands, versions, dimensions, seeds, and environment;
- `runs.csv` and `runs.jsonl`: one record per run;
- `*.pcap`: raw packet captures;
- `*.terminal`: captured client terminal bytes;
- `*.tcpdump.log`: capture diagnostics.

Packet captures are encrypted Mosh traffic, but terminal logs contain the
generated output and could contain connection diagnostics. Review the result
directory before sharing it.
