/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <png.h>
#include <zlib.h>

#include "src/protobufs/hostinput.pb.h"
#include "src/protobufs/kitty.pb.h"
#include "src/statesync/completeterminal.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/osc52.h"
#include "src/terminal/terminaldisplay.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

std::string rgb_cell( unsigned char r, unsigned char g, unsigned char b )
{
  std::string raw;
  raw.push_back( static_cast<char>( r ) );
  raw.push_back( static_cast<char>( g ) );
  raw.push_back( static_cast<char>( b ) );
  return raw;
}

std::string transmit_rgb( uint32_t id )
{
  const std::string b64 = Terminal::base64_encode_osc52( rgb_cell( 255, 0, 0 ) );
  return std::string( "\033_Ga=T,f=24,s=1,v=1,i=" ) + std::to_string( id ) + ";" + b64 + "\033\\";
}

std::string transmit_held_rgb( uint32_t id, unsigned char r, unsigned char g, unsigned char b )
{
  const std::string b64 = Terminal::base64_encode_osc52( rgb_cell( r, g, b ) );
  return std::string( "\033_Ga=T,C=1,f=24,s=1,v=1,i=" ) + std::to_string( id ) + ";" + b64 + "\033\\";
}

std::string rgba_png( unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                      unsigned width = 1, unsigned height = 1 )
{
  png_image image;
  memset( &image, 0, sizeof( image ) );
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_RGBA;
  std::string pixels;
  for ( size_t i = 0; i < static_cast<size_t>( width ) * height; i++ ) {
    pixels += rgb_cell( r, g, b );
    pixels.push_back( static_cast<char>( a ) );
  }

  png_alloc_size_t size = 0;
  require( png_image_write_to_memory( &image, NULL, &size, 0, pixels.data(), 0, NULL ) != 0, "measure PNG output" );
  std::string png( size, '\0' );
  require( png_image_write_to_memory( &image, &png[0], &size, 0, pixels.data(), 0, NULL ) != 0, "write PNG output" );
  png.resize( size );
  return png;
}

std::string decode_webp_rgba( const Terminal::KittyImage& image )
{
  require( image.format == Terminal::KITTY_FORMAT_WEBP, "image is stored as WebP" );
  require( image.data && !image.data->empty(), "WebP payload exists" );
  std::string rgba;
  uint32_t width = 0, height = 0;
  require( Terminal::kitty_webp_to_rgba( *image.data, rgba, width, height ), "decode stored WebP" );
  require( width == image.width && height == image.height, "stored WebP dimensions" );
  return rgba;
}

Terminal::KittyCommand first_kitty_command( const std::string& output )
{
  const size_t start = output.find( "\033_G" );
  require( start != std::string::npos, "Kitty APC is present" );
  const size_t end = output.find( "\033\\", start );
  require( end != std::string::npos, "Kitty APC terminator is present" );
  Terminal::KittyCommand command;
  require( Terminal::parse_kitty_command( output.substr( start + 2, end - start - 2 ), command ),
           "parse rendered Kitty APC" );
  return command;
}

const KittyBuffers::StateDelta* find_kitty_delta( const std::string& wire, HostBuffers::HostMessage& message )
{
  require( message.ParseFromString( wire ), "parse host state diff" );
  const KittyBuffers::StateDelta* found = NULL;
  for ( int i = 0; i < message.instruction_size(); i++ ) {
    const HostBuffers::Instruction& instruction = message.instruction( i );
    if ( instruction.HasExtension( HostBuffers::hostbytes ) ) {
      require( instruction.GetExtension( HostBuffers::hostbytes ).hoststring().find( "\033_G" )
                 == std::string::npos,
               "internal terminal bytes do not contain Kitty image data" );
    }
    if ( instruction.HasExtension( HostBuffers::kitty ) ) {
      found = &instruction.GetExtension( HostBuffers::kitty );
    }
  }
  return found;
}

