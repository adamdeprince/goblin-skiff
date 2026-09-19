// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/network/network.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

using Network::Relay::Cipher;
using Network::Relay::Chain;

static void require( bool condition, const char* message )
{
  if ( !condition ) { throw std::runtime_error( message ); }
}

template<class F> static void rejects( F operation )
{
  bool rejected = false;
  try { operation(); } catch ( const std::exception& ) { rejected = true; }
  require( rejected, "Invalid relay input accepted" );
}

static void codec( Crypto::Mode mode )
{
  const std::string key = Crypto::Base64Key( mode ).printable_key();
  Cipher client( key, mode, false ), server( key, mode, true ), wrong( Crypto::Base64Key( mode ).printable_key(), mode, true );
  bool newest = false;
  auto a = client.seal( "one" ), b = client.seal( "two" ), c = client.seal( "three" );
  rejects( [&] { wrong.open( a, newest ); } );
  rejects( [&] { client.open( a, newest ); } ); // direction / reflection
  auto damaged = a; damaged.back() ^= 1;
  rejects( [&] { server.open( damaged, newest ); } );
  for ( size_t i = 0; i < a.size(); ++i ) { rejects( [&] { server.open( a.substr( 0, i ), newest ); } ); }
  require( server.open( c, newest ) == "three" && newest, "New packet missing" );
  require( server.open( a, newest ) == "one" && !newest, "Reordering failed" );
  require( server.open( b, newest ) == "two" && !newest, "Reordering failed" );
  rejects( [&] { server.open( a, newest ); } );
  rejects( [&] { server.open( b, newest ); } );
  require( client.open( server.seal( "response" ), newest ) == "response" && newest, "Reply failed" );
  const auto old = client.seal( "old" );
  for ( unsigned i = 0; i < 1024; ++i ) { c = client.seal( "advance" ); }
  server.open( c, newest );
  rejects( [&] { server.open( old, newest ); } );
  require( server.open( client.seal( "" ), newest ).empty() && newest, "Close failed" );
  const auto max = std::string( Network::Relay::WIRE_MTU - Network::Relay::overhead( mode, 1 ), 'x' );
  require( client.seal( max ).size() == Network::Relay::WIRE_MTU, "Wrong wire overhead" );
  rejects( [&] { client.seal( max + "x" ); } );
  rejects( [&] { server.open( std::string( 65536, 'x' ), newest ); } );
  for ( unsigned count = 1; count <= 4; ++count ) {
    Chain chain;
    std::string keys;
    std::vector<std::unique_ptr<Cipher>> relays;
    for ( unsigned i = 0; i < count; ++i ) {
      const auto hop_key = Crypto::Base64Key( mode ).printable_key();
      if ( i ) { keys += ','; }
      keys += hop_key;
      relays.emplace_back( new Cipher( hop_key, mode, true ) );
    }
    chain.configure( keys, mode );
    const std::string payload( Network::Relay::WIRE_MTU - Network::Relay::overhead( mode, count ), 'z' );
    auto wire = chain.seal( payload );
    require( wire.size() == Network::Relay::WIRE_MTU, "Chain MTU budget wrong" );
    for ( auto& relay : relays ) { wire = relay->open( wire, newest ); }
    require( wire == payload, "Chain changed inner ciphertext" );
    for ( auto it = relays.rbegin(); it != relays.rend(); ++it ) { wire = ( *it )->seal( wire ); }
    require( chain.open( wire ) == payload, "Chain reply failed" );
    for ( unsigned close = count; close; --close ) {
      wire = chain.close_packet( close - 1 );
      for ( unsigned i = 0; i < close; ++i ) { wire = relays[i]->open( wire, newest ); }
      require( wire.empty() && newest, "Nested close failed" );
    }
    rejects( [&] { chain.configure( keys, mode ); } );
  }
  for ( const auto& invalid : { std::string(), key + ',', ',' + key, key + ",bad", key + ',' + key,
                               key + ',' + key + ',' + key + ',' + key + ',' + key } ) {
    rejects( [&] { Chain chain; chain.configure( invalid, mode ); } );
  }
  rejects( [&] { Network::Relay::overhead( mode, 5 ); } );
}

