AM_CPPFLAGS = -I$(top_srcdir)/ $(CRYPTO_CFLAGS) $(ZSTD_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)

bin_PROGRAMS = goblin-skiffcp goblin-skiff-compile-dictionary
noinst_LIBRARIES = libmoshcp.a

libmoshcp_a_SOURCES = protocol.cc protocol.h

goblin_skiffcp_SOURCES = moshcp.cc
goblin_skiffcp_LDADD = libmoshcp.a ../fec/libmoshfec.a ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS) $(ZSTD_LIBS)

goblin_skiff_compile_dictionary_SOURCES = mosh-compile-dictionary.cc
goblin_skiff_compile_dictionary_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS) $(ZSTD_LIBS)
