#!/bin/bash
# Finalize an unpublished old-world package without installing it.
set -euo pipefail
package=${1:?Debian package path required}
work=$(mktemp -d /tmp/skiff-package.XXXXXX)
trap 'rm -rf "$work"' EXIT
dpkg-deb -R "$package" "$work/root"
root="$work/root"
private="$root/usr/lib/goblin-skiff"
licenses="$root/usr/share/doc/goblin-skiff/private-licenses"
mkdir -p "$licenses"
cp "$HOME/skiff-deps/bin/cjb2" "$private/cjb2"
patchelf --set-rpath '$ORIGIN' "$private/cjb2"
python3 - "$root/usr/bin" "$HOME/skiff-deps/bin/cjb2" <<'PYRELOCATE'
from pathlib import Path
import sys
old = sys.argv[2].encode() + b'\0'
new = b'/usr/lib/goblin-skiff/cjb2\0'
if len(new) > len(old):
    raise SystemExit('Encoder path cannot be relocated in place')
replacement = new + bytes(len(old) - len(new))
changed = 0
for binary in Path(sys.argv[1]).iterdir():
    data = binary.read_bytes()
    if data.startswith(b'\x7fELF') and old in data:
        changed += data.count(old)
        binary.write_bytes(data.replace(old, replacement))
if not changed:
    raise SystemExit('No encoder references were relocated')
print('Relocated private encoder references:', changed)
PYRELOCATE
cp "$HOME/skiff-deps/src/build/protobuf-3.21.12/LICENSE" "$licenses/protobuf.txt"
cp "$HOME/skiff-deps/src/build/libwebp-1.6.0/COPYING" "$licenses/libwebp.txt"
cp "$HOME/skiff-deps/src/build/djvulibre-3.5.30/COPYING" "$licenses/DjVuLibre-GPL.txt"
cp "$HOME/skiff-deps/src/build/djvulibre-3.5.30/COPYRIGHT" "$licenses/DjVuLibre-copyright.txt"
cp "$HOME/gcc/COPYING3" "$licenses/GCC-GPL.txt"
cp "$HOME/gcc/COPYING.RUNTIME" "$licenses/GCC-runtime-exception.txt"
cp "$0" "$root/usr/share/doc/goblin-skiff/finalize-package.sh"
# Match normal Debian release stripping, while retaining required exports.
for binary in "$root/usr/bin/"* "$private/"*; do
  if readelf -h "$binary" >/dev/null 2>&1; then
    strip --strip-unneeded "$binary"
    env -u LD_LIBRARY_PATH ldd "$binary" | tee -a "$work/linkage.log"
  fi
done
! grep -q 'not found' "$work/linkage.log"
env -u LD_LIBRARY_PATH "$root/usr/bin/goblin-skiff-client" --version
env -u LD_LIBRARY_PATH "$root/usr/bin/goblin-skiff-server" --version
printf 'P1\n1 1\n0\n' > "$work/pixel.pbm"
env -u LD_LIBRARY_PATH "$private/cjb2" -lossless "$work/pixel.pbm" "$work/pixel.djvu"
test -s "$work/pixel.djvu"
(cd "$root" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z | xargs -0 -r md5sum) > "$root/DEBIAN/md5sums"
size=$(du -sk --exclude=DEBIAN "$root" | cut -f1)
sed -i "s/^Installed-Size:.*/Installed-Size: $size/" "$root/DEBIAN/control"
dpkg-deb --root-owner-group -Zxz --build "$root" "$work/final.deb"
mv "$work/final.deb" "$package"
printf 'Final package: '
sha256sum "$package"
