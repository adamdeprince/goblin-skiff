// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "relay-server.h"
#include "src/network/network.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;

class FD
{
  int value = -1;
public:
  FD() = default;
  FD( const FD& ) = delete;
  FD& operator=( const FD& ) = delete;
  ~FD() { if ( value >= 0 ) { close( value ); } }
  int get() const { return value; }
  void open( int family ) {
    value = socket( family, SOCK_DGRAM, 0 );
    if ( value < 0 || fcntl( value, F_SETFL, O_NONBLOCK ) < 0 || fcntl( value, F_SETFD, FD_CLOEXEC ) < 0 ) {
      throw Network::NetworkException( "relay socket", errno );
    }
  }
};

struct Address
{
  Network::Addr addr {};
  socklen_t len = 0;
  Address( const std::string& ip, const std::string& port ) {
    addrinfo hints {}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    const int error = getaddrinfo( ip.c_str(), port.c_str(), &hints, &result );
    if ( error ) { throw std::runtime_error( std::string( "relay address: " ) + gai_strerror( error ) ); }
    len = result->ai_addrlen;
    if ( len > sizeof( addr ) ) { freeaddrinfo( result ); throw std::runtime_error( "relay address too large" ); }
    memcpy( &addr, result->ai_addr, len );
    freeaddrinfo( result );
  }
  void port( unsigned number ) {
    if ( addr.sa.sa_family == AF_INET ) { addr.sin.sin_port = htons( number ); }
    else { addr.sin6.sin6_port = htons( number ); }
  }
};

unsigned positive_number( const char* text )
{
  if ( !*text || strspn( text, "0123456789" ) != strlen( text ) ) {
    throw std::invalid_argument( "Expected a positive integer" );
  }
  char* end = nullptr;
  errno = 0;
  const auto result = strtoul( text, &end, 10 );
  if ( errno || *end || !result || result > UINT_MAX ) { throw std::invalid_argument( "Invalid integer" ); }
  return result;
}

std::string ssh_bind_address()
{
  const char* connection = getenv( "SSH_CONNECTION" );
  std::string client, client_port, ip;
  if ( connection ) { std::istringstream( connection ) >> client >> client_port >> ip; }
  if ( ip.empty() ) { throw std::runtime_error( "relay requires SSH_CONNECTION or --bind=IP" ); }
  if ( !ip.compare( 0, 7, "::ffff:" ) ) { ip.erase( 0, 7 ); }
  return ip;
}

void announce( const FD& listener, const Crypto::Base64Key& key, Crypto::Mode mode, pid_t pid )
{
  Network::Addr address {};
  socklen_t len = sizeof( address );
  char host[NI_MAXHOST], port[NI_MAXSERV];
  if ( getsockname( listener.get(), &address.sa, &len ) < 0
       || getnameinfo( &address.sa, len, host, sizeof( host ), port, sizeof( port ),
                       NI_NUMERICHOST | NI_NUMERICSERV ) ) {
    throw std::runtime_error( "Cannot identify UDP relay listener" );
  }
  printf( "MOSH RELAY 1 %s %s %s %s\n", Crypto::mode_name( mode ), host, port, key.printable_key().c_str() );
  fprintf( stderr, "[goblin-mosh UDP relay, pid = %ld]\n", static_cast<long>( pid ) );
  fflush( nullptr );
}

void serve( const FD& ingress, const FD& egress, Network::Relay::Cipher& cipher,
            unsigned startup_timeout, unsigned idle_timeout )
{
  Network::Addr client {};
  socklen_t client_len = 0;
  auto last_client = Clock::now();
  for ( ;; ) {
    const auto now = Clock::now();
    const auto lease = std::chrono::seconds( client_len ? idle_timeout : startup_timeout );
    if ( now - last_client >= lease ) { return; }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>( lease - ( now - last_client ) ).count();
    pollfd ready[] = { { ingress.get(), POLLIN, 0 }, { egress.get(), POLLIN, 0 } };
    if ( poll( ready, 2, static_cast<int>( std::min<int64_t>( 1000, remaining ) ) ) < 0 ) {
      if ( errno == EINTR ) { continue; }
      throw Network::NetworkException( "relay poll", errno );
    }
    // Fixed read quanta per direction. No user-space send queue: a full
    // socket drops packets, leaving recovery and pacing to the Mosh peers.
    for ( unsigned side = 0; side < 2; ++side ) {
      if ( !( ready[side].revents & ( POLLIN | POLLERR ) ) ) { continue; }
      for ( unsigned packets = 0; packets < 64; ++packets ) {
        char bytes[Network::Relay::WIRE_MTU + 1];
        Network::Addr source {};
        socklen_t source_len = sizeof( source );
        const ssize_t size = recvfrom( ready[side].fd, bytes, sizeof( bytes ), 0, &source.sa, &source_len );
        if ( size < 0 ) { break; }
        if ( size > Network::Relay::WIRE_MTU ) { continue; }
        try {
          if ( side == 0 ) {
            bool newest = false;
            const auto inner = cipher.open( std::string( bytes, size ), newest );
            // Reordered data is useful, but must never roll a roaming client
            // back to an old address or allow a replay to renew its lease.
            if ( newest ) {
              if ( inner.empty() ) { return; } // authenticated close
              client = source;
              client_len = source_len;
              last_client = Clock::now();
            }
            if ( !inner.empty() ) { (void)send( egress.get(), inner.data(), inner.size(), 0 ); }
          } else if ( client_len && size > 0 ) {
            // egress is connected: the kernel accepts replies only from the
            // destination authorized at SSH setup. No on-wire destination.
            const auto outer = cipher.seal( std::string( bytes, size ) );
            (void)sendto( ingress.get(), outer.data(), outer.size(), 0, &client.sa, client_len );
          }
        } catch ( const Crypto::CryptoException& error ) {
          if ( error.fatal ) { throw; }
          // Silent discard: unauthenticated packets must not create replies,
          // log amplification, client migration, or a longer relay lifetime.
        }
      }
    }
  }
}
} // namespace

