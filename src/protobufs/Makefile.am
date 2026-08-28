source = clipboard.proto stream.proto userinput.proto hostinput.proto transportinstruction.proto

AM_CPPFLAGS = $(protobuf_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS) -Wno-error

SUFFIXES = .proto .pb.cc

.proto.pb.cc:
	$(AM_V_GEN)$(PROTOC) --cpp_out=. -I$(srcdir) $<

userinput.pb.cc: stream.pb.cc clipboard.pb.cc
hostinput.pb.cc: stream.pb.cc clipboard.pb.cc

noinst_LIBRARIES = libmoshprotos.a

libmoshprotos_a_SOURCES = $(source)
nodist_libmoshprotos_a_SOURCES = $(source:.proto=.pb.cc) $(source:.proto=.pb.h)

BUILT_SOURCES = $(source:.proto=.pb.cc)
CLEANFILES = $(source:.proto=.pb.cc) $(source:.proto=.pb.h)
