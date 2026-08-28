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

#include <stdexcept>

#include "src/fec/raptorq.h"
#include "src/fec/reedsolomon.h"

using namespace FEC;

Symbol::Symbol() : id( 0 ), payload() {}

Symbol::Symbol( uint32_t s_id, const std::string& s_payload ) : id( s_id ), payload( s_payload ) {}

BlockMetadata::BlockMetadata()
  : codec( CodecKind::ReedSolomon ), original_size( 0 ), symbol_size( 0 ), source_symbols( 0 ),
    raptorq_oti_common( 0 ), raptorq_oti_scheme( 0 )
{}

const char* FEC::codec_name( CodecKind kind )
{
  switch ( kind ) {
    case CodecKind::ReedSolomon:
      return "reed-solomon";
    case CodecKind::RaptorQ:
      return "raptorq";
    default:
      throw std::runtime_error( "unknown FEC codec" );
  }
}

CodecKind FEC::parse_codec_name( const std::string& name )
{
  if ( name == "rs" || name == "reed-solomon" || name == "reedsolomon" ) {
    return CodecKind::ReedSolomon;
  }
  if ( name == "raptorq" || name == "rfc6330" ) {
    return CodecKind::RaptorQ;
  }
  throw std::runtime_error( "unknown FEC codec: " + name );
}

bool FEC::codec_available( CodecKind kind )
{
  switch ( kind ) {
    case CodecKind::ReedSolomon:
      return true;
    case CodecKind::RaptorQ:
      return raptorq_available();
    default:
      return false;
  }
}

std::unique_ptr<BlockCodec> FEC::make_codec( CodecKind kind )
{
  switch ( kind ) {
    case CodecKind::ReedSolomon:
      return make_reed_solomon_codec();
    case CodecKind::RaptorQ:
      return make_raptorq_codec();
    default:
      throw std::runtime_error( "unknown FEC codec" );
  }
}
