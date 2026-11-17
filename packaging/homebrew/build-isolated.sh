#!/bin/bash
# Build and bottle without changing the installed release or its opt links.
set -euo pipefail
brew_repository=$(brew --repository)
brew_prefix=$(brew --prefix)
isolated="$DEPLOY_WORK/homebrew"
mkdir -p "$isolated/bin" "$isolated/Library/Taps" "$isolated/Cellar" "$isolated/opt" "$isolated/var/homebrew"
cp "$brew_repository/bin/brew" "$isolated/bin/brew"
ln -s "$brew_repository/Library/Homebrew" "$isolated/Library/Homebrew"
# Existing dependency kegs are read through symlinks. The product has its own
# rack, tap, locks and opt link, all below DEPLOY_WORK.
for rack in "$brew_prefix/Cellar/"*; do
  [ "${rack##*/}" = goblin-skiff ] || ln -s "$rack" "$isolated/Cellar/${rack##*/}"
done
for opt in "$brew_prefix/opt/"*; do
  [ "${opt##*/}" = goblin-skiff ] || ln -s "$opt" "$isolated/opt/${opt##*/}"
done
for owner in "$brew_repository/Library/Taps/"*; do
  [ "${owner##*/}" = goblin-deploy ] || ln -s "$owner" "$isolated/Library/Taps/${owner##*/}"
done
export PATH="$isolated/bin:$PATH"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 HOMEBREW_NO_ENV_HINTS=1
cp packaging/homebrew/goblin-skiff.rb.in "$DEPLOY_WORK/goblin-skiff.rb"
"$DEPLOY_SELF" brew-bottle "$DEPLOY_WORK/goblin-skiff.rb"
