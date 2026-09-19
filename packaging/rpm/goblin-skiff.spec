# Modified for Goblin Skiff on 2026-09-19.
%global debug_package %{nil}

Name:           goblin-skiff
Version:        1.4.0
Release:        20260911.2%{?dist}
Summary:        Mobile shell optimized for low-bandwidth links
License:        GPL-3.0-or-later AND ISC
URL:            https://skiff.goblinreactor.com/
Source0:        goblin-skiff-1.4.0-goblin20260911.2.tar.gz

# Preserve the source-file OpenSSL linking exceptions and all upstream notices.
BuildRequires:  gcc-c++, make, autoconf, automake, pkgconf-pkg-config
BuildRequires:  perl, protobuf-compiler, protobuf-devel
BuildRequires:  libutempter-devel, zlib-devel, ncurses-devel
BuildRequires:  openssl-devel >= 3.0
BuildRequires:  librsync-devel >= 2.3
BuildRequires:  libzstd-devel, libwebp-devel, libpng-devel
BuildRequires:  djvulibre-devel >= 3.5.28
BuildRequires:  djvulibre
BuildRequires:  bash-completion, python3, glibc-langpack-en
Requires:       openssh-clients
Requires:       djvulibre
Requires:       perl(IO::Socket::IP)

%description
Goblin Skiff is a fork of Mosh, optimized for bandwidth-constrained links. It combines zstd level 22
compression, adaptive pacing, forward error correction and resumable file-menu
transfers with librsync deltas. The commands coexist with the
distribution's mosh package. OpenSSL 3 provider support is included, but this
package does not supply a validated FIPS module or claim FIPS compliance.

%prep
%setup -q -n goblin-skiff

%build
autoreconf -fi
%configure --disable-silent-rules --disable-completion --disable-ufw \
    --docdir=%{_docdir}/%{name} --with-utempter --with-zstd \
    --with-librsync --with-fips-crypto --without-libraptorq \
    --enable-compile-warnings=yes
%make_build
sh packaging/check-prebuilt-fec.sh ./src/moshcp/goblin-skiffcp

%check
mkdir -m 700 package-test-runtime
XDG_RUNTIME_DIR="$PWD/package-test-runtime" %make_build check \
    TESTS='ocb-aes encrypt-decrypt fips-crypto base64 nonce-incr fec-codecs bulk-datagram skiffcp-protocol bulk-loss-sim transport-compression session-version state-samples osc52-parse kitty-graphics terminal-geometry terminal-display tmux-control control-panel mascot file-transfer link-budget sixel-state terminal-extensions download download-forward udp-relay udp-jump-wrapper.test'

%install
%make_install
# Fedora splits completion pkg-config metadata differently from Rocky. Use
# the shared data directory explicitly instead of the legacy /etc fallback.
install -D -m 0644 conf/bash-completion/completions/goblin-skiff \
    %{buildroot}%{_datadir}/bash-completion/completions/goblin-skiff
install -m 0644 README.md GOBLIN_DOWNLOAD_PROTOCOL.md SIXEL_STATE.md AUDIO.md UDP_RELAY.md \
    debian/copyright %{buildroot}%{_docdir}/%{name}/

%files
%license COPYING debian/copyright
%{_bindir}/goblin-skiff
%{_bindir}/goblin-skiff-client
%{_bindir}/goblin-skiff-server
%{_bindir}/goblin-skiffcp
%{_bindir}/goblin-skiff-compile-dictionary
%{_mandir}/man1/goblin-skiff*.1*
%{_datadir}/bash-completion/completions/goblin-skiff
%{_docdir}/%{name}/

%changelog
* Fri Sep 11 2026 Adam DePrince <adam.deprince@gmail.com> - 1.4.0-20260911.2
- Carry session UDP traffic through authenticated, independently keyed jump relays.
- Test installed four-hop transfers and keep prebuilt binaries RaptorQ-free.

* Wed Sep 09 2026 Adam DePrince <adam.deprince@gmail.com> - 1.4.0-20260909.1
- Package Goblin Skiff with matching source and native system dependencies.
- Include the file-transfer scheduler and bounded download-consent test fixes.
