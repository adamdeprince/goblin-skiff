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

#include <cstdlib>
#include <iostream>
#include <stdexcept>

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

void roundtrip( bool allow_zstd )
{
  TransportBuffers::Instruction inst;
  inst.set_protocol_version( Network::MOSH_PROTOCOL_VERSION );
  inst.set_old_num( 1 );
  inst.set_new_num( 2 );
  inst.set_ack_num( 3 );
  inst.set_throwaway_num( 1 );
  inst.set_diff( std::string( 16384, 'x' ) );
  inst.set_chaff( "test chaff" );
  inst.set_zstd_supported( true );

  Network::Fragmenter fragmenter;
  const std::vector<Network::Fragment> fragments
    = fragmenter.make_fragments( inst, 32768, allow_zstd, false, "", 12, 128 );
  require( !fragments.empty(), "fragmenter returned no fragments" );

#ifdef HAVE_ZSTD
  if ( allow_zstd ) {
    require( looks_like_zstd_frame( fragments.front().contents ), "large repetitive state was not zstd-compressed" );
  }
#else
  (void)allow_zstd;
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
}

int main( void )
{
  try {
    roundtrip( false );
    roundtrip( true );
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
