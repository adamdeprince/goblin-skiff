/*
    Goblin Skiff: wide gamut and HDR colors for terminals without them
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/colorspace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Terminal {

namespace {
/* Linear light of an sRGB encoded component, and back. */
float decode( uint8_t v )
{
  static float table[256];
  static bool ready = false;
  if ( !ready ) {
    for ( int i = 0; i < 256; i++ ) {
      const double c = i / 255.0;
      table[i] = float( c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 ) );
    }
    ready = true;
  }
  return table[v];
}

uint8_t encode( double linear )
{
  linear = std::min( 1.0, std::max( 0.0, linear ) );
  const double c = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow( linear, 1 / 2.4 ) - 0.055;
  return uint8_t( std::lround( c * 255 ) );
}

/* From the primaries of a gamut to those of sRGB, in linear light; both
   have the D65 white. */
const double P3_TO_SRGB[3][3] = { { 1.2249401, -0.2249404, 0.0 },
                                  { -0.0420569, 1.0420571, 0.0 },
                                  { -0.0196376, -0.0786361, 1.0982735 } };
const double REC2020_TO_SRGB[3][3] = { { 1.6604910, -0.5876411, -0.0728499 },
                                       { -0.1245505, 1.1328999, -0.0083494 },
                                       { -0.0181508, -0.1005789, 1.1187297 } };

}

float half_to_float( uint16_t h )
{
  const uint32_t sign = uint32_t( h & 0x8000 ) << 16;
  const uint32_t exponent = ( h >> 10 ) & 0x1f;
  uint32_t mantissa = h & 0x3ff;
  uint32_t bits;
  if ( exponent == 0 ) {
    if ( mantissa == 0 ) {
      bits = sign;
    } else { /* subnormal */
      int shift = 0;
      while ( !( mantissa & 0x400 ) ) {
        mantissa <<= 1;
        shift++;
      }
      mantissa &= 0x3ff;
      bits = sign | uint32_t( 127 - 15 - shift + 1 ) << 23 | mantissa << 13;
    }
  } else if ( exponent == 31 ) {
    bits = sign | 0x7f800000 | mantissa << 13;
  } else {
    bits = sign | ( exponent + 127 - 15 ) << 23 | mantissa << 13;
  }
  float out;
  std::memcpy( &out, &bits, sizeof out );
  return out;
}

void decode_image_color( uint32_t transfer, uint32_t gamut, double c[4] )
{
  for ( unsigned k = 0; k < 3; ++k ) {
    const double v = c[k];
    if ( transfer == 8 ) { continue; }
    if ( transfer == 16 ) {
      const double p = std::pow( v, 1.0 / ( 2523.0 / 32 ) );
      c[k] = std::pow( std::max( p - 3424.0 / 4096, 0.0 ) / ( 2413.0 / 128 - ( 2392.0 / 128 ) * p ),
                       1.0 / ( 2610.0 / 16384 ) ) * ( 10000.0 / 203 );
    } else if ( transfer == 18 ) {
      c[k] = v <= 0.5 ? v * v / 3 : ( std::exp( ( v - 0.55991073 ) / 0.17883277 ) + 0.28466892 ) / 12;
    } else {
      c[k] = v <= 0.04045 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 );
    }
  }
  if ( transfer == 18 ) {
    const double y = gamut == 2 ? 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]
                     : gamut == 1 ? 0.22897456 * c[0] + 0.69173852 * c[1] + 0.07928691 * c[2]
                     : 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    const double scale = std::pow( std::max( y, 0.0 ), 0.2 ) * ( 1000.0 / 203 );
    for ( unsigned k = 0; k < 3; ++k ) { c[k] *= scale; }
  }
  if ( gamut == 1 || gamut == 2 ) {
    const double ( *m )[3] = gamut == 1 ? P3_TO_SRGB : REC2020_TO_SRGB;
    const double r = c[0], g = c[1], b = c[2];
    for ( unsigned k = 0; k < 3; ++k ) { c[k] = m[k][0] * r + m[k][1] * g + m[k][2] * b; }
  }
}

