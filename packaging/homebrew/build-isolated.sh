#!/bin/bash
# Build and bottle without changing the installed release or its opt links.
set -euo pipefail
brew_repository=$(brew --repository)
brew_prefix=$(brew --prefix)
isolated="$DEPLOY_WORK/homebrew"
mkdir -p "$isolated/bin" "$isolated/Library/Taps" "$isolated/Cellar" "$isolated/opt" "$isolated/var/homebrew"
cp "$brew_repository/bin/brew" "$isolated/bin/brew"
ln -s "$brew_repository/Library/Homebrew" "$isolated/Library/Homebrew"
# Clone installed dependency kegs on APFS: Homebrew requires real kegs below
# its Cellar, and cloning keeps their data blocks shared with the originals.
python3 - "$brew_prefix" "$isolated" <<'PYDEPS'
import json
import os
from pathlib import Path
import subprocess
import sys
original, isolated = map(Path, sys.argv[1:])
pending = ['autoconf', 'automake', 'pkgconf', 'abseil', 'djvulibre', 'libpng',
           'librsync', 'ncurses', 'openssl@3', 'protobuf', 'webp', 'zstd', 'python@3.14']
cloned = set()
while pending:
    name = pending.pop()
    if name in cloned:
        continue
    rack = original / 'Cellar' / name
    if not rack.is_dir():
        raise SystemExit('Missing installed build dependency: ' + name)
    subprocess.run(['cp', '-cR', str(rack), str(isolated / 'Cellar' / name)], check=True)
    cloned.add(name)
    for receipt in rack.glob('*/INSTALL_RECEIPT.json'):
        pending.extend(d['full_name'].rsplit('/', 1)[-1]
                       for d in json.loads(receipt.read_text()).get('runtime_dependencies', []))
    opt = original / 'opt' / name
    if opt.is_dir():
        (isolated / 'opt' / name).symlink_to('../Cellar/' + name + '/' + opt.resolve().name)
PYDEPS
for owner in "$brew_repository/Library/Taps/"*; do
  [ "${owner##*/}" = goblin-deploy ] || ln -s "$owner" "$isolated/Library/Taps/${owner##*/}"
done
export PATH="$isolated/bin:$PATH"
export XDG_CONFIG_HOME="$DEPLOY_WORK/build-config"
mkdir -p "$DEPLOY_WORK/brew-temp"
export HOMEBREW_TEMP="$DEPLOY_WORK/brew-temp"
brew trust --formula goblin-deploy/build/goblin-skiff
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 HOMEBREW_NO_ENV_HINTS=1
cp packaging/homebrew/goblin-skiff.rb.in "$DEPLOY_WORK/goblin-skiff.rb"
# Build with the selected CLT directly. Homebrew's install preflight rejects
# an unrelated older Xcode.app even when the current CLT is selected.
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
export CC="$DEVELOPER_DIR/usr/bin/clang" CXX="$DEVELOPER_DIR/usr/bin/clang++"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export CPATH="$brew_prefix/opt/librsync/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$brew_prefix/opt/librsync/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export CPPFLAGS="-I$brew_prefix/opt/openssl@3/include -I$brew_prefix/opt/ncurses/include"
export LDFLAGS="-L$brew_prefix/opt/openssl@3/lib -L$brew_prefix/opt/ncurses/lib -Wl,-headerpad_max_install_names"
export PKG_CONFIG_PATH="$brew_prefix/opt/openssl@3/lib/pkgconfig:$brew_prefix/opt/ncurses/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
full=goblin-deploy/build/goblin-skiff
brew tap-new --no-git goblin-deploy/build
formula="$isolated/Library/Taps/goblin-deploy/homebrew-build/Formula/goblin-skiff.rb"
python3 - "$DEPLOY_WORK/goblin-skiff.rb" "$formula" <<'PYFORMULA'
import os
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text()
for key, value in [('url', os.environ['DEPLOY_SOURCE_URL']),
                   ('version', os.environ['DEPLOY_BREW_VERSION']),
                   ('sha256', os.environ['DEPLOY_SOURCE_SHA256'])]:
    text = re.sub(r'^  ' + key + r' "[^"\n]*"', '  ' + key + ' "' + value + '"', text, flags=re.M)
