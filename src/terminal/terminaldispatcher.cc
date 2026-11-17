/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#include "src/terminal/colorspace.h"
#include "src/terminal/inputlimits.h"
#include "src/terminal/gpusurface.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/packedimage.h"
#include "src/terminal/parseraction.h"
#include "src/terminal/terminalframebuffer.h"
#include "terminaldispatcher.h"

using namespace Terminal;

Dispatcher::Dispatcher()
  : params(), parsed_params(), parsed( false ), dispatch_chars(), OSC_string(), OSC_overflow( false ),
    clipboard_events(), APC_string(), kitty_uploading( false ), kitty_partial(),
    kitty_payload(), client_geometry( NULL ), terminal_to_host()
{}

void Dispatcher::newparamchar( const Parser::Param* act )
{
  assert( act->char_present );
  assert( ( act->ch == ';' ) || ( act->ch == ':' ) || ( ( act->ch >= '0' ) && ( act->ch <= '9' ) ) );
  if ( csi_byte_limit && params.size() >= csi_byte_limit ) {
    csi_overflow = true;
    return;
  }
  params.push_back( act->ch );
  parsed = false;
}

void Dispatcher::collect( const Parser::Collect* act )
{
  DCS_escape_end( 0, NULL );
  assert( act->char_present );
  if ( ( dispatch_chars.length() < 8 ) /* never should need more than 2 */
       && ( act->ch <= 255 ) ) {       /* ignore non-8-bit */
    dispatch_chars.push_back( act->ch );
  }
}

void Dispatcher::clear( const Parser::Clear* act )
{
  if ( DCS_escape && !( act->ch == 0x1b && DCS_just_escaped ) ) { DCS_escape_end( 0, NULL ); }
  DCS_just_escaped = false;
  params.clear();
  csi_overflow = false;
  dispatch_chars.clear();
  parsed = false;
}

/* Empty is -1, which getparam reads as "use the default". A value past
   what an int holds saturates at INT_MAX; handlers that repeat still stop
   at the edge of the screen. */
static int parse_param_value( const std::string& text )
{
  if ( text.empty() ) {
    return -1;
  }
  const size_t digits = text.find_first_not_of( '0' );
  if ( digits == std::string::npos ) {
    return 0;
  }
  errno = 0;
  char* end = nullptr;
  const long val = strtol( text.c_str() + digits, &end, 10 );
  if ( end == text.c_str() + digits ) {
    return -1;
  }
  if ( errno == ERANGE || val > INT_MAX ) {
    return INT_MAX;
  }
  if ( val < 0 ) {
    return 0;
  }
  return static_cast<int>( val );
}

void Dispatcher::parse_params( void )
{
  if ( parsed ) {
    return;
  }

  /* Parameters are separated by ';', and the sub-parameters of one by ':'.
     An empty one is -1, as is one too large to be meant. */
  parsed_params.clear();
  parsed_subparams.clear();
  size_t start = 0;
  while ( true ) {
    size_t end = params.find( ';', start );
    if ( end == std::string::npos ) {
      end = params.size();
    }
    const std::string group = params.substr( start, end - start );
    size_t colon = group.find( ':' );
    parsed_params.push_back( parse_param_value( group.substr( 0, colon ) ) );
    std::vector<int> subs;
    while ( colon != std::string::npos ) {
      const size_t next = group.find( ':', colon + 1 );
      subs.push_back( parse_param_value(
        group.substr( colon + 1, next == std::string::npos ? std::string::npos : next - colon - 1 ) ) );
      colon = next;
    }
    parsed_subparams.push_back( std::move( subs ) );
    if ( end == params.size() ) {
      break;
    }
    start = end + 1;
  }

  parsed = true;
}

bool Dispatcher::has_subparams( void )
{
  parse_params();
  for ( const auto& subs : parsed_subparams ) {
    if ( !subs.empty() ) {
      return true;
    }
  }
  return false;
}

int Dispatcher::subparam_count( size_t N )
{
  parse_params();
  return N < parsed_subparams.size() ? parsed_subparams[N].size() : 0;
}

int Dispatcher::getsubparam( size_t N, size_t K, int defaultval )
{
  parse_params();
  if ( N >= parsed_subparams.size() || K >= parsed_subparams[N].size() || parsed_subparams[N][K] < 0 ) {
    return defaultval;
  }
  return parsed_subparams[N][K];
}

int Dispatcher::getparam( size_t N, int defaultval )
{
  int ret = defaultval;
  if ( !parsed ) {
    parse_params();
  }

  if ( parsed_params.size() > N ) {
    ret = parsed_params[N];
  }

  if ( ret < 1 )
    ret = defaultval;

  return ret;
}

int Dispatcher::param_count( void )
{
  if ( !parsed ) {
    parse_params();
  }

  return parsed_params.size();
}

std::string Dispatcher::str( void )
{
  char assum[64];
  snprintf( assum, 64, "[dispatch=\"%s\" params=\"%s\"]", dispatch_chars.c_str(), params.c_str() );
  return std::string( assum );
}

/* construct on first use to avoid static initialization order crash */
DispatchRegistry& Terminal::get_global_dispatch_registry( void )
{
  static DispatchRegistry global_dispatch_registry;
  return global_dispatch_registry;
}

static void register_function( Function_Type type, const std::string& dispatch_chars, Function f )
{
  switch ( type ) {
    case ESCAPE:
      get_global_dispatch_registry().escape.insert( dispatch_map_t::value_type( dispatch_chars, f ) );
      break;
    case CSI:
      get_global_dispatch_registry().CSI.insert( dispatch_map_t::value_type( dispatch_chars, f ) );
      break;
    case CONTROL:
      get_global_dispatch_registry().control.insert( dispatch_map_t::value_type( dispatch_chars, f ) );
      break;
  }
}

Function::Function( Function_Type type,
                    const std::string& dispatch_chars,
                    void ( *s_function )( Framebuffer*, Dispatcher* ),
                    bool s_clears_wrap_state )
  : function( s_function ), clears_wrap_state( s_clears_wrap_state )
{
  register_function( type, dispatch_chars, *this );
}

