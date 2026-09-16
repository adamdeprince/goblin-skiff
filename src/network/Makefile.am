AM_CPPFLAGS = -I$(top_builddir)/ -I$(top_srcdir)/ $(protobuf_CFLAGS) $(ZSTD_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

noinst_LIBRARIES = libmoshnetwork.a

libmoshnetwork_a_SOURCES = network.cc network.h networktransport-impl.h networktransport.h transportfragment.cc transportfragment.h transportsender-impl.h transportsender.h transportstate.h compressor.cc compressor.h statesamples.cc statesamples.h bulkdatagram.cc bulkdatagram.h bulkcontrol.cc bulkcontrol.h
libmoshnetwork_a_SOURCES += linkbudget.h relay.h relay.cc socks5.h socks5.cc
libmoshnetwork_a_SOURCES += sessionversion.h sessionversion.cc
