Name:           libvarlink
Version:        1
Release:        1%{?dist}
Summary:        Varlink C Library
License:        ASL2.0
URL:            https://github.com/varlink/%{name}
Source0:        https://github.com/varlink/%{name}/archive/%{name}-%{version}.tar.gz
BuildRequires:  meson
BuildRequires:  gcc

%description
Varlink C Library

%package        devel
Summary:        Development files for %{name}
Requires:       %{name} = %{version}-%{release}

%description    devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.

%prep
%setup -q

%build
%meson
%meson_build

%check
export LC_CTYPE=C.utf8
%meson_test

%install
%meson_install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%license LICENSE
%{_libdir}/libvarlink.so.*
%{_bindir}/varlink
%{_datadir}/bash-completion/completions/varlink
%{_datadir}/vim/vimfiles/after/*

%files devel
%{_includedir}/varlink.h
%{_libdir}/libvarlink.so
%{_libdir}/pkgconfig/libvarlink.pc

%changelog
* Tue Aug 29 2017 <info@varlink.org> 1-1
- libvarlink 1
