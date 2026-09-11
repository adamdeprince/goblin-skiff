# UDP jump hosts

```sh
goblin-mosh -J user@jump destination
goblin-mosh -J jump-a,jump-b destination
goblin-mosh -J user@jump:2222 --jump-port=60001:60999 destination
```

`-J` / `--jump` now routes **the Mosh UDP session**, not just its SSH
bootstrap. The wrapper also discovers OpenSSH `ProxyJump` settings, including
`-J` inside `--ssh` and nested first-hop aliases. At most four relay hops are
supported. With no jump configured, the direct UDP path is unchanged.

The updated client, destination server, and a `goblin-mosh-server` on **every
jump** are required. `--jump-server=COMMAND` selects the server command on the
jumps independently of `--server=COMMAND` on the destination. All helpers run
as the SSH login user; no root, system service, or permanent relay daemon is
needed. The login must permit running the helper, not only SSH TCP forwarding.

Like OpenSSH's `-J`, jump credentials, usernames, and SSH ports normally come
from their own `Host` entries. `user@host:port` can override user/port; IPv6
addresses use `[address]`. The wrapper carries `--ssh`'s `-F` configuration
file to hop connections, but not destination-specific command-line options
such as `-i`, `-l`, or `-p`. Put jump-specific options in SSH configuration.
Custom `--ssh` commands must support OpenSSH's `-G` configuration output.
Arbitrary `ProxyCommand` programs cannot be translated into UDP relay routes;
specify the hosts using `-J` or `ProxyJump` instead.
Jump entries currently use the `[user@]host[:port]` grammar above; SSH URI
syntax and percent-token substitutions in jump entries are rejected.

## Reachability and lifetime

The UDP path is client → first jump → subsequent jumps → destination, and
back. The client no longer needs a UDP route to the destination. Each hop must
still have UDP reachability to the next one. This does not tunnel UDP over
SSH/TCP or make a UDP-blocked first hop reachable.

By default each relay binds one UDP port in **60001–60999** on the interface
used for its incoming SSH session. Permit that range from the preceding hop
(from the client for the first jump), and permit the destination's Mosh port
from the last jump. `--jump-port=PORT[:PORT2]` changes the relay range on all
jumps; the existing `-p` still selects the destination's UDP port. A value of
zero requests an OS-assigned port and is mainly useful for testing.

For the first jump the wrapper records the address actually used by SSH,
including `HostName` resolution, so an SSH-facing private address behind NAT
is not handed to the client as its destination. Behind server-side NAT, the
selected UDP port must be forwarded with the same external port number.
Client-side NAT/address/port changes are handled automatically after a new
authenticated packet. Jump and destination addresses themselves remain fixed
for the session. No SSH connection remains in the data path.

There is no new periodic relay heartbeat. Normal Mosh packets, including the
15-second-to-15-minute idle keepalive backoff, traverse the relay unchanged
inside its envelope. A relay expires after **24 hours without a newer
authenticated client packet**. `--jump-idle-timeout=SECONDS` can select
1800–604800 seconds. Reconnecting before that deadline keeps the same route;
after expiry, start a new session. Destination Mosh timeouts may be shorter.

On normal client exit, authenticated close packets are sent deepest-hop first,
three attempts per hop. Close is best-effort, not an acknowledged shutdown
protocol; a lost close or crashed client is cleaned up by the lease. Relays
that never receive a valid client packet expire after ten minutes. The
destination's initial connection grace is also ten minutes for negotiated
relay clients, to allow time for multiple SSH authentications. Failed setup
can leave these temporary helpers until their startup grace expires.

## Encryption and packet handling

SSH authorizes each relay's **single fixed numeric destination and UDP port**.
The helper binds an ingress UDP socket and connects a separate egress UDP
socket to that destination. Clients cannot select destinations in packets;
the kernel filters egress replies by the connected peer.

Each relay generates an independent random 128-bit key. The client wraps
the already-encrypted Mosh datagram in one authenticated envelope per hop.
Each hop strips only its own envelope on the forward path and adds it on
replies. No jump is given the end-to-end Mosh key. A jump can observe traffic
sizes/timing or disrupt connectivity, but cannot read or forge the inner
terminal, clipboard, forwarding, or transfer messages.