void test_parse_command( void )
{
  Terminal::KittyCommand cmd;
  require( Terminal::parse_kitty_command( "Ga=T,f=24,s=1,v=1,i=7;/wAA", cmd ), "parse failed" );
  require( cmd.action == 'T', "action" );
  require( cmd.format == Terminal::KITTY_FORMAT_RGB, "format" );
  require( cmd.width == 1 && cmd.height == 1, "size" );
  require( cmd.image_id == 7, "id" );
  require( cmd.payload.size() == 3, "decoded rgb" );
  require( static_cast<unsigned char>( cmd.payload[0] ) == 255, "red" );
}

void test_query_reply( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string reply = term.act( std::string( "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\" ) );
  require( reply.find( "\033_Gi=31;OK\033\\" ) != std::string::npos, "query should reply OK" );
  require( term.get_fb().get_kitty_images().empty(), "query must not store an image" );
}

void test_transmit_and_place( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string reply = term.act( transmit_rgb( 3 ) );
  require( reply.find( ";OK" ) != std::string::npos, "transmit reply" );
  require( term.get_fb().get_kitty_images().size() == 1, "one image stored" );
  const Terminal::KittyImage* image = term.get_fb().find_kitty_image( 3 );
  require( image != NULL, "id 3 present" );
  const std::string rgba = decode_webp_rgba( *image );
  require( rgba.size() == 4 && static_cast<unsigned char>( rgba[0] ) == 255
             && static_cast<unsigned char>( rgba[1] ) == 0 && static_cast<unsigned char>( rgba[2] ) == 0
             && static_cast<unsigned char>( rgba[3] ) == 255,
           "RGB input round trips through lossless WebP" );
  require( term.get_fb().get_kitty_placements().size() == 1, "one placement" );
  require( term.get_fb().get_kitty_placements()[0].image_id == 3, "placement id" );
  require( term.get_fb().get_kitty_placements()[0].row == 0, "placed at cursor row" );
}

void test_png_transmit( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string png = rgba_png( 7, 19, 233, 101 );
  const std::string command
    = std::string( "\033_Ga=t,f=100,s=99,v=77,i=12;" ) + Terminal::base64_encode_osc52( png ) + "\033\\";
  const std::string reply = term.act( command );
  require( reply.find( ";OK" ) != std::string::npos, "PNG transmit reply" );
  const Terminal::KittyImage* image = term.get_fb().find_kitty_image( 12 );
  require( image != NULL, "PNG image stored" );
  const std::string rgba = decode_webp_rgba( *image );
  require( rgba.size() == 4 && static_cast<unsigned char>( rgba[0] ) == 7
             && static_cast<unsigned char>( rgba[1] ) == 19 && static_cast<unsigned char>( rgba[2] ) == 233
             && static_cast<unsigned char>( rgba[3] ) == 101,
           "PNG input round trips through lossless WebP" );
}

void test_chunked_transmit( void )
{
  Terminal::Complete term( 80, 24 );
  std::string raw = rgb_cell( 1, 2, 3 );
  raw += rgb_cell( 4, 5, 6 );
  const std::string b64 = Terminal::base64_encode_osc52( raw );
  require( b64.size() >= 8, "base64 length" );
  const std::string first = "\033_Ga=t,f=24,s=2,v=1,i=9,m=1;" + b64.substr( 0, 4 ) + "\033\\";
  const std::string second = "\033_Gm=0;" + b64.substr( 4 ) + "\033\\";
  term.act( first );
  require( term.get_fb().get_kitty_images().empty(), "incomplete upload is not stored" );
  term.act( second );
  const Terminal::KittyImage* image = term.get_fb().find_kitty_image( 9 );
  require( image != NULL, "chunked image stored" );
  const std::string rgba = decode_webp_rgba( *image );
  require( rgba.size() == 8 && static_cast<unsigned char>( rgba[0] ) == 1
             && static_cast<unsigned char>( rgba[4] ) == 4,
           "chunked pixels survive WebP normalization" );
}

void test_zlib_transmit( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string raw = rgb_cell( 17, 31, 47 );
  uLongf compressed_size = compressBound( raw.size() );
  std::string compressed( compressed_size, '\0' );
  require( compress2( reinterpret_cast<Bytef*>( &compressed[0] ),
                      &compressed_size,
                      reinterpret_cast<const Bytef*>( raw.data() ),
                      raw.size(),
                      Z_BEST_COMPRESSION )
             == Z_OK,
           "compress Kitty test image" );
  compressed.resize( compressed_size );
  const std::string command = std::string( "\033_Ga=t,f=24,s=1,v=1,S=3,o=z,i=10;" )
                              + Terminal::base64_encode_osc52( compressed ) + "\033\\";
  const std::string reply = term.act( command );
  require( reply.find( ";OK" ) != std::string::npos, "zlib transmit reply" );
  const Terminal::KittyImage* image = term.get_fb().find_kitty_image( 10 );
  require( image != NULL, "zlib image stored" );
  const std::string rgba = decode_webp_rgba( *image );
  require( rgba.size() == 4 && static_cast<unsigned char>( rgba[0] ) == 17
             && static_cast<unsigned char>( rgba[1] ) == 31 && static_cast<unsigned char>( rgba[2] ) == 47,
           "zlib input normalizes to WebP" );
}

void test_zlib_high_compression_ratio( void )
{
  Terminal::Complete term( 80, 24 );
  const uint32_t width = 64, height = 64;
  std::string raw( static_cast<size_t>( width ) * height * 3, static_cast<char>( 29 ) );
  uLongf compressed_size = compressBound( raw.size() );
  std::string compressed( compressed_size, '\0' );
  require( compress2( reinterpret_cast<Bytef*>( &compressed[0] ),
                      &compressed_size,
                      reinterpret_cast<const Bytef*>( raw.data() ),
                      raw.size(),
                      Z_BEST_COMPRESSION )
             == Z_OK,
           "compress repetitive Kitty image" );
  compressed.resize( compressed_size );
  const std::string command
    = std::string( "\033_Ga=t,f=24,s=64,v=64,o=z,i=11;" ) + Terminal::base64_encode_osc52( compressed ) + "\033\\";
  const std::string reply = term.act( command );
  require( reply.find( ";OK" ) != std::string::npos, "high-ratio zlib transmit reply" );
  const Terminal::KittyImage* image = term.get_fb().find_kitty_image( 11 );
  require( image != NULL && decode_webp_rgba( *image ).size() == static_cast<size_t>( width ) * height * 4,
           "high-ratio zlib input normalizes to WebP" );
}

void test_webp_reduces_repetitive_state( void )
{
  const uint32_t width = 64, height = 64;
  std::string raw( static_cast<size_t>( width ) * height * 3, '\0' );
  for ( size_t i = 0; i < raw.size(); i += 3 ) {
    raw[i] = 23;
    raw[i + 1] = 71;
    raw[i + 2] = static_cast<char>( 191 );
  }
  std::string webp, error;
  uint32_t output_width = 0, output_height = 0;
  require( Terminal::kitty_normalize_webp(
             Terminal::KITTY_FORMAT_RGB, width, height, raw, webp, output_width, output_height, error ),
           "normalize repetitive image" );
  require( output_width == width && output_height == height, "normalized image dimensions" );
  require( webp.size() < raw.size(), "lossless WebP reduces repetitive state" );
}

void test_webp_quota_counts_decoded_pixels( void )
{
  const uint32_t width = 1024, height = 1024;
  const std::string raw( static_cast<size_t>( width ) * height * 3, static_cast<char>( 37 ) );
  std::string webp, error;
  uint32_t output_width = 0, output_height = 0;
  require( Terminal::kitty_normalize_webp(
             Terminal::KITTY_FORMAT_RGB, width, height, raw, webp, output_width, output_height, error ),
           "normalize quota test image" );

  Terminal::Framebuffer framebuffer( 80, 24 );
  const uint32_t capacity = Terminal::KITTY_IMAGE_QUOTA / ( width * height * 4 );
  for ( uint32_t id = 1; id <= capacity + 1; id++ ) {
    Terminal::KittyImage image;
    image.id = id;
    image.width = width;
    image.height = height;
    image.data = std::make_shared<std::string>( webp );
    framebuffer.put_kitty_image( image );
    Terminal::KittyPlacement placement;
    placement.image_id = id;
    placement.placement_id = id;
    framebuffer.put_kitty_placement( placement );
  }
  require( framebuffer.find_kitty_image( 1 ) == NULL, "decoded-state quota evicts the oldest image" );
  require( framebuffer.find_kitty_image( capacity + 1 ) != NULL, "decoded-state quota preserves the new upload" );
  require( framebuffer.get_kitty_images().size() == capacity, "decoded-state quota caps aggregate pixels" );
}

void test_invalid_dimensions( void )
{
  Terminal::Complete term( 80, 24 );
  const std::string command = std::string( "\033_Ga=t,f=24,s=2,v=1,i=13;" )
                              + Terminal::base64_encode_osc52( rgb_cell( 1, 2, 3 ) ) + "\033\\";
  const std::string reply = term.act( command );
  require( reply.find( "EINVAL" ) != std::string::npos, "invalid dimensions are rejected" );
  require( term.get_fb().find_kitty_image( 13 ) == NULL, "invalid image is not stored" );
}

void test_delete( void )
{
  Terminal::Complete term( 80, 24 );
  term.act( transmit_rgb( 4 ) );
  term.act( std::string( "\033_Ga=d,d=I,i=4\033\\" ) );
  require( term.get_fb().get_kitty_placements().empty(), "placement deleted" );
  require( term.get_fb().find_kitty_image( 4 ) == NULL, "image data freed" );
}

void test_terminal_output_converts_webp_to_kitty( void )
{
  Terminal::Complete empty( 80, 24 );
  Terminal::Complete with_image( 80, 24 );
  with_image.act( transmit_rgb( 5 ) );

  std::string first;
  Terminal::append_kitty_frame( first, true, empty.get_fb(), with_image.get_fb() );
  const Terminal::KittyCommand command = first_kitty_command( first );
  require( command.action == Terminal::KittyTransmit, "terminal output transmits the image" );
  require( command.format == Terminal::KITTY_FORMAT_RGBA, "terminal output uses standard Kitty RGBA" );
  require( command.width == 1 && command.height == 1, "terminal output dimensions" );
  require( command.payload.size() == 4 && static_cast<unsigned char>( command.payload[0] ) == 255
             && static_cast<unsigned char>( command.payload[3] ) == 255,
           "terminal output contains decoded WebP pixels" );

  std::string second;
  Terminal::append_kitty_frame( second, true, with_image.get_fb(), with_image.get_fb() );
  require( second.find( "\033_G" ) == std::string::npos, "unchanged image is not retransmitted" );

  std::string repaint;
  Terminal::append_kitty_frame( repaint, false, with_image.get_fb(), with_image.get_fb() );
  require( repaint.find( "a=d,d=I,i=5" ) != std::string::npos,
           "full repaint clears known virtual placements by image id" );
  require( repaint.find( "a=d,d=A" ) == std::string::npos,
           "full repaint never globally deletes unrelated local graphics" );
  require( repaint.find( "a=t" ) != std::string::npos && repaint.find( "a=p" ) != std::string::npos,
           "full repaint restores image data and placement" );

  Terminal::Display internal_display( false );
  const std::string internal = internal_display.new_frame( true, empty.get_fb(), with_image.get_fb() );
  require( internal.find( "\033_G" ) == std::string::npos,
           "internal terminal rendering never serializes Kitty APC data" );
}

void test_scroll_removes_offscreen_placement( void )
{
  Terminal::Complete term( 80, 24 );
  term.act( transmit_rgb( 6 ) );
  require( !term.get_fb().get_kitty_placements().empty(), "placed" );
  term.act( std::string( "\033[1;1H\033[M" ) ); /* delete line 1 */
  require( term.get_fb().get_kitty_placements().empty(), "scrolled-off placement removed" );
}

void test_state_diff_uses_webp_delta( void )
{
  Terminal::Complete src( 80, 24 );
  src.act( transmit_rgb( 8 ) );
  Terminal::Complete blank( 80, 24 );
  const std::string wire = src.diff_from( blank );
  require( !wire.empty(), "state diff carries image" );

  HostBuffers::HostMessage message;
  const KittyBuffers::StateDelta* delta = find_kitty_delta( wire, message );
  require( delta != NULL, "state diff has structured Kitty delta" );
  require( delta->image_size() == 1, "state diff carries one image" );
  require( delta->image( 0 ).id() == 8, "state diff image id" );
  uint32_t width = 0, height = 0;
  require( Terminal::kitty_webp_dimensions( delta->image( 0 ).webp(), width, height ),
           "state diff payload is WebP" );
  require( width == 1 && height == 1, "state diff WebP dimensions" );
  require( delta->replace_placements() && delta->placement_size() == 1, "state diff carries placement state" );

  Terminal::Complete dst( 80, 24 );
  dst.apply_string( wire );
  const Terminal::KittyImage* received = dst.get_fb().find_kitty_image( 8 );
  require( received != NULL, "client state has image" );
  require( received->format == Terminal::KITTY_FORMAT_WEBP, "client state keeps canonical WebP" );
  require( !dst.get_fb().get_kitty_placements().empty(), "client state has placement" );
}

void test_placement_delta_does_not_resend_image( void )
{
  Terminal::Complete current( 80, 24 );
  current.act( transmit_rgb( 18 ) );
  Terminal::Complete previous( 80, 24 );
  previous.apply_string( current.diff_from( previous ) );

  current.act( std::string( "\033_Ga=d,d=i,i=18\033\\" ) );
  const std::string wire = current.diff_from( previous );
  HostBuffers::HostMessage message;
  const KittyBuffers::StateDelta* delta = find_kitty_delta( wire, message );
  require( delta != NULL, "placement deletion has Kitty delta" );
  require( delta->image_size() == 0 && delta->delete_image_id_size() == 0,
           "placement-only delta does not resend WebP" );
  require( delta->replace_placements() && delta->placement_size() == 0, "placement-only delta clears placements" );

  previous.apply_string( wire );
  require( previous.get_fb().find_kitty_image( 18 ) != NULL, "placement deletion retains image data" );
  require( previous.get_fb().get_kitty_placements().empty(), "placement deletion applies" );
}

void test_text_diff_checkpoints_placements( void )
{
  Terminal::Complete previous( 80, 24 );
  previous.act( transmit_held_rgb( 21, 2, 4, 8 ) );
  Terminal::Complete current( previous );
  current.act( std::string( "text" ) );
  require( previous.get_fb().get_kitty_placements() == current.get_fb().get_kitty_placements(),
           "text update leaves server placement unchanged" );

  const std::string wire = current.diff_from( previous );
  HostBuffers::HostMessage message;
  const KittyBuffers::StateDelta* delta = find_kitty_delta( wire, message );
  require( delta != NULL && delta->image_size() == 0, "text diff does not resend WebP" );
  require( delta->replace_placements() && delta->placement_size() == 1,
           "text diff checkpoints placement state after terminal replay" );

  Terminal::Complete received( previous );
  received.apply_string( wire );
  require( received.get_fb().get_kitty_images() == current.get_fb().get_kitty_images(),
           "text diff preserves image state" );
  require( received.get_fb().get_kitty_placements() == current.get_fb().get_kitty_placements(),
           "text diff restores authoritative placement state" );
}

void test_image_replacement_restores_placement( void )
{
  Terminal::Complete previous( 80, 24 );
  previous.act( transmit_held_rgb( 19, 255, 0, 0 ) );
  Terminal::Complete current( previous );
  current.act( transmit_held_rgb( 19, 0, 255, 0 ) );
  require( previous.get_fb().get_kitty_placements() == current.get_fb().get_kitty_placements(),
           "replacement preserves the same placement in server state" );

  const std::string wire = current.diff_from( previous );
  HostBuffers::HostMessage message;
  const KittyBuffers::StateDelta* delta = find_kitty_delta( wire, message );
  require( delta != NULL && delta->image_size() == 1 && delta->delete_image_id_size() == 1,
           "replacement sends changed WebP state" );
  require( delta->replace_placements() && delta->placement_size() == 1,
           "replacement resends placement invalidated by image deletion" );

  Terminal::Complete received( previous );
  received.apply_string( wire );
  require( received.get_fb() == current.get_fb(), "replacement state round trips exactly" );

  std::string output;
  Terminal::append_kitty_frame( output, true, previous.get_fb(), current.get_fb() );
  require( output.find( "a=d,d=I,i=19" ) != std::string::npos, "old terminal image is removed" );
  require( output.find( "a=p,C=1,q=2,i=19" ) != std::string::npos, "replacement restores its terminal placement" );
}

void test_anonymous_placement_deletion_resets_image_placements( void )
{
  Terminal::Complete previous( 80, 24 );
  previous.act( transmit_held_rgb( 20, 1, 2, 3 ) );
  Terminal::Complete current( previous );
  current.act( std::string( "\033_Ga=d,d=i,i=20\033\\" ) );
  require( current.get_fb().find_kitty_image( 20 ) != NULL, "placement deletion keeps image data" );
  require( current.get_fb().get_kitty_placements().empty(), "anonymous placement is gone from state" );

  std::string output;
  Terminal::append_kitty_frame( output, true, previous.get_fb(), current.get_fb() );
  require( output.find( "a=d,d=i,i=20" ) != std::string::npos,
           "anonymous placement deletion clears the image's terminal placements" );
  require( output.find( "a=t" ) == std::string::npos, "placement deletion does not resend WebP pixels" );
}

void check_kittycairo_output( const std::string& input )
{
  Terminal::Complete source( 80, 40 ), blank( 80, 40 ), received( 80, 40 );
  const Terminal::ClientGeometry geometry( 80, 40, 1600, 1600, 20, 40 );
  /* gnuplot sends chunked PNG with a=T, no s/v or c/r, then ED0 and LF.
     Its 640x480 pixel plot occupies 32 columns and 12 rows in this terminal. */
  for ( size_t pos = 0; pos < input.size(); pos += 997 ) {
    source.act( input.substr( pos, 997 ), &geometry );
  }
  require( source.get_fb().get_kitty_images().size() == 1, "kittycairo produces one stored image" );
  const auto& image = source.get_fb().get_kitty_images().begin()->second;
  require( image.width == 640 && image.height == 480, "kittycairo PNG dimensions survive WebP storage" );
  require( source.get_fb().ds.get_cursor_row() == 13 && source.get_fb().ds.get_cursor_col() == 32,
           "kittycairo cursor follows the full image rectangle and trailing LF, not a single cell" );
  const auto& placement = source.get_fb().get_kitty_placements().at( 0 );
  require( placement.columns == 0 && placement.rows == 0,
           "native pixel dimensions must not be replaced with a rounded cell rectangle" );
  received.apply_string( source.diff_from( blank ) );
  require( !source.compare( received ) && received.get_fb().ds == source.get_fb().ds
             && received.get_fb().get_kitty_images() == source.get_fb().get_kitty_images()
             && received.get_fb().get_kitty_placements() == source.get_fb().get_kitty_placements(),
           "kittycairo layout survives state synchronization" );
  std::string output;
  Terminal::append_kitty_frame( output, false, blank.get_fb(), received.get_fb() );
  const auto command = first_kitty_command( output );
  require( command.width == 640 && command.height == 480, "client renders original kittycairo pixel dimensions" );
  require( output.find( "a=p,C=1,q=2,i=" ) != std::string::npos,
           "client placement leaves synchronized cursor restoration to the display renderer" );
  Terminal::Display display( false );
  Terminal::Complete rendered( 80, 40 );
  rendered.act( display.new_frame( false, blank.get_fb(), received.get_fb() ) );
  require( rendered.get_fb().ds.get_cursor_row() == 13 && rendered.get_fb().ds.get_cursor_col() == 32,
           "display restores text cursor below the plot" );
}

void test_pixel_placement_cursor( void )
{
  const std::string png = rgba_png( 70, 100, 200, 255, 640, 480 );
  check_kittycairo_output( "\033[0J\033[H" + Terminal::encode_kitty_chunks( "a=T,f=100", png ) + "\033[0J\n" );

  Terminal::Complete stored( 80, 80 );
  stored.act( Terminal::encode_kitty_chunks( "a=t,f=100,i=61", png ) );
  const Terminal::ClientGeometry geometry( 80, 80, 1600, 3200, 20, 40 );
  struct Case { const char* keys; int col, row; };
  const Case cases[] = { { "", 32, 12 }, { ",c=16", 16, 6 }, { ",r=6", 16, 6 },
                         { ",c=20,r=3", 20, 3 }, { ",X=10,Y=10", 33, 13 },
                         { ",x=20,y=40,w=320,h=120", 16, 3 }, { ",C=1", 0, 0 },
                         { ",U=1,c=16,r=6", 0, 0 }, { ",P=62", 0, 0 },
                         { ",c=4294967295,r=4294967295", 79, 79 } };
  for ( const auto& test : cases ) {
    Terminal::Complete term( stored );
    term.act( std::string( "\033_Ga=p,i=61" ) + test.keys + "\033\\", &geometry );
    require( term.get_fb().ds.get_cursor_col() == test.col && term.get_fb().ds.get_cursor_row() == test.row,
             "pixel/cell placement, cropping, offsets, cursor-hold and bounded movement" );
  }
  const Terminal::ClientGeometry changed( 80, 80, 800, 1600, 10, 20 );
  stored.act( "\033_Ga=p,i=61\033\\", &changed );
  require( stored.get_fb().ds.get_cursor_col() == 64 && stored.get_fb().ds.get_cursor_row() == 24,
           "placement uses current attachment geometry rather than dimensions stored with the image" );
}
}

static void test_file_media()
{
  char directory[] = "/tmp/goblin-kitty-media.XXXXXX";
  require( mkdtemp( directory ) != NULL, "create test directory" );
  const std::string regular = std::string( directory ) + "/image", temp = std::string( directory ) + "/tty-graphics-protocol-image";
  const std::string bytes = "prefix" + rgb_cell( 255, 0, 0 ) + "suffix";
  const std::string memory = "/goblin-kitty-" + std::to_string( getpid() );
  for ( char medium : { 'd', 'f', 't', 's' } ) {
    std::string payload = rgb_cell( 255, 0, 0 );
    if ( medium == 'f' || medium == 't' ) {
      payload = medium == 'f' ? regular : temp;
      const int fd = open( payload.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600 );
      require( fd >= 0, "create Kitty file" );
      require( write( fd, bytes.data(), bytes.size() ) == ssize_t( bytes.size() ), "write Kitty file" ); close( fd );
    } else if ( medium == 's' ) {
      payload = memory;
      const int fd = shm_open( memory.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600 );
      require( fd >= 0 && ftruncate( fd, bytes.size() ) == 0, "create shared memory" );
      void* mapping = mmap( NULL, bytes.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
      require( mapping != MAP_FAILED, "map shared memory" );
      memcpy( mapping, bytes.data(), bytes.size() ); munmap( mapping, bytes.size() ); close( fd );
    }
    Terminal::Complete source( 80, 24 ), received( 80, 24 ), blank( 80, 24 );
    const auto reply = source.act( "\033_Ga=T,t=" + std::string( 1, medium ) + ",i=90,f=24,s=1,v=1"
                                  + ( medium == 'd' ? "" : ",O=6,S=3" ) + ";" + Terminal::base64_encode_osc52( payload ) + "\033\\" );
    require( reply.find( ";OK" ) != std::string::npos, "incoming Kitty medium accepted" );
    const auto* img = source.get_fb().find_kitty_image( 90 );
    require( img && img->width == 1 && img->height == 1, "image stored for all four media" );
    received.apply_string( source.diff_from( blank ) );
    require( received.get_fb().find_kitty_image( 90 ) != NULL, "all four media survive state sync" );
    Terminal::Display display( false );
    display.set_graphics( Terminal::ClientGraphics( true ), true );
    const auto output = display.new_frame( true, blank.get_fb(), received.get_fb() );
    require( output.find( "t=f" ) == std::string::npos && output.find( "t=t" ) == std::string::npos
               && output.find( "t=s" ) == std::string::npos && output.find( "\033_G" ) != std::string::npos,
             "client output uses inline pixels, never server references" );
    if ( medium == 'f' ) { require( access( regular.c_str(), F_OK ) == 0, "regular file preserved" ); }
    if ( medium == 't' ) { require( access( temp.c_str(), F_OK ) < 0, "protocol temporary file removed" ); }
    if ( medium == 's' ) { const int fd = shm_open( memory.c_str(), O_RDONLY, 0 ); require( fd < 0, "shared memory unlinked" ); }
  }
  Terminal::KittyCommand command;
  command.medium = 't'; command.payload = regular; command.data_offset = 6; command.data_size = 3;
  std::string data, error;
  require( Terminal::kitty_read_medium( command, data, error ) && access( regular.c_str(), F_OK ) == 0,
           "temporary medium never deletes an ordinary filename" );
  command.payload = regular + std::string( 1, '\0' ) + "ignored";
  require( !Terminal::kitty_read_medium( command, data, error ), "reject NUL path" );
  const std::string fifo = std::string( directory ) + "/fifo";
  require( mkfifo( fifo.c_str(), 0600 ) == 0, "create FIFO" );
  command.payload = fifo;
  require( !Terminal::kitty_read_medium( command, data, error ), "reject special file without blocking" );
  unlink( fifo.c_str() ); unlink( regular.c_str() ); rmdir( directory );
}

int main( int argc, char* argv[] )
{
  try {
    if ( argc == 2 && std::string( argv[1] ) == "--kittycairo" ) {
      const std::string input( ( std::istreambuf_iterator<char>( std::cin ) ), std::istreambuf_iterator<char>() );
      check_kittycairo_output( input );
      return 0;
    }
    test_parse_command();
    test_file_media();
    test_query_reply();
    test_transmit_and_place();
    test_png_transmit();
    test_chunked_transmit();
    test_zlib_transmit();
    test_zlib_high_compression_ratio();
    test_webp_reduces_repetitive_state();
    test_webp_quota_counts_decoded_pixels();
    test_invalid_dimensions();
    test_delete();
    test_scroll_removes_offscreen_placement();
    test_terminal_output_converts_webp_to_kitty();
    test_state_diff_uses_webp_delta();
    test_placement_delta_does_not_resend_image();
    test_text_diff_checkpoints_placements();
    test_image_replacement_restores_placement();
    test_anonymous_placement_deletion_resets_image_placements();
    test_pixel_placement_cursor();
  } catch ( const std::exception& e ) {
    std::cerr << "kitty-graphics: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
