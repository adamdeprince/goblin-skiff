/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "streamforward.h"

#include "src/include/config.h"
#include "src/crypto/prng.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static bool set_nonblocking( int fd )
{
  int flags = fcntl( fd, F_GETFL, 0 );
  return ( flags >= 0 ) && ( fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0 );
}

static bool set_close_on_exec( int fd )
{
  int flags = fcntl( fd, F_GETFD, 0 );
  return ( flags >= 0 ) && ( fcntl( fd, F_SETFD, flags | FD_CLOEXEC ) == 0 );
}

static bool parse_port( const std::string& text, uint32_t& port )
{
  if ( text.empty() ) {
    return false;
  }

  char* end = NULL;
  errno = 0;
  unsigned long parsed = strtoul( text.c_str(), &end, 10 );
  if ( errno || *end || parsed > 65535 ) {
    return false;
  }
  port = parsed;
  return true;
}

static std::vector<std::string> split_colon( const std::string& input )
{
  std::vector<std::string> parts;
  std::string current;
  for ( std::string::const_iterator it = input.begin(); it != input.end(); it++ ) {
    if ( *it == ':' ) {
      parts.push_back( current );
      current.clear();
    } else {
      current.push_back( *it );
    }
  }
  parts.push_back( current );
  return parts;
}

static void append_be16( std::string& out, uint16_t value )
{
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
}

static bool read_be16( const std::string& in, size_t& offset, uint16_t& value )
{
  if ( offset + 2 > in.size() ) {
    return false;
  }
  value = ( static_cast<uint16_t>( static_cast<unsigned char>( in[offset] ) ) << 8 )
          | static_cast<unsigned char>( in[offset + 1] );
  offset += 2;
  return true;
}

static size_t pad4( size_t size )
{
  return ( size + 3 ) & ~static_cast<size_t>( 3 );
}

static bool parse_display_number( const std::string& display, uint32_t& display_number )
{
  const size_t colon = display.rfind( ':' );
  if ( colon == std::string::npos ) {
    return false;
  }
  size_t start = colon + 1;
  size_t end = start;
  while ( end < display.size() && display[end] >= '0' && display[end] <= '9' ) {
    end++;
  }
  if ( start == end ) {
    return false;
  }
  return parse_port( display.substr( start, end - start ), display_number );
}

static std::string display_host( const std::string& display )
{
  const size_t colon = display.rfind( ':' );
  if ( colon == std::string::npos ) {
    return std::string();
  }
  return display.substr( 0, colon );
}

static bool fill_random( std::string& out, size_t size, Crypto::Mode mode )
{
  out.assign( size, '\0' );
  if ( size == 0 ) {
    return true;
  }
  if ( mode == Crypto::Mode::FipsAES128GCM ) {
    try {
      PRNG( mode ).fill( &out[0], size );
      return true;
    } catch ( const Crypto::CryptoException& ) {
      return false;
    }
  }
#if defined( HAVE_GETRANDOM ) && defined( HAVE_SYS_RANDOM_H )
  size_t offset = 0;
  while ( offset < size ) {
    ssize_t got = getrandom( &out[0] + offset, size - offset, 0 );
    if ( got > 0 ) {
      offset += got;
    } else if ( got < 0 && errno == EINTR ) {
      continue;
    } else {
      break;
    }
  }
  if ( offset == size ) {
    return true;
  }
#endif

  std::ifstream urandom( "/dev/urandom", std::ios::in | std::ios::binary );
  if ( !urandom ) {
    return false;
  }
  urandom.read( &out[0], out.size() );
  return urandom.gcount() == static_cast<std::streamsize>( out.size() );
}

static std::string default_xauthority_path( void )
{
  const char* xauthority = getenv( "XAUTHORITY" );
  if ( xauthority && *xauthority ) {
    return xauthority;
  }
  const char* home = getenv( "HOME" );
  if ( home && *home ) {
    return std::string( home ) + "/.Xauthority";
  }
  return std::string();
}

