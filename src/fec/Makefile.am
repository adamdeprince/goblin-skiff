AM_CPPFLAGS = -I$(top_srcdir)/ $(RAPTORQ_CPPFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(RAPTORQ_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)

noinst_LIBRARIES = libmoshfec.a

libmoshfec_a_SOURCES = fec.cc fec.h raptorq.cc raptorq.h reedsolomon.cc reedsolomon.h
