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

#include <cstring>
#include <limits>

using namespace Network::Bulk;

namespace {
const char MAGIC[] = { 'M', 'O', 'S', 'H', 'C', 'P', '1', '\0' };
const size_t MAGIC_LEN = sizeof( MAGIC );
const uint8_t WIRE_VERSION = 1;
const size_t HEADER_LEN = MAGIC_LEN + 1 + 1 + 8 + 4 + 4 + 4;

void append_u8( std::string& out, uint8_t value )
{
  out.push_back( static_cast<char>( value ) );
}

void append_u32( std::string& out, uint32_t value )
{
  out.push_back( static_cast<char>( ( value >> 24 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 16 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
}

void append_u64( std::string& out, uint64_t value )
{
  for ( int shift = 56; shift >= 0; shift -= 8 ) {
    out.push_back( static_cast<char>( ( value >> shift ) & 0xff ) );
  }
}

bool read_u8( const std::string& in, size_t& offset, uint8_t& value )
{
  if ( offset + 1 > in.size() ) {
    return false;
  }
  value = static_cast<uint8_t>( in[offset] );
  offset++;
  return true;
}

bool read_u32( const std::string& in, size_t& offset, uint32_t& value )
{
  if ( offset + 4 > in.size() ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 4; i++ ) {
    value = ( value << 8 ) | static_cast<uint8_t>( in[offset + i] );
  }
  offset += 4;
  return true;
}

bool read_u64( const std::string& in, size_t& offset, uint64_t& value )
{
  if ( offset + 8 > in.size() ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 8; i++ ) {
    value = ( value << 8 ) | static_cast<uint8_t>( in[offset + i] );
  }
  offset += 8;
  return true;
}
}

Datagram::Datagram() : type( PacketType::Manifest ), transfer_id( 0 ), block_id( 0 ), symbol_id( 0 ), payload() {}

std::string Network::Bulk::encode_datagram( const Datagram& datagram )
{
  if ( datagram.payload.size() > std::numeric_limits<uint32_t>::max() ) {
    throw NetworkException( "bulk datagram payload too large", 0 );
  }

  std::string out;
  out.reserve( HEADER_LEN + datagram.payload.size() );
  out.append( MAGIC, MAGIC_LEN );
  append_u8( out, WIRE_VERSION );
  append_u8( out, static_cast<uint8_t>( datagram.type ) );
  append_u64( out, datagram.transfer_id );
  append_u32( out, datagram.block_id );
  append_u32( out, datagram.symbol_id );
  append_u32( out, static_cast<uint32_t>( datagram.payload.size() ) );
  out.append( datagram.payload );
  return out;
}

bool Network::Bulk::decode_datagram( const std::string& packet, Datagram& datagram )
{
  if ( packet.size() < HEADER_LEN || memcmp( packet.data(), MAGIC, MAGIC_LEN ) != 0 ) {
    return false;
  }

  size_t offset = MAGIC_LEN;
  uint8_t version = 0;
  uint8_t type = 0;
  uint32_t payload_size = 0;
  if ( !read_u8( packet, offset, version ) || version != WIRE_VERSION ) {
    return false;
  }
  if ( !read_u8( packet, offset, type ) || type < static_cast<uint8_t>( PacketType::Manifest )
       || type > static_cast<uint8_t>( PacketType::FileAck ) ) {
    return false;
  }
  if ( !read_u64( packet, offset, datagram.transfer_id ) || !read_u32( packet, offset, datagram.block_id )
       || !read_u32( packet, offset, datagram.symbol_id ) || !read_u32( packet, offset, payload_size ) ) {
    return false;
  }
  if ( packet.size() - offset != payload_size ) {
    return false;
  }

  datagram.type = static_cast<PacketType>( type );
  datagram.payload.assign( packet.data() + offset, payload_size );
  return true;
}

SecureDatagramChannel::SecureDatagramChannel( Connection& s_connection ) : connection( s_connection ) {}

void SecureDatagramChannel::send( const Datagram& datagram )
{
  const std::string encoded = encode_datagram( datagram );
  const int max_payload = connection.get_MTU() - connection.packet_overhead();
  if ( max_payload <= 0 || encoded.size() > static_cast<size_t>( max_payload ) ) {
    throw NetworkException( "bulk datagram too large for path MTU", 0 );
  }
  connection.send( encoded );
}

bool SecureDatagramChannel::recv( Datagram& datagram )
{
  return decode_datagram( connection.recv(), datagram );
}