static bool read_xauthority_cookie( const std::string& path, const std::string& display, std::string& cookie )
{
  uint32_t display_number = 0;
  if ( path.empty() || !parse_display_number( display, display_number ) ) {
    return false;
  }

  std::ifstream in( path.c_str(), std::ios::in | std::ios::binary );
  if ( !in ) {
    return false;
  }
  std::string bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
  size_t offset = 0;
  const std::string number_text = std::to_string( display_number );
  while ( offset < bytes.size() ) {
    uint16_t family = 0;
    uint16_t len = 0;
    std::string address;
    std::string number;
    std::string name;
    std::string data;
    if ( !read_be16( bytes, offset, family ) || !read_be16( bytes, offset, len ) || offset + len > bytes.size() ) {
      return false;
    }
    address.assign( bytes.data() + offset, len );
    offset += len;
    if ( !read_be16( bytes, offset, len ) || offset + len > bytes.size() ) {
      return false;
    }
    number.assign( bytes.data() + offset, len );
    offset += len;
    if ( !read_be16( bytes, offset, len ) || offset + len > bytes.size() ) {
      return false;
    }
    name.assign( bytes.data() + offset, len );
    offset += len;
    if ( !read_be16( bytes, offset, len ) || offset + len > bytes.size() ) {
      return false;
    }
    data.assign( bytes.data() + offset, len );
    offset += len;

    (void)family;
    (void)address;
    if ( name == "MIT-MAGIC-COOKIE-1" && number == number_text && !data.empty() ) {
      cookie = data;
      return true;
    }
  }

  return false;
}

static bool write_xauthority_cookie( const std::string& path, uint32_t display_number, const std::string& cookie )
{
  std::string entry;
  append_be16( entry, 65535 ); /* FamilyWild */
  append_be16( entry, 0 );
  const std::string number = std::to_string( display_number );
  append_be16( entry, number.size() );
  entry.append( number );
  const std::string name = "MIT-MAGIC-COOKIE-1";
  append_be16( entry, name.size() );
  entry.append( name );
  append_be16( entry, cookie.size() );
  entry.append( cookie );

  std::ofstream out( path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc );
  if ( !out ) {
    return false;
  }
  out.write( entry.data(), entry.size() );
  out.close();
  chmod( path.c_str(), S_IRUSR | S_IWUSR );
  return static_cast<bool>( out );
}

StreamForwarder::StreamForwarder( Side s_side,
                                  unsigned int s_stream_delay_ms,
                                  unsigned int s_stream_rate_bytes_per_second,
                                  Crypto::Mode s_crypto_mode )
  : side( s_side ), crypto_mode( s_crypto_mode ), next_stream_id( s_side == ClientSide ? 1 : 2 ),
    stream_delay_ms( s_stream_delay_ms ),
    stream_rate_bytes_per_second( s_stream_rate_bytes_per_second ? s_stream_rate_bytes_per_second : 2048 ),
    automatic_rate( s_stream_rate_bytes_per_second == 0 ),
    stream_tokens(), last_token_update( Network::timestamp() ), agent_requested( false ), x11_requested( false ),
    agent_path(), agent_dir(), local_agent_path(), local_x11_display(), local_x11_auth_data(), x11_auth_data(),
    x11_auth_path(), x11_display_string(), requested_listeners(), listeners(), streams(), fd_to_stream(),
    pending_medium_control(), pending_low_control()
{
  stream_tokens[priority_index( Network::StreamPriorityLow )] = 0;
  stream_tokens[priority_index( Network::StreamPriorityMedium )] = 0;

  const char* ssh_auth_sock = getenv( "SSH_AUTH_SOCK" );
  if ( ssh_auth_sock ) {
    local_agent_path = ssh_auth_sock;
  }
  const char* display = getenv( "DISPLAY" );
  if ( display ) {
    local_x11_display = display;
  }
}

StreamForwarder::~StreamForwarder()
{
  for ( std::vector<Listener>::iterator it = listeners.begin(); it != listeners.end(); it++ ) {
    close_listener( *it );
  }
  for ( std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); it++ ) {
    close_stream( it->second, false );
  }
  if ( !agent_path.empty() ) {
    unlink( agent_path.c_str() );
  }
  if ( !agent_dir.empty() ) {
    rmdir( agent_dir.c_str() );
  }
  if ( !x11_auth_path.empty() ) {
    unlink( x11_auth_path.c_str() );
  }
}

void StreamForwarder::adapt_link_budget( double bytes_per_second )
{
  if ( automatic_rate ) {
    stream_rate_bytes_per_second = bytes_per_second > 0 ? std::max( 64U, unsigned( bytes_per_second * .9 ) ) : 2048;
  }
}

bool StreamForwarder::add_tcp_forward( const std::string& spec, std::string& error )
{
  Listener listener;
  listener.kind = TcpListener;
  if ( !parse_tcp_forward( spec, listener, error ) ) {
    return false;
  }
  requested_listeners.push_back( listener );
  return true;
}

bool StreamForwarder::add_dynamic_forward( const std::string& spec, std::string& error )
{
  Listener listener;
  listener.kind = DynamicListener;
  if ( !parse_bind_port( spec, listener, error ) ) {
    return false;
  }
  requested_listeners.push_back( listener );
  return true;
}

