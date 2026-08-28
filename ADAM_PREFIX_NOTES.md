# Reverting the adam- Command Prefix

This branch intentionally installs and invokes the experimental binaries with an
`adam-` prefix so it can coexist with stock Mosh on the same machine. To turn it
back into normal `foo` command names, undo these rename surfaces.

## Build Targets

- `scripts/Makefile.am`
  - `bin_SCRIPTS = adam-mosh` -> `mosh`
  - target `adam-mosh:` -> `mosh:`
  - generated output file `adam-mosh` -> `mosh`
- `src/frontend/Makefile.am`
  - `adam-mosh-client` -> `mosh-client`
  - `adam-mosh-server` -> `mosh-server`
  - automake variable prefixes `adam_mosh_client_*` -> `mosh_client_*`
  - automake variable prefixes `adam_mosh_server_*` -> `mosh_server_*`
- `src/moshcp/Makefile.am`
  - `adam-moshcp` -> `moshcp`
  - `adam-mosh-compile-dictionary` -> `mosh-compile-dictionary`
  - automake variable prefixes `adam_moshcp_*` -> `moshcp_*`
  - automake variable prefixes `adam_mosh_compile_dictionary_*` -> `mosh_compile_dictionary_*`

After editing Makefile.am files, rerun `./autogen.sh` or let `make` refresh
the generated Makefiles if your tree supports that.

## Wrapper Defaults

- `scripts/mosh.pl`
  - default client command `adam-mosh-client` -> `mosh-client`
  - default server command `adam-mosh-server` -> `mosh-server`
  - user-facing help text for `adam-mosh` -> `mosh`
  - temporary zstd dictionary pattern `adam-mosh-zstd-dict.XXXXXX` -> `mosh-zstd-dict.XXXXXX`

## moshcp Socket Namespace

- `src/network/bulkcontrol.cc`
  - fallback runtime dir `/tmp/adam-moshcp-$UID` -> `/tmp/moshcp-$UID`
  - socket name `adam-moshcp-$role-$pid.sock` -> `moshcp-$role-$pid.sock`
  - latest symlink `adam-moshcp.latest` -> `moshcp.latest`
  - env var `ADAM_MOSHCP_SOCK` -> `MOSHCP_SOCK`
- `src/frontend/stmclient.cc` and `src/frontend/mosh-server.cc`
  - exported env var `ADAM_MOSHCP_SOCK` -> `MOSHCP_SOCK`
- `src/moshcp/moshcp.cc`
  - command/status prefix `adam-moshcp:` -> `moshcp:`
  - default output names `adam-moshcp.out` and `adam-moshcp-archive` -> `moshcp.out` and `moshcp-archive`
  - temp-file suffix `.adam-moshcp.tmp.` -> `.moshcp.tmp.`

## Docs and Integration Files

- Rename man pages back:
  - `man/adam-mosh.1` -> `man/mosh.1`
  - `man/adam-mosh-client.1` -> `man/mosh-client.1`
  - `man/adam-mosh-server.1` -> `man/mosh-server.1`
  - `man/adam-moshcp.1` -> `man/moshcp.1`
  - `man/adam-mosh-compile-dictionary.1` -> `man/mosh-compile-dictionary.1`
- `man/Makefile.am`
  - swap `adam-*` man-page names back to unprefixed names.
- `conf/bash-completion/completions/adam-mosh`
  - rename back to `conf/bash-completion/completions/mosh`
  - change the `complete ... adam-mosh` line back to `complete ... mosh`
- `conf/ufw/applications.d/adam-mosh`
  - rename back to `conf/ufw/applications.d/mosh`
  - profile heading `[adam-mosh]` -> `[mosh]`
- `conf/Makefile.am`
  - swap the completion and UFW file paths back.
- `README.md`
  - replace command examples and prose references from `adam-*` back to stock names.

## Runtime Strings

These are not strictly required for coexistence, but they should be reverted for
a clean upstream-style build:

- `src/frontend/mosh-client.cc`
  - version banner `adam-mosh-client` -> `mosh-client`
  - exit banner `adam-mosh` -> `mosh`
- `src/frontend/mosh-server.cc`
  - version/detach/exit banners `adam-mosh-server` -> `mosh-server`
  - utmp host tags `adam-mosh [...]` -> `mosh [...]`
  - unattached-session scanner prefix `adam-mosh ` -> `mosh `
- `src/frontend/stmclient.cc`
  - title prefix `[adam-mosh]` -> `[mosh]`
  - user-facing messages `adam-mosh-client`, `adam-mosh-server`, `adam-mosh` -> stock names
- `src/moshcp/mosh-compile-dictionary.cc`
  - status prefix `adam-mosh-compile-dictionary:` -> `mosh-compile-dictionary:`
- `configure.ac`
  - help text `adam-mosh-client`/`adam-mosh-server` -> stock names.

## Tests

- `src/tests/local.test`
  - wrapper path `../../scripts/adam-mosh` -> `../../scripts/mosh`
  - frontend paths `adam-mosh-client`/`adam-mosh-server` -> stock names
- `src/tests/e2e-test`
  - `MOSH_CLIENT` and `MOSH_SERVER` paths back to stock frontend binaries
  - SUT wrapper path `../../scripts/adam-mosh` -> `../../scripts/mosh`

## Quick Check

After reverting, run:

```sh
make -j4
make -C src/tests check TESTS="local.test e2e-success.test"
```

The second command may need to run outside restrictive sandboxes because it uses
local sockets and ptys.
