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

#include "src/moshcp/protocol.h"

#include <cstring>
#include <limits>
#include <stdexcept>

using namespace MoshCP;

namespace {
const char MANIFEST_MAGIC[] = { 'M', 'C', 'P', 'M', 2 };
const char ACK_MAGIC[] = { 'M', 'C', 'P', 'A', 2 };
const char FINISH_MAGIC[] = { 'M', 'C', 'P', 'F', 1 };

void append_u8( std::string& out, uint8_t value )
{
  out.push_back( static_cast<char>( value ) );
}

void append_u16( std::string& out, uint16_t value )
{
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
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

void append_i64( std::string& out, int64_t value )
{
  append_u64( out, static_cast<uint64_t>( value ) );
}

void append_string( std::string& out, const std::string& value )
{
  if ( value.size() > std::numeric_limits<uint32_t>::max() ) {
    throw std::runtime_error( "moshcp protocol string too large" );
  }
  append_u32( out, static_cast<uint32_t>( value.size() ) );
  out.append( value );
}

bool read_u8( const std::string& in, size_t& offset, uint8_t& value )
{
  if ( offset + 1 > in.size() ) {
    return false;
  }
  value = static_cast<uint8_t>( in[offset++] );
  return true;
}

bool read_u16( const std::string& in, size_t& offset, uint16_t& value )
{
  if ( offset + 2 > in.size() ) {
    return false;
  }
  value = ( static_cast<uint16_t>( static_cast<uint8_t>( in[offset] ) ) << 8 )
          | static_cast<uint8_t>( in[offset + 1] );
  offset += 2;
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

bool read_i64( const std::string& in, size_t& offset, int64_t& value )
{
  uint64_t raw = 0;
  if ( !read_u64( in, offset, raw ) ) {
    return false;
  }
  value = static_cast<int64_t>( raw );
  return true;
}

bool read_string( const std::string& in, size_t& offset, std::string& value )
{
  uint32_t size = 0;
  if ( !read_u32( in, offset, size ) || offset + size > in.size() ) {
    return false;
  }
  value.assign( in.data() + offset, size );
  offset += size;
  return true;
}

bool check_magic( const std::string& in, size_t& offset, const char* magic, size_t size )
{
  if ( in.size() < size || memcmp( in.data(), magic, size ) != 0 ) {
    return false;
  }
  offset = size;
  return true;
}
}

BlockInfo::BlockInfo() : original_size( 0 ), source_symbols( 0 ), raptorq_oti_common( 0 ), raptorq_oti_scheme( 0 ) {}

Manifest::Manifest()
  : filename(), size( 0 ), uncompressed_size( 0 ), mode( 0644 ), mtime( 0 ), block_size( 0 ), symbol_size( 0 ),
    codec( FEC::CodecKind::ReedSolomon ), archive( false ), preserve_permissions( false ), zstd_compressed( false ),
    zstd_level( 0 ), sha256_hex(), blocks()
{}

RepairRequest::RepairRequest() : block_id( 0 ), extra_symbols( 0 ) {}

RepairRequest::RepairRequest( uint32_t s_block_id, uint32_t s_extra_symbols )
  : block_id( s_block_id ), extra_symbols( s_extra_symbols )
{}

Ack::Ack()
  : decoded_ranges(), repair_requests(), decoded_blocks( 0 ), received_symbols( 0 ), duplicate_symbols( 0 ),
    repair_symbols_requested( 0 )
{}

Finish::Finish() : success( false ), sha256_hex(), message() {}

std::string MoshCP::encode_manifest( const Manifest& manifest )
{
  std::string out( MANIFEST_MAGIC, sizeof MANIFEST_MAGIC );
  append_string( out, manifest.filename );
  append_u64( out, manifest.size );
  append_u64( out, manifest.uncompressed_size );
  append_u32( out, manifest.mode );
  append_i64( out, manifest.mtime );
  append_u32( out, manifest.block_size );
  append_u16( out, manifest.symbol_size );
  append_u8( out, static_cast<uint8_t>( manifest.codec ) );
  append_u8( out, manifest.archive ? 1 : 0 );
  append_u8( out, manifest.preserve_permissions ? 1 : 0 );
  append_u8( out, manifest.zstd_compressed ? 1 : 0 );
  append_u32( out, manifest.zstd_level );
  append_string( out, manifest.sha256_hex );
  append_u32( out, static_cast<uint32_t>( manifest.blocks.size() ) );
  for ( std::vector<BlockInfo>::const_iterator it = manifest.blocks.begin(); it != manifest.blocks.end(); ++it ) {
    append_u64( out, it->original_size );
    append_u16( out, it->source_symbols );
    append_u64( out, it->raptorq_oti_common );
    append_u32( out, it->raptorq_oti_scheme );
  }
  return out;
}

bool MoshCP::decode_manifest( const std::string& payload, Manifest& manifest )
{
  size_t offset = 0;
  if ( !check_magic( payload, offset, MANIFEST_MAGIC, sizeof MANIFEST_MAGIC ) ) {
    return false;
  }
  uint8_t codec = 0;
  uint8_t archive = 0;
  uint8_t preserve_permissions = 0;
  uint8_t zstd_compressed = 0;
  uint32_t blocks = 0;
  if ( !read_string( payload, offset, manifest.filename ) || !read_u64( payload, offset, manifest.size )
       || !read_u64( payload, offset, manifest.uncompressed_size ) || !read_u32( payload, offset, manifest.mode )
       || !read_i64( payload, offset, manifest.mtime )
       || !read_u32( payload, offset, manifest.block_size ) || !read_u16( payload, offset, manifest.symbol_size )
       || !read_u8( payload, offset, codec ) || !read_u8( payload, offset, archive )
       || !read_u8( payload, offset, preserve_permissions ) || !read_u8( payload, offset, zstd_compressed )
       || !read_u32( payload, offset, manifest.zstd_level ) || !read_string( payload, offset, manifest.sha256_hex )
       || !read_u32( payload, offset, blocks ) ) {
    return false;
  }
  if ( codec > static_cast<uint8_t>( FEC::CodecKind::RaptorQ ) ) {
    return false;
  }
  manifest.codec = static_cast<FEC::CodecKind>( codec );
  manifest.archive = archive != 0;
  manifest.preserve_permissions = preserve_permissions != 0;
  manifest.zstd_compressed = zstd_compressed != 0;
  manifest.blocks.clear();
  manifest.blocks.reserve( blocks );
  for ( uint32_t i = 0; i < blocks; i++ ) {
    BlockInfo block;
    if ( !read_u64( payload, offset, block.original_size ) || !read_u16( payload, offset, block.source_symbols )
         || !read_u64( payload, offset, block.raptorq_oti_common )
         || !read_u32( payload, offset, block.raptorq_oti_scheme ) ) {
      return false;
    }
    manifest.blocks.push_back( block );
  }
  return offset == payload.size();
}

std::string MoshCP::encode_ack( const Ack& ack )
{
  std::string out( ACK_MAGIC, sizeof ACK_MAGIC );
  append_u32( out, static_cast<uint32_t>( ack.decoded_ranges.size() ) );
  for ( std::vector<std::pair<uint32_t, uint32_t>>::const_iterator it = ack.decoded_ranges.begin();
        it != ack.decoded_ranges.end();
        ++it ) {
    append_u32( out, it->first );
    append_u32( out, it->second );
  }
  append_u32( out, static_cast<uint32_t>( ack.repair_requests.size() ) );
  for ( std::vector<RepairRequest>::const_iterator it = ack.repair_requests.begin(); it != ack.repair_requests.end(); ++it ) {
    append_u32( out, it->block_id );
    append_u32( out, it->extra_symbols );
  }
  append_u32( out, ack.decoded_blocks );
  append_u64( out, ack.received_symbols );
  append_u64( out, ack.duplicate_symbols );
  append_u64( out, ack.repair_symbols_requested );
  return out;
}

bool MoshCP::decode_ack( const std::string& payload, Ack& ack )
{
  size_t offset = 0;
  uint32_t ranges = 0;
  uint32_t repairs = 0;
  if ( !check_magic( payload, offset, ACK_MAGIC, sizeof ACK_MAGIC ) || !read_u32( payload, offset, ranges ) ) {
    return false;
  }
  ack.decoded_ranges.clear();
  ack.decoded_ranges.reserve( ranges );
  for ( uint32_t i = 0; i < ranges; i++ ) {
    uint32_t first = 0;
    uint32_t second = 0;
    if ( !read_u32( payload, offset, first ) || !read_u32( payload, offset, second ) || second < first ) {
      return false;
    }
    ack.decoded_ranges.push_back( std::make_pair( first, second ) );
  }
  if ( !read_u32( payload, offset, repairs ) ) {
    return false;
  }
  ack.repair_requests.clear();
  ack.repair_requests.reserve( repairs );
  for ( uint32_t i = 0; i < repairs; i++ ) {
    RepairRequest request;
    if ( !read_u32( payload, offset, request.block_id ) || !read_u32( payload, offset, request.extra_symbols ) ) {
      return false;
    }
    ack.repair_requests.push_back( request );
  }
  if ( !read_u32( payload, offset, ack.decoded_blocks ) || !read_u64( payload, offset, ack.received_symbols )
       || !read_u64( payload, offset, ack.duplicate_symbols )
       || !read_u64( payload, offset, ack.repair_symbols_requested ) ) {
    return false;
  }
  return offset == payload.size();
}

std::string MoshCP::encode_finish( const Finish& finish )
{
  std::string out( FINISH_MAGIC, sizeof FINISH_MAGIC );
  append_u8( out, finish.success ? 1 : 0 );
  append_string( out, finish.sha256_hex );
  append_string( out, finish.message );
  return out;
}

bool MoshCP::decode_finish( const std::string& payload, Finish& finish )
{
  size_t offset = 0;
  uint8_t success = 0;
  if ( !check_magic( payload, offset, FINISH_MAGIC, sizeof FINISH_MAGIC ) || !read_u8( payload, offset, success )
       || !read_string( payload, offset, finish.sha256_hex ) || !read_string( payload, offset, finish.message ) ) {
    return false;
  }
  finish.success = success != 0;
  return offset == payload.size();
}