bool StreamForwarder::enable_agent_forwarding( std::string& error )
{
  agent_requested = true;
  if ( side == ClientSide && local_agent_path.empty() ) {
    error = "SSH_AUTH_SOCK is not set; cannot enable agent forwarding";
    return false;
  }
  return true;
}

bool StreamForwarder::enable_x11_forwarding( std::string& error )
{
  x11_requested = true;
  if ( side == ClientSide ) {
    if ( local_x11_display.empty() ) {
      error = "DISPLAY is not set; cannot enable X11 forwarding";
      return false;
    }
    if ( !read_xauthority_cookie( default_xauthority_path(), local_x11_display, local_x11_auth_data ) ) {
      error = "could not find MIT-MAGIC-COOKIE-1 data for DISPLAY in XAUTHORITY";
      return false;
    }
  }
  return true;
}

bool StreamForwarder::parse_tcp_forward( const std::string& spec, Listener& listener, std::string& error ) const
{
  std::vector<std::string> parts = split_colon( spec );

  if ( parts.size() == 3 ) {
    listener.bind_host = "127.0.0.1";
    if ( !parse_port( parts[0], listener.bind_port ) || !parse_port( parts[2], listener.target_port ) ) {
      error = "Bad forwarding port in " + spec;
      return false;
    }
    listener.target_host = parts[1];
  } else if ( parts.size() == 4 ) {
    listener.bind_host = parts[0].empty() ? "127.0.0.1" : parts[0];
    if ( !parse_port( parts[1], listener.bind_port ) || !parse_port( parts[3], listener.target_port ) ) {
      error = "Bad forwarding port in " + spec;
      return false;
    }
    listener.target_host = parts[2];
  } else {
    error = "Forwarding spec must be [bind_address:]port:host:hostport";
    return false;
  }

  if ( listener.target_host.empty() ) {
    error = "Forwarding target host is empty";
    return false;
  }
  return true;
}

bool StreamForwarder::parse_bind_port( const std::string& spec, Listener& listener, std::string& error ) const
{
  size_t colon = spec.rfind( ':' );
  std::string port_text;
  if ( colon == std::string::npos ) {
    listener.bind_host = "127.0.0.1";
    port_text = spec;
  } else {
    listener.bind_host = spec.substr( 0, colon );
    if ( listener.bind_host.empty() ) {
      listener.bind_host = "127.0.0.1";
    }
    port_text = spec.substr( colon + 1 );
  }

  if ( !parse_port( port_text, listener.bind_port ) ) {
    error = "Bad listen port in " + spec;
    return false;
  }
  return true;
}

bool StreamForwarder::listen( std::string& error )
{
  for ( std::vector<Listener>::iterator it = requested_listeners.begin(); it != requested_listeners.end(); it++ ) {
    Listener listener( *it );
    if ( !open_tcp_listener( listener, error ) ) {
      return false;
    }
    listeners.push_back( listener );
  }

  if ( agent_requested && side == ServerSide ) {
    Listener listener;
    listener.kind = AgentListener;
    if ( !open_agent_listener( listener, error ) ) {
      return false;
    }
    listeners.push_back( listener );
  }

  if ( x11_requested && side == ServerSide ) {
    Listener listener;
    listener.kind = X11Listener;
    if ( !open_x11_listener( listener, error ) ) {
      return false;
    }
    listeners.push_back( listener );
  }

  return true;
}

bool StreamForwarder::open_tcp_listener( Listener& listener, std::string& error )
{
  struct addrinfo hints;
  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  std::ostringstream port_stream;
  port_stream << listener.bind_port;

  struct addrinfo* results = NULL;
  int gai = getaddrinfo( listener.bind_host.c_str(), port_stream.str().c_str(), &hints, &results );
  if ( gai != 0 ) {
    error = std::string( "getaddrinfo: " ) + gai_strerror( gai );
    return false;
  }

  int fd = -1;
  for ( struct addrinfo* ai = results; ai; ai = ai->ai_next ) {
    fd = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
    if ( fd < 0 ) {
      continue;
    }

    int one = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );

    if ( bind( fd, ai->ai_addr, ai->ai_addrlen ) == 0 && ::listen( fd, 16 ) == 0 ) {
      break;
    }

    close( fd );
    fd = -1;
  }

  freeaddrinfo( results );

  if ( fd < 0 ) {
    error = std::string( "bind/listen: " ) + strerror( errno );
    return false;
  }

  set_nonblocking( fd );
  set_close_on_exec( fd );
  listener.fd = fd;
  return true;
}

