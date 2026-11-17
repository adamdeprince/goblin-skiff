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

#include "src/include/config.h"

#include <limits>
#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <zlib.h>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include "compressor.h"
#include "src/network/statesamples.h"
#include "src/util/dos_assert.h"

using namespace Network;

namespace {
const int LEVELS[] = { CompressionPacer::TOP_LEVEL, 19, 15, 12, 9, 6, 3, 1 };
/* A cautious first guess, in nanoseconds a byte, until a level has been timed. */
const double FIRST_COST[] = { 2000, 800, 200, 50, 20, 8, 3, 1.5 };
/* Of output to input, until a level has been seen to do better or worse. */
const double FIRST_RATIO = 0.25;

std::string zlib_compress_str( const std::string& input )
{
  if ( input.size() > std::numeric_limits<uLong>::max() ) { throw std::length_error( "transport state cannot be represented by zlib" ); }
  uLongf len = compressBound( input.size() );
  std::string output( len, '\0' );
  dos_assert( Z_OK == compress( reinterpret_cast<Bytef*>( &output[0] ), &len,
                                reinterpret_cast<const Bytef*>( input.data() ), input.size() ) );
  output.resize( len );
  return output;
}

std::string zlib_uncompress_str( const std::string& input )
{
  if ( input.size() > std::numeric_limits<uLong>::max() ) { throw std::length_error( "transport frame cannot be represented by zlib" ); }
  // zlib frames do not advertise their decoded size. Grow an output
  // buffer; ordinary text screens still need only a few KiB.
  size_t capacity = 4096;
  for ( ;; ) {
    std::string output( capacity, '\0' );
    uLongf len = capacity;
    const int status = uncompress( reinterpret_cast<Bytef*>( &output[0] ), &len,
                                    reinterpret_cast<const Bytef*>( input.data() ), input.size() );
    if ( status == Z_OK ) { output.resize( len ); return output; }
    if ( status != Z_BUF_ERROR ) { throw std::runtime_error( "invalid zlib transport frame" ); }
    const size_t maximum = std::min<size_t>( output.max_size(), std::numeric_limits<uLongf>::max() );
    if ( capacity == maximum ) { throw std::length_error( "transport state cannot be represented" ); }
    capacity = capacity > maximum / 2 ? maximum : capacity * 2;
  }
}

bool looks_like_zstd_frame( const std::string& input )
{
  return input.size() >= 4 && static_cast<unsigned char>( input[0] ) == 0x28
         && static_cast<unsigned char>( input[1] ) == 0xb5 && static_cast<unsigned char>( input[2] ) == 0x2f
         && static_cast<unsigned char>( input[3] ) == 0xfd;
}

#ifdef HAVE_ZSTD
std::string zstd_compress_str( const std::string& input, int level )
{
  const size_t bound = ZSTD_compressBound( input.size() );
  std::string compressed( bound, '\0' );
  const size_t written
    = ZSTD_compress( &compressed[0], compressed.size(), input.data(), input.size(), level );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd compression failed: " ) + ZSTD_getErrorName( written ) );
  }
  compressed.resize( written );
  return compressed;
}

std::string zstd_compress_str( const std::string& input,
                               const std::string& dictionary,
                               void*& cached_cdict,
                               int level )
{
  if ( dictionary.empty() ) {
    return zstd_compress_str( input, level );
  }

  if ( cached_cdict == NULL ) {
    cached_cdict = ZSTD_createCDict( dictionary.data(), dictionary.size(), level );
    if ( cached_cdict == NULL ) {
      throw std::runtime_error( "could not allocate zstd compression dictionary" );
    }
  }

  const size_t bound = ZSTD_compressBound( input.size() );
  std::string compressed( bound, '\0' );
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  if ( ctx == NULL ) {
    throw std::runtime_error( "could not allocate zstd compressor" );
  }
  const size_t written = ZSTD_compress_usingCDict(
    ctx, &compressed[0], compressed.size(), input.data(), input.size(), reinterpret_cast<ZSTD_CDict*>( cached_cdict ) );
  ZSTD_freeCCtx( ctx );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd dictionary compression failed: " ) + ZSTD_getErrorName( written ) );
  }
  compressed.resize( written );
  return compressed;
}

