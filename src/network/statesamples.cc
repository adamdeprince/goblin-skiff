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

#include "src/network/statesamples.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef HAVE_ZSTD
#include <zdict.h>
#include <zstd.h>
#endif

namespace {
const char SAMPLE_MAGIC[] = { 'M', 'S', 'M', 'P', '1', '\0' };
const int STATE_DICTIONARY_FILE_ZSTD_LEVEL = 22;

void append_u32( std::string& out, uint32_t value )
{
  out.push_back( static_cast<char>( ( value >> 24 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 16 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
}

bool read_u32( const std::string& in, size_t& offset, uint32_t& value )
{
  if ( offset + 4 > in.size() ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 4; i++ ) {
    value = ( value << 8 ) | static_cast<unsigned char>( in[offset + i] );
  }
  offset += 4;
  return true;
}

bool looks_like_zstd_frame( const std::string& input )
{
  return input.size() >= 4 && static_cast<unsigned char>( input[0] ) == 0x28
         && static_cast<unsigned char>( input[1] ) == 0xb5 && static_cast<unsigned char>( input[2] ) == 0x2f
         && static_cast<unsigned char>( input[3] ) == 0xfd;
}

std::string compress_zstd_buffer( const std::string& input, int level, const char* description )
{
#ifdef HAVE_ZSTD
  const size_t bound = ZSTD_compressBound( input.size() );
  std::string output( bound, '\0' );
  const size_t written = ZSTD_compress( &output[0], output.size(), input.data(), input.size(), level );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd " ) + description + " compression failed: "
                              + ZSTD_getErrorName( written ) );
  }
  output.resize( written );
  return output;
#else
  (void)input;
  (void)level;
  (void)description;
  throw std::runtime_error( "zstd support was not enabled at configure time" );
#endif
}

std::string decompress_zstd_stream( const std::string& input, const char* description )
{
#ifdef HAVE_ZSTD
  ZSTD_DStream* stream = ZSTD_createDStream();
  if ( stream == NULL ) {
    throw std::runtime_error( std::string( "could not allocate zstd " ) + description + " decompressor" );
  }

  std::string output;
  std::vector<char> out_buffer( ZSTD_DStreamOutSize() );
  ZSTD_inBuffer in = { input.data(), input.size(), 0 };
  size_t ret = 0;
  while ( in.pos < in.size ) {
    ZSTD_outBuffer out = { out_buffer.data(), out_buffer.size(), 0 };
    ret = ZSTD_decompressStream( stream, &out, &in );
    if ( ZSTD_isError( ret ) ) {
      ZSTD_freeDStream( stream );
      throw std::runtime_error( std::string( "zstd " ) + description + " decompression failed: "
                                + ZSTD_getErrorName( ret ) );
    }
    output.append( out_buffer.data(), out.pos );
  }

  ZSTD_freeDStream( stream );
  if ( ret != 0 ) {
    throw std::runtime_error( std::string( "truncated zstd " ) + description );
  }
  return output;
#else
  (void)input;
  (void)description;
  throw std::runtime_error( "zstd support was not enabled at configure time" );
#endif
}

std::string hex64( uint64_t value )
{
  static const char hexdigits[] = "0123456789abcdef";
  std::string out( 16, '0' );
  for ( int i = 15; i >= 0; i-- ) {
    out[i] = hexdigits[value & 0xf];
    value >>= 4;
  }
  return out;
}
}

using namespace Network;

StateSampleWriter::StateSampleWriter() : file( NULL ), zstd_stream( NULL ), out_buffer(), min_size( 0 ), closed( true ) {}

StateSampleWriter::StateSampleWriter( const std::string& path, size_t s_min_size )
  : file( NULL ), zstd_stream( NULL ), out_buffer(), min_size( 0 ), closed( true )
{
  open( path, s_min_size );
}

StateSampleWriter::~StateSampleWriter()
{
  try {
    close();
  } catch ( const std::exception& ) {
  }
}

void StateSampleWriter::open( const std::string& path, size_t s_min_size )
{
  close();
  if ( path.empty() ) {
    return;
  }

#ifdef HAVE_ZSTD
  file = fopen( path.c_str(), "wb" );
  if ( file == NULL ) {
    throw std::runtime_error( std::string( "could not open state sample log: " ) + strerror( errno ) );
  }

  zstd_stream = ZSTD_createCStream();
  if ( zstd_stream == NULL ) {
    fclose( file );
    file = NULL;
    throw std::runtime_error( "could not allocate zstd sample compressor" );
  }

  out_buffer.assign( ZSTD_CStreamOutSize(), '\0' );
  min_size = s_min_size;
  closed = false;
  write_compressed( SAMPLE_MAGIC, sizeof SAMPLE_MAGIC, ZSTD_e_flush );
#else
  (void)s_min_size;
  throw std::runtime_error( "state sample logging requires libzstd support" );
#endif
}

void StateSampleWriter::write_compressed( const void* data, size_t size, int directive )
{
#ifdef HAVE_ZSTD
  ZSTD_inBuffer in = { data, size, 0 };
  while ( in.pos < in.size || directive != ZSTD_e_continue ) {
    ZSTD_outBuffer out = { out_buffer.data(), out_buffer.size(), 0 };
    const size_t ret = ZSTD_compressStream2( reinterpret_cast<ZSTD_CStream*>( zstd_stream ),
                                             &out,
                                             &in,
                                             static_cast<ZSTD_EndDirective>( directive ) );
    if ( ZSTD_isError( ret ) ) {
      throw std::runtime_error( std::string( "zstd sample compression failed: " ) + ZSTD_getErrorName( ret ) );
    }
    if ( out.pos && fwrite( out_buffer.data(), 1, out.pos, file ) != out.pos ) {
      throw std::runtime_error( std::string( "write state sample log: " ) + strerror( errno ) );
    }
    if ( ret == 0 ) {
      break;
    }
  }
#else
  (void)data;
  (void)size;
  (void)directive;
#endif
}

void StateSampleWriter::close( void )
{
#ifdef HAVE_ZSTD
  if ( zstd_stream != NULL && !closed ) {
    write_compressed( NULL, 0, ZSTD_e_end );
    closed = true;
  }
  if ( zstd_stream != NULL ) {
    ZSTD_freeCStream( reinterpret_cast<ZSTD_CStream*>( zstd_stream ) );
    zstd_stream = NULL;
  }
#endif
  if ( file != NULL ) {
    if ( fclose( file ) != 0 ) {
      file = NULL;
      throw std::runtime_error( std::string( "close state sample log: " ) + strerror( errno ) );
    }
    file = NULL;
  }
}

void StateSampleWriter::write_record( const std::string& sample )
{
  if ( !enabled() || sample.size() < min_size ) {
    return;
  }
  if ( sample.size() > std::numeric_limits<uint32_t>::max() ) {
    throw std::runtime_error( "state sample is too large to frame" );
  }

  std::string record;
  append_u32( record, static_cast<uint32_t>( sample.size() ) );
  record.append( sample );
  write_compressed( record.data(), record.size(), ZSTD_e_flush );
}

std::string Network::read_file_bytes( const std::string& path )
{
  std::ifstream in( path.c_str(), std::ios::in | std::ios::binary );
  if ( !in ) {
    throw std::runtime_error( "could not open file: " + path );
  }
  return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
}

std::vector<std::string> Network::read_state_sample_file( const std::string& path, size_t max_samples )
{
  const std::string compressed = read_file_bytes( path );
  const std::string raw = decompress_zstd_stream( compressed, "sample file" );
  if ( raw.size() < sizeof SAMPLE_MAGIC || memcmp( raw.data(), SAMPLE_MAGIC, sizeof SAMPLE_MAGIC ) != 0 ) {
    throw std::runtime_error( "bad state sample file magic" );
  }

  size_t offset = sizeof SAMPLE_MAGIC;
  std::vector<std::string> samples;
  while ( offset < raw.size() && ( max_samples == 0 || samples.size() < max_samples ) ) {
    uint32_t size = 0;
    if ( !read_u32( raw, offset, size ) || offset + size > raw.size() ) {
      throw std::runtime_error( "truncated state sample record" );
    }
    samples.push_back( raw.substr( offset, size ) );
    offset += size;
  }
  return samples;
}

std::string Network::train_state_dictionary( const std::vector<std::string>& samples, size_t dictionary_size )
{
#ifdef HAVE_ZSTD
  if ( samples.empty() ) {
    throw std::runtime_error( "no samples available for dictionary training" );
  }
  if ( dictionary_size == 0 ) {
    throw std::runtime_error( "dictionary size must be greater than zero" );
  }

  std::string joined;
  std::vector<size_t> sizes;
  sizes.reserve( samples.size() );
  for ( std::vector<std::string>::const_iterator it = samples.begin(); it != samples.end(); ++it ) {
    if ( it->empty() ) {
      continue;
    }
    joined.append( *it );
    sizes.push_back( it->size() );
  }
  if ( sizes.empty() ) {
    throw std::runtime_error( "no non-empty samples available for dictionary training" );
  }

  std::string dictionary( dictionary_size, '\0' );
  const size_t written = ZDICT_trainFromBuffer( &dictionary[0], dictionary.size(), joined.data(), &sizes[0], sizes.size() );
  if ( ZDICT_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd dictionary training failed: " ) + ZDICT_getErrorName( written ) );
  }
  dictionary.resize( written );
  return dictionary;
#else
  (void)samples;
  (void)dictionary_size;
  throw std::runtime_error( "dictionary training requires libzstd support" );
#endif
}

std::string Network::compress_state_dictionary_file( const std::string& dictionary )
{
  return compress_zstd_buffer( dictionary, STATE_DICTIONARY_FILE_ZSTD_LEVEL, "dictionary file" );
}

std::string Network::read_state_dictionary_file( const std::string& path )
{
  const std::string data = read_file_bytes( path );
  if ( looks_like_zstd_frame( data ) ) {
    return decompress_zstd_stream( data, "dictionary file" );
  }
  return data;
}

std::string Network::state_dictionary_id( const std::string& dictionary )
{
  uint64_t hash = 1469598103934665603ULL;
  for ( std::string::const_iterator it = dictionary.begin(); it != dictionary.end(); ++it ) {
    hash ^= static_cast<unsigned char>( *it );
    hash *= 1099511628211ULL;
  }
  return hex64( hash );
}