bool StreamForwarder::open_agent_listener( Listener& listener, std::string& error )
{
  char tmpl[] = "/tmp/mosh-agent.XXXXXX";
  char* dir = mkdtemp( tmpl );
  if ( dir == NULL ) {
    error = std::string( "mkdtemp: " ) + strerror( errno );
    return false;
  }

  agent_dir = dir;
  agent_path = agent_dir + "/agent";

  int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
  if ( fd < 0 ) {
    error = std::string( "socket: " ) + strerror( errno );
    return false;
  }

  struct sockaddr_un addr;
  memset( &addr, 0, sizeof( addr ) );
  addr.sun_family = AF_UNIX;
  if ( agent_path.size() >= sizeof( addr.sun_path ) ) {
    close( fd );
    error = "agent socket path is too long";
    return false;
  }
  strncpy( addr.sun_path, agent_path.c_str(), sizeof( addr.sun_path ) - 1 );

  if ( bind( fd, reinterpret_cast<struct sockaddr*>( &addr ), sizeof( addr ) ) < 0 || ::listen( fd, 16 ) < 0 ) {
    error = std::string( "agent bind/listen: " ) + strerror( errno );
    close( fd );
    return false;
  }

  chmod( agent_path.c_str(), S_IRUSR | S_IWUSR );
  set_nonblocking( fd );
  set_close_on_exec( fd );
  listener.fd = fd;
  listener.path = agent_path;
  return true;
}

bool StreamForwarder::open_x11_listener( Listener& listener, std::string& error )
{
  if ( !fill_random( x11_auth_data, 16, crypto_mode ) ) {
    error = "could not generate X11 forwarding cookie";
    return false;
  }

  bool listening = false;
  for ( uint32_t display_number = 10; display_number < 100; display_number++ ) {
    listener.bind_host = "127.0.0.1";
    listener.bind_port = 6000 + display_number;
    listener.target_host.clear();
    listener.target_port = 0;
    if ( open_tcp_listener( listener, error ) ) {
      std::ostringstream display;
      display << "localhost:" << display_number << ".0";
      x11_display_string = display.str();

      char auth_tmpl[] = "/tmp/mosh-xauth.XXXXXX";
      int auth_fd = mkstemp( auth_tmpl );
      if ( auth_fd < 0 ) {
        close_listener( listener );
        error = std::string( "mkstemp: " ) + strerror( errno );
        return false;
      }
      close( auth_fd );
      x11_auth_path = auth_tmpl;
      if ( !write_xauthority_cookie( x11_auth_path, display_number, x11_auth_data ) ) {
        close_listener( listener );
        unlink( x11_auth_path.c_str() );
        x11_auth_path.clear();
        error = "could not write X11 authority file";
        return false;
      }

      listening = true;
      break;
    }
  }

  if ( !listening ) {
    if ( error.empty() ) {
      error = "could not listen on an X11 forwarding display";
    }
    return false;
  }

  return true;
}

void StreamForwarder::close_listener( Listener& listener )
{
  if ( listener.fd >= 0 ) {
    close( listener.fd );
    listener.fd = -1;
  }
}

std::vector<int> StreamForwarder::fds( void ) const
{
  std::vector<int> ret;
  for ( std::vector<Listener>::const_iterator it = listeners.begin(); it != listeners.end(); it++ ) {
    if ( it->fd >= 0 ) {
      ret.push_back( it->fd );
    }
  }

  for ( std::map<uint64_t, Stream>::const_iterator it = streams.begin(); it != streams.end(); it++ ) {
    const Stream& stream = it->second;
    if ( stream.fd >= 0 && !stream.closed && !stream.local_eof_sent
         && stream.to_network.size() < STREAM_QUEUE_LIMIT ) {
      ret.push_back( stream.fd );
    }
  }

  return ret;
}

