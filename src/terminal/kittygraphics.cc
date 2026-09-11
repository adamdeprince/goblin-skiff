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
#include "src/terminal/osc52.h"
#include "src/terminal/terminalframebuffer.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/mman.h>

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
    delete_target( 'a' ), payload()
{}

KittyImage::KittyImage()
  : id( 0 ), number( 0 ), format( KITTY_FORMAT_WEBP ), width( 0 ), height( 0 ), origin( ImageOrigin::Kitty ),
    data( std::make_shared<std::string>() ), serial( 0 )
{}

bool KittyImage::operator==( const KittyImage& other ) const
{
  return ( id == other.id ) && ( number == other.number ) && ( format == other.format ) && ( width == other.width )
         && ( height == other.height ) && ( origin == other.origin ) && ( serial == other.serial )
         && ( ( data == other.data ) || ( data && other.data && *data == *other.data ) );
}

KittyPlacement::KittyPlacement()
  : image_id( 0 ), placement_id( 0 ), row( 0 ), col( 0 ), columns( 0 ), rows( 0 ), src_x( 0 ), src_y( 0 ),
    src_w( 0 ), src_h( 0 ), cell_x( 0 ), cell_y( 0 ), z( 0 ), cursor_hold( false ), unicode_placeholder( false ),
    parent_image( 0 ), parent_placement( 0 ), H( 0 ), V( 0 )
{}

bool KittyPlacement::operator==( const KittyPlacement& other ) const
{
  return ( image_id == other.image_id ) && ( placement_id == other.placement_id ) && ( row == other.row )
         && ( col == other.col ) && ( columns == other.columns ) && ( rows == other.rows )
         && ( src_x == other.src_x ) && ( src_y == other.src_y ) && ( src_w == other.src_w )
         && ( src_h == other.src_h ) && ( cell_x == other.cell_x ) && ( cell_y == other.cell_y ) && ( z == other.z )
         && ( cursor_hold == other.cursor_hold ) && ( unicode_placeholder == other.unicode_placeholder )
         && ( parent_image == other.parent_image ) && ( parent_placement == other.parent_placement )
         && ( H == other.H ) && ( V == other.V );
}

