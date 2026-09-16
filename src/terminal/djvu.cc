/* Distributed under the GNU GPL, version 3 or later. */
#include "src/include/config.h"
#include "djvu.h"
#include "kittygraphics.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libdjvu/ddjvuapi.h>

namespace Terminal {
namespace {
unsigned plane_count( size_t colors )
{
  unsigned count = 1;
  while ( ( size_t( 1 ) << count ) < colors ) { count++; }
  return count;
}

bool dimensions_ok( uint32_t width, uint32_t height )
{
  return width && height && width <= GRAPHICS_MAX_DIMENSION && height <= GRAPHICS_MAX_DIMENSION
         && uint64_t( width ) * height <= GRAPHICS_MAX_PIXELS;
}

// cjb2 needs seekable input/output. Anonymous temporary files avoid named
// image files, shell commands, and pipe deadlocks. Bound both runtime and size.
bool cjb2_encode( const std::string& pbm, bool lossy, std::string& result )
{
  using File = std::unique_ptr<FILE, decltype( &fclose )>;
  File input( tmpfile(), fclose ), output( tmpfile(), fclose );
  if ( !input || !output || fwrite( pbm.data(), 1, pbm.size(), input.get() ) != pbm.size()
       || fflush( input.get() ) || fseek( input.get(), 0, SEEK_SET ) ) {
    return false;
  }
  const int input_fd = fileno( input.get() ), output_fd = fileno( output.get() );
  const int null_fd = open( "/dev/null", O_WRONLY );
  if ( null_fd < 0 ) { return false; }
  const pid_t child = fork();
  if ( child == 0 ) {
    const struct rlimit cpu = { 5, 5 }, file = { KITTY_ENCODED_QUOTA, KITTY_ENCODED_QUOTA }, core = { 0, 0 };
    sigset_t mask;
    sigemptyset( &mask );
    sigprocmask( SIG_SETMASK, &mask, NULL );
    signal( SIGTERM, SIG_DFL );
    signal( SIGINT, SIG_DFL );
    signal( SIGPIPE, SIG_DFL );
    if ( setrlimit( RLIMIT_CPU, &cpu ) || setrlimit( RLIMIT_FSIZE, &file ) || setrlimit( RLIMIT_CORE, &core )
         || dup2( input_fd, STDIN_FILENO ) < 0 || dup2( output_fd, STDOUT_FILENO ) < 0
         || dup2( null_fd, STDERR_FILENO ) < 0 ) {
      _exit( 127 );
    }
    if ( input_fd > STDERR_FILENO ) { close( input_fd ); }
    if ( output_fd > STDERR_FILENO ) { close( output_fd ); }
    if ( null_fd > STDERR_FILENO ) { close( null_fd ); }
    execl( CJB2_PATH, "cjb2", lossy ? "-lossy" : "-lossless", "-", "-", static_cast<char*>( NULL ) );
    _exit( 127 );
  }
  close( null_fd );
  if ( child < 0 ) { return false; }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
  int status = 0;
  while ( true ) {
    const pid_t done = waitpid( child, &status, WNOHANG );
    if ( done == child ) { break; }
    if ( done < 0 && errno != EINTR ) { return false; }
    if ( std::chrono::steady_clock::now() >= deadline ) {
      kill( child, SIGKILL );
      while ( waitpid( child, &status, 0 ) < 0 && errno == EINTR ) {}
      return false;
    }
    poll( NULL, 0, 1 );
  }
  struct stat st;
  if ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 || fstat( output_fd, &st ) < 0 || st.st_size <= 0
       || uint64_t( st.st_size ) > KITTY_ENCODED_QUOTA || fseek( output.get(), 0, SEEK_SET ) ) {
    return false;
  }
  result.resize( st.st_size );
  return fread( &result[0], 1, result.size(), output.get() ) == result.size();
}

uint32_t read_be32( const char* p )
{
  const auto* bytes = reinterpret_cast<const unsigned char*>( p );
  return uint32_t( bytes[0] ) << 24 | uint32_t( bytes[1] ) << 16 | uint32_t( bytes[2] ) << 8 | bytes[3];
}

// Accept only the standalone INFO + Sjbz file emitted by cjb2. Validate the
// dimensions before asking the decoder to allocate pixels; reject references,
// annotations, extra pages, truncated chunks, and other DjVu image layers.
bool check_djvu( const std::string& data, uint32_t width, uint32_t height )
{
  if ( data.size() < 42 || data.size() > KITTY_ENCODED_QUOTA || data.compare( 0, 8, "AT&TFORM" )
       || read_be32( data.data() + 8 ) != data.size() - 12 || data.compare( 12, 4, "DJVU" )
       || data.compare( 16, 4, "INFO" ) || read_be32( data.data() + 20 ) != 10 ) {
    return false;
  }
  const auto* info = reinterpret_cast<const unsigned char*>( data.data() + 24 );
  if ( ( uint32_t( info[0] ) << 8 | info[1] ) != width
       || ( uint32_t( info[2] ) << 8 | info[3] ) != height
       || data.compare( 34, 4, "Sjbz" ) || read_be32( data.data() + 38 ) != data.size() - 42 ) {
    return false;
  }
  return true;
}

bool wait_for_job( ddjvu_context_t* context, ddjvu_job_t* job )
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
  while ( true ) {
    while ( const ddjvu_message_t* message = ddjvu_message_peek( context ) ) {
      const bool error = message->m_any.tag == DDJVU_ERROR
                         || ( message->m_any.tag == DDJVU_NEWSTREAM && message->m_newstream.streamid != 0 );
      ddjvu_message_pop( context );
      if ( error ) { ddjvu_job_stop( job ); return false; }
    }
    const ddjvu_status_t status = ddjvu_job_status( job );
    if ( status >= DDJVU_JOB_OK ) { return status == DDJVU_JOB_OK; }
    if ( std::chrono::steady_clock::now() >= deadline ) { ddjvu_job_stop( job ); return false; }
    poll( NULL, 0, 1 );
  }
}
}

