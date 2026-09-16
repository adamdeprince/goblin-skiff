EXTRA_DIST = wrap-compiler-for-flag-check mosh.pl
if BUILD_CLIENT
  bin_SCRIPTS = goblin-mosh
endif
CLEANFILES = $(bin_SCRIPTS)

goblin-mosh:	mosh.pl ../VERSION.stamp Makefile
	perl -Mdiagnostics -c $(srcdir)/mosh.pl
	@sed -e "s/\@VERSION\@/`cat ../VERSION.stamp`/" -e "s/\@PACKAGE_STRING\@/@PACKAGE_STRING@/" -e "s/\@GOBLIN_VERSION\@/@GOBLIN_VERSION@/" $(srcdir)/mosh.pl > goblin-mosh
	@chmod a+x goblin-mosh
