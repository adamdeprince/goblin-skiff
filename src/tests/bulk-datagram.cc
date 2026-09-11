/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "src/network/bulkdatagram.h"
#include "src/network/bulkcontrol.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

void backpressure_test()
{
  struct Runtime {
    char path[40] = "/tmp/goblin-bulk-queue.XXXXXX";
    bool existed = getenv( "XDG_RUNTIME_DIR" );
    std::string previous = existed ? getenv( "XDG_RUNTIME_DIR" ) : "";
    Runtime() { require( mkdtemp( path ), "queue-test runtime directory" ); setenv( "XDG_RUNTIME_DIR", path, 1 ); }
    ~Runtime() { if ( existed ) { setenv( "XDG_RUNTIME_DIR", previous.c_str(), 1 ); } else { unsetenv( "XDG_RUNTIME_DIR" ); } rmdir( path ); }
  } runtime;
  Network::Bulk::ControlServer server( "test" );
  Network::Bulk::ControlClient client( server.socket_path() );
  require( fcntl( client.fd(), F_SETFL, O_NONBLOCK ) == 0, "nonblocking test producer" );
  const int listener = server.fds()[0];
  server.process_readable_fd( listener );
  std::string stream;
  for ( unsigned i = 0; i < 200; ++i ) {
    Network::Bulk::Datagram packet; packet.symbol_id = i; packet.payload.assign( 64, 'x' );
    auto wire = Network::Bulk::encode_datagram( packet );
    for ( int shift = 24; shift >= 0; shift -= 8 ) { stream += char( wire.size() >> shift ); }
    stream += wire;
  }
  size_t written = 0;
  unsigned received = 0;
  bool blocked = false;
  for ( unsigned turn = 0; turn < 1000 && received < 200; ++turn ) {
    if ( written < stream.size() ) {
      const auto count = write( client.fd(), stream.data() + written, stream.size() - written );
      if ( count > 0 ) { written += count; }
    }
    for ( int fd : server.fds() ) { if ( fd != listener ) { server.process_readable_fd( fd ); } }
    blocked = blocked || server.fds().size() == 1;
    Network::Bulk::Datagram packet;
    if ( server.pop_outgoing( packet ) ) {
      require( packet.symbol_id == received++, "backpressure preserves datagram order" );
    }
  }
  require( blocked && received == 200 && written == stream.size(), "bounded queue backpressures and resumes an unlimited producer" );
}
}

int main( void )
{
  try {
    backpressure_test();
    Network::Bulk::Datagram original;
    original.type = Network::Bulk::PacketType::Symbol;
    original.transfer_id = 0x0102030405060708ULL;
    original.block_id = 17;
    original.symbol_id = 23;
    original.payload.assign( "abc\0def", 7 );

    const std::string wire = Network::Bulk::encode_datagram( original );

    Network::Bulk::Datagram decoded;
    require( Network::Bulk::decode_datagram( wire, decoded ), "bulk datagram did not decode" );
    require( decoded.type == original.type, "bulk datagram type mismatch" );
    require( decoded.transfer_id == original.transfer_id, "bulk datagram transfer id mismatch" );
    require( decoded.block_id == original.block_id, "bulk datagram block id mismatch" );
    require( decoded.symbol_id == original.symbol_id, "bulk datagram symbol id mismatch" );
    require( decoded.payload == original.payload, "bulk datagram payload mismatch" );

    std::string corrupted = wire;
    corrupted[0] = 'X';
    require( !Network::Bulk::decode_datagram( corrupted, decoded ), "bulk datagram accepted bad magic" );

    corrupted = wire.substr( 0, wire.size() - 1 );
    require( !Network::Bulk::decode_datagram( corrupted, decoded ), "bulk datagram accepted truncated payload" );
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