Path(sys.argv[2]).write_text(text)
PYFORMULA
cache=$(brew --cache --build-from-source "$full")
mkdir -p "$(dirname "$cache")"
cp "$DEPLOY_SOURCE" "$cache"
prefix="$isolated/Cellar/goblin-skiff/$DEPLOY_BREW_VERSION"
autoreconf -fi
./configure --prefix="$prefix" --docdir="$prefix/share/doc/goblin-skiff" \
  --disable-completion --disable-ufw --without-utempter \
  --with-crypto-library=apple-common-crypto --with-zstd --with-librsync \
  --with-fips-crypto --without-libraptorq --enable-compile-warnings=yes
make -j"$DEPLOY_JOBS"
sh packaging/check-prebuilt-fec.sh ./src/moshcp/goblin-skiffcp
runtime=$(mktemp -d /tmp/skiff-check.XXXXXX)
trap 'rm -rf "$runtime"' EXIT
XDG_RUNTIME_DIR="$runtime" make -j"${DEPLOY_TEST_JOBS:-1}" check TESTS="$(cat packaging/release-tests.txt)"
cp src/tests/test-suite.log "$DEPLOY_OUT/test-suite.log"
make install
mkdir -p "$prefix/.brew" "$prefix/share/bash-completion/completions"
cp "$formula" "$prefix/.brew/goblin-skiff.rb"
cp conf/bash-completion/completions/goblin-skiff "$prefix/share/bash-completion/completions/"
mkdir -p "$prefix/share/goblin-skiff/tests" "$prefix/share/goblin-skiff/verification"
cp src/tests/control-panel-integration.py packaging/check-prebuilt-fec.sh "$prefix/share/goblin-skiff/tests/"
cp src/tests/test-suite.log "$prefix/share/goblin-skiff/verification/"
test -f "$prefix/share/man/man1/goblin-skiff.1"
GOBLIN_SKIFF_TEST_CLIENT="$prefix/bin/goblin-skiff-client" \
GOBLIN_SKIFF_TEST_SERVER="$prefix/bin/goblin-skiff-server" \
python3 src/tests/control-panel-integration.py --files
GOBLIN_SKIFF_TEST_CLIENT="$prefix/bin/goblin-skiff-client" \
GOBLIN_SKIFF_TEST_SERVER="$prefix/bin/goblin-skiff-server" \
python3 src/tests/control-panel-integration.py --udp-relay
brew ruby -e 'require "formula"; require "tab/tab"; f = Formulary.factory(ARGV.fetch(0)); t = Tab.create(f, :clang, "libc++"); t.built_as_bottle = true; t.built_prefix = HOMEBREW_PREFIX.to_s; t.write' "$full"
mkdir "$DEPLOY_WORK/bottle"
(cd "$DEPLOY_WORK/bottle"
 brew bottle --json --root-url="$DEPLOY_BREW_URL" "$full"
 brew bottle --merge --write --no-commit ./*.bottle.json
 python3 - "$DEPLOY_OUT" <<'PYBOTTLE'
import json
from pathlib import Path
import shutil
import sys
manifest, = Path('.').glob('*.bottle.json')
info, = json.loads(manifest.read_text()).values()
tags = info['bottle']['tags']
if list(tags) != ['arm64_tahoe']:
    raise SystemExit('Unexpected bottle platforms: ' + repr(list(tags)))
tag = tags['arm64_tahoe']
shutil.move(tag['local_filename'], Path(sys.argv[1]) / tag['filename'])
shutil.copy2(manifest, Path(sys.argv[1]) / manifest.name)
PYBOTTLE
)
cp "$formula" "$DEPLOY_OUT/goblin-skiff.rb"
brew untap --force goblin-deploy/build
