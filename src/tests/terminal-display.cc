/*
    Mosh: the mobile shell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include "src/statesync/completeterminal.h"
#include "src/terminal/terminaldisplay.h"
#include "src/terminal/terminalframebuffer.h"

namespace {

void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

bool same_cells( const Terminal::Framebuffer& left, const Terminal::Framebuffer& right )
{
  if ( left.ds.get_width() != right.ds.get_width() || left.ds.get_height() != right.ds.get_height() ) {
    return false;
  }
  for ( int row = 0; row < left.ds.get_height(); row++ ) {
    for ( int col = 0; col < left.ds.get_width(); col++ ) {
      if ( *left.get_cell( row, col ) != *right.get_cell( row, col ) ) {
        return false;
      }
    }
  }
  return true;
}

std::string sparse_redraw_reproducer( void )
{
  std::string input = "\033[H\033[2JSlheqiwyo yk Kwk kj \033[32;1mEcdi\033[m (#850)"
                      "\033[26;40H\033[93;44;1m";
  for ( int i = 0; i < 14; i++ ) {
    input += "\302\240";
  }
  input += "\033[m\033[1;25r\033[25;1H\n\033[1;27r\033[27;1H\033[30;47m\033[K\033[m\r\n\033M"
           "\033[93Gu\r\n\033[30;47m \033[m\ru\033[30;47m \033[m\bu\033[30;47m \033[m\r"
           "waf\033[30;47m \033[m\rtihm\033[30;47m \033[m\b\b\033[30;47m \033[m\033[K\b\b"
           "\033[30;47m \033[m\033[K\ru\033[30;47m \033[m\033[K\r\033[30;47m \033[m\033[K"
           "\033[H\033M# *** yn pcr dfqqh \"dkvc\" ***\033[27;93H\033[?7l\033[30;47m \033[?7h"
           "\033[m\b\033[30;47m \033[m\033[K\r\033[30;47m \033[m\033[K\r";
  return input;
}

void test_sparse_redraw_does_not_invent_scroll( void )
{
  /* Regression for mobile-shell/mosh#1400: shared blank rows must not be
     mistaken for proof that the physical terminal scrolled. */
  const std::string input = sparse_redraw_reproducer();
  require( input.size() == 389, "sparse redraw fixture length" );

  Terminal::Complete source( 93, 27 );
  Terminal::Complete rendered( 93, 27 );
  Terminal::Framebuffer last( source.get_fb() );
  Terminal::Display display( false );

  static const size_t CHUNK_SIZE = 40;
  for ( size_t offset = 0; offset < input.size(); offset += CHUNK_SIZE ) {
    source.act( input.substr( offset, std::min( CHUNK_SIZE, input.size() - offset ) ) );
    const std::string update = display.new_frame( true, last, source.get_fb() );
    rendered.act( update );
    if ( !same_cells( source.get_fb(), rendered.get_fb() ) ) {
      throw std::runtime_error( "display update diverged after sparse redraw at byte "
                                + std::to_string( std::min( offset + CHUNK_SIZE, input.size() ) ) );
    }
    last = source.get_fb();
  }
}

void test_full_screen_scroll_uses_terminal_scroll( void )
{
  Terminal::Complete source( 8, 4 );
  Terminal::Complete rendered( 8, 4 );
  Terminal::Display display( false );

  source.act( "one\r\ntwo\r\nthree\r\nfour" );
  std::string update = display.new_frame( true, rendered.get_fb(), source.get_fb() );
  rendered.act( update );
  require( same_cells( source.get_fb(), rendered.get_fb() ), "initial display update" );

  const Terminal::Framebuffer last( source.get_fb() );
  source.act( "\r\nfive" );
  update = display.new_frame( true, last, source.get_fb() );
  require( update.find( "\r\n" ) != std::string::npos, "full-screen scroll was redrawn instead of scrolled" );
  rendered.act( update );
  require( same_cells( source.get_fb(), rendered.get_fb() ), "full-screen scroll display update" );
}

