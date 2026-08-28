EXTRA_DIST = wrap-compiler-for-flag-check mosh.pl
if BUILD_CLIENT
  bin_SCRIPTS = adam-mosh
endif
CLEANFILES = $(bin_SCRIPTS)

adam-mosh:	mosh.pl ../VERSION.stamp Makefile
	perl -Mdiagnostics -c $(srcdir)/mosh.pl
	@sed -e "s/\@VERSION\@/`cat ../VERSION.stamp`/" -e "s/\@PACKAGE_STRING\@/@PACKAGE_STRING@/" $(srcdir)/mosh.pl > adam-mosh
	@chmod a+x adam-mosh
