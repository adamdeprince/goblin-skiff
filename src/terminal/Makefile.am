AM_CPPFLAGS = -I$(top_srcdir)/ $(TINFO_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

noinst_LIBRARIES = libmoshterminal.a

libmoshterminal_a_SOURCES = parseraction.cc parseraction.h parser.cc parser.h parserstate.cc parserstatefamily.h parserstate.h parsertransition.h osc52.cc osc52.h kittygraphics.cc kittygraphics.h terminal.cc terminaldispatcher.cc terminaldispatcher.h terminaldisplay.cc terminaldisplayinit.cc terminaldisplay.h terminalframebuffer.cc terminalframebuffer.h terminalfunctions.cc terminalgeometry.h terminal.h terminaluserinput.cc terminaluserinput.h
libmoshterminal_a_SOURCES += sixel.cc sixel.h sixelstate.cc
libmoshterminal_a_SOURCES += djvu.cc djvu.h
libmoshterminal_a_SOURCES += sizedtext.cc sizedtext.h
libmoshterminal_a_SOURCES += grapheme.cc grapheme.h unicode16-data.h
EXTRA_DIST = gen-unicode16.py
libmoshterminal_a_SOURCES += osc5522.cc osc5522.h
libmoshterminal_a_SOURCES += download.cc download.h
