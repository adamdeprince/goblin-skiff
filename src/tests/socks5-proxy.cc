// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/network/network.h"
#include "src/util/timestamp.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

static void require( bool condition, const char* message )
{
  if ( !condition ) { throw std::runtime_error( message ); }
}

template<class F> static void rejects( F f, const char* input = "(binary)" )
{
  bool rejected = false;
  try { f(); } catch ( const std::exception& ) { rejected = true; }
  if ( !rejected ) { throw std::runtime_error( std::string( "Invalid SOCKS5 input accepted: " ) + input ); }
}

static void codec()
{
  using Network::Socks5UDP;
  for ( const auto& host : { "127.0.0.1", "::1", "[::1]", "only-in-tailnet.invalid" } ) {
    const auto address = Socks5UDP::address( host, "60001" );
    require( Socks5UDP::address_length( address, 0 ) == address.size(), "Address length" );
    require( address.substr( address.size() - 2 ) == std::string( "\xea\x61", 2 ), "Port byte order" );
    for ( size_t n = 0; n < address.size(); ++n ) {
      require( !Socks5UDP::address_length( address.substr( 0, n ), 0 ), "Truncated address accepted" );
    }
  }
  for ( const auto& proxy : { "127.0.0.1:1055", "localhost:1055", "[::1]:1055" } ) { Socks5UDP::validate_proxy( proxy ); }
  for ( const auto& bad : { "", "http://localhost:1055", "user@localhost:1055", "::1:1055", "localhost:0", "a:65536", "a:1x", ":1055", "[bad]:1055" } ) {
    rejects( [&] { Socks5UDP::validate_proxy( bad ); }, bad );
  }
  for ( const auto& bad : { "", "0", "65536", "1x", "-1" } ) { rejects( [&] { Socks5UDP::address( "host", bad ); }, bad ); }
  for ( const auto& bad : { "", "bad host", "host/path", "host;command", "fe80::1%en0" } ) {
    rejects( [&] { Socks5UDP::address( bad, "1" ); }, bad );
  }
  rejects( [&] { Socks5UDP::address( std::string( 254, 'a' ), "1" ); } );
  rejects( [&] { Socks5UDP::address_length( std::string( 7, '\0' ), 0 ); } );
  rejects( [&] { Socks5UDP::address_length( std::string( "\3\0\0\0", 4 ), 0 ); } );
}

static std::string receive( Network::Connection& connection )
{
  try { return connection.recv(); }
  catch ( const Network::NetworkException& e ) {
    if ( e.the_errno && e.the_errno != EAGAIN && e.the_errno != EWOULDBLOCK && e.the_errno != ECONNREFUSED ) { throw; }
    return {};
  }
}

static void exercise( const std::string& proxy, const char* host )
{
  const bool ipv6 = std::string( host ) == "::1";
  Network::Connection server( ipv6 ? "::1" : "127.0.0.1", "0", true );
  Network::Connection client( server.get_key().c_str(), host, server.port().c_str(), true,
                              Crypto::Mode::LegacyOCB, proxy );
  require( client.get_MTU() == 1216, "Proxy MTU must reserve IPv6 overhead" );
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds( 12 );
  int phase = 0;
  uint64_t last_send = 0;
  std::string message;
  auto started = std::chrono::steady_clock::now();
  while ( std::chrono::steady_clock::now() < until && phase < 20 ) {
    freeze_timestamp();
    client.tick(); server.tick();
    const auto now = Network::timestamp();
    if ( message.empty() ) {
      message = std::to_string( phase ) + ":";
      message.resize( client.get_MTU() - client.packet_overhead(), static_cast<char>( phase ) );
    }
    if ( now - last_send >= 50 ) { client.send( message ); last_send = now; }
    auto inbound = receive( server );
    if ( !inbound.empty() ) {
      const auto prior = std::stoi( inbound.substr( 0, inbound.find( ':' ) ) );
      auto expected = std::to_string( prior ) + ":";
      expected.resize( client.get_MTU() - client.packet_overhead(), static_cast<char>( prior ) );
      // UDP may deliver retransmissions of earlier phases. Validate their
      // contents, but do not confuse an old packet with the current exchange.
      require( prior >= 0 && prior <= phase && inbound == expected, "Corrupt upload" );
      server.send( inbound );
    }
    if ( receive( client ) == message ) { ++phase; message.clear(); }
    std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
  }
  require( phase == 20, "Encrypted session did not recover/finish through SOCKS5" );
  // A ready, idle association must not introduce a short polling/keepalive loop.
  freeze_timestamp(); client.tick();
  require( client.keepalive_wait_time() > 10000, "Idle proxy polling wastes battery" );
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
  printf( "PASS: 20 maximum-size authenticated bidirectional exchanges via %s in %ld ms\n", host, static_cast<long>( elapsed ) );
}

int main( int argc, char** argv )
{
  try {
    codec();
    if ( argc == 3 ) { exercise( argv[1], argv[2] ); }
    else { require( argc == 1, "Expected [PROXY HOST]" ); puts( "PASS: SOCKS5 addresses, lengths and invalid inputs" ); }
    return 0;
  } catch ( const std::exception& error ) {
    fprintf( stderr, "FAIL: %s\n", error.what() ); return 1;
  }
}