uint16_t float_to_half( float f )
{
  uint32_t bits, mant;
  std::memcpy( &bits, &f, sizeof bits );
  const uint16_t sign = ( bits >> 16 ) & 0x8000;
  const int exp = int( ( bits >> 23 ) & 0xff ) - 112;
  mant = bits & 0x7fffff;
  if ( exp == 143 ) {
    return sign | 0x7c00 | ( mant ? 0x200 : 0 ); /* inf or nan */
  }
  if ( exp >= 31 ) {
    return sign | 0x7c00;
  }
  unsigned shift = 13;
  if ( exp <= 0 ) { /* subnormal */
    shift = 14 - exp;
    if ( shift > 24 ) {
      return sign;
    }
    mant |= 0x800000;
  }
  uint32_t h = ( exp > 0 ? uint32_t( exp ) << 10 : 0 ) | ( mant >> shift );
  const uint32_t rem = mant & ( ( 1u << shift ) - 1 ), half = 1u << ( shift - 1 );
  if ( rem > half || ( rem == half && ( h & 1 ) ) ) {
    h++;
  }
  return uint16_t( sign | h );
}

namespace {
/* The inverse of a 3x3 matrix. */
void invert( const double m[3][3], double out[3][3] )
{
  const double det = m[0][0] * ( m[1][1] * m[2][2] - m[1][2] * m[2][1] )
                     - m[0][1] * ( m[1][0] * m[2][2] - m[1][2] * m[2][0] )
                     + m[0][2] * ( m[1][0] * m[2][1] - m[1][1] * m[2][0] );
  out[0][0] = ( m[1][1] * m[2][2] - m[1][2] * m[2][1] ) / det;
  out[0][1] = ( m[0][2] * m[2][1] - m[0][1] * m[2][2] ) / det;
  out[0][2] = ( m[0][1] * m[1][2] - m[0][2] * m[1][1] ) / det;
  out[1][0] = ( m[1][2] * m[2][0] - m[1][0] * m[2][2] ) / det;
  out[1][1] = ( m[0][0] * m[2][2] - m[0][2] * m[2][0] ) / det;
  out[1][2] = ( m[0][2] * m[1][0] - m[0][0] * m[1][2] ) / det;
  out[2][0] = ( m[1][0] * m[2][1] - m[1][1] * m[2][0] ) / det;
  out[2][1] = ( m[0][1] * m[2][0] - m[0][0] * m[2][1] ) / det;
  out[2][2] = ( m[0][0] * m[1][1] - m[0][1] * m[1][0] ) / det;
}
}

void gamut_to_srgb( unsigned gamut, uint8_t& red, uint8_t& green, uint8_t& blue )
{
  if ( gamut != 1 && gamut != 2 ) {
    return;
  }
  const double( *m )[3] = gamut == 1 ? P3_TO_SRGB : REC2020_TO_SRGB;
  const double r = decode( red ), g = decode( green ), b = decode( blue );
  red = encode( m[0][0] * r + m[0][1] * g + m[0][2] * b );
  green = encode( m[1][0] * r + m[1][1] * g + m[1][2] * b );
  blue = encode( m[2][0] * r + m[2][1] * g + m[2][2] * b );
}

void gamut_pixels_to_srgb( unsigned gamut, std::string& pixels, size_t bytes_per_pixel )
{
  if ( ( gamut != 1 && gamut != 2 ) || bytes_per_pixel < 3 ) {
    return;
  }
  for ( size_t at = 0; at + bytes_per_pixel <= pixels.size(); at += bytes_per_pixel ) {
    uint8_t* p = reinterpret_cast<uint8_t*>( &pixels[at] );
    gamut_to_srgb( gamut, p[0], p[1], p[2] );
  }
}

