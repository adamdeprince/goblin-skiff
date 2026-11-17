#!/bin/bash
# Kylin old-world LoongArch: use existing source-built graphics dependencies,
# add OpenSSL 3/librsync privately, and bundle private runtime libraries.
set -euo pipefail
. "$HOME/skiff-deps/env.sh"
export CC=/opt/loongson-gcc-15.2.0/bin/gcc
export CXX=/opt/loongson-gcc-15.2.0/bin/g++
export PATH="/opt/loongson-cmake-4.3.2/bin:/opt/loongson-gcc-15.2.0/bin:$PATH"
cache="$HOME/skiff-release-deps"
prefix="$cache/prefix"
mkdir -p "$cache/build" "$prefix"
if [ ! -f "$prefix/lib/pkgconfig/openssl.pc" ]; then
  expected=$(awk '{print $1}' "$cache/downloads/openssl-3.5.5.tar.gz.sha256")
  actual=$(sha256sum "$cache/downloads/openssl-3.5.5.tar.gz" | cut -d' ' -f1)
  [ "$expected" = "$actual" ] || deploy_die "OpenSSL source checksum mismatch"
  tar -xzf "$cache/downloads/openssl-3.5.5.tar.gz" -C "$cache/build"
  (cd "$cache/build/openssl-3.5.5"
   ./Configure linux64-loongarch64 shared no-asm no-tests --prefix="$prefix" --libdir=lib
   make -j"$DEPLOY_JOBS"
   make install_sw)
fi
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$prefix/lib:$(dirname "$($CXX -print-file-name=libstdc++.so.6)"):$LD_LIBRARY_PATH"
if [ ! -f "$prefix/lib/librsync.so" ]; then
  tar -xzf "$cache/downloads/librsync-2.3.4.tar.gz" -C "$cache/build"
  cmake -S "$cache/build/librsync-2.3.4" -B "$cache/build/librsync" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON -DBUILD_RDIFF=OFF -DBUILD_TESTING=OFF
  cmake --build "$cache/build/librsync" --parallel "$DEPLOY_JOBS"
  cmake --install "$cache/build/librsync"
fi
export CPPFLAGS="-I$prefix/include -I$SKIFF_DEPS/include"
# Build-host paths are replaced in the staged package after testing.
export LDFLAGS="-L$prefix/lib -L$SKIFF_DEPS/lib -Wl,-rpath,$prefix/lib -Wl,-rpath,$SKIFF_DEPS/lib"
autoreconf -fi
./configure --prefix=/usr --docdir=/usr/share/doc/goblin-skiff \
  --disable-completion --disable-ufw --without-utempter --with-zstd \
  --with-librsync --with-fips-crypto --without-libraptorq --enable-compile-warnings=yes
make -j"$DEPLOY_JOBS"
sh packaging/check-prebuilt-fec.sh ./src/moshcp/goblin-skiffcp
runtime=$(mktemp -d /tmp/skiff-check.XXXXXX)
trap 'rm -rf "$runtime"' EXIT
XDG_RUNTIME_DIR="$runtime" make -j"$DEPLOY_JOBS" check TESTS="$(cat packaging/release-tests.txt)"
cp src/tests/test-suite.log "$DEPLOY_OUT/test-suite.log"
root="$DEPLOY_WORK/root"
make DESTDIR="$root" install
install -D -m 0644 conf/bash-completion/completions/goblin-skiff "$root/usr/share/bash-completion/completions/goblin-skiff"
private="$root/usr/lib/goblin-skiff"
mkdir -p "$private"
python3 - "$root/usr/bin" "$private" "$prefix" "$SKIFF_DEPS" /opt/loongson-gcc-15.2.0 <<'PYLIBS'
from pathlib import Path
import shutil
import subprocess
import sys
bindir, private, *prefixes = map(Path, sys.argv[1:])
for binary in bindir.iterdir():
    with binary.open('rb') as stream:
        if stream.read(4) != b'\x7fELF':
            continue
    for line in subprocess.check_output(['ldd', str(binary)], text=True).splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[1] != '=>':
            continue
        path = Path(fields[2]).resolve()
        if any(path.is_relative_to(prefix.resolve()) for prefix in prefixes):
            shutil.copy2(path, private / fields[0])
PYLIBS
for executable in "$root/usr/bin/"*; do
  if readelf -h "$executable" >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib/goblin-skiff' "$executable"
  fi
done
for lib in "$private/"*; do
  patchelf --set-rpath '$ORIGIN' "$lib"
done
# Verify the staged programs without the build host's library environment.
env -u LD_LIBRARY_PATH "$root/usr/bin/goblin-skiff-client" --version
env -u LD_LIBRARY_PATH "$root/usr/bin/goblin-skiff-server" --version
# dpkg-shlibdeps must not turn bundled libraries into unavailable Kylin package
# dependencies. Generate local shlibs records for every private SONAME.
: > debian/shlibs.local
for lib in "$private/"*; do
  soname=$(readelf -d "$lib" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
  if [[ "$soname" =~ ^(.*)\.so\.([0-9]+)$ ]]; then
    printf '%s %s goblin-skiff\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" >> debian/shlibs.local
  fi
done
export DEPLOY_SHLIBDEPS_ARGS="-l$private -L$PWD/debian/shlibs.local -xgoblin-skiff"
mkdir -p "$root/usr/share/doc/goblin-skiff/private-licenses"
cp "$cache/build/openssl-3.5.5/LICENSE.txt" "$root/usr/share/doc/goblin-skiff/private-licenses/OpenSSL.txt"
cp "$cache/build/librsync-2.3.4/COPYING" "$root/usr/share/doc/goblin-skiff/private-licenses/librsync.txt"
deploy_make_deb goblin-skiff "$DEPLOY_VERSION+${DEPLOY_SUITE}$DEPLOY_DEB_REVISION" "$root" \
  "Remote terminal optimized for bandwidth-constrained links" "openssh-client, perl"
sha256sum "$cache/downloads/"*.tar.gz > "$DEPLOY_OUT/private-dependency-sources.sha256"

bash packaging/loongson/finalize-package.sh "$DEPLOY_OUT/goblin-skiff_${DEPLOY_VERSION}+${DEPLOY_SUITE}${DEPLOY_DEB_REVISION}_${DEPLOY_ARCH}.deb"
