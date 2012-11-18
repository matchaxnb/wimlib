Summary:   Library to extract, create, and modify WIM files
Name:      wimlib
Version:   1.1.0
Release:   1
License:   GPLv3+
Group:     System/Libraries
URL:       http://wimlib.sourceforge.net
Packager:  Eric Biggers <ebiggers3@gmail.com>
Source:    http://downloads.sourceforge.net/wimlib/wimlib-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

Requires: libxml2, openssl
BuildRequires: libxml2-devel, openssl-devel
%description
wimlib is a library that can be used to create, extract, and modify files in the
Windows Imaging Format. These files are normally created by the 'imagex.exe'
program on Windows, but this library provides a free implementation of 'imagex'
for UNIX-based systems. wimlib supports mounting WIM files, just like
imagex.exe.

%package devel
Summary:  Development files for wimlib
Group:    Development/Libraries
Requires: %{name} = %{version}-%{release}
%description devel
Development files for wimlib

%prep
%setup -q -n %{name}-%{version}

%build
%configure --prefix=/usr                 \
           --disable-rpath               \
	   --with-libcrypto              \
	   --without-ntfs-3g		 \
	   --without-fuse		 \
	   --disable-xattr               \
           --disable-verify-compression  \
	   --disable-custom-memory-allocator \
	   --disable-assertions
%__make %{?_smp_mflags}

%check
make check

%install
%__rm -rf %{buildroot}
%__make DESTDIR=%{buildroot} install

%clean
%__rm -rf %{buildroot}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%defattr(-, root, root)
%doc AUTHORS COPYING README TODO
%{_bindir}/imagex
%{_bindir}/mkwinpeimg
%{_libdir}/libwim.so
%{_libdir}/libwim.so.0
%{_libdir}/libwim.so.0.0.0
%doc %{_mandir}/man1/*.1.gz

%files devel
%defattr(-, root, root)
%{_libdir}/libwim.a
%{_libdir}/libwim.la
%{_includedir}/wimlib.h
%{_libdir}/pkgconfig/wimlib.pc
