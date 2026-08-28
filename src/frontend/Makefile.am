AM_CPPFLAGS = -I$(top_srcdir)/ $(TINFO_CFLAGS) $(protobuf_CFLAGS) $(CRYPTO_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)
LDADD = ../crypto/libmoshcrypto.a ../network/libmoshnetwork.a ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a -lm $(TINFO_LIBS) $(protobuf_LIBS) $(CRYPTO_LIBS) $(ZSTD_LIBS)

bin_PROGRAMS =

if BUILD_CLIENT
  bin_PROGRAMS += adam-mosh-client
endif

if BUILD_SERVER
  bin_PROGRAMS += adam-mosh-server
endif

adam_mosh_client_SOURCES = mosh-client.cc stmclient.cc stmclient.h streamforward.cc streamforward.h terminaloverlay.cc terminaloverlay.h
adam_mosh_server_SOURCES = mosh-server.cc streamforward.cc streamforward.h
adam_mosh_server_LDADD = $(LDADD)
