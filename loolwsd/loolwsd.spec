#
# spec file for package loolwsd
#
# Copyright (c) 2015 Collabora
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.


# See also http://en.opensuse.org/openSUSE:Specfile_guidelines

Name:           loolwsd
Version:        1.0.12
Release:        0
Vendor:         Collabora
Summary:        LibreOffice On-Line WebSocket Daemon
License:        MPL
Source0:        loolwsd-1.0.12.tar.gz
BuildRequires:  libcap-progs libcap-devel libpng-devel poco-devel >= 1.6.0
Requires:       libcap libcap-progs libpng libPocoFoundation30 >= 1.6.0 libPocoNet30 >= 1.6.0

%define owner lool
%define group lool

# This works for now only with the TDF nightly builds or similar, I think
%define sofficeversion 5.0
%define sofficepackage libreofficedev%{sofficeversion}

%description

%prep
%setup -q

%build
%configure --with-lokit-path=bundled/include

make %{?_smp_mflags}

%check
make check

%install
make install DESTDIR=%{buildroot}

%files
/usr/bin/loolwsd
/usr/bin/loolwsd-systemplate-setup

%doc README

%post
setcap cap_fowner,cap_sys_chroot=ep /usr/bin/loolwsd

getent group %{group} >/dev/null || groupadd -r %{group}
getent passwd %{owner} >/dev/null || useradd -g %{group} -m -r %{owner}

mkdir -p /var/cache/loolwsd && chmod og+w /var/cache/loolwsd

su - %{owner} -c 'mkdir lool-child-roots'

# Figure out where LO is installed
loroot=`rpm -ql %{sofficepackage} | grep '/soffice$' | sed -e 's-/program/soffice--'`

su - %{owner} -c "loolwsd-systemplate-setup /home/lool/lool-systemplate ${loroot} >/dev/null"

%changelog
* Tue May 19 2015 Tor Lillqvist
- Initial RPM release