std::string zstd_uncompress_str( const std::string& input, void* zstd_ddict )
{
  const unsigned long long frame_size = ZSTD_getFrameContentSize( input.data(), input.size() );
  if ( frame_size == ZSTD_CONTENTSIZE_ERROR || frame_size == ZSTD_CONTENTSIZE_UNKNOWN ) {
    throw std::runtime_error( "zstd transport frame has no usable content size" );
  }
  if ( frame_size > std::string().max_size() ) {
    throw std::length_error( "zstd transport frame cannot be represented" );
  }

  std::string decompressed( static_cast<size_t>( frame_size ), '\0' );
  void* output = decompressed.empty() ? NULL : &decompressed[0];
  if ( zstd_ddict != NULL ) {
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if ( ctx == NULL ) {
      throw std::runtime_error( "could not allocate zstd decompressor" );
    }
    const size_t written = ZSTD_decompress_usingDDict(
      ctx, output, decompressed.size(), input.data(), input.size(), reinterpret_cast<ZSTD_DDict*>( zstd_ddict ) );
    ZSTD_freeDCtx( ctx );
    if ( !ZSTD_isError( written ) && written == decompressed.size() ) {
      return decompressed;
    }
  }

  const size_t written = ZSTD_decompress( output, decompressed.size(), input.data(), input.size() );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd decompression failed: " ) + ZSTD_getErrorName( written ) );
  }
  if ( written != decompressed.size() ) {
    throw std::runtime_error( "zstd transport frame size mismatch" );
  }
  return decompressed;
}
#endif
}

CompressionPacer::CompressionPacer()
{
  std::copy( std::begin( FIRST_COST ), std::end( FIRST_COST ), cost_ns_per_byte );
  std::fill( std::begin( ratio ), std::end( ratio ), FIRST_RATIO );
}

void CompressionPacer::set_budget( uint64_t send_interval_ms, double link_bytes_per_second )
{
  interval_ms = send_interval_ms;
  link_rate = link_bytes_per_second;
}

int CompressionPacer::level_for( size_t input_size ) const
{
  if ( link_rate <= 0 ) {
    return CompressionPacer::TOP_LEVEL;
  }
  for ( size_t rung = 0; rung + 1 < RUNGS; rung++ ) {
    /* A lower level makes more to carry, which also gives it longer. */
    const double on_the_wire_ms = 1000. * double( input_size ) * ratio[rung] / link_rate;
    if ( cost_ns_per_byte[rung] * double( input_size ) / 1e6 <= std::max( double( interval_ms ), on_the_wire_ms ) ) {
      return LEVELS[rung];
    }
  }
  return LEVELS[RUNGS - 1];
}

void CompressionPacer::note( int level, size_t input_size, size_t output_size, double elapsed_ms )
{
  if ( !input_size ) {
    return;
  }
  size_t used = 0;
  while ( used + 1 < RUNGS && LEVELS[used] != level ) {
    used++;
  }
  ratio[used] = 0.75 * ratio[used] + 0.25 * ( double( output_size ) / double( input_size ) );
  /* A lower level is never taken to compress better than a higher one. */
  for ( size_t rung = used + 1; rung < RUNGS; rung++ ) {
    ratio[rung] = std::max( ratio[rung], ratio[rung - 1] );
  }
  /* What a level does to one kind of data says little of another, and a pause
     of the whole machine looks like a slow level. So what was learnt of the
     levels above the one in use fades, and they are tried again in time. */
  for ( size_t rung = 0; rung < used; rung++ ) {
    cost_ns_per_byte[rung] = std::max( FIRST_COST[rung], cost_ns_per_byte[rung] * 0.98 );
  }
  /* Below a millisecond the clock says nothing about the level. */
  if ( elapsed_ms >= 1 ) {
    const double measured = elapsed_ms * 1e6 / double( input_size );
    cost_ns_per_byte[used] = 0.5 * cost_ns_per_byte[used] + 0.5 * measured;
  }
  /* A higher level is never taken to be faster than a lower one. */
  for ( size_t rung = used; rung-- > 0; ) {
    cost_ns_per_byte[rung] = std::max( cost_ns_per_byte[rung], cost_ns_per_byte[rung + 1] );
  }
  for ( size_t rung = used + 1; rung < RUNGS; rung++ ) {
    cost_ns_per_byte[rung] = std::min( cost_ns_per_byte[rung], cost_ns_per_byte[rung - 1] );
  }
}

