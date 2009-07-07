Summary: Valgrind Memory Debugger
Name: valgrind
Version: 3.5.0.SVN
Release: 1
Epoch: 1
License: GPL
URL: http://www.valgrind.org/
Group: Development/Debuggers
Packager: Jeremy Fitzhardinge <jeremy@goop.org>
Source: valgrind-3.5.0.SVN.tar.bz2

Buildroot: %{_tmppath}/%{name}-root

%description 

Valgrind is an award-winning suite of tools for debugging and profiling
Linux programs. With the tools that come with Valgrind, you can
automatically detect many memory management and threading bugs, avoiding
hours of frustrating bug-hunting, making your programs more stable. You can
also perform detailed profiling, to speed up and reduce memory use of your
programs.

The Valgrind distribution currently includes five tools: two memory error
detectors, a thread error detector, a cache profiler and a heap profiler.

%prep
%setup -n valgrind-3.5.0.SVN

%build
%configure
make

%install
make install DESTDIR=$RPM_BUILD_ROOT

%makeinstall
mkdir docs.installed
mv $RPM_BUILD_ROOT%{_datadir}/doc/valgrind/* docs.installed/

%files
%defattr(-,root,root)
%doc AUTHORS COPYING FAQ.txt INSTALL NEWS README*
%doc docs.installed/html/*.html docs.installed/html/images/*.png
%{_bindir}/*
%{_includedir}/valgrind
%{_libdir}/valgrind
%{_libdir}/pkgconfig/*

%doc
%defattr(-,root,root)
%{_mandir}/*/*

%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf ${RPM_BUILD_ROOT}
