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
brew trust --formula goblin-deploy/build/goblin-skiff
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 HOMEBREW_NO_ENV_HINTS=1
cp packaging/homebrew/goblin-skiff.rb.in "$DEPLOY_WORK/goblin-skiff.rb"
"$DEPLOY_SELF" brew-bottle "$DEPLOY_WORK/goblin-skiff.rb"
