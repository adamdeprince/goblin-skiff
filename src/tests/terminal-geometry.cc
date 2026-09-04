/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/include/config.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <typeinfo>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/terminal/parseraction.h"
#include "src/terminal/terminalgeometry.h"
#include "src/util/pty_compat.h"

namespace {

void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

void test_geometry_event_round_trip( void )
{
  Network::UserStream empty;
  Network::UserStream sent;
  const Terminal::ClientGeometry geometry( 120, 40, 1200, 800, 10, 20 );
  sent.push_back( geometry );
  sent.push_back( Parser::Resize( 120, 40 ) );

  Network::UserStream received;
  received.apply_string( sent.diff_from( empty ) );

  require( received.size() == 2, "geometry and resize events survive serialization" );
  require( received.is_client_geometry_event( 0 ), "geometry event remains out-of-band" );
  require( received.get_client_geometry_event( 0 ) == geometry, "geometry values survive serialization" );
  const Parser::Action& resize_action = received.get_action( 1 );
  require( typeid( resize_action ) == typeid( Parser::Resize ), "logical resize follows geometry" );
  const Parser::Resize& resize = static_cast<const Parser::Resize&>( resize_action );
  require( resize.width == 120 && resize.height == 40, "logical resize values survive serialization" );
}

void test_xterm_geometry_queries( void )
{
  Terminal::Complete terminal( 120, 40 );
  const Terminal::Complete original( terminal );
  const Terminal::ClientGeometry geometry( 120, 40, 1200, 800, 10, 20 );

  require( terminal.act( "\033[14t", &geometry ) == "\033[4;800;1200t", "text-area pixel query" );
  require( terminal.act( "\033[16t", &geometry ) == "\033[6;20;10t", "cell pixel query" );
  require( terminal.act( "\033[18t", &geometry ) == "\033[8;40;120t", "text-area cell query" );
  require( terminal.act( "\033[14;2t", &geometry ).empty(), "unsupported window query variant" );
  require( terminal == original, "presentation geometry is not terminal state" );
}

void test_query_requires_attached_pixel_geometry( void )
{
  Terminal::Complete terminal( 80, 24 );
  require( terminal.act( "\033[16t" ).empty(), "cell query without attached pixel geometry" );
  const Terminal::ClientGeometry stale_geometry( 100, 30, 1000, 600, 10, 20 );
  require( terminal.act( "\033[16t", &stale_geometry ).empty(), "cell query with stale attachment geometry" );
  require( terminal.act( "\033[18t" ) == "\033[8;24;80t", "cell-grid query needs no pixel geometry" );
}

void test_query_reply_order( void )
{
  Terminal::Complete terminal( 80, 24 );
  const Terminal::ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  require( terminal.act( "\033[16t\033[6n", &geometry ) == "\033[6;20;10t\033[1;1R",
           "geometry reply preserves terminal response order" );
}

void run_remote_query_test( void )
{
  struct winsize size;
  require( ioctl( STDIN_FILENO, TIOCGWINSZ, &size ) == 0, "read remote PTY geometry" );
  require( size.ws_col == 80 && size.ws_row == 24, "remote PTY grid geometry" );
  require( size.ws_xpixel == 800 && size.ws_ypixel == 480, "remote PTY pixel geometry" );

  struct termios saved;
  require( tcgetattr( STDIN_FILENO, &saved ) == 0, "read remote PTY attributes" );
  struct termios raw = saved;
  cfmakeraw( &raw );
  require( tcsetattr( STDIN_FILENO, TCSANOW, &raw ) == 0, "put remote PTY in raw mode" );

  const std::string query( "\033[16t" );
  require( write( STDOUT_FILENO, query.data(), query.size() ) == static_cast<ssize_t>( query.size() ),
           "write cell-size query" );

  std::string reply;
  while ( reply.find( 't' ) == std::string::npos && reply.size() < 64 ) {
    fd_set readable;
    FD_ZERO( &readable );
    FD_SET( STDIN_FILENO, &readable );
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    require( select( STDIN_FILENO + 1, &readable, NULL, NULL, &timeout ) == 1, "cell-size query timeout" );
    char buf[64];
    const ssize_t bytes = read( STDIN_FILENO, buf, sizeof( buf ) );
    require( bytes > 0, "read cell-size reply" );
    reply.append( buf, static_cast<size_t>( bytes ) );
  }

  require( tcsetattr( STDIN_FILENO, TCSANOW, &saved ) == 0, "restore remote PTY attributes" );
  require( reply == "\033[6;20;10t", "remote cell-size reply" );
  std::cout << "terminal-geometry-query-ok" << std::endl;
}

}

int main( int argc, char* argv[] )
{
  try {
    if ( argc == 2 && std::string( argv[1] ) == "--remote-query" ) {
      run_remote_query_test();
      return 0;
    }
    test_geometry_event_round_trip();
    test_xterm_geometry_queries();
    test_query_requires_attached_pixel_geometry();
    test_query_reply_order();
  } catch ( const std::exception& e ) {
    std::cerr << "terminal-geometry: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
