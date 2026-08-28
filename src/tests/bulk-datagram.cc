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

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}
}

int main( void )
{
  try {
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