bool encode_palette_djvu( const unsigned char* pixels, uint32_t width, uint32_t height, bool alpha, bool lossy,
                          std::string& palette, std::string& djvu )
{
  palette.clear();
  djvu.clear();
  if ( !pixels || !dimensions_ok( width, height ) ) { return false; }
  std::array<uint32_t, GRAPHICS_MAX_PALETTE> colors = {};
  size_t count = 0;
  const size_t pixel_count = size_t( width ) * height;
  std::string indices( pixel_count, '\0' );
  for ( size_t i = 0; i < pixel_count; i++ ) {
    const unsigned char* p = pixels + i * ( alpha ? 4 : 3 );
    const uint32_t color = uint32_t( p[0] ) << 24 | uint32_t( p[1] ) << 16 | uint32_t( p[2] ) << 8
                           | ( alpha ? p[3] : 255 );
    size_t index = 0;
    while ( index < count && colors[index] != color ) { index++; }
    if ( index == count ) {
      if ( count == colors.size() ) { palette.clear(); return false; }
      colors[count++] = color;
      palette.append( reinterpret_cast<const char*>( p ), 3 );
      palette.push_back( static_cast<char>( alpha ? p[3] : 255 ) );
    }
    indices[i] = static_cast<char>( index );
  }
  const unsigned planes = plane_count( count );
  const size_t stride = ( width + 7 ) / 8;
  std::string pbm = "P4\n" + std::to_string( width ) + " " + std::to_string( height * planes ) + "\n";
  const size_t offset = pbm.size();
  pbm.resize( offset + stride * height * planes, '\0' );
  for ( unsigned plane = 0; plane < planes; plane++ ) {
    for ( uint32_t y = 0; y < height; y++ ) {
      for ( uint32_t x = 0; x < width; x++ ) {
        if ( ( static_cast<unsigned char>( indices[size_t( y ) * width + x] ) >> plane ) & 1 ) {
          pbm[offset + ( size_t( plane ) * height + y ) * stride + x / 8] |= char( 0x80 >> ( x % 8 ) );
        }
      }
    }
  }
  // Lossy substitution on multiple index planes could invent palette indices.
  // Restrict the opt-in to true two-color images (including their alpha).
  return cjb2_encode( pbm, lossy && count == 2, djvu ) && check_djvu( djvu, width, height * planes );
}

