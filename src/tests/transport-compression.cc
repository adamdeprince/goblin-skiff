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

#include "src/include/config.h"
#include "src/network/network.h"
#include "src/network/transportfragment.h"
#include "src/network/compressor.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace {
bool looks_like_zstd_frame( const std::string& input )
{
  return input.size() >= 4 && static_cast<unsigned char>( input[0] ) == 0x28
         && static_cast<unsigned char>( input[1] ) == 0xb5 && static_cast<unsigned char>( input[2] ) == 0x2f
         && static_cast<unsigned char>( input[3] ) == 0xfd;
}

void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

#ifdef HAVE_ZSTD
std::string compress_zstd_22( const std::string& input )
{
  std::string compressed( ZSTD_compressBound( input.size() ), '\0' );
  const size_t written = ZSTD_compress( &compressed[0], compressed.size(), input.data(), input.size(), 22 );
  require( !ZSTD_isError( written ), "test zstd-22 compression failed" );
  compressed.resize( written );
  return compressed;
}
#endif

void roundtrip( bool allow_zstd )
{
  TransportBuffers::Instruction inst;
  inst.set_protocol_version( Network::MOSH_PROTOCOL_VERSION );
  inst.set_old_num( 1 );
  inst.set_new_num( 2 );
  inst.set_ack_num( 3 );
  inst.set_throwaway_num( 1 );
  inst.set_diff( "x" );
  inst.set_chaff( "test chaff" );
  inst.set_zstd_supported( true );

  Network::Fragmenter fragmenter;
  const std::vector<Network::Fragment> fragments
    = fragmenter.make_fragments( inst, 32768, allow_zstd, false, "" );
  require( !fragments.empty(), "fragmenter returned no fragments" );

#ifdef HAVE_ZSTD
  if ( allow_zstd ) {
    require( fragments.size() == 1, "small zstd state unexpectedly fragmented" );
    require( looks_like_zstd_frame( fragments.front().contents ), "small state was not zstd-compressed" );
    require( fragments.front().contents == compress_zstd_22( inst.SerializeAsString() ),
             "state was not compressed at zstd level 22" );
  } else {
    require( !looks_like_zstd_frame( fragments.front().contents ), "zstd was used before peer negotiation" );
  }
#endif

  Network::FragmentAssembly assembly;
  bool complete = false;
  for ( std::vector<Network::Fragment>::const_iterator it = fragments.begin(); it != fragments.end(); ++it ) {
    Network::Fragment fragment = *it;
    complete = assembly.add_fragment( fragment );
  }
  require( complete, "fragment assembly did not complete" );

  const TransportBuffers::Instruction decoded = assembly.get_assembly();
  require( decoded.protocol_version() == inst.protocol_version(), "protocol version mismatch" );
  require( decoded.old_num() == inst.old_num(), "old_num mismatch" );
  require( decoded.new_num() == inst.new_num(), "new_num mismatch" );
  require( decoded.ack_num() == inst.ack_num(), "ack_num mismatch" );
  require( decoded.throwaway_num() == inst.throwaway_num(), "throwaway_num mismatch" );
  require( decoded.diff() == inst.diff(), "diff mismatch" );
  require( decoded.chaff() == inst.chaff(), "chaff mismatch" );
  require( decoded.zstd_supported(), "zstd_supported mismatch" );
}

