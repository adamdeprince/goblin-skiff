/*
    Goblin Skiff: Silkie's packed image formats, kept packed
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/include/config.h"

#include "src/terminal/packedimage.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <zlib.h>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

/* The container, "packed-state-v2":

     byte 0   the compressor: 0 none, 1 zlib, 2 zstd ("packed-zstd-v1")
     the rest the compressed rows

   A row is one byte naming its filter and then its samples. The samples of a
   grayscale image are the bytes of the row as the program packed them. Those
   of RGB565 are three bytes a pixel, red, green and blue as their five, six
   and five bit values: a channel a byte compresses far better than words
   whose bytes each hold parts of two channels, and costs nothing, as the
   compressor spends bits by what a byte can be and not by its width.

   The filters are PNG's, over bytes, with the pixel to the left one sample
   away (three for RGB565): 0 none, 1 sub, 2 up, 3 average, 4 Paeth. Filter 5
   is the exclusive or with the row above, which is what a row of one-bit
   pixels shares with its neighbour.

   Nothing here is sized by anything but the image. */

namespace Terminal {

namespace {
enum Method : unsigned char
{
  STORED = 0,
  ZLIB = 1,
  ZSTD = 2
};
enum Filter : unsigned char
{
  NONE = 0,
  SUB = 1,
  UP = 2,
  AVERAGE = 3,
  PAETH = 4,
  XOR_UP = 5,
  FILTERS = 6
};

/* The samples of a row, and how far apart the samples of neighbouring pixels are. */
bool sample_layout( uint32_t format, uint32_t width, uint32_t height, size_t& row, size_t& step, size_t& stream )
{
  size_t packed_row = 0, packed_total = 0;
  if ( !packed_size( format, width, height, packed_row, packed_total ) ) {
    return false;
  }
  if ( format == 516 ) {
    if ( size_t( width ) > ( SIZE_MAX - 1 ) / 3 ) {
      return false;
    }
    row = size_t( width ) * 3;
    step = 3;
  } else {
    row = packed_row;
    step = 1;
  }
  if ( size_t( height ) > SIZE_MAX / ( row + 1 ) ) {
    return false;
  }
  stream = ( row + 1 ) * height;
  return true;
}

unsigned char paeth( unsigned char a, unsigned char b, unsigned char c )
{
  const int p = int( a ) + int( b ) - int( c );
  const int pa = abs( p - int( a ) ), pb = abs( p - int( b ) ), pc = abs( p - int( c ) );
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Writes the filtered row to out, and returns what PNG's encoders judge a
   filter by: the sum of the filtered bytes as distances from zero. */
uint64_t filter_row( Filter filter, const unsigned char* cur, const unsigned char* prev, size_t row, size_t step,
                     unsigned char* out )
{
  uint64_t cost = 0;
  for ( size_t i = 0; i < row; i++ ) {
    const unsigned char a = i >= step ? cur[i - step] : 0;
    const unsigned char b = prev ? prev[i] : 0;
    const unsigned char c = prev && i >= step ? prev[i - step] : 0;
    unsigned char v = cur[i];
    switch ( filter ) {
      case NONE:
        break;
      case SUB:
        v = static_cast<unsigned char>( v - a );
        break;
      case UP:
        v = static_cast<unsigned char>( v - b );
        break;
      case AVERAGE:
        v = static_cast<unsigned char>( v - ( ( unsigned( a ) + unsigned( b ) ) >> 1 ) );
        break;
      case PAETH:
        v = static_cast<unsigned char>( v - paeth( a, b, c ) );
        break;
      case XOR_UP:
        v = static_cast<unsigned char>( v ^ b );
        break;
      default:
        break;
    }
    out[i] = v;
    cost += v < 128 ? v : 256 - v;
  }
  return cost;
}

void unfilter_row( Filter filter, unsigned char* cur, const unsigned char* prev, size_t row, size_t step )
{
  for ( size_t i = 0; i < row; i++ ) {
    const unsigned char a = i >= step ? cur[i - step] : 0;
    const unsigned char b = prev ? prev[i] : 0;
    const unsigned char c = prev && i >= step ? prev[i - step] : 0;
    switch ( filter ) {
      case SUB:
        cur[i] = static_cast<unsigned char>( cur[i] + a );
        break;
      case UP:
        cur[i] = static_cast<unsigned char>( cur[i] + b );
        break;
      case AVERAGE:
        cur[i] = static_cast<unsigned char>( cur[i] + ( ( unsigned( a ) + unsigned( b ) ) >> 1 ) );
        break;
      case PAETH:
        cur[i] = static_cast<unsigned char>( cur[i] + paeth( a, b, c ) );
        break;
      case XOR_UP:
        cur[i] = static_cast<unsigned char>( cur[i] ^ b );
        break;
      default:
        break;
    }
  }
}

/* The rows with one filter throughout, or, for adaptive, with the filter
   each row is cheapest in. */
std::string filtered( const std::string& samples, size_t row, size_t step, uint32_t first, uint32_t count,
                      bool adaptive, Filter fixed )
{
  std::string out( ( row + 1 ) * count, '\0' );
  std::vector<unsigned char> trial( adaptive ? row : 0 );
  const unsigned char* in = reinterpret_cast<const unsigned char*>( samples.data() );
  for ( uint32_t y = first; y < first + count; y++ ) {
    const unsigned char* cur = in + size_t( y ) * row;
    const unsigned char* prev = y ? cur - row : nullptr;
    unsigned char* dest = reinterpret_cast<unsigned char*>( &out[size_t( y - first ) * ( row + 1 )] );
    Filter chosen = fixed;
    if ( adaptive ) {
      uint64_t best = UINT64_MAX;
      for ( Filter candidate : { NONE, SUB, UP, AVERAGE, PAETH } ) {
        const uint64_t cost = filter_row( candidate, cur, prev, row, step, trial.data() );
        if ( cost < best ) {
          best = cost;
          chosen = candidate;
        }
      }
    }
    dest[0] = chosen;
    filter_row( chosen, cur, prev, row, step, dest + 1 );
  }
  return out;
}

bool deflate_to( const std::string& in, int level, std::string& out )
{
  if ( in.size() > ULONG_MAX ) {
    return false;
  }
  uLongf size = compressBound( static_cast<uLong>( in.size() ) );
  out.assign( size, '\0' );
  if ( compress2( reinterpret_cast<Bytef*>( &out[0] ), &size, reinterpret_cast<const Bytef*>( in.data() ),
                  static_cast<uLong>( in.size() ), level )
       != Z_OK ) {
    return false;
  }
  out.resize( size );
  return true;
}

bool zstd_to( const std::string& in, int level, std::string& out )
{
#ifdef HAVE_ZSTD
  out.assign( ZSTD_compressBound( in.size() ), '\0' );
  const size_t size = ZSTD_compress( &out[0], out.size(), in.data(), in.size(), level );
  if ( ZSTD_isError( size ) ) {
    return false;
  }
  out.resize( size );
  return true;
#else
  (void)in;
  (void)level;
  (void)out;
  return false;
#endif
}

/* The effort a compressor is given: all of it for what is compressed in a
   moment anyway, less for an image so large that the session, which does
   nothing else meanwhile, would wait on it. These choose how hard to try,
   never what can be stored. */
const size_t SMALL_STREAM = size_t( 1 ) << 18, MEDIUM_STREAM = size_t( 1 ) << 21;
/* How much of a large image the filterings are tried on: bands of its rows,
   from the top, the middle and the bottom. */
const size_t TRIAL_BYTES = size_t( 1 ) << 18;
}

bool packed_format( uint32_t format )
{
  return format == 301 || format == 302 || format == 304 || format == 308 || format == 516;
}

bool packed_zstd_available( void )
{
#ifdef HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

bool packed_size( uint32_t format, uint32_t width, uint32_t height, size_t& row, size_t& size )
{
  if ( !packed_format( format ) || !width || !height ) {
    return false;
  }
  if ( format < 308 ) {
    const unsigned per_byte = 8 / ( format - 300 );
    row = size_t( width ) / per_byte + ( width % per_byte != 0 );
  } else {
    const size_t per_pixel = ( format == 516 ? 2 : 1 );
    if ( size_t( width ) > SIZE_MAX / per_pixel ) {
      return false;
    }
    row = size_t( width ) * per_pixel;
  }
  if ( size_t( height ) > SIZE_MAX / row ) {
    return false;
  }
  size = row * height;
  return true;
}

bool packed_expand( uint32_t format, uint32_t width, uint32_t height, const std::string& packed, bool alpha,
                    std::string& pixels )
{
  size_t row = 0, size = 0;
  const size_t channels = alpha ? 4 : 3;
  if ( !packed_size( format, width, height, row, size ) || packed.size() != size
       || size_t( width ) > SIZE_MAX / channels || size_t( height ) > SIZE_MAX / ( size_t( width ) * channels ) ) {
    return false;
  }
  pixels.assign( size_t( width ) * height * channels, char( 0xff ) );
  unsigned char* dst = reinterpret_cast<unsigned char*>( &pixels[0] );
  for ( uint32_t y = 0; y < height; y++ ) {
    const unsigned char* src = reinterpret_cast<const unsigned char*>( packed.data() ) + size_t( y ) * row;
    if ( format == 516 ) {
      for ( uint32_t x = 0; x < width; x++, dst += channels ) {
        /* A little-endian word whatever this machine is; each channel to
           eight bits, rounded to nearest. */
        const unsigned value = src[2 * size_t( x )] | ( unsigned( src[2 * size_t( x ) + 1] ) << 8 );
        dst[0] = static_cast<unsigned char>( ( ( ( value >> 11 ) & 31 ) * 255 + 15 ) / 31 );
        dst[1] = static_cast<unsigned char>( ( ( ( value >> 5 ) & 63 ) * 255 + 31 ) / 63 );
        dst[2] = static_cast<unsigned char>( ( ( value & 31 ) * 255 + 15 ) / 31 );
      }
    } else {
      const unsigned per_byte = 8 / ( format - 300 ), maximum = ( 1u << ( format - 300 ) ) - 1;
      for ( uint32_t x = 0; x < width; x++, dst += channels ) {
        const unsigned shift = 8 - ( format - 300 ) - ( x % per_byte ) * ( format - 300 );
        const unsigned value = ( src[x / per_byte] >> shift ) & maximum;
        dst[0] = dst[1] = dst[2] = static_cast<unsigned char>( value * ( 255 / maximum ) );
      }
    }
  }
  return true;
}

bool packed_encode( uint32_t format, uint32_t width, uint32_t height, const std::string& packed, bool zstd,
                    std::string& container )
{
  size_t packed_row = 0, packed_total = 0, row = 0, step = 0, stream = 0;
  if ( !packed_size( format, width, height, packed_row, packed_total ) || packed.size() != packed_total
       || !sample_layout( format, width, height, row, step, stream ) ) {
    return false;
  }
  zstd = zstd && packed_zstd_available();

  /* The samples the filters work on. The bits a row leaves unused in its
     last byte are the program's and are kept. */
  std::string split;
  if ( format == 516 ) {
    split.resize( row * height );
    for ( size_t i = 0, n = size_t( width ) * height; i < n; i++ ) {
      const unsigned value = static_cast<unsigned char>( packed[2 * i] )
                             | ( unsigned( static_cast<unsigned char>( packed[2 * i + 1] ) ) << 8 );
      split[3 * i] = static_cast<char>( ( value >> 11 ) & 31 );
      split[3 * i + 1] = static_cast<char>( ( value >> 5 ) & 63 );
      split[3 * i + 2] = static_cast<char>( value & 31 );
    }
  }
  const std::string& samples = format == 516 ? split : packed;

  /* Which filtering suits an image depends on what is in it: line art and
     text compress best untouched or against the row above, photographs with
     a filter chosen row by row. Each is tried with a quick compressor, on
     bands of an image too large to try whole, and the best is then given
     the effort its size allows. */
  struct Candidate
  {
    bool adaptive;
    Filter fixed;
  };
  std::vector<Candidate> candidates { { false, NONE }, { false, UP } };
  if ( format < 308 ) {
    candidates.push_back( { false, XOR_UP } );
  } else {
    candidates.push_back( { true, NONE } );
    candidates.push_back( { false, PAETH } );
  }
  const uint32_t band = static_cast<uint32_t>(
    std::min<size_t>( height, std::max<size_t>( 1, TRIAL_BYTES / 3 / ( row + 1 ) ) ) );
  const bool sampled = size_t( band ) * 3 < height;
  Candidate best = candidates[0];
  size_t best_size = SIZE_MAX;
  for ( const Candidate& candidate : candidates ) {
    std::string rows, trial;
    if ( sampled ) {
      for ( uint32_t first : { uint32_t( 0 ), ( height - band ) / 2, height - band } ) {
        rows += filtered( samples, row, step, first, band, candidate.adaptive, candidate.fixed );
      }
    } else {
      rows = filtered( samples, row, step, 0, height, candidate.adaptive, candidate.fixed );
    }
    if ( !( zstd ? zstd_to( rows, 3, trial ) : deflate_to( rows, 1, trial ) ) ) {
      continue;
    }
    if ( trial.size() < best_size ) {
      best_size = trial.size();
      best = candidate;
    }
  }
  const std::string best_stream = filtered( samples, row, step, 0, height, best.adaptive, best.fixed );

  std::string compressed;
  Method method = STORED;
  if ( zstd ) {
    const int level = stream <= SMALL_STREAM ? 19 : stream <= MEDIUM_STREAM ? 12 : 6;
    if ( zstd_to( best_stream, level, compressed ) ) {
      method = ZSTD;
    }
  }
  if ( method == STORED
       && deflate_to( best_stream, stream <= SMALL_STREAM ? 9 : stream <= MEDIUM_STREAM ? 6 : 4, compressed ) ) {
    method = ZLIB;
  }
  if ( method == STORED || compressed.size() >= stream ) {
    /* Nothing to gain: the rows as they are. */
    method = STORED;
    compressed = filtered( samples, row, step, 0, height, false, NONE );
  }
  container.assign( 1, static_cast<char>( method ) );
  container += compressed;
  return true;
}

bool packed_decode( uint32_t format, uint32_t width, uint32_t height, const std::string& container,
                    std::string& packed )
{
  size_t packed_row = 0, packed_total = 0, row = 0, step = 0, stream = 0;
  if ( container.empty() || !packed_size( format, width, height, packed_row, packed_total )
       || !sample_layout( format, width, height, row, step, stream ) ) {
    return false;
  }
  /* The rows are exactly as long as the image says, so what does not come to
     that length is not this image. */
  std::string rows;
  const char* body = container.data() + 1;
  const size_t body_size = container.size() - 1;
  switch ( static_cast<unsigned char>( container[0] ) ) {
    case STORED:
      if ( body_size != stream ) {
        return false;
      }
      rows.assign( body, body_size );
      break;
    case ZLIB: {
      if ( stream > ULONG_MAX || body_size > ULONG_MAX ) {
        return false;
      }
      rows.assign( stream, '\0' );
      uLongf size = static_cast<uLongf>( stream );
      if ( uncompress( reinterpret_cast<Bytef*>( &rows[0] ), &size, reinterpret_cast<const Bytef*>( body ),
                       static_cast<uLong>( body_size ) )
             != Z_OK
           || size != stream ) {
        return false;
      }
      break;
    }
    case ZSTD: {
#ifdef HAVE_ZSTD
      rows.assign( stream, '\0' );
      const size_t size = ZSTD_decompress( &rows[0], rows.size(), body, body_size );
      if ( ZSTD_isError( size ) || size != stream ) {
        return false;
      }
      break;
#else
      return false;
#endif
    }
    default:
      return false;
  }

  std::string samples( row * height, '\0' );
  for ( uint32_t y = 0; y < height; y++ ) {
    const unsigned char* src = reinterpret_cast<const unsigned char*>( rows.data() ) + size_t( y ) * ( row + 1 );
    if ( src[0] >= FILTERS ) {
      return false;
    }
    unsigned char* cur = reinterpret_cast<unsigned char*>( &samples[size_t( y ) * row] );
    memcpy( cur, src + 1, row );
    unfilter_row( static_cast<Filter>( src[0] ), cur, y ? cur - row : nullptr, row, step );
  }
  if ( format != 516 ) {
    packed.swap( samples );
    return true;
  }
  packed.assign( packed_total, '\0' );
  for ( size_t i = 0, n = size_t( width ) * height; i < n; i++ ) {
    const unsigned r = static_cast<unsigned char>( samples[3 * i] ), g = static_cast<unsigned char>( samples[3 * i + 1] ),
                   b = static_cast<unsigned char>( samples[3 * i + 2] );
    if ( r > 31 || g > 63 || b > 31 ) {
      return false;
    }
    const unsigned value = ( r << 11 ) | ( g << 5 ) | b;
    packed[2 * i] = static_cast<char>( value & 0xff );
    packed[2 * i + 1] = static_cast<char>( value >> 8 );
  }
  return true;
}

}
