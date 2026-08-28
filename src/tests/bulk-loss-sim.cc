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

#include "src/fec/fec.h"
#include "src/network/bulkdatagram.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
std::string fixture( size_t size )
{
  std::string data;
  data.reserve( size );
  for ( size_t i = 0; i < size; i++ ) {
    data.push_back( static_cast<char>( ( i * 29 + i / 5 ) & 0xff ) );
  }
  return data;
}

void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

bool deterministic_drop( uint32_t symbol_id, unsigned int loss_percent )
{
  return ( ( symbol_id * 1103515245U + 12345U ) % 100U ) < loss_percent;
}

std::vector<FEC::Symbol> simulate_link( const std::vector<FEC::Symbol>& symbols, unsigned int loss_percent )
{
  std::vector<Network::Bulk::Datagram> datagrams;
  for ( std::vector<FEC::Symbol>::const_iterator it = symbols.begin(); it != symbols.end(); ++it ) {
    if ( deterministic_drop( it->id, loss_percent ) ) {
      continue;
    }

    Network::Bulk::Datagram datagram;
    datagram.type = Network::Bulk::PacketType::Symbol;
    datagram.transfer_id = 0x1234;
    datagram.block_id = 7;
    datagram.symbol_id = it->id;
    datagram.payload = it->payload;
    datagrams.push_back( datagram );
  }

  std::reverse( datagrams.begin(), datagrams.end() );
  std::vector<FEC::Symbol> received;
  for ( std::vector<Network::Bulk::Datagram>::const_iterator it = datagrams.begin(); it != datagrams.end(); ++it ) {
    const std::string wire = Network::Bulk::encode_datagram( *it );
    Network::Bulk::Datagram decoded;
    require( Network::Bulk::decode_datagram( wire, decoded ), "bulk datagram decode failed" );
    received.push_back( FEC::Symbol( decoded.symbol_id, decoded.payload ) );
  }
  return received;
}

void test_loss_rate( FEC::CodecKind kind, unsigned int loss_percent )
{
  std::unique_ptr<FEC::BlockCodec> codec = FEC::make_codec( kind );
  const std::string input = fixture( 8192 );
  const FEC::EncodedBlock encoded = codec->encode( input, 128, 128 );
  const std::vector<FEC::Symbol> received = simulate_link( encoded.symbols, loss_percent );

  std::string decoded;
  const std::string name = FEC::codec_name( kind );
  require( codec->decode( encoded.metadata, received, decoded ), ( name + " decode failed under loss" ).c_str() );
  require( decoded == input, ( name + " decoded wrong bytes under loss" ).c_str() );
}
}

int main( void )
{
  try {
    const unsigned int losses[] = { 0, 5, 20, 40 };
    for ( size_t i = 0; i < sizeof( losses ) / sizeof( losses[0] ); i++ ) {
      test_loss_rate( FEC::CodecKind::ReedSolomon, losses[i] );
      if ( FEC::codec_available( FEC::CodecKind::RaptorQ ) ) {
        test_loss_rate( FEC::CodecKind::RaptorQ, losses[i] );
      }
    }
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
