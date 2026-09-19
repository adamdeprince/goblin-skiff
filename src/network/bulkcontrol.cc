/*
    Modified for Goblin Skiff on 2026-09-19.
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

#include "src/network/bulkcontrol.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Network::Bulk;

namespace {
const uint32_t MAX_FRAME = 1024 * 1024;
const size_t MAX_QUEUED_DATAGRAMS = 16;

void append_u32( std::string& out, uint32_t value )
{
  out.push_back( static_cast<char>( ( value >> 24 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 16 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
}

bool read_u32( const std::string& in, uint32_t& value )
{
  if ( in.size() < 4 ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 4; i++ ) {
    value = ( value << 8 ) | static_cast<uint8_t>( in[i] );
  }
  return true;
}

std::string frame_datagram( const Datagram& datagram )
{
  const std::string wire = encode_datagram( datagram );
  if ( wire.size() > MAX_FRAME ) {
    throw std::runtime_error( "bulk control frame too large" );
  }
  std::string frame;
  frame.reserve( wire.size() + 4 );
  append_u32( frame, static_cast<uint32_t>( wire.size() ) );
  frame.append( wire );
  return frame;
}

std::string runtime_dir( void )
{
  const char* xdg = getenv( "XDG_RUNTIME_DIR" );
  if ( xdg && *xdg ) {
    return xdg;
  }

  char fallback[128];
  snprintf( fallback, sizeof fallback, "/tmp/goblin-skiffcp-%ld", static_cast<long>( getuid() ) );
  if ( mkdir( fallback, 0700 ) < 0 && errno != EEXIST ) {
    throw std::runtime_error( std::string( "mkdir: " ) + strerror( errno ) );
  }
  chmod( fallback, 0700 );
  return fallback;
}

void set_nonblocking( int fd )
{
  int flags = fcntl( fd, F_GETFL, 0 );
  if ( flags < 0 || fcntl( fd, F_SETFL, flags | O_NONBLOCK ) < 0 ) {
    throw std::runtime_error( std::string( "fcntl: " ) + strerror( errno ) );
  }
  int fdflags = fcntl( fd, F_GETFD, 0 );
  if ( fdflags < 0 || fcntl( fd, F_SETFD, fdflags | FD_CLOEXEC ) < 0 ) {
    throw std::runtime_error( std::string( "fcntl: " ) + strerror( errno ) );
  }
}

sockaddr_un socket_address( const std::string& path )
{
  sockaddr_un addr;
  memset( &addr, 0, sizeof addr );
  addr.sun_family = AF_UNIX;
  if ( path.size() >= sizeof addr.sun_path ) {
    throw std::runtime_error( "bulk control socket path is too long" );
  }
  strncpy( addr.sun_path, path.c_str(), sizeof addr.sun_path - 1 );
  return addr;
}

void write_all( int fd, const std::string& frame )
{
  size_t offset = 0;
  while ( offset < frame.size() ) {
    ssize_t written = write( fd, frame.data() + offset, frame.size() - offset );
    if ( written < 0 && errno == EINTR ) {
      continue;
    }
    if ( written < 0 ) {
      throw std::runtime_error( std::string( "write: " ) + strerror( errno ) );
    }
    if ( written == 0 ) {
      throw std::runtime_error( "short write to bulk control socket" );
    }
    offset += written;
  }
}

void read_exact( int fd, char* buf, size_t size )
{
  size_t offset = 0;
  while ( offset < size ) {
    ssize_t got = read( fd, buf + offset, size - offset );
    if ( got < 0 && errno == EINTR ) {
      continue;
    }
    if ( got < 0 ) {
      throw std::runtime_error( std::string( "read: " ) + strerror( errno ) );
    }
    if ( got == 0 ) {
      throw std::runtime_error( "bulk control socket closed" );
    }
    offset += got;
  }
}
}

ControlServer::Client::Client() : fd( -1 ), in() {}

ControlServer::Client::Client( int s_fd ) : fd( s_fd ), in() {}

ControlServer::ControlServer( const std::string& role )
  : listen_fd( -1 ), path(), latest_path(), clients(), outgoing()
{
  const std::string dir = runtime_dir();
  char pathbuf[256];
  snprintf( pathbuf, sizeof pathbuf, "%s/goblin-skiffcp-%s-%ld.sock", dir.c_str(), role.c_str(), static_cast<long>( getpid() ) );
  path = pathbuf;
  latest_path = dir + "/goblin-skiffcp.latest";

  listen_fd = socket( AF_UNIX, SOCK_STREAM, 0 );
  if ( listen_fd < 0 ) {
    throw std::runtime_error( std::string( "socket: " ) + strerror( errno ) );
  }
  set_nonblocking( listen_fd );

  unlink( path.c_str() );
  sockaddr_un addr = socket_address( path );
  if ( bind( listen_fd, reinterpret_cast<sockaddr*>( &addr ), sizeof addr ) < 0 ) {
    const int saved_errno = errno;
    close( listen_fd );
    listen_fd = -1;
    throw std::runtime_error( std::string( "bind: " ) + strerror( saved_errno ) );
  }
  chmod( path.c_str(), 0600 );
  if ( listen( listen_fd, 16 ) < 0 ) {
    const int saved_errno = errno;
    close( listen_fd );
    unlink( path.c_str() );
    listen_fd = -1;
    throw std::runtime_error( std::string( "listen: " ) + strerror( saved_errno ) );
  }

  unlink( latest_path.c_str() );
  if ( symlink( path.c_str(), latest_path.c_str() ) < 0 && errno != EEXIST ) {
    latest_path.clear();
  }
}

ControlServer::~ControlServer()
{
  for ( std::map<int, Client>::iterator it = clients.begin(); it != clients.end(); ++it ) {
    close( it->first );
  }
  clients.clear();
  if ( listen_fd >= 0 ) {
    close( listen_fd );
  }
  if ( !path.empty() ) {
    unlink( path.c_str() );
  }
  if ( !latest_path.empty() ) {
    char target[512];
    ssize_t len = readlink( latest_path.c_str(), target, sizeof target - 1 );
    if ( len >= 0 ) {
      target[len] = '\0';
      if ( path == target ) {
        unlink( latest_path.c_str() );
      }
    }
  }
}

std::vector<int> ControlServer::fds( void ) const
{
  std::vector<int> result;
  if ( listen_fd >= 0 ) {
    result.push_back( listen_fd );
  }
  for ( std::map<int, Client>::const_iterator it = clients.begin(); it != clients.end(); ++it ) {
    // Let the Unix stream backpressure an unlimited-rate local producer.
    if ( outgoing.size() < MAX_QUEUED_DATAGRAMS ) { result.push_back( it->first ); }
  }
  return result;
}

void ControlServer::close_client( int fd )
{
  std::map<int, Client>::iterator it = clients.find( fd );
  if ( it != clients.end() ) {
    close( fd );
    clients.erase( it );
  }
}

void ControlServer::accept_client( void )
{
  while ( true ) {
    int fd = accept( listen_fd, NULL, NULL );
    if ( fd < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) {
      return;
    }
    if ( fd < 0 && errno == EINTR ) {
      continue;
    }
    if ( fd < 0 ) {
      throw std::runtime_error( std::string( "accept: " ) + strerror( errno ) );
    }
    set_nonblocking( fd );
    if ( clients.size() >= 16 ) { close( fd ); continue; }
    clients.insert( std::make_pair( fd, Client( fd ) ) );
  }
}

void ControlServer::read_client( int fd )
{
  if ( outgoing.size() >= MAX_QUEUED_DATAGRAMS ) { return; }
  std::map<int, Client>::iterator client = clients.find( fd );
  if ( client == clients.end() ) {
    return;
  }

  char buf[8192];
  while ( client->second.in.size() < MAX_FRAME + 4 ) {
    ssize_t got = read( fd, buf, sizeof buf );
    if ( got < 0 && errno == EINTR ) {
      continue;
    }
    if ( got < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) {
      break;
    }
    if ( got <= 0 ) {
      close_client( fd );
      return;
    }
    client->second.in.append( buf, got );
    break; // Bounded work per event-loop turn, including --rate=0 writers.
  }

  while ( client->second.in.size() >= 4 && outgoing.size() < MAX_QUEUED_DATAGRAMS ) {
    uint32_t frame_size = 0;
    read_u32( client->second.in, frame_size );
    if ( frame_size > MAX_FRAME ) {
      close_client( fd );
      return;
    }
    if ( client->second.in.size() < 4 + frame_size ) {
      return;
    }
    const std::string frame = client->second.in.substr( 4, frame_size );
    client->second.in.erase( 0, 4 + frame_size );
    Datagram datagram;
    if ( decode_datagram( frame, datagram ) ) {
      outgoing.push_back( datagram );
    }
  }
}

void ControlServer::process_readable_fd( int fd )
{
  if ( fd == listen_fd ) {
    accept_client();
  } else {
    read_client( fd );
  }
}

bool ControlServer::pop_outgoing( Datagram& datagram )
{
  if ( outgoing.empty() ) {
    return false;
  }
  datagram = outgoing.front();
  outgoing.pop_front();
  // A complete frame may already be buffered even when the fd no longer
  // signals readable. Service that backlog when queue space becomes free.
  std::vector<int> buffered;
  for ( const auto& client : clients ) { if ( !client.second.in.empty() ) { buffered.push_back( client.first ); } }
  for ( int fd : buffered ) { read_client( fd ); }
  return true;
}

void ControlServer::broadcast( const Datagram& datagram )
{
  const std::string frame = frame_datagram( datagram );
  std::vector<int> failed;
  for ( std::map<int, Client>::iterator it = clients.begin(); it != clients.end(); ++it ) {
    try {
      write_all( it->first, frame );
    } catch ( const std::exception& ) {
      failed.push_back( it->first );
    }
  }
  for ( std::vector<int>::const_iterator it = failed.begin(); it != failed.end(); ++it ) {
    close_client( *it );
  }
}

ControlClient::ControlClient( const std::string& path ) : sock_fd( -1 )
{
  sock_fd = socket( AF_UNIX, SOCK_STREAM, 0 );
  if ( sock_fd < 0 ) {
    throw std::runtime_error( std::string( "socket: " ) + strerror( errno ) );
  }

  sockaddr_un addr = socket_address( path );
  if ( connect( sock_fd, reinterpret_cast<sockaddr*>( &addr ), sizeof addr ) < 0 ) {
    const int saved_errno = errno;
    close( sock_fd );
    sock_fd = -1;
    throw std::runtime_error( std::string( "connect: " ) + strerror( saved_errno ) );
  }
}

ControlClient::~ControlClient()
{
  if ( sock_fd >= 0 ) {
    close( sock_fd );
  }
}

void ControlClient::send( const Datagram& datagram )
{
  write_all( sock_fd, frame_datagram( datagram ) );
}

bool ControlClient::recv( Datagram& datagram )
{
  char header[4];
  read_exact( sock_fd, header, sizeof header );
  std::string header_str( header, sizeof header );
  uint32_t frame_size = 0;
  read_u32( header_str, frame_size );
  if ( frame_size > MAX_FRAME ) {
    throw std::runtime_error( "bulk control frame too large" );
  }

  std::string frame( frame_size, '\0' );
  if ( frame_size ) {
    read_exact( sock_fd, &frame[0], frame.size() );
  }
  return decode_datagram( frame, datagram );
}

std::string Network::Bulk::discover_control_socket( void )
{
  const char* env = getenv( "GOBLIN_SKIFF_CP_SOCK" );
  if ( env && *env ) {
    return env;
  }
  return runtime_dir() + "/goblin-skiffcp.latest";
}
