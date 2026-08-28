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

#ifndef NETWORK_BULKCONTROL_H
#define NETWORK_BULKCONTROL_H

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "src/network/bulkdatagram.h"

namespace Network {
namespace Bulk {
class ControlServer
{
private:
  struct Client
  {
    int fd;
    std::string in;

    Client();
    explicit Client( int s_fd );
  };

  int listen_fd;
  std::string path;
  std::string latest_path;
  std::map<int, Client> clients;
  std::deque<Datagram> outgoing;

  void close_client( int fd );
  void accept_client( void );
  void read_client( int fd );

public:
  explicit ControlServer( const std::string& role );
  ~ControlServer();

  ControlServer( const ControlServer& );
  ControlServer& operator=( const ControlServer& );

  const std::string& socket_path( void ) const { return path; }
  std::vector<int> fds( void ) const;
  void process_readable_fd( int fd );
  bool has_outgoing( void ) const { return !outgoing.empty(); }
  bool pop_outgoing( Datagram& datagram );
  void broadcast( const Datagram& datagram );
};

class ControlClient
{
private:
  int sock_fd;

public:
  explicit ControlClient( const std::string& path );
  ~ControlClient();

  ControlClient( const ControlClient& );
  ControlClient& operator=( const ControlClient& );

  int fd( void ) const { return sock_fd; }
  void send( const Datagram& datagram );
  bool recv( Datagram& datagram );
};

std::string discover_control_socket( void );
}
}

#endif
