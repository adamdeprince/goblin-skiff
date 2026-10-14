/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

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

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include <cassert>
#include <cstring>
#include <stdexcept>

#include "compressor.h"
#include "src/network/statesamples.h"
#include "src/crypto/byteorder.h"
#include "src/protobufs/transportinstruction.pb.h"
#include "src/util/fatal_assert.h"
#include "transportfragment.h"

using namespace Network;
using namespace TransportBuffers;

static const uint64_t LARGE_FRAGMENT_FLAG = uint64_t( 1 ) << 63;

static std::string network_order_string( uint16_t host_order )
{
  uint16_t net_int = htobe16( host_order );
  return std::string( (char*)&net_int, sizeof( net_int ) );
}

static std::string network_order_string( uint64_t host_order )
{
  uint64_t net_int = htobe64( host_order );
  return std::string( (char*)&net_int, sizeof( net_int ) );
}

std::string Fragment::tostring( void )
{
  assert( initialized );

  std::string ret;

  fatal_assert( !( id & LARGE_FRAGMENT_FLAG ) );
  ret += network_order_string( id | ( large ? LARGE_FRAGMENT_FLAG : 0 ) );
  if ( large ) {
    fatal_assert( !( fragment_num & LARGE_FRAGMENT_FLAG ) );
    ret += network_order_string( fragment_num | ( final ? LARGE_FRAGMENT_FLAG : 0 ) );
  } else {
    fatal_assert( fragment_num < 32768 );
    ret += network_order_string( uint16_t( ( final << 15 ) | fragment_num ) );
  }

  ret += contents;

  return ret;
}

Fragment::Fragment( const std::string& x )
  : id( -1 ), fragment_num( -1 ), final( false ), large( false ), initialized( true ), contents()
{
  fatal_assert( x.size() >= frag_header_len );
  uint64_t data64;
  memcpy( &data64, x.data(), sizeof( data64 ) );
  id = be64toh( data64 );
  large = id & LARGE_FRAGMENT_FLAG;
  id &= ~LARGE_FRAGMENT_FLAG;
  if ( large ) {
    fatal_assert( x.size() >= large_frag_header_len );
    memcpy( &data64, x.data() + sizeof( uint64_t ), sizeof( data64 ) );
    fragment_num = be64toh( data64 );
    final = fragment_num & LARGE_FRAGMENT_FLAG;
    fragment_num &= ~LARGE_FRAGMENT_FLAG;
  } else {
    uint16_t data16;
    memcpy( &data16, x.data() + sizeof( uint64_t ), sizeof( data16 ) );
    fragment_num = be16toh( data16 );
    final = ( fragment_num & 0x8000 ) != 0;
    fragment_num &= 0x7FFF;
  }
  contents.assign( x, large ? large_frag_header_len : frag_header_len, std::string::npos );
}

bool FragmentAssembly::add_fragment( Fragment& frag )
{
  /* see if this is a totally new packet */
  if ( current_id != frag.id ) {
    fragments.clear();
    fragments_total = 0; /* unknown */
    current_id = frag.id;
  }
  const auto inserted = fragments.emplace( frag.fragment_num, frag );
  assert( inserted.second || inserted.first->second == frag );
  if ( frag.final ) {
    fragments_total = frag.fragment_num + 1;
  }
  if ( fragments_total ) {
    assert( fragments.rbegin()->first < fragments_total );
  }

  /* see if we're done */
  return fragments_total && fragments.size() == fragments_total;
}

Instruction FragmentAssembly::get_assembly( StateSampleWriter* sample_writer )
{
  assert( fragments_total && fragments.size() == fragments_total );

  std::string encoded;

  for ( const auto& fragment : fragments ) {
    encoded += fragment.second.contents;
  }

  Instruction ret;
  const std::string decoded = get_compressor().uncompress_str( encoded );
  if ( sample_writer != NULL ) {
    sample_writer->write_record( decoded );
  }
  fatal_assert( ret.ParseFromString( decoded ) );

  fragments.clear();
  fragments_total = 0;

  return ret;
}

bool Fragment::operator==( const Fragment& x ) const
{
  return ( id == x.id ) && ( fragment_num == x.fragment_num ) && ( final == x.final )
         && ( large == x.large ) && ( initialized == x.initialized ) && ( contents == x.contents );
}

std::vector<Fragment> Fragmenter::make_fragments( const Instruction& inst,
                                                  size_t MTU,
                                                  bool allow_zstd,
                                                  bool allow_zstd_dictionary,
                                                  const std::string& zstd_dictionary_id,
                                                  bool allow_large_fragments )
{
  if ( MTU <= Fragment::frag_header_len ) { throw std::invalid_argument( "MTU is too small for a state fragment" ); }
  MTU -= Fragment::frag_header_len;
  if ( ( inst.old_num() != last_instruction.old_num() ) || ( inst.new_num() != last_instruction.new_num() )
       || ( inst.ack_num() != last_instruction.ack_num() )
       || ( inst.throwaway_num() != last_instruction.throwaway_num() )
       || ( inst.chaff() != last_instruction.chaff() )
       || ( inst.protocol_version() != last_instruction.protocol_version() )
       || ( inst.goblin_release() != last_instruction.goblin_release() )
       || ( inst.goblin_build() != last_instruction.goblin_build() )
       || ( inst.goblin_protocol() != last_instruction.goblin_protocol() )
       || ( inst.zstd_supported() != last_instruction.zstd_supported() )
       || ( inst.zstd_dict_id() != last_instruction.zstd_dict_id() ) || ( last_MTU != MTU )
       || ( inst.large_fragments_supported() != last_instruction.large_fragments_supported() )
       || ( allow_large_fragments != last_allow_large_fragments )
       || ( allow_zstd != last_allow_zstd ) || ( allow_zstd_dictionary != last_allow_zstd_dictionary )
       || ( zstd_dictionary_id != last_zstd_dictionary_id ) ) {
    next_instruction_id++;
  }

  if ( ( inst.old_num() == last_instruction.old_num() ) && ( inst.new_num() == last_instruction.new_num() ) ) {
    assert( inst.diff() == last_instruction.diff() );
  }

  last_instruction = inst;
  last_MTU = MTU;
  last_allow_zstd = allow_zstd;
  last_allow_zstd_dictionary = allow_zstd_dictionary;
  last_zstd_dictionary_id = zstd_dictionary_id;
  last_allow_large_fragments = allow_large_fragments;

  std::string payload = get_compressor().compress_str( inst.SerializeAsString(), allow_zstd, allow_zstd_dictionary );
  size_t count = payload.size() / MTU + ( payload.size() % MTU != 0 );
  if ( !allow_large_fragments && count > 32768 ) {
    throw std::length_error( "large screen state requires a peer supporting extended fragments" );
  }
  const bool large = count > 32768;
  if ( large ) {
    const size_t extra_header = Fragment::large_frag_header_len - Fragment::frag_header_len;
    if ( MTU <= extra_header ) { throw std::invalid_argument( "MTU is too small for an extended state fragment" ); }
    MTU -= extra_header;
    count = payload.size() / MTU + ( payload.size() % MTU != 0 );
  }
  std::vector<Fragment> ret;
  ret.reserve( count );
  // Offset slicing is linear. Repeatedly copying the remaining payload was
  // quadratic in image size and could stall a wide-screen redraw for seconds.
  for ( size_t i = 0; i < count; i++ ) {
    ret.emplace_back( next_instruction_id, i, i + 1 == count, payload.substr( i * MTU, MTU ), large );
  }

  return ret;
}
