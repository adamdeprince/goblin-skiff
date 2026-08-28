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

#include "src/fec/reedsolomon.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace FEC;

namespace {
typedef std::vector<uint8_t> Row;
typedef std::vector<Row> Matrix;

uint8_t exp_table[512];
uint8_t log_table[256];
bool tables_ready = false;

void ensure_tables( void )
{
  if ( tables_ready ) {
    return;
  }

  uint16_t x = 1;
  for ( size_t i = 0; i < 255; i++ ) {
    exp_table[i] = static_cast<uint8_t>( x );
    log_table[x] = static_cast<uint8_t>( i );
    x <<= 1;
    if ( x & 0x100 ) {
      x ^= 0x11d;
    }
  }
  for ( size_t i = 255; i < 512; i++ ) {
    exp_table[i] = exp_table[i - 255];
  }
  log_table[0] = 0;
  tables_ready = true;
}

uint8_t gf_mul( uint8_t a, uint8_t b )
{
  if ( a == 0 || b == 0 ) {
    return 0;
  }
  ensure_tables();
  return exp_table[log_table[a] + log_table[b]];
}

uint8_t gf_inv( uint8_t a )
{
  if ( a == 0 ) {
    throw std::runtime_error( "Reed-Solomon matrix is singular" );
  }
  ensure_tables();
  return exp_table[255 - log_table[a]];
}

Matrix invert( const Matrix& matrix )
{
  const size_t n = matrix.size();
  if ( n == 0 ) {
    return Matrix();
  }
  for ( size_t row = 0; row < n; row++ ) {
    if ( matrix[row].size() != n ) {
      throw std::runtime_error( "Reed-Solomon matrix is not square" );
    }
  }

  Matrix augmented( n, Row( 2 * n, 0 ) );
  for ( size_t row = 0; row < n; row++ ) {
    for ( size_t col = 0; col < n; col++ ) {
      augmented[row][col] = matrix[row][col];
    }
    augmented[row][n + row] = 1;
  }

  for ( size_t col = 0; col < n; col++ ) {
    size_t pivot = col;
    while ( pivot < n && augmented[pivot][col] == 0 ) {
      pivot++;
    }
    if ( pivot == n ) {
      throw std::runtime_error( "Reed-Solomon matrix is singular" );
    }
    if ( pivot != col ) {
      std::swap( augmented[pivot], augmented[col] );
    }

    const uint8_t inv_pivot = gf_inv( augmented[col][col] );
    for ( size_t i = col; i < 2 * n; i++ ) {
      augmented[col][i] = gf_mul( augmented[col][i], inv_pivot );
    }

    for ( size_t row = 0; row < n; row++ ) {
      if ( row == col ) {
        continue;
      }
      const uint8_t factor = augmented[row][col];
      if ( factor == 0 ) {
        continue;
      }
      for ( size_t i = col; i < 2 * n; i++ ) {
        augmented[row][i] ^= gf_mul( factor, augmented[col][i] );
      }
    }
  }

  Matrix inverse( n, Row( n, 0 ) );
  for ( size_t row = 0; row < n; row++ ) {
    for ( size_t col = 0; col < n; col++ ) {
      inverse[row][col] = augmented[row][n + col];
    }
  }
  return inverse;
}

Row vandermonde_row( uint32_t id, size_t source_symbols )
{
  Row row( source_symbols, 0 );
  uint8_t value = 1;
  const uint8_t x = static_cast<uint8_t>( id );
  for ( size_t col = 0; col < source_symbols; col++ ) {
    row[col] = value;
    value = gf_mul( value, x );
  }
  return row;
}

Matrix systematic_inverse( size_t source_symbols )
{
  Matrix top( source_symbols, Row( source_symbols, 0 ) );
  for ( size_t row = 0; row < source_symbols; row++ ) {
    top[row] = vandermonde_row( row, source_symbols );
  }
  return invert( top );
}

Row multiply_row( const Row& row, const Matrix& matrix )
{
  Row result( row.size(), 0 );
  for ( size_t col = 0; col < row.size(); col++ ) {
    uint8_t value = 0;
    for ( size_t i = 0; i < row.size(); i++ ) {
      value ^= gf_mul( row[i], matrix[i][col] );
    }
    result[col] = value;
  }
  return result;
}

Row generator_row( uint32_t id, size_t source_symbols, const Matrix& top_inverse )
{
  if ( id < source_symbols ) {
    Row row( source_symbols, 0 );
    row[id] = 1;
    return row;
  }

  return multiply_row( vandermonde_row( id, source_symbols ), top_inverse );
}

void validate_rs_metadata( const BlockMetadata& metadata )
{
  if ( metadata.codec != CodecKind::ReedSolomon ) {
    throw std::runtime_error( "metadata does not describe a Reed-Solomon block" );
  }
  if ( metadata.original_size == 0 ) {
    return;
  }
  if ( metadata.symbol_size == 0 || metadata.source_symbols == 0 ) {
    throw std::runtime_error( "invalid Reed-Solomon block metadata" );
  }
  if ( metadata.source_symbols > 256 ) {
    throw std::runtime_error( "Reed-Solomon supports at most 256 source symbols per block" );
  }
  const uint64_t capacity = static_cast<uint64_t>( metadata.symbol_size ) * metadata.source_symbols;
  if ( metadata.original_size > capacity ) {
    throw std::runtime_error( "Reed-Solomon metadata size exceeds block capacity" );
  }
}

class ReedSolomonCodec : public BlockCodec
{
public:
  CodecKind kind( void ) const override { return CodecKind::ReedSolomon; }