void test_first_frame_preserves_history( void )
{
  Terminal::Complete source( 8, 4 ), rendered( 8, 4 );
  const Terminal::Framebuffer blank( source.get_fb() );
  Terminal::Display display( false );
  Terminal::StartupScreen startup;
  rendered.act( "old shell\r\nchicken\r\n" );
  source.act( "remote$ " );
  const std::string first = display.new_frame( false, blank, source.get_fb(), &startup );
  require( first.find( "\033[2J" ) == std::string::npos && first.find( "\033[3J" ) == std::string::npos,
           "startup never erases the old viewport or scrollback" );
  require( first.find( "\033[4;1H\r\n\033[4;1H" ) != std::string::npos,
           "startup reserves only the row needed for the prompt" );
  rendered.act( first );
  require( rendered.get_fb().get_cell( 1, 0 )->debug_contents().find( "'c'" ) == 0, "chicken stays visible above prompt" );
  for ( int col = 0; col < 8; col++ ) {
    require( *source.get_fb().get_cell( 0, col ) == *rendered.get_fb().get_cell( 3, col ),
             "remote prompt appears below the local prefix" );
  }
  Terminal::Framebuffer last( source.get_fb() );
  source.act( "\r\nnext" );
  const std::string next = display.new_frame( true, last, source.get_fb(), &startup );
  rendered.act( next );
  require( rendered.get_fb().get_cell( 0, 0 )->debug_contents().find( "'c'" ) == 0, "output scrolls chicken up one row" );
  require( rendered.get_fb().get_cell( 2, 0 )->debug_contents().find( "'r'" ) == 0, "existing prompt scrolls with prefix" );
  require( rendered.get_fb().get_cell( 3, 0 )->debug_contents().find( "'n'" ) == 0, "new output appears below prompt" );
  last = source.get_fb();
  source.act( "\r\nthird\r\nfourth" );
  rendered.act( display.new_frame( true, last, source.get_fb(), &startup ) );
  require( same_cells( source.get_fb(), rendered.get_fb() ), "output eventually takes over the full viewport" );
  last = source.get_fb();
  source.act( "\r\nfifth" );
  rendered.act( display.new_frame( true, last, source.get_fb(), &startup ) );
  require( same_cells( source.get_fb(), rendered.get_fb() ), "ordinary full-screen scrolling after startup" );
  require( display.new_frame( false, blank, source.get_fb() ).find( "\033[2J" ) != std::string::npos,
           "ordinary forced repaints retain normal clear behavior" );
}

void test_startup_fullscreen_and_repaint( void )
{
  for ( const std::string& action : { std::string( "\033[4;1Hstatus" ), std::string( "\033[?1000h" ),
                                     std::string( "\033[2;4r" ), std::string( "repaint" ),
                                     std::string( "resize" ) } ) {
    Terminal::Complete source( 8, 4 ), rendered( 8, 4 );
    Terminal::Display display( false );
    Terminal::StartupScreen startup;
    const Terminal::Framebuffer blank( source.get_fb() );
    rendered.act( "chicken\r\n" );
    startup.set_cursor_row( rendered.get_fb().ds.get_cursor_row() );
    source.act( "$ " );
    rendered.act( display.new_frame( false, blank, source.get_fb(), &startup ) );
    const Terminal::Framebuffer last( source.get_fb() );
    if ( action == "resize" ) {
      source.act( Parser::Resize( 10, 5 ) );
      rendered.act( Parser::Resize( 10, 5 ) );
    } else if ( action != "repaint" ) {
      source.act( action );
    }
    const std::string update = display.new_frame( action != "repaint", last, source.get_fb(), &startup );
    require( update.find( "\033[2J" ) == std::string::npos, "prefix is scrolled out before taking full viewport" );
    rendered.act( update );
    require( same_cells( source.get_fb(), rendered.get_fb() ), "full viewport takeover preserves remote cells" );
    require( source.get_fb().ds.get_cursor_row() == rendered.get_fb().ds.get_cursor_row(),
             "full viewport takeover preserves remote cursor coordinates" );
  }
}

