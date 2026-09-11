AM_CPPFLAGS = -I$(top_srcdir)/ $(CRYPTO_CFLAGS) $(ZSTD_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)

bin_PROGRAMS = goblin-moshcp goblin-mosh-compile-dictionary
noinst_LIBRARIES = libmoshcp.a

libmoshcp_a_SOURCES = protocol.cc protocol.h

goblin_moshcp_SOURCES = moshcp.cc
goblin_moshcp_LDADD = libmoshcp.a ../fec/libmoshfec.a ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS) $(ZSTD_LIBS)

goblin_mosh_compile_dictionary_SOURCES = mosh-compile-dictionary.cc
goblin_mosh_compile_dictionary_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS) $(ZSTD_LIBS)
