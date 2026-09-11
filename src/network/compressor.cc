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
const int STATE_ZSTD_LEVEL = 22;

std::string zlib_compress_str( const std::string& input )
{
  if ( input.size() > Compressor::MAX_STATE_SIZE ) { throw std::length_error( "transport state exceeds size limit" ); }
  uLongf len = compressBound( input.size() );
  std::string output( len, '\0' );
  dos_assert( Z_OK == compress( reinterpret_cast<Bytef*>( &output[0] ), &len,
                                reinterpret_cast<const Bytef*>( input.data() ), input.size() ) );
  output.resize( len );
  return output;
}

std::string zlib_uncompress_str( const std::string& input )
{
  if ( input.size() > Compressor::MAX_STATE_SIZE ) { throw std::length_error( "transport frame exceeds size limit" ); }
  // zlib frames do not advertise their decoded size. Grow a bounded output
  // buffer; ordinary text screens still need only a few KiB.
  size_t capacity = 4096;
  for ( ;; ) {
    std::string output( capacity, '\0' );
    uLongf len = capacity;
    const int status = uncompress( reinterpret_cast<Bytef*>( &output[0] ), &len,
                                    reinterpret_cast<const Bytef*>( input.data() ), input.size() );
    if ( status == Z_OK ) { output.resize( len ); return output; }
    if ( status != Z_BUF_ERROR ) { throw std::runtime_error( "invalid zlib transport frame" ); }
    if ( capacity == Compressor::MAX_STATE_SIZE ) { throw std::length_error( "transport state exceeds size limit" ); }
    capacity *= 2;
  }
}

bool looks_like_zstd_frame( const std::string& input )
{
  return input.size() >= 4 && static_cast<unsigned char>( input[0] ) == 0x28
         && static_cast<unsigned char>( input[1] ) == 0xb5 && static_cast<unsigned char>( input[2] ) == 0x2f
         && static_cast<unsigned char>( input[3] ) == 0xfd;
}

#ifdef HAVE_ZSTD
std::string zstd_compress_str( const std::string& input )
{
  if ( input.size() > Compressor::MAX_STATE_SIZE ) { throw std::length_error( "transport state exceeds size limit" ); }
  const size_t bound = ZSTD_compressBound( input.size() );
  std::string compressed( bound, '\0' );
  const size_t written
    = ZSTD_compress( &compressed[0], compressed.size(), input.data(), input.size(), STATE_ZSTD_LEVEL );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd compression failed: " ) + ZSTD_getErrorName( written ) );
  }
  compressed.resize( written );
  return compressed;
}

std::string zstd_compress_str( const std::string& input,
                               const std::string& dictionary,
                               void*& cached_cdict )
{
  if ( input.size() > Compressor::MAX_STATE_SIZE ) { throw std::length_error( "transport state exceeds size limit" ); }
  if ( dictionary.empty() ) {
    return zstd_compress_str( input );
  }

  if ( cached_cdict == NULL ) {
    cached_cdict = ZSTD_createCDict( dictionary.data(), dictionary.size(), STATE_ZSTD_LEVEL );
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
  if ( frame_size > Compressor::MAX_STATE_SIZE ) {
    throw std::runtime_error( "zstd transport frame exceeds decompression limit" );
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

Compressor::Compressor()
  : zstd_dictionary(), zstd_dictionary_id_value(), zstd_cdict( NULL ), zstd_ddict( NULL )
{}

Compressor::~Compressor()
{
#ifdef HAVE_ZSTD
  if ( zstd_cdict != NULL ) {
    ZSTD_freeCDict( reinterpret_cast<ZSTD_CDict*>( zstd_cdict ) );
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

std::string Compressor::compress_str( const std::string& input, bool allow_zstd, bool allow_zstd_dictionary )
{
#ifdef HAVE_ZSTD
  if ( allow_zstd ) {
    if ( allow_zstd_dictionary && !zstd_dictionary.empty() ) {
      return zstd_compress_str( input, zstd_dictionary, zstd_cdict );
    }
    return zstd_compress_str( input );
  }
#else
  (void)allow_zstd;
  (void)allow_zstd_dictionary;
#endif
  return zlib_compress_str( input );
}

std::string Compressor::uncompress_str( const std::string& input )
{
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
  if ( zstd_cdict != NULL ) {
    ZSTD_freeCDict( reinterpret_cast<ZSTD_CDict*>( zstd_cdict ) );
    zstd_cdict = NULL;
  }
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
