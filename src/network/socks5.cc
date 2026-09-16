// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/include/config.h"
#include "socks5.h"
#include "network.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#include <unistd.h>

namespace {
unsigned port_number( const std::string& port )
{
  if ( port.empty() || port.size() > 5 || port.find_first_not_of( "0123456789" ) != std::string::npos ) {
    throw std::invalid_argument( "SOCKS5: expected a port in 1..65535" );
  }
  const auto value = std::stoul( port );
  if ( !value || value > 65535 ) { throw std::invalid_argument( "SOCKS5: expected a port in 1..65535" ); }
  return static_cast<unsigned>( value );
}

std::pair<std::string, std::string> split_proxy( const std::string& proxy )
{
  const auto colon = proxy.rfind( ':' );
  if ( colon == std::string::npos ) { throw std::invalid_argument( "SOCKS5: expected HOST:PORT or [IPv6]:PORT" ); }
  auto host = proxy.substr( 0, colon );
  const auto port = proxy.substr( colon + 1 );
  port_number( port );
  if ( !host.empty() && host.front() == '[' && host.back() == ']' ) {
    host = host.substr( 1, host.size() - 2 );
    in6_addr ip {};
    if ( host.find( '%' ) != std::string::npos || inet_pton( AF_INET6, host.c_str(), &ip ) != 1 ) { throw std::invalid_argument( "SOCKS5: invalid IPv6 proxy" ); }
  } else if ( host.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" ) != std::string::npos ) {
    throw std::invalid_argument( "SOCKS5: expected HOST:PORT (no URL, credentials or unbracketed IPv6)" );
  }
  if ( host.empty() || host.size() > 253 ) { throw std::invalid_argument( "SOCKS5: invalid proxy host" ); }
  return { host, port };
}

int open_socket( int family, int type )
{
  const int fd = socket( family, type, 0 );
  if ( fd < 0 ) { throw Network::NetworkException( "SOCKS5 socket", errno ); }
  if ( fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0 || fcntl( fd, F_SETFL, O_NONBLOCK ) < 0 ) {
    const int saved = errno;
    close( fd );
    throw Network::NetworkException( "SOCKS5 nonblocking socket", saved );
  }
#ifdef SO_NOSIGPIPE
  int on = 1;
  if ( setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof( on ) ) < 0 ) {
    const int saved = errno;
    close( fd );
    throw Network::NetworkException( "SOCKS5 SO_NOSIGPIPE", saved );
  }
#endif
  return fd;
}

bool again() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

ssize_t safe_send( int fd, const void* data, size_t length )
{
  return ::send( fd, data, length,
#ifdef MSG_NOSIGNAL
                 MSG_NOSIGNAL
#else
                 0
#endif
  );
}
}

void Network::Socks5UDP::validate_proxy( const std::string& proxy ) { (void)split_proxy( proxy ); }

std::string Network::Socks5UDP::address( const std::string& raw_host, const std::string& port )
{
  const auto number = port_number( port );
  auto host = raw_host;
  if ( host.size() > 2 && host.front() == '[' && host.back() == ']' ) { host = host.substr( 1, host.size() - 2 ); }
  if ( host.find( '%' ) != std::string::npos ) { throw std::invalid_argument( "SOCKS5: scoped IPv6 destinations are not supported" ); }
  std::string result;
  in_addr ipv4 {};
  in6_addr ipv6 {};
  if ( inet_pton( AF_INET, host.c_str(), &ipv4 ) == 1 ) {
    result = '\1' + std::string( reinterpret_cast<const char*>( &ipv4 ), sizeof( ipv4 ) );
  } else if ( inet_pton( AF_INET6, host.c_str(), &ipv6 ) == 1 ) {
    result = '\4' + std::string( reinterpret_cast<const char*>( &ipv6 ), sizeof( ipv6 ) );
  } else {
    if ( host.empty() || host.size() > 253
         || host.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" ) != std::string::npos ) {
      throw std::invalid_argument( "SOCKS5: invalid destination hostname or IP address" );
    }
    result = '\3' + std::string( 1, static_cast<char>( host.size() ) ) + host;
  }
  result += static_cast<char>( number >> 8 );
  result += static_cast<char>( number & 255 );
  return result;
}