class UDP
{
public:
  int fd;
  Network::Addr addr {};
  socklen_t len;
  explicit UDP( bool ipv6 ) : fd( socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0 ) ),
                             len( ipv6 ? sizeof( sockaddr_in6 ) : sizeof( sockaddr_in ) ) {
    require( fd >= 0, "UDP socket" );
    if ( ipv6 ) { addr.sin6.sin6_family = AF_INET6; addr.sin6.sin6_addr = in6addr_loopback; }
    else { addr.sin.sin_family = AF_INET; addr.sin.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); }
    require( bind( fd, &addr.sa, len ) == 0, "UDP bind" );
    require( getsockname( fd, &addr.sa, &len ) == 0, "UDP name" );
  }
  UDP( const UDP& ) = delete;
  UDP& operator=( const UDP& ) = delete;
  ~UDP() { close( fd ); }
  unsigned port() const { return ntohs( addr.sa.sa_family == AF_INET6 ? addr.sin6.sin6_port : addr.sin.sin_port ); }
  void send( const std::string& data, const Network::Addr& destination, socklen_t length ) {
    require( sendto( fd, data.data(), data.size(), 0, &destination.sa, length ) == ssize_t( data.size() ), "UDP send" );
  }
  bool readable( int ms = 150 ) {
    pollfd item { fd, POLLIN, 0 };
    require( poll( &item, 1, ms ) >= 0, "UDP poll" );
    return item.revents & POLLIN;
  }
  std::string receive( Network::Addr& source, socklen_t& length ) {
    require( readable( 2000 ), "UDP response timed out" );
    char bytes[4096];
    length = sizeof( source );
    const auto n = recvfrom( fd, bytes, sizeof( bytes ), 0, &source.sa, &length );
    require( n >= 0, "UDP receive" );
    return std::string( bytes, n );
  }
};

class RelayProcess
{
  pid_t child = -1;
public:
  std::string key {};
  Network::Addr address {};
  socklen_t length = 0;
  explicit RelayProcess( const UDP& target, bool ipv6 ) {
    int pipefd[2];
    require( pipe( pipefd ) == 0, "relay pipe" );
    const auto port = std::to_string( target.port() );
    const char* server = getenv( "GOBLIN_SKIFF_TEST_SERVER" );
    if ( !server ) { server = "../frontend/goblin-skiff-server"; }
    child = fork();
    require( child >= 0, "relay fork" );
    if ( !child ) {
      close( pipefd[0] );
      dup2( pipefd[1], STDOUT_FILENO );
      close( pipefd[1] );
      // Do not inherit the test's target socket into the relay process.
      close( target.fd );
      execl( server, server, "relay", "--foreground", ipv6 ? "--bind=::1" : "--bind=127.0.0.1",
             "--port=0", "--startup-timeout=1", "--idle-timeout=2", ipv6 ? "::1" : "127.0.0.1", port.c_str(), (char*)nullptr );
      _exit( 127 );
    }
    close( pipefd[1] );
    pollfd item { pipefd[0], POLLIN, 0 };
    const int result = poll( &item, 1, 5000 );
    char banner[512] {};
    const auto n = result > 0 ? read( pipefd[0], banner, sizeof( banner ) - 1 ) : -1;
    close( pipefd[0] );
    if ( n <= 0 ) { kill( child, SIGTERM ); waitpid( child, nullptr, 0 ); child = -1; throw std::runtime_error( "Relay bootstrap failed" ); }
    std::string mosh, relay, version, mode, host;
    unsigned bound_port = 0;
    std::istringstream( banner ) >> mosh >> relay >> version >> mode >> host >> bound_port >> key;
    require( mosh == "MOSH" && relay == "RELAY" && version == "1" && mode == "ocb-aes128" && key.size() == 22, "Bad relay banner" );
    address = target.addr;
    length = target.len;
    if ( ipv6 ) { address.sin6.sin6_port = htons( bound_port ); }
    else { address.sin.sin_port = htons( bound_port ); }
  }
  RelayProcess( const RelayProcess& ) = delete;
  RelayProcess& operator=( const RelayProcess& ) = delete;
  ~RelayProcess() { if ( child > 0 ) { kill( child, SIGTERM ); waitpid( child, nullptr, 0 ); } }
  void expect_exit( unsigned timeout_ms ) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeout_ms );
    int status = 0;
    while ( waitpid( child, &status, WNOHANG ) == 0 ) {
      require( std::chrono::steady_clock::now() < end, "Relay did not expire/close" );
      poll( nullptr, 0, 10 );
    }
    child = -1;
    require( WIFEXITED( status ) && WEXITSTATUS( status ) == 0, "Relay failed" );
  }
};

