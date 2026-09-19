# Modified for Goblin Skiff on 2026-09-19.
Name:		goblin-skiff
Version:	1.2.5
Release:	1%{?dist}
Summary:	Mobile shell that supports roaming and intelligent local echo

License:	GPLv3+
Group:		Applications/Internet
URL:		https://skiff.goblinreactor.com/
Source0:	https://github.com/adamdeprince/goblin-skiff/releases/download/goblin-skiff-%{version}/goblin-skiff-%{version}.tar.gz

BuildRequires:	protobuf-compiler
BuildRequires:	protobuf-devel
BuildRequires:	libutempter-devel
BuildRequires:	zlib-devel
BuildRequires:	ncurses-devel
BuildRequires:	openssl-devel
Requires:	openssh-clients
Requires:	openssl
Requires:	perl-IO-Socket-IP

%description
Goblin Skiff is a fork of Mosh, optimized for bandwidth-constrained links.
It supports:
  - intermittent network connectivity,
  - roaming to different IP address without dropping the connection, and
  - intelligent local echo and line editing to reduce the effects
    of "network lag" on high-latency connections.


%prep
%setup -q


%build
# Use upstream's more aggressive hardening instead of Fedora's defaults
export CFLAGS="-g -O2" CXXFLAGS="-g -O2"
%configure --enable-compile-warnings=error
make %{?_smp_mflags}


%install
make install DESTDIR=$RPM_BUILD_ROOT


%files
%doc README.md COPYING ChangeLog
%{_bindir}/goblin-skiff
%{_bindir}/goblin-skiff-client
%{_bindir}/goblin-skiff-server
%{_bindir}/goblin-skiffcp
%{_bindir}/goblin-skiff-compile-dictionary
%{_mandir}/man1/goblin-skiff.1.gz
%{_mandir}/man1/goblin-skiff-client.1.gz
%{_mandir}/man1/goblin-skiff-server.1.gz
%{_mandir}/man1/goblin-skiffcp.1.gz
%{_mandir}/man1/goblin-skiff-compile-dictionary.1.gz
%{_docdir}/%{name}/


%changelog
* Sun Jul 12 2015 John Hood <cgull@glup.org> - 1.2.5-1
- Update to mosh 1.2.5

* Fri Jun 26 2015 John Hood <cgull@glup.org> - 1.2.4.95rc2-1
- Update to mosh 1.2.4.95rc2

* Mon Jun 08 2015 John Hood <cgull@glup.org> - 1.2.4.95rc1-1
- Update to mosh 1.2.4.95rc1

* Wed Mar 27 2013 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2.4-1
- Update to mosh 1.2.4

* Sun Mar 10 2013 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2.3-3
- Rebuilt for Protobuf API change from 2.4.1 to 2.5.0

* Thu Feb 14 2013 Fedora Release Engineering <rel-eng@lists.fedoraproject.org> - 1.2.3-2
- Rebuilt for https://fedoraproject.org/wiki/Fedora_19_Mass_Rebuild

* Fri Oct 19 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2.3-1
- Update to mosh 1.2.3

* Fri Jul 20 2012 Fedora Release Engineering <rel-eng@lists.fedoraproject.org> - 1.2.2-2
- Rebuilt for https://fedoraproject.org/wiki/Fedora_18_Mass_Rebuild

* Wed Jun 13 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2.2-1
- Update to mosh 1.2.2

* Sat Apr 28 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2-2
- Add -g and -O2 CFLAGS

* Fri Apr 27 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.2-1
- Update to mosh 1.2.

* Mon Mar 26 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.1.1-1
- Update to mosh 1.1.1.

* Wed Mar 21 2012 Alexander Chernyakhovsky <achernya@mit.edu> - 1.1-1
- Initial packaging for mosh.
