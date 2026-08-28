/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "src/statesync/completeterminal.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/osc52.h"
#include "src/terminal/terminaldisplay.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

std::string rgb_cell( unsigned char r, unsigned char g, unsigned char b )
{
  std::string raw;
  raw.push_back( static_cast<char>( r ) );
  raw.push_back( static_cast<char>( g ) );
  raw.push_back( static_cast<char>( b ) );
  return raw;
}

std::string transmit_rgb( uint32_t id )
{
  const std::string b64 = Terminal::base64_encode_osc52( rgb_cell( 255, 0, 0 ) );
  return std::string( "\033_Ga=T,f=24,s=1,v=1,i=" ) + std::to_string( id ) + ";" + b64 + "\033\\";
}

void test_parse_command( void )
{
  Terminal::KittyCommand cmd;
  require( Terminal::parse_kitty_command( "Ga=T,f=24,s=1,v=1,i=7;/wAA", cmd ), "parse failed" );
  require( cmd.action == 'T', "action" );
  require( cmd.format == 24, "format" );
  require( cmd.width == 1 && cmd.height == 1, "size" );
  require( cmd.image_id == 7, "id" );
  require( cmd.payload.size() == 3, "decoded rgb" );
  require( static_cast<unsigned char>( cmd.payload[0] ) == 255, "red" );
}

void test_query_reply( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string reply = term.act( std::string( "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\" ) );
  require( reply.find( "\033_Gi=31;OK\033\\" ) != std::string::npos, "query should reply OK" );
  require( term.get_fb().get_kitty_images().empty(), "query must not store an image" );
}

void test_transmit_and_place( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string reply = term.act( transmit_rgb( 3 ) );
  require( reply.find( ";OK" ) != std::string::npos, "transmit reply" );
  require( term.get_fb().get_kitty_images().size() == 1, "one image stored" );
  require( term.get_fb().find_kitty_image( 3 ) != NULL, "id 3 present" );
  require( term.get_fb().get_kitty_placements().size() == 1, "one placement" );
  require( term.get_fb().get_kitty_placements()[0].image_id == 3, "placement id" );
  require( term.get_fb().get_kitty_placements()[0].row == 0, "placed at cursor row" );
}

void test_chunked_transmit( void )
{
  Terminal::Complete term( 80, 24 );
  std::string raw = rgb_cell( 1, 2, 3 );
  raw += rgb_cell( 4, 5, 6 );
  const std::string b64 = Terminal::base64_encode_osc52( raw );
  require( b64.size() >= 8, "base64 length" );
  std::string first = "\033_Ga=t,f=24,s=2,v=1,i=9,m=1;" + b64.substr( 0, 4 ) + "\033\\";
  std::string second = "\033_Gm=0;" + b64.substr( 4 ) + "\033\\";
  term.act( first );
  require( term.get_fb().get_kitty_images().empty(), "incomplete upload is not stored" );
  term.act( second );
  const Terminal::KittyImage* img = term.get_fb().find_kitty_image( 9 );
  require( img != NULL, "chunked image stored" );
  require( img->data && *img->data == raw, "chunked payload" );
}

void test_delete( void )
{
  Terminal::Complete term( 80, 24 );
  term.act( transmit_rgb( 4 ) );
  term.act( std::string( "\033_Ga=d,d=I,i=4\033\\" ) );
  require( term.get_fb().get_kitty_placements().empty(), "placement deleted" );
  require( term.get_fb().find_kitty_image( 4 ) == NULL, "image data freed" );
}

void test_display_emits_once( void )
{
  Terminal::Complete empty( 80, 24 );
  Terminal::Complete with_image( 80, 24 );
  with_image.act( transmit_rgb( 5 ) );

  Terminal::Display display( false );
  const std::string first = display.new_frame( true, empty.get_fb(), with_image.get_fb() );
  require( first.find( "\033_G" ) != std::string::npos, "first frame emits kitty APC" );
  require( first.find( "a=t" ) != std::string::npos, "first frame transmits image" );
  require( first.find( "q=2" ) != std::string::npos, "emitted commands are quiet" );

  const std::string second = display.new_frame( true, with_image.get_fb(), with_image.get_fb() );
  require( second.find( "\033_G" ) == std::string::npos, "unchanged image is not retransmitted" );
}

void test_scroll_removes_offscreen_placement( void )
{
  Terminal::Complete term( 80, 24 );
  term.act( transmit_rgb( 6 ) );
  require( !term.get_fb().get_kitty_placements().empty(), "placed" );
  term.act( std::string( "\033[1;1H\033[M" ) ); /* delete line 1 */
  require( term.get_fb().get_kitty_placements().empty(), "scrolled-off placement removed" );
}

void test_state_diff_once( void )
{
  Terminal::Complete src( 80, 24 );
  src.act( transmit_rgb( 8 ) );
  Terminal::Complete blank( 80, 24 );
  const std::string wire = src.diff_from( blank );
  require( !wire.empty(), "state diff carries image" );

  Terminal::Complete dst( 80, 24 );
  dst.apply_string( wire );
  require( dst.get_fb().find_kitty_image( 8 ) != NULL, "client state has image" );
  require( !dst.get_fb().get_kitty_placements().empty(), "client state has placement" );

  Terminal::Display display( false );
  const std::string visual = display.new_frame( true, dst.get_fb(), src.get_fb() );
  require( visual.find( "a=t" ) == std::string::npos, "acked image is not resent in display" );
}
}

int main( void )
{
  try {
    test_parse_command();
    test_query_reply();
    test_transmit_and_place();
    test_chunked_transmit();
    test_delete();
    test_scroll_removes_offscreen_placement();
    test_display_emits_once();
    test_state_diff_once();
  } catch ( const std::exception& e ) {
    std::cerr << "kitty-graphics: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