Normal envelopes use the existing AES-128-OCB implementation. `--fips-crypto`
requires the configured OpenSSL FIPS provider on the client, destination,
**and every jump**, using AES-128-GCM for all layers. A missing provider or a
wrong/unsupported suite fails setup; no fallback to OCB. This is not a claim
of whole-system FIPS compliance, and SSH configuration remains a separate
deployment responsibility.

Each hop accepts reordered packets within a 1024-packet replay window, once
only. Only a sequence newer than any previously authenticated packet can
change the client address or renew the lease. Old/replayed packets cannot
roll a roaming client back to an earlier network. Invalid, reflected,
oversized, and duplicate packets are silently discarded without a response.

The relay has no unbounded application send queue. Both sockets are
nonblocking, reads are limited to 64 datagrams per direction per turn, and a
full kernel send queue drops a datagram. End-to-end Mosh recovery and the
existing independent directional pacing controllers remain responsible for
loss and bandwidth. Relays do not interpret terminal state, retransmit data,
or alter the FEC scheme; no new FEC dependency is added.

## Wire format, version 1

SSH bootstrap, on the destination:

```
MOSH_RELAY_HOPS=N MOSH_CLIENT_CAPS=...,udp-relay-v1 goblin-mosh-server new ...
MOSH RELAY-MTU 1 N
MOSH CONNECT PORT END_TO_END_KEY
```

On each jump, working from the destination back toward the client:

```
goblin-mosh-server relay [--fips-crypto] [--port=RANGE] -- TARGET_IP TARGET_PORT
MOSH RELAY 1 SUITE BIND_IP PORT RELAY_KEY
```

`SUITE` is `ocb-aes128` or `aes128-gcm-v1`. Keys are 22-character unpadded
base64 encodings of 16 random bytes. Setup version and suite are checked
before launching the client. The local binary is preflighted with
`--udp-relay -c`; the destination must acknowledge the exact hop count so an
older binary cannot accidentally send oversized packets.

The wrapper supplies `--udp-relay` and `MOSH_RELAY_KEYS=KEY1,KEY2,...` to the
client, ordered nearest to farthest. It keeps `MOSH_KEY` as the destination's
end-to-end key. Keys are never command-line arguments. The client consumes
and unsets these environment variables. Duplicate hop keys are rejected.

Each envelope uses the existing `Crypto::Session` wire encoding:

| Mode | Fields | Extra bytes per hop |
| --- | --- | ---: |
| OCB | 8-byte direction/sequence, ciphertext, 16-byte tag | 24 |
| FIPS GCM | 12-byte random IV, 8-byte direction/sequence, ciphertext, 16-byte tag | 36 |

The sequence is a network-order 64-bit integer: the top bit is 0 toward the
destination, 1 toward the client; the remaining bits count from 1 independently
in each direction. Sequence numbers never wrap or reset within a relay key's
lifetime. FIPS mode also uses the existing directional key derivation and
IV/invocation limits. Empty authenticated plaintext means **close this hop**;
ordinary data plaintext is the complete inner datagram. A Mosh keepalive is
not empty at this layer: it still has inner encryption and timestamps.

For a two-hop path the forward wire is `R1(R2(Mosh))`, and replies have the
same nesting. To close hop 2 send `R1(R2(empty))`; then close hop 1 with
`R1(empty)`. There are no magic bytes or per-packet destination fields.

Both Mosh peers reserve all hop overhead against a conservative **1216-byte
outer UDP payload** budget, including mixed IPv4/IPv6 routes. The Mosh inner
datagram budget is `1216 - N*24`, or `1216 - N*36` in FIPS mode, before its own
encryption/timestamp overhead. Directional bandwidth accounting includes the
outer envelopes and a conservative IPv6 header allowance.

This is **Mosh-session UDP relaying**, not arbitrary UDP port forwarding.
`-L/-R` remain TCP forwards and `-D` remains SOCKS5 CONNECT, not UDP ASSOCIATE.
