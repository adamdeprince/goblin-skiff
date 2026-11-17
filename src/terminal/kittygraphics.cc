/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/include/config.h"

#include "src/terminal/kittygraphics.h"
#include "src/terminal/colorspace.h"
#include "src/terminal/djvu.h"
#include "src/terminal/osc52.h"
#include "src/terminal/packedimage.h"
#include "src/terminal/terminalframebuffer.h"

#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/mman.h>

#include <csetjmp>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <zlib.h>

namespace Terminal {

KittyCommand::KittyCommand()
  : action( KittyTransmit ), medium( 'd' ), compression( 0 ), format( KITTY_FORMAT_RGBA ), width( 0 ), height( 0 ),
    data_size( 0 ), data_offset( 0 ), image_id( 0 ), image_number( 0 ), placement_id( 0 ), more( 0 ), quiet( 0 ),
    columns( 0 ), rows( 0 ), src_x( 0 ), src_y( 0 ), src_w( 0 ), src_h( 0 ), cell_x( 0 ), cell_y( 0 ), z( 0 ),
    cursor_hold( 0 ), unicode_placeholder( 0 ), parent_image( 0 ), parent_placement( 0 ), H( 0 ), V( 0 ),
    gamut( 0 ), tone_map( 0 ), flags( 0 ), delete_target( 'a' ), payload()
{}

KittyImage::KittyImage()
  : id( 0 ), number( 0 ), format( KITTY_FORMAT_WEBP ), width( 0 ), height( 0 ), origin( ImageOrigin::Kitty ),
    data( std::make_shared<std::string>() ), palette(), serial( 0 )
{}

bool KittyImage::operator==( const KittyImage& other ) const
{
  return ( id == other.id ) && ( number == other.number ) && ( format == other.format ) && ( width == other.width )
         && ( height == other.height ) && ( origin == other.origin ) && ( serial == other.serial )
         && ( palette == other.palette ) && ( gamut == other.gamut ) && ( tone_map == other.tone_map )
         && ( png_original == other.png_original ) && ( native == other.native ) && ( transfer == other.transfer )
         && ( vector == other.vector
              || ( vector && other.vector && vector->lineage == other.vector->lineage
                   && vector->version == other.vector->version ) )
         && ( ( data == other.data ) || ( data && other.data && *data == *other.data ) );
}

KittyPlacement::KittyPlacement()
  : image_id( 0 ), placement_id( 0 ), row( 0 ), col( 0 ), columns( 0 ), rows( 0 ), src_x( 0 ), src_y( 0 ),
    src_w( 0 ), src_h( 0 ), cell_x( 0 ), cell_y( 0 ), z( 0 ), cursor_hold( false ), unicode_placeholder( false ),
    parent_image( 0 ), parent_placement( 0 ), H( 0 ), V( 0 ), flags( 0 )
{}

bool KittyPlacement::operator==( const KittyPlacement& other ) const
{
  return ( image_id == other.image_id ) && ( placement_id == other.placement_id ) && ( row == other.row )
         && ( col == other.col ) && ( columns == other.columns ) && ( rows == other.rows )
         && ( src_x == other.src_x ) && ( src_y == other.src_y ) && ( src_w == other.src_w )
         && ( src_h == other.src_h ) && ( cell_x == other.cell_x ) && ( cell_y == other.cell_y ) && ( z == other.z )
         && ( cursor_hold == other.cursor_hold ) && ( unicode_placeholder == other.unicode_placeholder )
         && ( parent_image == other.parent_image ) && ( parent_placement == other.parent_placement )
         && ( H == other.H ) && ( V == other.V ) && ( flags == other.flags ) && ( depth == other.depth )
         && ( depth_pixels == other.depth_pixels ) && ( phase == other.phase );
}

static bool parse_u32( const std::string& s, uint32_t& out )
{
  if ( s.empty() || s.find_first_not_of( "0123456789" ) != std::string::npos ) {
    return false;
  }
  char* end = NULL;
  errno = 0;
  const unsigned long val = strtoul( s.c_str(), &end, 10 );
  if ( errno != 0 || end == s.c_str() || *end != '\0' || val > 0xffffffffUL ) {
    return false;
  }
  out = static_cast<uint32_t>( val );
  return true;
}

static bool parse_i32( const std::string& s, int32_t& out )
{
  if ( s.empty() ) {
    return false;
  }
  char* end = NULL;
  errno = 0;
  const long val = strtol( s.c_str(), &end, 10 );
  if ( errno != 0 || end == s.c_str() || *end != '\0' || val < -2147483647L - 1 || val > 2147483647L ) {
    return false;
  }
  out = static_cast<int32_t>( val );
  return true;
}

static bool parse_u64( const std::string& s, uint64_t& out )
{
  if ( s.empty() ) { return false; }
  uint64_t value = 0;
  for ( char c : s ) {
    if ( c < '0' || c > '9' || value > ( UINT64_MAX - ( c - '0' ) ) / 10 ) { return false; }
    value = value * 10 + ( c - '0' );
  }
  out = value;
  return true;
}

bool parse_kitty_command( const std::string& apc, KittyCommand& cmd )
{
  cmd = KittyCommand();
  if ( apc.empty() || apc[0] != 'G' ) {
    return false;
  }

  const std::string body = apc.substr( 1 );
  const size_t semi = body.find( ';' );
  const std::string controls = semi == std::string::npos ? body : body.substr( 0, semi );
  const std::string b64 = semi == std::string::npos ? std::string() : body.substr( semi + 1 );

  size_t start = 0;
  while ( start < controls.size() ) {
    size_t comma = controls.find( ',', start );
    if ( comma == std::string::npos ) {
      comma = controls.size();
    }
    const std::string pair = controls.substr( start, comma - start );
    const size_t eq = pair.find( '=' );
    if ( eq != std::string::npos && eq > 0 ) {
      const std::string key = pair.substr( 0, eq );
      const std::string val = pair.substr( eq + 1 );
      if ( key == "a" && val.size() == 1 ) {
        cmd.action = val[0];
      } else if ( key == "t" && val.size() == 1 ) {
        cmd.medium = val[0];
      } else if ( key == "o" && val.size() == 1 ) {
        cmd.compression = val[0];
      } else if ( key == "f" ) {
        if ( !parse_u32( val, cmd.format ) ) { return false; }
      } else if ( key == "s" ) {
        if ( !parse_u32( val, cmd.width ) ) { return false; }
      } else if ( key == "v" ) {
        if ( !parse_u32( val, cmd.height ) ) { return false; }
      } else if ( key == "S" ) {
        if ( !parse_u64( val, cmd.data_size ) ) { return false; }
      } else if ( key == "O" ) {
        if ( !parse_u64( val, cmd.data_offset ) ) { return false; }
      } else if ( key == "e" && val.size() == 1 ) {
        cmd.feature = val[0];
      } else if ( key == "E" ) {
        if ( !parse_u32( val, cmd.transfer ) ) { return false; }
      } else if ( key == "i" ) {
        if ( !parse_u32( val, cmd.image_id ) ) { return false; }
      } else if ( key == "I" ) {
        if ( !parse_u32( val, cmd.image_number ) ) { return false; }
      } else if ( key == "p" ) {
        if ( !parse_u32( val, cmd.placement_id ) ) { return false; }
      } else if ( key == "m" ) {
        if ( !parse_u32( val, cmd.more ) ) { return false; }
      } else if ( key == "q" ) {
        if ( !parse_u32( val, cmd.quiet ) ) { return false; }
      } else if ( key == "c" ) {
        if ( !parse_u32( val, cmd.columns ) ) { return false; }
      } else if ( key == "r" ) {
        if ( !parse_u32( val, cmd.rows ) ) { return false; }
      } else if ( key == "x" ) {
        if ( !parse_u32( val, cmd.src_x ) ) { return false; }
      } else if ( key == "y" ) {
        if ( !parse_u32( val, cmd.src_y ) ) { return false; }
      } else if ( key == "w" ) {
        if ( !parse_u32( val, cmd.src_w ) ) { return false; }
      } else if ( key == "h" ) {
        if ( !parse_u32( val, cmd.src_h ) ) { return false; }
      } else if ( key == "X" ) {
        if ( !parse_u32( val, cmd.cell_x ) ) { return false; }
      } else if ( key == "Y" ) {
        if ( !parse_u32( val, cmd.cell_y ) ) { return false; }
      } else if ( key == "z" ) {
        parse_i32( val, cmd.z );
      } else if ( key == "C" ) {
        if ( !parse_u32( val, cmd.cursor_hold ) ) { return false; }
      } else if ( key == "U" ) {
        if ( !parse_u32( val, cmd.unicode_placeholder ) ) { return false; }
      } else if ( key == "P" ) {
        if ( !parse_u32( val, cmd.parent_image ) ) { return false; }
      } else if ( key == "Q" ) {
        if ( !parse_u32( val, cmd.parent_placement ) ) { return false; }
      } else if ( key == "H" ) {
        parse_i32( val, cmd.H );
      } else if ( key == "V" ) {
        parse_i32( val, cmd.V );
      } else if ( key == "G" ) {
        if ( !parse_u32( val, cmd.gamut ) ) { return false; }
        cmd.gamut_set = true;
      } else if ( key == "T" ) {
        if ( !parse_u32( val, cmd.tone_map ) ) { return false; }
      } else if ( key == "F" ) {
        if ( !parse_u32( val, cmd.flags ) ) { return false; }
      } else if ( key == "d" && val.size() == 1 ) {
        cmd.delete_target = val[0];
      }
    }
    start = comma + 1;
  }

  if ( !cmd.feature && !b64.empty() && !base64_decode_osc52( b64, cmd.payload ) ) {
    return false;
  }
  return true;
}

std::string encode_kitty_chunks( const std::string& controls, const std::string& binary )
{
  const std::string encoded = base64_encode_osc52( binary );
  if ( encoded.empty() ) {
    return std::string( "\033_G" ) + controls + "\033\\";
  }

  std::string out;
  bool first = true;
  for ( size_t off = 0; off < encoded.size(); ) {
    const size_t n = encoded.size() - off > KITTY_CHUNK_B64 ? KITTY_CHUNK_B64 : encoded.size() - off;
    const bool last = off + n >= encoded.size();
    out.append( "\033_G" );
    if ( first ) {
      out.append( controls );
      if ( !controls.empty() && controls[controls.size() - 1] != ',' ) {
        out.push_back( ',' );
      }
      first = false;
    }
    out.push_back( 'm' );
    out.push_back( '=' );
    out.push_back( last ? '0' : '1' );
    if ( !first || controls.find( "q=" ) == std::string::npos ) {
      out.append( ",q=2" );
    }
    out.push_back( ';' );
    out.append( encoded, off, n );
    out.append( "\033\\" );
    off += n;
  }
  return out;
}

std::string encode_vector_service( uint32_t request_id, const std::string& json, uint32_t quiet )
{
  /* Unlike every other command sent to a terminal this one is answered, so
     it is not quiet, but for what the client asks for itself. The later
     chunks take after the first. */
  const std::string controls
    = "a=v,f=200,i=" + std::to_string( request_id ) + ( quiet ? ",q=" + std::to_string( quiet ) : std::string() );
  const std::string encoded = base64_encode_osc52( json );
  if ( encoded.empty() ) {
    return "\033_G" + controls + "\033\\";
  }
  std::string out;
  for ( size_t off = 0; off < encoded.size(); off += KITTY_CHUNK_B64 ) {
    const size_t n = std::min( KITTY_CHUNK_B64, encoded.size() - off );
    out += "\033_G" + ( off ? std::string() : controls + "," ) + "m=" + ( off + n < encoded.size() ? "1" : "0" ) + ";";
    out.append( encoded, off, n );
    out += "\033\\";
  }
  return out;
}

std::string kitty_response( uint32_t image_id, uint32_t placement_id, const std::string& message, uint32_t frame )
{
  std::ostringstream out;
  out << "\033_Gi=" << image_id;
  if ( placement_id != 0 ) {
    out << ",p=" << placement_id;
  }
  if ( frame != 0 ) {
    out << ",r=" << frame;
  }
  out << ';' << message << "\033\\";
  return out.str();
}

static bool path_is_sensitive( const std::string& path )
{
  return path.compare( 0, 6, "/proc/" ) == 0 || path == "/proc" || path.compare( 0, 5, "/sys/" ) == 0
         || path == "/sys" || ( path.compare( 0, 5, "/dev/" ) == 0 && path.compare( 0, 9, "/dev/shm/" ) ) || path == "/dev";
}

static std::string canonical_path( const std::string& path )
{
  if ( path.empty() || path.find( '\0' ) != std::string::npos ) { return {}; }
  char* resolved = realpath( path.c_str(), NULL );
  if ( !resolved ) { return {}; }
  const std::string result( resolved ); free( resolved ); return result;
}

static bool graphics_temporary_path( const std::string& path )
{
  if ( path.find( "tty-graphics-protocol" ) == std::string::npos ) { return false; }
  const char* roots[] = { "/tmp", "/var/tmp", "/dev/shm", getenv( "TMPDIR" ) };
  for ( const char* root : roots ) {
    if ( !root ) { continue; }
    const auto resolved = canonical_path( root );
    if ( !resolved.empty() && resolved != "/" && path.compare( 0, resolved.size() + 1, resolved + "/" ) == 0 ) { return true; }
  }
  return false;
}

static bool read_regular_file( const std::string& path, uint64_t offset, uint64_t size, std::string& data, bool temporary )
{
  // Check the opened descriptor, not a stat-then-reopen race. O_NONBLOCK
  // prevents a swapped FIFO/device from blocking the terminal connection.
  const int fd = open( path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC );
  if ( fd < 0 ) { return false; }
  struct stat st;
  if ( fstat( fd, &st ) < 0 || !S_ISREG( st.st_mode ) || st.st_size < 0 ) {
    close( fd ); return false;
  }
  const uint64_t total = static_cast<uint64_t>( st.st_size );
  const uint64_t off = offset;
  if ( off > total ) {
    close( fd ); return false;
  }
  const uint64_t available = total - off;
  const uint64_t wanted = size == 0 ? available : size;
  if ( wanted > available || wanted > data.max_size() ) { close( fd ); return false; }
  data.assign( static_cast<size_t>( wanted ), '\0' );
  size_t at = 0;
  while ( at < wanted ) {
    const ssize_t n = pread( fd, &data[at], std::min<uint64_t>( wanted - at, SSIZE_MAX ), off + at );
    if ( n < 0 && errno == EINTR ) { continue; }
    if ( n <= 0 ) { data.clear(); close( fd ); return false; }
    at += n;
  }
  if ( temporary && graphics_temporary_path( path ) ) {
    struct stat current;
    if ( lstat( path.c_str(), &current ) == 0 && current.st_dev == st.st_dev && current.st_ino == st.st_ino ) { unlink( path.c_str() ); }
  }
  close( fd );
  return true;
}

bool kitty_read_medium( const KittyCommand& cmd, std::string& data, std::string& error )
{
  data.clear();
  if ( cmd.medium == 'd' || cmd.medium == 0 ) {
    data = cmd.payload;
    return true;
  }

  if ( cmd.data_offset > uint64_t( std::numeric_limits<off_t>::max() )
       || cmd.data_size > uint64_t( std::numeric_limits<off_t>::max() ) - cmd.data_offset ) {
    error = "EOVERFLOW:file range does not fit in an offset";
    return false;
  }

  if ( cmd.medium == 'f' || cmd.medium == 't' ) {
    const std::string path = canonical_path( cmd.payload );
    if ( path.empty() || path_is_sensitive( path ) ) {
      error = "EINVAL:refused to read path";
      return false;
    }
    if ( !read_regular_file( path, cmd.data_offset, cmd.data_size, data, cmd.medium == 't' ) ) {
      error = "ENOENT:could not read file";
      return false;
    }
    return true;
  }

  if ( cmd.medium == 's' ) {
#if defined( __unix__ ) || defined( __APPLE__ )
    const std::string name = cmd.payload;
    if ( name.empty() || name.find( '\0' ) != std::string::npos ) {
      error = "EINVAL:empty shm name";
      return false;
    }
    int fd = shm_open( name.c_str(), O_RDONLY, 0 );
    if ( fd < 0 ) {
      error = "ENOENT:shm_open failed";
      return false;
    }
    struct stat st;
    if ( fstat( fd, &st ) < 0 || st.st_size < 0 ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "EINVAL:shm fstat failed";
      return false;
    }
    const uint64_t total64 = static_cast<uint64_t>( st.st_size );
    const uint64_t off64 = cmd.data_offset;
    if ( total64 > SIZE_MAX || off64 > total64 ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "EINVAL:shm offset";
      return false;
    }
    const size_t total = static_cast<size_t>( total64 );
    const size_t off = static_cast<size_t>( off64 );
    const size_t available = total - off;
    const size_t want = cmd.data_size == 0 ? available : cmd.data_size;
    if ( want > available || want > data.max_size() ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "EINVAL:shared memory image range";
      return false;
    }
    if ( want == 0 ) {
      close( fd );
      shm_unlink( name.c_str() );
      return true;
    }
    const size_t page = static_cast<size_t>( getpagesize() );
    const size_t base = off - off % page, skip = off - base;
    const size_t mapped = skip + want;
    void* map = mmap( NULL, mapped, PROT_READ, MAP_SHARED, fd, base );
    close( fd );
    if ( map == MAP_FAILED ) {
      shm_unlink( name.c_str() );
      error = "EINVAL:mmap failed";
      return false;
    }
    data.assign( static_cast<char*>( map ) + skip, want );
    munmap( map, mapped );
    shm_unlink( name.c_str() );
    return true;
#else
    error = "EINVAL:shared memory not supported";
    return false;
#endif
  }

  error = "EINVAL:unknown transmission medium";
  return false;
}

bool graphics_pixel_size( uint32_t width, uint32_t height, size_t bytes_per_pixel, size_t& size )
{
  if ( width == 0 || height == 0 || bytes_per_pixel == 0 ) {
    return false;
  }
  const size_t capacity = std::string().max_size();
  if ( width > capacity / bytes_per_pixel
       || height > capacity / ( static_cast<size_t>( width ) * bytes_per_pixel ) ) {
    return false;
  }
  size = static_cast<size_t>( width ) * static_cast<size_t>( height ) * bytes_per_pixel;
  return true;
}

bool kitty_webp_dimensions( const std::string& webp, uint32_t& width, uint32_t& height )
{
  int decoded_width = 0, decoded_height = 0;
  size_t rgba_size = 0;
  if ( webp.empty()
       || !WebPGetInfo(
         reinterpret_cast<const uint8_t*>( webp.data() ), webp.size(), &decoded_width, &decoded_height )
       || decoded_width <= 0 || decoded_height <= 0
       || !graphics_pixel_size(
         static_cast<uint32_t>( decoded_width ), static_cast<uint32_t>( decoded_height ), 4, rgba_size ) ) {
    return false;
  }
  width = static_cast<uint32_t>( decoded_width );
  height = static_cast<uint32_t>( decoded_height );
  return true;
}

static bool has_small_palette( const unsigned char* pixels, size_t count, bool alpha )
{
  std::array<uint32_t, GRAPHICS_MAX_PALETTE> colors;
  size_t used = 0;
  for ( size_t i = 0; i < count; i++ ) {
    const unsigned char* p = pixels + i * ( alpha ? 4 : 3 );
    const uint32_t color = uint32_t( p[0] ) << 24 | uint32_t( p[1] ) << 16 | uint32_t( p[2] ) << 8
                           | ( alpha ? p[3] : 255 );
    size_t index = 0;
    while ( index < used && colors[index] != color ) { index++; }
    if ( index == used ) {
      if ( used == colors.size() ) { return false; }
      colors[used++] = color;
    }
  }
  return true;
}

static bool encode_image( const unsigned char* pixels,
                          uint32_t width,
                          uint32_t height,
                          bool alpha,
                          KittyImage& image,
                          const ImageEncoding& encoding,
                          std::string& error )
{
  if ( encoding.quality < -1 || encoding.quality > 100 ) {
    error = "EINVAL:WebP quality must be between 0 and 100";
    return false;
  }
  image.width = width;
  image.height = height;
  image.palette.clear();
  image.data = std::make_shared<std::string>();
  // A cell-tiled viewer sends thousands of tiny images. Forking cjb2 for
  // each one (and starting decoder threads twice at the client) can stall
  // the terminal long enough to lose network acknowledgements. WebP runs
  // in-process and has less overhead for these images. This selects a
  // codec; it does not restrict image dimensions or the number of images.
  const bool small_image = uint64_t( width ) * height < 1024;
  if ( encoding.palette_djvu && !small_image
       && ( encoding.png || ( width <= WEBP_MAX_DIMENSION && height <= WEBP_MAX_DIMENSION ) )
       && encode_palette_djvu( pixels, width, height, alpha, encoding.djvu_lossy, image.palette, *image.data ) ) {
    image.format = KITTY_FORMAT_DJVU;
    return true;
  }
  // Palette images remain exact even when WebP lossiness is requested,
  // whether cjb2 failed or we deliberately bypassed it for a small image.
  const bool exact_palette = !image.palette.empty()
                             || ( encoding.palette_djvu && small_image
                                  && has_small_palette( pixels, size_t( width ) * height, alpha ) );
  const bool lossy = encoding.quality >= 0 && !exact_palette;
  image.palette.clear();
  image.format = KITTY_FORMAT_WEBP;
  if ( width > WEBP_MAX_DIMENSION || height > WEBP_MAX_DIMENSION ) {
    if ( !encoding.png ) {
      error = "EINVAL:image needs peer capability png-state-v1";
      return false;
    }
    // PNG has no WebP 16383-pixel dimension ceiling. Keep exact pixels when
    // the image cannot be represented by either of the preferred codecs.
    png_image png;
    memset( &png, 0, sizeof( png ) );
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;
    png_alloc_size_t size = 0;
    if ( !png_image_write_to_memory( &png, NULL, &size, 0, pixels, 0, NULL )
         || size > image.data->max_size() ) {
      png_image_free( &png );
      error = "EINVAL:PNG encode failed";
      return false;
    }
    image.data->resize( size );
    const bool ok = png_image_write_to_memory( &png, &(*image.data)[0], &size, 0, pixels, 0, NULL );
    png_image_free( &png );
    if ( !ok ) { error = "EINVAL:PNG encode failed"; return false; }
    image.data->resize( size );
    image.format = KITTY_FORMAT_PNG;
    return true;
  }
  const int stride = static_cast<int>( width * ( alpha ? 4 : 3 ) );
  if ( !lossy ) {
    WebPConfig config;
    WebPPicture picture;
    if ( !WebPConfigInit( &config ) || !WebPPictureInit( &picture ) ) {
      error = "EINVAL:WebP initialization failed";
      return false;
    }
    config.lossless = 1;
    config.quality = 75;
    // The simple lossless API may discard RGB beneath transparent pixels.
    // Preserve the same exact RGBA semantics as palette/DjVu.
    config.exact = 1;
    picture.use_argb = 1;
    picture.width = width;
    picture.height = height;
    WebPMemoryWriter writer;
    WebPMemoryWriterInit( &writer );
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    const bool imported = alpha ? WebPPictureImportRGBA( &picture, pixels, stride )
                                : WebPPictureImportRGB( &picture, pixels, stride );
    const bool ok = imported && WebPEncode( &config, &picture );
    if ( ok ) { image.data->assign( reinterpret_cast<const char*>( writer.mem ), writer.size ); }
    WebPPictureFree( &picture );
    WebPMemoryWriterClear( &writer );
    if ( !ok ) { error = "EINVAL:WebP encode failed"; }
    return ok;
  }
  uint8_t* encoded = NULL;
  const size_t encoded_size = alpha ? WebPEncodeRGBA( pixels, width, height, stride, encoding.quality, &encoded )
                                    : WebPEncodeRGB( pixels, width, height, stride, encoding.quality, &encoded );
  if ( encoded_size == 0 || encoded == NULL ) {
    WebPFree( encoded );
    error = "EINVAL:WebP encode failed";
    return false;
  }
  image.data->assign( reinterpret_cast<const char*>( encoded ), encoded_size );
  WebPFree( encoded );
  return true;
}

bool kitty_normalize_webp( uint32_t format,
                           uint32_t width,
                           uint32_t height,
                           const std::string& input,
                           std::string& webp,
                           uint32_t& output_width,
                           uint32_t& output_height,
                           std::string& error )
{
  webp.clear();
  KittyImage image;
  if ( !kitty_normalize_image( format, width, height, input, image, ImageEncoding(), error ) ) { return false; }
  webp = *image.data;
  output_width = image.width;
  output_height = image.height;
  return true;
}

/* Whether a PNG carries an ICC profile or a cICP chunk: colors it says
   are its own, which an RGBA copy would lose. */
static bool png_has_color_profile( const std::string& png )
{
  static const char signature[] = "\x89PNG\r\n\x1a\n";
  if ( png.size() < 8 || png.compare( 0, 8, signature, 8 ) != 0 ) {
    return false;
  }
  for ( size_t at = 8; at + 8 <= png.size(); ) {
    const uint32_t length = uint32_t( uint8_t( png[at] ) ) << 24 | uint32_t( uint8_t( png[at + 1] ) ) << 16
                            | uint32_t( uint8_t( png[at + 2] ) ) << 8 | uint8_t( png[at + 3] );
    const std::string type = png.substr( at + 4, 4 );
    if ( type == "iCCP" || type == "cICP" ) {
      return true;
    }
    if ( type == "IDAT" || type == "IEND" || length > png.size() - at - 8 ) {
      return false; /* both come before the image data */
    }
    at += size_t( length ) + 12;
  }
  return false;
}

/* The colors of a PNG, as Silkie's png-reader.c reads them, with the ICC
   profiles it meets in practice (matrix and curves) read here instead of by
   lcms. See Silkie's docs/wide-gamut-protocol.rst. */
namespace {
uint32_t be32( const std::string& data, size_t at )
{
  return uint32_t( uint8_t( data[at] ) ) << 24 | uint32_t( uint8_t( data[at + 1] ) ) << 16
         | uint32_t( uint8_t( data[at + 2] ) ) << 8 | uint8_t( data[at + 3] );
}

double s15fixed16( const std::string& data, size_t at )
{
  return double( int32_t( be32( data, at ) ) ) / 65536.0;
}

/* A tone curve of an ICC profile: from the encoded value to linear light. */
struct IccCurve
{
  int kind = -1; /* -1 identity, 0..4 parametric of that type, 5 table */
  double g = 1, a = 1, b = 0, c = 0, d = 0, e = 0, f = 0;
  std::vector<uint16_t> table;