void Dispatcher::dispatch( Function_Type type, const Parser::Action* act, Framebuffer* fb )
{
  /* add final char to dispatch key */
  if ( ( type == ESCAPE ) || ( type == CSI ) ) {
    assert( act->char_present );
    Parser::Collect act2;
    act2.char_present = true;
    act2.ch = act->ch;
    collect( &act2 );
  }

  dispatch_map_t* map = NULL;
  switch ( type ) {
    case ESCAPE:
      map = &get_global_dispatch_registry().escape;
      break;
    case CSI:
      map = &get_global_dispatch_registry().CSI;
      break;
    case CONTROL:
      map = &get_global_dispatch_registry().control;
      break;
  }

  if ( type == CSI && csi_overflow ) {
    fb->ds.next_print_will_wrap = false;
    return;
  }

  std::string key = dispatch_chars;
  if ( type == CONTROL ) {
    assert( act->ch <= 255 );
    char ctrlstr[2] = { (char)act->ch, 0 };
    key = std::string( ctrlstr, 1 );
  }

  dispatch_map_t::const_iterator i = map->find( key );
  /* Only SGR takes sub-parameters; anything else with them is not a sequence
     this terminal knows. */
  if ( i == map->end() || ( type == CSI && key != "m" && has_subparams() ) ) {
    /* unknown function */
    fb->ds.next_print_will_wrap = false;
    return;
  }
  if ( i->second.clears_wrap_state ) {
    fb->ds.next_print_will_wrap = false;
  }
  i->second.function( fb, this );
}

void Dispatcher::OSC_put( const Parser::OSC_Put* act )
{
  assert( act->char_present );
  if ( escape_byte_limit && OSC_string.size() >= escape_byte_limit ) {
    OSC_overflow = true;
    return;
  }
  OSC_string.push_back( act->ch );
}

void Dispatcher::OSC_start( const Parser::OSC_Start* act __attribute( ( unused ) ) )
{
  finish_download( false );
  finish_st_frame( false, nullptr );
  DCS_escape_end( 0, NULL );
  OSC_string.clear();
  OSC_overflow = false;
}

std::vector<ClipboardEvent> Dispatcher::take_clipboard_events( void )
{
  std::vector<ClipboardEvent> out;
  out.swap( clipboard_events );
  return out;
}

void Dispatcher::APC_put( const Parser::APC_Put* act )
{
  assert( act->char_present );
  if ( APC_overflow ) {
    return;
  }
  if ( escape_byte_limit && APC_string.size() >= escape_byte_limit ) {
    APC_overflow = true;
    APC_string.clear();
    return;
  }
  APC_string.push_back( static_cast<char>( act->ch ) );
}

void Dispatcher::APC_start( const Parser::APC_Start* act __attribute( ( unused ) ) )
{
  finish_download( false );
  finish_st_frame( false, nullptr );
  DCS_escape_end( 0, NULL );
  APC_string.clear();
  APC_overflow = false;
}

bool Dispatcher::operator==( const Dispatcher& x ) const
{
  return ( params == x.params ) && ( parsed_params == x.parsed_params ) && ( parsed == x.parsed )
         && ( dispatch_chars == x.dispatch_chars ) && ( OSC_string == x.OSC_string )
         && ( OSC_overflow == x.OSC_overflow ) && ( clipboard_events == x.clipboard_events )
         && ( APC_string == x.APC_string )
         && ( kitty_uploading == x.kitty_uploading ) && ( kitty_payload == x.kitty_payload )
         && ( DCS_string == x.DCS_string ) && ( DCS_active == x.DCS_active ) && ( DCS_escape == x.DCS_escape )
         && ( terminal_to_host == x.terminal_to_host );
}

void Dispatcher::DCS_start( wchar_t final )
{
  finish_download( false );
  finish_st_frame( false, nullptr );
  DCS_string.clear();
  DCS_escape = DCS_just_escaped = false;
  /* XTGETTCAP is collected whatever its length; sixel keeps its own bound. */
  DCS_termcap = final == 'q' && dispatch_chars == "+";
  DCS_active = DCS_termcap || ( sixel_enabled && final == 'q' && dispatch_chars.empty() );
  if ( DCS_active && !DCS_termcap ) { DCS_string = "\033P" + params + "q"; }
}

void Dispatcher::DCS_put( wchar_t ch )
{
  if ( !DCS_active ) { return; }
  if ( ch < 0 || ch > 127 || ( !DCS_termcap && escape_byte_limit && DCS_string.size() >= escape_byte_limit ) ) {
    DCS_active = false;
    DCS_string.clear();
    return;
  }
  DCS_string.push_back( char( ch ) );
}

void Dispatcher::DCS_end( wchar_t ch, Framebuffer* fb )
{
  if ( !DCS_active ) { return; }
  DCS_active = false;
  if ( ch == 0x9c ) { finish_dcs( fb ); }
  else if ( ch == 0x1b ) { DCS_escape = DCS_just_escaped = true; }
  else { DCS_string.clear(); }
}

void Dispatcher::DCS_escape_end( wchar_t ch, Framebuffer* fb )
{
  if ( !DCS_escape ) { return; }
  DCS_escape = DCS_just_escaped = false;
  if ( ch == '\\' && fb ) { finish_dcs( fb ); }
  else { DCS_string.clear(); }
}

void Dispatcher::finish_dcs( Framebuffer* fb )
{
  if ( DCS_termcap ) {
    finish_termcap();
  } else {
    finish_sixel( fb );
  }
}