#ifdef HAVE_ZSTD
/* The level is 22 unless the CPU cannot keep up with the wire. */
void adaptive_level()
{
  const double SLOW = 1000, FAST = 100e6; /* bytes a second: 8 kbit/s and 800 Mbit/s */
  const size_t HUGE_STATE = size_t( 64 ) * 1024 * 1024;

  /* Nothing says the CPU is behind a link that has not been measured. */
  Network::CompressionPacer unknown;
  unknown.set_budget( 4, 0 );
  require( unknown.level_for( HUGE_STATE ) == 22, "an unmeasured link keeps level 22, whatever the size" );
  unknown.note( 22, 200 * 1024, 40 * 1024, 60000 );
  require( unknown.level_for( HUGE_STATE ) == 22, "however slow level 22 has been" );

  /* A slow link keeps it for an update of any size: the wire is the bottleneck. */
  Network::CompressionPacer slow;
  slow.set_budget( 100, SLOW );
  require( slow.level_for( 1024 ) == 22 && slow.level_for( HUGE_STATE ) == 22, "the wire is behind the CPU" );
  /* Even when level 22 is at its slowest here: 1 ms a byte would be behind. */
  slow.note( 22, 200 * 1024, 40 * 1024, 2000 );
  require( slow.level_for( HUGE_STATE ) == 22, "10 microseconds a byte still outruns 8 kbit/s" );

  /* A fast link: an update that takes longer than the next send and the wire. */
  Network::CompressionPacer fast;
  fast.set_budget( 100, FAST );
  require( fast.level_for( 1024 ) == 22, "what fits before the next send is no backlog" );
  require( fast.level_for( 16 * 1024 * 1024 ) < 22, "seconds of CPU for milliseconds of wire is" );

  /* It learns. Level 22 turns out slow at this data: 10 microseconds a byte. */
  fast.note( 22, 200 * 1024, 40 * 1024, 2000 );
  fast.note( 22, 200 * 1024, 40 * 1024, 2000 );
  require( fast.level_for( 200 * 1024 ) < 22, "it is not used again at that size" );
  require( fast.level_for( 2 * 1024 ) == 22, "while what fits still gets it" );
  size_t size = 1024;
  int previous = 22;
  for ( int i = 0; i < 16; i++, size *= 2 ) {
    const int level = fast.level_for( size );
    require( level <= previous, "more data never means a higher level" );
    previous = level;
  }
  require( previous >= 1, "there is always a level" );

  /* What was learnt fades: a pause of the machine is not forever. */
  for ( int i = 0; i < 400 && fast.level_for( 40 * 1024 ) < 22; i++ ) {
    fast.note( 9, 40 * 1024, 8 * 1024, 2 );
  }
  require( fast.level_for( 40 * 1024 ) == 22, "level 22 comes back" );

  /* The link slowing down brings it back at once. */
  fast.note( 22, 200 * 1024, 40 * 1024, 2000 );
  fast.note( 22, 200 * 1024, 40 * 1024, 2000 );
  require( fast.level_for( 1024 * 1024 ) < 22, "behind the fast link" );
  fast.set_budget( 100, SLOW );
  require( fast.level_for( 1024 * 1024 ) == 22, "not behind the slow one" );

  /* The ratio a level gets counts: making less to carry gives less time. */
  Network::CompressionPacer dense;
  dense.set_budget( 1, 2e6 );
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 ); /* 2 us a byte, and 100 to 1 */
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 );
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 );
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 );
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 );
  dense.note( 22, 1024 * 1024, 10 * 1024, 2000 );
  require( dense.level_for( 1024 * 1024 ) < 22, "at 2 MB/s, 100 to 1 leaves 5 ms a MB" );

  /* Whatever the level, the frame decodes, and with no word to the peer. */
  std::string text;
  for ( int i = 0; text.size() < 3 * 1024 * 1024; i++ ) {
    text += "777;tablet-pointer;v=1:op=event:kind=1:phase=2:x=" + std::to_string( i * 7 ) + ":y=" + std::to_string( i * 3 ) + "\n";
  }
  Network::Compressor compressor;
  compressor.pace( 4, FAST );
  const std::string quick = compressor.compress_str( text, true, false );
  require( compressor.last_zstd_level() < 22, "a big update at a low level" );
  require( Network::Compressor().uncompress_str( quick ) == text, "decodes as any other" );
  Network::Compressor with_dictionary;
  with_dictionary.set_zstd_dictionary( text.substr( 0, 8192 ) );
  with_dictionary.pace( 4, FAST );
  const std::string small = with_dictionary.compress_str( text, true, true );
  require( with_dictionary.last_zstd_level() < 22 && with_dictionary.uncompress_str( small ) == text, "with a dictionary too" );

  /* The same instruction sent again is the same bytes, whatever has happened
     to the level since, or the receiver would splice two compressions. */
  TransportBuffers::Instruction inst;
  inst.set_old_num( 1 );
  inst.set_new_num( 2 );
  inst.set_diff( text.substr( 0, 400 * 1024 ) );
  Network::Fragmenter fragmenter;
  Network::get_compressor().pace( 100, SLOW );
  const auto once = fragmenter.make_fragments( inst, 1200, true, false, "" );
  require( Network::get_compressor().last_zstd_level() == 22, "at level 22 on a slow link" );
  Network::get_compressor().pace( 1, FAST );
  const auto again = fragmenter.make_fragments( inst, 1200, true, false, "" );
  require( once == again, "a retransmission is byte for byte" );
  Network::get_compressor().pace( 100, 0 );
}
#endif