static bool parse_u32( const std::string& s, uint32_t& out )
{
  if ( s.empty() ) {
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
        parse_u32( val, cmd.format );
      } else if ( key == "s" ) {
        parse_u32( val, cmd.width );
      } else if ( key == "v" ) {
        parse_u32( val, cmd.height );
      } else if ( key == "S" ) {
        parse_u32( val, cmd.data_size );
      } else if ( key == "O" ) {
        parse_u32( val, cmd.data_offset );
      } else if ( key == "i" ) {
        parse_u32( val, cmd.image_id );
      } else if ( key == "I" ) {
        parse_u32( val, cmd.image_number );
      } else if ( key == "p" ) {
        parse_u32( val, cmd.placement_id );
      } else if ( key == "m" ) {
        parse_u32( val, cmd.more );
      } else if ( key == "q" ) {
        parse_u32( val, cmd.quiet );
      } else if ( key == "c" ) {
        parse_u32( val, cmd.columns );
      } else if ( key == "r" ) {
        parse_u32( val, cmd.rows );
      } else if ( key == "x" ) {
        parse_u32( val, cmd.src_x );
      } else if ( key == "y" ) {
        parse_u32( val, cmd.src_y );
      } else if ( key == "w" ) {
        parse_u32( val, cmd.src_w );
      } else if ( key == "h" ) {
        parse_u32( val, cmd.src_h );
      } else if ( key == "X" ) {
        parse_u32( val, cmd.cell_x );
      } else if ( key == "Y" ) {
        parse_u32( val, cmd.cell_y );
      } else if ( key == "z" ) {
        parse_i32( val, cmd.z );
      } else if ( key == "C" ) {
        parse_u32( val, cmd.cursor_hold );
      } else if ( key == "U" ) {
        parse_u32( val, cmd.unicode_placeholder );
      } else if ( key == "P" ) {
        parse_u32( val, cmd.parent_image );
      } else if ( key == "Q" ) {
        parse_u32( val, cmd.parent_placement );
      } else if ( key == "H" ) {
        parse_i32( val, cmd.H );
      } else if ( key == "V" ) {
        parse_i32( val, cmd.V );
      } else if ( key == "d" && val.size() == 1 ) {
        cmd.delete_target = val[0];
      }
    }
    start = comma + 1;
  }

  if ( !b64.empty() && !base64_decode_osc52( b64, cmd.payload ) ) {
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

std::string kitty_response( uint32_t image_id, uint32_t placement_id, const std::string& message )
{
  std::ostringstream out;
  out << "\033_Gi=" << image_id;
  if ( placement_id != 0 ) {
    out << ",p=" << placement_id;
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

static bool read_regular_file( const std::string& path, uint32_t offset, uint32_t size, std::string& data, bool temporary )
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
  if ( wanted > available || wanted > KITTY_IMAGE_QUOTA ) { close( fd ); return false; }
  data.assign( static_cast<size_t>( wanted ), '\0' );
  size_t at = 0;
  while ( at < wanted ) {
    const ssize_t n = pread( fd, &data[at], wanted - at, off + at );
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

  if ( cmd.medium == 'f' || cmd.medium == 't' ) {
    const std::string path = canonical_path( cmd.payload );
    if ( path.empty() || path_is_sensitive( path ) ) {
      error = "EINVAL: refused to read path";
      return false;
    }
    if ( !read_regular_file( path, cmd.data_offset, cmd.data_size, data, cmd.medium == 't' ) ) {
      error = "ENOENT: could not read file";
      return false;
    }
    return true;
  }

  if ( cmd.medium == 's' ) {
#if defined( __unix__ ) || defined( __APPLE__ )
    const std::string name = cmd.payload;
    if ( name.empty() || name.find( '\0' ) != std::string::npos ) {
      error = "EINVAL: empty shm name";
      return false;
    }
    int fd = shm_open( name.c_str(), O_RDONLY, 0 );
    if ( fd < 0 ) {
      error = "ENOENT: shm_open failed";
      return false;
    }
    struct stat st;
    if ( fstat( fd, &st ) < 0 || st.st_size < 0 ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "EINVAL: shm fstat failed";
      return false;
    }
    const uint64_t total64 = static_cast<uint64_t>( st.st_size );
    const uint64_t off64 = cmd.data_offset;
    if ( total64 > SIZE_MAX || off64 > total64 ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "EINVAL: shm offset";
      return false;
    }
    const size_t total = static_cast<size_t>( total64 );
    const size_t off = static_cast<size_t>( off64 );
    const size_t available = total - off;
    const size_t want = cmd.data_size == 0 ? available : cmd.data_size;
    if ( want > KITTY_IMAGE_QUOTA || want > available ) {
      close( fd );
      shm_unlink( name.c_str() );
      error = "ENOSPC: shared memory image exceeds storage quota";
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
      error = "EINVAL: mmap failed";
      return false;
    }
    data.assign( static_cast<char*>( map ) + skip, want );
    munmap( map, mapped );
    shm_unlink( name.c_str() );
    return true;
#else
    error = "EINVAL: shared memory not supported";
    return false;
#endif
  }

  error = "EINVAL: unknown transmission medium";
  return false;
}

static bool checked_pixel_size( uint32_t width, uint32_t height, size_t bytes_per_pixel, size_t& size )
{
  if ( width == 0 || height == 0 || width > GRAPHICS_MAX_DIMENSION || height > GRAPHICS_MAX_DIMENSION
       || uint64_t( width ) * height > GRAPHICS_MAX_PIXELS || bytes_per_pixel == 0 ) {
    return false;
  }
  if ( width > KITTY_IMAGE_QUOTA / bytes_per_pixel
       || height > KITTY_IMAGE_QUOTA / ( static_cast<size_t>( width ) * bytes_per_pixel ) ) {
    return false;
  }
  size = static_cast<size_t>( width ) * static_cast<size_t>( height ) * bytes_per_pixel;
  return size <= KITTY_IMAGE_QUOTA;
}

bool kitty_webp_dimensions( const std::string& webp, uint32_t& width, uint32_t& height )
{
  int decoded_width = 0, decoded_height = 0;
  size_t rgba_size = 0;
  if ( webp.empty() || webp.size() > KITTY_IMAGE_QUOTA
       || !WebPGetInfo(
         reinterpret_cast<const uint8_t*>( webp.data() ), webp.size(), &decoded_width, &decoded_height )
       || decoded_width <= 0 || decoded_height <= 0
       || !checked_pixel_size(
         static_cast<uint32_t>( decoded_width ), static_cast<uint32_t>( decoded_height ), 4, rgba_size ) ) {
    return false;
  }
  width = static_cast<uint32_t>( decoded_width );
  height = static_cast<uint32_t>( decoded_height );
  return true;
}

static bool encode_lossless_webp( const unsigned char* pixels,
                                  uint32_t width,
                                  uint32_t height,
                                  bool alpha,
                                  std::string& webp,
                                  std::string& error )
{
  const int stride = static_cast<int>( width * ( alpha ? 4 : 3 ) );
  uint8_t* encoded = NULL;
  const size_t encoded_size = alpha ? WebPEncodeLosslessRGBA( pixels, width, height, stride, &encoded )
                                    : WebPEncodeLosslessRGB( pixels, width, height, stride, &encoded );
  if ( encoded_size == 0 || encoded == NULL || encoded_size > KITTY_IMAGE_QUOTA ) {
    WebPFree( encoded );
    error = "EINVAL: WebP encode failed";
    return false;
  }
  webp.assign( reinterpret_cast<const char*>( encoded ), encoded_size );
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
  error.clear();

  if ( format == KITTY_FORMAT_RGB || format == KITTY_FORMAT_RGBA ) {
    const size_t bytes_per_pixel = format == KITTY_FORMAT_RGB ? 3 : 4;
    size_t expected = 0, rgba_size = 0;
    if ( !checked_pixel_size( width, height, bytes_per_pixel, expected )
         || !checked_pixel_size( width, height, 4, rgba_size ) || input.size() != expected ) {
      error = "EINVAL: pixel data size does not match dimensions";
      return false;
    }
    output_width = width;
    output_height = height;
    return encode_lossless_webp( reinterpret_cast<const unsigned char*>( input.data() ),
                                 width,
                                 height,
                                 format == KITTY_FORMAT_RGBA,
                                 webp,
                                 error );
  }

  if ( format == KITTY_FORMAT_PNG ) {
    png_image image;
    memset( &image, 0, sizeof( image ) );
    image.version = PNG_IMAGE_VERSION;
    if ( input.empty() || !png_image_begin_read_from_memory( &image, input.data(), input.size() ) ) {
      error = "EINVAL: invalid PNG image";
      return false;
    }

    size_t rgba_size = 0;
    const uint32_t png_width = image.width;
    const uint32_t png_height = image.height;
    if ( !checked_pixel_size( png_width, png_height, 4, rgba_size ) ) {
      png_image_free( &image );
      error = "EINVAL: PNG dimensions exceed image quota";
      return false;
    }

    image.format = PNG_FORMAT_RGBA;
    std::string rgba( rgba_size, '\0' );
    if ( !png_image_finish_read( &image, NULL, &rgba[0], 0, NULL ) ) {
      png_image_free( &image );
      error = "EINVAL: PNG decode failed";
      return false;
    }
    png_image_free( &image );
    output_width = png_width;
    output_height = png_height;
    return encode_lossless_webp(
      reinterpret_cast<const unsigned char*>( rgba.data() ), output_width, output_height, true, webp, error );
  }

  error = "EINVAL: unsupported Kitty image format";
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

void append_kitty_frame( std::string& out, bool initialized, const Framebuffer& last, const Framebuffer& current,
                         bool convert_sixel )
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
    append_kitty_frame( out, initialized, old_kitty, new_kitty );
    return;
  }
  const std::map<uint32_t, KittyImage>& now_images = current.get_kitty_images();
  const std::map<uint32_t, KittyImage>& old_images = last.get_kitty_images();
  const std::vector<KittyPlacement>& now_places = current.get_kitty_placements();
  const std::vector<KittyPlacement>& old_places = last.get_kitty_placements();
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

  for ( std::map<uint32_t, KittyImage>::const_iterator it = old_images.begin(); it != old_images.end(); ++it ) {
    std::map<uint32_t, KittyImage>::const_iterator cur = now_images.find( it->first );
    if ( cur == now_images.end() || cur->second.serial != it->second.serial ) {
      if ( placements_cleared.insert( it->first ).second ) {
        char buf[64];
        snprintf( buf, sizeof( buf ), "\033_Ga=d,d=I,i=%u,q=2\033\\", it->first );
        out.append( buf );
      }
    }
  }

  for ( std::map<uint32_t, KittyImage>::const_iterator it = now_images.begin(); it != now_images.end(); ++it ) {
    std::map<uint32_t, KittyImage>::const_iterator old = old_images.find( it->first );
    if ( initialized && old != old_images.end() && old->second.serial == it->second.serial ) {
      continue;
    }
    if ( it->second.format != KITTY_FORMAT_WEBP || !it->second.data ) {
      continue;
    }
    const std::string& webp = *it->second.data;
    std::string rgba;
    uint32_t width = 0, height = 0;
    if ( !kitty_webp_to_rgba( webp, rgba, width, height ) || width != it->second.width
         || height != it->second.height ) {
      continue;
    }
    std::string controls( "a=t,q=2" );
    append_u32_key( controls, "f", KITTY_FORMAT_RGBA, false );
    append_u32_key( controls, "i", it->second.id, false );
    append_u32_key( controls, "s", width );
    append_u32_key( controls, "v", height );
    out.append( encode_kitty_chunks( controls, rgba ) );
  }

  for ( std::map<uint32_t, KittyImage>::const_iterator it = now_images.begin(); it != now_images.end(); ++it ) {
    if ( placements_cleared.find( it->first ) == placements_cleared.end()
         && anonymous_placement_removed( old_places, now_places, it->first ) ) {
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
    for ( size_t j = 0; j < now_places.size(); j++ ) {
      if ( old_places[i] == now_places[j] ) {
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

  for ( size_t i = 0; i < now_places.size(); i++ ) {
    bool already = false;
    if ( initialized ) {
      for ( size_t j = 0; j < old_places.size(); j++ ) {
        if ( now_places[i] == old_places[j] ) {
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
    out.append( "\033_G" );
    out.append( controls );
    out.append( "\033\\" );
  }
}

bool kitty_inflate( const std::string& input, std::string& output, size_t hint )
{
  size_t dest_len = hint ? hint : ( input.size() * 4 + 64 );
  if ( dest_len < 64 ) {
    dest_len = 64;
  }
  if ( dest_len > KITTY_IMAGE_QUOTA ) {
    dest_len = KITTY_IMAGE_QUOTA;
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
    if ( dest_len == KITTY_IMAGE_QUOTA ) {
      return false;
    }
    dest_len = dest_len > KITTY_IMAGE_QUOTA / 2 ? KITTY_IMAGE_QUOTA : dest_len * 2;
  }
}

}