static int hex_value( char c )
{
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* XTGETTCAP: names in hex, separated by semicolons, each answered on its
   own, as kitty answers: with the value of a name the relay knows, and with
   DCS 0 + r for any other, so a program need not wait for a timeout. */
void Dispatcher::finish_termcap()
{
  static const char digits[] = "0123456789ABCDEF";
  size_t start = 0;
  while ( start <= DCS_string.size() ) {
    size_t end = DCS_string.find( ';', start );
    if ( end == std::string::npos ) { end = DCS_string.size(); }
    const std::string hex = DCS_string.substr( start, end - start );
    std::string name, value;
    bool valid = hex.size() % 2 == 0;
    for ( size_t i = 0; valid && i < hex.size(); i += 2 ) {
      const int hi = hex_value( hex[i] ), lo = hex_value( hex[i + 1] );
      valid = hi >= 0 && lo >= 0;
      name += char( hi * 16 + lo );
    }
    if ( valid && ext_relay.termcap( name, value ) ) {
      std::string encoded;
      for ( unsigned char c : value ) { encoded += digits[c >> 4]; encoded += digits[c & 15]; }
      terminal_to_host += "\033P1+r" + hex + "=" + encoded + "\033\\";
    } else {
      terminal_to_host += "\033P0+r" + hex + "\033\\";
    }
    start = end + 1;
  }
  DCS_string.clear();
  DCS_termcap = false;
}

void Dispatcher::finish_sixel( Framebuffer* fb )
{
  DCS_string += "\033\\";
  Sixel::Bitmap bitmap;
  std::string error;
  if ( sixel_enabled && Sixel::decode( DCS_string, bitmap, error, 0,
                                      sixel_private_palette ? nullptr : &sixel_palette ) ) {
    Sixel::place( *fb, bitmap, client_geometry, sixel_display_mode, sixel_cursor_right );
  }
  DCS_string.clear();
}

void Dispatcher::reset_sixel()
{
  DCS_string.clear();
  DCS_active = DCS_escape = DCS_just_escaped = DCS_termcap = false;
  sixel_display_mode = sixel_cursor_right = false;
  sixel_private_palette = true;
  sixel_palette = Sixel::Palette();
}

static void kitty_reply( Dispatcher* dispatch, const KittyCommand& cmd, uint32_t image_id, const std::string& msg,
                         uint32_t frame = 0 )
{
  if ( cmd.quiet >= 2 ) {
    return;
  }
  const bool ok = msg.size() >= 2 && msg[0] == 'O' && msg[1] == 'K';
  if ( ok && cmd.quiet >= 1 ) {
    return;
  }
  dispatch->terminal_to_host.append( kitty_response( image_id, cmd.placement_id, msg, frame ) );
}

static uint32_t resolve_kitty_id( Framebuffer* fb, const KittyCommand& cmd, bool creating )
{
  if ( cmd.image_id != 0 ) {
    return cmd.image_id;
  }
  if ( cmd.image_number != 0 && !creating ) {
    KittyImage* newest = fb->find_newest_kitty_number( cmd.image_number );
    return newest ? newest->id : 0;
  }
  return 0;
}

static uint64_t divide_round_up( uint64_t value, uint64_t divisor )
{
  return value / divisor + ( value % divisor != 0 );
}

static void advance_kitty_cursor( Framebuffer* fb, const KittyPlacement& place, const ClientGeometry* geometry )
{
  if ( place.cursor_hold || place.unicode_placeholder || place.parent_image != 0 ) {
    return;
  }
  const KittyImage* image = fb->find_kitty_image( place.image_id );
  assert( image != NULL );

  /* Geometry is attachment metadata, not image/terminal state. If a terminal
     exposes no pixel geometry, use a conventional 8x16 cell estimate rather
     than treating an arbitrarily large image as a single character. */
  uint64_t cw = 8, ch = 16;
  if ( geometry && geometry->columns == static_cast<uint32_t>( fb->ds.get_width() )
       && geometry->rows == static_cast<uint32_t>( fb->ds.get_height() ) ) {
    if ( geometry->has_cell_size() ) {
      cw = std::min( geometry->cell_width_px, 65535U );
      ch = std::min( geometry->cell_height_px, 65535U );
    } else if ( geometry->has_pixel_size() ) {
      cw = std::max( 1U, std::min( geometry->width_px / geometry->columns, 65535U ) );
      ch = std::max( 1U, std::min( geometry->height_px / geometry->rows, 65535U ) );
    }
  }

  /* Cropping selects the source rectangle; c/r select its displayed size.
     Preserve c/r as supplied for the client: rounding native pixels into an
     explicit cell rectangle would stretch the plot and change its size. */
  const uint64_t source_width = image->width - std::min( place.src_x, image->width );
  const uint64_t source_height = image->height - std::min( place.src_y, image->height );
  const uint64_t width = place.src_w ? std::min<uint64_t>( place.src_w, source_width ) : source_width;
  const uint64_t height = place.src_h ? std::min<uint64_t>( place.src_h, source_height ) : source_height;
  if ( !width || !height ) {
    return;
  }
  const uint64_t x = std::min<uint64_t>( place.cell_x, cw - 1 );
  const uint64_t y = std::min<uint64_t>( place.cell_y, ch - 1 );
  uint64_t cols = place.columns, rows = place.rows;
  if ( !cols && !rows ) {
    cols = divide_round_up( width + x, cw );
    rows = divide_round_up( height + y, ch );
  } else if ( !rows ) {
    rows = divide_round_up( divide_round_up( ( cols * cw - x ) * height, width ) + y, ch );
  } else if ( !cols ) {
    cols = divide_round_up( divide_round_up( ( rows * ch - y ) * width, height ) + x, cw );
  }

  /* Kitty leaves out-of-screen cursor positioning implementation-defined.
     Keep our existing clamp-to-edge policy, bounding unsigned protocol values
     before converting to int so large c/r cannot wrap or move backwards. */
  fb->ds.move_col( static_cast<int>( std::min<uint64_t>( cols, fb->ds.get_width() ) ), true, false );
  fb->ds.move_row( static_cast<int>( std::min<uint64_t>( rows, fb->ds.get_height() ) ), true );
}

/* a=e: records applied to an existing vector image as a patch, as a whole
   or, when one is malformed, not at all. */
void Dispatcher::finish_vector_edit( const KittyCommand& cmd, const std::string& data, Framebuffer* fb )
{
  /* Silkie's answers, word for word: CODE:message, with no space. */
  if ( !cmd.image_id && !cmd.image_number ) {
    return; /* "Vector edit command without image id or number" is only logged */
  }
  const uint32_t id = resolve_kitty_id( fb, cmd, false );
  const KittyImage* image = id ? fb->find_kitty_image( id ) : NULL;
  if ( !image ) {
    kitty_reply( this, cmd, cmd.image_id,
                 "ENOENT:Vector edit command refers to non-existent image with id: " + std::to_string( cmd.image_id )
                   + " and number: " + std::to_string( cmd.image_number ) );
    return;
  }
  if ( image->format != KITTY_FORMAT_VECTOR ) {
    kitty_reply( this, cmd, id, "EINVAL:Not a vector image" );
    return;
  }
  std::vector<Vector::Record> records;
  std::string error;
  const Vector::FontCheck fonts = ext_relay.font_check( *fb );
  if ( !Vector::parse_records( data, records, error, &fonts ) ) {
    kitty_reply( this, cmd, id, "EINVAL:" + error );
    return;
  }
  Vector::Step step;
  step.records = data;
  fb->edit_vector_image( id, step, error );
  fb->prune_vector_fonts();
  kitty_reply( this, cmd, id, error.empty() ? "OK" : "EINVAL:" + error );
}

bool Dispatcher::replace_image_pixels( const KittyImage& existing, const std::string& before,
                                      const std::string& after, Framebuffer* fb, std::string& error )
{
  const uint32_t width = existing.width, height = existing.height;
  KittyImage image;
  /* An image that came packed stays exact through its edits. */
  const ImageEncoding encoding
    = kitty_image_is_packed( existing ) ? kitty_exact_encoding( fb->image_encoding ) : fb->image_encoding;
  if ( !kitty_normalize_image( KITTY_FORMAT_RGBA, width, height, after, image, encoding, error ) ) {
    return false;
  }
  uint32_t x = 0, y = 0, w = width, h = height;
  if ( kitty_dirty_rect( before, after, width, height, x, y, w, h ) && uint64_t( w ) * h < uint64_t( width ) * height ) {
    std::string pixels;
    pixels.reserve( size_t( w ) * h * 4 );
    for ( uint32_t row = y; row < y + h; row++ ) {
      pixels.append( after, ( size_t( row ) * width + x ) * 4, size_t( w ) * 4 );
    }
    auto delta = std::make_shared<ImageDelta>();
    delta->base_serial = existing.serial;
    delta->x = x;
    delta->y = y;
    std::string ignored;
    if ( kitty_normalize_image( KITTY_FORMAT_RGBA, w, h, pixels, delta->rect, encoding, ignored ) ) {
      image.delta = delta;
    }
  }
  image.id = existing.id;
  image.number = existing.number;
  image.origin = existing.origin;
  image.gamut = existing.gamut;
  image.surface = existing.surface;
  return fb->refresh_kitty_image( image );
}

/* a=r: the surface behind an image is copied again, from the handle taken
   when it was transmitted. */
void Dispatcher::refresh_gpu_surface( const KittyCommand& cmd, Framebuffer* fb )
{
  const uint32_t id = resolve_kitty_id( fb, cmd, false );
  const KittyImage* existing = id ? fb->find_kitty_image( id ) : NULL;
  if ( !existing ) {
    kitty_reply( this, cmd, cmd.image_id, "ENOENT:image not found" );
    return;
  }
  if ( !existing->surface ) {
    kitty_reply( this, cmd, id, "EINVAL:not a GPU surface" );
    return;
  }
  std::string rgba, error;
  uint32_t width = 0, height = 0;
  if ( !read_gpu_surface( *existing->surface, rgba, width, height, error ) ) {
    kitty_reply( this, cmd, id, error );
    return;
  }
  /* A surface that did not change is not sent again; one that did, only
     where it did, to a client that has the version before. */
  std::string before;
  uint32_t x = 0, y = 0, w = width, h = height;
  const bool same_size = width == existing->width && height == existing->height
                         && kitty_image_to_rgba( *existing, before );
  if ( same_size && !kitty_dirty_rect( before, rgba, width, height, x, y, w, h ) ) {
    kitty_reply( this, cmd, id, "OK" );
    return;
  }
  if ( same_size ) {
    if ( !replace_image_pixels( *existing, before, rgba, fb, error ) ) {
      kitty_reply( this, cmd, id, error );
      return;
    }
  } else {
    KittyImage image;
    if ( !kitty_normalize_image( KITTY_FORMAT_RGBA, width, height, rgba, image, fb->image_encoding, error ) ) {
      kitty_reply( this, cmd, id, error );
      return;
    }
    image.id = id;
    image.number = existing->number;
    image.gamut = existing->gamut;
    image.surface = existing->surface;
    fb->refresh_kitty_image( image );
  }
  kitty_reply( this, cmd, id, "OK" );
}

namespace {
/* The pixels of a frame as an image keeps them: straight RGBA in the gamut of
   the image, or RGBA half floats. False with the reply when they cannot be. */
bool frame_pixels( const KittyCommand& cmd, const std::string& data, const KittyImage& image, std::string& pixels,
                   uint32_t& width, uint32_t& height, std::string& error )
{
  char message[160];
  if ( cmd.format == KITTY_FORMAT_PNG ) {
    uint32_t gamut = 0;
    if ( !kitty_png_to_rgba( data, pixels, width, height, gamut ) ) {
      error = "EINVAL:invalid PNG image";
      return false;
    }
    convert_pixels_between_gamuts( gamut, image.gamut, pixels, 4 );
    return true;
  }
  if ( packed_format( cmd.format ) ) {
    /* Packed pixels are expanded as they enter a frame, as Silkie expands
       them; grayscale has no gamut. */
    size_t row = 0, size = 0;
    if ( !cmd.width || !cmd.height ) {
      error = "EINVAL:Zero width/height not allowed";
      return false;
    }
    if ( !packed_size( cmd.format, cmd.width, cmd.height, row, size ) ) {
      error = "EINVAL:pixel data size does not match dimensions";
      return false;
    }
    if ( data.size() < size ) {
      snprintf( message, sizeof message, "ENODATA:Insufficient image data %zu < %zu", data.size(), size );
      error = message;
      return false;
    }
    if ( !packed_expand( cmd.format, cmd.width, cmd.height, data.substr( 0, size ), true, pixels ) ) {
      error = "EINVAL:pixel data size does not match dimensions";
      return false;
    }
    width = cmd.width;
    height = cmd.height;
    convert_pixels_between_gamuts( cmd.format == 516 && cmd.gamut <= 2 ? cmd.gamut : 0, image.gamut, pixels, 4 );
    return true;
  }
  const size_t bytes_per_pixel = cmd.format == KITTY_FORMAT_RGB ? 3 : cmd.format == KITTY_FORMAT_HDR ? 8 : 4;
  if ( cmd.format != KITTY_FORMAT_RGB && cmd.format != KITTY_FORMAT_RGBA && cmd.format != KITTY_FORMAT_HDR ) {
    snprintf( message, sizeof message, "EINVAL:Unknown image format: %u", cmd.format );
    error = message;
    return false;
  }
  size_t size = 0;
  if ( !cmd.width || !cmd.height ) {
    error = "EINVAL:Zero width/height not allowed";
    return false;
  }
  if ( !graphics_pixel_size( cmd.width, cmd.height, bytes_per_pixel, size ) ) {
    error = "EINVAL:pixel data size does not match dimensions";
    return false;
  }
  if ( data.size() < size ) {
    snprintf( message, sizeof message, "ENODATA:Insufficient image data %zu < %zu", data.size(), size );
    error = message;
    return false;
  }
  width = cmd.width;
  height = cmd.height;
  if ( bytes_per_pixel != 3 ) {
    pixels.assign( data, 0, size );
  } else {
    pixels.assign( size_t( width ) * height * 4, char( 0xff ) );
    for ( size_t i = 0, n = size_t( width ) * height; i < n; i++ ) {
      pixels.replace( i * 4, 3, data, i * 3, 3 );
    }
  }
  if ( cmd.format != KITTY_FORMAT_HDR ) {
    /* The frames of an image are in its gamut. */
    convert_pixels_between_gamuts( cmd.gamut <= 2 ? cmd.gamut : 0, image.gamut, pixels, 4 );
  }
  return true;
}

/* The root frame of an image, to edit: RGBA, or half floats for HDR. */
bool root_frame( const KittyImage& image, std::string& pixels, uint32_t& gamut )
{
  gamut = image.gamut;
  if ( image.format == KITTY_FORMAT_HDR ) {
    if ( !image.data ) {
      return false;
    }
    pixels = *image.data;
    return true;
  }
  if ( image.png_original && image.data ) {
    uint32_t width = 0, height = 0;
    return kitty_png_to_rgba( *image.data, pixels, width, height, gamut ) && width == image.width
           && height == image.height;
  }
  return kitty_image_to_rgba( image, pixels );
}

/* Stores an edited root frame. */
bool store_root_frame( Dispatcher* dispatch, const KittyImage& existing, const std::string& before,
                       const std::string& after, uint32_t gamut, Framebuffer* fb, std::string& error )
{
  if ( existing.format == KITTY_FORMAT_HDR ) {
    KittyImage image = existing;
    image.data = std::make_shared<std::string>( after );
    image.delta.reset();
    return fb->refresh_kitty_image( image );
  }
  if ( gamut != existing.gamut || existing.png_original ) {
    /* A PNG that kept its color profile is an RGB image in its gamut now. */
    KittyImage image;
    if ( !kitty_normalize_image( KITTY_FORMAT_RGBA, existing.width, existing.height, after, image, fb->image_encoding,
                                 error ) ) {
      return false;
    }
    image.id = existing.id;
    image.number = existing.number;
    image.origin = existing.origin;
    image.gamut = gamut;
    return fb->refresh_kitty_image( image );
  }
  return dispatch->replace_image_pixels( existing, before, after, fb, error );
}
}

/* a=f: kitty adds a frame to an image, or edits one. An image here has only
   its root frame, which a=f with r=1 edits; the terminal is sent the result. */
void Dispatcher::finish_animation_frame( const KittyCommand& cmd, const std::string& data, Framebuffer* fb )
{
  const uint32_t id = resolve_kitty_id( fb, cmd, false );
  const KittyImage* image = id ? fb->find_kitty_image( id ) : NULL;
  char message[160];
  if ( !image ) {
    snprintf( message, sizeof message, "ENOENT:Animation command refers to non-existent image with id: %u and number: %u",
              cmd.image_id, cmd.image_number );
    kitty_reply( this, cmd, cmd.image_id, message, cmd.rows );
    return;
  }
  if ( image->native || kitty_native_format( cmd.format ) || ( cmd.transfer && cmd.transfer != 13 ) ) {
    kitty_reply( this, cmd, id, "EINVAL:Animation is not supported for indexed, palette or precision images", cmd.rows );
    return;
  }
  if ( image->format == KITTY_FORMAT_VECTOR ) {
    kitty_reply( this, cmd, id, "EINVAL:Animation frames are not supported for vector images", cmd.rows );
    return;
  }
  /* The number of a frame: past the last, or none, is a new frame. */
  const uint32_t frame = cmd.rows == 1 ? 1 : 2;
  if ( ( image->format == KITTY_FORMAT_HDR ) != ( cmd.format == KITTY_FORMAT_HDR ) ) {
    kitty_reply( this, cmd, id, "EINVAL:Animation frame format must match the image format", frame );
    return;
  }
  if ( image->surface ) {
    kitty_reply( this, cmd, id, "EINVAL:Animation frames are not supported for GPU surface images", frame );
    return;
  }
  if ( frame != 1 ) {
    kitty_reply( this, cmd, id, "EINVAL:Animation frames other than the root frame are not supported", frame );
    return;
  }
  std::string pixels, error;
  uint32_t width = 0, height = 0;
  if ( !frame_pixels( cmd, data, *image, pixels, width, height, error ) ) {
    kitty_reply( this, cmd, id, error, frame );
    return;
  }
  if ( width > image->width ) {
    snprintf( message, sizeof message, "EINVAL:Frame width %u larger than image width: %u", width, image->width );
    kitty_reply( this, cmd, id, message, frame );
    return;
  }
  if ( height > image->height ) {
    snprintf( message, sizeof message, "EINVAL:Frame height %u larger than image height: %u", height, image->height );
    kitty_reply( this, cmd, id, message, frame );
    return;
  }
  std::string before, after;
  uint32_t gamut = 0;
  if ( !root_frame( *image, before, gamut ) ) {
    kitty_reply( this, cmd, id, "EINVAL:No data associated with frame number: 1", frame );
    return;
  }
  if ( gamut != image->gamut ) {
    convert_pixels_between_gamuts( image->gamut, gamut, pixels, 4 );
  }
  after = before;
  const bool half = image->format == KITTY_FORMAT_HDR;
  /* X=1 replaces; otherwise a frame with alpha is blended over the root. */
  kitty_compose( after, image->width, image->height, pixels, width, height, cmd.src_x, cmd.src_y,
                 cmd.cell_x != 1 && cmd.format != KITTY_FORMAT_RGB, half );
  if ( after != before && !store_root_frame( this, *image, before, after, gamut, fb, error ) ) {
    kitty_reply( this, cmd, id, error.empty() ? "ENOSPC:Failed to cache data for image frame" : error, frame );
    return;
  }
  kitty_reply( this, cmd, id, "OK", frame );
}

/* a=c: a rectangle of one frame composed onto another; here both are the
   root frame. */
void Dispatcher::compose_frames( const KittyCommand& cmd, Framebuffer* fb )
{
  const uint32_t id = resolve_kitty_id( fb, cmd, false );
  const KittyImage* image = id ? fb->find_kitty_image( id ) : NULL;
  char message[160];
  if ( !image ) {
    snprintf( message, sizeof message, "ENOENT:Animation command refers to non-existent image with id: %u and number: %u",
              cmd.image_id, cmd.image_number );
    kitty_reply( this, cmd, cmd.image_id, message );
    return;
  }
  if ( image->native ) {
    kitty_reply( this, cmd, id, "EINVAL:Composition is not supported for indexed, palette or precision images" );
    return;
  }
  if ( image->format == KITTY_FORMAT_VECTOR ) {
    kitty_reply( this, cmd, id, "EINVAL:Composition is not supported for vector images" );
    return;
  }
  if ( cmd.rows != 1 ) {
    snprintf( message, sizeof message, "ENOENT:No source frame number %u exists in image id: %u\n", cmd.rows, id );
    kitty_reply( this, cmd, id, message );
    return;
  }
  if ( cmd.columns != 1 ) {
    snprintf( message, sizeof message, "ENOENT:No destination frame number %u exists in image id: %u\n", cmd.columns,
              id );
    kitty_reply( this, cmd, id, message );
    return;
  }
  if ( image->surface ) {
    kitty_reply( this, cmd, id, "EINVAL:Composition is not supported for GPU surface images" );
    return;
  }
  const uint64_t width = cmd.src_w ? cmd.src_w : image->width, height = cmd.src_h ? cmd.src_h : image->height;
  const uint64_t dest_x = cmd.src_x, dest_y = cmd.src_y, src_x = cmd.cell_x, src_y = cmd.cell_y;
  if ( dest_x + width > image->width || dest_y + height > image->height ) {
    kitty_reply( this, cmd, id, "EINVAL:The destination rectangle is out of bounds" );
    return;
  }
  if ( src_x + width > image->width || src_y + height > image->height ) {
    kitty_reply( this, cmd, id, "EINVAL:The source rectangle is out of bounds" );
    return;
  }
  const bool x_overlaps = std::max( src_x, dest_x ) < std::min( src_x, dest_x ) + width;
  const bool y_overlaps = std::max( src_y, dest_y ) < std::min( src_y, dest_y ) + height;
  if ( x_overlaps && y_overlaps ) {
    kitty_reply( this, cmd, id,
                 "EINVAL:The source and destination rectangles overlap and the src and destination frames are the same" );
    return;
  }
  std::string before, error;
  uint32_t gamut = 0;
  if ( !root_frame( *image, before, gamut ) ) {
    kitty_reply( this, cmd, id, "EINVAL:Failed to get data for src frame: 0" );
    return;
  }
  std::string after = before;
  kitty_compose_within( after, image->width, uint32_t( src_x ), uint32_t( src_y ), uint32_t( dest_x ),
                        uint32_t( dest_y ), uint32_t( width ), uint32_t( height ), !cmd.cursor_hold,
                        image->format == KITTY_FORMAT_HDR );
  if ( after != before && !store_root_frame( this, *image, before, after, gamut, fb, error ) ) {
    kitty_reply( this, cmd, id, error.empty() ? "ENOSPC:Failed to store image data in cache" : error );
    return;
  }
  kitty_reply( this, cmd, id, "OK" );
}

/* a=a: with one frame there is nothing to animate, and kitty answers only
   when the image does not exist. */
void Dispatcher::control_animation( const KittyCommand& cmd, Framebuffer* fb )
{
  const uint32_t id = resolve_kitty_id( fb, cmd, false );
  if ( !id || !fb->find_kitty_image( id ) ) {
    char message[160];
    snprintf( message, sizeof message, "ENOENT:Animation command refers to non-existent image with id: %u and number: %u",
              cmd.image_id, cmd.image_number );
    kitty_reply( this, cmd, cmd.image_id, message, cmd.rows );
  }
}

/* The F key of a placement: a terminal behind the relay that does not know it
   would drop the whole command, and bits nobody knows are refused. */
static bool placement_flags_ok( Dispatcher* dispatch, const KittyCommand& cmd )
{
  if ( !cmd.flags ) {
    return true;
  }
  if ( ( cmd.flags & ~KITTY_PLACE_KNOWN ) || cmd.unicode_placeholder ) {
    kitty_reply( dispatch, cmd, cmd.image_id, "EINVAL:F has bits that cannot be used here" );
    return false;
  }
  if ( !dispatch->ext_relay.client.has_graphics( "viewport" ) ) {
    kitty_reply( dispatch, cmd, cmd.image_id, "EINVAL:the terminal has no viewport placements" );
    return false;
  }
  return true;
}

/* The placement a=p or a=T makes, at the cursor. */
static KittyPlacement placement_of( const KittyCommand& cmd, uint32_t image_id, Framebuffer* fb )
{
  KittyPlacement place;
  place.image_id = image_id;
  place.placement_id = cmd.placement_id;
  place.row = fb->ds.get_cursor_row();
  place.col = fb->ds.get_cursor_col();
  place.columns = cmd.columns;
  place.rows = cmd.rows;
  place.src_x = cmd.src_x;
  place.src_y = cmd.src_y;
  place.src_w = cmd.src_w;
  place.src_h = cmd.src_h;
  place.cell_x = cmd.cell_x;
  place.cell_y = cmd.cell_y;
  place.z = cmd.z;
  place.cursor_hold = cmd.cursor_hold != 0 || cmd.parent_image != 0;
  place.unicode_placeholder = cmd.unicode_placeholder != 0;
  place.parent_image = cmd.parent_image;
  place.parent_placement = cmd.parent_placement;
  place.H = cmd.H;
  place.V = cmd.V;
  place.flags = cmd.flags;
  /* A placement is made at the depth and phase the text has just then. */
  const TextStyle& style = text_style_of( fb->ds.get_renditions().get_text_style() );
  place.depth = style.depth;
  place.depth_pixels = style.depth_pixels;
  place.phase = style.phase;
  return place;
}

void Dispatcher::finish_kitty_upload( Framebuffer* fb )
{
  KittyCommand cmd = kitty_partial;
  cmd.payload.swap( kitty_payload );
  const bool chunked = kitty_chunked;
  kitty_uploading = false;
  kitty_chunked = false;
  kitty_payload.clear();

  if ( cmd.action == KittyService
       && ( !cmd.image_id || cmd.format != KITTY_FORMAT_VECTOR || cmd.image_number || cmd.medium == 'g' ) ) {
    kitty_reply( this, cmd, cmd.image_id, "EINVAL:Vector service requires a=v,f=200 and a request id" );
    return;
  }
  if ( cmd.action == KittyQuery && !placement_flags_ok( this, cmd ) ) {
    return;
  }

  try {
    std::string data, error;
    if ( !kitty_transfer_valid( cmd.format, cmd.transfer ) || ( cmd.medium == 'g' && cmd.transfer ) ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:transfer function is not supported for this format" );
      return;
    }
    std::shared_ptr<GpuSurface> surface;
    if ( cmd.medium == 'g' && ( cmd.action == KittyFrame || cmd.action == KittyEdit || cmd.action == KittyService ) ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:Unknown transmission type: g" );
      return;
    }
    if ( cmd.medium == 'g' ) {
      /* A GPU surface is read here, where the program is, and its pixels are
         what reach the client. The surface is held from now on, so that a=r
         copies the same buffer however the program fares with its own handle. */
      if ( cmd.compression || chunked ) {
        kitty_reply( this, cmd, cmd.image_id, "EINVAL:chunking and compression are not allowed with t=g" );
        return;
      }
      uint32_t width = 0, height = 0;
      surface = open_gpu_surface( cmd.payload, cmd.width, cmd.height, error );
      if ( !surface || !read_gpu_surface( *surface, data, width, height, error ) ) {
        kitty_reply( this, cmd, cmd.image_id, error );
        return;
      }
      cmd.format = KITTY_FORMAT_RGBA;
      cmd.width = width;
      cmd.height = height;
    } else if ( !kitty_read_medium( cmd, data, error ) ) {
      kitty_reply( this, cmd, cmd.image_id, error.empty() ? "EINVAL:transmit failed" : error );
      return;
    }

    if ( cmd.compression == 'z' ) {
      std::string inflated;
      if ( !kitty_inflate( data, inflated, 0 ) ) {
        kitty_reply( this, cmd, cmd.image_id, "EINVAL:zlib inflate failed" );
        return;
      }
      data.swap( inflated );
    } else if ( cmd.compression != 0 ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:unsupported compression" );
      return;
    }

    if ( cmd.action == KittyEdit ) {
      const uint32_t id = resolve_kitty_id( fb, cmd, false );
      const KittyImage* old = id ? fb->find_kitty_image( id ) : nullptr;
      if ( old && old->native && kitty_palette_format( old->format ) ) {
        const uint32_t slots = 1u << ( old->format - 400 );
        const uint32_t count = cmd.src_w ? cmd.src_w : ( cmd.src_x < slots ? slots - cmd.src_x : 0 );
        if ( cmd.transfer || cmd.src_x >= slots || !count || count > slots - cmd.src_x || ( cmd.gamut_set && cmd.gamut != old->gamut ) ) {
          kitty_reply( this, cmd, id, "EINVAL:palette range or gamut does not match" );
        } else if ( data.size() != size_t( count ) * 4 ) {
          kitty_reply( this, cmd, id, data.size() < size_t( count ) * 4 ? "ENODATA:palette slots missing" : "EFBIG:too many palette bytes" );
        } else {
          KittyImage updated = *old;
          updated.data = std::make_shared<std::string>( *old->data );
          updated.data->replace( size_t( cmd.src_x ) * 4, size_t( count ) * 4, data );
          fb->refresh_kitty_image( updated );
          kitty_reply( this, cmd, id, "OK" );
        }
        return;
      }
      finish_vector_edit( cmd, data, fb );
      return;
    }

    if ( cmd.action == KittyFrame ) {
      finish_animation_frame( cmd, data, fb );
      return;
    }

    if ( cmd.action == KittyService ) {
      ext_relay.vector_service( cmd.image_id, cmd.quiet, data, *fb, terminal_to_host );
      return;
    }

    /* State keeps an image as the program sent it: HDR pixels with their T,
       RGB pixels with their gamut. The client converts them for a terminal
       that cannot take them as they are when it draws them, so a terminal
       that turns out to take them later is sent them as they are. */
    KittyImage image;
    const bool wide = ( cmd.format == KITTY_FORMAT_RGB || cmd.format == KITTY_FORMAT_RGBA || cmd.format == 516 )
                      && ( cmd.gamut == 1 || cmd.gamut == 2 );
    const bool native = kitty_native_format( cmd.format ) || ( cmd.transfer && cmd.transfer != 13 && cmd.format != KITTY_FORMAT_HDR );
    if ( native ) {
      if ( !fb->image_encoding.native || ( ( cmd.format == 8 || cmd.format == 16 )
           && !ext_relay.client.has_format( std::to_string( cmd.format ) ) ) ) {
        kitty_reply( this, cmd, cmd.image_id, "ENOTSUP:client does not support this image format" );
        return;
      }
      size_t size = 0;
      if ( !kitty_native_size( cmd.format, cmd.width, cmd.height, size ) ) {
        kitty_reply( this, cmd, cmd.image_id, !cmd.width || !cmd.height ? "EINVAL:s and v are required" : "EOVERFLOW:image size" );
        return;
      }
      if ( data.size() != size ) {
        kitty_reply( this, cmd, cmd.image_id, data.size() < size ? "ENODATA:pixel data missing" : "EFBIG:too many pixel bytes" );
        return;
      }
      image.native = true;
      image.format = cmd.format;
      image.width = cmd.width;
      image.height = cmd.height;
      image.transfer = cmd.transfer;
      image.tone_map = cmd.tone_map == 1;
      image.gamut = ( cmd.format == 316 || ( cmd.format >= 301 && cmd.format <= 308 ) || cmd.format == 8 || cmd.format == 16 )
                      ? 0 : cmd.gamut <= 2 ? cmd.gamut : 0;
      image.data = std::make_shared<std::string>( std::move( data ) );
    } else if ( cmd.format == KITTY_FORMAT_VECTOR || cmd.format == KITTY_FORMAT_HDR ) {
      /* A vector image reaches the terminal as it was sent, so it is accepted
         only when the client's terminal draws them. An HDR image sent anyway
         is shown at SDR, so only a query says HDR is not there. */
      if ( !ext_relay.accepts_format( cmd.format )
           && ( cmd.format == KITTY_FORMAT_VECTOR || cmd.action == KittyQuery ) ) {
        kitty_reply( this, cmd, cmd.image_id, "EINVAL:unsupported Kitty image format" );
        return;
      }
      if ( !cmd.width || !cmd.height ) {
        kitty_reply( this, cmd, cmd.image_id,
                     cmd.format == KITTY_FORMAT_VECTOR ? "EINVAL:Zero width/height not allowed"
                                                       : "EINVAL:s and v are required" );
        return;
      }
      image.format = cmd.format;
      image.width = cmd.width;
      image.height = cmd.height;
      if ( cmd.format == KITTY_FORMAT_VECTOR ) {
        std::vector<Vector::Record> records;
        auto table = std::make_shared<Vector::Table>();
        const Vector::FontCheck fonts = ext_relay.font_check( *fb );
        if ( !Vector::parse_records( data, records, error, &fonts ) || !( error = table->apply( records ) ).empty() ) {
          kitty_reply( this, cmd, cmd.image_id, "EINVAL:" + error );
          return;
        }
        image.vector = Vector::make_doc( Vector::new_lineage(), 0, cmd.width, cmd.height, table );
      } else {
        size_t size = 0;
        if ( !graphics_pixel_size( cmd.width, cmd.height, 8, size ) || data.size() != size ) {
          kitty_reply( this, cmd, cmd.image_id, "EINVAL:pixel data size does not match dimensions" );
          return;
        }
        image.data = std::make_shared<std::string>( std::move( data ) );
      }
      image.tone_map = cmd.tone_map == 1;
    } else if ( !kitty_normalize_image( cmd.format, cmd.width, cmd.height, data, image, fb->image_encoding, error ) ) {
      kitty_reply( this, cmd, cmd.image_id, error );
      return;
    } else if ( wide ) {
      image.gamut = cmd.gamut; /* any other value is sRGB */
    }

    if ( cmd.action == KittyTransmitAndDisplay && !placement_flags_ok( this, cmd ) ) {
      return;
    }

    const bool query_only = cmd.action == KittyQuery;
    if ( query_only ) {
      kitty_reply( this, cmd, cmd.image_id ? cmd.image_id : 1, "OK" );
      return;
    }

    if ( cmd.action != KittyTransmit && cmd.action != KittyTransmitAndDisplay ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:unexpected action at upload end" );
      return;
    }

    image.id = cmd.image_id;
    image.number = cmd.image_number;
    image.surface = surface;
    image.id = fb->put_kitty_image( image );
    fb->prune_vector_fonts(); /* the scene it replaced may have named a released font */
    if ( !image.id ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:could not store image" );
      return;
    }

    kitty_reply( this, cmd, image.id, "OK" );

    if ( cmd.action == KittyTransmitAndDisplay ) {
      KittyPlacement place = placement_of( cmd, image.id, fb );
      /* kitty has answered for the image by now: a placement it may not make
         is not made, and nothing more is said. */
      if ( ( cmd.unicode_placeholder && cmd.parent_image ) || !kitty_check_parent( *fb, place ).empty() ) {
        return;
      }
      fb->put_kitty_placement( place );
      advance_kitty_cursor( fb, place, client_geometry );
    }
  } catch ( const std::bad_alloc& ) {
    kitty_reply( this, cmd, cmd.image_id, "ENOMEM:image allocation failed" );
  } catch ( const std::length_error& ) {
    kitty_reply( this, cmd, cmd.image_id, "EOVERFLOW:image allocation size" );
  }
}

void Dispatcher::APC_dispatch( const Parser::APC_End* act __attribute( ( unused ) ), Framebuffer* fb )
{
  if ( APC_overflow || APC_string.empty() ) {
    APC_overflow = false;
    APC_string.clear();
    kitty_uploading = false;
    kitty_payload.clear();
    return;
  }

  KittyCommand cmd;
  if ( !parse_kitty_command( APC_string, cmd ) ) {
    if ( kitty_uploading ) {
      kitty_reply( this, kitty_partial, kitty_partial.image_id, "EINVAL:malformed Kitty continuation" );
      kitty_uploading = false;
      kitty_payload.clear();
    }
    APC_string.clear();
    return;
  }
  APC_string.clear();

  if ( cmd.feature ) {
    // A feature query must not complete or interrupt a chunked upload.
    if ( cmd.action != KittyQuery || !cmd.image_id ) { return; }
    std::string reply = "\033_Gi=" + std::to_string( cmd.image_id );
    const char action = cmd.feature;
    if ( std::string( "tTpqdefacrv" ).find( action ) != std::string::npos ) {
      reply += std::string( ",e=" ) + action;
      const auto key = [&]( const char* name, uint64_t value ) {
        if ( value ) { reply += std::string( "," ) + name + "=" + std::to_string( value ); }
      };
      const bool upload = action == 't' || action == 'T' || action == 'q' || action == 'f';
      const bool indexed = cmd.format == 8 || cmd.format == 16;
      const bool native = kitty_native_format( cmd.format );
      const bool format = ( ext_relay.accepts_format( cmd.format )
                            || ( native && fb->image_encoding.native && ( !indexed || ext_relay.client.has_format( std::to_string( cmd.format ) ) ) ) )
                          && ( action != 'f' || ( !native && cmd.format != KITTY_FORMAT_VECTOR ) );
      if ( upload ) {
        if ( format && cmd.format != KITTY_FORMAT_RGBA ) { key( "f", cmd.format ); }
        key( "s", cmd.width ); key( "v", cmd.height );
        if ( std::string( "dfts" ).find( cmd.medium ) != std::string::npos || ( cmd.medium == 'g' && gpu_surfaces_supported() && action != 'f' ) ) {
          if ( cmd.medium != 'd' ) { reply += std::string( ",t=" ) + cmd.medium; }
          key( "S", cmd.data_size ); key( "O", cmd.data_offset );
        }
        if ( cmd.compression == 'z' ) { reply += ",o=z"; }
        if ( format && !indexed ) { key( "G", cmd.gamut <= 2 ? cmd.gamut : 0 ); }
        if ( format && cmd.medium != 'g' && kitty_transfer_valid( cmd.format, cmd.transfer )
             && ( !cmd.transfer || cmd.format == KITTY_FORMAT_HDR || cmd.transfer == 13 || fb->image_encoding.native ) ) {
          key( "E", cmd.transfer );
        }
        if ( format && !indexed && !kitty_palette_format( cmd.format ) ) { key( "T", cmd.tone_map == 1 ? 1 : 0 ); }
      }
      if ( action == 'T' || action == 'p' || action == 'f' || ( action == 'e' && fb->image_encoding.native ) ) {
        key( "x", cmd.src_x ); key( "y", cmd.src_y ); key( "w", cmd.src_w ); key( "h", cmd.src_h );
      }
      if ( action == 'T' || action == 'p' ) {
        key( "p", cmd.placement_id ); key( "c", cmd.columns ); key( "r", cmd.rows );
        key( "C", cmd.cursor_hold ); key( "U", cmd.unicode_placeholder );
        key( "P", cmd.parent_image ); key( "Q", cmd.parent_placement );
        if ( ext_relay.client.has_graphics( "viewport" ) && !( cmd.flags & ~KITTY_PLACE_KNOWN ) ) { key( "F", cmd.flags ); }
      }
      if ( action == 'f' || action == 'a' || action == 'c' ) { key( "r", cmd.rows == 1 ? 1 : 0 ); }
    }
    terminal_to_host += reply + ";OK\033\\";
    return;
  }

  if ( kitty_uploading ) {
    if ( cmd.action == KittyDelete ) {
      kitty_uploading = false;
      kitty_payload.clear();
      fb->delete_kitty( cmd, client_geometry );
      fb->prune_vector_fonts();
      return;
    }
    kitty_payload.append( cmd.payload );
    kitty_chunked = true;
    if ( cmd.more == 0 ) {
      finish_kitty_upload( fb );
    }
    return;
  }

  if ( cmd.action == KittyDelete ) {
    fb->delete_kitty( cmd, client_geometry );
    fb->prune_vector_fonts();
    return;
  }

  if ( cmd.action == KittyPut ) {
    if ( cmd.unicode_placeholder && cmd.parent_image ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:Put command creating a virtual placement cannot refer to a parent" );
      return;
    }
    uint32_t id = resolve_kitty_id( fb, cmd, false );
    if ( id == 0 || fb->find_kitty_image( id ) == NULL ) {
      char message[160];
      snprintf( message, sizeof message, "ENOENT:Put command refers to non-existent image with id: %u and number: %u",
                cmd.image_id, cmd.image_number );
      kitty_reply( this, cmd, cmd.image_id, message );
      return;
    }
    if ( !placement_flags_ok( this, cmd ) ) {
      return;
    }
    KittyPlacement place = placement_of( cmd, id, fb );
    /* What the terminal would refuse is refused here, as kitty words it. */
    const std::string refused = kitty_check_parent( *fb, place );
    if ( !refused.empty() ) {
      kitty_reply( this, cmd, cmd.image_id, refused );
      return;
    }
    fb->put_kitty_placement( place );
    kitty_reply( this, cmd, id, "OK" );
    advance_kitty_cursor( fb, place, client_geometry );
    return;
  }

  if ( cmd.action == KittyQuery || cmd.action == KittyTransmit || cmd.action == KittyTransmitAndDisplay
       || cmd.action == KittyEdit || cmd.action == KittyService || cmd.action == KittyFrame ) {
    if ( cmd.image_id != 0 && cmd.image_number != 0 ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL:both i and I specified" );
      return;
    }
    kitty_uploading = true;
    kitty_chunked = cmd.more != 0;
    kitty_partial = cmd;
    kitty_payload = cmd.payload;
    if ( cmd.more == 0 ) {
      finish_kitty_upload( fb );
    }
    return;
  }

  if ( cmd.action == KittyRefresh ) {
    refresh_gpu_surface( cmd, fb );
    return;
  }

  if ( cmd.action == KittyCompose ) {
    compose_frames( cmd, fb );
    return;
  }

  if ( cmd.action == KittyAnimate ) {
    control_animation( cmd, fb );
  }
}
