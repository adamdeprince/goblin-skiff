# SSH agents over the terminal channel

Odin Seat and Skiff expose a stable Unix socket in `SSH_AUTH_SOCK`. Each agent
packet queries the current terminal provider, then names its generation in the
request. Public-key lists and signing responses are never cached. Private keys
remain at the provider.

`odin-seat -A attach` explicitly authorizes its client's local `SSH_AUTH_SOCK`
as a fallback, including a new forwarded agent after reconnecting over SSH.
Skiff's `-A` has the same fallback permission. Native terminal providers are
preferred.

Enable `ssh_agent_forwarding yes` in Silkie's `silkie.conf`. Silkie uses its
local agent, or starts `ssh-agent` and exports its socket to new local shells.
Load keys there with `ssh-add`. The option defaults to off; enabling it grants
programs that can write terminal output access to public keys and signing.
Agent key addition/removal, smartcard loading, and locking are refused.

The canonical specification is [Silkie's SSH agent protocol](https://silkie.goblinreactor.com/ssh-agent-protocol.html)
([source](https://github.com/adamdeprince/silkie/blob/silkie/docs/ssh-agent-protocol.rst)).
It wraps a complete [RFC 9987 agent packet](https://www.rfc-editor.org/rfc/rfc9987.html),
including the four-byte length, in standard padded base64:

```
ESC ] 777 ; ssh-agent ; v=1:op=query:id=query-1 ST
ESC ] 777 ; ssh-agent ; v=1:op=cap:id=query-1:caps=1:g=providerA ST
ESC ] 777 ; ssh-agent ; v=1:op=request:id=request-1:c=connection-1:g=providerA:expires=1794787200000:data=AAAAAQs= ST
```

`id`, `c` and `g` are nonempty ASCII tokens of letters, digits, `_`, `-`, and `.`.
`id` identifies a request, `c` a persistent agent connection, and `g` a provider
generation. `expires` is an unsigned 64-bit Unix millisecond deadline; expired
requests must not begin. `op=reply` echoes `id`, `c`, and `g`, with one complete
agent response in `data`. Transport failures return SSH_AGENT_FAILURE plus an
`error` token. `op=close` names the connection and generation and has no reply.
Duplicate fields and malformed frames are rejected; unknown fields are ignored.
The `caps` listing advertises `ssh-agent.1`, separate from a provider's permission
and availability (`caps=0` or `caps=1`).

Connections retain the real backend socket, including OpenSSH session bindings.
A connection is pinned to its initial generation. Changing providers fails it
rather than silently opening an unbound socket against another agent. Reopen
the stable `SSH_AUTH_SOCK` for the new keys. A close permanently retires that
connection token in its generation. Duplicate IDs and expired operations are
refused, not replayed. An operation already delivered to the real agent cannot
be undone by closing the channel.

Socket directories are mode 0700 and sockets mode 0600. Messages and connection
storage grow with use, without a bounded pending-request table or fixed packet
buffer. SSH's actual uint32 packet length and the OS Unix-path limit still apply.
The proxies allow five seconds to negotiate and two minutes for agent work;
Skiff uses an authorized local fallback after a one-second terminal query.
Clocks must agree within the remaining request validity interval.
