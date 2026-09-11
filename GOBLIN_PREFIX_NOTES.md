# Reverting the goblin- Command Prefix

This branch intentionally installs and invokes its binaries with a
`goblin-` prefix so it can coexist with stock Mosh on the same machine. To turn it
back into normal `foo` command names, undo these rename surfaces.

## Build Targets

- `scripts/Makefile.am`
  - `bin_SCRIPTS = goblin-mosh` -> `mosh`
  - target `goblin-mosh:` -> `mosh:`
  - generated output file `goblin-mosh` -> `mosh`
- `src/frontend/Makefile.am`
  - `goblin-mosh-client` -> `mosh-client`
  - `goblin-mosh-server` -> `mosh-server`
  - automake variable prefixes `goblin_mosh_client_*` -> `mosh_client_*`
  - automake variable prefixes `goblin_mosh_server_*` -> `mosh_server_*`
- `src/moshcp/Makefile.am`
  - `goblin-moshcp` -> `moshcp`
  - `goblin-mosh-compile-dictionary` -> `mosh-compile-dictionary`
  - automake variable prefixes `goblin_moshcp_*` -> `moshcp_*`
  - automake variable prefixes `goblin_mosh_compile_dictionary_*` -> `mosh_compile_dictionary_*`

After editing Makefile.am files, rerun `./autogen.sh` or let `make` refresh
the generated Makefiles if your tree supports that.

## Wrapper Defaults

- `scripts/mosh.pl`
  - default client command `goblin-mosh-client` -> `mosh-client`
  - default server command `goblin-mosh-server` -> `mosh-server`
  - user-facing help text for `goblin-mosh` -> `mosh`
  - temporary zstd dictionary pattern `goblin-mosh-zstd-dict.XXXXXX` -> `mosh-zstd-dict.XXXXXX`

## moshcp Socket Namespace

- `src/network/bulkcontrol.cc`
  - fallback runtime dir `/tmp/goblin-moshcp-$UID` -> `/tmp/moshcp-$UID`
  - socket name `goblin-moshcp-$role-$pid.sock` -> `moshcp-$role-$pid.sock`
  - latest symlink `goblin-moshcp.latest` -> `moshcp.latest`
  - env var `GOBLIN_MOSHCP_SOCK` -> `MOSHCP_SOCK`
- `src/frontend/stmclient.cc` and `src/frontend/mosh-server.cc`
  - exported env var `GOBLIN_MOSHCP_SOCK` -> `MOSHCP_SOCK`
- `src/moshcp/moshcp.cc`
  - command/status prefix `goblin-moshcp:` -> `moshcp:`
  - default output names `goblin-moshcp.out` and `goblin-moshcp-archive` -> `moshcp.out` and `moshcp-archive`
  - temp-file suffix `.goblin-moshcp.tmp.` -> `.moshcp.tmp.`

## Docs and Integration Files

- Rename man pages back:
  - `man/goblin-mosh.1` -> `man/mosh.1`
  - `man/goblin-mosh-client.1` -> `man/mosh-client.1`
  - `man/goblin-mosh-server.1` -> `man/mosh-server.1`
  - `man/goblin-moshcp.1` -> `man/moshcp.1`
  - `man/goblin-mosh-compile-dictionary.1` -> `man/mosh-compile-dictionary.1`
- `man/Makefile.am`
  - swap `goblin-*` man-page names back to unprefixed names.
- `conf/bash-completion/completions/goblin-mosh`
  - rename back to `conf/bash-completion/completions/mosh`
  - change the `complete ... goblin-mosh` line back to `complete ... mosh`
- `conf/ufw/applications.d/goblin-mosh`
  - rename back to `conf/ufw/applications.d/mosh`
  - profile heading `[goblin-mosh]` -> `[mosh]`
- `conf/Makefile.am`
  - swap the completion and UFW file paths back.
- `README.md`
  - replace command examples and prose references from `goblin-*` back to stock names.

## Runtime Strings

These are not strictly required for coexistence, but they should be reverted for
a clean upstream-style build:

- `src/frontend/mosh-client.cc`
  - version banner `goblin-mosh-client` -> `mosh-client`
  - exit banner `goblin-mosh` -> `mosh`
- `src/frontend/mosh-server.cc`
  - version/detach/exit banners `goblin-mosh-server` -> `mosh-server`
  - utmp host tags `goblin-mosh [...]` -> `mosh [...]`
  - unattached-session scanner prefix `goblin-mosh ` -> `mosh `
- `src/frontend/stmclient.cc`
  - title prefix `[goblin-mosh]` -> `[mosh]`
  - user-facing messages `goblin-mosh-client`, `goblin-mosh-server`, `goblin-mosh` -> stock names
- `src/moshcp/mosh-compile-dictionary.cc`
  - status prefix `goblin-mosh-compile-dictionary:` -> `mosh-compile-dictionary:`
- `configure.ac`
  - help text `goblin-mosh-client`/`goblin-mosh-server` -> stock names.

## Tests

- `src/tests/local.test`
  - wrapper path `../../scripts/goblin-mosh` -> `../../scripts/mosh`
  - frontend paths `goblin-mosh-client`/`goblin-mosh-server` -> stock names
- `src/tests/e2e-test`
  - `MOSH_CLIENT` and `MOSH_SERVER` paths back to stock frontend binaries
  - SUT wrapper path `../../scripts/goblin-mosh` -> `../../scripts/mosh`

## Quick Check

After reverting, run:

```sh
make -j4
make -C src/tests check TESTS="local.test e2e-success.test"
```

The second command may need to run outside restrictive sandboxes because it uses
local sockets and ptys.
