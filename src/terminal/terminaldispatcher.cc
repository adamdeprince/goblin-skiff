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

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/terminal/kittygraphics.h"
#include "src/terminal/parseraction.h"
#include "src/terminal/terminalframebuffer.h"
#include "terminaldispatcher.h"

using namespace Terminal;

Dispatcher::Dispatcher()
  : params(), parsed_params(), parsed( false ), dispatch_chars(), OSC_string(), OSC_overflow( false ),
    clipboard_events(), APC_string(), APC_overflow( false ), kitty_uploading( false ), kitty_partial(),
    kitty_payload(), terminal_to_host()
{}

void Dispatcher::newparamchar( const Parser::Param* act )
{
  assert( act->char_present );
  assert( ( act->ch == ';' ) || ( ( act->ch >= '0' ) && ( act->ch <= '9' ) ) );
  if ( params.length() < 100 ) {
    /* enough for 16 five-char params plus 15 semicolons */
    params.push_back( act->ch );
  }
  parsed = false;
}

void Dispatcher::collect( const Parser::Collect* act )
{
  assert( act->char_present );
  if ( ( dispatch_chars.length() < 8 ) /* never should need more than 2 */
       && ( act->ch <= 255 ) ) {       /* ignore non-8-bit */
    dispatch_chars.push_back( act->ch );
  }
}

void Dispatcher::clear( const Parser::Clear* act __attribute( ( unused ) ) )
{
  params.clear();
  dispatch_chars.clear();
  parsed = false;
}

