AM_CPPFLAGS = -I$(top_srcdir)/ $(protobuf_CFLAGS) $(ZSTD_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

noinst_LIBRARIES = libmoshstatesync.a

libmoshstatesync_a_SOURCES = clipboard.cc clipboard.h completeterminal.cc completeterminal.h stream.cc stream.h user.cc user.h