void convert_pixels_between_gamuts( unsigned from, unsigned to, std::string& pixels, size_t bytes_per_pixel )
{
  from = from <= 2 ? from : 0;
  to = to <= 2 ? to : 0;
  if ( from == to || bytes_per_pixel < 3 ) {
    return;
  }
  /* From the primaries of one to those of the other, through those of sRGB. */
  static const double IDENTITY[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
  const double( *to_srgb )[3] = from == 1 ? P3_TO_SRGB : from == 2 ? REC2020_TO_SRGB : IDENTITY;
  double from_srgb[3][3];
  invert( to == 1 ? P3_TO_SRGB : to == 2 ? REC2020_TO_SRGB : IDENTITY, from_srgb );
  double m[3][3];
  for ( int r = 0; r < 3; r++ ) {
    for ( int c = 0; c < 3; c++ ) {
      m[r][c] = from_srgb[r][0] * to_srgb[0][c] + from_srgb[r][1] * to_srgb[1][c] + from_srgb[r][2] * to_srgb[2][c];
    }
  }
  for ( size_t at = 0; at + bytes_per_pixel <= pixels.size(); at += bytes_per_pixel ) {
    uint8_t* p = reinterpret_cast<uint8_t*>( &pixels[at] );
    const double r = decode( p[0] ), g = decode( p[1] ), b = decode( p[2] );
    p[0] = encode( m[0][0] * r + m[0][1] * g + m[0][2] * b );
    p[1] = encode( m[1][0] * r + m[1][1] * g + m[1][2] * b );
    p[2] = encode( m[2][0] * r + m[2][1] * g + m[2][2] * b );
  }
}

bool hdr_to_srgb8( const std::string& half_floats, uint32_t width, uint32_t height, bool tone_map, std::string& rgba )
{
  const size_t pixels = size_t( width ) * height;
  if ( !width || !height || pixels / width != height || half_floats.size() / 8 != pixels
       || half_floats.size() % 8 ) {
    return false;
  }
  std::vector<float> values( pixels * 4 );
  for ( size_t i = 0; i < pixels * 4; i++ ) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>( half_floats.data() ) + i * 2;
    values[i] = half_to_float( uint16_t( p[0] | p[1] << 8 ) ); /* little endian */
  }
  /* With T=1 the bright parts are compressed into the headroom, here 1:
     up to the knee nothing changes, and the brightest component lands at 1. */
  const double headroom = 1, knee = 0.75 * headroom;
  double peak = 1;
  if ( tone_map ) {
    for ( size_t i = 0; i < pixels; i++ ) {
      for ( int c = 0; c < 3; c++ ) {
        if ( std::isfinite( values[i * 4 + c] ) ) {
          peak = std::max<double>( peak, values[i * 4 + c] );
        }
      }
    }
  }
  const double s = peak > headroom ? 1 / ( headroom - knee ) - 1 / ( peak - knee ) : 0;
  rgba.resize( pixels * 4 );
  for ( size_t i = 0; i < pixels; i++ ) {
    double r = values[i * 4], g = values[i * 4 + 1], b = values[i * 4 + 2];
    const double m = std::max( { r, g, b } );
    if ( tone_map && peak > headroom && m > knee ) {
      const double f = knee + ( m - knee ) / ( 1 + s * ( m - knee ) );
      r *= f / m;
      g *= f / m;
      b *= f / m;
    }
    rgba[i * 4] = char( encode( r ) );
    rgba[i * 4 + 1] = char( encode( g ) );
    rgba[i * 4 + 2] = char( encode( b ) );
    const double a = std::isfinite( values[i * 4 + 3] ) ? std::min( 1.0, std::max( 0.0, double( values[i * 4 + 3] ) ) ) : 1;
    rgba[i * 4 + 3] = char( std::lround( a * 255 ) );
  }
  return true;
}

}