int StreamForwarder::wait_time( uint64_t now ) const
{
  if ( !pending_medium_control.empty() || !pending_low_control.empty() ) {
    return 0;
  }

  int ret = INT_MAX;

  for ( std::map<uint64_t, Stream>::const_iterator it = streams.begin(); it != streams.end(); it++ ) {
    if ( !it->second.to_local.empty() ) {
      ret = std::min( ret, 10 );
    }
  }

  for ( int priority = Network::StreamPriorityMedium; priority >= Network::StreamPriorityLow; priority-- ) {
    Network::StreamPriority stream_priority = static_cast<Network::StreamPriority>( priority );
    bool has_priority_data = false;
    int stream_wait = INT_MAX;
    size_t minimum_tokens_needed = 1;

    for ( std::map<uint64_t, Stream>::const_iterator it = streams.begin(); it != streams.end(); it++ ) {
      const Stream& stream = it->second;
      if ( stream.priority != stream_priority ) {
        continue;
      }
      if ( stream.local_eof_pending && stream.to_network.empty() ) {
        return 0;
      }
      if ( !stream.to_network.empty() ) {
        has_priority_data = true;
        if ( stream.to_network.size() >= MIN_STREAM_CHUNK && !stream.local_eof_pending ) {
          minimum_tokens_needed = MIN_STREAM_CHUNK;
        }
        if ( stream.to_network.size() >= MAX_STREAM_CHUNK ) {
          stream_wait = std::min( stream_wait, 0 );
        } else if ( now >= stream.first_pending_byte + stream_delay_ms ) {
          stream_wait = std::min( stream_wait, 0 );
        } else {
          stream_wait = std::min( stream_wait, static_cast<int>( stream.first_pending_byte + stream_delay_ms - now ) );
        }
      }
    }

    if ( has_priority_data ) {
      if ( stream_tokens_for( stream_priority ) < minimum_tokens_needed ) {
        double needed = static_cast<double>( minimum_tokens_needed ) - stream_tokens_for( stream_priority );
        int token_wait = static_cast<int>( ( needed * 1000.0 ) / stream_rate_bytes_per_second ) + 1;
        if ( token_wait < 1 ) {
          token_wait = 1;
        }
        stream_wait = std::max( stream_wait, token_wait );
      }
      ret = std::min( ret, stream_wait );
      break;
    }
  }

  return ret;
}

bool StreamForwarder::has_pending_network_data( void ) const
{
  if ( !pending_medium_control.empty() || !pending_low_control.empty() ) {
    return true;
  }
  for ( std::map<uint64_t, Stream>::const_iterator it = streams.begin(); it != streams.end(); ++it ) {
    if ( !it->second.to_network.empty() || it->second.local_eof_pending ) {
      return true;
    }
  }
  return false;
}

void StreamForwarder::process_readable_fd( int fd )
{
  try_write_local();

  for ( std::vector<Listener>::const_iterator it = listeners.begin(); it != listeners.end(); it++ ) {
    if ( it->fd == fd ) {
      accept_connection( *it );
      return;
    }
  }

  std::map<int, uint64_t>::const_iterator found = fd_to_stream.find( fd );
  if ( found == fd_to_stream.end() ) {
    return;
  }

  std::map<uint64_t, Stream>::iterator stream_it = streams.find( found->second );
  if ( stream_it == streams.end() ) {
    return;
  }

  Stream& stream = stream_it->second;
  if ( stream.socks_state == SocksGreeting || stream.socks_state == SocksRequest ) {
    read_socks( stream );
  } else {
    read_stream( stream );
  }
}

uint64_t StreamForwarder::allocate_stream_id( void )
{
  uint64_t id = next_stream_id;
  next_stream_id += 2;
  return id;
}

void StreamForwarder::accept_connection( const Listener& listener )
{
  while ( true ) {
    int fd = accept( listener.fd, NULL, NULL );
    if ( fd < 0 ) {
      if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
        return;
      }
      return;
    }

    set_nonblocking( fd );
    set_close_on_exec( fd );

    Stream stream;
    stream.id = allocate_stream_id();
    stream.fd = fd;
    stream.priority = ( listener.kind == AgentListener || listener.kind == X11Listener ) ? Network::StreamPriorityMedium
                                                                                         : Network::StreamPriorityLow;
    stream.x11 = listener.kind == X11Listener;

    if ( listener.kind == DynamicListener ) {
      stream.socks_state = SocksGreeting;
    } else {
      open_local_stream( stream,
                         listener.target_host,
                         listener.target_port,
                         listener.kind == AgentListener,
                         listener.kind == X11Listener,
                         listener.kind == X11Listener ? x11_auth_data : std::string(),
                         stream.priority );
    }

    fd_to_stream[fd] = stream.id;
    streams[stream.id] = stream;
  }
}

void StreamForwarder::open_local_stream( Stream& stream,
                                         const std::string& host,
                                         uint32_t port,
                                         bool agent,
                                         bool x11,
                                         const std::string& x11_peer_auth_data,
                                         Network::StreamPriority priority )
{
  stream.socks_state = SocksEstablished;
  stream.priority = priority;
  stream.x11 = x11;
  queue_control( Network::StreamEvent::open( stream.id, host, port, agent, x11, x11_peer_auth_data, priority ) );
}

