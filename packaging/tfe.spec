# tfe — RPM packaging (Fedora / RHEL family)
#
# Preferred: ./build.sh (or ./build.sh -r) runs rpmbuild inside tfe-build and
# writes RPMs to ./dist/rpm/. Uses prebuilt ./dist binaries.
# Falls back to `make -C src` if dist/ binaries are missing in the source
# tarball.
#
# TFE must be built 32-bit (-m32, see src/Makefile), so the x86_64 package
# depends on the i686 glibc and libstdc++.
#
# Host-side rpmbuild (optional; same prebuilt path):
#
#   ./build.sh
#   VERSION=$(awk '/^Version:/{print $2; exit}' packaging/tfe.spec)
#   mkdir -p "$HOME/rpmbuild"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
#   tar -czf "$HOME/rpmbuild/SOURCES/tfe-${VERSION}.tar.gz" \
#     --exclude=.git --exclude='*.rpm' --exclude='./src/OBJ' \
#     --transform "s,^[.],tfe-${VERSION}," .
#   cp packaging/tfe.spec "$HOME/rpmbuild/SPECS/"
#   rpmbuild -ba "$HOME/rpmbuild/SPECS/tfe.spec"
#
# SRPM → mock (clean chroot):
#
#   mock -r fedora-40-x86_64 "$HOME/rpmbuild/SRPMS/tfe-${VERSION}-*.src.rpm"

Name:           tfe
Version:        1.0
Release:        3%{?dist}
# build.sh increments the number above on each run so rpm -U / dnf can upgrade.
Summary:        The Forest's Edge (TFE) Classic MUD server

# TFE Classic ships no licence file or notice. Confirm the terms with the
# upstream authors before distributing this package.
License:        LicenseRef-TFE-unknown
URL:            https://github.com/phessto/The-forests-edge
Source0:        %{name}-%{version}.tar.gz

ExclusiveArch:  x86_64 %{ix86}

BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  glibc-devel(x86-32)
BuildRequires:  libstdc++-devel(x86-32)
# The SSH listener (src/ssh.cc) links the 32-bit libssh.
BuildRequires:  libssh-devel(x86-32)
%if 0%{?rhel} && 0%{?rhel} < 9
# EL8: systemd macros ship in systemd itself
BuildRequires:  systemd
%else
BuildRequires:  systemd-rpm-macros
%endif
Requires(pre):  shadow-utils
# tfe-run and tfe-accounts
Requires:       /usr/bin/awk coreutils grep sed
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
Requires:       glibc(x86-32)
Requires:       libstdc++(x86-32)
Requires:       libssh(x86-32)
Requires:       bash

# Prebuilt -g binaries are shipped as built; skip the debuginfo split.
%global debug_package %{nil}
# The world data is data, not scripts: do not mangle or byte-compile it.
%global __brp_mangle_shebangs %{nil}

%description
The Forest's Edge (TFE) Classic is a text-based multi-user dungeon (MUD).

This package installs:
  - tfe        the MUD server, run by tfe.service as system user tfe
  - daemon     the host-name lookup helper tfe starts as ./daemon
  - tfe-run    service wrapper: seeds the world, maps exit reasons for systemd
  - the world (areas, tables, files, notes, help pages) under /usr/share/tfe,
    copied into /var/lib/tfe on first start

The service is not enabled by default. Settings are in /etc/tfe/tfe.env.

%prep
%setup -q

%build
set -e
mkdir -p dist
if [ -x dist/tfe ] && [ -x dist/daemon ]; then
  echo "Using prebuilt dist/ artifacts"
else
  echo "dist/ incomplete; building with make -C src"
  make -C src %{?_smp_mflags}
  install -m 0755 src/tfe src/daemon dist/
fi

%install
rm -rf %{buildroot}

install -d -m 0755 %{buildroot}%{_libexecdir}/tfe
install -p -m 0755 dist/tfe             %{buildroot}%{_libexecdir}/tfe/tfe
install -p -m 0755 dist/daemon          %{buildroot}%{_libexecdir}/tfe/daemon
install -p -m 0755 packaging/tfe-run    %{buildroot}%{_libexecdir}/tfe/tfe-run
install -p -m 0755 packaging/tfe-accounts %{buildroot}%{_libexecdir}/tfe/tfe-accounts

# Pristine world, seeded into /var/lib/tfe by tfe-run. The upstream test
# login (TempImm / temp account) is kept apart and only seeded on request.
install -d -m 0755 %{buildroot}%{_datadir}/tfe/world
for d in area files tables notes public_html; do
  cp -r --preserve=timestamps "$d" %{buildroot}%{_datadir}/tfe/world/
done
rm -f %{buildroot}%{_datadir}/tfe/world/files/accounts
install -d -m 0755 %{buildroot}%{_datadir}/tfe/test-login/files
install -d -m 0755 %{buildroot}%{_datadir}/tfe/test-login/player
install -p -m 0644 files/accounts   %{buildroot}%{_datadir}/tfe/test-login/files/accounts
install -p -m 0644 player/TempImm   %{buildroot}%{_datadir}/tfe/test-login/player/TempImm
find %{buildroot}%{_datadir}/tfe -type d -exec chmod 0755 {} +
find %{buildroot}%{_datadir}/tfe -type f -exec chmod 0644 {} +