  double operator()( double x ) const
  {
    switch ( kind ) {
      case 0:
        return std::pow( x, g );
      case 1:
        return x >= -b / a ? std::pow( std::max( 0.0, a * x + b ), g ) : 0;
      case 2:
        return x >= -b / a ? std::pow( std::max( 0.0, a * x + b ), g ) + c : c;
      case 3:
        return x >= d ? std::pow( std::max( 0.0, a * x + b ), g ) : c * x;
      case 4:
        return x >= d ? std::pow( std::max( 0.0, a * x + b ), g ) + e : c * x + f;
      case 5: {
        const double at = x * double( table.size() - 1 );
        const size_t i = size_t( std::floor( at ) );
        if ( i + 1 >= table.size() ) {
          return table.back() / 65535.0;
        }
        return ( table[i] + ( table[i + 1] - table[i] ) * ( at - double( i ) ) ) / 65535.0;
      }
      default:
        return x;
    }
  }
};

/* An RGB profile of a matrix and three curves, the kind displays and images
   carry: the matrix takes linear RGB to XYZ, adapted to D50. */
struct IccProfile
{
  double matrix[3][3] = {};
  IccCurve curves[3];
};

bool icc_tag( const std::string& icc, const char* signature, size_t& at, size_t& size )
{
  if ( icc.size() < 132 ) {
    return false;
  }
  const uint64_t count = be32( icc, 128 );
  for ( uint64_t i = 0; i < count && 132 + ( i + 1 ) * 12 <= icc.size(); i++ ) {
    const size_t entry = size_t( 132 + i * 12 );
    if ( icc.compare( entry, 4, signature, 4 ) != 0 ) {
      continue;
    }
    const uint64_t offset = be32( icc, entry + 4 ), length = be32( icc, entry + 8 );
    if ( offset + length > icc.size() || length < 12 ) {
      return false;
    }
    at = size_t( offset );
    size = size_t( length );
    return true;
  }
  return false;
}

bool icc_curve( const std::string& icc, const char* signature, IccCurve& curve )
{
  size_t at = 0, size = 0;
  if ( !icc_tag( icc, signature, at, size ) ) {
    return false;
  }
  if ( icc.compare( at, 4, "curv" ) == 0 ) {
    const uint64_t count = be32( icc, at + 8 );
    if ( 12 + count * 2 > size ) {
      return false;
    }
    if ( count == 0 ) {
      curve.kind = -1;
    } else if ( count == 1 ) {
      curve.kind = 0;
      curve.g = ( uint8_t( icc[at + 12] ) * 256 + uint8_t( icc[at + 13] ) ) / 256.0;
    } else {
      curve.kind = 5;
      curve.table.resize( size_t( count ) );
      for ( size_t i = 0; i < count; i++ ) {
        curve.table[i] = uint16_t( uint8_t( icc[at + 12 + i * 2] ) << 8 | uint8_t( icc[at + 13 + i * 2] ) );
      }
    }
    return true;
  }
  if ( icc.compare( at, 4, "para" ) == 0 ) {
    static const size_t params[] = { 1, 3, 4, 5, 7 };
    const unsigned type = uint8_t( icc[at + 8] ) << 8 | uint8_t( icc[at + 9] );
    if ( type > 4 || 12 + params[type] * 4 > size ) {
      return false;
    }
    double v[7] = { 1, 1, 0, 0, 0, 0, 0 };
    for ( size_t i = 0; i < params[type]; i++ ) {
      v[i] = s15fixed16( icc, at + 12 + i * 4 );
    }
    curve.kind = int( type );
    curve.g = v[0];
    curve.a = v[1];
    curve.b = v[2];
    curve.c = v[3];
    curve.d = v[4];
    curve.e = v[5];
    curve.f = v[6];
    if ( ( type == 1 || type == 2 ) && curve.a == 0 ) {
      return false;
    }
    return true;
  }
  return false;
}

bool parse_icc( const std::string& icc, IccProfile& profile )
{
  if ( icc.size() < 132 || icc.compare( 16, 4, "RGB " ) != 0 || icc.compare( 20, 4, "XYZ " ) != 0 ) {
    return false;
  }
  const char* columns[3] = { "rXYZ", "gXYZ", "bXYZ" };
  const char* curves[3] = { "rTRC", "gTRC", "bTRC" };
  for ( int c = 0; c < 3; c++ ) {
    size_t at = 0, size = 0;
    if ( !icc_tag( icc, columns[c], at, size ) || size < 20 || icc.compare( at, 4, "XYZ " ) != 0
         || !icc_curve( icc, curves[c], profile.curves[c] ) ) {
      return false;
    }
    for ( int r = 0; r < 3; r++ ) {
      profile.matrix[r][c] = s15fixed16( icc, at + 8 + r * 4 );
    }
  }
  return true;
}

void invert3( const double m[3][3], double out[3][3] )
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

void multiply3( const double a[3][3], const double b[3][3], double out[3][3] )
{
  for ( int r = 0; r < 3; r++ ) {
    for ( int c = 0; c < 3; c++ ) {
      out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    }
  }
}

/* The matrix of a profile in a gamut as lcms makes it: the primaries with
   the D65 white, adapted to D50 by Bradford. Gamut 0 is sRGB. */
void gamut_matrix( unsigned gamut, double out[3][3] )
{
  static const double primaries[3][6] = { { 0.64, 0.33, 0.30, 0.60, 0.15, 0.06 },
                                          { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060 },
                                          { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046 } };
  const double* p = primaries[gamut <= 2 ? gamut : 0];
  const double wx = 0.3127, wy = 0.3290;
  const double white[3] = { wx / wy, 1, ( 1 - wx - wy ) / wy };
  double m[3][3];
  for ( int c = 0; c < 3; c++ ) {
    const double x = p[c * 2], y = p[c * 2 + 1];
    m[0][c] = x / y;
    m[1][c] = 1;
    m[2][c] = ( 1 - x - y ) / y;
  }
  double inverse[3][3];
  invert3( m, inverse );
  double scale[3];
  for ( int r = 0; r < 3; r++ ) {
    scale[r] = inverse[r][0] * white[0] + inverse[r][1] * white[1] + inverse[r][2] * white[2];
  }
  for ( int r = 0; r < 3; r++ ) {
    for ( int c = 0; c < 3; c++ ) {
      m[r][c] *= scale[c];
    }
  }
  static const double bradford[3][3]
    = { { 0.8951, 0.2664, -0.1614 }, { -0.7502, 1.7135, 0.0367 }, { 0.0389, -0.0685, 1.0296 } };
  const double d50[3] = { 0.9642, 1.0, 0.8249 };
  double bradford_inverse[3][3], cone[3][3] = {}, adapt[3][3], step[3][3];
  invert3( bradford, bradford_inverse );
  for ( int r = 0; r < 3; r++ ) {
    const double to = bradford[r][0] * d50[0] + bradford[r][1] * d50[1] + bradford[r][2] * d50[2];
    const double from = bradford[r][0] * white[0] + bradford[r][1] * white[1] + bradford[r][2] * white[2];
    cone[r][r] = to / from;
  }
  multiply3( cone, bradford, step );
  multiply3( bradford_inverse, step, adapt );
  multiply3( adapt, m, out );
}

/* The wide gamut a profile is in, 0 when neither: each of its primaries
   within 0.005 in x and in y of that of the gamut, both adapted to D50. */
unsigned wide_gamut_of_profile( const IccProfile& profile )
{
  for ( unsigned gamut = 1; gamut <= 2; gamut++ ) {
    double wanted[3][3];
    gamut_matrix( gamut, wanted );
    bool same = true;
    for ( int c = 0; c < 3 && same; c++ ) {
      const double sum = profile.matrix[0][c] + profile.matrix[1][c] + profile.matrix[2][c];
      const double want = wanted[0][c] + wanted[1][c] + wanted[2][c];
      if ( !( sum > 0 ) ) {
        return 0;
      }
      same = std::fabs( profile.matrix[0][c] / sum - wanted[0][c] / want ) <= 0.005
             && std::fabs( profile.matrix[1][c] / sum - wanted[1][c] / want ) <= 0.005;
    }
    if ( same ) {
      return gamut;
    }
  }
  return 0;
}

double srgb_encode( double linear )
{
  linear = std::min( 1.0, std::max( 0.0, linear ) );
  return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow( linear, 1 / 2.4 ) - 0.055;
}

/* Pixels in a profile to the gamut whose primaries it has, with the sRGB
   curve: only the curve changes. Or, to_srgb, to sRGB through XYZ. */
void apply_profile( const IccProfile& profile, bool to_srgb, std::string& rgba )
{
  double linear[3][256];
  for ( int c = 0; c < 3; c++ ) {
    for ( int v = 0; v < 256; v++ ) {
      linear[c][v] = profile.curves[c]( v / 255.0 );
    }
  }
  double m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
  if ( to_srgb ) {
    double srgb[3][3], inverse[3][3];
    gamut_matrix( 0, srgb );
    invert3( srgb, inverse );
    multiply3( inverse, profile.matrix, m );
  }
  for ( size_t at = 0; at + 4 <= rgba.size(); at += 4 ) {
    uint8_t* p = reinterpret_cast<uint8_t*>( &rgba[at] );
    const double r = linear[0][p[0]], g = linear[1][p[1]], b = linear[2][p[2]];
    for ( int c = 0; c < 3; c++ ) {
      p[c] = uint8_t( std::lround( srgb_encode( m[c][0] * r + m[c][1] * g + m[c][2] * b ) * 255 ) );
    }
  }
}

struct PngInput
{
  const std::string* data;
  size_t at;
  jmp_buf jump;
};

void png_input_read( png_structp png, png_bytep out, png_size_t length )
{
  PngInput* input = static_cast<PngInput*>( png_get_io_ptr( png ) );
  if ( !input || length > input->data->size() - input->at ) {
    png_error( png, "PNG data is truncated" );
  }
  memcpy( out, input->data->data() + input->at, length );
  input->at += length;
}

void png_input_error( png_structp png, png_const_charp )
{
  PngInput* input = static_cast<PngInput*>( png_get_error_ptr( png ) );
  longjmp( input->jump, 1 );
}

void png_input_warning( png_structp, png_const_charp ) {}

/* The code points of ITU-T H.273 in a cICP chunk, read from the stream so
   that any libpng will do: primaries 12 are Display P3 and 9 Rec. 2020, with
   transfer 13 the sRGB curve. */
unsigned cicp_gamut( const std::string& png )
{
  for ( size_t at = 8; at + 12 <= png.size(); ) {
    const uint32_t length = be32( png, at );
    const std::string type = png.substr( at + 4, 4 );
    if ( type == "IDAT" || type == "IEND" || length > png.size() - at - 12 ) {
      return 0;
    }
    if ( type == "cICP" && length == 4 ) {
      const uint8_t primaries = png[at + 8], transfer = png[at + 9], matrix = png[at + 10], full = png[at + 11];
      if ( transfer == 13 && matrix == 0 && full == 1 ) {
        return primaries == 12 ? 1 : primaries == 9 ? 2 : 0;
      }
      return 0;
    }
    at += size_t( length ) + 12;
  }
  return 0;
}
}

bool kitty_png_to_rgba( const std::string& data, std::string& rgba, uint32_t& width, uint32_t& height, uint32_t& gamut )
{
  static const char signature[] = "\x89PNG\r\n\x1a\n";
  gamut = 0;
  if ( data.size() < 8 || data.compare( 0, 8, signature, 8 ) != 0 ) {
    return false;
  }
  PngInput input;
  input.data = &data;
  input.at = 0;
  std::string icc;
  IccProfile profile;
  png_structp png = png_create_read_struct( PNG_LIBPNG_VER_STRING, &input, png_input_error, png_input_warning );
  if ( !png ) {
    return false;
  }
  png_infop info = png_create_info_struct( png );
  std::vector<png_bytep> rows;
  /* What changes after setjmp and is read after a longjmp. */
  volatile int profile_use = 0; /* 1: wide, curve only; 2: to sRGB */
  if ( !info || setjmp( input.jump ) ) {
    png_destroy_read_struct( &png, info ? &info : nullptr, nullptr );
    rgba.clear();
    return false;
  }
  png_set_read_fn( png, &input, png_input_read );
  png_read_info( png, info );
  width = png_get_image_width( png, info );
  height = png_get_image_height( png, info );
  const png_byte color_type = png_get_color_type( png, info ), bit_depth = png_get_bit_depth( png, info );
  int intent = 0;
  const bool srgb = png_get_sRGB( png, info, &intent ) != 0;
  png_charp name = nullptr;
  int compression = 0;
  png_bytep profile_data = nullptr;
  png_uint_32 profile_length = 0;
  if ( png_get_iCCP( png, info, &name, &compression, &profile_data, &profile_length ) & PNG_INFO_iCCP ) {
    icc.assign( reinterpret_cast<const char*>( profile_data ), profile_length );
  }
  /* A file in a wide gamut stays in it, tagged with the gamut. */
  unsigned source = cicp_gamut( data );
  bool wide = source != 0;
  if ( !source && !srgb && !icc.empty() && parse_icc( icc, profile ) && ( source = wide_gamut_of_profile( profile ) ) ) {
    wide = true;
    profile_use = 1;
  }
  double file_gamma = 0;
  if ( wide ) {
    gamut = source;
  } else if ( srgb ) {
    /* sRGB, as wanted */
  } else if ( png_get_gAMA( png, info, &file_gamma ) ) {
    if ( file_gamma != 0 && std::fabs( file_gamma - 1.0 / 2.2 ) > 0.0001 ) {
      png_set_gamma( png, 2.2, file_gamma );
    }
  } else if ( !icc.empty() && parse_icc( icc, profile ) ) {
    profile_use = 2; /* any other profile is converted to sRGB */
  }
  if ( bit_depth == 16 ) {
    png_set_strip_16( png );
  }
  if ( color_type == PNG_COLOR_TYPE_PALETTE ) {
    png_set_palette_to_rgb( png );
  }
  if ( color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8 ) {
    png_set_expand_gray_1_2_4_to_8( png );
  }
  if ( png_get_valid( png, info, PNG_INFO_tRNS ) ) {
    png_set_tRNS_to_alpha( png );
  }
  if ( color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE ) {
    png_set_filler( png, 0xff, PNG_FILLER_AFTER );
  }
  if ( color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA ) {
    png_set_gray_to_rgb( png );
  }
  png_set_interlace_handling( png );
  png_read_update_info( png, info );
  const size_t row_bytes = png_get_rowbytes( png, info );
  size_t size = 0;
  if ( row_bytes != size_t( width ) * 4 || !graphics_pixel_size( width, height, 4, size ) ) {
    png_destroy_read_struct( &png, &info, nullptr );
    return false;
  }
  rgba.assign( size, '\0' );
  rows.resize( height );
  for ( size_t y = 0; y < height; y++ ) {
    rows[y] = reinterpret_cast<png_bytep>( &rgba[y * row_bytes] );
  }
  png_read_image( png, rows.data() );
  png_destroy_read_struct( &png, &info, nullptr );
  if ( profile_use ) {
    apply_profile( profile, profile_use == 2, rgba );
  }
  return true;
}

bool kitty_normalize_image( uint32_t format, uint32_t width, uint32_t height, const std::string& input,
                            KittyImage& output, const ImageEncoding& encoding, std::string& error )
{
  error.clear();
  output.png_original = false;
  output.gamut = 0;

  if ( format == KITTY_FORMAT_RGB || format == KITTY_FORMAT_RGBA ) {
    const size_t bytes_per_pixel = format == KITTY_FORMAT_RGB ? 3 : 4;
    size_t expected = 0, rgba_size = 0;
    if ( !graphics_pixel_size( width, height, bytes_per_pixel, expected )
         || !graphics_pixel_size( width, height, 4, rgba_size ) || input.size() != expected ) {
      error = "EINVAL:pixel data size does not match dimensions";
      return false;
    }
    return encode_image( reinterpret_cast<const unsigned char*>( input.data() ),
                          width,
                          height,
                          format == KITTY_FORMAT_RGBA,
                          output,
                          encoding,
                          error );
  }

  if ( packed_format( format ) ) {
    size_t row = 0, expected = 0;
    if ( !packed_size( format, width, height, row, expected ) || input.size() != expected ) {
      error = "EINVAL:pixel data size does not match dimensions";
      return false;
    }
    if ( encoding.packed ) {
      /* Kept as the program packed them, and compressed without loss. */
      auto container = std::make_shared<std::string>();
      if ( !packed_encode( format, width, height, input, encoding.packed_zstd, *container ) ) {
        error = "EINVAL:could not store packed image";
        return false;
      }
      output.format = format;
      output.width = width;
      output.height = height;
      output.palette.clear();
      output.data = container;
      return true;
    }
    /* A client from before packed state takes the pixels expanded. They are
       exact all the same: a program that packed its pixels chose every one. */
    std::string rgb;
    if ( !packed_expand( format, width, height, input, false, rgb ) ) {
      error = "EINVAL:pixel data size does not match dimensions";
      return false;
    }
    return encode_image( reinterpret_cast<const unsigned char*>( rgb.data() ), width, height, false, output,
                         kitty_exact_encoding( encoding ), error );
  }

  if ( format == KITTY_FORMAT_PNG ) {
    png_image image;
    memset( &image, 0, sizeof( image ) );
    image.version = PNG_IMAGE_VERSION;
    if ( input.empty() || !png_image_begin_read_from_memory( &image, input.data(), input.size() ) ) {
      error = "EINVAL:invalid PNG image";
      return false;
    }
    if ( encoding.png && png_has_color_profile( input ) ) {
      /* A PNG that says what its colors are, Display P3 or Rec. 2020 for
         one, reaches the terminal as it is, for the terminal to read. */
      output.format = KITTY_FORMAT_PNG;
      output.width = image.width;
      output.height = image.height;
      output.data = std::make_shared<std::string>( input );
      output.png_original = true;
      png_image_free( &image );
      return true;
    }

    png_image_free( &image );
    /* Its colors as Silkie reads them: a PNG in Display P3 or Rec. 2020 is
       kept in that gamut and tagged with it, any other profile is converted
       to sRGB. */
    std::string rgba;
    uint32_t png_width = 0, png_height = 0, gamut = 0;
    if ( !kitty_png_to_rgba( input, rgba, png_width, png_height, gamut ) ) {
      error = "EINVAL:PNG decode failed";
      return false;
    }
    if ( !encode_image( reinterpret_cast<const unsigned char*>( rgba.data() ), png_width, png_height, true, output,
                        encoding, error ) ) {
      return false;
    }
    output.gamut = gamut;
    return true;
  }

  error = "EINVAL:unsupported Kitty image format";
  return false;
}

bool kitty_webp_to_rgba( const std::string& webp, std::string& rgba, uint32_t& width, uint32_t& height )
{
  if ( !kitty_webp_dimensions( webp, width, height ) ) {
    return false;
  }
  const size_t size = static_cast<size_t>( width ) * static_cast<size_t>( height ) * 4;
  rgba.assign( size, '\0' );
  if ( WebPDecodeRGBAInto( reinterpret_cast<const uint8_t*>( webp.data() ),
                           webp.size(),
                           reinterpret_cast<uint8_t*>( &rgba[0] ),
                           rgba.size(),
                           static_cast<int>( width * 4 ) )
       == NULL ) {
    rgba.clear();
    return false;
  }
  return true;
}

bool kitty_dirty_rect( const std::string& before, const std::string& after, uint32_t width, uint32_t height,
                       uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h )
{
  const size_t row = size_t( width ) * 4;
  if ( before.size() != after.size() || before.size() != row * height ) {
    x = y = 0;
    w = width;
    h = height;
    return true;
  }
  uint32_t left = width, right = 0, top = height, bottom = 0;
  for ( uint32_t r = 0; r < height; r++ ) {
    const char* a = before.data() + r * row;
    const char* b = after.data() + r * row;
    if ( memcmp( a, b, row ) == 0 ) {
      continue;
    }
    top = std::min( top, r );
    bottom = r;
    uint32_t first = 0, last = width - 1;
    while ( first < width && memcmp( a + first * 4, b + first * 4, 4 ) == 0 ) { first++; }
    while ( last > first && memcmp( a + size_t( last ) * 4, b + size_t( last ) * 4, 4 ) == 0 ) { last--; }
    left = std::min( left, first );
    right = std::max( right, last );
  }
  if ( top == height ) {
    return false;
  }
  x = left;
  y = top;
  w = right - left + 1;
  h = bottom - top + 1;
  return true;
}

bool kitty_image_is_packed( const KittyImage& image )
{
  return !image.native && packed_format( image.format );
}

bool kitty_image_packed_bytes( const KittyImage& image, std::string& packed )
{
  return kitty_image_is_packed( image ) && image.data
         && packed_decode( image.format, image.width, image.height, *image.data, packed );
}

ImageEncoding kitty_exact_encoding( const ImageEncoding& encoding )
{
  ImageEncoding exact = encoding;
  exact.quality = -1;
  exact.djvu_lossy = false;
  return exact;
}

uint32_t kitty_format_bit( uint32_t format )
{
  // These are internal capability bits, not the public f values.
  unsigned bit = 0;
  for ( uint32_t f : { 301u, 302u, 304u, 308u, 516u, 8u, 16u, 401u, 402u, 404u, 408u, 316u, 632u, 664u, 24u, 32u } ) {
    if ( f == format ) { return uint32_t( 1 ) << bit; }
    ++bit;
  }
  return 0;
}

bool kitty_palette_format( uint32_t format )
{
  return format == 401 || format == 402 || format == 404 || format == 408;
}

bool kitty_native_format( uint32_t format )
{
  return format == 8 || format == 16 || kitty_palette_format( format )
         || format == 316 || format == 632 || format == 664;
}

bool kitty_transfer_valid( uint32_t format, uint32_t transfer )
{
  if ( !transfer ) { return true; }
  if ( format == KITTY_FORMAT_HDR ) { return transfer == 8; }
  if ( format != 24 && format != 32 && !packed_format( format )
       && format != 316 && format != 632 && format != 664 ) { return false; }
  return transfer == 8 || transfer == 13 || transfer == 16 || transfer == 18;
}

bool kitty_native_size( uint32_t format, uint32_t width, uint32_t height, size_t& size )
{
  size_t row = 0;
  if ( packed_format( format ) ) { return packed_size( format, width, height, row, size ); }
  if ( kitty_palette_format( format ) ) {
    const unsigned bits = format - 400;
    if ( !packed_size( 300 + bits, width, height, row, size ) ) { return false; }
    const size_t palette = size_t( 4 ) << bits;
    if ( size > std::string().max_size() - palette ) { return false; }
    size += palette;
    return true;
  }
  const size_t bytes = format == 8 ? 1 : format == 16 || format == 316 ? 2
                       : format == 24 ? 3 : format == 32 || format == 632 ? 4 : format == 664 ? 8 : 0;
  return bytes && graphics_pixel_size( width, height, bytes, size );
}

/* Decode only at the point of drawing. State always retains every source bit.
   Indexed pixels require the terminal's live palette and are sent to it as
   indices; as in Silkie they cannot be sampled by a vector image. */
bool kitty_native_to_hdr( const KittyImage& image, std::string& half )
{
  size_t size = 0, half_size = 0;
  if ( !image.native || !image.data || image.format == 8 || image.format == 16
       || !kitty_native_size( image.format, image.width, image.height, size ) || image.data->size() != size
       || !graphics_pixel_size( image.width, image.height, 8, half_size ) ) { return false; }
  const std::string& input = *image.data;
  std::string expanded;
  if ( packed_format( image.format ) && !packed_expand( image.format, image.width, image.height, input, true, expanded ) ) {
    return false;
  }
  half.resize( half_size );
  const auto byte = [&]( size_t at ) { return uint32_t( uint8_t( input[at] ) ); };
  const auto word = [&]( size_t at ) { return byte( at ) | ( byte( at + 1 ) << 8 ); };
  const unsigned bits = kitty_palette_format( image.format ) ? image.format - 400 : 0;
  const size_t row = bits ? ( size_t( image.width ) / ( 8 / bits ) + ( image.width % ( 8 / bits ) != 0 ) ) : 0;
  for ( size_t i = 0, n = half_size / 8; i < n; ++i ) {
    double c[4] = { 0, 0, 0, 1 };
    if ( bits ) {
      const size_t x = i % image.width, y = i / image.width;
      const unsigned index = ( byte( ( size_t( 4 ) << bits ) + y * row + x / ( 8 / bits ) )
                               >> ( 8 - bits - ( x % ( 8 / bits ) ) * bits ) ) & ( ( 1u << bits ) - 1 );
      for ( unsigned k = 0; k < 4; ++k ) { c[k] = byte( 4 * index + k ) / 255.0; }
    } else if ( !expanded.empty() ) {
      for ( unsigned k = 0; k < 4; ++k ) { c[k] = uint8_t( expanded[4 * i + k] ) / 255.0; }
    } else if ( image.format == 316 ) {
      c[0] = c[1] = c[2] = word( 2 * i ) / 65535.0;
    } else if ( image.format == 632 ) {
      const uint32_t value = word( 4 * i ) | ( word( 4 * i + 2 ) << 16 );
      for ( unsigned k = 0; k < 3; ++k ) { c[k] = ( ( value >> ( 10 * k ) ) & 1023 ) / 1023.0; }
      c[3] = ( value >> 30 ) / 3.0;
    } else if ( image.format == 664 ) {
      for ( unsigned k = 0; k < 4; ++k ) { c[k] = word( 8 * i + 2 * k ) / 65535.0; }
    } else {
      const unsigned channels = image.format == 24 ? 3 : 4;
      for ( unsigned k = 0; k < channels; ++k ) { c[k] = byte( channels * i + k ) / 255.0; }
    }
    decode_image_color( image.transfer, image.gamut, c );
    for ( unsigned k = 0; k < 4; ++k ) {
      const uint16_t value = float_to_half( float( c[k] ) );
      half[8 * i + 2 * k] = char( value & 255 );
      half[8 * i + 2 * k + 1] = char( value >> 8 );
    }
  }
  return true;
}

uint32_t kitty_packed_formats( const std::string& listing )
{
  uint32_t mask = 0;
  size_t at = 0;
  while ( at <= listing.size() ) {
    const size_t end = std::min( listing.find( ',', at ), listing.size() );
    const std::string item = listing.substr( at, end - at );
    for ( uint32_t format : { 301u, 302u, 304u, 308u, 516u, 8u, 16u, 401u, 402u, 404u, 408u, 316u, 632u, 664u, 24u, 32u } ) {
      if ( item == std::to_string( format ) ) {
        mask |= kitty_format_bit( format );
      }
    }
    at = end + 1;
  }
  return mask;
}

bool kitty_image_to_rgba( const KittyImage& image, std::string& rgba )
{
  if ( image.native ) {
    std::string half;
    return kitty_native_to_hdr( image, half ) && hdr_to_srgb8( half, image.width, image.height, image.tone_map, rgba );
  }
  rgba.clear();
  if ( !image.data ) { return false; }
  if ( kitty_image_is_packed( image ) ) {
    std::string packed;
    return kitty_image_packed_bytes( image, packed )
           && packed_expand( image.format, image.width, image.height, packed, true, rgba );
  }
  if ( image.format == KITTY_FORMAT_RAW ) {
    size_t size = 0;
    if ( !graphics_pixel_size( image.width, image.height, 4, size ) || image.data->size() != size ) { return false; }
    rgba = *image.data;
    return true;
  }
  if ( image.format == KITTY_FORMAT_DJVU ) {
    return decode_palette_djvu( image.palette, *image.data, image.width, image.height, rgba );
  }
  if ( image.format == KITTY_FORMAT_PNG && image.palette.empty() ) {
    png_image png;
    memset( &png, 0, sizeof( png ) );
    png.version = PNG_IMAGE_VERSION;
    if ( image.data->empty() || !png_image_begin_read_from_memory( &png, image.data->data(), image.data->size() ) ) {
      png_image_free( &png );
      return false;
    }
    size_t size = 0;
    if ( png.width != image.width || png.height != image.height
         || !graphics_pixel_size( png.width, png.height, 4, size ) ) {
      png_image_free( &png );
      return false;
    }
    png.format = PNG_FORMAT_RGBA;
    rgba.resize( size );
    const bool ok = png_image_finish_read( &png, NULL, &rgba[0], 0, NULL );
    png_image_free( &png );
    if ( !ok ) { rgba.clear(); }
    return ok;
  }
  if ( image.format != KITTY_FORMAT_WEBP || !image.palette.empty() ) { return false; }
  uint32_t width = 0, height = 0;
  if ( !kitty_webp_to_rgba( *image.data, rgba, width, height ) || width != image.width || height != image.height ) {
    rgba.clear();
    return false;
  }
  return true;
}

static void append_u32_key( std::string& controls, const char* key, uint32_t value, bool skip_zero = true )
{
  if ( skip_zero && value == 0 ) {
    return;
  }
  char buf[64];
  snprintf( buf, sizeof( buf ), "%s%s=%u", controls.empty() ? "" : ",", key, value );
  controls.append( buf );
}

static bool anonymous_placement_removed( const std::vector<KittyPlacement>& old_places,
                                         const std::vector<KittyPlacement>& now_places,
                                         uint32_t image_id )
{
  std::vector<bool> matched( now_places.size(), false );
  for ( size_t i = 0; i < old_places.size(); i++ ) {
    if ( old_places[i].image_id != image_id || old_places[i].placement_id != 0 ) {
      continue;
    }
    bool found = false;
    for ( size_t j = 0; j < now_places.size(); j++ ) {
      if ( !matched[j] && now_places[j].image_id == image_id && now_places[j].placement_id == 0
           && old_places[i] == now_places[j] ) {
        matched[j] = true;
        found = true;
        break;
      }
    }
    if ( !found ) {
      return true;
    }
  }
  return false;
}

/* A vector image that only moved on from the version the terminal has: it
   is sent the records it lacks, not the scene again. */
static bool vector_steps( const KittyImage& before, const KittyImage& after, std::vector<Vector::Step>& steps )
{
  return before.format == KITTY_FORMAT_VECTOR && after.format == KITTY_FORMAT_VECTOR && before.vector
         && after.vector && before.serial == after.serial
         && Vector::steps_between( before.vector->lineage, before.vector->version, after.vector, steps );
}

/* An image whose newest version only changed a rectangle of the version
   the terminal has. */
static bool rect_patch( const KittyImage& before, const KittyImage& after )
{
  return after.delta && after.delta->base_serial == before.serial && before.width == after.width
         && before.height == after.height && after.origin == ImageOrigin::Kitty;
}

std::string kitty_vector_records( const KittyImage& image )
{
  return image.vector ? image.vector->table()->text() : std::string();
}

bool kitty_image_needs_gamut( const KittyImage& image )
{
  return image.gamut != 0 && image.format != KITTY_FORMAT_VECTOR && image.format != KITTY_FORMAT_HDR
         && !image.png_original;
}

bool kitty_image_needs_hdr( const KittyImage& image )
{
  return image.format == KITTY_FORMAT_HDR || image.native;
}

namespace {
/* The session fonts of the client's terminal, brought in line with the
   state it is drawn: the uploads the program made are written to it as they
   come, so what it lacks is what it never saw (a new client, a repaint) and
   what was released since. A scene sent whole, or a patch, that names a font
   released since is given that font for the while, as the terminal lays its
   text out again from the font. */
class FontReplay
{
  const Vector::SessionFonts& now;
  std::set<std::string> has; /* what the terminal has, as far as is known */

