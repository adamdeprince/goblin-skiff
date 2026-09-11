/* Distributed under the GNU GPL, version 3 or later. */
#include <clocale>
#include <iostream>
#include <stdexcept>
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/terminal/grapheme.h"

using namespace Terminal;
namespace {
#include "unicode16-tests.h"
void require( bool ok, const std::string& message ) { if ( !ok ) { throw std::runtime_error( message ); } }

void unicode()
{
  size_t number = 0;
  for ( const auto* fixture : break_tests ) {
    std::wstring text;
    std::vector<std::wstring> expected;
    for ( const auto* c = fixture; *c; c++ ) {
      if ( *c == 0xfdd0 ) { if ( !text.empty() ) { expected.push_back( text ); text.clear(); } }
      else { text += *c; }
    }
    for ( const auto& cluster : expected ) { text += cluster; }
    require( Unicode16::graphemes( text ) == expected, "Unicode 16 grapheme test " + std::to_string( ++number ) );
  }
  require( number > 1000, "full Unicode 16 conformance corpus" );
  require( Unicode16::cell_width( L"\u2764\ufe0f" ) == 2 && Unicode16::cell_width( L"\u231a\ufe0e" ) == 1,
           "emoji/text presentation selectors" );
  Complete t( 40, 10 ), receiver( 40, 10 ), blank( 40, 10 );
  const std::string unicode_text = "\033]66;s=2;A\xcc\x81\xe2\x9d\xa4\xef\xb8\x8f"
    "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\007";
  t.act( unicode_text );
  require( t.get_fb().ds.get_cursor_col() == 14, "accent, VS16, flag, and ZWJ footprint" );
  receiver.apply_string( t.diff_from( blank ) );
  require( receiver.get_fb().ds.get_cursor_col() == 14, "Unicode OSC 66 wire cursor" );
}

void same_screen( const Complete& a, const Complete& b, size_t position )
{
  const auto& left = a.get_fb();
  const auto& right = b.get_fb();
  for ( int y = 0; y < left.ds.get_height(); y++ ) {
    for ( int x = 0; x < left.ds.get_width(); x++ ) {
      const auto* l = left.get_cell( y, x );
      const auto* r = right.get_cell( y, x );
      const bool equivalent_blank = l->is_blank() && r->is_blank() && l->get_renditions() == r->get_renditions()
                                    && l->get_hyperlink() == r->get_hyperlink() && l->get_wrap() == r->get_wrap();
      if ( *l != *r && !equivalent_blank ) {
        throw std::runtime_error( "state mismatch byte " + std::to_string( position ) + " cell "
                                  + std::to_string( x ) + "," + std::to_string( y ) + " "
                                  + l->debug_contents() + " vs " + r->debug_contents()
                                  + " sized " + ( l->get_sized_text() ? l->get_sized_text()->sequence() : "none" )
                                  + " vs " + ( r->get_sized_text() ? r->get_sized_text()->sequence() : "none" )
                                  + " wrap " + std::to_string( l->get_wrap() ) + "/" + std::to_string( r->get_wrap() ) );
      }
    }
  }
  require( left.ds.get_cursor_row() == right.ds.get_cursor_row()
             && left.ds.get_cursor_col() == right.ds.get_cursor_col(), "cursor state mismatch" );
}

void text_sizing()
{
  Complete t( 20, 10 );
  require( t.act( "\033]66;s=2:w=2:n=1:d=2:v=2:h=1;Wide\007\033[6n" ) == "\033[1;5R", "OSC 66 cursor advance" );
  const auto text = t.get_fb().get_cell( 0, 0 )->get_sized_text();
  require( text && text->scale == 2 && text->width == 2 && text->denominator == 2, "OSC 66 metadata" );
  require( t.get_fb().get_cell( 1, 3 )->get_sized_text() == text, "whole block occupancy" );
  t.act( "\033[2;1Hx" );
  require( t.get_fb().get_cell( 1, 4 )->debug_contents().find( "'x'" ) == 0, "lower row skips block" );
  t.act( "\033[1;2Hz" );
  require( !t.get_fb().get_cell( 1, 3 )->get_sized_text(), "overwrite erases whole block" );
  t.act( "\033[H\033]66;s=3;ab\033\\" );
  require( t.get_fb().ds.get_cursor_col() == 6, "w=0 creates scaled graphemes" );
  t.act( "\033[2;2H\033[X" );
  require( !t.get_fb().get_cell( 0, 0 )->get_sized_text() && t.get_fb().get_cell( 0, 3 )->get_sized_text(),
           "ECH erases intersecting block only" );
  t.act( "\033[H\033]66;s=7:w=7;oversized\007" );
  require( t.get_fb().ds.get_cursor_col() == 0, "oversized block rejected without moving cursor" );

  const std::string input = "head\r\n\033]66;s=2:w=2:n=1:d=2:v=1:h=2;Wide\033\\plain"
    "\033[3;1Hx\033[5;1H\033]66;s=3;ABC\007\033[6;4H\033[X"
    "\033[1S\033[2;1H\033[L\033[3;1H\033[M\033[2;1H\033[@\033[P"
    "\033[9;1H\033]66;s=3:w=2;bottom\007\r\nlast\033[H\033[K\033[2Jdone"
    "\033[H\033[2J\033]66;s=2;abcdefghij\007Z\033[2J\033[H12345678901234567890"
    "\033]66;s=3:w=2;wrap\007\033[9;20H\033[?7l\033]66;s=2:w=2;back\007\033[?7h";
  for ( size_t chunk : { 1U, 2U, 7U, 19U, 67U } ) {
    Complete source( 20, 10 ), receiver( 20, 10 ), lost( 20, 10 ), blank( 20, 10 );
    Complete previous( source );
    for ( size_t pos = 0; pos < input.size(); pos += chunk ) {
      source.act( input.substr( pos, chunk ) );
      receiver.apply_string( source.diff_from( previous ) );
      same_screen( source, receiver, pos );
      // A newly attached/loss-recovering receiver must reconstruct the same
      // state without receiving any of the preceding OSC sequences.
      lost = blank;
      lost.apply_string( source.diff_from( blank ) );
      same_screen( source, lost, pos );
      previous = source;
    }
  }
  Complete sized( 20, 10 ), blank( 20, 10 );
  sized.act( "\033]66;s=2:w=3;readable\007" );
  Display fallback( false );
  fallback.set_graphics( ClientGraphics(), true );
  const auto plain = fallback.new_frame( true, blank.get_fb(), sized.get_fb() );
  require( plain.find( "\033]66;" ) == std::string::npos && plain.find( "readab" ) != std::string::npos,
           "unsupported terminal receives bounded ordinary text" );
}

void keyboard()
{
  Complete source( 20, 10 ), receiver( 20, 10 ), blank( 20, 10 );
  require( source.act( "\033[?u" ).empty(), "do not advertise keyboard before client probe" );
  source.set_keyboard_enabled( true );
  require( source.act( "\033[>27u\033[?u" ) == "\033[?27u", "application push/query" );
  receiver.apply_string( source.diff_from( blank ) );
  require( receiver.get_fb().ds.kitty_keyboard_flags == 27, "keyboard mode survives state sync" );
  Display display( false );
  display.set_graphics( ClientGraphics( true, false, true ), true );
  require( display.new_frame( true, blank.get_fb(), receiver.get_fb() ).find( "\033[=27u" ) != std::string::npos,
           "client sets effective mode, never replays pushes on retransmission" );
  require( display.new_frame( true, receiver.get_fb(), receiver.get_fb() ).empty(), "unchanged keyboard mode is free" );
  require( source.act( "\033[=4;2u\033[?u\033[=8;3u\033[?u" ) == "\033[?31u\033[?23u", "set/or/and-not" );
  require( source.act( "\033[?1049h\033[?u\033[>8u\033[?1049l\033[?u" ) == "\033[?0u\033[?23u", "independent alternate-screen mode" );
  require( source.act( "\033[?1049h\033[?u\033[<u\033[?u\033[?1049l\033[<u\033[?u" )
             == "\033[?8u\033[?0u\033[?0u", "independent alternate-screen stacks" );
  require( display.open().find( "\033[>0u" ) != std::string::npos && display.close().find( "\033[<u" ) != std::string::npos,
           "local mode stack balanced across suspend/exit" );
  const std::string keys = "\033f\033[57443;3:1u\033[57443;1:3u\033[57449;3:1u\033[57449;1:3u";
  Network::UserStream input, received, empty;
  for ( char c : keys ) { input.push_back( Parser::UserByte( c ) ); }
  received.apply_string( input.diff_from( empty ) );
  std::string output;
  for ( unsigned i = 0; i < received.size(); i++ ) { output += source.act( received.get_action( i ) ); }
  require( output == keys, "Option-as-Alt and standalone Alt press/release preserved exactly" );
}
}

int main()
{
  std::setlocale( LC_ALL, "" );
  try { unicode(); text_sizing(); keyboard(); }
  catch ( const std::exception& e ) { std::cerr << "terminal-extensions: " << e.what() << '\n'; return 1; }
  std::cout << "OSC 66 state and Kitty keyboard tests passed\n";
}