bool decode_palette_djvu( const std::string& palette, const std::string& djvu,
                          uint32_t width, uint32_t height, std::string& rgba )
{
  rgba.clear();
  if ( !dimensions_ok( width, height ) || palette.empty() || palette.size() % 4
       || palette.size() > GRAPHICS_MAX_PALETTE * 4 ) { return false; }
  const size_t colors = palette.size() / 4;
  const unsigned planes = plane_count( colors );
  const uint32_t page_height = height * planes;
  if ( !check_djvu( djvu, width, page_height ) ) { return false; }

  const auto release_document = []( ddjvu_document_t* p ) { ddjvu_document_release( p ); };
  const auto release_page = []( ddjvu_page_t* p ) { ddjvu_page_release( p ); };
  std::unique_ptr<ddjvu_context_t, decltype( &ddjvu_context_release )> context(
    ddjvu_context_create( "goblin-mosh" ), ddjvu_context_release );
  if ( !context ) { return false; }
  std::unique_ptr<ddjvu_document_t, decltype( release_document )> document(
    ddjvu_document_create( context.get(), NULL, false ), release_document );
  if ( !document ) { return false; }
  ddjvu_stream_write( document.get(), 0, djvu.data(), djvu.size() );
  ddjvu_stream_close( document.get(), 0, false );
  if ( !wait_for_job( context.get(), ddjvu_document_job( document.get() ) )
       || ddjvu_document_get_pagenum( document.get() ) != 1 ) { return false; }
  std::unique_ptr<ddjvu_page_t, decltype( release_page )> page(
    ddjvu_page_create_by_pageno( document.get(), 0 ), release_page );
  if ( !page || !wait_for_job( context.get(), ddjvu_page_job( page.get() ) )
       || ddjvu_page_get_width( page.get() ) != int( width )
       || ddjvu_page_get_height( page.get() ) != int( page_height ) ) { return false; }
  std::unique_ptr<ddjvu_format_t, decltype( &ddjvu_format_release )> format(
    ddjvu_format_create( DDJVU_FORMAT_MSBTOLSB, 0, NULL ), ddjvu_format_release );
  if ( !format ) { return false; }
  ddjvu_format_set_row_order( format.get(), true );
  ddjvu_format_set_y_direction( format.get(), true );
  const ddjvu_rect_t rect = { 0, 0, width, page_height };
  const size_t stride = ( width + 7 ) / 8;
  std::string bits( stride * page_height, '\0' );
  if ( !ddjvu_page_render( page.get(), DDJVU_RENDER_MASKONLY, &rect, &rect, format.get(), stride, &bits[0] ) ) {
    return false;
  }
  std::string pixels( size_t( width ) * height * 4, '\0' );
  for ( uint32_t y = 0; y < height; y++ ) {
    for ( uint32_t x = 0; x < width; x++ ) {
      size_t index = 0;
      for ( unsigned plane = 0; plane < planes; plane++ ) {
        const unsigned char b = bits[( size_t( plane ) * height + y ) * stride + x / 8];
        index |= size_t( ( b >> ( 7 - x % 8 ) ) & 1 ) << plane;
      }
      if ( index >= colors ) { return false; }
      memcpy( &pixels[( size_t( y ) * width + x ) * 4], palette.data() + index * 4, 4 );
    }
  }
  rgba.swap( pixels );
  return true;
}
}
