/* Distributed under the GNU GPL, version 3 or later. */
#include "sixel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace Terminal {
namespace Sixel {
namespace {
uint32_t rgb( unsigned r, unsigned g, unsigned b )
{
  return ( r << 16 ) | ( g << 8 ) | b;
}

unsigned byte_from_percent( unsigned value )
{
  return ( value * 255 + 50 ) / 100;
}

bool dimensions( uint32_t width, uint32_t height )
{
  return width > 0 && height > 0 && width <= MAX_DIMENSION && height <= MAX_DIMENSION
         && size_t( width ) * height <= MAX_PIXELS;
}

bool reject( std::string& error, const char* message )
{
  error = message;
  return false;
}

// Parse at most five bounded numeric fields. Empty fields are zero.
bool parameters( const std::string& data, size_t& position, std::vector<unsigned>& values )
{
  values.assign( 1, 0 );
  while ( position < data.size() ) {
    const char c = data[position];
    if ( c == ';' ) {
      if ( values.size() == 5 ) {
        return false;
      }
      values.push_back( 0 );
    } else if ( c >= '0' && c <= '9' ) {
      if ( values.back() > ( 65535U - unsigned( c - '0' ) ) / 10U ) {
        return false;
      }
      values.back() = values.back() * 10 + unsigned( c - '0' );
    } else {
      break;
    }
    position++;
  }
  return true;
}

std::array<uint32_t, MAX_COLORS> initial_palette()
{
  // VT340's first sixteen registers, followed by a neutral default for
  // undefined registers. Encoders normally define every color they use.
  const unsigned defaults[16][3] = { { 0, 0, 0 },
                                     { 20, 20, 80 },
                                     { 80, 13, 13 },
                                     { 20, 80, 20 },
                                     { 80, 20, 80 },
                                     { 20, 80, 80 },
                                     { 80, 80, 20 },
                                     { 53, 53, 53 },
                                     { 26, 26, 26 },
                                     { 33, 33, 60 },
                                     { 60, 26, 26 },
                                     { 33, 60, 33 },
                                     { 60, 33, 60 },
                                     { 33, 60, 60 },
                                     { 60, 60, 33 },
                                     { 80, 80, 80 } };
  std::array<uint32_t, MAX_COLORS> palette = {};
  for ( unsigned i = 0; i < 16; i++ ) {
    palette[i] = rgb( byte_from_percent( defaults[i][0] ),
                      byte_from_percent( defaults[i][1] ),
                      byte_from_percent( defaults[i][2] ) );
  }
  return palette;
}

uint32_t hls( unsigned hue, unsigned lightness, unsigned saturation )
{
  // DEC's hue wheel starts at blue, with red at 120 and green at 240.
  const double h = double( ( hue + 240 ) % 360 ) / 60.0;
  const double l = double( lightness ) / 100.0, s = double( saturation ) / 100.0;
  const double chroma = ( 1.0 - std::abs( 2.0 * l - 1.0 ) ) * s;
  const double intermediate = chroma * ( 1.0 - std::abs( std::fmod( h, 2.0 ) - 1.0 ) );
  double r = 0, g = 0, b = 0;
  if ( h < 1 ) {
    r = chroma;
    g = intermediate;
  } else if ( h < 2 ) {
    r = intermediate;
    g = chroma;
  } else if ( h < 3 ) {
    g = chroma;
    b = intermediate;
  } else if ( h < 4 ) {
    g = intermediate;
    b = chroma;
  } else if ( h < 5 ) {
    r = intermediate;
    b = chroma;
  } else {
    r = chroma;
    b = intermediate;
  }
  const double m = l - chroma / 2.0;
  return rgb( unsigned( std::lround( ( r + m ) * 255 ) ),
              unsigned( std::lround( ( g + m ) * 255 ) ),
              unsigned( std::lround( ( b + m ) * 255 ) ) );
}

unsigned initial_aspect( unsigned parameter )
{
  if ( parameter == 2 ) {
    return 5;
  }
  if ( parameter == 3 || parameter == 4 ) {
    return 3;
  }
  if ( parameter == 0 || parameter == 1 || parameter == 5 || parameter == 6 ) {
    return 2;
  }
  return 1;
}

struct Layout
{
  uint32_t width, height;
  Layout() : width( 0 ), height( 0 ) {}
};

bool walk( const std::string& payload, unsigned aspect, Layout& size, Bitmap* pixels, std::string& error,
           std::array<uint32_t, MAX_COLORS>& palette )
{
  unsigned color = 0;
  uint32_t x = 0, y = 0;
  size_t work = 0;
  std::vector<unsigned> params;
  for ( size_t at = 0; at < payload.size(); ) {
    const unsigned char c = payload[at++];
    if ( c == '$' ) {
      x = 0;
      continue;
    }
    if ( c == '-' ) {
      if ( y > MAX_DIMENSION - 6 * aspect ) {
        return reject( error, "Sixel height limit exceeded" );
      }
      y += 6 * aspect;
      x = 0;
      continue;
    }
    if ( c == 0x1b || c == 0x18 || c == 0x1a ) {
      return reject( error, "Canceled sixel DCS" );
    }
    if ( c < 32 ) {
      continue;
    } // Embedded C0 whitespace is not pixel data.
    if ( c == '#' || c == '"' ) {
      if ( !parameters( payload, at, params ) ) {
        return reject( error, "Invalid sixel parameters" );
      }
      if ( c == '#' ) {
        if ( params[0] >= MAX_COLORS || ( params.size() != 1 && params.size() != 5 ) ) {
          return reject( error, "Invalid sixel color register" );
        }
        color = params[0];
        if ( params.size() == 5 ) {
          if ( params[3] > 100 || params[4] > 100 ) {
            return reject( error, "Invalid sixel color" );
          }
          if ( params[1] == 2 && params[2] <= 100 ) {
            palette[color] = rgb(
              byte_from_percent( params[2] ), byte_from_percent( params[3] ), byte_from_percent( params[4] ) );
          } else if ( params[1] == 1 && params[2] <= 360 ) {
            palette[color] = hls( params[2], params[3], params[4] );
          } else {
            return reject( error, "Invalid sixel color space or component" );
          }
        }
      } else {
        if ( params.size() != 2 && params.size() != 4 ) {
          return reject( error, "Invalid sixel raster attributes" );
        }
        const unsigned pan = params[0] ? params[0] : 1, pad = params[1] ? params[1] : 1;
        // Integer vertical aspect ratio, rounded up like DEC graphics terminals.
        aspect = std::max( 1U, ( pan + pad - 1 ) / pad );
        if ( aspect > 32 ) {
          return reject( error, "Sixel aspect ratio limit exceeded" );
        }
        if ( params.size() == 4 ) {
          size.width = std::max( size.width, params[2] );
          size.height = std::max( size.height, params[3] );
        }
        if ( size.width > MAX_DIMENSION || size.height > MAX_DIMENSION
             || size_t( size.width ) * size.height > MAX_PIXELS ) {
          return reject( error, "Sixel raster size limit exceeded" );
        }
      }
      continue;
    }
    unsigned count = 1, mask = c;
    if ( c == '!' ) {
      const size_t number_at = at;
      if ( !parameters( payload, at, params ) || params.size() != 1 || number_at == at || params[0] == 0
           || at == payload.size() ) {
        return reject( error, "Invalid sixel repeat" );
      }
      count = params[0];
      mask = uint8_t( payload[at++] );
    }
    if ( mask < '?' || mask > '~' ) {
      return reject( error, "Invalid sixel data byte" );
    }
    mask -= '?';
    if ( count > MAX_DIMENSION - x ) {
      return reject( error, "Sixel width limit exceeded" );
    }
    work += size_t( count ) * 6 * aspect;
    if ( work > 8 * MAX_PIXELS ) {
      return reject( error, "Sixel drawing work limit exceeded" );
    }
    unsigned extent = 0;
    for ( unsigned bit = 0; bit < 6; bit++ ) {
      if ( mask & ( 1U << bit ) ) {
        extent = bit + 1;
      }
    }
    size.width = std::max( size.width, x + count );
    size.height = std::max( size.height, y + extent * aspect );
    if ( size.height > MAX_DIMENSION || size_t( size.width ) * size.height > MAX_PIXELS ) {
      return reject( error, "Sixel pixel limit exceeded" );
    }
    if ( pixels ) {
      for ( unsigned bit = 0; bit < 6; bit++ ) {
        if ( !( mask & ( 1U << bit ) ) ) {
          continue;
        }
        for ( unsigned dy = 0; dy < aspect; dy++ ) {
          for ( uint32_t dx = x; dx < x + count; dx++ ) {
            const size_t offset = ( size_t( y + bit * aspect + dy ) * pixels->width + dx ) * 4;
            pixels->rgba[offset] = char( palette[color] >> 16 );
            pixels->rgba[offset + 1] = char( palette[color] >> 8 );
            pixels->rgba[offset + 2] = char( palette[color] );
            pixels->rgba[offset + 3] = char( 255 );
          }
        }
      }
    }
    x += count;
  }
  if ( size.width && size.height == 0 ) {
    size.height = 6 * aspect;
  }
  return ( !size.width && !size.height ) || dimensions( size.width, size.height )
         || reject( error, "Empty or oversized sixel image" );
}

void repeat( std::string& output, char byte, unsigned count )
{
  if ( count > 3 ) {
    output += "!" + std::to_string( count ) + byte;
  } else {
    output.append( count, byte );
  }
}
}

Palette::Palette() : colors( initial_palette() ) {}

bool decode( const std::string& dcs, Bitmap& output, std::string& error, uint32_t background_rgb, Palette* shared_palette )
{
  output = Bitmap();
  error.clear();
  if ( dcs.size() > MAX_ENCODED_BYTES ) {
    return reject( error, "Sixel encoded size limit exceeded" );
  }
  size_t start = dcs.compare( 0, 2, "\033P" ) == 0 ? 2 : !dcs.empty() && uint8_t( dcs[0] ) == 0x90 ? 1 : 0;
  const size_t end = dcs.size() >= 2 && dcs.compare( dcs.size() - 2, 2, "\033\\" ) == 0 ? 2
                     : !dcs.empty() && uint8_t( dcs.back() ) == 0x9c                    ? 1
                                                                                        : 0;
  if ( !start || !end || dcs.size() <= start + end ) {
    return reject( error, "Incomplete sixel DCS" );
  }
  std::vector<unsigned> params;
  if ( !parameters( dcs, start, params ) || params.size() > 3 || start >= dcs.size() - end
       || dcs[start++] != 'q' ) {
    return reject( error, "Not a sixel DCS" );
  }
  const bool transparent = params.size() > 1 && params[1] == 1;
  if ( params.size() > 1 && params[1] > 2 ) {
    return reject( error, "Invalid sixel background option" );
  }
  const unsigned aspect = initial_aspect( params[0] );
  const std::string payload = dcs.substr( start, dcs.size() - end - start );
  Layout layout;
  auto palette = shared_palette ? shared_palette->colors : initial_palette();
  auto checked_palette = palette;
  if ( !walk( payload, aspect, layout, nullptr, error, checked_palette ) ) {
    return false;
  }
  Bitmap decoded;
  decoded.width = layout.width;
  decoded.height = layout.height;
  decoded.rgba.assign( size_t( layout.width ) * layout.height * 4, '\0' );
  if ( !transparent ) {
    for ( size_t i = 0; i < decoded.rgba.size(); i += 4 ) {
      decoded.rgba[i] = char( background_rgb >> 16 );
      decoded.rgba[i + 1] = char( background_rgb >> 8 );
      decoded.rgba[i + 2] = char( background_rgb );
      decoded.rgba[i + 3] = char( 255 );
    }
  }
  Layout checked;
  if ( !walk( payload, aspect, checked, &decoded, error, palette ) ) {
    return false;
  }
  output = std::move( decoded );
  if ( shared_palette ) { shared_palette->colors = palette; }
  return true;
}

bool encode( const Bitmap& input, std::string& dcs, std::string& error, unsigned offset_x, unsigned offset_y )
{
  dcs.clear();
  error.clear();
  if ( offset_x > 65535 || offset_y > 65535 ) { return reject( error, "Sixel output origin limit exceeded" ); }
  if ( !dimensions( input.width, input.height ) || input.rgba.size() != size_t( input.width ) * input.height * 4 ) {
    return reject( error, "Invalid sixel bitmap" );
  }
  std::map<uint32_t, unsigned> palette;
  std::vector<uint32_t> colors;
  const uint16_t transparent = MAX_COLORS;
  std::vector<uint16_t> indices( size_t( input.width ) * input.height, transparent );
  for ( size_t i = 0; i < indices.size(); i++ ) {
    const auto* pixel = reinterpret_cast<const unsigned char*>( input.rgba.data() + i * 4 );
    if ( !pixel[3] ) {
      continue;
    }
    if ( pixel[3] != 255 ) {
      return reject( error, "Sixel requires binary transparency" );
    }
    // Sixel RGB definitions use percentages, not 8-bit channel values.
    const uint32_t color
      = rgb( ( pixel[0] * 100 + 127 ) / 255, ( pixel[1] * 100 + 127 ) / 255, ( pixel[2] * 100 + 127 ) / 255 );
    auto found = palette.find( color );
    if ( found == palette.end() ) {
      if ( colors.size() == MAX_COLORS ) {
        return reject( error, "Sixel output palette limit exceeded" );
      }
      found = palette.emplace( color, colors.size() ).first;
      colors.push_back( color );
    }
    indices[i] = found->second;
  }
  std::string output = "\033P0;1;0q\"1;1;" + std::to_string( input.width + offset_x ) + ";"
                       + std::to_string( input.height + offset_y );
  for ( size_t i = 0; i < colors.size(); i++ ) {
    output += "#" + std::to_string( i ) + ";2;" + std::to_string( colors[i] >> 16 ) + ";"
              + std::to_string( ( colors[i] >> 8 ) & 255 ) + ";" + std::to_string( colors[i] & 255 );
  }
  output.append( offset_y / 6, '-' );
  const unsigned lead = offset_y % 6;
  for ( uint32_t y = 0; y < input.height + lead; y += 6 ) {
    // Sparse color runs per band: O(pixels log(width)), not
    // O(pixels * palette size) on high-color images.
    std::map<unsigned, std::map<uint32_t, unsigned>> band;
    for ( uint32_t dy = 0; dy < 6 && y + dy < input.height + lead; dy++ ) {
      if ( y + dy < lead ) { continue; }
      for ( uint32_t x = 0; x < input.width; x++ ) {
        const unsigned color = indices[size_t( y + dy - lead ) * input.width + x];
        if ( color != transparent ) {
          band[color][x] |= 1U << dy;
        }
      }
    }
    for ( const auto& color : band ) {
      output += "#" + std::to_string( color.first );
      if ( offset_x ) { repeat( output, '?', offset_x ); }
      uint32_t position = 0;
      auto pixel = color.second.begin();
      while ( pixel != color.second.end() ) {
        if ( pixel->first > position ) {
          repeat( output, '?', pixel->first - position );
        }
        const unsigned mask = pixel->second;
        position = pixel->first + 1;
        unsigned count = 1;
        while ( ++pixel != color.second.end() && pixel->first == position && pixel->second == mask ) {
          position++;
          count++;
        }
        repeat( output, char( '?' + mask ), count );
      }
      output += '$';
      if ( output.size() > MAX_ENCODED_BYTES - 2 ) {
        return reject( error, "Sixel output size limit exceeded" );
      }
    }
    if ( y + 6 < input.height + lead ) {
      output += '-';
    }
  }
  output += "\033\\";
  if ( output.size() > MAX_ENCODED_BYTES ) {
    return reject( error, "Sixel output size limit exceeded" );
  }
  dcs.swap( output );
  return true;
}

Renderer select_renderer( bool local_sixel, bool local_kitty )
{
  return local_sixel ? Renderer::Sixel : local_kitty ? Renderer::Kitty : Renderer::None;
}

bool normalize( const std::string& dcs, KittyImage& image, std::string& error, uint32_t background_rgb )
{
  image = KittyImage();
  Bitmap bitmap;
  if ( !decode( dcs, bitmap, error, background_rgb ) ) {
    return false;
  }
  if ( !kitty_normalize_webp( KITTY_FORMAT_RGBA,
                              bitmap.width,
                              bitmap.height,
                              bitmap.rgba,
                              *image.data,
                              image.width,
                              image.height,
                              error ) ) {
    return false;
  }
  image.origin = ImageOrigin::Sixel;
  return true;
}

bool render( const KittyImage& image, bool local_sixel, bool local_kitty, std::string& bytes, std::string& error )
{
  bytes.clear();
  error.clear();
  if ( image.origin != ImageOrigin::Sixel || !image.data ) {
    return reject( error, "Not a synchronized sixel image" );
  }
  const Renderer renderer = select_renderer( local_sixel, local_kitty );
  if ( renderer == Renderer::None ) {
    return reject( error, "No supported local graphics protocol" );
  }
  Bitmap bitmap;
  if ( !kitty_image_to_rgba( image, bitmap.rgba ) ) {
    return reject( error, "Invalid sixel image state" );
  }
  bitmap.width = image.width;
  bitmap.height = image.height;
  if ( renderer == Renderer::Sixel ) {
    return encode( bitmap, bytes, error );
  }
  bytes = encode_kitty_chunks( "a=T,t=d,f=32,q=2,C=1,i=" + std::to_string( image.id )
                                 + ",s=" + std::to_string( image.width ) + ",v=" + std::to_string( image.height ),
                               bitmap.rgba );
  return true;
}
}
}