install -d -m 0755 %{buildroot}%{_unitdir}
install -p -m 0644 packaging/tfe.service %{buildroot}%{_unitdir}/tfe.service

install -d -m 0755 %{buildroot}%{_tmpfilesdir}
install -p -m 0644 packaging/tfe.tmpfiles %{buildroot}%{_tmpfilesdir}/tfe.conf

install -d -m 0755 %{buildroot}%{_presetdir}
install -p -m 0644 packaging/90-tfe.preset %{buildroot}%{_presetdir}/90-tfe.preset

install -d -m 0755 %{buildroot}%{_sysconfdir}/tfe
install -p -m 0644 packaging/tfe.env %{buildroot}%{_sysconfdir}/tfe/tfe.env

# Game root: every directory from tfe.tmpfiles, plus bin/ with the binaries
# tfe expects in its working directory.
while read -r type path mode user group rest; do
  [ "$type" = d ] || continue
  install -d -m "$mode" "%{buildroot}$path"
done < packaging/tfe.tmpfiles
ln -s ../../../..%{_libexecdir}/tfe/tfe    %{buildroot}%{_localstatedir}/lib/tfe/bin/tfe
ln -s ../../../..%{_libexecdir}/tfe/daemon %{buildroot}%{_localstatedir}/lib/tfe/bin/daemon

# %%files entries for the tfe-owned directories, generated from the same list.
awk '$1 == "d" && $4 == "tfe" { print "%%dir %%attr(" $3 "," $4 "," $5 ") " $2 }' \
  packaging/tfe.tmpfiles > tfe-dirs.files

%pre
getent group tfe >/dev/null || groupadd -r tfe
getent passwd tfe >/dev/null || \
  useradd -r -g tfe -d %{_localstatedir}/lib/tfe -s /sbin/nologin \
    -c "The Forest's Edge MUD" tfe
exit 0

%post
%tmpfiles_create tfe.conf
%systemd_post tfe.service
if [ "$1" -eq 1 ]; then
  echo "tfe: installed, not enabled and not started. Next:"
  echo "tfe:   1. edit /etc/tfe/tfe.env (port; TFE_SEED_TEST_LOGIN=yes adds the"
  echo "tfe:      upstream test immortal TempImm, password llama)"
  echo "tfe:   2. systemctl enable --now tfe"
  echo "tfe:   3. telnet <host> 4000, or ssh -p 4022 play@<host>; at 'Choice:'"
  echo "tfe:      type TempImm, then llama (open 4000/tcp and 4022/tcp in the"
  echo "tfe:      firewall for remote players)"
elif ! grep -q '^TFE_SSH_PORT=' %{_sysconfdir}/tfe/tfe.env 2>/dev/null; then
  echo "tfe: this version can listen for SSH. Your /etc/tfe/tfe.env was kept, so"
  echo "tfe: SSH stays off; to turn it on add TFE_SSH_PORT=4022 (see the comments"
  echo "tfe: in /etc/tfe/tfe.env.rpmnew) and restart tfe."
fi

%preun
%systemd_preun tfe.service

%postun
# No restart on upgrade: systemctl restart kills tfe without the in-game
# save. The new binaries take effect at the next in-game reboot or restart.
%systemd_postun tfe.service

%files -f tfe-dirs.files
%doc README
%dir %{_libexecdir}/tfe
%{_libexecdir}/tfe/tfe
%{_libexecdir}/tfe/daemon
%{_libexecdir}/tfe/tfe-run
%{_libexecdir}/tfe/tfe-accounts
%{_datadir}/tfe
%{_unitdir}/tfe.service
%{_presetdir}/90-tfe.preset
%{_tmpfilesdir}/tfe.conf
%dir %attr(0755,root,root) %{_sysconfdir}/tfe
%config(noreplace) %attr(0644,root,root) %{_sysconfdir}/tfe/tfe.env
%dir %attr(0750,root,tfe) %{_localstatedir}/lib/tfe
%dir %attr(0755,root,root) %{_localstatedir}/lib/tfe/bin
%{_localstatedir}/lib/tfe/bin/tfe
%{_localstatedir}/lib/tfe/bin/daemon

%changelog
* Sun Oct 25 2026 Joshua D <58604081+phessto@users.noreply.github.com> - 1.0-3
- SSH listener in tfe (libssh): TFE_SSH_PORT (default 4022 in tfe.env),
  TFE_SSH_USER, TFE_SSH_MAX_SESSIONS, TFE_SSH_LOGIN_TIMEOUT,
  TFE_SSH_HOSTKEY_DIR; host keys in /var/lib/tfe/ssh
- needs 32-bit libssh

* Sun Oct 25 2026 Joshua D <58604081+phessto@users.noreply.github.com> - 1.0-2
- tfe-run: seed the test login as a pair (temp account + TempImm), on any
  start; pre-flight check that every player's account exists; replace a
  zero-byte files/accounts; restore missing tables/notes; show why a boot
  failed in the journal (output without CRs, bugs.txt excerpt)
- tfe.service: recreate missing directories before start
- no service restart on upgrade; %post prints the next steps

* Sat Oct 24 2026 Joshua D <58604081+phessto@users.noreply.github.com> - 1.0-1
- Initial RPM: 32-bit tfe and daemon, tfe-run wrapper, systemd unit,
  tfe system user, world seeded from /usr/share/tfe into /var/lib/tfe