void StreamForwarder::read_stream( Stream& stream )
{
  char buf[4096];
  while ( stream.to_network.size() < STREAM_QUEUE_LIMIT ) {
    ssize_t bytes = read( stream.fd, buf, sizeof( buf ) );
    if ( bytes > 0 ) {
      if ( stream.to_network.empty() ) {
        stream.first_pending_byte = Network::timestamp();
      }
      stream.to_network.append( buf, bytes );
    } else if ( bytes == 0 ) {
      stream.local_eof_pending = true;
      return;
    } else {
      if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
        return;
      }
      close_stream( stream, true );
      return;
    }
  }
}

void StreamForwarder::read_socks( Stream& stream )
{
  char buf[512];
  while ( stream.socks_buffer.size() < 1024 ) {
    ssize_t bytes = read( stream.fd, buf, sizeof( buf ) );
    if ( bytes > 0 ) {
      stream.socks_buffer.append( buf, bytes );
    } else if ( bytes == 0 ) {
      close_stream( stream, false );
      return;
    } else {
      if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
        break;
      }
      close_stream( stream, false );
      return;
    }
  }

  if ( stream.socks_state == SocksGreeting ) {
    if ( stream.socks_buffer.size() < 2 ) {
      return;
    }
    unsigned char nmethods = static_cast<unsigned char>( stream.socks_buffer[1] );
    if ( stream.socks_buffer.size() < 2 + static_cast<size_t>( nmethods ) ) {
      return;
    }
    const char reply[] = { 0x05, 0x00 };
    stream.to_local.append( reply, sizeof( reply ) );
    stream.socks_buffer.erase( 0, 2 + nmethods );
    stream.socks_state = SocksRequest;
  }

  if ( stream.socks_state == SocksRequest ) {
    handle_socks_request( stream );
  }
  try_write_local();
}

void StreamForwarder::handle_socks_request( Stream& stream )
{
  if ( stream.socks_buffer.size() < 4 ) {
    return;
  }

  const unsigned char version = static_cast<unsigned char>( stream.socks_buffer[0] );
  const unsigned char command = static_cast<unsigned char>( stream.socks_buffer[1] );
  const unsigned char atyp = static_cast<unsigned char>( stream.socks_buffer[3] );
  if ( version != 5 || command != 1 ) {
    close_stream( stream, false );
    return;
  }

  size_t offset = 4;
  std::string host;
  if ( atyp == 1 ) {
    if ( stream.socks_buffer.size() < offset + 4 + 2 ) {
      return;
    }
    char tmp[32];
    snprintf( tmp,
              sizeof( tmp ),
              "%u.%u.%u.%u",
              static_cast<unsigned char>( stream.socks_buffer[offset] ),
              static_cast<unsigned char>( stream.socks_buffer[offset + 1] ),
              static_cast<unsigned char>( stream.socks_buffer[offset + 2] ),
              static_cast<unsigned char>( stream.socks_buffer[offset + 3] ) );
    host = tmp;
    offset += 4;
  } else if ( atyp == 3 ) {
    if ( stream.socks_buffer.size() < offset + 1 ) {
      return;
    }
    unsigned char len = static_cast<unsigned char>( stream.socks_buffer[offset++] );
    if ( stream.socks_buffer.size() < offset + len + 2 ) {
      return;
    }
    host.assign( stream.socks_buffer.begin() + offset, stream.socks_buffer.begin() + offset + len );
    offset += len;
  } else {
    close_stream( stream, false );
    return;
  }

  uint32_t port = ( static_cast<unsigned char>( stream.socks_buffer[offset] ) << 8 )
                  | static_cast<unsigned char>( stream.socks_buffer[offset + 1] );
  stream.socks_buffer.clear();

  const char success[] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
  stream.to_local.append( success, sizeof( success ) );
  open_local_stream( stream, host, port, false, false, std::string(), Network::StreamPriorityLow );
}

void StreamForwarder::handle_remote_event( const Network::StreamEvent& event )
{
  switch ( event.type ) {
    case Network::StreamOpenType:
      connect_remote_stream( event );
      break;
    case Network::StreamDataType: {
      std::map<uint64_t, Stream>::iterator it = streams.find( event.stream_id );
      if ( it != streams.end() && !it->second.closed ) {
        append_stream_data( it->second, event.data );
        if ( it->second.to_local.size() > LOCAL_QUEUE_LIMIT ) {
          close_stream( it->second, true );
        }
      }
      try_write_local();
    } break;
    case Network::StreamEOFType: {
      std::map<uint64_t, Stream>::iterator it = streams.find( event.stream_id );
      if ( it != streams.end() && it->second.fd >= 0 ) {
        shutdown( it->second.fd, SHUT_WR );
        it->second.remote_eof_seen = true;
      }
    } break;
    case Network::StreamCloseType: {
      std::map<uint64_t, Stream>::iterator it = streams.find( event.stream_id );
      if ( it != streams.end() ) {
        close_stream( it->second, false );
      }
    } break;
  }
}

