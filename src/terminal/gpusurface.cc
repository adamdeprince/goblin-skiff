/*
    Goblin Skiff: GPU surfaces (t=g) of the graphics protocol
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/gpusurface.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <unistd.h>

#if defined( __APPLE__ )
#include <dlfcn.h>
#elif defined( __linux__ )
#include <climits>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

namespace Terminal {

static pid_t surface_session = 0;

void set_gpu_surface_session( pid_t session )
{
  surface_session = session;
}

[[maybe_unused]] static bool parse_unsigned( const std::string& text, uint64_t& out, int base = 10 )
{
  if ( text.empty() ) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = strtoull( text.c_str(), &end, base );
  if ( errno || *end || text[0] == '-' || text[0] == '+' ) {
    return false;
  }
  out = value;
  return true;
}

/* Copies rows of 32 bit pixels into straight RGBA; order gives the byte of
   each of R, G, B and A in a source pixel, a negative A meaning opaque. */
[[maybe_unused]] static void copy_pixels( const unsigned char* base,
                         size_t stride,
                         uint32_t width,
                         uint32_t height,
                         const int order[4],
                         std::string& rgba )
{
  rgba.resize( size_t( width ) * height * 4 );
  for ( uint32_t y = 0; y < height; y++ ) {
    const unsigned char* row = base + size_t( y ) * stride;
    char* out = &rgba[size_t( y ) * width * 4];
    for ( size_t x = 0; x < width; x++ ) {
      const unsigned char* px = row + x * 4;
      out[x * 4] = px[order[0]];
      out[x * 4 + 1] = px[order[1]];
      out[x * 4 + 2] = px[order[2]];
      out[x * 4 + 3] = order[3] < 0 ? char( 0xff ) : px[order[3]];
    }
  }
}

bool read_gpu_surface( const std::string& name,
                       uint32_t width,
                       uint32_t height,
                       std::string& rgba,
                       uint32_t& out_width,
                       uint32_t& out_height,
                       std::string& error )
{
  const std::shared_ptr<GpuSurface> surface = open_gpu_surface( name, width, height, error );
  return surface && read_gpu_surface( *surface, rgba, out_width, out_height, error );
}

#if defined( __APPLE__ )

