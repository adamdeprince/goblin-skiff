EXTRA_DIST = wrap-compiler-for-flag-check mosh.pl
if BUILD_CLIENT
  bin_SCRIPTS = goblin-skiff
endif
CLEANFILES = $(bin_SCRIPTS)

goblin-skiff:	mosh.pl ../VERSION.stamp Makefile
	perl -Mdiagnostics -c $(srcdir)/mosh.pl
	@sed -e "s/\@VERSION\@/`cat ../VERSION.stamp`/" -e "s/\@PACKAGE_STRING\@/@PACKAGE_STRING@/" -e "s/\@GOBLIN_VERSION\@/@GOBLIN_VERSION@/" $(srcdir)/mosh.pl > goblin-skiff
	@chmod a+x goblin-skiff