Compressor::Compressor()
  : zstd_dictionary(), zstd_dictionary_id_value(), zstd_cdicts(), zstd_ddict( NULL ), pacer()
{}

Compressor::~Compressor()
{
#ifdef HAVE_ZSTD
  for ( auto& item : zstd_cdicts ) {
    ZSTD_freeCDict( reinterpret_cast<ZSTD_CDict*>( item.second ) );
  }
  if ( zstd_ddict != NULL ) {
    ZSTD_freeDDict( reinterpret_cast<ZSTD_DDict*>( zstd_ddict ) );
  }
#endif
}

std::string Compressor::compress_str( const std::string& input )
{
  return zlib_compress_str( input );
}

std::string Compressor::compress_str( const std::string& input, bool allow_zstd, bool allow_zstd_dictionary, bool allow_raw )
{
  std::string output;
#ifdef HAVE_ZSTD
  if ( allow_zstd ) {
    const int level = pacer.level_for( input.size() );
    const auto started = std::chrono::steady_clock::now();
    output = allow_zstd_dictionary && !zstd_dictionary.empty()
               ? zstd_compress_str( input, zstd_dictionary, zstd_cdicts[level], level )
               : zstd_compress_str( input, level );
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
    pacer.note( level, input.size(), output.size(), elapsed.count() );
    last_level = level;
  } else
#else
  (void)allow_zstd;
  (void)allow_zstd_dictionary;
#endif
  {
    output = zlib_compress_str( input );
  }
  if ( allow_raw && input.size() + 1 < output.size() ) {
    return std::string( 1, char( RAW_FRAME ) ) + input;
  }
  return output;
}

std::string Compressor::uncompress_str( const std::string& input )
{
  if ( !input.empty() && static_cast<unsigned char>( input[0] ) == RAW_FRAME ) {
    return input.substr( 1 );
  }
#ifdef HAVE_ZSTD
  if ( looks_like_zstd_frame( input ) ) {
    return zstd_uncompress_str( input, zstd_ddict );
  }
#endif
  return zlib_uncompress_str( input );
}

bool Compressor::zstd_available( void ) const
{
#ifdef HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

void Compressor::set_zstd_dictionary( const std::string& dictionary )
{
#ifdef HAVE_ZSTD
  for ( auto& item : zstd_cdicts ) {
    ZSTD_freeCDict( reinterpret_cast<ZSTD_CDict*>( item.second ) );
  }
  zstd_cdicts.clear();
  if ( zstd_ddict != NULL ) {
    ZSTD_freeDDict( reinterpret_cast<ZSTD_DDict*>( zstd_ddict ) );
    zstd_ddict = NULL;
  }
  zstd_dictionary = dictionary;
  zstd_dictionary_id_value = state_dictionary_id( zstd_dictionary );
  if ( !zstd_dictionary.empty() ) {
    zstd_ddict = ZSTD_createDDict( zstd_dictionary.data(), zstd_dictionary.size() );
    if ( zstd_ddict == NULL ) {
      zstd_dictionary.clear();
      zstd_dictionary_id_value.clear();
      throw std::runtime_error( "could not allocate zstd decompression dictionary" );
    }
  }
#else
  (void)dictionary;
  throw std::runtime_error( "zstd dictionary support was not enabled at configure time" );
#endif
}

void Compressor::set_zstd_dictionary_from_file( const std::string& path )
{
  set_zstd_dictionary( read_state_dictionary_file( path ) );
}

/* construct on first use */
Compressor& Network::get_compressor( void )
{
  static Compressor the_compressor;
  return the_compressor;
}