size_t Network::Socks5UDP::address_length( const std::string& bytes, size_t offset )
{
  if ( bytes.size() <= offset ) { return 0; }
  size_t length;
  switch ( static_cast<unsigned char>( bytes[offset] ) ) {
    case 1: length = 7; break;
    case 4: length = 19; break;
    case 3:
      if ( bytes.size() <= offset + 1 ) { return 0; }
      length = 4 + static_cast<unsigned char>( bytes[offset + 1] );
      if ( length == 4 ) { throw std::runtime_error( "SOCKS5: empty address" ); }
      break;
    default: throw std::runtime_error( "SOCKS5: unsupported address type" );
  }
  return bytes.size() - offset >= length ? length : 0;
}

Network::Socks5UDP::Socks5UDP( const std::string& proxy, const std::string& host, const std::string& port )
  : destination( address( host, port ) )
{
  const auto endpoint = split_proxy( proxy );
  addrinfo hints {}, *addresses = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  // Only the proxy is resolved locally, once at startup. Destinations are
  // encoded verbatim and resolved by the proxy (including MagicDNS names).
  const int error = getaddrinfo( endpoint.first.c_str(), endpoint.second.c_str(), &hints, &addresses );
  if ( error ) { throw std::runtime_error( std::string( "SOCKS5 proxy lookup: " ) + gai_strerror( error ) ); }
  for ( auto item = addresses; item; item = item->ai_next ) {
    if ( ( item->ai_family != AF_INET && item->ai_family != AF_INET6 ) || item->ai_addrlen > sizeof( sockaddr_storage ) ) { continue; }
    Endpoint entry;
    memcpy( &entry.addr, item->ai_addr, item->ai_addrlen );
    entry.length = item->ai_addrlen;
    proxies.push_back( entry );
  }
  freeaddrinfo( addresses );
  if ( proxies.empty() ) { throw std::runtime_error( "SOCKS5: no usable proxy addresses" ); }
}

Network::Socks5UDP::~Socks5UDP() { close_sockets(); }

void Network::Socks5UDP::close_sockets()
{
  if ( datagram >= 0 ) { close( datagram ); datagram = -1; }
  if ( control >= 0 ) { close( control ); control = -1; }
}

void Network::Socks5UDP::failed( const std::string& error, uint64_t now )
{
  close_sockets();
  last_error = "SOCKS5: " + error;
  state = State::Backoff;
  retry_at = now + retry_delay;
  retry_delay = std::min( retry_delay * 2, 30000u );
  proxy_index = ( proxy_index + 1 ) % proxies.size();
}

void Network::Socks5UDP::reconnect( uint64_t now )
{
  if ( state != State::Ready ) { return; }
  failed( "reopening UDP association", now );
  retry_at = now;
}

void Network::Socks5UDP::start( uint64_t now )
{
  const auto& endpoint = proxies[proxy_index];
  control = open_socket( endpoint.addr.ss_family, SOCK_STREAM );
  state = State::Connecting;
  deadline = now + 10000;
  output.clear(); input.clear(); written = 0;
  if ( connect( control, reinterpret_cast<const sockaddr*>( &endpoint.addr ), endpoint.length ) < 0 && errno != EINPROGRESS ) {
    failed( std::string( "connect: " ) + strerror( errno ), now );
  }
}

void Network::Socks5UDP::associate()
{
  // RFC 1928 permits a zero address when the client doesn't yet know it.
  output.assign( "\5\3\0\1\0\0\0\0\0\0", 10 );
  input.clear(); written = 0;
  state = State::Associate;
}

