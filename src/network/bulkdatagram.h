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

#ifndef NETWORK_BULKDATAGRAM_H
#define NETWORK_BULKDATAGRAM_H

#include <cstdint>
#include <string>

#include "src/network/network.h"

namespace Network {
namespace Bulk {
enum class PacketType : uint8_t
{
  Manifest = 1,
  Symbol = 2,
  Ack = 3,
  Finish = 4
};

struct Datagram
{
  PacketType type;
  uint64_t transfer_id;
  uint32_t block_id;
  uint32_t symbol_id;
  std::string payload;

  Datagram();
};

std::string encode_datagram( const Datagram& datagram );
bool decode_datagram( const std::string& packet, Datagram& datagram );

class SecureDatagramChannel
{
private:
  Connection& connection;

public:
  explicit SecureDatagramChannel( Connection& s_connection );

  void send( const Datagram& datagram );
  bool recv( Datagram& datagram );
};
}
}

#endif