void test_startup_at_actual_cursor( void )
{
  // A fresh or half-used window must not put the prompt at the bottom and
  // insert a screenful of blank space after the image. Bottom-of-window
  // startup must still grow into scrollback one row at a time.
  for ( int height : { 4, 24, 48 } ) {
    for ( int origin = 0; origin < height; origin++ ) {
      Terminal::Complete source( 16, height ), rendered( 16, height );
      Terminal::Display display( false );
      Terminal::StartupScreen startup;
      for ( int row = 0; row < origin; row++ ) { rendered.act( "local banner\r\n" ); }
      startup.set_cursor_row( rendered.get_fb().ds.get_cursor_row() );
      Terminal::Framebuffer last( source.get_fb() );
      source.act( "remote$ " );
      for ( int line = 0; line < height + 3; line++ ) {
        if ( line ) { source.act( "\r\nline " + std::to_string( line ) ); }
        const auto update = display.new_frame( line != 0, last, source.get_fb(), &startup );
        if ( !line ) {
          require( update.find( "\033[" + std::to_string( height ) + ";1H\r\n" ) == std::string::npos,
                   "first prompt consumes a free row, not a scroll" );
        }
        rendered.act( update );
        const int offset = std::min( origin, height - source.get_fb().ds.get_cursor_row() - 1 );
        require( rendered.get_fb().ds.get_cursor_row() == source.get_fb().ds.get_cursor_row() + offset,
                 "fresh-window cursor follows the banner without a blank gap" );
        for ( int row = 0; row < offset; row++ ) {
          require( rendered.get_fb().get_cell( row, 0 )->debug_contents().find( "'l'" ) == 0,
                   "local prefix only scrolls when remote output needs the space" );
        }
        for ( int row = 0; row < height - offset; row++ ) {
          for ( int col = 0; col < 16; col++ ) {
            require( *source.get_fb().get_cell( row, col ) == *rendered.get_fb().get_cell( row + offset, col ),
                     "remote rows render correctly while consuming the unused viewport" );
          }
        }
        // A late CPR after fallback/attachment must not reposition a session.
        startup.set_cursor_row( height - 1 );
        last = source.get_fb();
      }
    }
  }
}

void test_startup_chunked_rendering( void )
{
  /* Exercise native layout independently of the fixed-origin e2e harness:
     wrapping, colored erases, cursor motion, sparse redraws and scrolling. */
  const std::string input = "prompt$ 1234567890\r\n\033[31;44mcolored\033[K\033[m\r\n"
                            "\033[2;1Hreplace\033[K\033[4;1Hfour\r\nfive\r\nsix\r\n"
                            "\033[H\033[2Jreset\r\n";
  for ( size_t chunk = 1; chunk <= input.size(); chunk++ ) {
    Terminal::Complete source( 8, 6 ), rendered( 8, 6 );
    Terminal::Display display( false );
    Terminal::StartupScreen startup;
    rendered.act( "local\r\nchicken\r\n" );
    startup.set_cursor_row( rendered.get_fb().ds.get_cursor_row() );
    Terminal::Framebuffer last( source.get_fb() );
    for ( size_t pos = 0; pos < input.size(); pos += chunk ) {
      source.act( input.substr( pos, chunk ) );
      rendered.act( display.new_frame( pos != 0, last, source.get_fb(), &startup ) );
      const int offset = rendered.get_fb().ds.get_cursor_row() - source.get_fb().ds.get_cursor_row();
      require( offset >= 0 && offset < 6, "startup cursor has a valid physical offset" );
      for ( int row = 0; row < 6 - offset; row++ ) {
        for ( int col = 0; col < 8; col++ ) {
          require( *source.get_fb().get_cell( row, col ) == *rendered.get_fb().get_cell( row + offset, col ),
                   "chunked startup output matches the remote cells at its physical offset" );
        }
      }
      last = source.get_fb();
    }
  }
}

}

int main( void )
{
  try {
    test_sparse_redraw_does_not_invent_scroll();
    test_full_screen_scroll_uses_terminal_scroll();
    test_first_frame_preserves_history();
    test_startup_fullscreen_and_repaint();
    test_startup_at_actual_cursor();
    test_startup_chunked_rendering();
  } catch ( const std::exception& error ) {
    std::cerr << "terminal-display: " << error.what() << std::endl;
    return 1;
  }
  return 0;
}
