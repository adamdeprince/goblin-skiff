AM_CPPFLAGS = -I$(top_builddir)/ -I$(top_srcdir)/ $(TINFO_CFLAGS) $(protobuf_CFLAGS) $(CRYPTO_CFLAGS) $(ZSTD_CFLAGS) $(FILECRYPTO_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)
LDADD = ../crypto/libmoshcrypto.a ../network/libmoshnetwork.a ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a -lm $(TINFO_LIBS) $(protobuf_LIBS) $(CRYPTO_LIBS) $(ZSTD_LIBS)

bin_PROGRAMS =

if BUILD_CLIENT
  bin_PROGRAMS += goblin-skiff-client
endif

if BUILD_SERVER
  bin_PROGRAMS += goblin-skiff-server
endif

noinst_LIBRARIES = libmoshcontrol.a libmoshmascot.a
libmoshcontrol_a_SOURCES = directory.cc directory.h controlpanel.cc controlpanel.h
libmoshcontrol_a_SOURCES += mimeclipboard.cc mimeclipboard.h
libmoshcontrol_a_SOURCES += download.cc download.h
libmoshcontrol_a_SOURCES += downloadforward.cc downloadforward.h
libmoshcontrol_a_SOURCES += filetransfer.cc filetransfer.h
libmoshmascot_a_SOURCES = mascot.cc mascot.h
goblin_skiff_client_SOURCES = mosh-client.cc stmclient.cc stmclient.h streamforward.cc streamforward.h terminaloverlay.cc terminaloverlay.h
goblin_skiff_server_SOURCES = mosh-server.cc streamforward.cc streamforward.h relay-server.cc relay-server.h
goblin_skiff_client_LDADD = libmoshcontrol.a libmoshmascot.a $(LDADD) ../fec/libmoshfec.a $(LIBRSYNC_LIBS) $(FILECRYPTO_LIBS)
goblin_skiff_server_LDADD = libmoshcontrol.a $(LDADD) ../fec/libmoshfec.a $(LIBRSYNC_LIBS) $(FILECRYPTO_LIBS)

EXTRA_DIST = goblin.webp embed-mascot.pl
BUILT_SOURCES = goblin-webp.inc
CLEANFILES = goblin-webp.inc
goblin-webp.inc: $(srcdir)/goblin.webp $(srcdir)/embed-mascot.pl
	$(AM_V_GEN)perl $(srcdir)/embed-mascot.pl $(srcdir)/goblin.webp > $@.tmp && mv $@.tmp $@
