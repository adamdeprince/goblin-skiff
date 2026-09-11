#!/bin/sh
# Run only in the disposable packaging container; /out is its artifact mount.
set -eu
test "$#" -eq 2
goblin_suite=$1
goblin_version=$2
case "$goblin_suite" in jammy|noble|resolute|bookworm|trixie) ;; *) exit 2 ;; esac
dpkg --validate-version "$goblin_version"
test -d /out
goblin_save_logs() {
    if test -f /build/goblin-mosh/src/tests/test-suite.log; then
        cp /build/goblin-mosh/src/tests/test-suite.log /out/unit-tests.log
    fi
}
trap goblin_save_logs EXIT
test ! -e /build
mkdir /build
tar -xzf /tmp/goblin-snapshot.tar.gz -C /build
cd /build/goblin-mosh
export DEBFULLNAME='Adam DePrince' DEBEMAIL='adam.deprince@gmail.com'
dch --newversion "$goblin_version" --distribution "$goblin_suite" \
    --force-distribution "Build against the native $goblin_suite distribution libraries."
dpkg-buildpackage -us -uc -j12
goblin_deb="/build/goblin-mosh_${goblin_version}_$(dpkg --print-architecture).deb"
test -f "$goblin_deb"
apt-get install -y --no-install-recommends "$goblin_deb"
/usr/bin/goblin-mosh-client --version
/usr/bin/goblin-mosh-server --version
sh packaging/check-prebuilt-fec.sh /usr/bin/goblin-moshcp > /out/prebuilt-fec.log 2>&1
mkdir -m 700 /build/installed-runtime
export XDG_RUNTIME_DIR=/build/installed-runtime
export GOBLIN_TEST_CLIENT=/usr/bin/goblin-mosh-client GOBLIN_TEST_SERVER=/usr/bin/goblin-mosh-server
cd src/tests
python3 control-panel-integration.py --files > /out/installed-transfer.log 2>&1
cp /build/*.deb /build/*.dsc /build/*.tar.xz /build/*.buildinfo /build/*.changes /out/
cp test-suite.log /out/unit-tests.log
dpkg-deb --info "$goblin_deb" > /out/package-info.txt
dpkg-query -W > /out/build-packages.txt
printf 'Package and installed transfer test passed: %s\n' "$goblin_version"