/* On a lossy link a big update gets through in passes, whatever each pass loses. */
void resend_fills_gaps( bool zstd )
{
  std::string diff( 200 * 1024, '\0' );
  uint32_t random = 0x2545f491U;
  for ( auto& c : diff ) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; c = char( random ); }
  TransportBuffers::Instruction inst;
  inst.set_old_num( 5 );
  inst.set_new_num( 9 );
  inst.set_ack_num( 3 );
  inst.set_throwaway_num( 1 );
  inst.set_diff( diff );
  inst.set_chaff( "first" );
  Network::Fragmenter fragmenter;
  Network::FragmentAssembly assembly;
  bool complete = false;
  /* Each pass, with new chaff and a newer ack as any resend has, loses a different
     half. No pass would ever do on its own. */
  for ( unsigned pass = 0; pass < 2; pass++ ) {
    const auto fragments = fragmenter.make_fragments( inst, 1200, zstd, false, "" );
    require( fragments.size() > 100, "the fixture takes many fragments" );
    for ( size_t i = pass; i < fragments.size(); i += 2 ) {
      Network::Fragment fragment = fragments[i];
      complete = assembly.add_fragment( fragment );
    }
    require( complete == ( pass == 1 ), "whole only once every gap is filled" );
    inst.set_chaff( "second" );
    inst.set_ack_num( 4 );
    inst.set_throwaway_num( 2 );
  }
  const TransportBuffers::Instruction decoded = assembly.get_assembly();
  require( decoded.diff() == diff && decoded.new_num() == 9, "and is what was sent" );

  /* A different update is a new one, however it starts. */
  const auto before = fragmenter.make_fragments( inst, 1200, zstd, false, "" );
  inst.set_new_num( 10 );
  const auto after = fragmenter.make_fragments( inst, 1200, zstd, false, "" );
  require( before.front().id != after.front().id, "a new state has a new id" );

  /* The end of a session is never delayed by what a resend carries. */
  inst.set_ack_num( uint64_t( -1 ) );
  const auto closing = fragmenter.make_fragments( inst, 1200, zstd, false, "" );
  require( closing.front().id != after.front().id, "the shutdown ack is fresh" );
  inst.set_ack_num( 4 );
  inst.set_new_num( uint64_t( -1 ) );
  const auto shut = fragmenter.make_fragments( inst, 1200, zstd, false, "" );
  inst.set_chaff( "third" );
  require( fragmenter.make_fragments( inst, 1200, zstd, false, "" ).front().id != shut.front().id, "as is a shutdown" );

  /* A small update is as ever: one fragment, sent anew. */
  TransportBuffers::Instruction small;
  small.set_old_num( 1 );
  small.set_new_num( 2 );
  small.set_diff( "x" );
  small.set_chaff( "a" );
  Network::Fragmenter keys;
  const auto one = keys.make_fragments( small, 1200, zstd, false, "" );
  small.set_chaff( "b" );
  const auto two = keys.make_fragments( small, 1200, zstd, false, "" );
  require( one.size() == 1 && one.front().id != two.front().id && one.front().contents != two.front().contents,
           "a keystroke's packet still changes with its chaff" );
}

