/* Distributed under the GNU GPL, version 3 or later. */
#include <iostream>
#include <stdexcept>
#include <vector>

#include "src/protobufs/hostinput.pb.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/kitty.h"
#include "src/terminal/sixel.h"

namespace {
void require( bool value, const char* message )
{
  if ( !value ) {
    throw std::runtime_error( message );
  }
}

std::string sample()
{
  return "\033P0;1;0q\"1;1;8;8#1;2;100;0;0!4~#2;2;0;100;0!4~-$#3;2;0;0;100!8B\033\\";
}

unsigned component( const Terminal::Sixel::Bitmap& image, unsigned x, unsigned y, unsigned channel )
{
  return uint8_t( image.rgba.at( ( y * image.width + x ) * 4 + channel ) );
}

void codecs()
{
  using namespace Terminal::Sixel;
  Bitmap image, restored;
  std::string error, encoded;
  const std::string input = sample();
  for ( size_t i = 0; i < input.size(); i++ ) {
    require( !decode( input.substr( 0, i ), image, error ) && image.rgba.empty(),
             "no partial DCS becomes an image" );
  }
  require( decode( input, image, error ), "decode sixel" );
  require( image.width == 8 && image.height == 8 && image.rgba.size() == 256, "raster dimensions" );
  require( component( image, 0, 0, 0 ) == 255 && component( image, 4, 0, 1 ) == 255
             && component( image, 7, 7, 2 ) == 255,
           "RGB palette and sixel rows" );
  require( encode( image, encoded, error ) && encoded.find( "\033P" ) == 0, "native sixel encoder" );
  require( decode( encoded, restored, error ) && image.rgba == restored.rgba && image.width == restored.width
             && image.height == restored.height,
           "native sixel roundtrip" );
  const std::string c1 = std::string( 1, char( 0x90 ) ) + input.substr( 2, input.size() - 4 ) + char( 0x9c );
  require( decode( c1, restored, error ) && restored.rgba == image.rgba, "8-bit DCS framing" );

  require( decode( "\033P0;1q\"1;1;2;2#1;2;100;0;0@\033\\", image, error ), "transparent image" );
  require( component( image, 0, 0, 3 ) == 255 && component( image, 1, 1, 3 ) == 0, "P2 transparent background" );
  require( encode( image, encoded, error ) && decode( encoded, restored, error ) && restored.rgba == image.rgba,
           "transparent holes survive output" );
  require( decode( "\033P0;0q\"1;1;2;2#1;2;100;0;0@\033\\", image, error, 0x123456 ), "opaque image" );
  require( component( image, 1, 1, 0 ) == 0x12 && component( image, 1, 1, 3 ) == 255,
           "caller supplies background" );
  require( decode( "\033P0;1q\"1;1;3;1#1;1;0;50;100@#2;1;120;50;100@#3;1;240;50;100@\033\\", image, error ),
           "HLS palette" );
  require( component( image, 0, 0, 2 ) == 255 && component( image, 1, 0, 0 ) == 255
             && component( image, 2, 0, 1 ) == 255,
           "DEC HLS blue/red/green hue order" );
  require( decode( "\033P7;1q#1;2;100;0;0@\033\\", image, error ) && image.height == 1, "square macro aspect" );
  require( decode( "\033P2;1q#1;2;100;0;0@\033\\", image, error ) && image.height == 5, "vertical macro aspect" );
  require( decode( "\033P0;1q\"3;2;1;1#1;2;100;0;0@\033\\", image, error ) && image.height == 2,
           "raster aspect rounded up" );

  // Exercise sparse color bands, gaps, run lengths, and the non-six-multiple
  // last row without relying on a single hand-picked fixture.
  for ( unsigned height = 1; height <= 19; height++ ) {
    image.width = 37;
    image.height = height;
    image.rgba.assign( image.width * image.height * 4, '\0' );
    for ( unsigned i = 0; i < image.width * image.height; i++ ) {
      if ( i % 7 == 0 ) {
        continue;
      }
      for ( unsigned c = 0; c < 3; c++ ) {
        image.rgba[4 * i + c] = ( ( i + c ) % 3 == 0 ) ? char( 255 ) : 0;
      }
      image.rgba[4 * i + 3] = char( 255 );
    }
    require( encode( image, encoded, error ) && decode( encoded, restored, error ) && restored.rgba == image.rgba
               && restored.width == image.width && restored.height == image.height,
             "patterned native roundtrip" );
  }
}

void limits()
{
  using namespace Terminal::Sixel;
  const std::vector<std::string> invalid = { "\033Pq!999999999999999999999999~\033\\",
                                             "\033Pq!0~\033\\",
                                             "\033Pq!3\033\\",
                                             "\033Pq!16384~\033\\",
                                             "\033Pq\"1;1;16383;16383~\033\\",
                                             "\033Pq#1024~\033\\",
                                             "\033Pq#1;2;101;0;0~\033\\",
                                             "\033Pq#1;1;361;50;50~\033\\",
                                             "\033Pq#1;3;1;2;3~\033\\",
                                             "\033Pq\"1000;1;1;1~\033\\",
                                             "\033Pq\"1;1;1;1;1~\033\\",
                                             "\033P$q~\033\\",
                                             "\033Pq~\033[0m\033\\",
                                             "\033Pq~\030~\033\\",
                                             "\033Pq~\032~\033\\" };
  Bitmap image;
  std::string error, output;
  for ( const auto& dcs : invalid ) {
    require( !decode( dcs, image, error ) && image.rgba.empty() && !error.empty(),
             "malformed or oversized image rejected" );
  }
  require( !decode( std::string( MAX_ENCODED_BYTES + 1, '~' ), image, error ), "encoded byte quota" );
  std::string costly = "\033P7;1q";
  for ( unsigned i = 0; i < 12000; i++ ) {
    costly += "!4096?$";
  }
  costly += "\033\\";
  require( !decode( costly, image, error ) && error.find( "work" ) != std::string::npos,
           "repeat/overdraw CPU quota" );
  image.width = 1025;
  image.height = 1;
  image.rgba.assign( image.width * 4, '\0' );
  for ( unsigned i = 0; i < image.width; i++ ) {
    image.rgba[i * 4] = char( ( ( i % 101 ) * 255 + 50 ) / 100 );
    image.rgba[i * 4 + 1] = char( ( ( i / 101 ) * 255 + 50 ) / 100 );
    image.rgba[i * 4 + 3] = char( 255 );
  }
  require( !encode( image, output, error ) && output.empty(), "output palette quota" );
  image.width = image.height = 1;
  image.rgba.assign( 4, char( 127 ) );
  require( !encode( image, output, error ) && output.empty(), "partial alpha must not be silently discarded" );
}

void wide_images()
{
  using namespace Terminal;
  for ( unsigned width : { 4095U, 4096U, 4097U, 5120U, 8192U } ) {
    Sixel::Bitmap image, decoded;
    image.width = width;
    image.height = width == 5120 ? 2048 : 12; // 40 MiB RGBA: over the former aggregate quota
    image.rgba.assign( size_t( image.width ) * image.height * 4, char( 255 ) );
    // Colored right edge catches off-by-one clipping, not just header parsing.
    for ( unsigned y = 0; y < image.height; y++ ) {
      image.rgba[( size_t( y ) * width + width - 1 ) * 4 + 1] = 0;
      image.rgba[( size_t( y ) * width + width - 1 ) * 4 + 2] = 0;
    }
    std::string encoded, error;
    require( Sixel::encode( image, encoded, error ), "encode wide sixel" );
    require( Sixel::decode( encoded, decoded, error ) && decoded.rgba == image.rgba, "wide sixel preserves every pixel" );
    const unsigned columns = ( width + 7 ) / 8, rows = ( image.height + 15 ) / 16;
    Complete source( columns, rows + 1 ), receiver( columns, rows + 1 ), blank( columns, rows + 1 );
    ClientGeometry geometry;
    geometry.columns = columns; geometry.rows = rows + 1;
    geometry.cell_width_px = 8; geometry.cell_height_px = 16;
    source.set_sixel_enabled( true );
    source.act( encoded, &geometry );
    require( source.get_fb().get_kitty_images().size() == 1, "large canvas retained in image state" );
    receiver.apply_string( source.diff_from( blank ) );
    const auto& retained = receiver.get_fb().get_kitty_images().begin()->second;
    require( retained.width >= width && retained.height >= image.height, "wide state image dimensions preserved" );
    std::string rgba;
    uint32_t rw = 0, rh = 0;
    require( kitty_webp_to_rgba( *retained.data, rgba, rw, rh ), "client decodes wide WebP" );
    require( uint8_t( rgba[( size_t( image.height - 1 ) * rw + width - 1 ) * 4] ) == 255
               && rgba[( size_t( image.height - 1 ) * rw + width - 1 ) * 4 + 1] == 0, "right edge survived state transport" );
    for ( bool native : { false, true } ) {
      Display display( false );
      display.set_graphics( ClientGraphics( true, native ), true );
      display.set_graphics_geometry( geometry );
      const auto output = display.new_frame( true, blank.get_fb(), receiver.get_fb() );
      require( output.find( native ? "\033P" : "\033_G" ) != std::string::npos, "large native/fallback client render" );
    }
  }
}

void state_and_rendering()
{
  using namespace Terminal;
  KittyImage image;
  std::string error, output;
  require( Sixel::normalize( sample(), image, error ) && image.origin == ImageOrigin::Sixel,
           "normalize to sixel-origin state" );
  require( image.format == KITTY_FORMAT_WEBP && image.data->compare( 0, 4, "RIFF" ) == 0,
           "WebP is the stored payload" );
  Framebuffer blank_fb( 80, 24 ), frame( blank_fb );
  image.id = frame.put_kitty_image( image );
  KittyPlacement placement;
  placement.image_id = image.id;
  placement.row = 3;
  placement.col = 5;
  frame.put_kitty_placement( placement );
  KittyBuffers::StateDelta delta;
  require( kitty_state_delta_to_proto( blank_fb, frame, &delta, false ) && delta.image_size() == 1
             && delta.image( 0 ).origin() == KittyBuffers::Image::SIXEL,
           "image origin in normal state delta" );
  Framebuffer kitty_frame( blank_fb );
  KittyImage kitty_image( image );
  kitty_image.origin = ImageOrigin::Kitty;
  kitty_frame.put_kitty_image( kitty_image );
  KittyBuffers::StateDelta kitty_delta;
  require( kitty_state_delta_to_proto( blank_fb, kitty_frame, &kitty_delta, false ) && kitty_delta.image_size() == 1
             && !kitty_delta.image( 0 ).has_origin(),
           "existing Kitty image messages carry no new origin bytes" );
  Framebuffer restored_kitty( blank_fb );
  apply_kitty_state_delta( kitty_delta, restored_kitty );
  require( restored_kitty.find_kitty_image( image.id )
             && restored_kitty.find_kitty_image( image.id )->origin == ImageOrigin::Kitty,
           "missing origin remains Kitty for existing peers" );
  HostBuffers::HostMessage message;
  message.add_instruction()->MutableExtension( HostBuffers::kitty )->CopyFrom( delta );
  Complete empty( 80, 24 ), sender( empty ), receiver( empty );
  sender.apply_string( message.SerializeAsString() );
  const Complete first( sender );
  sender.act( "text alongside image" );
  // Losing the first state still recovers both text and image from a later
  // cumulative update, using the existing HostBuffers state transport.
  receiver.apply_string( sender.diff_from( empty ) );
  const auto* received = receiver.get_fb().find_kitty_image( image.id );
  require( received && received->origin == ImageOrigin::Sixel && *received->data == *image.data,
           "state recovers image and origin after loss" );
  require( receiver.get_fb().get_kitty_placements()[0] == placement, "placement accompanies image" );
  require( receiver.diff_from( receiver ).empty(), "duplicate state does not redraw/resend image" );
  HostBuffers::HostMessage text_delta;
  require( text_delta.ParseFromString( sender.diff_from( first ) ), "parse text-only delta" );
  for ( const auto& instruction : text_delta.instruction() ) {
    if ( instruction.HasExtension( HostBuffers::kitty ) ) {
      require( instruction.GetExtension( HostBuffers::kitty ).image_size() == 0,
               "text changes do not resend image bytes" );
    }
  }
  const Complete before_reset( sender );
  sender.act( "\033c" );
  receiver.apply_string( sender.diff_from( before_reset ) );
  require( receiver.get_fb().get_kitty_images().empty(), "terminal reset deletes image state" );

  require( Sixel::select_renderer( true, true ) == Sixel::Renderer::Sixel,
           "dual-capable terminal must prefer sixel" );
  require( Sixel::select_renderer( true, false ) == Sixel::Renderer::Sixel, "sixel-only terminal" );
  require( Sixel::select_renderer( false, true ) == Sixel::Renderer::Kitty, "Kitty is fallback only" );
  require( Sixel::select_renderer( false, false ) == Sixel::Renderer::None, "unsupported terminal" );
  require( Sixel::render( image, true, true, output, error ) && output.find( "\033P" ) == 0
             && output.find( "\033_G" ) == std::string::npos,
           "dual-capable client gets native sixel" );
  Sixel::Bitmap original, rendered;
  require( Sixel::decode( sample(), original, error ) && Sixel::decode( output, rendered, error )
             && rendered.rgba == original.rgba,
           "WebP state to native sixel pixels" );
  require( Sixel::render( image, false, true, output, error ) && output.find( "\033_G" ) == 0,
           "client-only Kitty fallback" );
  Complete kitty_client( 80, 24 );
  kitty_client.act( output );
  require( kitty_client.get_fb().find_kitty_image( image.id ) != nullptr, "fallback accepted by Kitty parser" );
  require( !Sixel::render( image, false, false, output, error ) && output.empty(), "no unsupported escape output" );
  append_kitty_frame( output, false, blank_fb, frame );
  require( output.empty(), "ordinary Kitty compositor never implicitly converts sixel" );
}

void live_pipeline()
{
  using namespace Terminal;
  const ClientGeometry geometry( 20, 10, 80, 40, 4, 4 );
  Complete blank( 20, 10 ), server( blank ), client( blank );
  require( server.act( "\033[c", &geometry ) == "\033[?62c", "unnegotiated peer does not advertise sixel" );
  server.act( sample(), &geometry );
  require( server.get_fb().get_kitty_images().empty(), "unnegotiated DCS is ignored" );
  server.set_sixel_enabled( true );
  require( server.act( "\033[c", &geometry ) == "\033[?62;4c", "negotiated sixel DA" );
  const auto dcs = sample();
  for ( size_t i = 0; i < dcs.size(); i++ ) {
    server.act( dcs.substr( i, 1 ), &geometry );
    require( i + 1 == dcs.size() || server.get_fb().get_kitty_images().empty(), "unfinished DCS not published" );
  }
  require( server.get_fb().get_kitty_images().size() == 1, "live DCS decoded into image state" );
  require( server.get_fb().ds.get_cursor_row() == 1 && server.get_fb().ds.get_cursor_col() == 0,
           "sixel cursor is on last occupied row, ready for a newline" );
  client.apply_string( server.diff_from( blank ) );
  require( client.get_fb().get_kitty_images() == server.get_fb().get_kitty_images()
             && client.get_fb().get_kitty_placements() == server.get_fb().get_kitty_placements(), "live graphics sync" );
  for ( bool sixel : { false, true } ) {
    for ( bool kitty : { false, true } ) {
      Display display( false );
      display.set_graphics( ClientGraphics( kitty, sixel ), true );
      display.set_graphics_geometry( geometry );
      const auto bytes = display.new_frame( true, blank.get_fb(), client.get_fb() );
      require( ( bytes.find( "\033P" ) != std::string::npos ) == sixel, "native sixel on sixel and dual-capable clients" );
      require( ( bytes.find( "\033_G" ) != std::string::npos ) == ( kitty && !sixel ), "client-only Kitty fallback" );
      require( display.new_frame( true, client.get_fb(), client.get_fb() ).empty(), "unchanged image never repainted" );
    }
  }
  const Complete first( server );
  server.act( "\033[1;1HX", &geometry );
  require( server.get_fb().get_kitty_placements().size() == 2, "text erases only overlapped image cell" );
  client.apply_string( server.diff_from( first ) );
  require( client.get_fb().get_kitty_images() == server.get_fb().get_kitty_images()
             && client.get_fb().get_kitty_placements() == server.get_fb().get_kitty_placements(),
           "text delta does not delete retained image payload" );
  const Complete second( server );
  server.act( "\033[1S", &geometry );
  client.apply_string( server.diff_from( second ) );
  require( client.get_fb().get_kitty_placements() == server.get_fb().get_kitty_placements(), "scroll preserves remaining cropped pixels" );
  server.act( "\033[2J", &geometry );
  client.apply_string( server.diff_from( second ) ); // skipped/lost intermediate state
  require( client.get_fb().get_kitty_images().empty(), "erase after skipped state removes image" );

  for ( const std::string& suffix : { std::string( "\030" ), std::string( "\032" ), std::string( "\033[0m" ),
                                    std::string( "\033\033\\" ), std::string( "\033]0;title\007" ) } ) {
    Complete canceled( blank );
    canceled.set_sixel_enabled( true );
    canceled.act( dcs.substr( 0, dcs.size() - 2 ) + suffix + "\033\\OK", &geometry );
    require( canceled.get_fb().get_kitty_images().empty(), "canceled DCS never becomes an image" );
    require( canceled.get_fb().get_cell( 0, 0 )->debug_contents().find( "O" ) != std::string::npos, "text after cancellation preserved" );
  }
  Sixel::Palette shared;
  Sixel::Bitmap a, b;
  std::string error;
  require( Sixel::decode( "\033Pq#1;2;100;0;0\033\\", a, error, 0, &shared ), "shared palette definition" );
  require( Sixel::decode( "\033P7;1q#1~\033\\", b, error, 0, &shared ) && component( b, 0, 0, 0 ) == 255,
           "shared palette persists across DCS" );
}
}

int main()
{
  try {
    codecs();
    limits();
    wide_images();
    state_and_rendering();
    live_pipeline();
    std::cout << "sixel codec, state and renderer-selection tests passed\n";
  } catch ( const std::exception& error ) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
