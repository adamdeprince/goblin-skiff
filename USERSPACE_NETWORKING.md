# Userspace Tailscale and SOCKS5

Goblin Skiff can use a SOCKS5 proxy for both SSH setup and the encrypted UDP
session. This allows devices such as a reMarkable 2 to use an existing
userspace `tailscaled` without a TUN device, kernel modules, routing changes,
or an embedded second Tailscale instance.

Configure the device's existing daemon with these options (retain its
existing state and socket paths):

```sh
tailscaled --tun=userspace-networking --socks5-server=127.0.0.1:1055
```

Authenticate that daemon normally, then run:

```sh
goblin-skiff --socks5-proxy=127.0.0.1:1055 adam@naamah
goblin-skiff --socks5-proxy=127.0.0.1:1055 -J adam@jump destination
```

The updated wrapper and client are required. An ordinary reachable Skiff
server does not need a SOCKS5-specific update. Goblin features and jump
relays retain their existing peer-version requirements.

## Routing and authentication

- SSH uses SOCKS5 `CONNECT`. OpenSSH still checks host keys and handles
  passwords, keys, agents, `HostName`, users and SSH ports.
- Session packets use SOCKS5 `UDP ASSOCIATE`. Its TCP connection controls
  the association's lifetime; it does **not** carry session datagrams.
- Destination names are sent to the proxy for resolution, including
  MagicDNS names unavailable to the device's system resolver. Only the proxy
  address is resolved locally. Numeric IPv4/IPv6 destinations also work.
  SOCKS5 hostname requests cannot constrain the proxy's DNS address family;
  use a numeric destination to select IPv4/IPv6. `--family` does not restrict
  the separate client-to-proxy connection.
- With `-J`, SSH bootstrap follows the complete jump route through the
  proxy; UDP goes through the proxy to the first Goblin relay. Subsequent
  relay-to-relay traffic is unchanged. Each adjacent remote hop still needs
  UDP reachability. The first jump's `HostName` is resolved by the proxy.
- Selecting this option explicitly replaces the SSH `ProxyCommand` with
  the required SOCKS/jump transport, while preserving the remaining SSH
  configuration. Existing `ProxyJump` routes are discovered before this
  replacement. Shared SSH control masters are disabled for these launches.
- There is no direct-network fallback, and no implicit `ALL_PROXY` support.
  `--local` cannot be combined with the proxy option. The option determines
  destination discovery regardless of `--experimental-remote-ip`.
- `-D` is different: it offers a SOCKS5 **TCP** proxy inside a Skiff session;
  it cannot serve as this outer UDP proxy.

Use a loopback-only proxy listener. This client supports SOCKS5's
no-authentication method, not proxy username/password authentication. It
accepts numeric IPv4/IPv6 UDP relay addresses, including wildcard replies
that mean the proxy's TCP address. Domain-name relay *bind* replies and
SOCKS fragmentation are not supported; destination names are supported.
IPv6 proxy addresses use `[::1]:1055`; scoped link-local addresses are not
supported. Prefer a numeric loopback proxy address on embedded devices.

Verify UDP ASSOCIATE support in the exact Tailscale binary used on the
device. TCP-only SOCKS implementations are rejected with an explicit error;
successfully opening SSH alone is not sufficient.

## Reconnects, resource bounds and bandwidth

The UDP association's handshake is nonblocking with a ten-second deadline.
Connection failures back off from 250 milliseconds to 30 seconds. A proxy
disconnect closes the old UDP socket and recreates the association, without
resetting Skiff keys, packet sequence numbers, replay checks, or screen state.
There is no disconnected datagram queue: state synchronization and reliable
channels retransmit as needed, while stale real-time packets are discarded.
The initial client connection still has Skiff's normal startup timeout.

A ready idle association adds no periodic polling timer or wire heartbeat.
Skiff retains its existing idle keepalive backoff. On recovery, a fresh
authenticated ping lets the server learn the new source port. Local UDP
responses must come from the negotiated proxy socket and match the target
port/address; Skiff still authenticates every inner packet.

The proxy path budgets a conservative 1,216-byte encrypted UDP payload
before any Goblin relay envelopes. SOCKS framing is removed by the proxy,
so it is not charged as remote Skiff traffic. Tailscale's own encapsulation,
discovery, keepalives and relay traffic are outside Skiff's traffic counter.
Measure the physical link when evaluating total satellite usage or power.

## Checks and current validation scope

```sh
make
make -C src/tests check TESTS='socks5-proxy socks5-integration.test socks5-session.test udp-jump-wrapper.test'
```

The local fixture exercises encrypted bidirectional datagrams, proxy-side
names, IPv4/IPv6, fragmented TCP handshakes, invalid packets, packet loss,
association loss/recreation and explicit rejection. A real PTY/session test
also runs through SOCKS5 and a Goblin UDP relay, with loss/reordering, live
keyboard input, a stalled directory worker and persistent UI state. Wrapper
tests cover direct and jump routing. These tests do not require a Tailscale account or
kernel tunnel. They are not a substitute for testing the specific
reMarkable firmware and Tailscale build, including suspend/resume.

References: [Tailscale userspace networking](https://tailscale.com/docs/concepts/userspace-networking),
[SOCKS5, RFC 1928](https://www.rfc-editor.org/rfc/rfc1928).