int relay_server_main( int argc, char* argv[] )
{
  try {
    Crypto::Mode mode = Crypto::Mode::LegacyOCB;
    std::string bind_ip, ports = "60001:60999";
    unsigned startup_timeout = Network::Relay::STARTUP_TIMEOUT, idle_timeout = Network::Relay::IDLE_TIMEOUT;
    bool foreground = false, check = false;
    const option options[] = {
      { "bind", required_argument, nullptr, 'i' }, { "port", required_argument, nullptr, 'p' },
      { "fips-crypto", no_argument, nullptr, 'f' }, { "foreground", no_argument, nullptr, 'v' },
      { "check", no_argument, nullptr, 'c' }, { "help", no_argument, nullptr, 'h' },
      { "idle-timeout", required_argument, nullptr, 't' },
      { "startup-timeout", required_argument, nullptr, 's' }, { nullptr, 0, nullptr, 0 }
    };
    int opt;
    while ( ( opt = getopt_long( argc, argv, "", options, nullptr ) ) != -1 ) {
      switch ( opt ) {
        case 'i': bind_ip = optarg; break;
        case 'p': ports = optarg; break;
        case 'f': mode = Crypto::Mode::FipsAES128GCM; break;
        case 'v': foreground = true; break;
        case 'c': check = true; break;
        case 's': startup_timeout = positive_number( optarg ); break;
        case 't': idle_timeout = positive_number( optarg ); break;
        case 'h':
          puts( "Usage: goblin-mosh-server relay [--bind=IP] [--port=PORT[:PORT2]] [--fips-crypto]\n"
                "       [--foreground] [--idle-timeout=SECONDS] [--startup-timeout=SECONDS] TARGET_IP TARGET_PORT\n"
                "       goblin-mosh-server relay --check [--fips-crypto]" );
          return 0;
        default: throw std::invalid_argument( "Invalid relay option (see relay --help)" );
      }
    }
    Crypto::ensure_mode_available( mode );
    if ( check && argc == optind ) { printf( "MOSH RELAY SUPPORT 1 %s\n", Crypto::mode_name( mode ) ); return 0; }
    if ( check || argc - optind != 2 ) { throw std::invalid_argument( "relay requires TARGET_IP TARGET_PORT" ); }
    const unsigned target_port = positive_number( argv[optind + 1] );
    if ( target_port > 65535 ) { throw std::invalid_argument( "Invalid target port" ); }
    if ( startup_timeout > 3600 || idle_timeout > 604800 || ( !foreground && idle_timeout < 1800 ) ) {
      throw std::invalid_argument( "Relay startup timeout must be <=3600; idle timeout 1800..604800 seconds" );
    }
    if ( bind_ip.empty() ) { bind_ip = ssh_bind_address(); }
    Address bind( bind_ip, "0" ), target( argv[optind], argv[optind + 1] );
    FD ingress, egress;
    ingress.open( bind.addr.sa.sa_family );
    egress.open( target.addr.sa.sa_family );
    int low = 0, high = 0;
    if ( ports.empty() || ports.back() == ':' || ports.front() == ':'
         || ports.find_first_not_of( "0123456789:" ) != std::string::npos
         || !Network::Connection::parse_portrange( ports.c_str(), low, high ) ) {
      throw std::invalid_argument( "Invalid relay port range" );
    }
    if ( high < 0 ) { high = low; }
    bool bound = false;
    for ( int port = low; port <= high; ++port ) {
      bind.port( port );
      if ( ::bind( ingress.get(), &bind.addr.sa, bind.len ) == 0 ) { bound = true; break; }
    }
    if ( !bound ) { throw Network::NetworkException( "relay bind", errno ); }
    if ( connect( egress.get(), &target.addr.sa, target.len ) < 0 ) {
      throw Network::NetworkException( "relay connect", errno );
    }
    Crypto::Base64Key key( mode );
    Network::Relay::Cipher cipher( key.printable_key(), mode, true );
    if ( !foreground ) {
      signal( SIGHUP, SIG_IGN );
      signal( SIGPIPE, SIG_IGN );
      fflush( nullptr );
      const pid_t child = fork();
      if ( child < 0 ) { throw Network::NetworkException( "relay fork", errno ); }
      if ( child > 0 ) { announce( ingress, key, mode, child ); return 0; }
      if ( setsid() < 0 ) { _exit( 1 ); }
      const int null = open( "/dev/null", O_RDWR );
      if ( null < 0 ) { _exit( 1 ); }
      for ( int fd = 0; fd <= 2; ++fd ) { if ( dup2( null, fd ) < 0 ) { _exit( 1 ); } }
      if ( null > 2 ) { close( null ); }
    } else { announce( ingress, key, mode, getpid() ); }
    serve( ingress, egress, cipher, startup_timeout, idle_timeout );
    return 0;
  } catch ( const std::exception& error ) {
    fprintf( stderr, "goblin-mosh UDP relay: %s\n", error.what() );
    return 1;
  }
}
