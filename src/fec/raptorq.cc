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

#include "src/fec/raptorq.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef HAVE_LIBRAPTORQ
#if !defined( RQ_LITTLE_ENDIAN ) && !defined( RQ_BIG_ENDIAN )
#if defined( __BYTE_ORDER__ ) && defined( __ORDER_BIG_ENDIAN__ ) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RQ_BIG_ENDIAN
#else
#define RQ_LITTLE_ENDIAN
#endif
#endif

#include <RaptorQ/RFC6330_v1_hdr.hpp>
#endif

using namespace FEC;

namespace {
#ifdef HAVE_LIBRAPTORQ
namespace RFC6330 = RFC6330__v1;
namespace RaptorQ = RaptorQ__v1;

class RaptorQCodec : public BlockCodec
{
public:
  CodecKind kind( void ) const override { return CodecKind::RaptorQ; }

  EncodedBlock encode( const std::string& block, uint16_t symbol_size, uint32_t repair_symbols ) const override
  {
    if ( symbol_size == 0 ) {
      throw std::runtime_error( "symbol size must be greater than zero" );
    }
    if ( block.empty() ) {
      EncodedBlock encoded;
      encoded.metadata.codec = CodecKind::RaptorQ;
      encoded.metadata.symbol_size = symbol_size;
      return encoded;
    }

    std::vector<uint8_t> input( block.begin(), block.end() );
    const uint16_t min_subsymbol_size = symbol_size;
    const size_t max_sub_block = std::max<size_t>( input.size(), symbol_size );

    RFC6330::Encoder<uint8_t*, uint8_t*> encoder(
      input.data(), input.data() + input.size(), min_subsymbol_size, symbol_size, max_sub_block );
    if ( !encoder ) {
      throw std::runtime_error( "could not initialize libRaptorQ encoder" );
    }

    encoder.compute( RFC6330::Compute::COMPLETE | RFC6330::Compute::NO_BACKGROUND | RFC6330::Compute::NO_POOL );

    EncodedBlock encoded;
    encoded.metadata.codec = CodecKind::RaptorQ;
    encoded.metadata.original_size = block.size();
    encoded.metadata.symbol_size = encoder.symbol_size();
    encoded.metadata.raptorq_oti_common = encoder.OTI_Common();
    encoded.metadata.raptorq_oti_scheme = encoder.OTI_Scheme_Specific();

    uint32_t source_symbols = 0;
    for ( auto block : encoder ) {
      source_symbols += block.symbols();

      for ( auto symbol_it = block.begin_source(); symbol_it != block.end_source(); ++symbol_it ) {
        std::vector<uint8_t> payload( encoder.symbol_size(), 0 );
        uint8_t* it = payload.data();
        const size_t written = ( *symbol_it )( it, payload.data() + payload.size() );
        if ( written != payload.size() ) {
          throw std::runtime_error( "libRaptorQ returned a short source symbol" );
        }
        encoded.symbols.emplace_back( ( *symbol_it ).id(), std::string( payload.begin(), payload.end() ) );
      }

      uint32_t emitted_repair = 0;
      for ( auto symbol_it = block.begin_repair();
            emitted_repair < repair_symbols && symbol_it != block.end_repair( block.max_repair() );
            ++symbol_it, ++emitted_repair ) {
        std::vector<uint8_t> payload( encoder.symbol_size(), 0 );
        uint8_t* it = payload.data();
        const size_t written = ( *symbol_it )( it, payload.data() + payload.size() );
        if ( written != payload.size() ) {
          throw std::runtime_error( "libRaptorQ returned a short repair symbol" );
        }
        encoded.symbols.emplace_back( ( *symbol_it ).id(), std::string( payload.begin(), payload.end() ) );
      }
    }

    if ( source_symbols > std::numeric_limits<uint16_t>::max() ) {
      throw std::runtime_error( "libRaptorQ source-symbol count exceeds metadata range" );
    }
    encoded.metadata.source_symbols = static_cast<uint16_t>( source_symbols );
    return encoded;
  }

  bool decode( const BlockMetadata& metadata, const std::vector<Symbol>& symbols, std::string& block ) const override
  {
    if ( metadata.codec != CodecKind::RaptorQ ) {
      throw std::runtime_error( "metadata does not describe an RFC6330 block" );
    }
    block.clear();
    if ( metadata.original_size == 0 ) {
      return true;
    }

    RFC6330::Decoder<uint8_t*, uint8_t*> decoder( metadata.raptorq_oti_common, metadata.raptorq_oti_scheme );
    if ( !decoder ) {
      throw std::runtime_error( "could not initialize libRaptorQ decoder" );
    }

    auto async_decode = decoder.compute( RFC6330::Compute::COMPLETE );

    for ( const auto& symbol : symbols ) {
      if ( symbol.payload.size() != metadata.symbol_size ) {
        throw std::runtime_error( "wrong RFC6330 symbol size" );
      }
      std::vector<uint8_t> payload( symbol.payload.begin(), symbol.payload.end() );
      uint8_t* it = payload.data();
      const RFC6330::Error error = decoder.add_symbol( it, payload.data() + payload.size(), symbol.id );
      if ( error != RFC6330::Error::NONE && error != RFC6330::Error::NOT_NEEDED ) {
        return false;
      }
    }

    decoder.end_of_input( RFC6330::Fill_With_Zeros::NO );
    async_decode.wait();

    std::vector<uint8_t> output( static_cast<size_t>( metadata.original_size ), 0 );
    uint8_t* it = output.data();
    const uint64_t decoded = decoder.decode_bytes( it, output.data() + output.size(), 0 );
    if ( decoded < metadata.original_size ) {
      return false;
    }

    block.assign( output.begin(), output.end() );
    return true;
  }
};
#endif
}

bool FEC::raptorq_available( void )
{
#ifdef HAVE_LIBRAPTORQ
  return true;
#else
  return false;
#endif
}

std::unique_ptr<BlockCodec> FEC::make_raptorq_codec( void )
{
#ifdef HAVE_LIBRAPTORQ
  return std::unique_ptr<BlockCodec>( new RaptorQCodec() );
#else
  throw std::runtime_error( "libRaptorQ RFC6330 backend was not enabled at configure time" );
#endif
}