  EncodedBlock encode( const std::string& block, uint16_t symbol_size, uint32_t repair_symbols ) const override
  {
    if ( symbol_size == 0 ) {
      throw std::runtime_error( "symbol size must be greater than zero" );
    }

    EncodedBlock encoded;
    encoded.metadata.codec = CodecKind::ReedSolomon;
    encoded.metadata.original_size = block.size();
    encoded.metadata.symbol_size = symbol_size;

    if ( block.empty() ) {
      return encoded;
    }

    const size_t source_symbols = ( block.size() + symbol_size - 1 ) / symbol_size;
    if ( source_symbols > 256 ) {
      throw std::runtime_error( "Reed-Solomon supports at most 256 source symbols per block" );
    }
    if ( repair_symbols > 256 || source_symbols + repair_symbols > 256 ) {
      throw std::runtime_error( "Reed-Solomon supports at most 256 total symbols per block" );
    }
    encoded.metadata.source_symbols = static_cast<uint16_t>( source_symbols );

    std::vector<std::string> source;
    source.reserve( source_symbols );
    for ( size_t i = 0; i < source_symbols; i++ ) {
      const size_t offset = i * symbol_size;
      const size_t count = std::min<size_t>( symbol_size, block.size() - offset );
      std::string shard( symbol_size, '\0' );
      shard.replace( 0, count, block.data() + offset, count );
      encoded.symbols.emplace_back( static_cast<uint32_t>( i ), shard );
      source.push_back( shard );
    }

    if ( repair_symbols == 0 ) {
      return encoded;
    }

    const Matrix top_inverse = systematic_inverse( source_symbols );
    for ( uint32_t repair = 0; repair < repair_symbols; repair++ ) {
      const uint32_t id = static_cast<uint32_t>( source_symbols ) + repair;
      const Row row = generator_row( id, source_symbols, top_inverse );
      std::string shard( symbol_size, '\0' );
      for ( size_t source_id = 0; source_id < source_symbols; source_id++ ) {
        const uint8_t coefficient = row[source_id];
        if ( coefficient == 0 ) {
          continue;
        }
        for ( size_t byte = 0; byte < symbol_size; byte++ ) {
          shard[byte] = static_cast<char>(
            static_cast<uint8_t>( shard[byte] )
            ^ gf_mul( coefficient, static_cast<uint8_t>( source[source_id][byte] ) ) );
        }
      }
      encoded.symbols.emplace_back( id, shard );
    }

    return encoded;
  }

  bool decode( const BlockMetadata& metadata, const std::vector<Symbol>& symbols, std::string& block ) const override
  {
    validate_rs_metadata( metadata );
    block.clear();
    if ( metadata.original_size == 0 ) {
      return true;
    }

    const size_t source_symbols = metadata.source_symbols;
    const size_t symbol_size = metadata.symbol_size;
    const Matrix top_inverse = systematic_inverse( source_symbols );

    std::vector<bool> seen( 256, false );
    std::vector<Symbol> selected;
    selected.reserve( source_symbols );
    for ( const auto& symbol : symbols ) {
      if ( symbol.id >= 256 || seen[symbol.id] ) {
        continue;
      }
      if ( symbol.payload.size() != symbol_size ) {
        throw std::runtime_error( "wrong Reed-Solomon symbol size" );
      }
      seen[symbol.id] = true;
      selected.push_back( symbol );
      if ( selected.size() == source_symbols ) {
        break;
      }
    }

    if ( selected.size() < source_symbols ) {
      return false;
    }

    Matrix received_matrix( source_symbols, Row( source_symbols, 0 ) );
    for ( size_t row = 0; row < source_symbols; row++ ) {
      received_matrix[row] = generator_row( selected[row].id, source_symbols, top_inverse );
    }

    Matrix decode_matrix;
    try {
      decode_matrix = invert( received_matrix );
    } catch ( const std::runtime_error& ) {
      return false;
    }

    std::vector<std::string> decoded( source_symbols, std::string( symbol_size, '\0' ) );
    for ( size_t source_id = 0; source_id < source_symbols; source_id++ ) {
      for ( size_t received_id = 0; received_id < source_symbols; received_id++ ) {
        const uint8_t coefficient = decode_matrix[source_id][received_id];
        if ( coefficient == 0 ) {
          continue;
        }
        const std::string& received = selected[received_id].payload;
        for ( size_t byte = 0; byte < symbol_size; byte++ ) {
          decoded[source_id][byte] = static_cast<char>(
            static_cast<uint8_t>( decoded[source_id][byte] )
            ^ gf_mul( coefficient, static_cast<uint8_t>( received[byte] ) ) );
        }
      }
    }

    block.reserve( source_symbols * symbol_size );
    for ( const auto& shard : decoded ) {
      block.append( shard );
    }
    block.resize( static_cast<size_t>( metadata.original_size ) );
    return true;
  }
};
}

std::unique_ptr<BlockCodec> FEC::make_reed_solomon_codec( void )
{
  return std::unique_ptr<BlockCodec>( new ReedSolomonCodec() );
}
