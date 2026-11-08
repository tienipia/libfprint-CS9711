# Private build of the libfprint-CS9711 fork for the CS9711 / Acl16
# (USB 2541:2549) reader, with a diagnostic tool.
#
# Everything is installed below %%{privdir}. The package replaces no
# distribution file and changes no global configuration: no ld.so.conf, no
# udev rules or hwdb, no PAM/authselect, no SELinux policy, no fingerprint
# database. The base package therefore has no effect on authentication.
# Only the optional -fprintd subpackage makes fprintd use this library.
#
# Build with packaging/rpm/make-srpm.sh, which defines commit, snapdate and
# dirtytag and creates Source0 from the working tree.

%global privdir   %{_libdir}/libfprint-cs9711
%global privlib   %{privdir}/lib

%{!?commit:   %global commit 0000000000000000000000000000000000000000}
%{!?snapdate: %global snapdate 0}
%{!?dirtytag: %global dirtytag %{nil}}
%global shortcommit %(c=%{commit}; echo ${c:0:7})
%{!?_unitdir: %global _unitdir %{_prefix}/lib/systemd/system}

# The private library keeps the upstream SONAME (libfprint-2.so.2) so that
# fprintd can load it through LD_LIBRARY_PATH without relinking. It must
# never be advertised as a provider of that SONAME (it would satisfy other
# packages' dependencies while not being on the linker path), and the
# private files must not depend on the distribution libfprint either.
%global __provides_exclude_from ^%{privdir}/.*$
%global __requires_exclude ^libfprint-2\\.so\\.2.*$

Name:           libfprint-cs9711
Version:        1.94.10
Release:        0.3.%{snapdate}git%{shortcommit}%{dirtytag}%{?dist}
Summary:        Private libfprint build with the CS9711/Acl16 driver and diagnostics

# Same terms as the distribution libfprint: LGPL-2.1-or-later (libfprint,
# SIGFM matcher) and NIST-PD (bundled NBIS).
License:        LGPL-2.1-or-later AND NIST-PD
URL:            https://github.com/tienipia/libfprint-CS9711
Source0:        %{name}-%{version}.tar.gz
Source1:        50-libfprint-cs9711.conf
Source2:        README.md

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 0.59.0
BuildRequires:  pkgconfig(glib-2.0) >= 2.68
BuildRequires:  pkgconfig(gio-unix-2.0) >= 2.68
BuildRequires:  pkgconfig(gobject-2.0) >= 2.68
BuildRequires:  pkgconfig(gusb) >= 0.3.0
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(gudev-1.0)
BuildRequires:  pkgconfig(opencv4)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  systemd-rpm-macros

%description
A copy of the libfprint-CS9711 fork (libfprint %{version} with the CS9711
driver, Acl16 support and the SIGFM matcher), built with the default driver
set and installed privately below %{privdir}. Nothing on the system uses it
unless it is asked to: the package does not register the library with the
dynamic linker and does not provide the libfprint SONAME.

It contains fprint-cs9711-diag, a diagnostic tool for the reader that reports
when the sensor is armed, uses bounded timeouts, prints JSON lines and never
stores images.

This is test software for one reader model. It is not verified for
production authentication.

%package fprintd
Summary:        Make fprintd use the private libfprint-cs9711 library (opt-in)
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       fprintd%{?_isa}

%description fprintd
Installs a systemd drop-in for fprintd.service only. It points fprintd at
the library in %{privlib}; no other program is affected. Removing this
package removes the drop-in, and fprintd loads the distribution libfprint
again the next time it starts.

The private library supports fewer readers than the distribution libfprint;
see README.md before installing. Enrolled prints stay in /var/lib/fprint and
are never removed by this package.

%prep
%autosetup -n %{name}-%{version}
cp -p %{SOURCE2} README.cs9711-rpm.md

%build
# Fedora's %%meson sets the hardening flags; the directory options after it
# override its standard paths, so every installed file stays private.
%meson \
    --prefix=%{privdir} \
    --libdir=lib \
    --libexecdir=libexec \
    --bindir=bin \
    --sbindir=bin \
    --includedir=include \
    --datadir=share \
    --mandir=share/man \
    --infodir=share/info \
    --localedir=share/locale \
    -Ddrivers=default \
    -Dintrospection=false \
    -Ddoc=false \
    -Dgtk-examples=false \
    -Dudev_rules=disabled \
    -Dudev_hwdb=disabled \
    -Dinstalled-tests=false \
    -Dcs9711-diag=true
%meson_build

%install
%meson_install
# Nothing may be built against the private copy.
rm -r %{buildroot}%{privdir}/include
rm -r %{buildroot}%{privlib}/pkgconfig
rm %{buildroot}%{privlib}/libfprint-2.so
mkdir -p %{buildroot}%{_unitdir}/fprintd.service.d
sed 's|@PRIVLIB@|%{privlib}|' %{SOURCE1} > %{buildroot}%{_unitdir}/fprintd.service.d/50-libfprint-cs9711.conf
chmod 0644 %{buildroot}%{_unitdir}/fprintd.service.d/50-libfprint-cs9711.conf

%check
%meson_test --suite unit-tests
# Nothing may be installed outside the private prefix except the fprintd
# drop-in. Build-id links and debug files are added by rpm itself.
unexpected=$(cd %{buildroot} && find . ! -type d | sed 's|^\.||' | grep -v \
    -e '^%{privdir}/' \
    -e '^%{_unitdir}/fprintd\.service\.d/50-libfprint-cs9711\.conf$' \
    -e '^/usr/lib/\.build-id/' -e '^/usr/lib/debug/' -e '^/usr/src/debug/' || :)
if [ -n "$unexpected" ]; then
    echo "Files outside the private prefix:" >&2
    echo "$unexpected" >&2
    exit 1
fi
grep -qx 'Environment=LD_LIBRARY_PATH=%{privlib}' \
    %{buildroot}%{_unitdir}/fprintd.service.d/50-libfprint-cs9711.conf

%files
%license COPYING
%doc README.md NEWS README.cs9711-rpm.md
%dir %{privdir}
%dir %{privlib}
%{privlib}/libfprint-2.so.2
%{privlib}/libfprint-2.so.2.0.0
%dir %{privdir}/bin
%{privdir}/bin/fprint-cs9711-diag
%dir %{privdir}/share
%dir %{privdir}/share/metainfo
%{privdir}/share/metainfo/org.freedesktop.libfprint.metainfo.xml

%files fprintd
%dir %{_unitdir}/fprintd.service.d
%{_unitdir}/fprintd.service.d/50-libfprint-cs9711.conf

%changelog
* Sun Nov 08 2026 libfprint-CS9711 maintainers - 1.94.10-0.3
- Acl16: consume the complete 9856-byte frame and retain the compatible image crop
- Follow scan/stop protocol without repeated initialization or fixed capture delays
- Add operation and frame-to-result timing to diagnostics

* Sun Nov 08 2026 libfprint-CS9711 maintainers - 1.94.10-0.2
- Acl16: keep the post-reset quiet period before the next action's init;
  fprintd enrollment stalled after its duplicate-check scan

* Sun Nov 08 2026 libfprint-CS9711 maintainers - 1.94.10-0.1
- Private build of the fork with Acl16 stabilization and fprint-cs9711-diag