void StreamForwarder::connect_remote_stream( const Network::StreamEvent& event )
{
  int fd = -1;
  if ( event.agent ) {
    fd = connect_unix( local_agent_path );
  } else if ( event.x11 ) {
    fd = connect_x11();
  } else {
    fd = connect_tcp( event.host, event.port );
  }

  if ( fd < 0 ) {
    queue_control( Network::StreamEvent::close( event.stream_id, event.priority ) );
    return;
  }

  Stream stream;
  stream.id = event.stream_id;
  stream.fd = fd;
  stream.socks_state = SocksEstablished;
  stream.priority = event.priority;
  stream.x11 = event.x11;
  stream.x11_peer_auth_data = event.x11_auth_data;
  fd_to_stream[fd] = stream.id;
  streams[stream.id] = stream;
}

int StreamForwarder::connect_tcp( const std::string& host, uint32_t port )
{
  struct addrinfo hints;
  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  std::ostringstream port_stream;
  port_stream << port;

  struct addrinfo* results = NULL;
  int gai = getaddrinfo( host.c_str(), port_stream.str().c_str(), &hints, &results );
  if ( gai != 0 ) {
    return -1;
  }

  int fd = -1;
  for ( struct addrinfo* ai = results; ai; ai = ai->ai_next ) {
    fd = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
    if ( fd < 0 ) {
      continue;
    }
    if ( connect( fd, ai->ai_addr, ai->ai_addrlen ) == 0 ) {
      break;
    }
    close( fd );
    fd = -1;
  }

  freeaddrinfo( results );

  if ( fd >= 0 ) {
    set_nonblocking( fd );
    set_close_on_exec( fd );
  }
  return fd;
}

int StreamForwarder::connect_unix( const std::string& path )
{
  if ( path.empty() ) {
    return -1;
  }

  int fd = socket( AF_UNIX, SOCK_STREAM, 0 );
  if ( fd < 0 ) {
    return -1;
  }

  struct sockaddr_un addr;
  memset( &addr, 0, sizeof( addr ) );
  addr.sun_family = AF_UNIX;
  if ( path.size() >= sizeof( addr.sun_path ) ) {
    close( fd );
    return -1;
  }
  strncpy( addr.sun_path, path.c_str(), sizeof( addr.sun_path ) - 1 );

  if ( connect( fd, reinterpret_cast<struct sockaddr*>( &addr ), sizeof( addr ) ) < 0 ) {
    close( fd );
    return -1;
  }

  set_nonblocking( fd );
  set_close_on_exec( fd );
  return fd;
}

int StreamForwarder::connect_x11( void )
{
  uint32_t display_number = 0;
  if ( !parse_display_number( local_x11_display, display_number ) ) {
    return -1;
  }

  const std::string host = display_host( local_x11_display );
  if ( host.empty() || host == "unix" ) {
    std::ostringstream path;
    path << "/tmp/.X11-unix/X" << display_number;
    int fd = connect_unix( path.str() );
    if ( fd >= 0 ) {
      return fd;
    }
    return connect_tcp( "127.0.0.1", 6000 + display_number );
  }

  return connect_tcp( host, 6000 + display_number );
}

void StreamForwarder::append_stream_data( Stream& stream, const std::string& data )
{
  if ( !stream.x11 || stream.x11_auth_rewritten || stream.x11_peer_auth_data.empty() || local_x11_auth_data.empty() ) {
    stream.to_local += data;
    return;
  }

  stream.x11_setup_buffer += data;
  rewrite_x11_setup( stream );
}