  void upload( const std::string& name, const Vector::SessionFont& font, std::string& out )
  {
    if ( font.upload ) {
      out += encode_vector_service( Vector::FONT_REPLAY_ID, *font.upload, 2 );
      has.insert( name );
    }
  }

public:
  FontReplay( const Framebuffer& last, const Framebuffer& current, bool initialized, std::string& out )
    : now( current.get_vector_fonts() )
  {
    const Vector::SessionFonts& before = last.get_vector_fonts();
    if ( initialized ) {
      for ( const auto& font : before ) {
        if ( !font.second.released ) {
          has.insert( font.first );
        }
      }
    }
    for ( const auto& font : now ) {
      if ( font.second.released ) {
        continue;
      }
      const auto old = before.find( font.first );
      const bool changed = old == before.end() || old->second.generation != font.second.generation;
      if ( initialized && !changed && !old->second.released ) {
        continue;
      }
      if ( initialized && changed && font.second.request ) {
        has.insert( font.first ); /* the program's own upload, written as it came */
        continue;
      }
      upload( font.first, font.second, out );
    }
  }
  bool available( const std::string& name ) const
  {
    const auto font = now.find( name );
    return has.count( name ) || ( font != now.end() && font->second.upload );
  }
  void need( const std::vector<std::string>& names, std::string& out )
  {
    for ( const auto& name : names ) {
      const auto font = now.find( name );
      if ( !has.count( name ) && font != now.end() ) {
        upload( name, font->second, out );
      }
    }
  }
  /* What the terminal has that the program released, it releases. */
  void finish( std::string& out )
  {
    for ( const auto& name : has ) {
      const auto font = now.find( name );
      if ( font == now.end() || font->second.released ) {
        out += encode_vector_service( Vector::FONT_REPLAY_ID, Vector::font_release_request( name ), 2 );
      }
    }
  }
};

/* Whether some records hold a text record: a line that starts with t. */
bool has_text_record( const std::string& records )
{
  for ( size_t at = 0; at < records.size(); ) {
    const size_t start = records.find_first_not_of( ' ', at );
    if ( start == std::string::npos ) {
      break;
    }
    if ( records[start] == 't' && ( start + 1 == records.size() || records[start + 1] == ' ' ) ) {
      return true;
    }
    const size_t nl = records.find( '\n', start );
    at = nl == std::string::npos ? records.size() : nl + 1;
  }
  return false;
}

/* The session fonts the text records of some steps name. */
std::vector<std::string> step_fonts( const std::vector<Vector::Step>& steps )
{
  std::vector<std::string> names;
  for ( const auto& step : steps ) {
    if ( !has_text_record( step.records ) ) {
      continue;
    }
    std::vector<Vector::Record> records;
    std::string error;
    if ( Vector::parse_records( step.records, records, error ) ) {
      for ( const auto& record : records ) {
        names.insert( names.end(), record.fonts.begin(), record.fonts.end() );
      }
    }
  }
  return names;
}
}

/* A patch the terminal can take: every font its text names is one the
   terminal has or can be given; otherwise the scene is sent whole. */
static bool vector_patch_ok( const KittyImage& before,
                             const KittyImage& after,
                             std::vector<Vector::Step>& steps,
                             const FontReplay& fonts )
{
  if ( !vector_steps( before, after, steps ) ) {
    return false;
  }
  for ( const auto& name : step_fonts( steps ) ) {
    if ( !fonts.available( name ) ) {
      return false;
    }
  }
  return true;
}

static void append_kitty_images( std::string& out,
                                 bool initialized,
                                 const Framebuffer& last,
                                 const Framebuffer& current,
                                 bool convert_sixel,
                                 bool gamut_supported,
                                 bool* vector_resent,
                                 bool viewport_supported,
                                 bool hdr_supported,
                                 uint32_t packed_formats,
                                 bool depth_supported,
                                 CursorDepth* cursor_depth,
                                 FontReplay& fonts );

void append_kitty_frame( std::string& out, bool initialized, const Framebuffer& last, const Framebuffer& current,
                         bool convert_sixel, bool gamut_supported, bool* vector_resent, bool viewport_supported,
                         bool hdr_supported, uint32_t packed_formats, bool depth_supported, CursorDepth* cursor_depth )
{
  FontReplay fonts( last, current, initialized, out );
  append_kitty_images( out, initialized, last, current, convert_sixel, gamut_supported, vector_resent,
                       viewport_supported, hdr_supported, packed_formats, depth_supported, cursor_depth, fonts );
  fonts.finish( out );
}

static void append_kitty_images( std::string& out,
                                 bool initialized,
                                 const Framebuffer& last,
                                 const Framebuffer& current,
                                 bool convert_sixel,
                                 bool gamut_supported,
                                 bool* vector_resent,
                                 bool viewport_supported,
                                 bool hdr_supported,
                                 uint32_t packed_formats,
                                 bool depth_supported,
                                 CursorDepth* cursor_depth,
                                 FontReplay& fonts )
{
  // Sixel-origin images must not be implicitly converted by the existing
  // Kitty renderer. Their client compositor will choose native sixel first.
  bool contains_sixel = false;
  for ( const auto& image : last.get_kitty_images() ) {
    contains_sixel = contains_sixel || image.second.origin == ImageOrigin::Sixel;
  }
  for ( const auto& image : current.get_kitty_images() ) {
    contains_sixel = contains_sixel || image.second.origin == ImageOrigin::Sixel;
  }
  if ( contains_sixel && !convert_sixel ) {
    Framebuffer old_kitty( last ), new_kitty( current );
    for ( const auto& image : last.get_kitty_images() ) {
      if ( image.second.origin == ImageOrigin::Sixel ) { old_kitty.erase_kitty_image( image.first ); }
    }
    for ( const auto& image : current.get_kitty_images() ) {
      if ( image.second.origin == ImageOrigin::Sixel ) { new_kitty.erase_kitty_image( image.first ); }
    }
    append_kitty_images( out, initialized, old_kitty, new_kitty, false, gamut_supported, vector_resent,
                         viewport_supported, hdr_supported, packed_formats, depth_supported, cursor_depth, fonts );
    return;
  }
  const std::map<uint32_t, KittyImage>& now_images = current.get_kitty_images();
  const std::map<uint32_t, KittyImage>& old_images = last.get_kitty_images();
  const std::vector<KittyPlacement>& now_places = current.get_kitty_placements();
  const std::vector<KittyPlacement>& old_places = last.get_kitty_placements();
  // Cell-tiled desktop viewers have thousands of distinct image IDs.
  // Compare placements within each image rather than rescanning the entire
  // desktop for every image and placement.
  std::map<uint32_t, std::vector<KittyPlacement>> now_places_by_image, old_places_by_image;
  for ( const auto& placement : now_places ) { now_places_by_image[placement.image_id].push_back( placement ); }
  for ( const auto& placement : old_places ) { old_places_by_image[placement.image_id].push_back( placement ); }
  std::set<uint32_t> placements_cleared;

  if ( now_images.empty() && now_places.empty() && old_images.empty() && old_places.empty() ) {
    return;
  }

  if ( !initialized ) {
    // Only delete images owned by this synchronized state. A global delete
    // also removes unrelated local images still visible on the screen.
    for ( std::map<uint32_t, KittyImage>::const_iterator it = old_images.begin(); it != old_images.end(); ++it ) {
      char buf[64];
      snprintf( buf, sizeof( buf ), "\033_Ga=d,d=I,i=%u,q=2\033\\", it->first );
      out.append( buf );
      placements_cleared.insert( it->first );
    }
  }

  std::vector<Vector::Step> steps;
  for ( std::map<uint32_t, KittyImage>::const_iterator it = old_images.begin(); it != old_images.end(); ++it ) {
    std::map<uint32_t, KittyImage>::const_iterator cur = now_images.find( it->first );
    if ( cur == now_images.end()
         || ( !( cur->second == it->second ) && !( initialized && vector_patch_ok( it->second, cur->second, steps, fonts ) )
              && !( initialized && rect_patch( it->second, cur->second ) ) ) ) {
      if ( placements_cleared.insert( it->first ).second ) {
        char buf[64];
        snprintf( buf, sizeof( buf ), "\033_Ga=d,d=I,i=%u,q=2\033\\", it->first );
        out.append( buf );
      }
    }
  }

  for ( std::map<uint32_t, KittyImage>::const_iterator it = now_images.begin(); it != now_images.end(); ++it ) {
    std::map<uint32_t, KittyImage>::const_iterator old = old_images.find( it->first );
    // Received states can branch from the same older network snapshot.
    // Their locally assigned serials can match even when pixels differ.
    if ( initialized && old != old_images.end() && old->second == it->second ) {
      continue;
    }
    if ( initialized && old != old_images.end() && vector_patch_ok( old->second, it->second, steps, fonts ) ) {
      /* Ink the terminal drew itself is on its screen already. */
      fonts.need( step_fonts( steps ), out );
      std::string records;
      for ( const auto& step : steps ) {
        if ( !step.terminal_applied ) {
          records += step.records;
          if ( !records.empty() && records.back() != '\n' ) {
            records += '\n';
          }
        }
      }
      if ( !records.empty() ) {
        std::string controls( "a=e,q=2" );
        append_u32_key( controls, "i", it->second.id, false );
        out.append( encode_kitty_chunks( controls, records ) );
      }
      continue;
    }
    if ( initialized && old != old_images.end() && rect_patch( old->second, it->second ) ) {
      /* Only a rectangle changed: it is written over the image in place. */
      std::string rect;
      if ( kitty_image_to_rgba( it->second.delta->rect, rect ) ) {
        std::string controls( "a=f,r=1,X=1,q=2" );
        append_u32_key( controls, "f", KITTY_FORMAT_RGBA, false );
        append_u32_key( controls, "i", it->second.id, false );
        /* The rectangle is in the gamut of the image: a terminal that has the
           image in that gamut is told so, any other gets sRGB as it got the
           image. */
        if ( gamut_supported ) {
          append_u32_key( controls, "G", it->second.gamut );
        } else {
          gamut_pixels_to_srgb( it->second.gamut, rect, 4 );
        }
        append_u32_key( controls, "x", it->second.delta->x );
        append_u32_key( controls, "y", it->second.delta->y );
        append_u32_key( controls, "s", it->second.delta->rect.width );
        append_u32_key( controls, "v", it->second.delta->rect.height );
        out.append( encode_kitty_chunks( controls, rect ) );
      }
      continue;
    }
    std::string controls( "a=t,q=2" ), pixels;
    if ( it->second.format == KITTY_FORMAT_VECTOR ) {
      /* The terminal lays the text out again, with the fonts it names. Text
         that names a font nobody can give it would cost the whole scene,
         so it is left out. */
      if ( it->second.vector ) {
        const std::shared_ptr<const Vector::Table> table = it->second.vector->table();
        std::vector<std::string> names;
        for ( const auto& font : table->fonts() ) {
          names.push_back( font.first );
        }
        fonts.need( names, out );
        pixels = table->text( [&]( const std::string& name ) { return fonts.available( name ); } );
      }
      append_u32_key( controls, "f", KITTY_FORMAT_VECTOR, false );
      append_u32_key( controls, "T", it->second.tone_map );
      if ( vector_resent ) {
        *vector_resent = true;
      }
    } else if ( it->second.native ) {
      const KittyImage& image = it->second;
      const bool has_transfer = image.transfer && image.transfer != 13;
      const bool precision_terminal = packed_formats & ( kitty_format_bit( 316 ) | kitty_format_bit( 632 ) | kitty_format_bit( 664 ) );
      if ( ( packed_formats & kitty_format_bit( image.format ) ) && ( !has_transfer || precision_terminal )
           && ( !image.gamut || gamut_supported ) ) {
        pixels = *image.data;
        append_u32_key( controls, "f", image.format, false );
        append_u32_key( controls, "G", image.gamut );
        append_u32_key( controls, "E", image.transfer );
        append_u32_key( controls, "T", image.tone_map );
      } else if ( image.format == 8 || image.format == 16 ) {
        // Only the terminal can resolve its live palette. Keep the indices.
        pixels = *image.data;
        append_u32_key( controls, "f", image.format, false );
      } else {
        std::string half;
        if ( !kitty_native_to_hdr( image, half ) ) { continue; }
        if ( hdr_supported ) {
          pixels.swap( half );
          append_u32_key( controls, "f", KITTY_FORMAT_HDR, false );
          append_u32_key( controls, "T", image.tone_map );
        } else if ( hdr_to_srgb8( half, image.width, image.height, image.tone_map, pixels ) ) {
          append_u32_key( controls, "f", KITTY_FORMAT_RGBA, false );
        } else { continue; }
      }
    } else if ( it->second.format == KITTY_FORMAT_HDR ) {
      if ( !it->second.data ) {
        continue;
      }
      if ( hdr_supported ) {
        pixels = *it->second.data;
        append_u32_key( controls, "f", KITTY_FORMAT_HDR, false );
        append_u32_key( controls, "T", it->second.tone_map );
      } else if ( hdr_to_srgb8( *it->second.data, it->second.width, it->second.height, it->second.tone_map, pixels ) ) {
        /* Shown at SDR, clipped or, with T=1, compressed, as such a
           terminal would show it. */
        append_u32_key( controls, "f", KITTY_FORMAT_RGBA, false );
      } else {
        continue;
      }
    } else if ( it->second.format == KITTY_FORMAT_PNG && it->second.png_original && it->second.data ) {
      pixels = *it->second.data;
      append_u32_key( controls, "f", KITTY_FORMAT_PNG, false );
    } else if ( kitty_image_is_packed( it->second ) ) {
      /* A terminal that lists the format is given the pixels as the program
         packed them; any other, the same pixels as RGB. */
      std::string packed;
      if ( !kitty_image_packed_bytes( it->second, packed ) ) {
        continue;
      }
      const uint32_t gamut = it->second.format == 516 ? it->second.gamut : 0;
      if ( ( packed_formats & kitty_format_bit( it->second.format ) ) && ( !gamut || gamut_supported ) ) {
        pixels.swap( packed );
        append_u32_key( controls, "f", it->second.format, false );
        if ( gamut_supported ) {
          append_u32_key( controls, "G", gamut );
        }
      } else if ( packed_expand( it->second.format, it->second.width, it->second.height, packed, false, pixels ) ) {
        append_u32_key( controls, "f", KITTY_FORMAT_RGB, false );
        if ( gamut_supported ) {
          append_u32_key( controls, "G", gamut );
        } else {
          gamut_pixels_to_srgb( gamut, pixels, 3 );
        }
      } else {
        continue;
      }
    } else {
      if ( !kitty_image_to_rgba( it->second, pixels ) ) {
        continue;
      }
      append_u32_key( controls, "f", KITTY_FORMAT_RGBA, false );
      if ( gamut_supported ) {
        append_u32_key( controls, "G", it->second.gamut );
      } else {
        gamut_pixels_to_srgb( it->second.gamut, pixels, 4 );
      }
    }
    append_u32_key( controls, "i", it->second.id, false );
    append_u32_key( controls, "s", it->second.width );
    append_u32_key( controls, "v", it->second.height );
    out.append( encode_kitty_chunks( controls, pixels ) );
  }

  for ( std::map<uint32_t, KittyImage>::const_iterator it = now_images.begin(); it != now_images.end(); ++it ) {
    if ( placements_cleared.find( it->first ) == placements_cleared.end()
         && anonymous_placement_removed( old_places_by_image[it->first], now_places_by_image[it->first], it->first ) ) {
      char buf[64];
      snprintf( buf, sizeof( buf ), "\033_Ga=d,d=i,i=%u,q=2\033\\", it->first );
      out.append( buf );
      placements_cleared.insert( it->first );
    }
  }

  for ( size_t i = 0; i < old_places.size(); i++ ) {
    if ( placements_cleared.find( old_places[i].image_id ) != placements_cleared.end() ) {
      continue;
    }
    bool still = false;
    for ( const auto& placement : now_places_by_image[old_places[i].image_id] ) {
      if ( old_places[i] == placement ) {
        still = true;
        break;
      }
    }
    if ( !still && old_places[i].image_id != 0 ) {
      char buf[80];
      if ( old_places[i].placement_id ) {
        snprintf( buf,
                  sizeof( buf ),
                  "\033_Ga=d,d=i,i=%u,p=%u,q=2\033\\",
                  old_places[i].image_id,
                  old_places[i].placement_id );
      } else {
        /* Anonymous extra placements are wiped with the image delete above
           when the image itself disappears; skip a full-screen wipe. */
        continue;
      }
      out.append( buf );
    }
  }

  /* What depth the terminal's cursor has is the text's, which the caller
     knows or does not; a placement that needs another gives it one. */
  CursorDepth depth_unknown;
  CursorDepth& depth_now = cursor_depth ? *cursor_depth : depth_unknown;
  for ( size_t i = 0; i < now_places.size(); i++ ) {
    bool already = false;
    if ( initialized ) {
      for ( const auto& placement : old_places_by_image[now_places[i].image_id] ) {
        if ( now_places[i] == placement ) {
          already = true;
          break;
        }
      }
    }
    if ( placements_cleared.find( now_places[i].image_id ) != placements_cleared.end() ) {
      already = false;
    }
    if ( already ) {
      continue;
    }
    const KittyPlacement& p = now_places[i];
    /* A placement takes the depth the terminal's cursor has: the terminal is
       given the placement's, and whoever draws next gives it its own again. */
    if ( depth_supported
         && ( !depth_now.known || p.depth != depth_now.depth || p.depth_pixels != depth_now.depth_pixels
              || p.phase != depth_now.phase ) ) {
      out.append( depth_frame( p.depth, p.depth_pixels, p.phase ) );
      depth_now.known = true;
      depth_now.depth = p.depth;
      depth_now.depth_pixels = p.depth_pixels;
      depth_now.phase = p.phase;
    }
    if ( !p.unicode_placeholder && p.parent_image == 0 ) {
      char cup[32];
      snprintf( cup, sizeof( cup ), "\033[%d;%dH", p.row + 1, p.col + 1 );
      out.append( cup );
    }
    std::string controls( "a=p,C=1,q=2" );
    append_u32_key( controls, "i", p.image_id, false );
    append_u32_key( controls, "p", p.placement_id );
    append_u32_key( controls, "c", p.columns );
    append_u32_key( controls, "r", p.rows );
    append_u32_key( controls, "x", p.src_x );
    append_u32_key( controls, "y", p.src_y );
    append_u32_key( controls, "w", p.src_w );
    append_u32_key( controls, "h", p.src_h );
    append_u32_key( controls, "X", p.cell_x );
    append_u32_key( controls, "Y", p.cell_y );
    if ( p.z != 0 ) {
      char zbuf[32];
      snprintf( zbuf, sizeof( zbuf ), ",z=%d", p.z );
      controls.append( zbuf );
    }
    if ( p.unicode_placeholder ) {
      controls.append( ",U=1" );
    }
    append_u32_key( controls, "P", p.parent_image );
    append_u32_key( controls, "Q", p.parent_placement );
    if ( p.H != 0 ) {
      char buf[32];
      snprintf( buf, sizeof( buf ), ",H=%d", p.H );
      controls.append( buf );
    }
    if ( p.V != 0 ) {
      char buf[32];
      snprintf( buf, sizeof( buf ), ",V=%d", p.V );
      controls.append( buf );
    }
    /* A terminal that does not know F would not make the placement at all. */
    if ( viewport_supported ) {
      append_u32_key( controls, "F", p.flags );
    }
    out.append( "\033_G" );
    out.append( controls );
    out.append( "\033\\" );
  }
}

bool kitty_inflate( const std::string& input, std::string& output, size_t hint )
{
  const size_t capacity = std::min<size_t>( output.max_size(), ULONG_MAX );
  if ( input.size() > ULONG_MAX ) { return false; }
  size_t dest_len = hint ? hint : ( input.size() <= ( capacity - 64 ) / 4 ? input.size() * 4 + 64 : capacity );
  if ( dest_len < 64 ) {
    dest_len = 64;
  }

  while ( true ) {
    output.assign( dest_len, '\0' );
    uLongf out_len = dest_len;
    const int rc = uncompress( reinterpret_cast<Bytef*>( &output[0] ),
                               &out_len,
                               reinterpret_cast<const Bytef*>( input.data() ),
                               input.size() );
    if ( rc == Z_OK ) {
      output.resize( out_len );
      return true;
    }
    if ( rc != Z_BUF_ERROR ) {
      return false;
    }
    if ( dest_len == capacity ) {
      return false;
    }
    dest_len = dest_len > capacity / 2 ? capacity : dest_len * 2;
  }
}

/* Compositing, as kitty does it for the frames of an image. */
namespace {
uint32_t div255_round( uint32_t x )
{
  const uint32_t y = x + 128;
  return ( y + ( y >> 8 ) ) >> 8;
}

/* The over operator on straight alpha in 8 bit integers, bit for bit as
   kitty's blend_pixel_over_straight. */
void blend_pixel( unsigned char* dst, const unsigned char* src )
{
  const uint32_t alpha = src[3], dst_alpha = dst[3], inv_alpha = 255 - alpha;
  const uint32_t denom = alpha * 255 + dst_alpha * inv_alpha;
  if ( !denom ) {
    return;
  }
  for ( unsigned c = 0; c < 3; c++ ) {
    const uint32_t num = src[c] * alpha * 255 + dst[c] * dst_alpha * inv_alpha;
    dst[c] = static_cast<unsigned char>( lrintf( float( num ) / float( denom ) ) );
  }
  dst[3] = static_cast<unsigned char>( div255_round( denom ) );
}

/* The same in linear light, on half floats, as kitty's blend_over_half. */
void blend_pixel_half( unsigned char* dst, const unsigned char* src )
{
  uint16_t d[4], s[4];
  memcpy( d, dst, 8 );
  memcpy( s, src, 8 );
  const float alpha = half_to_float( s[3] ), dst_alpha = half_to_float( d[3] ) * ( 1.f - alpha ),
              out_alpha = alpha + dst_alpha;
  if ( out_alpha <= 0.f ) {
    return;
  }
  for ( unsigned c = 0; c < 3; c++ ) {
    d[c] = float_to_half( ( half_to_float( s[c] ) * alpha + half_to_float( d[c] ) * dst_alpha ) / out_alpha );
  }
  d[3] = float_to_half( out_alpha );
  memcpy( dst, d, 8 );
}

void compose_row( unsigned char* under, const unsigned char* over, size_t pixels, bool blend, bool half )
{
  const size_t size = half ? 8 : 4;
  if ( !blend ) {
    memmove( under, over, pixels * size );
    return;
  }
  for ( size_t x = 0; x < pixels; x++ ) {
    if ( half ) {
      blend_pixel_half( under + x * size, over + x * size );
    } else {
      blend_pixel( under + x * size, over + x * size );
    }
  }
}
}

void kitty_compose( std::string& under, uint32_t under_width, uint32_t under_height, const std::string& over,
                    uint32_t over_width, uint32_t over_height, uint32_t x, uint32_t y, bool blend, bool half )
{
  const size_t size = half ? 8 : 4;
  const size_t row_pixels = std::min<size_t>( x < under_width ? under_width - x : 0, over_width );
  for ( size_t row = 0; row + y < under_height && row < over_height && row_pixels; row++ ) {
    const size_t under_at = ( ( row + y ) * under_width + x ) * size, over_at = row * over_width * size;
    if ( under_at + row_pixels * size > under.size() || over_at + row_pixels * size > over.size() ) {
      break;
    }
    compose_row( reinterpret_cast<unsigned char*>( &under[under_at] ),
                 reinterpret_cast<const unsigned char*>( over.data() + over_at ), row_pixels, blend, half );
  }
}

void kitty_compose_within( std::string& pixels, uint32_t width, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                           uint32_t w, uint32_t h, bool blend, bool half )
{
  const size_t size = half ? 8 : 4;
  for ( size_t row = 0; row < h; row++ ) {
    const size_t dst = ( ( row + dy ) * width + dx ) * size, src = ( ( row + sy ) * width + sx ) * size;
    if ( dst + size_t( w ) * size > pixels.size() || src + size_t( w ) * size > pixels.size() ) {
      break;
    }
    compose_row( reinterpret_cast<unsigned char*>( &pixels[dst] ),
                 reinterpret_cast<const unsigned char*>( pixels.data() + src ), w, blend, half );
  }
}

/* Relative placements; see Silkie's docs/viewport-protocol.rst and kitty's
   graphics protocol. */
namespace {
/* The placement a reference (image, placement) names: the first with that
   image and id, as kitty takes the first placement of an image for a
   reference without one. */
const KittyPlacement* find_placement( const Framebuffer& fb, uint32_t image, uint32_t placement )
{
  for ( const auto& candidate : fb.get_kitty_placements() ) {
    if ( candidate.image_id == image && candidate.placement_id == placement ) {
      return &candidate;
    }
  }
  return nullptr;
}

/* The number of a row and column diacritic of a Unicode placeholder, as
   kitty's diacritic_to_num has them (Unicode 17.0.0): 0 for none. */
int diacritic_to_num( uint32_t code )
{
  static const uint32_t ranges[][3] = {
  { 0x305, 0x306, 1 }, { 0x30d, 0x30f, 2 }, { 0x310, 0x311, 4 }, { 0x312, 0x313, 5 }, { 0x33d, 0x340, 6 }, {
  0x346, 0x347, 9 }, { 0x34a, 0x34d, 10 }, { 0x350, 0x353, 13 }, { 0x357, 0x358, 16 }, { 0x35b, 0x35c, 17 }, {
  0x363, 0x370, 18 }, { 0x483, 0x488, 31 }, { 0x592, 0x596, 36 }, { 0x597, 0x59a, 40 }, { 0x59c, 0x5a2, 43 }, {
  0x5a8, 0x5aa, 49 }, { 0x5ab, 0x5ad, 51 }, { 0x5af, 0x5b0, 53 }, { 0x5c4, 0x5c5, 54 }, { 0x610, 0x618, 55 }, {
  0x657, 0x65c, 63 }, { 0x65d, 0x65f, 68 }, { 0x6d6, 0x6dd, 70 }, { 0x6df, 0x6e3, 77 }, { 0x6e4, 0x6e5, 81 }, {
  0x6e7, 0x6e9, 82 }, { 0x6eb, 0x6ed, 84 }, { 0x730, 0x731, 86 }, { 0x732, 0x734, 87 }, { 0x735, 0x737, 89 }, {
  0x73a, 0x73b, 91 }, { 0x73d, 0x73e, 92 }, { 0x73f, 0x742, 93 }, { 0x743, 0x744, 96 }, { 0x745, 0x746, 97 }, {
  0x747, 0x748, 98 }, { 0x749, 0x74b, 99 }, { 0x7eb, 0x7f2, 101 }, { 0x7f3, 0x7f4, 108 }, { 0x816, 0x81a, 109 }, {
  0x81b, 0x824, 113 }, { 0x825, 0x828, 122 }, { 0x829, 0x82e, 125 }, { 0x951, 0x952, 130 }, { 0x953, 0x955, 131 },
  { 0xf82, 0xf84, 133 }, { 0xf86, 0xf88, 135 }, { 0x135d, 0x1360, 137 }, { 0x17dd, 0x17de, 140 }, { 0x193a,
  0x193b, 141 }, { 0x1a17, 0x1a18, 142 }, { 0x1a75, 0x1a7d, 143 }, { 0x1b6b, 0x1b6c, 151 }, { 0x1b6d, 0x1b74, 152
  }, { 0x1cd0, 0x1cd3, 159 }, { 0x1cda, 0x1cdc, 162 }, { 0x1ce0, 0x1ce1, 164 }, { 0x1dc0, 0x1dc2, 165 }, { 0x1dc3,
  0x1dca, 167 }, { 0x1dcb, 0x1dcd, 174 }, { 0x1dd1, 0x1de7, 176 }, { 0x1dfe, 0x1dff, 198 }, { 0x20d0, 0x20d2, 199
  }, { 0x20d4, 0x20d8, 201 }, { 0x20db, 0x20dd, 205 }, { 0x20e1, 0x20e2, 207 }, { 0x20e7, 0x20e8, 208 }, { 0x20e9,
  0x20ea, 209 }, { 0x20f0, 0x20f1, 210 }, { 0x2cef, 0x2cf2, 211 }, { 0x2de0, 0x2e00, 214 }, { 0xa66f, 0xa670, 246
  }, { 0xa67c, 0xa67e, 247 }, { 0xa6f0, 0xa6f2, 249 }, { 0xa8e0, 0xa8f2, 251 }, { 0xaab0, 0xaab1, 269 }, { 0xaab2,
  0xaab4, 270 }, { 0xaab7, 0xaab9, 272 }, { 0xaabe, 0xaac0, 274 }, { 0xaac1, 0xaac2, 276 }, { 0xfe20, 0xfe27, 277
  }, { 0x10a0f, 0x10a10, 284 }, { 0x10a38, 0x10a39, 285 }, { 0x1d185, 0x1d18a, 286 }, { 0x1d1aa, 0x1d1ae, 291 }, {
  0x1d242, 0x1d245, 295 }
  };
  for ( const auto& range : ranges ) {
    if ( code >= range[0] && code <= range[1] ) {
      return int( code - range[0] + range[2] );
    }
  }
  return 0;
}

std::vector<uint32_t> code_points( const std::string& text )
{
  std::vector<uint32_t> out;
  for ( size_t i = 0; i < text.size(); ) {
    const unsigned char c = text[i];
    const size_t length = c < 0x80 ? 1 : ( c >> 5 ) == 6 ? 2 : ( c >> 4 ) == 14 ? 3 : ( c >> 3 ) == 30 ? 4 : 0;
    if ( !length || i + length > text.size() ) {
      break;
    }
    uint32_t code = length == 1 ? c : c & ( 0x7f >> length );
    for ( size_t k = 1; k < length; k++ ) {
      code = code << 6 | ( uint8_t( text[i + k] ) & 0x3f );
    }
    out.push_back( code );
    i += length;
  }
  return out;
}

/* The cell kitty takes a virtual placement to be at: the first row and the
   first column of the placeholders of its image (U+10EEEE, the image id in
   the foreground color and, above 24 bits, in the third diacritic). The
   placement id is in the underline color, which this terminal does not keep,
   so every placeholder of the image counts. */
bool placeholder_cell( const Framebuffer& fb, const KittyPlacement& virtual_placement, int64_t& row, int64_t& col )
{
  bool found = false;
  for ( int y = 0; y < fb.ds.get_height(); y++ ) {
    for ( int x = 0; x < fb.ds.get_width(); x++ ) {
      const Cell* cell = fb.get_cell( y, x );
      const std::string& contents = cell->get_contents();
      if ( contents.compare( 0, 4, "\xF4\x8E\xBB\xAE" ) != 0 ) {
        continue;
      }
      const uint32_t color = cell->get_renditions().get_foreground_color();
      uint32_t id = Renditions::is_true_color( color ) ? color & 0xffffff : color >= 30 ? color - 30 : 0;
      const std::vector<uint32_t> codes = code_points( contents );
      if ( codes.size() >= 4 && diacritic_to_num( codes[3] ) > 0 ) {
        id |= uint32_t( diacritic_to_num( codes[3] ) - 1 ) << 24;
      }
      if ( id != virtual_placement.image_id ) {
        continue;
      }
      row = found ? std::min<int64_t>( row, y ) : y;
      col = found ? std::min<int64_t>( col, x ) : x;
      found = true;
    }
  }
  return found;
}
}

std::string kitty_check_parent( const Framebuffer& fb, KittyPlacement& p )
{
  if ( !p.parent_image ) {
    return std::string();
  }
  char reply[256];
  if ( !fb.find_kitty_image( p.parent_image ) ) {
    snprintf( reply, sizeof reply, "ENOPARENT:Put command refers to a parent image with id: %u that does not exist",
              p.parent_image );
    return reply;
  }
  const KittyPlacement* parent = nullptr;
  for ( const auto& candidate : fb.get_kitty_placements() ) {
    if ( candidate.image_id == p.parent_image && ( !p.parent_placement || candidate.placement_id == p.parent_placement ) ) {
      parent = &candidate;
      break;
    }
  }
  if ( !parent && !p.parent_placement ) {
    snprintf( reply, sizeof reply, "ENOPARENT:Put command refers to a parent image with id: %u that has no placements",
              p.parent_image );
    return reply;
  }
  if ( !parent ) {
    snprintf( reply, sizeof reply,
              "ENOPARENT:Put command refers to a parent image placement with id: %u and placement id: %u that does not "
              "exist",
              p.parent_image, p.parent_placement );
    return reply;
  }
  p.parent_placement = parent->placement_id;
  /* A placement with an id that exists already is the one put moves. */
  const bool replaces = p.placement_id && find_placement( fb, p.image_id, p.placement_id );
  if ( replaces && p.parent_image == p.image_id && p.parent_placement == p.placement_id ) {
    return "EINVAL:Put command refers to itself as its own parent";
  }
  const KittyPlacement* r = &p;
  for ( unsigned depth = 0; r->parent_image; ) {
    if ( r == &p && depth ) {
      return "ECYCLE:This parent reference creates a cycle";
    }
    if ( depth++ >= KITTY_PARENT_DEPTH_LIMIT ) {
      return "ETOODEEP:Too many levels of parent references";
    }
    if ( !fb.find_kitty_image( r->parent_image ) ) {
      snprintf( reply, sizeof reply, "ENOENT:One of the ancestors of this ref with image id: %u not found",
                r->parent_image );
      return reply;
    }
    const KittyPlacement* next = replaces && r->parent_image == p.image_id && r->parent_placement == p.placement_id
                                   ? &p
                                   : find_placement( fb, r->parent_image, r->parent_placement );
    if ( !next ) {
      snprintf( reply, sizeof reply,
                "ENOENT:One of the ancestors of this ref with image id: %u and ref id: %u not found", r->parent_image,
                r->parent_placement );
      return reply;
    }
    r = next;
  }
  return std::string();
}

bool kitty_placement_cell( const Framebuffer& fb, const KittyPlacement& p, int64_t& row, int64_t& col )
{
  int64_t dx = 0, dy = 0;
  const KittyPlacement* r = &p;
  for ( unsigned depth = 0; r->parent_image; depth++ ) {
    if ( depth >= KITTY_PARENT_DEPTH_LIMIT ) {
      return false; /* a cycle, or too many ancestors */
    }
    const KittyPlacement* parent = find_placement( fb, r->parent_image, r->parent_placement );
    if ( !parent ) {
      return false;
    }
    dx += r->H;
    dy += r->V;
    if ( parent->unicode_placeholder ) {
      if ( !placeholder_cell( fb, *parent, row, col ) ) {
        return false;
      }
      row += dy;
      col += dx;
      return true;
    }
    r = parent;
  }
  if ( r->unicode_placeholder ) {
    return false;
  }
  row = r->row + dy;
  col = r->col + dx;
  return true;
}

/* The cell a placement resolves to, as kitty resolves it, and where its cell
   box starts in pixels; see Silkie's docs/viewport-protocol.rst. The chain
   of parents is at most 8 long: that is a rule of the protocol (ETOODEEP). */
namespace {
struct PlacementOrigin
{
  int64_t row = 0, col = 0;
  double x = 0, y = 0;
  bool clipped = false;
  double left = 0, top = 0, right = 0, bottom = 0;
};

bool placement_box( const Framebuffer& fb, const KittyPlacement& p, double cw, double ch, double x, double y,
                    KittyPlacementRect& r )
{
  const KittyImage* image = fb.find_kitty_image( p.image_id );
  if ( !image ) {
    return false;
  }
  r.sx = p.src_x;
  r.sy = p.src_y;
  r.sw = std::min<double>( p.src_w ? p.src_w : image->width,
                           double( image->width ) - std::min<double>( p.src_x, image->width ) );
  r.sh = std::min<double>( p.src_h ? p.src_h : image->height,
                           double( image->height ) - std::min<double>( p.src_y, image->height ) );
  if ( !( r.sw > 0 ) || !( r.sh > 0 ) ) {
    return false;
  }
  /* An offset into the cell is at most the cell, less a pixel. */
  r.left = x + std::min<double>( p.cell_x, cw - 1 );
  r.top = y + std::min<double>( p.cell_y, ch - 1 );
  if ( p.rows ) {
    r.height = y + double( p.rows ) * ch - r.top;
    r.width = p.columns ? x + double( p.columns ) * cw - r.left : r.height * r.sw / r.sh;
  } else {
    r.width = p.columns ? x + double( p.columns ) * cw - r.left : r.sw;
    r.height = r.width * r.sh / r.sw;
  }
  return r.width > 0 && r.height > 0;
}

bool resolve_placement( const Framebuffer& fb, const KittyPlacement& p, double cw, double ch, unsigned depth,
                        PlacementOrigin& out )
{
  if ( depth >= KITTY_PARENT_DEPTH_LIMIT ) {
    return false; /* a cycle, or too many ancestors */
  }
  const KittyPlacement* parent = find_placement( fb, p.parent_image, p.parent_placement );
  if ( !parent ) {
    return false;
  }
  PlacementOrigin above;
  if ( parent->unicode_placeholder ) {
    /* A virtual parent is where kitty puts the cells of its placeholders. */
    if ( !placeholder_cell( fb, *parent, above.row, above.col ) ) {
      return false;
    }
    above.x = double( above.col ) * cw;
    above.y = double( above.row ) * ch;
  } else if ( parent->parent_image ) {
    if ( !resolve_placement( fb, *parent, cw, ch, depth + 1, above ) ) {
      return false;
    }
  } else {
    above.row = parent->row;
    above.col = parent->col;
    above.x = above.col * cw;
    above.y = above.row * ch;
  }
  if ( ( parent->flags & KITTY_PLACE_VIEWPORT ) && !parent->unicode_placeholder ) {
    KittyPlacementRect box;
    if ( !placement_box( fb, *parent, cw, ch, above.x, above.y, box ) ) {
      return false;
    }
    const double right = box.left + box.width, bottom = box.top + box.height;
    above.left = above.clipped ? std::max( above.left, box.left ) : box.left;
    above.top = above.clipped ? std::max( above.top, box.top ) : box.top;
    above.right = above.clipped ? std::min( above.right, right ) : right;
    above.bottom = above.clipped ? std::min( above.bottom, bottom ) : bottom;
    above.clipped = true;
  }
  out = above;
  out.row = above.row + p.V;
  out.col = above.col + p.H;
  if ( p.flags & KITTY_PLACE_PIXEL_RELATIVE ) {
    out.x = above.x + std::min<double>( parent->cell_x, cw - 1 ) + double( p.H ) * cw;
    out.y = above.y + std::min<double>( parent->cell_y, ch - 1 ) + double( p.V ) * ch;
  } else {
    out.x = out.col * cw;
    out.y = out.row * ch;
  }
  return true;
}
}

bool kitty_placement_rect( const Framebuffer& fb, const KittyPlacement& p, double cw, double ch, KittyPlacementRect& out )
{
  if ( p.unicode_placeholder ) {
    return false;
  }
  PlacementOrigin origin;
  if ( p.parent_image ) {
    if ( !resolve_placement( fb, p, cw, ch, 0, origin ) ) {
      return false;
    }
  } else {
    origin.x = double( p.col ) * cw;
    origin.y = double( p.row ) * ch;
  }
  if ( !placement_box( fb, p, cw, ch, origin.x, origin.y, out ) ) {
    return false;
  }
  out.clipped = origin.clipped;
  out.clip_left = origin.left;
  out.clip_top = origin.top;
  out.clip_right = origin.right;
  out.clip_bottom = origin.bottom;
  return true;
}

}
