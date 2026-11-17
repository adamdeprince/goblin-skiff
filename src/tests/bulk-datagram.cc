// Modified for Goblin Skiff on 2026-09-19.
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
#include "src/network/noticechannel.h"

#include <cstdlib>
#include <deque>
#include <vector>
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
    std::string path = std::string( getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" ) + "/gbq.XXXXXX";
    bool existed = getenv( "XDG_RUNTIME_DIR" );
    std::string previous = existed ? getenv( "XDG_RUNTIME_DIR" ) : "";
    Runtime() { require( mkdtemp( &path[0] ), "queue-test runtime directory" ); setenv( "XDG_RUNTIME_DIR", path.c_str(), 1 ); }
    ~Runtime() { if ( existed ) { setenv( "XDG_RUNTIME_DIR", previous.c_str(), 1 ); } else { unsetenv( "XDG_RUNTIME_DIR" ); } rmdir( path.c_str() ); }
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


/* Notifications cross on a channel of their own: every message once, whole
   and in order, whatever the path loses, repeats, reorders or shrinks. */
void notice_channel_test()
{
  using Network::NoticeChannel;
  using Network::Bulk::Datagram;
  uint32_t seed = 99;
  const auto random = [&]() { return ( seed = seed * 1664525u + 1013904223u ) >> 8; };

  std::vector<std::string> sent, got;
  NoticeChannel a, b;
  sent.push_back( "\033]9;build finished\033\\" );
  sent.push_back( std::string( 1, 'x' ) );
  std::string icon( 3 * NoticeChannel::SEGMENT_MAX + 12345, '\0' ); /* several segments */
  for ( char& c : icon ) { c = char( random() ); }
  sent.push_back( icon );
  sent.push_back( std::string( NoticeChannel::SEGMENT_MAX, 'e' ) ); /* exactly one */
  for ( int i = 0; i < 200; i++ ) { sent.push_back( "\033]99;i=" + std::to_string( i ) + ";n\033\\" ); }
  for ( const auto& message : sent ) { a.enqueue( message ); }
  a.enqueue( "" );
  require( !a.idle() && b.idle(), "queued at one end" );

  /* A path that drops a third of what it carries each way, repeats and
     reorders some, and whose datagrams change size as it goes. */
  std::deque<std::string> to_b, to_a;
  uint64_t now = 1000;
  size_t datagrams = 0, largest = 0;
  for ( unsigned step = 0; step < 4000000 && got.size() < sent.size(); step++ ) {
    const size_t mtu = step < 300 ? 1200 : step < 600 ? 160 : 500 + random() % 900;
    Datagram packet;
    for ( int burst = 0; burst < 4 && a.take_packet( now, 40, mtu, true, packet ); burst++ ) {
      const std::string wire = Network::Bulk::encode_datagram( packet );
      require( wire.size() <= mtu, "a datagram fits the path as it is when written" );
      largest = std::max( largest, wire.size() );
      datagrams++;
      if ( random() % 3 ) { to_b.push_back( wire ); }
      if ( random() % 17 == 0 ) { to_b.push_back( wire ); }
    }
    while ( b.take_packet( now, 40, mtu, true, packet ) ) {
      if ( random() % 3 ) { to_a.push_back( Network::Bulk::encode_datagram( packet ) ); }
    }
    for ( auto* queue : { &to_b, &to_a } ) {
      NoticeChannel& end = queue == &to_b ? b : a;
      for ( int n = 0; n < 6 && !queue->empty(); n++ ) {
        const size_t pick = random() % 5 == 0 ? random() % queue->size() : 0;
        Datagram arrived;
        require( Network::Bulk::decode_datagram( ( *queue )[pick], arrived ) && end.receive( arrived ),
                 "a notice datagram decodes and is the channel's" );
        queue->erase( queue->begin() + pick );
      }
    }
    std::string message;
    while ( b.take_message( message ) ) { got.push_back( message ); }
    now += 7;
  }
  require( got == sent, "every message arrived once, whole and in order" );
  for ( unsigned step = 0; step < 100000 && !( a.idle() && b.idle() ); step++ ) {
    Datagram packet;
    while ( a.take_packet( now, 40, 1200, true, packet ) ) { b.receive( packet ); }
    while ( b.take_packet( now, 40, 1200, true, packet ) ) { a.receive( packet ); }
    now += 50;
  }
  std::string extra;
  require( a.idle() && b.idle() && !b.take_message( extra ) && a.wait_time( now, true ) == INT_MAX,
           "and both ends come to rest" );
  require( largest > 1000 && datagrams > 3000, "the path was used as wide as it was" );

  /* Nothing is written to a peer that is not there, but what it sent is still acknowledged. */
  NoticeChannel quiet, other;
  quiet.enqueue( "hello" );
  Datagram packet;
  require( !quiet.take_packet( now, 40, 1200, false, packet ) && quiet.wait_time( now, false ) == INT_MAX
             && quiet.wait_time( now, true ) == 0,
           "a message waits for the peer to be heard from" );
  other.enqueue( "reply" );
  require( other.take_packet( now, 40, 1200, true, packet ) && quiet.receive( packet )
             && quiet.wait_time( now, false ) == 0 && quiet.take_packet( now, 40, 1200, false, packet )
             && packet.type == Network::Bulk::PacketType::NoticeAck && other.receive( packet ) && other.idle(),
           "an acknowledgment is always due" );
  /* A repeat is due after about two round trips, and then more patiently. */
  require( quiet.take_packet( now, 40, 1200, true, packet ) && !quiet.take_packet( now, 40, 1200, true, packet )
             && quiet.wait_time( now, true ) > 100 && quiet.wait_time( now, true ) <= 200
             && quiet.take_packet( now + 200, 40, 1200, true, packet ) && quiet.wait_time( now + 200, true ) > 200,
           "repeats back off" );
  /* Other datagrams are not this channel's, and nonsense is not taken in. */
  Datagram file;
  file.type = Network::Bulk::PacketType::FileSymbol;
  require( !quiet.receive( file ), "another channel's datagram" );
  Datagram bad;
  bad.type = Network::Bulk::PacketType::Notice;
  bad.transfer_id = 7;
  bad.block_id = 10;
  bad.symbol_id = 12;
  bad.payload = "toolong";
  NoticeChannel fresh;
  require( fresh.receive( bad ) && fresh.idle(), "bytes past the end of their segment are dropped" );
  bad.symbol_id = uint32_t( NoticeChannel::SEGMENT_MAX + 2 );
  require( fresh.receive( bad ) && fresh.idle(), "as is a segment longer than any is" );
}
}

int main( void )
{
  try {
    backpressure_test();
    notice_channel_test();
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