void Network::Socks5UDP::tick( uint64_t now )
{
  if ( state == State::Backoff ) {
    if ( now < retry_at ) { return; }
    start( now );
    if ( state == State::Backoff ) { return; }
  }
  if ( state != State::Ready && now >= deadline ) { failed( "handshake timed out", now ); return; }
  if ( state == State::Connecting ) {
    pollfd pfd { control, POLLOUT, 0 };
    const int count = poll( &pfd, 1, 0 );
    if ( count < 0 ) { if ( errno != EINTR ) { failed( "connect poll failed", now ); } return; }
    if ( !count ) { return; }
    int error = 0;
    socklen_t length = sizeof( error );
    if ( getsockopt( control, SOL_SOCKET, SO_ERROR, &error, &length ) < 0 ) { error = errno; }
    if ( error ) { failed( std::string( "connect: " ) + strerror( error ), now ); return; }
    state = State::Greeting;
    output.assign( "\5\1\0", 3 );
  }
  // Bounded work per event-loop iteration, including deliberately fragmented
  // handshakes. Never block the terminal, even if the proxy stops answering.
  for ( unsigned step = 0; step < 8; ++step ) {
    if ( state != State::Ready && written < output.size() ) {
      const auto count = safe_send( control, output.data() + written, output.size() - written );
      if ( count < 0 ) { if ( !again() ) { failed( "control write failed", now ); } return; }
      if ( !count ) { failed( "control connection closed", now ); return; }
      written += count;
      if ( written != output.size() ) { return; }
    }
    char bytes[512];
    const auto count = recv( control, bytes, sizeof( bytes ), 0 );
    if ( count < 0 ) { if ( !again() ) { failed( "control read failed", now ); } return; }
    if ( !count ) { failed( "proxy disconnected; reconnecting", now ); return; }
    if ( state == State::Ready ) { throw std::runtime_error( "SOCKS5: unexpected data on UDP control connection" ); }
    input.append( bytes, count );
    if ( input.size() > 262 ) { throw std::runtime_error( "SOCKS5: oversized handshake" ); }
    if ( state == State::Greeting ) {
      if ( input.size() < 2 ) { continue; }
      if ( input != std::string( "\5\0", 2 ) ) {
        throw std::runtime_error( "SOCKS5: proxy must support SOCKS5 without username/password authentication" );
      }
      associate();
      continue;
    }
    if ( input.size() < 4 ) { continue; }
    if ( input[0] != '\5' || input[2] != '\0' ) { throw std::runtime_error( "SOCKS5: malformed UDP ASSOCIATE reply" ); }
    if ( input[1] != '\0' ) {
      throw std::runtime_error( "SOCKS5: UDP ASSOCIATE rejected (reply "
                                + std::to_string( static_cast<unsigned char>( input[1] ) )
                                + "); proxy must support UDP, not just TCP CONNECT" );
    }
    const auto length = address_length( input, 3 );
    if ( !length ) { continue; }
    if ( input.size() != 3 + length ) { throw std::runtime_error( "SOCKS5: trailing handshake data" ); }
    Endpoint relay;
    if ( input[3] == '\1' ) {
      auto& addr = *reinterpret_cast<sockaddr_in*>( &relay.addr );
      addr.sin_family = AF_INET;
      memcpy( &addr.sin_addr, input.data() + 4, 4 );
      memcpy( &addr.sin_port, input.data() + 8, 2 );
      relay.length = sizeof( addr );
      if ( !addr.sin_addr.s_addr && proxies[proxy_index].addr.ss_family == AF_INET ) {
        addr.sin_addr = reinterpret_cast<const sockaddr_in*>( &proxies[proxy_index].addr )->sin_addr;
      }
      if ( !addr.sin_addr.s_addr || !addr.sin_port ) { throw std::runtime_error( "SOCKS5: invalid UDP relay endpoint" ); }
    } else if ( input[3] == '\4' ) {
      auto& addr = *reinterpret_cast<sockaddr_in6*>( &relay.addr );
      addr.sin6_family = AF_INET6;
      memcpy( &addr.sin6_addr, input.data() + 4, 16 );
      memcpy( &addr.sin6_port, input.data() + 20, 2 );
      relay.length = sizeof( addr );
      if ( IN6_IS_ADDR_UNSPECIFIED( &addr.sin6_addr ) && proxies[proxy_index].addr.ss_family == AF_INET6 ) {
        addr.sin6_addr = reinterpret_cast<const sockaddr_in6*>( &proxies[proxy_index].addr )->sin6_addr;
      }
      if ( IN6_IS_ADDR_UNSPECIFIED( &addr.sin6_addr ) || !addr.sin6_port ) { throw std::runtime_error( "SOCKS5: invalid UDP relay endpoint" ); }
    } else {
      throw std::runtime_error( "SOCKS5: proxy must advertise a numeric UDP relay address" );
    }
    datagram = open_socket( relay.addr.ss_family, SOCK_DGRAM );
    if ( connect( datagram, reinterpret_cast<const sockaddr*>( &relay.addr ), relay.length ) < 0 ) {
      failed( std::string( "UDP connect: " ) + strerror( errno ), now ); return;
    }
    state = State::Ready; ++generation;
    retry_delay = 250; last_error.clear(); input.clear(); output.clear();
    return;
  }
}

