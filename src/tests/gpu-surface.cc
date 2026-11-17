/* Distributed under the GNU GPL, version 3 or later. */
/* GPU surfaces (t=g): read where the program is, sent on as pixels. */
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#include "src/statesync/completeterminal.h"
#include "src/terminal/gpusurface.h"
#include "src/terminal/kittygraphics.h"

#include <cerrno>
#include <cstring>
#if defined( __APPLE__ )
#include <dlfcn.h>
#endif
#if defined( __linux__ )
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

using namespace Terminal;
namespace {
void require( bool ok, const std::string& message )
{
  if ( !ok ) {
    throw std::runtime_error( message );
  }
}

std::string base64( const std::string& in )
{
  static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for ( size_t i = 0; i < in.size(); i += 3 ) {
    unsigned v = ( unsigned char )in[i] << 16;
    if ( i + 1 < in.size() ) { v |= ( unsigned char )in[i + 1] << 8; }
    if ( i + 2 < in.size() ) { v |= ( unsigned char )in[i + 2]; }
    out += digits[v >> 18 & 63];
    out += digits[v >> 12 & 63];
    out += i + 1 < in.size() ? digits[v >> 6 & 63] : '=';
    out += i + 2 < in.size() ? digits[v & 63] : '=';
  }
  return out;
}

template<class Change>
void refreshes( Complete& terminal, Change change );

void refusals()
{
  Complete terminal( 80, 24 );
  std::string reply = terminal.act( "\033_Ga=t,t=g,i=5;" + base64( "dmabuf:1:AB24:0:3,0,8" ) + "\033\\" );
  require( reply.find( "EINVAL" ) != std::string::npos, "a surface that is not this platform's: " + reply );
  reply = terminal.act( "\033_Ga=t,t=g,i=5,m=1;" + base64( "iosurface:1" ) + "\033\\\033_Gm=0;\033\\" );
  require( reply.find( "EINVAL:chunking" ) != std::string::npos, "no chunking with t=g: " + reply );
  reply = terminal.act( "\033_Ga=r,i=77\033\\" );
  require( reply.find( "ENOENT" ) != std::string::npos, "refresh of no image: " + reply );
}

/* The gpu_surfaces spec: an image backed by a surface has no frames. */
[[maybe_unused]] void frames_refused( Complete& terminal )
{
  std::string reply = terminal.act( "\033_Ga=f,i=5,r=1,f=32,s=1,v=1,m=1;AAAA\033\\\033_Gm=0;AAAA\033\\" );
  require( reply.find( "EINVAL:Animation frames are not supported for GPU surface images" ) != std::string::npos,
           "no frames for a surface: " + reply );
  reply = terminal.act( "\033_Ga=c,i=5,r=1,c=1,x=1,w=1,h=1\033\\" );
  require( reply.find( "EINVAL:Composition is not supported for GPU surface images" ) != std::string::npos,
           "nor composition: " + reply );
  require( terminal.get_fb().get_kitty_images().size() >= 1 && !terminal.get_fb().find_kitty_image( 1 ),
           "and the chunks made no image" );
}

#if defined( __APPLE__ )
/* A global IOSurface, made through the frameworks at run time. */
struct Surface
{
  void* ref = nullptr;
  uint32_t id = 0;
  unsigned char* base = nullptr;
  size_t stride = 0;
};

Surface make_surface( int width, int height )
{
  void* io = dlopen( "/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_LAZY );
  void* cf = dlopen( "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY );
  require( io && cf, "frameworks" );
  auto create_dict = reinterpret_cast<void* ( * )( void*, long, const void*, const void* )>(
    dlsym( cf, "CFDictionaryCreateMutable" ) );
  auto set = reinterpret_cast<void ( * )( void*, const void*, const void* )>( dlsym( cf, "CFDictionarySetValue" ) );
  auto number = reinterpret_cast<void* ( * )( void*, long, const void* )>( dlsym( cf, "CFNumberCreate" ) );
  const void* key_callbacks = dlsym( cf, "kCFTypeDictionaryKeyCallBacks" );
  const void* value_callbacks = dlsym( cf, "kCFTypeDictionaryValueCallBacks" );
  const void* yes = *static_cast<const void* const*>( dlsym( cf, "kCFBooleanTrue" ) );
  auto key = [&]( const char* name ) { return *static_cast<const void* const*>( dlsym( io, name ) ); };
  auto create = reinterpret_cast<void* ( * )( void* )>( dlsym( io, "IOSurfaceCreate" ) );
  auto get_id = reinterpret_cast<uint32_t ( * )( void* )>( dlsym( io, "IOSurfaceGetID" ) );
  auto lock = reinterpret_cast<int ( * )( void*, uint32_t, uint32_t* )>( dlsym( io, "IOSurfaceLock" ) );
  auto base = reinterpret_cast<void* ( * )( void* )>( dlsym( io, "IOSurfaceGetBaseAddress" ) );
  auto stride = reinterpret_cast<size_t ( * )( void* )>( dlsym( io, "IOSurfaceGetBytesPerRow" ) );
  require( create_dict && set && number && create && get_id && lock && base && stride, "symbols" );
  void* props = create_dict( nullptr, 0, key_callbacks, value_callbacks );
  const int bytes = 4, format = 0x42475241; /* BGRA */
  set( props, key( "kIOSurfaceWidth" ), number( nullptr, 3, &width ) );
  set( props, key( "kIOSurfaceHeight" ), number( nullptr, 3, &height ) );
  set( props, key( "kIOSurfaceBytesPerElement" ), number( nullptr, 3, &bytes ) );
  set( props, key( "kIOSurfacePixelFormat" ), number( nullptr, 3, &format ) );
  set( props, key( "kIOSurfaceIsGlobal" ), yes );
  Surface s;
  s.ref = create( props );
  require( s.ref, "IOSurfaceCreate" );
  s.id = get_id( s.ref );
  lock( s.ref, 0, nullptr );
  s.base = static_cast<unsigned char*>( base( s.ref ) );
  s.stride = stride( s.ref );
  return s;
}

void iosurface()
{
  if ( !gpu_surfaces_supported() ) {
    std::cout << "IOSurface unavailable; skipped.\n";
    return;
  }
  Surface s = make_surface( 2, 1 );
  const unsigned char first[] = { 10, 20, 30, 255, 40, 50, 60, 128 }; /* B, G, R, A */
  std::copy( first, first + 8, s.base );
  Complete terminal( 80, 24 );
  const std::string name = base64( "iosurface:" + std::to_string( s.id ) );
  std::string reply = terminal.act( "\033_Ga=T,t=g,i=5;" + name + "\033\\" );
  require( reply.find( "OK" ) != std::string::npos, "a surface: " + reply );
  const KittyImage* image = terminal.get_fb().find_kitty_image( 5 );
  std::string rgba;
  require( image && image->width == 2 && image->height == 1 && kitty_image_to_rgba( *image, rgba )
             && rgba == std::string( "\x1e\x14\x0a\xff\x3c\x32\x28\x80", 8 ),
           "its pixels, in RGBA" );
  require( terminal.get_fb().get_kitty_placements().size() == 1, "placed" );

  refreshes( terminal, [&]( unsigned char value ) { s.base[6] = value; } ); /* R of the second pixel, in BGRA */
  frames_refused( terminal );

  /* The surface is held from the first t=g: the program may let go of it,
     and a=r still reads the same buffer. */
  void* cf = dlopen( "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY );
  void* io = dlopen( "/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_LAZY );
  auto release = reinterpret_cast<void ( * )( const void* )>( dlsym( cf, "CFRelease" ) );
  auto lookup = reinterpret_cast<void* ( * )( uint32_t )>( dlsym( io, "IOSurfaceLookup" ) );
  require( release && lookup, "symbols" );
  unsigned char* base = s.base;
  release( s.ref );
  base[6] = 77;
  reply = terminal.act( "\033_Ga=r,i=5\033\\" );
  require( reply.find( "OK" ) != std::string::npos && kitty_image_to_rgba( *terminal.get_fb().find_kitty_image( 5 ), rgba )
             && static_cast<unsigned char>( rgba[4] ) == 77,
           "refreshed from the surface it holds, after the program let go: " + reply );
  /* Deleting the image lets go of the surface, which then goes. */
  terminal.act( "\033_Ga=d,d=I,i=5,q=2\033\\" );
  void* gone = lookup( s.id );
  if ( gone ) {
    release( gone );
  }
  require( !gone, "the surface is let go with its image" );
}
#endif

#if defined( __linux__ )
/* A dma-buf made from memory with udmabuf, as Silkie's own tests make them. */
struct UdmabufCreate
{
  uint32_t memfd, flags;
  uint64_t offset, size;
};
const unsigned long UDMABUF_CREATE_REQUEST = _IOW( 'u', 0x42, UdmabufCreate );

void dmabuf()
{
  Complete terminal( 80, 24 );
  if ( !gpu_surfaces_supported() ) {
    const std::string reply = terminal.act( "\033_Ga=t,t=g,i=5,s=1,v=1;" + base64( "dmabuf:1:AB24:0:3,0,4" ) + "\033\\" );
    require( reply.find( "this kernel cannot" ) != std::string::npos, "an old kernel says why: " + reply );
    std::cout << "This kernel has no pidfd_getfd; dma-bufs are refused, as they should be.\n";
    return;
  }
  const int device = open( "/dev/udmabuf", O_RDWR | O_CLOEXEC );
  if ( device < 0 ) {
    std::cout << "No access to /dev/udmabuf; dma-buf reading skipped.\n";
    return;
  }
  const size_t page = size_t( sysconf( _SC_PAGESIZE ) );
  const int memfd = memfd_create( "skiff-test", MFD_ALLOW_SEALING );
  require( memfd >= 0 && ftruncate( memfd, off_t( page ) ) == 0 && fcntl( memfd, F_ADD_SEALS, F_SEAL_SHRINK ) == 0,
           "memfd" );
  unsigned char* memory
    = static_cast<unsigned char*>( mmap( nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0 ) );
  require( memory != MAP_FAILED, "mmap" );
  /* Two pixels of AB24: R, G, B, A in memory, on a stride of 64 bytes. */
  const unsigned char first[8] = { 10, 20, 30, 255, 40, 50, 60, 128 };
  std::memcpy( memory, first, 8 );
  UdmabufCreate create = { uint32_t( memfd ), 0, 0, page };
  const int buffer = ioctl( device, UDMABUF_CREATE_REQUEST, &create );
  require( buffer >= 0, std::string( "UDMABUF_CREATE: " ) + strerror( errno ) );
  set_gpu_surface_session( getsid( 0 ) );
  const std::string name = "dmabuf:" + std::to_string( getpid() ) + ":AB24:0:" + std::to_string( buffer ) + ",0,64";

  std::string reply = terminal.act( "\033_Ga=T,t=g,i=5,s=2,v=1;" + base64( name ) + "\033\\" );
  const KittyImage* image = terminal.get_fb().find_kitty_image( 5 );
  std::string rgba;
  require( reply.find( "OK" ) != std::string::npos && image && kitty_image_to_rgba( *image, rgba )
             && rgba == std::string( "\x0a\x14\x1e\xff\x28\x32\x3c\x80", 8 ),
           "a linear dma-buf: " + reply );
  refreshes( terminal, [&]( unsigned char value ) { memory[4] = value; } );

  /* Not in the session: refused. */
  set_gpu_surface_session( getsid( 0 ) + 1 );
  reply = terminal.act( "\033_Ga=t,t=g,i=6,s=2,v=1;" + base64( name ) + "\033\\" );
  require( reply.find( "not in the session" ) != std::string::npos, "another session's buffer: " + reply );
  set_gpu_surface_session( getsid( 0 ) );

  /* Bounds: a plane past the end of the buffer, and sizes that would wrap. */
  reply = terminal.act( "\033_Ga=t,t=g,i=6,s=2,v=1000;"
                        + base64( "dmabuf:" + std::to_string( getpid() ) + ":AB24:0:" + std::to_string( buffer ) + ",0,64" )
                        + "\033\\" );
  require( reply.find( "past the buffer" ) != std::string::npos, "a plane past the buffer: " + reply );
  reply = terminal.act( "\033_Ga=t,t=g,i=6,s=1048576,v=1;"
                        + base64( "dmabuf:" + std::to_string( getpid() ) + ":AB24:0:" + std::to_string( buffer )
                                  + ",8192,18446744073709547520" )
                        + "\033\\" );
  require( reply.find( "EINVAL" ) != std::string::npos, "a stride that wraps: " + reply );

  /* A layout the driver keeps to itself goes through GBM, when a driver here
     can import the buffer; either way the answer says what happened. */
  reply = terminal.act( "\033_Ga=t,t=g,i=7,s=2,v=1;"
                        + base64( "dmabuf:" + std::to_string( getpid() ) + ":AB24:0x00ffffffffffffff:"
                                  + std::to_string( buffer ) + ",0,64" )
                        + "\033\\" );
  const KittyImage* tiled = terminal.get_fb().find_kitty_image( 7 );
  if ( reply.find( "OK" ) != std::string::npos ) {
    require( tiled && kitty_image_to_rgba( *tiled, rgba ) && uint8_t( rgba[0] ) == 10, "read through GBM" );
    std::cout << "GBM read the dma-buf.\n";
  } else {
    require( reply.find( "GPU driver" ) != std::string::npos || reply.find( "libgbm" ) != std::string::npos,
             "a reason for GBM: " + reply );
    std::cout << "GBM could not read the dma-buf here: " << reply.substr( reply.find( ';' ) + 1 ) << "\n";
  }
  frames_refused( terminal );

  /* The descriptors are held from the first t=g: the program may close its
     own, and a=r still reads the same buffer. */
  close( buffer );
  memory[4] = 77;
  reply = terminal.act( "\033_Ga=r,i=5\033\\" );
  require( reply.find( "OK" ) != std::string::npos && kitty_image_to_rgba( *terminal.get_fb().find_kitty_image( 5 ), rgba )
             && static_cast<unsigned char>( rgba[4] ) == 77,
           "refreshed from the descriptors it holds, after the program closed its own: " + reply );
  const auto open_descriptors = []() {
    size_t count = 0;
    if ( DIR* dir = opendir( "/proc/self/fd" ) ) {
      while ( readdir( dir ) ) {
        count++;
      }
      closedir( dir );
    }
    return count;
  };
  const size_t held = open_descriptors();
  terminal.act( "\033_Ga=d,d=I,i=5,q=2\033\\" );
  require( open_descriptors() == held - 1, "and closed when the image is deleted" );
  munmap( memory, page );
  close( memfd );
  close( device );
}
#endif

/* A refresh with nothing new is not sent; one that changed a pixel is sent
   as that pixel, which the client writes over the image in place. */
template<class Change>
void refreshes( Complete& terminal, Change change )
{
  Complete client( 80, 24 );
  client.apply_string( terminal.diff_from( client ) );
  const uint64_t serial = terminal.get_fb().find_kitty_image( 5 )->serial;
  std::string reply = terminal.act( "\033_Ga=r,i=5\033\\" );
  require( reply.find( "OK" ) != std::string::npos && terminal.get_fb().find_kitty_image( 5 )->serial == serial,
           "an unchanged surface is not sent again" );
  change( 99 );
  reply = terminal.act( "\033_Ga=r,i=5\033\\" );
  const KittyImage* image = terminal.get_fb().find_kitty_image( 5 );
  std::string rgba;
  require( reply.find( "OK" ) != std::string::npos && kitty_image_to_rgba( *image, rgba )
             && static_cast<unsigned char>( rgba[4] ) == 99,
           "a refresh copies it again: " + reply );
  require( terminal.get_fb().get_kitty_placements().size() == 1, "and keeps its placement" );
  require( image->delta && image->delta->x == 1 && image->delta->y == 0 && image->delta->rect.width == 1
             && image->delta->rect.height == 1,
           "only the pixel that changed" );
  const Framebuffer shown = client.get_fb();
  const std::string diff = terminal.diff_from( client );
  client.apply_string( diff );
  const KittyImage* patched = client.get_fb().find_kitty_image( 5 );
  require( patched && kitty_image_to_rgba( *patched, rgba ) && static_cast<unsigned char>( rgba[4] ) == 99
             && client.get_fb().get_kitty_placements().size() == 1,
           "the client has the new pixels and the placement" );
  std::string out;
  append_kitty_frame( out, true, shown, client.get_fb() );
  require( out.find( "a=f,r=1,X=1" ) != std::string::npos && out.find( "a=t" ) == std::string::npos
             && out.find( "a=d" ) == std::string::npos,
           "and edits the image in its terminal in place: " + out );
}
}

int main()
{
  try {
    refusals();
#if defined( __APPLE__ )
    iosurface();
#endif
#if defined( __linux__ )
    dmabuf();
#endif
  } catch ( const std::exception& e ) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "GPU surfaces passed.\n";
  return 0;
}