bool StreamForwarder::rewrite_x11_setup( Stream& stream )
{
  std::string& buffer = stream.x11_setup_buffer;
  if ( buffer.size() < 12 ) {
    return false;
  }

  const unsigned char byte_order = static_cast<unsigned char>( buffer[0] );
  const bool little = byte_order == 'l';
  const bool big = byte_order == 'B';
  if ( !little && !big ) {
    stream.to_local += buffer;
    buffer.clear();
    stream.x11_auth_rewritten = true;
    return true;
  }

  auto read16 = [little]( const std::string& bytes, size_t offset ) -> uint16_t {
    if ( little ) {
      return static_cast<uint16_t>( static_cast<unsigned char>( bytes[offset] ) )
             | ( static_cast<uint16_t>( static_cast<unsigned char>( bytes[offset + 1] ) ) << 8 );
    }
    return ( static_cast<uint16_t>( static_cast<unsigned char>( bytes[offset] ) ) << 8 )
           | static_cast<unsigned char>( bytes[offset + 1] );
  };

  const uint16_t auth_name_len = read16( buffer, 6 );
  const uint16_t auth_data_len = read16( buffer, 8 );
  const size_t auth_name_offset = 12;
  const size_t auth_data_offset = auth_name_offset + pad4( auth_name_len );
  const size_t packet_len = auth_data_offset + pad4( auth_data_len );
  if ( buffer.size() < packet_len ) {
    return false;
  }

  std::string packet = buffer.substr( 0, packet_len );
  std::string rest = buffer.substr( packet_len );
  buffer.clear();

  if ( auth_name_offset + auth_name_len <= packet.size() && auth_data_offset + auth_data_len <= packet.size() ) {
    const std::string auth_name( packet.data() + auth_name_offset, auth_name_len );
    const std::string auth_data( packet.data() + auth_data_offset, auth_data_len );
    if ( auth_name == "MIT-MAGIC-COOKIE-1" && auth_data == stream.x11_peer_auth_data
         && local_x11_auth_data.size() == auth_data_len ) {
      packet.replace( auth_data_offset, auth_data_len, local_x11_auth_data );
    }
  }

  stream.to_local += packet;
  stream.to_local += rest;
  stream.x11_auth_rewritten = true;
  return true;
}

void StreamForwarder::queue_control( const Network::StreamEvent& event )
{
  if ( event.priority == Network::StreamPriorityMedium ) {
    pending_medium_control.push_back( event );
  } else {
    pending_low_control.push_back( event );
  }
}

void StreamForwarder::try_write_local( void )
{
  for ( std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); it++ ) {
    Stream& stream = it->second;
    while ( stream.fd >= 0 && !stream.to_local.empty() ) {
      ssize_t bytes = write( stream.fd, stream.to_local.data(), stream.to_local.size() );
      if ( bytes > 0 ) {
        stream.to_local.erase( 0, bytes );
      } else if ( bytes < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) ) {
        break;
      } else {
        close_stream( stream, true );
        break;
      }
    }
  }
}

void StreamForwarder::close_stream( Stream& stream, bool notify_peer )
{
  if ( stream.closed ) {
    return;
  }

  if ( stream.fd >= 0 ) {
    fd_to_stream.erase( stream.fd );
    close( stream.fd );
    stream.fd = -1;
  }
  stream.closed = true;
  stream.to_local.clear();
  stream.to_network.clear();
  stream.local_eof_pending = false;
  if ( notify_peer ) {
    queue_control( Network::StreamEvent::close( stream.id, stream.priority ) );
  }
}

void StreamForwarder::refill_tokens( uint64_t now, size_t chunk_cap )
{
  if ( now < last_token_update ) {
    last_token_update = now;
  }

  double elapsed = static_cast<double>( now - last_token_update ) / 1000.0;
  double capacity = std::max<double>( stream_rate_bytes_per_second, chunk_cap );
  for ( int priority = Network::StreamPriorityLow; priority <= Network::StreamPriorityMedium; priority++ ) {
    Network::StreamPriority stream_priority = static_cast<Network::StreamPriority>( priority );
    stream_tokens_for( stream_priority ) += elapsed * stream_rate_bytes_per_second;
    if ( stream_tokens_for( stream_priority ) > capacity ) {
      stream_tokens_for( stream_priority ) = capacity;
    }
  }
  last_token_update = now;
}

size_t StreamForwarder::stream_chunk_cap( unsigned int send_interval, size_t max_datagram_payload ) const
{
  size_t interval_budget = static_cast<size_t>(
    ( static_cast<uint64_t>( stream_rate_bytes_per_second ) * std::max<unsigned int>( send_interval, 1 ) ) / 1000 );
  interval_budget = std::max<size_t>( interval_budget, MIN_STREAM_CHUNK );

  size_t datagram_budget = std::max<size_t>( 1, max_datagram_payload / 2 );
  if ( max_datagram_payload > STREAM_DATAGRAM_OVERHEAD_ALLOWANCE ) {
    datagram_budget = max_datagram_payload - STREAM_DATAGRAM_OVERHEAD_ALLOWANCE;
  }

  return std::max<size_t>( 1, std::min( std::min( interval_budget, MAX_STREAM_CHUNK ), datagram_budget ) );
}