int Network::Socks5UDP::wait_time( uint64_t now ) const
{
  if ( state == State::Ready ) { return INT_MAX; }
  if ( state == State::Backoff ) { return retry_at > now ? static_cast<int>( retry_at - now ) : 0; }
  return deadline > now ? static_cast<int>( std::min<uint64_t>( 20, deadline - now ) ) : 0;
}

std::vector<int> Network::Socks5UDP::fds() const
{
  std::vector<int> result;
  if ( control >= 0 ) { result.push_back( control ); }
  if ( datagram >= 0 ) { result.push_back( datagram ); }
  return result;
}

ssize_t Network::Socks5UDP::send( const std::string& ciphertext )
{
  if ( !ready() ) { errno = EAGAIN; return -1; }
  const auto packet = std::string( 3, '\0' ) + destination + ciphertext;
  const auto count = safe_send( datagram, packet.data(), packet.size() );
  if ( count < 0 ) {
    const int error = errno;
    if ( error == ECONNREFUSED || error == ENETUNREACH || error == EHOSTUNREACH ) { reconnect( timestamp() ); }
    errno = error;
    return count;
  }
  if ( count != static_cast<ssize_t>( packet.size() ) ) { errno = EIO; return -1; }
  return ciphertext.size();
}

std::string Network::Socks5UDP::receive()
{
  if ( !ready() ) { throw NetworkException( "SOCKS5 association pending", EAGAIN ); }
  // The connected UDP socket pins the outer sender to the negotiated relay.
  // Account for a maximum SOCKS address header separately from Mosh's MTU.
  char bytes[Crypto::Session::RECEIVE_MTU + 262];
  iovec iov { bytes, sizeof( bytes ) };
  msghdr msg {};
  msg.msg_iov = &iov; msg.msg_iovlen = 1;
  const auto count = recvmsg( datagram, &msg, 0 );
  if ( count < 0 ) {
    const int error = errno;
    if ( error == ECONNREFUSED || error == ENETUNREACH || error == EHOSTUNREACH ) {
      reconnect( timestamp() );
      throw NetworkException( "SOCKS5 UDP relay lost; reconnecting", EAGAIN );
    }
    throw NetworkException( "SOCKS5 UDP receive", error );
  }
  const std::string packet( bytes, count );
  size_t length = 0;
  try { length = address_length( packet, 3 ); } catch ( const std::runtime_error& ) {}
  if ( ( msg.msg_flags & MSG_TRUNC ) || packet.size() < 4 || packet.compare( 0, 3, std::string( 3, '\0' ) )
       || !length || packet.size() - 3 - length > Crypto::Session::RECEIVE_MTU ) {
    throw NetworkException( "SOCKS5 malformed or fragmented UDP packet", EAGAIN );
  }
  const auto source = packet.substr( 3, length );
  // A hostname target may be returned as a numeric address by the proxy.
  // Never resolve it locally. Mosh authentication still verifies the peer.
  if ( source.substr( source.size() - 2 ) != destination.substr( destination.size() - 2 )
       || ( destination[0] != '\3' && source != destination )
       || ( source[0] == '\3' && source != destination ) ) {
    throw NetworkException( "SOCKS5 unexpected UDP destination", EAGAIN );
  }
  return packet.substr( 3 + length );
}