void Dispatcher::parse_params( void )
{
  if ( parsed ) {
    return;
  }

  parsed_params.clear();
  const char* str = params.c_str();
  const char* segment_begin = str;

  while ( 1 ) {
    const char* segment_end = strchr( segment_begin, ';' );
    if ( segment_end == NULL ) {
      break;
    }

    errno = 0;
    char* endptr;
    long val = strtol( segment_begin, &endptr, 10 );
    if ( endptr == segment_begin ) {
      val = -1;
    }

    if ( val > PARAM_MAX || errno == ERANGE ) {
      val = -1;
      errno = 0;
    }

    if ( errno == 0 || segment_begin == endptr ) {
      parsed_params.push_back( val );
    }

    segment_begin = segment_end + 1;
  }

  /* get last param */
  errno = 0;
  char* endptr;
  long val = strtol( segment_begin, &endptr, 10 );
  if ( endptr == segment_begin ) {
    val = -1;
  }

  if ( val > PARAM_MAX || errno == ERANGE ) {
    val = -1;
    errno = 0;
  }

  if ( errno == 0 || segment_begin == endptr ) {
    parsed_params.push_back( val );
  }

  parsed = true;
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

  std::string key = dispatch_chars;
  if ( type == CONTROL ) {
    assert( act->ch <= 255 );
    char ctrlstr[2] = { (char)act->ch, 0 };
    key = std::string( ctrlstr, 1 );
  }

  dispatch_map_t::const_iterator i = map->find( key );
  if ( i == map->end() ) {
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
  if ( OSC_string.size() < OSC52_MAX_OSC_CHARS ) {
    OSC_string.push_back( act->ch );
  } else {
    OSC_overflow = true;
  }
}

void Dispatcher::OSC_start( const Parser::OSC_Start* act __attribute( ( unused ) ) )
{
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
  if ( APC_string.size() < KITTY_MAX_APC_CHARS ) {
    APC_string.push_back( static_cast<char>( act->ch ) );
  } else {
    APC_overflow = true;
  }
}

void Dispatcher::APC_start( const Parser::APC_Start* act __attribute( ( unused ) ) )
{
  APC_string.clear();
  APC_overflow = false;
}

bool Dispatcher::operator==( const Dispatcher& x ) const
{
  return ( params == x.params ) && ( parsed_params == x.parsed_params ) && ( parsed == x.parsed )
         && ( dispatch_chars == x.dispatch_chars ) && ( OSC_string == x.OSC_string )
         && ( OSC_overflow == x.OSC_overflow ) && ( clipboard_events == x.clipboard_events )
         && ( APC_string == x.APC_string ) && ( APC_overflow == x.APC_overflow )
         && ( kitty_uploading == x.kitty_uploading ) && ( kitty_payload == x.kitty_payload )
         && ( terminal_to_host == x.terminal_to_host );
}

static void kitty_reply( Dispatcher* dispatch, const KittyCommand& cmd, uint32_t image_id, const std::string& msg )
{
  if ( cmd.quiet >= 2 ) {
    return;
  }
  const bool ok = msg.size() >= 2 && msg[0] == 'O' && msg[1] == 'K';
  if ( ok && cmd.quiet >= 1 ) {
    return;
  }
  dispatch->terminal_to_host.append( kitty_response( image_id, cmd.placement_id, msg ) );
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

void Dispatcher::finish_kitty_upload( Framebuffer* fb )
{
  KittyCommand cmd = kitty_partial;
  cmd.payload.swap( kitty_payload );
  kitty_uploading = false;
  kitty_payload.clear();

  if ( cmd.compression == 'z' ) {
    std::string inflated;
    if ( !kitty_inflate( cmd.payload, inflated, cmd.data_size ) ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL: zlib inflate failed" );
      return;
    }
    cmd.payload.swap( inflated );
    cmd.compression = 0;
  }

  std::string data, error;
  if ( !kitty_read_medium( cmd, data, error ) ) {
    kitty_reply( this, cmd, cmd.image_id, error.empty() ? "EINVAL: transmit failed" : error );
    return;
  }

  const bool query_only = cmd.action == KittyQuery;
  if ( query_only ) {
    kitty_reply( this, cmd, cmd.image_id ? cmd.image_id : 1, "OK" );
    return;
  }

  if ( cmd.action != KittyTransmit && cmd.action != KittyTransmitAndDisplay ) {
    kitty_reply( this, cmd, cmd.image_id, "EINVAL: unexpected action at upload end" );
    return;
  }

  KittyImage image;
  image.id = cmd.image_id;
  image.number = cmd.image_number;
  image.format = cmd.format ? cmd.format : 32;
  image.width = cmd.width;
  image.height = cmd.height;
  image.data = std::make_shared<std::string>( data );
  if ( image.id == 0 && image.number == 0 ) {
    /* assign internally so the image can live in terminal state */
  }
  image.id = fb->put_kitty_image( image );

  std::string ok = "OK";
  if ( cmd.image_number != 0 ) {
    /* include assigned id; kitty_response already has i= */
  }
  kitty_reply( this, cmd, image.id, ok );

  if ( cmd.action == KittyTransmitAndDisplay ) {
    KittyPlacement place;
    place.image_id = image.id;
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
    fb->put_kitty_placement( place );
    if ( !place.cursor_hold && !place.unicode_placeholder && place.parent_image == 0 ) {
      const int cols = place.columns ? static_cast<int>( place.columns ) : 1;
      const int rows = place.rows ? static_cast<int>( place.rows ) : 1;
      fb->ds.move_col( cols, true, false );
      fb->ds.move_row( rows, true );
    }
  }
}

void Dispatcher::APC_dispatch( const Parser::APC_End* act __attribute( ( unused ) ), Framebuffer* fb )
{
  if ( APC_overflow || APC_string.empty() ) {
    APC_string.clear();
    APC_overflow = false;
    return;
  }

  KittyCommand cmd;
  if ( !parse_kitty_command( APC_string, cmd ) ) {
    APC_string.clear();
    return;
  }
  APC_string.clear();

  if ( kitty_uploading ) {
    if ( cmd.action == KittyDelete ) {
      kitty_uploading = false;
      kitty_payload.clear();
      fb->delete_kitty( cmd );
      return;
    }
    kitty_payload.append( cmd.payload );
    if ( cmd.more == 0 ) {
      finish_kitty_upload( fb );
    }
    return;
  }

  if ( cmd.action == KittyDelete ) {
    fb->delete_kitty( cmd );
    return;
  }

  if ( cmd.action == KittyPut ) {
    uint32_t id = resolve_kitty_id( fb, cmd, false );
    if ( id == 0 || fb->find_kitty_image( id ) == NULL ) {
      kitty_reply( this, cmd, cmd.image_id, "ENOENT: image not found" );
      return;
    }
    KittyPlacement place;
    place.image_id = id;
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
    fb->put_kitty_placement( place );
    kitty_reply( this, cmd, id, "OK" );
    if ( !place.cursor_hold && !place.unicode_placeholder && place.parent_image == 0 ) {
      const int cols = place.columns ? static_cast<int>( place.columns ) : 1;
      const int rows = place.rows ? static_cast<int>( place.rows ) : 1;
      fb->ds.move_col( cols, true, false );
      fb->ds.move_row( rows, true );
    }
    return;
  }

  if ( cmd.action == KittyQuery || cmd.action == KittyTransmit || cmd.action == KittyTransmitAndDisplay ) {
    if ( cmd.image_id != 0 && cmd.image_number != 0 ) {
      kitty_reply( this, cmd, cmd.image_id, "EINVAL: both i and I specified" );
      return;
    }
    kitty_uploading = true;
    kitty_partial = cmd;
    kitty_payload = cmd.payload;
    if ( cmd.more == 0 ) {
      finish_kitty_upload( fb );
    }
    return;
  }

  /* animation / compose: ignore for now */
}
