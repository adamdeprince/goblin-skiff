#!/bin/sh
# Run only in the disposable packaging container; /out is its artifact mount.
set -eu
test "$#" -eq 1
goblin_suite=$1
case "$goblin_suite" in fedora44|rocky9|rocky10) ;; *) exit 2 ;; esac
test -d /out
test ! -e /build
mkdir -p /build/rpmbuild/BUILD /build/rpmbuild/BUILDROOT /build/rpmbuild/RPMS \
    /build/rpmbuild/SOURCES /build/rpmbuild/SPECS /build/rpmbuild/SRPMS
goblin_save_logs() {
    find /build/rpmbuild/BUILD -path '*/src/tests/test-suite.log' \
        -exec cp '{}' /out/unit-tests.log \;
}
trap goblin_save_logs EXIT
cp /tmp/goblin-snapshot.tar.gz /build/rpmbuild/SOURCES/goblin-mosh-1.4.0-goblin20260911.1.tar.gz
cp /tmp/goblin-mosh.spec /build/rpmbuild/SPECS/goblin-mosh.spec
rpmbuild -ba --noclean --define '_topdir /build/rpmbuild' --define '_smp_mflags -j12' \
    /build/rpmbuild/SPECS/goblin-mosh.spec
goblin_rpm=$(find /build/rpmbuild/RPMS -name 'goblin-mosh-*.rpm' -type f)
test "$(printf '%s\n' "$goblin_rpm" | wc -l)" -eq 1
test -f "$goblin_rpm"
# The locally built package is not signed until publication on hail. Public
# installation tests use gpgcheck and repo_gpgcheck, with the archive key.
dnf -y install "$goblin_rpm"
/usr/bin/goblin-mosh-client --version
/usr/bin/goblin-mosh-server --version
mkdir -m 700 /build/installed-runtime
export XDG_RUNTIME_DIR=/build/installed-runtime
export GOBLIN_TEST_CLIENT=/usr/bin/goblin-mosh-client GOBLIN_TEST_SERVER=/usr/bin/goblin-mosh-server
goblin_integration=$(find /build/rpmbuild/BUILD -path '*/src/tests/control-panel-integration.py' -type f)
test "$(printf '%s\n' "$goblin_integration" | wc -l)" -eq 1
cd "$(dirname "$goblin_integration")"
sh ../../packaging/check-prebuilt-fec.sh /usr/bin/goblin-moshcp > /out/prebuilt-fec.log 2>&1
python3 "$goblin_integration" --files > /out/installed-transfer.log 2>&1
python3 "$goblin_integration" --udp-relay > /out/installed-relay.log 2>&1
cp "$goblin_rpm" /build/rpmbuild/SRPMS/goblin-mosh-*.src.rpm /out/
rpm -qip "$goblin_rpm" > /out/package-info.txt
rpm -qpR "$goblin_rpm" > /out/package-requires.txt
rpm -qa | sort > /out/build-packages.txt
printf 'Package and installed transfer/UDP-relay tests passed: %s\n' "$goblin_suite"
