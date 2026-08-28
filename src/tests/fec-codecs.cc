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

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
std::string fixture( size_t size )
{
  std::string data;
  data.reserve( size );
  for ( size_t i = 0; i < size; i++ ) {
    data.push_back( static_cast<char>( ( i * 37 + i / 3 ) & 0xff ) );
  }
  return data;
}

std::vector<FEC::Symbol> drop_some_symbols( const std::vector<FEC::Symbol>& symbols )
{
  std::vector<FEC::Symbol> kept;
  for ( const auto& symbol : symbols ) {
    if ( symbol.id == 1 || symbol.id == 3 || symbol.id == 7 ) {
      continue;
    }
    kept.push_back( symbol );
  }
  return kept;
}

void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

void test_codec_roundtrip( FEC::CodecKind kind )
{
  std::unique_ptr<FEC::BlockCodec> codec = FEC::make_codec( kind );
  const std::string input = fixture( 4093 );
  const FEC::EncodedBlock encoded = codec->encode( input, 128, 12 );
  std::string decoded;

  const std::string name = FEC::codec_name( kind );
  require( codec->decode( encoded.metadata, drop_some_symbols( encoded.symbols ), decoded ),
           ( name + " decode failed" ).c_str() );
  require( decoded == input, ( name + " decoded bytes did not match input" ).c_str() );

  std::vector<FEC::Symbol> too_few;
  for ( size_t i = 0; i + 1 < encoded.metadata.source_symbols; i++ ) {
    too_few.push_back( encoded.symbols[i] );
  }
  require( !codec->decode( encoded.metadata, too_few, decoded ), ( name + " decoded with too few symbols" ).c_str() );
}
}

int main( void )
{
  try {
    test_codec_roundtrip( FEC::CodecKind::ReedSolomon );

    if ( FEC::codec_available( FEC::CodecKind::RaptorQ ) ) {
      test_codec_roundtrip( FEC::CodecKind::RaptorQ );
    }
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
