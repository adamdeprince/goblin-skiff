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

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "src/network/compressor.h"
#include "src/network/network.h"
#include "src/network/statesamples.h"
#include "src/network/transportfragment.h"
#include "src/protobufs/transportinstruction.pb.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

std::string temp_path( void )
{
  char path[] = "/tmp/goblin-mosh-state-samples-XXXXXX";
  int fd = mkstemp( path );
  if ( fd < 0 ) {
    throw std::runtime_error( "mkstemp failed" );
  }
  close( fd );
  return path;
}

void write_file( const std::string& path, const std::string& contents )
{
  std::ofstream out( path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc );
  if ( !out ) {
    throw std::runtime_error( "could not open temp file" );
  }
  out.write( contents.data(), contents.size() );
  if ( !out ) {
    throw std::runtime_error( "could not write temp file" );
  }
}

bool looks_like_zstd_frame( const std::string& input )
{
  return input.size() >= 4 && static_cast<unsigned char>( input[0] ) == 0x28
         && static_cast<unsigned char>( input[1] ) == 0xb5 && static_cast<unsigned char>( input[2] ) == 0x2f
         && static_cast<unsigned char>( input[3] ) == 0xfd;
}

std::string make_sample( unsigned int n )
{
  std::ostringstream out;
  for ( unsigned int row = 1; row <= 24; row++ ) {
    out << "\033[" << row << ";1H";
    out << "buffer:";
    out << ( n % 7 );
    out << " mode:emacs status:";
    out << ( row % 5 );
    out << " git:main line:";
    out << ( n * 31 + row );
    out << " command:";
    out << ( row % 3 == 0 ? "next-line" : "self-insert-command" );
  }
  return out.str();
}
}

int main( void )
{
#ifndef HAVE_ZSTD
  return 77;
#else
  std::string path;
  std::string assembly_path;
  std::string dictionary_path;
  try {
    path = temp_path();
    std::vector<std::string> samples;
    for ( unsigned int i = 0; i < 160; i++ ) {
      samples.push_back( make_sample( i ) );
    }

    {
      Network::StateSampleWriter writer( path, 0 );
      for ( std::vector<std::string>::const_iterator it = samples.begin(); it != samples.end(); ++it ) {
        writer.write_record( *it );
      }
    }

    const std::vector<std::string> first_three = Network::read_state_sample_file( path, 3 );
    require( first_three.size() == 3, "max-samples limit did not apply" );
    require( first_three[0] == samples[0], "first sample mismatch" );

    const std::vector<std::string> decoded_samples = Network::read_state_sample_file( path, 0 );
    require( decoded_samples == samples, "sample roundtrip mismatch" );

    const std::string dictionary = Network::train_state_dictionary( decoded_samples, 8192 );
    require( !dictionary.empty(), "dictionary training returned an empty dictionary" );
    require( dictionary.size() <= 8192, "dictionary exceeded requested size" );

    const std::string compressed_dictionary = Network::compress_state_dictionary_file( dictionary );
    require( looks_like_zstd_frame( compressed_dictionary ), "dictionary file was not zstd-compressed" );
    dictionary_path = temp_path();
    write_file( dictionary_path, compressed_dictionary );
    require( Network::read_state_dictionary_file( dictionary_path ) == dictionary,
             "compressed dictionary file roundtrip mismatch" );

    Network::get_compressor().set_zstd_dictionary_from_file( dictionary_path );

    TransportBuffers::Instruction inst;
    inst.set_protocol_version( Network::MOSH_PROTOCOL_VERSION );
    inst.set_old_num( 1 );
    inst.set_new_num( 2 );
    inst.set_ack_num( 1 );
    inst.set_throwaway_num( 1 );
    inst.set_diff( samples[7] + samples[8] );
    inst.set_chaff( "sample-test" );
    inst.set_zstd_supported( true );
    inst.set_zstd_dict_id( Network::state_dictionary_id( dictionary ) );

    Network::Fragmenter fragmenter;
    const std::vector<Network::Fragment> fragments = fragmenter.make_fragments(
      inst, 32768, true, true, Network::state_dictionary_id( dictionary ) );
    require( !fragments.empty(), "fragmenter returned no fragments" );

    assembly_path = temp_path();
    Network::StateSampleWriter assembly_writer( assembly_path, 0 );
    Network::FragmentAssembly assembly;
    bool complete = false;
    for ( std::vector<Network::Fragment>::const_iterator it = fragments.begin(); it != fragments.end(); ++it ) {
      Network::Fragment fragment = *it;
      complete = assembly.add_fragment( fragment );
    }
    require( complete, "fragment assembly did not complete" );
    const TransportBuffers::Instruction decoded = assembly.get_assembly( &assembly_writer );
    assembly_writer.close();

    require( decoded.diff() == inst.diff(), "dictionary fragment roundtrip mismatch" );
    const std::vector<std::string> assembly_samples = Network::read_state_sample_file( assembly_path, 0 );
    require( assembly_samples.size() == 1, "assembly writer did not log one sample" );
    require( assembly_samples[0] == inst.SerializeAsString(), "assembly sample contents mismatch" );

    unlink( path.c_str() );
    unlink( assembly_path.c_str() );
    unlink( dictionary_path.c_str() );
  } catch ( const std::exception& e ) {
    if ( !path.empty() ) {
      unlink( path.c_str() );
    }
    if ( !assembly_path.empty() ) {
      unlink( assembly_path.c_str() );
    }
    if ( !dictionary_path.empty() ) {
      unlink( dictionary_path.c_str() );
    }
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
#endif
}