void large_roundtrip( bool zstd )
{
  TransportBuffers::Instruction inst;
  inst.set_old_num( 1 ); inst.set_new_num( 2 );
  // An incompressible >4 MiB state used to fail decompression (or spend
  // quadratic time copying while fragmenting). Include loss and reordering.
  std::string payload( 17 * 1024 * 1024, '\0' );
  uint32_t random = 0x953badU;
  for ( auto& c : payload ) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; c = char( random ); }
  inst.set_diff( payload );
  Network::Fragmenter fragmenter;
  auto fragments = fragmenter.make_fragments( inst, 440, zstd, false, "", true );
  require( fragments.size() > 32768, "wide-image fixture exceeds legacy fragment count" );
  Network::FragmentAssembly assembly;
  for ( size_t i = fragments.size(); i-- > 1; ) {
    Network::Fragment decoded( fragments[i].tostring() );
    require( decoded == fragments[i], "extended fragment wire roundtrip" );
    require( decoded.tostring().size() <= 440, "extended header fits MTU" );
    require( !assembly.add_fragment( decoded ), "incomplete image not published before missing packet" );
  }
  require( !assembly.add_fragment( fragments.back() ), "duplicate packet does not complete missing image" );
  require( assembly.add_fragment( fragments[0] ), "late packet completes reordered image" );
  require( assembly.get_assembly().diff() == payload, "large state roundtrip" );
  bool rejected = false;
  try { fragmenter.make_fragments( inst, Network::Fragment::frag_header_len, zstd, false, "" ); }
  catch ( const std::invalid_argument& ) { rejected = true; }
  require( rejected, "invalid MTU rejected without underflow" );
  rejected = false;
  try { fragmenter.make_fragments( inst, 440, zstd, false, "", false ); }
  catch ( const std::length_error& ) { rejected = true; }
  require( rejected, "extended fragments require peer negotiation" );
}
}


/* An instruction that compression would only lengthen goes as it is, behind
   a zero byte, to a peer that takes one; nothing else does. */
void raw_frames()
{
  TransportBuffers::Instruction inst;
  inst.set_protocol_version( Network::MOSH_PROTOCOL_VERSION );
  inst.set_old_num( 1 );
  inst.set_new_num( 2 );
  inst.set_ack_num( 3 );
  inst.set_throwaway_num( 1 );
  inst.set_diff( "x" );
  inst.set_chaff( "c" );
  inst.set_zstd_supported( true );
  const std::string serialized = inst.SerializeAsString();

  for ( bool zstd : { false, true } ) {
    Network::Fragmenter fragmenter;
    const auto fragments = fragmenter.make_fragments( inst, 32768, zstd, false, "", false, true );
    require( fragments.size() == 1, "a small instruction is one fragment" );
    const std::string& contents = fragments.front().contents;
    require( !contents.empty() && static_cast<unsigned char>( contents[0] ) == Network::Compressor::RAW_FRAME
               && contents.substr( 1 ) == serialized,
             "a small instruction goes raw" );
    Network::FragmentAssembly assembly;
    Network::Fragment fragment = fragments.front();
    require( assembly.add_fragment( fragment ), "a raw fragment assembles" );
    const TransportBuffers::Instruction decoded = assembly.get_assembly();
    require( decoded.diff() == "x" && decoded.ack_num() == 3, "and reads back" );

    Network::Fragmenter careful;
    const auto compressed = careful.make_fragments( inst, 32768, zstd, false, "", false, false );
    require( static_cast<unsigned char>( compressed.front().contents[0] ) != Network::Compressor::RAW_FRAME,
             "nothing goes raw before the peer allows it" );
  }

  inst.set_diff( std::string( 4000, 'a' ) );
  Network::Fragmenter fragmenter;
  const auto fragments = fragmenter.make_fragments( inst, 32768, true, false, "", false, true );
  require( fragments.size() == 1 && static_cast<unsigned char>( fragments.front().contents[0] ) != Network::Compressor::RAW_FRAME
             && fragments.front().contents.size() < 200,
           "an instruction that compresses is compressed" );
  Network::FragmentAssembly assembly;
  Network::Fragment fragment = fragments.front();
  require( assembly.add_fragment( fragment ) && assembly.get_assembly().diff() == inst.diff(), "and reads back" );
}

int main( void )
{
  try {
    roundtrip( false );
    roundtrip( true );
#ifdef HAVE_ZSTD
    adaptive_level();
#endif
    resend_fills_gaps( false );
    resend_fills_gaps( true );
    large_roundtrip( false );
    large_roundtrip( true );
    raw_frames();
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
