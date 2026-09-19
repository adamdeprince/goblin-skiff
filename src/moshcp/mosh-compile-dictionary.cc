/*
    Modified for Goblin Skiff on 2026-09-19.
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

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <getopt.h>

#include "src/network/statesamples.h"

namespace {
void usage( const char* argv0 )
{
  std::cerr << "Usage: " << argv0
            << " --input=PATH --output=PATH [--dict-size=BYTES] [--max-samples=N]\n"
            << "       " << argv0 << " [options] INPUT OUTPUT\n";
}

size_t parse_size_option( const char* name, const char* value )
{
  char* end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull( value, &end, 10 );
  if ( errno || *end || parsed > static_cast<unsigned long long>( SIZE_MAX ) ) {
    throw std::runtime_error( std::string( "bad " ) + name + ": " + value );
  }
  return static_cast<size_t>( parsed );
}

void write_file( const std::string& path, const std::string& contents )
{
  std::ofstream out( path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc );
  if ( !out ) {
    throw std::runtime_error( "could not open output file: " + path );
  }
  out.write( contents.data(), contents.size() );
  if ( !out ) {
    throw std::runtime_error( "could not write output file: " + path );
  }
}
}

int main( int argc, char* argv[] )
{
  std::string input_path;
  std::string output_path;
  size_t dictionary_size = 64 * 1024;
  size_t max_samples = 0;

  static const struct option long_options[] = {
    { "input", required_argument, NULL, 'i' },
    { "output", required_argument, NULL, 'o' },
    { "dict-size", required_argument, NULL, 's' },
    { "max-samples", required_argument, NULL, 'm' },
    { "help", no_argument, NULL, 'h' },
    { 0, 0, 0, 0 },
  };

  int opt;
  while ( ( opt = getopt_long( argc, argv, "i:o:s:m:h", long_options, NULL ) ) != -1 ) {
    try {
      switch ( opt ) {
        case 'i':
          input_path = optarg;
          break;
        case 'o':
          output_path = optarg;
          break;
        case 's':
          dictionary_size = parse_size_option( "--dict-size", optarg );
          break;
        case 'm':
          max_samples = parse_size_option( "--max-samples", optarg );
          break;
        case 'h':
          usage( argv[0] );
          return 0;
        default:
          usage( argv[0] );
          return 1;
      }
    } catch ( const std::exception& e ) {
      std::cerr << argv[0] << ": " << e.what() << "\n";
      return 1;
    }
  }

  if ( input_path.empty() && optind < argc ) {
    input_path = argv[optind++];
  }
  if ( output_path.empty() && optind < argc ) {
    output_path = argv[optind++];
  }
  if ( optind != argc || input_path.empty() || output_path.empty() ) {
    usage( argv[0] );
    return 1;
  }

  try {
    const std::vector<std::string> samples = Network::read_state_sample_file( input_path, max_samples );
    const std::string dictionary = Network::train_state_dictionary( samples, dictionary_size );
    const std::string compressed_dictionary = Network::compress_state_dictionary_file( dictionary );
    write_file( output_path, compressed_dictionary );
    std::cerr << "goblin-skiff-compile-dictionary: wrote " << compressed_dictionary.size() << " compressed bytes to "
              << output_path << " from " << dictionary.size() << " raw dictionary bytes and " << samples.size()
              << " samples, id " << Network::state_dictionary_id( dictionary ) << "\n";
  } catch ( const std::exception& e ) {
    std::cerr << argv[0] << ": " << e.what() << "\n";
    return 1;
  }

  return 0;
}