/* IOSurface is looked up at run time, so nothing links against it. */
namespace {
typedef void* SurfaceRef;
struct IOSurfaceAPI
{
  SurfaceRef ( *lookup )( uint32_t ) = nullptr;
  int ( *lock )( SurfaceRef, uint32_t, uint32_t* ) = nullptr;
  int ( *unlock )( SurfaceRef, uint32_t, uint32_t* ) = nullptr;
  void* ( *base )( SurfaceRef ) = nullptr;
  size_t ( *width )( SurfaceRef ) = nullptr;
  size_t ( *height )( SurfaceRef ) = nullptr;
  size_t ( *stride )( SurfaceRef ) = nullptr;
  uint32_t ( *format )( SurfaceRef ) = nullptr;
  void ( *release )( const void* ) = nullptr;
  bool loaded = false;
};

const IOSurfaceAPI& iosurface()
{
  static IOSurfaceAPI api;
  static bool tried = false;
  if ( tried ) {
    return api;
  }
  tried = true;
  void* surface = dlopen( "/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_LAZY );
  void* foundation = dlopen( "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY );
  if ( !surface || !foundation ) {
    return api;
  }
  api.lookup = reinterpret_cast<decltype( api.lookup )>( dlsym( surface, "IOSurfaceLookup" ) );
  api.lock = reinterpret_cast<decltype( api.lock )>( dlsym( surface, "IOSurfaceLock" ) );
  api.unlock = reinterpret_cast<decltype( api.unlock )>( dlsym( surface, "IOSurfaceUnlock" ) );
  api.base = reinterpret_cast<decltype( api.base )>( dlsym( surface, "IOSurfaceGetBaseAddress" ) );
  api.width = reinterpret_cast<decltype( api.width )>( dlsym( surface, "IOSurfaceGetWidth" ) );
  api.height = reinterpret_cast<decltype( api.height )>( dlsym( surface, "IOSurfaceGetHeight" ) );
  api.stride = reinterpret_cast<decltype( api.stride )>( dlsym( surface, "IOSurfaceGetBytesPerRow" ) );
  api.format = reinterpret_cast<decltype( api.format )>( dlsym( surface, "IOSurfaceGetPixelFormat" ) );
  api.release = reinterpret_cast<decltype( api.release )>( dlsym( foundation, "CFRelease" ) );
  api.loaded = api.lookup && api.lock && api.unlock && api.base && api.width && api.height && api.stride
               && api.format && api.release;
  return api;
}

const uint32_t READ_ONLY = 1; /* kIOSurfaceLockReadOnly */
const uint32_t FORMAT_BGRA = 0x42475241, FORMAT_RGBA = 0x52474241;
}

bool gpu_surfaces_supported()
{
  return iosurface().loaded;
}

/* A reference to the IOSurface, taken by IOSurfaceLookup. */
struct GpuSurface::Impl
{
  SurfaceRef surface = nullptr;
};

GpuSurface::GpuSurface( Impl* held ) : impl( held ) {}

GpuSurface::~GpuSurface()
{
  release();
}

void GpuSurface::release()
{
  if ( impl ) {
    if ( impl->surface ) {
      iosurface().release( impl->surface );
    }
    delete impl;
    impl = nullptr;
  }
}

std::shared_ptr<GpuSurface> open_gpu_surface( const std::string& name, uint32_t, uint32_t, std::string& error )
{
  const IOSurfaceAPI& api = iosurface();
  uint64_t id = 0;
  if ( name.compare( 0, 10, "iosurface:" ) != 0 || !parse_unsigned( name.substr( 10 ), id ) || id > UINT32_MAX ) {
    error = "EINVAL:not an IOSurface";
    return nullptr;
  }
  if ( !api.loaded ) {
    error = "EINVAL:IOSurface is not available";
    return nullptr;
  }
  SurfaceRef surface = api.lookup( uint32_t( id ) );
  if ( !surface ) {
    error = "EINVAL:no such IOSurface, or it is not global";
    return nullptr;
  }
  const uint32_t format = api.format( surface );
  if ( format != FORMAT_BGRA && format != FORMAT_RGBA ) {
    api.release( surface );
    error = "EINVAL:the IOSurface is not 32 bit BGRA or RGBA";
    return nullptr;
  }
  GpuSurface::Impl* held = new ( std::nothrow ) GpuSurface::Impl;
  if ( !held ) {
    api.release( surface );
    error = "ENOMEM:out of memory";
    return nullptr;
  }
  held->surface = surface;
  return std::make_shared<GpuSurface>( held );
}

bool read_gpu_surface( const GpuSurface& handle, std::string& rgba, uint32_t& out_width, uint32_t& out_height,
                       std::string& error )
{
  const IOSurfaceAPI& api = iosurface();
  if ( !handle.held() || !api.loaded ) {
    error = "EINVAL:the GPU surface was let go";
    return false;
  }
  SurfaceRef surface = handle.get()->surface;
  const uint32_t format = api.format( surface );
  static const int bgra[4] = { 2, 1, 0, 3 }, plain[4] = { 0, 1, 2, 3 };
  bool ok = false;
  if ( api.lock( surface, READ_ONLY, nullptr ) != 0 ) {
    error = "EINVAL:the IOSurface cannot be locked";
  } else {
    out_width = uint32_t( api.width( surface ) );
    out_height = uint32_t( api.height( surface ) );
    const unsigned char* base = static_cast<const unsigned char*>( api.base( surface ) );
    if ( base && out_width && out_height ) {
      copy_pixels( base, api.stride( surface ), out_width, out_height, format == FORMAT_BGRA ? bgra : plain, rgba );
      ok = true;
    } else {
      error = "EINVAL:the IOSurface is empty";
    }
    api.unlock( surface, READ_ONLY, nullptr );
  }
  return ok;
}

#elif defined( __linux__ )

static std::vector<std::string> split( const std::string& text, char separator )
{
  std::vector<std::string> out;
  size_t start = 0;
  while ( true ) {
    const size_t end = text.find( separator, start );
    out.push_back( text.substr( start, end == std::string::npos ? std::string::npos : end - start ) );
    if ( end == std::string::npos ) {
      return out;
    }
    start = end + 1;
  }
}

namespace {
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
struct DmaBufSync
{
  uint64_t flags;
};
const uint64_t SYNC_READ = 1, SYNC_START = 0, SYNC_END = 4;
const unsigned long DMA_BUF_IOCTL_SYNC_REQUEST = _IOW( 'b', 0, DmaBufSync );
const uint64_t MODIFIER_LINEAR = 0;

struct Descriptor
{
  int fd = -1;
  Descriptor() = default;
  Descriptor( const Descriptor& ) = delete;
  Descriptor& operator=( const Descriptor& ) = delete;
  Descriptor( Descriptor&& other ) noexcept : fd( other.fd ) { other.fd = -1; }
  ~Descriptor()
  {
    if ( fd >= 0 ) {
      close( fd );
    }
  }
};

struct Plane
{
  Descriptor fd;
  uint64_t offset = 0, stride = 0;
};

bool is_dmabuf( int fd )
{
  char link[PATH_MAX];
  const std::string path = "/proc/self/fd/" + std::to_string( fd );
  const ssize_t n = readlink( path.c_str(), link, sizeof link - 1 );
  if ( n <= 0 ) {
    return false;
  }
  link[n] = 0;
  return strncmp( link, "anon_inode:dmabuf", 17 ) == 0 || strncmp( link, "/dmabuf:", 8 ) == 0;
}

/* GBM, looked up at run time: through it the GPU driver lays out for the
   CPU a buffer in whatever tiling it has. Nothing links against it. */
struct GbmImport
{
  uint32_t width, height, format, num_fds;
  int fds[4];
  int strides[4];
  int offsets[4];
  uint64_t modifier;
};
struct GbmImportFd /* the older import, of one plane in the driver's own layout */
{
  int fd;
  uint32_t width, height, stride, format;
};
const uint32_t GBM_BO_IMPORT_FD = 0x5503;
const uint32_t GBM_BO_IMPORT_FD_MODIFIER = 0x5504;
const uint32_t GBM_BO_USE_RENDERING = 1 << 2;
const uint32_t GBM_BO_TRANSFER_READ = 1 << 0;

struct GbmAPI
{
  void* ( *create_device )( int ) = nullptr;
  void ( *device_destroy )( void* ) = nullptr;
  void* ( *bo_import )( void*, uint32_t, void*, uint32_t ) = nullptr;
  void* ( *bo_map )( void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void** ) = nullptr;
  void ( *bo_unmap )( void*, void* ) = nullptr;
  void ( *bo_destroy )( void* ) = nullptr;
  bool loaded = false;
};

const GbmAPI& gbm()
{
  static GbmAPI api;
  static bool tried = false;
  if ( tried ) {
    return api;
  }
  tried = true;
  void* library = dlopen( "libgbm.so.1", RTLD_LAZY | RTLD_LOCAL );
  if ( !library ) {
    return api;
  }
  api.create_device = reinterpret_cast<decltype( api.create_device )>( dlsym( library, "gbm_create_device" ) );
  api.device_destroy = reinterpret_cast<decltype( api.device_destroy )>( dlsym( library, "gbm_device_destroy" ) );
  api.bo_import = reinterpret_cast<decltype( api.bo_import )>( dlsym( library, "gbm_bo_import" ) );
  api.bo_map = reinterpret_cast<decltype( api.bo_map )>( dlsym( library, "gbm_bo_map" ) );
  api.bo_unmap = reinterpret_cast<decltype( api.bo_unmap )>( dlsym( library, "gbm_bo_unmap" ) );
  api.bo_destroy = reinterpret_cast<decltype( api.bo_destroy )>( dlsym( library, "gbm_bo_destroy" ) );
  api.loaded = api.create_device && api.device_destroy && api.bo_import && api.bo_map && api.bo_unmap && api.bo_destroy;
  return api;
}

uint32_t fourcc( const std::string& code )
{
  return uint32_t( uint8_t( code[0] ) ) | uint32_t( uint8_t( code[1] ) ) << 8 | uint32_t( uint8_t( code[2] ) ) << 16
         | uint32_t( uint8_t( code[3] ) ) << 24;
}

/* Through the first render node whose driver takes the buffer. */
bool read_through_gbm( const std::vector<Plane>& planes,
                       const std::string& format,
                       uint64_t modifier,
                       uint32_t width,
                       uint32_t height,
                       const int order[4],
                       std::string& rgba,
                       std::string& error )
{
  const GbmAPI& api = gbm();
  if ( !api.loaded ) {
    error = "EINVAL:a tiled dma-buf needs libgbm, which is not installed";
    return false;
  }
  GbmImport import = {};
  import.width = width;
  import.height = height;
  import.format = fourcc( format );
  import.num_fds = uint32_t( planes.size() );
  import.modifier = modifier;
  for ( size_t i = 0; i < planes.size(); i++ ) {
    if ( planes[i].offset > INT_MAX || planes[i].stride > INT_MAX ) {
      error = "EINVAL:bad dma-buf plane";
      return false;
    }
    import.fds[i] = planes[i].fd.fd;
    import.strides[i] = int( planes[i].stride );
    import.offsets[i] = int( planes[i].offset );
  }
  GbmImportFd single = {};
  single.fd = planes[0].fd.fd;
  single.width = width;
  single.height = height;
  single.stride = uint32_t( planes[0].stride );
  single.format = import.format;
  /* The backend the system chose first, then Mesa's: some drivers' own
     backends import a buffer but cannot map it for the CPU. */
  const char* chosen = getenv( "GBM_BACKEND" );
  const std::string previous = chosen ? chosen : "";
  for ( const char* backend : { static_cast<const char*>( nullptr ), "dri" } ) {
    if ( backend ) {
      setenv( "GBM_BACKEND", backend, 1 );
    }
    bool ok = false;
    for ( int node = 128; node < 192 && !ok; node++ ) {
      Descriptor device;
      device.fd = open( ( "/dev/dri/renderD" + std::to_string( node ) ).c_str(), O_RDWR | O_CLOEXEC );
      if ( device.fd < 0 ) {
        continue;
      }
      void* gbm_device = api.create_device( device.fd );
      if ( !gbm_device ) {
        continue;
      }
      void* bo = api.bo_import( gbm_device, GBM_BO_IMPORT_FD_MODIFIER, &import, GBM_BO_USE_RENDERING );
      if ( !bo && planes.size() == 1 ) {
        bo = api.bo_import( gbm_device, GBM_BO_IMPORT_FD, &single, GBM_BO_USE_RENDERING );
      }
      if ( bo ) {
        uint32_t stride = 0;
        void* map_data = nullptr;
        void* map = api.bo_map( bo, 0, 0, width, height, GBM_BO_TRANSFER_READ, &stride, &map_data );
        if ( map && map != reinterpret_cast<void*>( -1 ) && stride >= uint64_t( width ) * 4 ) {
          copy_pixels( static_cast<const unsigned char*>( map ), stride, width, height, order, rgba );
          api.bo_unmap( bo, map_data );
          ok = true;
        }
        api.bo_destroy( bo );
      }
      api.device_destroy( gbm_device );
    }
    if ( backend ) {
      if ( chosen ) {
        setenv( "GBM_BACKEND", previous.c_str(), 1 );
      } else {
        unsetenv( "GBM_BACKEND" );
      }
    }
    if ( ok ) {
      return true;
    }
  }
  error = "EINVAL:no GPU driver here could read the dma-buf";
  return false;
}
}

bool gpu_surfaces_supported()
{
  /* A relay takes a program's buffers with pidfd_getfd, which kernels
     before 5.6 do not have. */
  static int supported = -1;
  if ( supported < 0 ) {
    errno = 0;
    const long probe = syscall( SYS_pidfd_getfd, -1, -1, 0 );
    supported = !( probe < 0 && errno == ENOSYS );
    errno = 0;
    const long pidfd = syscall( SYS_pidfd_open, getpid(), 0 );
    if ( pidfd < 0 && errno == ENOSYS ) {
      supported = 0;
    } else if ( pidfd >= 0 ) {
      close( int( pidfd ) );
    }
  }
  return supported != 0;
}

/* The descriptors taken from the program, and how to read them. */
struct GpuSurface::Impl
{
  std::vector<Plane> planes;
  std::string format;
  const int* order = nullptr;
  uint64_t modifier = 0;
  uint32_t width = 0, height = 0;
};

GpuSurface::GpuSurface( Impl* held ) : impl( held ) {}

GpuSurface::~GpuSurface()
{
  release();
}

void GpuSurface::release()
{
  delete impl; /* the descriptors close with their planes */
  impl = nullptr;
}

std::shared_ptr<GpuSurface> open_gpu_surface( const std::string& name, uint32_t width, uint32_t height,
                                              std::string& error )
{
  /* dmabuf:<pid>:<fourcc>:<modifier>:<fd>,<offset>,<stride>[:...] */
  const auto parts = split( name, ':' );
  uint64_t pid = 0, modifier = 0;
  if ( parts.size() < 5 || parts.size() > 8 || parts[0] != "dmabuf" || !parse_unsigned( parts[1], pid )
       || parts[2].size() != 4 || !parse_unsigned( parts[3], modifier, 0 ) ) {
    error = "EINVAL:not a dma-buf";
    return nullptr;
  }
  if ( !gpu_surfaces_supported() ) {
    error = "EINVAL:this kernel cannot hand a relay another process's buffers";
    return nullptr;
  }
  if ( !width || !height ) {
    error = "EINVAL:s and v are required for a dma-buf";
    return nullptr;
  }
  static const int ab24[4] = { 0, 1, 2, 3 }, ar24[4] = { 2, 1, 0, 3 }, xb24[4] = { 0, 1, 2, -1 },
                   xr24[4] = { 2, 1, 0, -1 };
  const int* order = parts[2] == "AB24"   ? ab24
                     : parts[2] == "AR24" ? ar24
                     : parts[2] == "XB24" ? xb24
                     : parts[2] == "XR24" ? xr24
                                          : nullptr;
  if ( !order ) {
    error = "EINVAL:unsupported dma-buf format";
    return nullptr;
  }
  /* Everything that is read lies inside the buffer, and no size wraps. */
  uint64_t pixels = 0;
  if ( __builtin_mul_overflow( uint64_t( width ) * 4, uint64_t( height ), &pixels )
       || pixels > std::string().max_size() ) {
    error = "EINVAL:the dma-buf does not fit in memory";
    return nullptr;
  }
  std::unique_ptr<GpuSurface::Impl> held( new ( std::nothrow ) GpuSurface::Impl );
  if ( !held ) {
    error = "ENOMEM:out of memory";
    return nullptr;
  }
  std::vector<uint64_t> targets;
  for ( size_t i = 4; i < parts.size(); i++ ) {
    const auto plane = split( parts[i], ',' );
    uint64_t target = 0;
    Plane p;
    if ( plane.size() != 3 || !parse_unsigned( plane[0], target ) || !parse_unsigned( plane[1], p.offset )
         || !parse_unsigned( plane[2], p.stride ) || target > INT_MAX ) {
      error = "EINVAL:bad dma-buf plane";
      return nullptr;
    }
    targets.push_back( target );
    held->planes.push_back( std::move( p ) );
  }
  /* The process is pinned before its session is checked, and checked to be
     alive after, so a pid that is reused in between is not mistaken. Only
     the program's own session may name its buffers. */
  Descriptor pidfd;
  pidfd.fd = pid > INT_MAX ? -1 : int( syscall( SYS_pidfd_open, pid_t( pid ), 0 ) );
  if ( pidfd.fd < 0 ) {
    error = std::string( "EINVAL:pidfd_open: " ) + strerror( errno );
    return nullptr;
  }
  if ( !surface_session || getsid( pid_t( pid ) ) != surface_session
       || syscall( SYS_pidfd_send_signal, pidfd.fd, 0, nullptr, 0 ) != 0 ) {
    error = "EINVAL:the process is not in the session of this window";
    return nullptr;
  }
  for ( size_t i = 0; i < held->planes.size(); i++ ) {
    held->planes[i].fd.fd = int( syscall( SYS_pidfd_getfd, pidfd.fd, int( targets[i] ), 0 ) );
    if ( held->planes[i].fd.fd < 0 ) {
      error = std::string( "EINVAL:pidfd_getfd: " ) + strerror( errno );
      return nullptr;
    }
    if ( !is_dmabuf( held->planes[i].fd.fd ) ) {
      error = "EINVAL:the descriptor is not a dma-buf";
      return nullptr;
    }
  }
  held->format = parts[2];
  held->order = order;
  held->modifier = modifier;
  held->width = width;
  held->height = height;
  return std::make_shared<GpuSurface>( held.release() );
}

bool read_gpu_surface( const GpuSurface& handle, std::string& rgba, uint32_t& out_width, uint32_t& out_height,
                       std::string& error )
{
  if ( !handle.held() ) {
    error = "EINVAL:the GPU surface was let go";
    return false;
  }
  const GpuSurface::Impl& surface = *handle.get();
  const uint32_t width = surface.width, height = surface.height;
  if ( surface.modifier != MODIFIER_LINEAR || surface.planes.size() != 1 ) {
    /* Tiled, or of a layout the driver keeps to itself: the driver reads it. */
    if ( !read_through_gbm( surface.planes, surface.format, surface.modifier, width, height, surface.order, rgba,
                            error ) ) {
      return false;
    }
    out_width = width;
    out_height = height;
    return true;
  }
  /* Linear: read straight from the buffer. */
  const Plane& plane = surface.planes[0];
  uint64_t rows = 0, length = 0;
  if ( plane.stride < uint64_t( width ) * 4 || __builtin_mul_overflow( plane.stride, uint64_t( height ), &rows )
       || __builtin_add_overflow( rows, plane.offset, &length ) || length > SIZE_MAX ) {
    error = "EINVAL:bad dma-buf plane";
    return false;
  }
  const off_t size = lseek( plane.fd.fd, 0, SEEK_END );
  if ( size < 0 || uint64_t( size ) < length ) {
    error = "EINVAL:the dma-buf plane reaches past the buffer";
    return false;
  }
  void* map = mmap( nullptr, length, PROT_READ, MAP_SHARED, plane.fd.fd, 0 );
  if ( map == MAP_FAILED ) {
    error = std::string( "EINVAL:mmap: " ) + strerror( errno );
    return false;
  }
  DmaBufSync sync = { SYNC_START | SYNC_READ };
  ioctl( plane.fd.fd, DMA_BUF_IOCTL_SYNC_REQUEST, &sync );
  copy_pixels( static_cast<const unsigned char*>( map ) + plane.offset, plane.stride, width, height, surface.order,
               rgba );
  sync.flags = SYNC_END | SYNC_READ;
  ioctl( plane.fd.fd, DMA_BUF_IOCTL_SYNC_REQUEST, &sync );
  munmap( map, length );
  out_width = width;
  out_height = height;
  return true;
}

#else

bool gpu_surfaces_supported()
{
  return false;
}

struct GpuSurface::Impl
{
};

GpuSurface::GpuSurface( Impl* held ) : impl( held ) {}

GpuSurface::~GpuSurface()
{
  release();
}

void GpuSurface::release()
{
  delete impl;
  impl = nullptr;
}

std::shared_ptr<GpuSurface> open_gpu_surface( const std::string&, uint32_t, uint32_t, std::string& error )
{
  error = "EINVAL:GPU surfaces are not supported on this platform";
  return nullptr;
}

bool read_gpu_surface( const GpuSurface&, std::string&, uint32_t&, uint32_t&, std::string& error )
{
  error = "EINVAL:GPU surfaces are not supported on this platform";
  return false;
}

#endif

}
