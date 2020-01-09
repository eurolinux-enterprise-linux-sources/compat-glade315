%global _changelog_trimtime %(date +%s -d "1 year ago")

Name:           compat-glade315
Version:        3.15.0
Release:        1%{?dist}
Summary:        Compat package with glade 3.15 libraries

# - /usr/bin/glade is GPLv2+
# - /usr/bin/glade-previewer is LGPLv2+
# - libgladeui-2.so, libgladegtk.so, and libgladepython.so all combine
#   GPLv2+ and LGPLv2+ code, so the resulting binaries are GPLv2+
License:        GPLv2+ and LGPLv2+
URL:            http://glade.gnome.org/
Source0:        http://ftp.gnome.org/pub/GNOME/sources/glade/3.15/glade-%{version}.tar.xz

BuildRequires:  desktop-file-utils
BuildRequires:  gettext
BuildRequires:  gtk3-devel
BuildRequires:  intltool
BuildRequires:  itstool
BuildRequires:  libxml2-devel
BuildRequires:  pygobject3-devel
BuildRequires:  python2-devel
BuildRequires:  docbook-style-xsl
BuildRequires:  libxslt

# Explicitly conflict with older glade packages that ship libraries
# with the same soname as this compat package
Conflicts: glade-libs < 3.16

%description
Compatibility package with glade 3.15 librarires.

%prep
%setup -q -n glade-%{version}

%build
%configure --disable-static

# Omit unused direct shared library dependencies.
sed -i -e 's/ -shared / -Wl,-O1,--as-needed\0/g' libtool

make %{?_smp_mflags}

%install
%make_install
find $RPM_BUILD_ROOT -type f -name "*.la" -delete

rm -rf $RPM_BUILD_ROOT%{_bindir}
rm -rf $RPM_BUILD_ROOT%{_includedir}
rm -rf $RPM_BUILD_ROOT%{_libdir}/girepository-1.0/
rm -rf $RPM_BUILD_ROOT%{_libdir}/glade/
rm -rf $RPM_BUILD_ROOT%{_libdir}/pkgconfig/
rm -rf $RPM_BUILD_ROOT%{_libdir}/*.so
rm -rf $RPM_BUILD_ROOT%{_datadir}

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%license COPYING*
%{_libdir}/libgladeui-2.so.*

%changelog
* Wed Oct 19 2016 Kalev Lember <klember@redhat.com> - 3.15.0-1
- Initial glade 3.15 compat package
