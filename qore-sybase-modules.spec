# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
%global source_date_epoch_from_changelog 1
%global use_source_date_epoch_as_buildtime 1
%if v"%{rpmversion}" >= v"4.20"
%global build_mtime_policy clamp_to_source_date_epoch
%else
%global clamp_mtime_to_source_date_epoch 1
%endif
%bcond_without tests
%bcond_without docs
Name: qore-freetds-module
Version: 1.4
Release: 2%{?dist}
Summary: FreeTDS database driver for Qore
License: MIT OR LGPL-2.1-or-later
URL: https://github.com/qoretechnologies/module-sybase
Source0: %{name}-%{version}.tar.xz
BuildRequires: cmake >= 3.5
BuildRequires: make
BuildRequires: gcc-c++
BuildRequires: freetds-devel
BuildRequires: qore-devel >= 3.0.0~
BuildRequires: qore-rpm-macros >= 3.0.0~
%if %{with docs}
BuildRequires: doxygen
%if 0%{?suse_version}
BuildRequires: util-linux
%else
BuildRequires: util-linux-core
%endif
%endif

%description
Qore DBI driver for Microsoft SQL Server and Sybase databases using the
system FreeTDS CT-Library. Supports transactions, prepared statements,
large objects, cancellation and native bulk loading. A database server
and the proprietary Sybase OCS client are not installed by this package.

%if %{with docs}
%package doc
Summary: Reference documentation for Qore's FreeTDS database driver
BuildArch: noarch
%description doc
API reference and connection examples for Qore's FreeTDS database driver.
%endif

%prep
%autosetup
%build
%{?set_build_flags}
. %{_rpmconfigdir}/qore/module-env.sh
unset FreeTDS_INCLUDE_DIR FreeTDS_LIBS FREETDS FREETDSCONF TDSVER TDSDUMP TDSDUMPCONFIG SYBASE SYBASE_OCS
qore_set_source_prefix_maps "%{qore_debug_source_dir}"
cmake -S . -B build -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE=-DNDEBUG \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_SKIP_RPATH=ON -DCMAKE_IGNORE_PREFIX_PATH=/usr/local \
  -DQore_DIR=%{_libdir}/cmake/Qore -DQORE_EXECUTABLE=/usr/bin/qore \
  -DQORE_QPP_EXECUTABLE=/usr/bin/qpp \
  -DWITH_FREETDS=ON -DWITH_SYBASE=OFF -DQORE_GENERATE_JAVA_BINDINGS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=%{!?with_docs:ON}%{?with_docs:OFF}
cmake --build build -- %{?_smp_mflags}
%if %{with docs}
printf '\nWARN_AS_ERROR = FAIL_ON_WARNINGS\n' >> build/Doxyfile
cmake --build build --target docs -- %{?_smp_mflags}
%endif
%install
DESTDIR=%{buildroot} cmake --install build
chmod 755 %{buildroot}%{_libdir}/qore-modules/freetds-api-*.qmod
%if %{with docs}
install -d %{buildroot}%{_docdir}/%{name}-doc
cp -a build/docs/freetds/html %{buildroot}%{_docdir}/%{name}-doc/
hardlink -t -O %{buildroot}%{_docdir}/%{name}-doc
%endif
%check
%if %{with tests}
. %{_rpmconfigdir}/qore/module-env.sh
/usr/bin/python3 -B -W error rpm/test_fixture.py -v
/usr/bin/python3 -B -W error rpm/run-tests.py --build-dir "$PWD/build"
%endif
%files
%license COPYING.MIT COPYING.LGPL
%doc README RELEASE-NOTES AUTHORS
%{_libdir}/qore-modules/freetds-api-*.qmod
%if %{with docs}
%files doc
%license COPYING.MIT COPYING.LGPL
%doc %{_docdir}/%{name}-doc/
%endif
%changelog
* Fri Oct 02 2026 David Nichols <david@qore.org> - 1.4-2
- Build the FreeTDS driver with the packaged Qore SDK and distribution client.
- Run isolated offline driver checks and package strict reference documentation.