static void live( bool ipv6 )
{
  UDP destination( ipv6 ), old_client( ipv6 ), new_client( ipv6 ), stranger( ipv6 );
  RelayProcess relay( destination, ipv6 );
  Cipher cipher( relay.key, Crypto::Mode::LegacyOCB, false );
  auto send = [&]( UDP& socket, const std::string& data ) { socket.send( data, relay.address, relay.length ); };
  Network::Addr egress {}, response_source {};
  socklen_t egress_len = 0, response_len = 0;
  bool newest = false;
  send( stranger, "not authenticated" );
  send( stranger, std::string( 4096, 'x' ) );
  require( !destination.readable(), "Unauthenticated traffic forwarded" );
  const auto first = cipher.seal( "first" );
  send( old_client, first );
  require( destination.receive( egress, egress_len ) == "first", "Forward changed datagram" );
  stranger.send( "spoofed reply", egress, egress_len );
  require( !old_client.readable(), "Reply accepted from wrong target" );
  destination.send( "reply", egress, egress_len );
  const auto reply = old_client.receive( response_source, response_len );
  require( cipher.open( reply, newest ) == "reply", "Return path failed" );
  send( old_client, reply ); // reflected server-direction ciphertext
  require( !destination.readable(), "Reflected packet accepted" );
  const auto delayed = cipher.seal( "delayed" );
  send( new_client, cipher.seal( "roaming" ) );
  require( destination.receive( egress, egress_len ) == "roaming", "Roaming input failed" );
  send( old_client, first );
  require( !destination.readable(), "Replayed packet forwarded" );
  send( old_client, delayed );
  require( destination.receive( egress, egress_len ) == "delayed", "Reordered input dropped" );
  destination.send( "roamed reply", egress, egress_len );
  require( cipher.open( new_client.receive( response_source, response_len ), newest ) == "roamed reply", "Roaming rolled back" );
  require( !old_client.readable(), "Old source received roaming reply" );
  const std::string full( Network::Relay::WIRE_MTU - Network::Relay::overhead( Crypto::Mode::LegacyOCB, 1 ), 'm' );
  send( new_client, cipher.seal( full ) );
  require( destination.receive( egress, egress_len ) == full, "Full-MTU forward failed" );
  destination.send( full, egress, egress_len );
  const auto full_reply = new_client.receive( response_source, response_len );
  require( full_reply.size() == Network::Relay::WIRE_MTU && cipher.open( full_reply, newest ) == full,
           "Full-MTU return failed" );
  send( new_client, cipher.seal( "" ) );
  relay.expect_exit( 1500 );
  RelayProcess unused( destination, ipv6 );
  unused.expect_exit( 2500 );
  RelayProcess idle( destination, ipv6 );
  Cipher idle_cipher( idle.key, Crypto::Mode::LegacyOCB, false );
  const auto valid = idle_cipher.seal( "start lease" );
  old_client.send( valid, idle.address, idle.length );
  require( destination.receive( egress, egress_len ) == "start lease", "Idle setup failed" );
  // Neither repeats nor target traffic renew the authenticated-client lease.
  for ( unsigned i = 0; i < 12; ++i ) {
    old_client.send( valid, idle.address, idle.length );
    destination.send( "unsolicited", egress, egress_len );
    poll( nullptr, 0, 100 );
  }
  idle.expect_exit( 1500 );
}

int main()
{
  try {
    codec( Crypto::Mode::LegacyOCB );
    Network::Connection server( "127.0.0.1", "0", true );
    Network::Connection client( server.get_key().c_str(), "127.0.0.1", server.port().c_str(), true );
    std::string keys;
    for ( unsigned i = 0; i < 4; ++i ) { if ( i ) { keys += ','; } keys += Crypto::Base64Key().printable_key(); }
    client.set_relay_keys( keys );
    server.set_relay_hops( 4 );
    require( client.get_MTU() == 1120 && server.get_MTU() == 1120, "Both directions must reserve relay overhead" );
    rejects( [&] { server.set_relay_hops( 5 ); } );
    live( false );
    live( true );
    bool fips = true;
    try { Crypto::ensure_mode_available( Crypto::Mode::FipsAES128GCM ); }
    catch ( const Crypto::CryptoException& ) { fips = false; }
    if ( fips ) { codec( Crypto::Mode::FipsAES128GCM ); }
    else { puts( "FIPS provider unavailable; relay FIPS roundtrip subtests skipped" ); }
    puts( "PASS: UDP relay authentication, direction, replay window, 1-4 hop framing, MTU, fixed target, IPv4/IPv6 roaming, close and expiry" );
    return 0;
  } catch ( const std::exception& error ) {
    fprintf( stderr, "FAIL: %s\n", error.what() );
    return 1;
  }
}
