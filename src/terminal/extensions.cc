/*
    Goblin Skiff: Silkie's OSC 777 extensions, relayed
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/extensions.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>

#include "src/terminal/gpusurface.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/osc52.h"
#include "src/terminal/packedimage.h"
#include "src/terminal/terminalframebuffer.h"
#include "src/terminal/terminalgeometry.h"
#include "src/terminal/vectorscene.h"

namespace Terminal {

/* The width of an ink stroke is worked out exactly as the terminal does. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

namespace {
const int64_t T_MAX = int64_t( 1 ) << 50;
const int64_t COORD_MAX = 1073741823;

/* The names of kitty's pointer shape protocol, asked about once so the
   relay can answer a program's support query, and the other names kitty
   takes for them. */
const char* const POINTER_NAMES[] = { "alias",       "cell",       "copy",          "crosshair", "default",
                                      "e-resize",    "ew-resize",  "grab",          "grabbing",  "help",
                                      "move",        "n-resize",   "ne-resize",     "nesw-resize", "no-drop",
                                      "not-allowed", "ns-resize",  "nw-resize",     "nwse-resize", "pointer",
                                      "progress",    "s-resize",   "se-resize",     "sw-resize", "text",
                                      "vertical-text", "w-resize", "wait",          "zoom-in",   "zoom-out" };
const char* const POINTER_ALIASES[]
  = { "left_ptr", "xterm", "ibeam", "pointing_hand", "hand2", "hand", "question_arrow", "whats_this", "clock",
      "watch", "half-busy", "left_ptr_watch", "tcross", "plus", "cross", "fleur", "pointer-move", "right_side",
      "top_right_corner", "top_left_corner", "top_side", "bottom_right_corner", "bottom_left_corner", "bottom_side",
      "left_side", "sb_h_double_arrow", "split_h", "sb_v_double_arrow", "split_v", "size_bdiag", "size-bdiag",
      "size_fdiag", "size-fdiag", "zoom_in", "zoom_out", "dnd-link", "dnd-copy", "forbidden", "crossed_circle",
      "dnd-no-drop", "openhand", "hand1", "closedhand", "dnd-none", "arrow", "beam" };

const char* const MODE_SELECTORS[]
  = { Selector::TABLET, Selector::GESTURE, Selector::SCROLL, Selector::FRAME, Selector::LATENCY, Selector::INK };

Ext::Fields head( const std::string& op )
{
  Ext::Fields f;
  f.set( "v", int64_t( 1 ) );
  f.set( "op", op );
  return f;
}

std::string mode_frame( const char* selector, int64_t mode )
{
  Ext::Fields f = head( "mode" );
  f.set( "mode", mode );
  return Ext::encode( selector, f );
}

int64_t steady_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() )
    .count();
}

std::vector<std::string> split( const std::string& text, char separator )
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

/* A field the terminal sends with a value from a list, or nothing at all. */
bool one_of( const Ext::Fields& f, const std::string& key, std::initializer_list<const char*> values, bool required )
{
  const std::string* value = f.find( key );
  if ( !value ) {
    return !required;
  }
  for ( const char* v : values ) {
    if ( *value == v ) {
      return true;
    }
  }
  return false;
}

bool base64_digit( char c )
{
  return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '+' || c == '/';
}

/* The text of a pad label: base64 with padding of well-formed UTF-8 without
   control characters, as Silkie decodes one (overlong sequences, surrogates
   and code points past U+10FFFF are not UTF-8). */
bool valid_label( const std::string& text )
{
  if ( text.size() % 4 ) {
    return false;
  }
  std::string bytes;
  unsigned bits = 0, value = 0;
  for ( size_t i = 0; i < text.size(); i++ ) {
    const char c = text[i];
    if ( c == '=' ) {
      if ( i + 2 < text.size() ) {
        return false;
      }
      continue;
    }
    if ( !base64_digit( c ) ) {
      return false;
    }
    const unsigned digit = c >= 'A' && c <= 'Z'   ? c - 'A'
                           : c >= 'a' && c <= 'z' ? c - 'a' + 26
                           : c >= '0' && c <= '9' ? c - '0' + 52
                           : c == '+'             ? 62
                                                  : 63;
    value = ( value << 6 ) | digit;
    bits += 6;
    if ( bits >= 8 ) {
      bits -= 8;
      bytes += static_cast<char>( ( value >> bits ) & 0xff );
    }
  }
  for ( size_t i = 0; i < bytes.size(); ) {
    const unsigned char c = bytes[i];
    unsigned need;
    uint32_t cp, least;
    if ( c < 0x80 ) {
      need = 0, cp = c, least = 0;
    } else if ( c >= 0xc2 && c <= 0xdf ) {
      need = 1, cp = c & 0x1f, least = 0x80;
    } else if ( c >= 0xe0 && c <= 0xef ) {
      need = 2, cp = c & 0x0f, least = 0x800;
    } else if ( c >= 0xf0 && c <= 0xf4 ) {
      need = 3, cp = c & 0x07, least = 0x10000;
    } else {
      return false;
    }
    if ( bytes.size() - i <= need ) {
      return false;
    }
    for ( unsigned k = 1; k <= need; k++ ) {
      const unsigned char d = bytes[i + k];
      if ( ( d & 0xc0 ) != 0x80 ) {
        return false;
      }
      cp = cp << 6 | ( d & 0x3f );
    }
    if ( cp < least || cp > 0x10ffff || ( cp >= 0xd800 && cp <= 0xdfff ) || cp < 0x20 || cp == 0x7f ) {
      return false;
    }
    i += need + 1;
  }
  return true;
}

/* image:<id>[:<x>[:<y>]] as kitty takes it, written out in full; empty
   when it is not one. */
std::string pointer_image( const std::string& name )
{
  if ( name.compare( 0, 6, "image:" ) != 0 ) {
    return {};
  }
  const auto parts = split( name.substr( 6 ), ':' );
  int64_t v[3] = { 0, 0, 0 };
  if ( parts.size() > 3 ) {
    return {};
  }
  for ( size_t i = 0; i < parts.size(); i++ ) {
    if ( parts[i].empty() || parts[i].find_first_not_of( "0123456789" ) != std::string::npos
         || !Ext::parse_integer( parts[i], 0, i ? 65535 : UINT32_MAX, v[i] ) ) {
      return {};
    }
  }
  if ( !v[0] ) {
    return {};
  }
  return "image:" + std::to_string( v[0] ) + ":" + std::to_string( v[1] ) + ":" + std::to_string( v[2] );
}

/* A number as the records of a vector image write one. */
bool parse_record_number( const std::string& text, double& out )
{
  if ( text.empty() || text.find_first_not_of( "0123456789.eE-+" ) != std::string::npos ) {
    return false;
  }
  char* end = nullptr;
  out = strtod( text.c_str(), &end );
  return !*end && std::isfinite( out );
}

/* A brush as the terminal takes one; anything else leaves the old brush. */
bool parse_brush( const Ext::Fields& f, InkBrush& out )
{
  Ext::CNumbers c_numbers;
  InkBrush b;
  b.remaining = 1;
  b.w0 = b.w1 = -1;
  bool has_first = false;
  for ( const auto& item : f.items ) {
    const std::string& key = item.first;
    const std::string& val = item.second;
    int64_t n;
    if ( key == "i" || key == "g" || key == "k" || key == "n" || key == "tools" ) {
      if ( val.empty() || val.find_first_not_of( "0123456789" ) != std::string::npos
           || !Ext::parse_integer( val, 0, UINT32_MAX, n ) ) {
        return false;
      }
      if ( key == "i" ) {
        b.image = n;
      } else if ( key == "g" ) {
        b.group = n;
      } else if ( key == "k" ) {
        b.next = n;
        has_first = true;
      } else if ( key == "n" ) {
        b.remaining = n;
      } else {
        if ( !n || n > 511 ) {
          return false;
        }
        b.tools = n;
      }
    } else if ( key == "w0" || key == "w1" ) {
      double w;
      if ( !parse_record_number( val, w ) || w < 0 ) {
        return false;
      }
      ( key == "w0" ? b.w0 : b.w1 ) = w;
    } else if ( key == "e" ) {
      double e;
      if ( !parse_record_number( val, e ) || !( e > 0 ) ) {
        return false;
      }
      b.exponent = e;
    } else if ( key == "fill" ) {
      /* A color as the records write it, that paints something. */
      if ( !Vector::color_paints( val ) ) {
        return false;
      }
      b.fill = val;
    }
  }
  if ( !b.image || !has_first || !b.next || !b.remaining || b.w1 < 0 ) {
    return false;
  }
  if ( uint64_t( b.remaining ) - 1 > uint64_t( UINT32_MAX ) - b.next ) {
    return false; /* the ids run past the largest there is */
  }
  if ( b.w0 < 0 ) {
    b.w0 = b.w1;
  }
  out = b;
  return true;
}

Ext::Fields without( Ext::Fields f, std::initializer_list<const char*> keys )
{
  for ( const char* key : keys ) {
    f.erase( key );
  }
  return f;
}
}

int64_t integer_or_zero( const Ext::Fields& f, const char* key )
{
  int64_t value = 0;
  return Ext::integer( f, key, INT64_MIN / 4, INT64_MAX / 4, value ) == Ext::Value::Valid ? value : 0;
}


Ext::Fields InkBrush::fields() const
{
  Ext::Fields f;
  f.set( "i", int64_t( image ) );
  f.set( "g", int64_t( group ) );
  f.set( "k", int64_t( next ) );
  f.set( "n", int64_t( remaining ) );
  f.set( "fill", fill );
  f.set( "w0", Ext::number( w0 ) );
  f.set( "w1", Ext::number( w1 ) );
  f.set( "e", Ext::number( exponent ) );
  f.set( "tools", int64_t( tools ) );
  return f;
}

std::string ExtensionModes::pointer_shape( const Framebuffer* fb ) const
{
  const auto& stack = pointer[alternate ? 1 : 0];
  for ( auto it = stack.rbegin(); it != stack.rend(); ++it ) {
    /* An image shape is passed over while its image does not exist. */
    if ( fb && it->compare( 0, 6, "image:" ) == 0
         && !fb->find_kitty_image( static_cast<uint32_t>( strtoul( it->c_str() + 6, nullptr, 10 ) ) ) ) {
      continue;
    }
    return *it;
  }
  return {};
}

bool ClientExtensions::has_format( const std::string& wanted ) const
{
  for ( const auto& item : split( format, ',' ) ) {
    if ( item == wanted ) {
      return true;
    }
  }
  return false;
}

bool ClientExtensions::has_graphics( const std::string& extension ) const
{
  for ( const auto& item : split( graphics, ',' ) ) {
    if ( item.compare( 0, extension.size() + 1, extension + "." ) == 0 ) {
      return true;
    }
  }
  return false;
}

uint32_t ClientExtensions::grant( const std::string& selector, uint32_t known ) const
{
  const auto it = grants.find( selector );
  return it == grants.end() ? known : it->second & known;
}

int64_t ClientExtensions::cap_int( const std::string& selector, const std::string& key, int64_t fallback ) const
{
  const auto it = caps.find( selector );
  int64_t value;
  if ( it == caps.end() || Ext::integer( it->second, key, INT64_MIN / 2, INT64_MAX / 2, value ) != Ext::Value::Valid ) {
    return fallback;
  }
  return value;
}

/* Terminal-side ink. The terminal at the far end draws a stroke into a
   vector image as the pen moves. The client works out the same records
   from what that terminal shows, and the server applies them to its own
   table, so the scene it would hand a terminal that attaches later is the
   scene on screen. */

std::string InkMap::render( int64_t x, int64_t y, int64_t p ) const
{
  const double px = x / 256., py = y / 256.;
  const double X = a * px + c * py + e, Y = b * px + d * py + f;
  const double fraction = pressure ? p / 65535. : 1.;
  const double curved = std::pow( fraction, exponent );
  const double span = w1 - w0;
  const double grown = span * curved;
  const double width = w0 + grown;
  char buf[128];
  Ext::CNumbers c_numbers;
  snprintf( buf, sizeof buf, "%.17g,%.17g,%.17g", X, Y, width );
  if ( record == 'k' ) {
    return "k id=" + std::to_string( element ) + ( group ? " g=" + std::to_string( group ) : "" ) + " pts=" + buf
           + " fill=" + fill + "\n";
  }
  return "a id=" + std::to_string( element ) + " pts=" + buf + "\n";
}

int ink_point( const InkStroke& stroke,
               const Ext::Fields& f,
               const Framebuffer& fb,
               const ClientGeometry& client,
               bool starting,
               InkStroke* found,
               std::string& point,
               InkMap* map )
{
  const ClientGeometry* geometry = &client;
  int64_t x, y;
  if ( Ext::integer( f, "x", -COORD_MAX, COORD_MAX, x ) != Ext::Value::Valid
       || Ext::integer( f, "y", -COORD_MAX, COORD_MAX, y ) != Ext::Value::Valid ) {
    return 0;
  }
  const KittyImage* image = fb.find_kitty_image( stroke.image );
  if ( !image || image->format != KITTY_FORMAT_VECTOR || !image->vector
       || ( !starting && image->vector->lineage != stroke.lineage ) ) {
    return -1; /* the image, or the transmission the stroke began on, has gone */
  }
  /* Without the client's cells, as in the middle of a resize, the point is
     not known; the terminal goes on drawing, and so does the stroke. */
  const int unknown = starting ? -1 : 0;
  if ( !geometry || geometry->columns != unsigned( fb.ds.get_width() )
       || geometry->rows != unsigned( fb.ds.get_height() ) ) {
    return unknown;
  }
  double cw, ch;
  if ( !client_cell_size( *geometry, cw, ch ) ) {
    return unknown;
  }
  const double px = x / 256., py = y / 256.;

  /* The rectangle a placement is shown in, in pixels from the top left of
     the cell area, as the terminal lays it out. */
  KittyPlacementRect rect;
  const KittyPlacement* chosen = nullptr;
  size_t chosen_ordinal = 0, ordinal = 0;
  for ( const auto& p : fb.get_kitty_placements() ) {
    if ( p.image_id != stroke.image ) {
      continue;
    }
    const size_t this_ordinal = p.placement_id ? 0 : ordinal++;
    if ( starting ) {
      /* The placement on top of the others under the pen, on the part of it
         its viewports leave visible. */
      KittyPlacementRect r;
      if ( !kitty_placement_rect( fb, p, cw, ch, r ) || px < r.left || py < r.top || px >= r.left + r.width
           || py >= r.top + r.height ) {
        continue;
      }
      if ( r.clipped && ( px < r.clip_left || py < r.clip_top || px >= r.clip_right || py >= r.clip_bottom ) ) {
        continue;
      }
      if ( !chosen || p.z >= chosen->z ) {
        chosen = &p;
        chosen_ordinal = this_ordinal;
        rect = r;
      }
    } else if ( stroke.placement ? p.placement_id == stroke.placement
                                 : !p.placement_id && this_ordinal == stroke.ordinal ) {
      /* After the first point the pen is mapped through the whole placement. */
      if ( !kitty_placement_rect( fb, p, cw, ch, rect ) ) {
        return 0;
      }
      chosen = &p;
      break;
    }
  }
  if ( !chosen ) {
    return -1;
  }
  double ux = rect.sx + ( px - rect.left ) * rect.sw / rect.width;
  double uy = rect.sy + ( py - rect.top ) * rect.sh / rect.height;
  Vector::Matrix inverse;
  if ( !image->vector->table()->group_inverse( stroke.group, inverse ) ) {
    return starting ? -1 : 0;
  }
  Vector::apply( inverse, ux, uy );
  if ( !std::isfinite( ux ) || !std::isfinite( uy ) ) {
    return starting ? -1 : 0;
  }
  int64_t valid = 0, p = 0;
  Ext::integer( f, "valid", 0, INT32_MAX, valid );
  const bool pressure = ( valid & 1 ) && Ext::integer( f, "p", 0, 65535, p ) == Ext::Value::Valid;
  const double fraction = pressure ? p / 65535. : 1.;
  const double curved = std::pow( fraction, stroke.exponent );
  const double span = stroke.w1 - stroke.w0;
  const double grown = span * curved;
  const double width = stroke.w0 + grown;
  char buf[128];
  Ext::CNumbers c_numbers;
  snprintf( buf, sizeof buf, "%.17g,%.17g,%.17g", ux, uy, width );
  point = buf;
  if ( map ) {
    /* (px, py) -> image units is a scaling, then the group's inverse: composed, one affine map. */
    const double kx = rect.sw / rect.width, ky = rect.sh / rect.height;
    const double ox = rect.sx - rect.left * kx, oy = rect.sy - rect.top * ky;
    map->present = true;
    map->a = inverse.a * kx;
    map->c = inverse.c * ky;
    map->e = inverse.a * ox + inverse.c * oy + inverse.e;
    map->b = inverse.b * kx;
    map->d = inverse.d * ky;
    map->f = inverse.b * ox + inverse.d * oy + inverse.f;
    map->w0 = stroke.w0;
    map->w1 = stroke.w1;
    map->exponent = stroke.exponent;
    map->pressure = pressure;
    map->group = stroke.group;
    map->element = stroke.element;
  }
  if ( found ) {
    *found = stroke;
    found->lineage = image->vector->lineage;
    found->placement = chosen->placement_id;
    found->ordinal = chosen_ordinal;
  }
  return 1;
}

/* The client's side */

std::vector<std::pair<std::string, uint32_t>> extension_probe_modes()
{
  return { { Selector::TABLET, TABLET_KNOWN }, { Selector::GESTURE, 3 }, { Selector::SCROLL, 1 },
           { Selector::FRAME, 1 },           { Selector::LATENCY, 1 }, { Selector::DISPLAY, 1 } };
}

std::string pointer_probe()
{
  std::string out = "\033]22;?__default__,__grabbed__";
  for ( const char* name : POINTER_NAMES ) {
    out += ",";
    out += name;
  }
  return out + ",image:1:2:3\033\\";
}

std::string extension_probe()
{
  std::string out;
  Ext::Fields query = head( "query" );
  query.set( "id", PROBE_ID );
  out += Ext::encode( Selector::CAPS, query );
  for ( const char* selector : { Selector::TABLET, Selector::SCROLL, Selector::GESTURE, Selector::FRAME,
                                 Selector::LATENCY, Selector::DISPLAY, Selector::HAPTIC, Selector::INK,
                                 Selector::FONT } ) {
    out += Ext::encode( selector, query );
  }
  /* What a terminal keeps of a mode is what its user allows; ask with every
     bit, read it back, and leave the mode as it was. Display reports stay
     on, so the client hears of a change of display. */
  for ( const auto& mode : extension_probe_modes() ) {
    out += mode_frame( mode.first.c_str(), mode.second );
    out += Ext::encode( mode.first, head( "mode" ) );
    if ( mode.first != Selector::DISPLAY ) {
      out += mode_frame( mode.first.c_str(), 0 );
    }
  }
  return out + pointer_probe() + notification_probe();
}

std::string notification_probe()
{
  return "\033]99;i=" + std::to_string( PROBE_ID ) + ":p=?:r=goblin-skiff/probe/0;\033\\";
}

namespace {
/* The metadata of an OSC 99, the part before its first semicolon, as kitty
   reads it: key=value pairs between colons. False when a pair has no =. */
bool notification_metadata( const std::string& payload, std::vector<std::pair<std::string, std::string>>& out,
                            std::string& rest )
{
  const size_t semi = payload.find( ';', 3 );
  const std::string metadata = payload.substr( 3, semi == std::string::npos ? std::string::npos : semi - 3 );
  rest = semi == std::string::npos ? std::string() : payload.substr( semi + 1 );
  for ( const auto& part : split( metadata, ':' ) ) {
    if ( part.empty() ) {
      continue;
    }
    const size_t eq = part.find( '=' );
    if ( eq == std::string::npos ) {
      return false;
    }
    out.emplace_back( part.substr( 0, eq ), part.substr( eq + 1 ) );
  }
  return true;
}

/* A return route: hops of component/instance/window between commas. */
bool notification_route_ok( const std::string& route )
{
  const auto token = []( const std::string& s ) {
    if ( s.empty() ) {
      return false;
    }
    for ( char c : s ) {
      if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-' ) ) {
        return false;
      }
    }
    return true;
  };
  for ( const auto& hop : split( route, ',' ) ) {
    const auto parts = split( hop, '/' );
    if ( parts.size() != 3 || !token( parts[0] ) || !token( parts[1] ) || parts[2].empty()
         || parts[2].find_first_not_of( "0123456789" ) != std::string::npos ) {
      return false;
    }
  }
  return !route.empty();
}
}

bool is_progress_report( const std::string& payload )
{
  return payload == "9;4" || payload.compare( 0, 4, "9;4;" ) == 0;
}

bool is_notification( const std::string& payload )
{
  return payload.compare( 0, 2, "9;" ) == 0 || payload.compare( 0, 3, "99;" ) == 0
         || payload.compare( 0, 11, "777;notify;" ) == 0;
}

bool notification_answer_ok( const std::string& frame )
{
  if ( frame.size() < 7 || frame.compare( 0, 5, "\033]99;" ) != 0 || frame.compare( frame.size() - 2, 2, "\033\\" ) != 0 ) {
    return false;
  }
  for ( size_t i = 2; i + 2 < frame.size(); i++ ) {
    const unsigned char c = static_cast<unsigned char>( frame[i] );
    if ( c < 0x20 || c >= 0x7f ) {
      return false; /* the answers of the protocol are ASCII */
    }
  }
  return true;
}

bool notification_probe_reply( const std::string& payload, ClientExtensions& info )
{
  if ( payload.compare( 0, 3, "99;" ) != 0 ) {
    return false;
  }
  std::vector<std::pair<std::string, std::string>> metadata;
  std::string answer;
  if ( !notification_metadata( payload, metadata, answer ) ) {
    return false;
  }
  bool query = false;
  for ( const auto& item : metadata ) {
    query = query || ( item.first == "p" && item.second == "?" );
  }
  if ( !query ) {
    return false;
  }
  /* A terminal that keeps return routes ends its answer by saying so. */
  static const std::string routes = ":r=1";
  info.notification_routes = answer.size() >= routes.size()
                             && answer.compare( answer.size() - routes.size(), routes.size(), routes ) == 0;
  if ( info.notification_routes ) {
    answer.erase( answer.size() - routes.size() );
  }
  info.notifications = answer;
  return true;
}

bool program_status_answer_ok( const std::string& frame )
{
  if ( frame.size() < 9 || frame.compare( 0, 7, "\033]7501;" ) != 0
       || frame.compare( frame.size() - 2, 2, "\033\\" ) != 0 ) { return false; }
  for ( size_t i = 7; i + 2 < frame.size(); ++i ) {
    const unsigned char c = static_cast<unsigned char>( frame[i] );
    if ( c < 32 || c >= 127 ) { return false; }
  }
  return true;
}

bool consume_probe_reply( const std::string& frame, ClientExtensions& info )
{
  const std::string payload = Ext::frame_payload( frame );
  if ( payload.compare( 0, 3, "22;" ) == 0 ) {
    info.pointer_shapes = payload.substr( 3 );
    return true;
  }
  if ( payload.compare( 0, 3, "99;" ) == 0 ) {
    return notification_probe_reply( payload, info );
  }
  Ext::Frame parsed;
  if ( !Ext::parse( payload, parsed ) ) {
    return false;
  }
  const std::string* op = parsed.fields.find( "op" );
  int64_t version = 0;
  if ( !op || Ext::integer( parsed.fields, "v", 0, INT32_MAX, version ) != Ext::Value::Valid || version != 1 ) {
    return false;
  }
  const std::string& selector = parsed.selector;
  if ( selector == Selector::CAPS ) {
    if ( *op != "cap" ) {
      return false;
    }
    info.listing = true;
    /* The listing does not say whether the G key is taken: the client asks
       with a query of a pixel, as docs/wide-gamut-protocol.rst says. */
    info.versions.clear();
    const std::string* ext = parsed.fields.find( "ext" );
    for ( const auto& item : split( ext ? *ext : std::string(), ',' ) ) {
      const size_t dot = item.rfind( '.' );
      int64_t v;
      if ( dot != std::string::npos && dot && Ext::parse_integer( item.substr( dot + 1 ), 1, INT32_MAX, v ) ) {
        info.versions[item.substr( 0, dot )] = v;
      }
    }
    const std::string* media = parsed.fields.find( "media" );
    const std::string* format = parsed.fields.find( "format" );
    const std::string* graphics = parsed.fields.find( "graphics" );
    info.media = media ? *media : std::string();
    info.format = format ? *format : std::string();
    info.graphics = graphics ? *graphics : std::string();
    return true;
  }
  if ( *op == "cap" ) {
    info.caps[selector] = without( parsed.fields, { "v", "op", "id", "mode" } );
    if ( selector == Selector::DISPLAY ) {
      info.display = info.caps[selector];
    }
    return true;
  }
  if ( *op == "event" && selector == Selector::DISPLAY ) {
    info.display = without( parsed.fields, { "v", "op", "t", "tu" } );
    return true;
  }
  return false;
}

std::string render_extension_modes( bool initialized,
                                    const Framebuffer& last_fb,
                                    const Framebuffer& now_fb,
                                    const ClientExtensions& terminal,
                                    uint32_t ink_next_hint,
                                    std::vector<std::pair<std::string, uint32_t>>* modes_sent,
                                    InkBrush* brush_sent )
{
  const ExtensionModes& last = last_fb.ds.ext;
  const ExtensionModes& now = now_fb.ds.ext;
  std::string out;
  /* A mode that is set is read back, so the client learns what the
     terminal's user lets through when that changes. */
  const auto mode = [&]( const char* selector, int64_t then, int64_t value ) {
    if ( !terminal.speaks( selector ) || ( initialized && then == value ) ) {
      return;
    }
    out += mode_frame( selector, value );
    if ( value ) {
      out += Ext::encode( selector, head( "mode" ) );
      if ( modes_sent ) {
        modes_sent->emplace_back( selector, uint32_t( value ) );
      }
    }
  };
  mode( Selector::TABLET, last.tablet, now.tablet );
  if ( terminal.speaks( Selector::TABLET ) && ( now.tablet & TABLET_PADS ) ) {
    /* A terminal takes a label only while pad reports are on, so the labels
       follow the mode, and all of them again whenever the mode was set. */
    const bool all = !initialized || last.tablet != now.tablet;
    const auto label = [&]( const PadControl& control, const std::string& text ) {
      Ext::Fields f = head( "label" );
      f.set( "device", std::to_string( std::get<0>( control ) ) );
      f.set( PAD_CONTROLS[std::get<1>( control )], int64_t( std::get<2>( control ) ) );
      if ( !text.empty() ) {
        f.set( "text", text );
      }
      out += Ext::encode( Selector::TABLET, f );
    };
    if ( !all ) {
      for ( const auto& item : last.pad_labels ) {
        if ( !now.pad_labels.count( item.first ) ) {
          label( item.first, std::string() );
        }
      }
    }
    for ( const auto& item : now.pad_labels ) {
      const auto then = last.pad_labels.find( item.first );
      if ( all || then == last.pad_labels.end() || then->second != item.second ) {
        label( item.first, item.second );
      }
    }
  }
  mode( Selector::GESTURE, last.gesture, now.gesture );
  mode( Selector::SCROLL, last.scroll, now.scroll );
  mode( Selector::FRAME, last.frame, now.frame );
  mode( Selector::LATENCY, last.latency, now.latency );
  if ( terminal.speaks( Selector::INK ) ) {
    bool given = false;
    if ( now.brush.present() && ( !initialized || last.brush_gen != now.brush_gen ) ) {
      /* After a repaint the terminal is handed the ids it has not used yet,
         which the client may know of before the server does. */
      InkBrush b = now.brush;
      if ( !initialized && ink_next_hint > b.next && ink_next_hint - b.next <= b.remaining ) {
        b.remaining -= ink_next_hint - b.next;
        b.next = ink_next_hint;
      }
      if ( b.remaining ) {
        Ext::Fields f = head( "brush" );
        f.set( "i", int64_t( b.image ) );
        if ( b.group ) {
          f.set( "g", int64_t( b.group ) );
        }
        f.set( "k", int64_t( b.next ) );
        if ( b.remaining != 1 ) {
          f.set( "n", int64_t( b.remaining ) );
        }
        if ( b.fill != "#000000" ) {
          f.set( "fill", b.fill );
        }
        if ( b.w0 != b.w1 ) {
          f.set( "w0", Ext::number( b.w0 ) );
        }
        f.set( "w1", Ext::number( b.w1 ) );
        if ( b.exponent != 1 ) {
          f.set( "e", Ext::number( b.exponent ) );
        }
        if ( b.tools != 58 ) {
          f.set( "tools", int64_t( b.tools ) );
        }
        out += Ext::encode( Selector::INK, f );
        given = true;
        if ( brush_sent ) {
          *brush_sent = b;
        }
      }
    }
    if ( given ? !now.ink : ( !initialized || last.ink != now.ink ) ) {
      out += mode_frame( Selector::INK, now.ink );
    }
    /* Read back once the terminal has a brush, which it needs to turn ink
       on: a terminal whose user refuses ink says so here. */
    if ( given && now.ink ) {
      out += Ext::encode( Selector::INK, head( "mode" ) );
      if ( modes_sent ) {
        modes_sent->emplace_back( Selector::INK, 1 );
      }
    }
  }
  /* The terminal is given the shape in effect, never a stack of its own. */
  const std::string shape = now.pointer_shape( &now_fb );
  if ( !terminal.pointer_shapes.empty() && ( !initialized || last.pointer_shape( &last_fb ) != shape ) ) {
    out += "\033]22;" + shape + "\033\\";
  }
  return out;
}

std::string extension_close( const ClientExtensions& terminal )
{
  std::string out;
  for ( const char* selector : MODE_SELECTORS ) {
    if ( terminal.speaks( selector ) ) {
      out += mode_frame( selector, 0 );
    }
  }
  if ( terminal.speaks( Selector::DISPLAY ) ) {
    out += mode_frame( Selector::DISPLAY, 0 );
  }
  if ( terminal.speaks( Selector::HAPTIC ) ) {
    out += Ext::encode( Selector::HAPTIC, head( "stop" ) );
  }
  if ( !terminal.pointer_shapes.empty() ) {
    out += "\033]22;\033\\";
  }
  return out;
}

std::string extension_open( const ClientExtensions& terminal )
{
  return terminal.speaks( Selector::DISPLAY ) ? mode_frame( Selector::DISPLAY, 1 ) : std::string();
}

std::string ExtensionClient::probe()
{
  for ( const auto& mode : extension_probe_modes() ) {
    mode_reads[mode.first].push_back( mode.second );
  }
  return extension_probe();
}

ExtensionClient::Result ExtensionClient::frame( const std::string& frame,
                                                const Framebuffer& shown,
                                                const ClientGeometry& geometry,
                                                uint64_t now )
{
  Result result;
  Ext::Frame parsed;
  const std::string osc_payload = Ext::frame_payload( frame );
  if ( osc_payload.compare( 0, 5, "7501;" ) == 0 ) {
    const std::string answer = "\033]" + osc_payload + "\033\\";
    if ( program_status_answer_ok( answer ) ) { result.notice = answer; }
    return result;
  }
  if ( osc_payload.compare( 0, 3, "99;" ) == 0 ) {
    /* Of the notification protocol, only the query is this client's own;
       whatever else the terminal says of a notification is for the program
       that sent it. */
    const ClientExtensions before = info;
    if ( notification_probe_reply( osc_payload, info ) ) {
      result.info_changed = info != before;
    } else {
      const std::string answer = "\033]" + osc_payload + "\033\\";
      if ( notification_answer_ok( answer ) ) {
        result.notice = answer;
      }
    }
    return result;
  }
  const bool extension = Ext::parse( osc_payload, parsed );
  const std::string* op = extension ? parsed.fields.find( "op" ) : nullptr;
  /* The terminal's clock, from whatever it stamps; the least delayed of
     its reports says most nearly how far its clock is from ours. A predicted
     sample is stamped for a time that has not come. */
  int64_t t;
  if ( extension && !parsed.fields.has( "pred" )
       && Ext::integer( parsed.fields, "t", 0, T_MAX, t ) == Ext::Value::Valid ) {
    const int64_t offset = t - int64_t( now );
    if ( !clock_known || offset > clock_offset ) {
      clock_offset = offset;
      clock_known = true;
    }
  }
  if ( op && *op == "mode" ) {
    /* A read back: of the bits the client asked for, those the terminal kept. */
    std::vector<uint32_t>& reads = mode_reads[parsed.selector];
    int64_t mode;
    if ( !reads.empty() && Ext::integer( parsed.fields, "mode", 0, INT32_MAX, mode ) == Ext::Value::Valid ) {
      const uint32_t requested = reads.front();
      reads.erase( reads.begin() );
      const auto known = info.grants.find( parsed.selector );
      const uint32_t before = known == info.grants.end() ? ~0U : known->second;
      const uint32_t grant = ( before & ~requested ) | ( uint32_t( mode ) & requested );
      if ( known == info.grants.end() || known->second != grant ) {
        info.grants[parsed.selector] = grant;
        result.info_changed = true;
      }
    }
    return result;
  }
  int64_t id;
  if ( op && *op == "cap" && parsed.selector == Selector::FRAME
       && Ext::integer( parsed.fields, "id", 0, INT32_MAX, id ) == Ext::Value::Valid && id != PROBE_ID ) {
    /* A marker the client wrote after the program's output: the terminal
       has taken that output in, so the next frame it presents shows it. */
    frame_output_pending = true;
    return result;
  }
  const ClientExtensions before = info;
  if ( consume_probe_reply( frame, info ) ) {
    result.info_changed = info != before;
    if ( graphics_allowed && !service_probed && info.has_format( std::to_string( KITTY_FORMAT_VECTOR ) ) ) {
      /* A terminal that has the service says an operation it does not know is
         one; a terminal that does not have it says nothing at all. */
      service_probed = service_probe_open = true;
      service_probe_at = now;
      result.to_terminal += encode_vector_service( uint32_t( PROBE_ID ), "{\"op\":\"probe\"}" );
    }
    return result;
  }
  if ( !op ) {
    return result;
  }
  /* Only reports of the modes this terminal was asked for, as the
     framebuffer it shows says; the server checks them again. */
  const ExtensionModes& ext = shown.ds.ext;
  const std::string& selector = parsed.selector;
  if ( selector == Selector::TABLET && ( *op == "event" || *op == "device" ) ) {
    result.forward = ext.tablet != 0;
    if ( result.forward && *op == "event" ) {
      ink_report( parsed, shown, geometry, result );
    }
  } else if ( selector == Selector::SCROLL && *op == "event" ) {
    result.forward = ext.scroll;
  } else if ( selector == Selector::GESTURE && *op == "event" ) {
    result.forward = ext.gesture != 0;
  } else if ( selector == Selector::FRAME && *op == "event" ) {
    /* A frame is reported to the program only when it showed the program's
       output, not the client's own echo or notifications. */
    result.forward = ext.frame && frame_output_pending;
    frame_output_pending = false;
  }
  if ( result.forward ) {
    track( parsed );
    result.input.payload = "777;" + parsed.selector + ";" + parsed.fields.text();
    result.input.clock = clock( now );
    last_sent = now;
  }
  return result;
}

/* What is in progress, so the client keeps being heard from while it is. */
void ExtensionClient::track( const Ext::Frame& frame )
{
  const Ext::Fields& f = frame.fields;
  int64_t kind = 0, phase = -1, device = 0, contact = 0, button = 0, state = 0;
  Ext::integer( f, "kind", 0, INT32_MAX, kind );
  Ext::integer( f, "phase", 0, INT32_MAX, phase );
  Ext::integer( f, "device", 0, INT64_MAX / 2, device );
  Ext::integer( f, "contact", 0, INT64_MAX / 2, contact );
  if ( frame.selector == Selector::TABLET && ( kind == 1 || kind == 2 ) ) {
    const auto key = std::make_tuple( kind, device, contact );
    if ( phase == 1 ) {
      strokes.insert( key );
    } else if ( phase == 3 || phase == 4 ) {
      strokes.erase( key );
    } else if ( phase == 5 ) {
      for ( auto it = strokes.begin(); it != strokes.end(); ) {
        it = std::get<0>( *it ) == kind && std::get<1>( *it ) == device ? strokes.erase( it ) : ++it;
      }
    }
  } else if ( frame.selector == Selector::TABLET && kind == 3
              && Ext::integer( f, "button", 0, INT32_MAX, button ) == Ext::Value::Valid
              && Ext::integer( f, "state", 0, 1, state ) == Ext::Value::Valid ) {
    if ( state ) {
      keys.insert( std::make_pair( device, button ) );
    } else {
      keys.erase( std::make_pair( device, button ) );
    }
  } else if ( frame.selector == Selector::GESTURE ) {
    if ( phase == 1 ) {
      gestures.insert( kind );
    } else if ( phase == 3 || phase == 4 ) {
      gestures.erase( kind );
    }
  }
}

void ExtensionClient::ink_report( Ext::Frame& frame,
                                  const Framebuffer& shown,
                                  const ClientGeometry& geometry,
                                  Result& result )
{
  Ext::Fields& f = frame.fields;
  int64_t kind = 0, phase = -1, device = 0, contact = 0, element = 0;
  if ( Ext::integer( f, "kind", 1, 1, kind ) != Ext::Value::Valid
       || Ext::integer( f, "phase", 1, 4, phase ) != Ext::Value::Valid ) {
    return;
  }
  Ext::integer( f, "device", 0, INT64_MAX / 2, device );
  Ext::integer( f, "contact", 0, INT64_MAX / 2, contact );
  const auto key = std::make_pair( device, contact );
  const auto edit = [&]( uint32_t image, const std::string& records ) {
    return encode_kitty_chunks( "a=e,q=2,i=" + std::to_string( image ), records );
  };
  std::string point;
  if ( phase == 1 ) {
    ink.erase( key ); /* a down for a contact still down replaces its stroke */
    if ( Ext::integer( f, "ink", 1, UINT32_MAX, element ) != Ext::Value::Valid ) {
      return;
    }
    ink_next_id = std::max<uint32_t>( ink_next_id, uint32_t( element ) + 1 );
    const bool ours = brush.present() && uint64_t( element ) >= brush.next
                      && uint64_t( element ) - brush.next < brush.remaining;
    InkStroke stroke, found;
    stroke.image = brush.image;
    stroke.element = element;
    stroke.group = brush.group;
    stroke.w0 = brush.w0;
    stroke.w1 = brush.w1;
    stroke.exponent = brush.exponent;
    if ( ours ) {
      brush.remaining -= uint32_t( element - brush.next + 1 );
      brush.next = uint32_t( element + 1 );
    }
    if ( ours && ink_point( stroke, f, shown, geometry, true, &found, point, &result.ink ) == 1 ) {
      result.input.ink_image = stroke.image;
      result.ink.record = 'k';
      result.ink.fill = brush.fill;
      result.input.ink_records = result.ink.render( integer_or_zero( f, "x" ), integer_or_zero( f, "y" ), integer_or_zero( f, "p" ) );
      result.input.ink = result.ink;
      ink[key] = found;
      return;
    }
    result.ink = InkMap();
    /* A stroke the client cannot follow is one the terminal does not draw:
       it takes its ink back, and the program draws the stroke itself. Only an
       id the brush set aside is the terminal's to take back. */
    f.erase( "ink" );
    if ( ours ) {
      result.to_terminal += edit( stroke.image, "x id=" + std::to_string( element ) + "\n" );
    }
    return;
  }
  const auto it = ink.find( key );
  if ( it == ink.end() ) {
    return;
  }
  const InkStroke& stroke = it->second;
  if ( phase == 4 ) {
    /* A cancelled stroke never happened. The terminal deletes it, and is
       told to as well, in case it had stopped drawing it. */
    result.input.ink_image = stroke.image;
    result.input.ink_records = "x id=" + std::to_string( stroke.element ) + "\n";
    result.to_terminal += edit( stroke.image, result.input.ink_records );
    ink.erase( it );
    return;
  }
  const int added = ink_point( stroke, f, shown, geometry, false, nullptr, point, &result.ink );
  if ( added == 1 ) {
    result.input.ink_image = stroke.image;
    result.ink.record = 'a';
    result.input.ink_records = result.ink.render( integer_or_zero( f, "x" ), integer_or_zero( f, "y" ), integer_or_zero( f, "p" ) );
    result.input.ink = result.ink;
    if ( stroke.client_draws ) {
      result.to_terminal += edit( stroke.image, result.input.ink_records );
    }
  } else {
    result.ink = InkMap();
  }
  if ( added < 0 || phase == 3 ) {
    ink.erase( it );
  }
}

std::string ExtensionClient::drawn( const Framebuffer& before,
                                    const Framebuffer& after,
                                    const std::vector<std::pair<std::string, uint32_t>>& modes_sent,
                                    const InkBrush* brush_sent,
                                    bool vector_resent )
{
  for ( const auto& mode : modes_sent ) {
    mode_reads[mode.first].push_back( mode.second );
  }
  if ( brush_sent ) {
    brush = *brush_sent; /* the terminal starts over with this one */
  }
  if ( !after.ds.ext.ink ) {
    /* Ink is off at the terminal now, by the mode or a reset: what has been
       drawn stays, and nothing is added to it from the reports that follow. */
    ink.clear();
  }
  if ( vector_resent ) {
    /* A new transmission of an image ends the strokes the terminal was
       drawing into it; the client writes the rest of them. */
    for ( auto& stroke : ink ) {
      stroke.second.client_draws = true;
    }
  }
  if ( after.ds.ext.frame && after.ds.ext.frame_output != before.ds.ext.frame_output
       && info.speaks( Selector::FRAME ) ) {
    /* Written after the program's output; the terminal's answer comes once
       it has taken that output in. Being output itself, it also makes sure a
       frame is reported when the program's output changed nothing. */
    marker = marker + 1 == uint32_t( PROBE_ID ) ? marker + 2 : marker + 1;
    marker &= 0x7fffffff;
    Ext::Fields query = head( "query" );
    query.set( "id", int64_t( marker ) );
    return Ext::encode( Selector::FRAME, query );
  }
  return {};
}

uint32_t ExtensionClient::ink_hint( const Framebuffer& next )
{
  /* A brush given again starts the ids again; otherwise the terminal is
     ahead of the server by the strokes it has begun since. */
  if ( next.ds.ext.brush_gen != brush_gen ) {
    brush_gen = next.ds.ext.brush_gen;
    ink_next_id = next.ds.ext.brush.next;
  } else {
    ink_next_id = std::max( ink_next_id, next.ds.ext.brush.next );
  }
  return ink_next_id;
}

void ExtensionClient::service_written( const std::string& frame )
{
  KittyCommand command;
  const std::string apc = Ext::frame_payload( frame );
  if ( !apc.empty() && apc[0] == 'G' && apc.find( "a=v" ) != std::string::npos
       && parse_kitty_command( apc.substr( 0, apc.find( ';' ) ), command ) && command.action == KittyService ) {
    service_ids.insert( command.image_id );
  }
}

bool ExtensionClient::service_pending( uint64_t now )
{
  if ( service_probe_open && now >= service_probe_at + 10000 ) {
    service_probe_open = false; /* its silence said no */
  }
  return service_probe_open || !service_ids.empty();
}

bool ExtensionClient::service_reply( const std::string& frame, uint64_t now, bool& forward, ExtensionInput& input,
                                     bool& info_changed )
{
  forward = info_changed = false;
  const std::string apc = Ext::frame_payload( frame );
  KittyCommand command;
  if ( apc.compare( 0, 3, "Gi=" ) != 0 || !parse_kitty_command( apc.substr( 0, apc.find( ';' ) ), command ) ) {
    return false;
  }
  if ( service_probe_open && command.image_id == uint32_t( PROBE_ID ) ) {
    service_probe_open = false;
    info_changed = !info.vector_service;
    info.vector_service = true;
    return true;
  }
  if ( !service_ids.erase( command.image_id ) ) {
    return false;
  }
  forward = true;
  input.service_reply = apc;
  input.clock = clock( now );
  return true;
}

bool ExtensionClient::heartbeat_due( uint64_t now ) const
{
  return live() && now >= last_sent + HEARTBEAT_MS;
}

ExtensionInput ExtensionClient::heartbeat( uint64_t now )
{
  ExtensionInput input;
  input.clock = clock( now );
  last_sent = now;
  return input;
}

int ExtensionClient::wait_time( uint64_t now ) const
{
  if ( !live() ) {
    return INT_MAX;
  }
  return now >= last_sent + HEARTBEAT_MS ? 0 : int( last_sent + HEARTBEAT_MS - now );
}

/* The server's side */

int64_t ExtensionRelay::now_t() const
{
  return clock_t >= 0 ? clock_t + ( steady_ms() - clock_at ) : steady_ms();
}

void ExtensionRelay::hear_time( const Ext::Fields& fields )
{
  /* The time of a report is when it happened, a link's delay ago; the
     client's own reckoning of the terminal's clock is better, when it sends one. */
  int64_t t;
  if ( !clock_from_client && Ext::integer( fields, "t", 0, T_MAX, t ) == Ext::Value::Valid ) {
    clock_t = t;
    clock_at = steady_ms();
  }
}

void ExtensionRelay::hear_clock( int64_t clock )
{
  if ( clock < 0 ) {
    return;
  }
  clock_t = clock + int64_t( relay_rtt_ms / 2 ); /* it took about half a round trip to get here */
  clock_at = steady_ms();
  clock_from_client = true;
}

void ExtensionRelay::notification( const std::string& payload, std::string& to_host )
{
  if ( payload.compare( 0, 3, "99;" ) == 0 ) {
    std::vector<std::pair<std::string, std::string>> metadata;
    std::string rest;
    const bool well_formed = notification_metadata( payload, metadata, rest );
    bool query = false;
    std::string id, route;
    for ( const auto& item : metadata ) {
      if ( item.first == "p" ) {
        query = item.second == "?";
      } else if ( item.first == "i" ) {
        /* An identifier as kitty keeps one. */
        id.clear();
        for ( char c : item.second ) {
          if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_'
               || c == '+' || c == '.' ) {
            id += c;
          }
        }
      } else if ( item.first == "r" ) {
        route = item.second;
      }
    }
    if ( !well_formed ) {
      return; /* Silkie, and kitty, drop an OSC 99 whose metadata they cannot read */
    }
    if ( query ) {
      /* The relay answers for the terminal, as it does every query: at once,
         from what the client learnt, and the terminal is asked again for the
         next program. One that gave no answer has none given for it, and a
         program is then told what kitty would tell it by saying nothing. */
      if ( client_known && !client.notifications.empty() && ( route.empty() || notification_route_ok( route ) ) ) {
        const bool routed = !route.empty() && client.notification_routes;
        to_host += "\033]99;i=" + ( id.empty() ? std::string( "0" ) : id ) + ":p=?" + ( routed ? ":r=" + route : "" )
                   + ";" + client.notifications + ( routed ? ":r=1" : "" ) + "\033\\";
      }
      if ( client_known ) {
        outputs.push_back( notification_probe() );
      }
      return;
    }
  }
  /* Everything else is the terminal's to read, as the program wrote it,
     return route included: the relay has one window and nothing to restore,
     so it adds no hop of its own, and the answer names the hops beyond it. */
  notices.push_back( "\033]" + payload + "\033\\" );
}

void ExtensionRelay::refresh( const std::string& selector )
{
  /* What a terminal can do may change, as when a pen is paired; the
     terminal is asked again, so the next program to ask is told. */
  Ext::Fields query = head( "query" );
  query.set( "id", PROBE_ID );
  outputs.push_back( Ext::encode( selector, query ) );
}

void ExtensionRelay::reply( const std::string& selector, Ext::Fields fields, std::string& to_host ) const
{
  to_host += Ext::encode( selector, fields );
}

bool ExtensionRelay::accepts_format( uint32_t format ) const
{
  if ( format == KITTY_FORMAT_RGB || format == KITTY_FORMAT_RGBA || format == KITTY_FORMAT_PNG
       || packed_format( format ) ) {
    return true;
  }
  return ( format == KITTY_FORMAT_HDR || format == KITTY_FORMAT_VECTOR ) && client.has_format( std::to_string( format ) );
}

Ext::Fields ExtensionRelay::display_fields() const
{
  /* A relay with no display to speak of says so, field by field. */
  Ext::Fields f;
  f.set( "kind", "unknown" );
  f.set( "color", "rgb" );
  f.set( "bits", int64_t( 0 ) );
  f.set( "gamut", "unknown" );
  f.set( "headroom", "1" );
  f.set( "scale", "0" );
  f.set( "period", int64_t( 0 ) );
  f.set( "ppi", "0" );
  for ( const auto& item : client.display.items ) {
    f.set( item.first, item.second );
  }
  return f;
}

void ExtensionRelay::set_display_mode( bool on, Framebuffer& fb, std::string& to_host )
{
  fb.ds.ext.display = on;
  if ( !on ) {
    return;
  }
  /* Turning the mode on reports the display, and later events are changes
     relative to that report. */
  display_sent = display_fields();
  Ext::Fields f = head( "event" );
  f.set( "t", now_t() );
  for ( const auto& item : display_sent.items ) {
    f.set( item.first, item.second );
  }
  reply( Selector::DISPLAY, f, to_host );
}

uint32_t ExtensionRelay::allowed( const char* selector, uint32_t known ) const
{
  return client.speaks( selector ) ? client.grant( selector, known ) : 0;
}

void ExtensionRelay::describe_devices( uint32_t mode, std::string& to_host ) const
{
  for ( const auto& item : devices ) {
    if ( !( mode & ( item.first.first == 3 ? TABLET_PADS : TABLET_TOOLS ) ) ) {
      continue;
    }
    Ext::Fields f = item.second;
    if ( !( mode & TABLET_IDS ) ) {
      for ( const char* key : { "serial", "hwid", "vid", "pid" } ) {
        f.erase( key );
      }
    }
    reply( Selector::TABLET, f, to_host );
  }
}

void ExtensionRelay::set_client( const ClientExtensions& info, Framebuffer& fb, std::string& to_host )
{
  client = info;
  client_known = true;
  /* Keep only what the terminal now lets programs have: a refused mode, or
     bit of one, reads back as off, and what it covered is cancelled. */
  ExtensionModes& ext = fb.ds.ext;
  const uint32_t tablet = ext.tablet & allowed( Selector::TABLET, TABLET_KNOWN );
  if ( tablet != ext.tablet ) {
    cancel_tablet( fb, to_host );
    ext.tablet = tablet;
    if ( !( tablet & TABLET_PADS ) ) {
      ext.pad_labels.clear();
    }
  }
  const uint32_t gesture = ext.gesture & allowed( Selector::GESTURE, 3 );
  if ( gesture != ext.gesture ) {
    cancel_gestures( to_host );
    ext.gesture = gesture;
  }
  ext.scroll = ext.scroll && allowed( Selector::SCROLL, 1 );
  if ( ext.frame && !allowed( Selector::FRAME, 1 ) ) {
    ext.frame = false;
    frame_seq = 0;
  }
  if ( ext.ink && !allowed( Selector::INK, 1 ) ) {
    ext.ink = false;
    ink_strokes.clear();
  }
  if ( ext.display && !allowed( Selector::DISPLAY, 1 ) ) {
    ext.display = false;
  } else if ( ext.display && display_fields() != display_sent ) {
    set_display_mode( true, fb, to_host );
  }
}

void ExtensionRelay::note_output( Framebuffer& fb )
{
  if ( fb.ds.ext.frame ) {
    fb.ds.ext.frame_output++;
  }
}

void ExtensionRelay::set_alternate( Framebuffer& fb, bool alternate )
{
  fb.ds.ext.alternate = alternate;
}

void ExtensionRelay::end_ink( std::pair<int64_t, int64_t> key, Framebuffer& fb )
{
  const auto it = ink_strokes.find( key );
  if ( it == ink_strokes.end() ) {
    return;
  }
  /* A stroke that is cancelled never happened; the terminal is told too,
     since it may not be the one that cancelled it. */
  Vector::Step step;
  step.records = "x id=" + std::to_string( it->second.second ) + "\n";
  std::string error;
  fb.edit_vector_image( it->second.first, step, error );
  ink_strokes.erase( it );
}

void ExtensionRelay::cancel_tablet( Framebuffer& fb, std::string& to_host )
{
  const int64_t t = now_t();
  std::set<int64_t> finger_devices;
  for ( const auto& item : contacts ) {
    const int64_t kind = std::get<0>( item.first ), device = std::get<1>( item.first ),
                  contact = std::get<2>( item.first );
    const Contact& c = item.second;
    Ext::Fields f = head( "event" );
    f.set( "kind", kind );
    f.set( "phase", int64_t( 4 ) );
    f.set( "device", device );
    f.set( "t", t );
    f.set( "contact", contact );
    f.set( "x", c.x );
    f.set( "y", c.y );
    if ( !c.src.empty() ) {
      f.set( "src", c.src );
    }
    f.set( "buttons", int64_t( 0 ) );
    f.set( "mods", c.mods );
    f.set( "tool", c.tool );
    f.set( "valid", int64_t( c.pressure ? 1 : 0 ) );
    if ( c.pressure ) {
      f.set( "p", int64_t( 0 ) );
    }
    reply( Selector::TABLET, f, to_host );
    if ( kind == 2 ) {
      finger_devices.insert( device );
    }
    suppressed.insert( item.first );
    end_ink( std::make_pair( device, contact ), fb );
  }
  for ( int64_t device : finger_devices ) {
    finger_group.erase( device );
    Ext::Fields f = head( "event" );
    f.set( "kind", int64_t( 2 ) );
    f.set( "phase", int64_t( 6 ) );
    f.set( "device", device );
    f.set( "t", t );
    reply( Selector::TABLET, f, to_host );
  }
  /* A key of a pad that is down is released, with the time of its press. */
  for ( const auto& key : pad_keys ) {
    Ext::Fields f = head( "event" );
    f.set( "kind", int64_t( 3 ) );
    f.set( "device", key.first.first );
    f.set( "t", key.second );
    f.set( "mods", int64_t( 0 ) );
    f.set( "button", key.first.second );
    f.set( "state", int64_t( 0 ) );
    reply( Selector::TABLET, f, to_host );
    released_keys.insert( key.first );
  }
  contacts.clear();
  pad_keys.clear();
  ink_strokes.clear();
}

void ExtensionRelay::cancel_gestures( std::string& to_host )
{
  const int64_t t = now_t();
  for ( const auto& item : gestures ) {
    const Gesture& g = item.second;
    Ext::Fields f = head( "event" );
    f.set( "kind", item.first );
    f.set( "phase", int64_t( 4 ) );
    f.set( "t", t );
    f.set( "x", g.x );
    f.set( "y", g.y );
    f.set( "mods", g.mods );
    if ( item.first == 1 ) {
      f.set( "scale", int64_t( 0 ) );
      f.set( "total", g.total.empty() ? std::string( "1000" ) : g.total );
    } else if ( item.first == 2 ) {
      f.set( "angle", int64_t( 0 ) );
      f.set( "total", g.total.empty() ? std::string( "0" ) : g.total );
    }
    if ( !g.fingers.empty() ) {
      f.set( "fingers", g.fingers );
    }
    reply( Selector::GESTURE, f, to_host );
    suppressed_gestures.insert( item.first );
  }
  gestures.clear();
}

void ExtensionRelay::reset_modes( Framebuffer& fb, std::string& to_host )
{
  ExtensionModes& ext = fb.ds.ext;
  cancel_tablet( fb, to_host );
  ext.tablet = 0;
  ext.pad_labels.clear(); /* the labels were the program's, whose pad reports are off */
  cancel_gestures( to_host );
  ext.gesture = 0;
  ext.scroll = false;
  ext.frame = false;
  frame_seq = 0;
  ext.latency = false;
  ext.display = false;
  ext.ink = false;
  /* Silkie returns the text to the plane of the terminal with the modes. */
  Renditions& rendition = fb.ds.get_renditions();
  if ( rendition.has_depth() ) {
    TextStyle style = text_style_of( rendition.get_text_style() );
    style.depth = 0;
    style.depth_pixels = style.phase = 0;
    rendition.set_text_style( intern_text_style( style ) );
  }
}

void ExtensionRelay::hard_reset( Framebuffer& fb, std::string& to_host )
{
  reset_modes( fb, to_host );
  fb.ds.ext.brush = InkBrush();
  fb.ds.ext.brush_gen++;
  fb.ds.ext.pointer[0].clear();
  fb.ds.ext.pointer[1].clear();
  display_sent = Ext::Fields();
  /* A reset terminal stops continuous feedback. */
  if ( client.speaks( Selector::HAPTIC ) ) {
    outputs.push_back( Ext::encode( Selector::HAPTIC, head( "stop" ) ) );
  }
}

void ExtensionRelay::detach( Framebuffer& fb, std::string& to_host )
{
  cancel_tablet( fb, to_host );
  cancel_gestures( to_host );
  devices.clear(); /* the tools and pads were the terminal's that has gone */
  /* No answer will come for what the terminal was asked: the program is not
     left waiting. An upload that may have reached it is not kept. */
  while ( !service_requests.empty() ) {
    const auto first = service_requests.begin();
    const uint32_t relay_id = first->first;
    const ServiceRequest request = first->second;
    service_requests.erase( first );
    font_refused( relay_id, request, fb );
    service_error( request.id, request.quiet, "The terminal went away before it answered", to_host );
  }
}

void ExtensionRelay::program_frame( const std::string& payload,
                                    Framebuffer& fb,
                                    const ClientGeometry* geometry,
                                    std::string& to_host )
{
  Ext::Frame frame;
  if ( !Ext::parse_program( payload, frame ) ) {
    return;
  }
  const Ext::Fields& f = frame.fields;
  const std::string& selector = frame.selector;
  if (selector == "ssh-agent" && *f.find("v") == "1") {
    const auto op = *f.find("op");
    if (op == "query" || op == "request" || op == "close") outputs.push_back(Ext::encode(frame));
    return;
  }
  if ( selector == Selector::FONT ) {
    font_program( f, fb, to_host );
    return;
  }
  if ( selector == Selector::DEPTH ) {
    depth_program( f, fb );
    return;
  }
  const std::string& op = *f.find( "op" );
  int64_t version = 0, id = -1, mode = -1;
  Ext::integer( f, "v", 0, INT32_MAX, version );
  if ( version != 1 || Ext::integer( f, "id", 0, INT32_MAX, id ) == Ext::Value::Invalid
       || Ext::integer( f, "mode", 0, INT32_MAX, mode ) == Ext::Value::Invalid ) {
    return;
  }
  ExtensionModes& ext = fb.ds.ext;

  if ( selector == Selector::CAPS ) {
    /* Turning the modes off needs nothing of the terminal. The listing is the
       relay's own, so it is answered once the terminal is described, whether
       or not that terminal has a listing of its own: only the selectors that
       reach a terminal are listed, those the relay carries and the client's
       terminal speaks, and latency, which the relay honours itself. */
    if ( op == "reset" ) {
      reset_modes( fb, to_host );
    } else if ( op == "query" && id >= 0 && client_known ) {
      std::string list = "caps.1,ssh-agent.1";
      for ( const char* s : { Selector::DEPTH, Selector::DISPLAY, Selector::FONT, Selector::FRAME, Selector::GESTURE, Selector::HAPTIC,
                              Selector::INK, Selector::LATENCY, Selector::SCROLL, Selector::TABLET } ) {
        if ( client.speaks( s ) || std::string( s ) == Selector::LATENCY ) {
          list += std::string( "," ) + s + ".1";
        }
      }
      /* Files, shared memory and GPU surfaces are read on this machine,
         where the program is, and cross the link as pixels. */
      /* The packed formats are expanded for a terminal that does not list
         them, so they are there whatever the terminal. */
      std::string format = "301,302,304,308,516,24,32";
      // New raw state needs a peer that understands it. Indexed formats also
      // require the terminal's live palette; the others have render fallbacks.
      if ( fb.image_encoding.native ) {
        format += ",316,401,402,404,408,632,664";
        for ( const char* indexed : { "8", "16" } ) {
          if ( client.has_format( indexed ) ) { format += std::string( "," ) + indexed; }
        }
      }
      for ( const char* extra : { "64", "100", "200" } ) {
        if ( std::string( extra ) == "100" || client.has_format( extra ) ) {
          format += std::string( "," ) + extra;
        }
      }
      Ext::Fields r = head( "cap" );
      r.set( "id", id );
      r.set( "caps", int64_t( 3 ) );
      r.set( "ext", list );
      r.set( "media", gpu_surfaces_supported() ? "d,f,t,s,g" : "d,f,t,s" );
      r.set( "format", format );
      /* Placements are kept here, so only what is implemented here is listed. */
      if ( client.has_graphics( "viewport" ) ) {
        r.set( "graphics", "viewport.1" );
      }
      reply( Selector::CAPS, r, to_host );
    }
    return;
  }

  if ( selector != Selector::LATENCY && !client.speaks( selector ) ) {
    return; /* a selector that cannot reach the display is not answered */
  }

  if ( op == "query" && id >= 0 && selector != Selector::DISPLAY ) {
    refresh( selector );
  }

  const auto cap = [&]( int64_t mode_value ) {
    Ext::Fields r = head( "cap" );
    r.set( "id", id );
    r.set( "caps", client.cap_int( selector, "caps", 0 ) );
    r.set( "mode", mode_value );
    return r;
  };
  const auto read_mode = [&]( int64_t mode_value ) {
    Ext::Fields r = head( "mode" );
    r.set( "mode", mode_value );
    reply( selector, r, to_host );
  };

  if ( selector == Selector::TABLET ) {
    tablet_program( op, f, fb, geometry, to_host );
  } else if ( selector == Selector::SCROLL ) {
    if ( op == "query" && id >= 0 ) {
      reply( selector, cap( ext.scroll ), to_host );
    } else if ( op == "mode" ) {
      if ( mode < 0 ) {
        read_mode( ext.scroll );
      } else {
        ext.scroll = mode != 0 && client.grant( selector, 1 );
      }
    }
  } else if ( selector == Selector::GESTURE ) {
    if ( op == "query" && id >= 0 ) {
      reply( selector, cap( ext.gesture ), to_host );
    } else if ( op == "mode" ) {
      if ( mode < 0 ) {
        read_mode( ext.gesture );
      } else {
        cancel_gestures( to_host );
        ext.gesture = mode & client.grant( selector, 3 );
      }
    }
  } else if ( selector == Selector::FRAME ) {
    if ( op == "query" && id >= 0 ) {
      Ext::Fields r = cap( ext.frame );
      const std::string* period = client.display.find( "period" );
      r.set( "period", period ? *period : std::to_string( client.cap_int( selector, "period", 0 ) ) );
      reply( selector, r, to_host );
    } else if ( op == "mode" ) {
      if ( mode < 0 ) {
        read_mode( ext.frame );
      } else {
        ext.frame = mode != 0 && client.grant( selector, 1 );
        frame_seq = 0;
      }
    }
  } else if ( selector == Selector::LATENCY ) {
    if ( op == "query" && id >= 0 ) {
      /* The relay's own waits come before the terminal's. */
      Ext::Fields r = head( "cap" );
      r.set( "id", id );
      r.set( "caps", int64_t( 1 ) );
      r.set( "mode", int64_t( ext.latency ) );
      r.set( "input", int64_t( relay_input_us ) + client.cap_int( selector, "input", 0 ) );
      r.set( "repaint", int64_t( relay_repaint_us ) + client.cap_int( selector, "repaint", 0 ) );
      reply( selector, r, to_host );
    } else if ( op == "mode" ) {
      if ( mode < 0 ) {
        read_mode( ext.latency );
      } else {
        ext.latency = mode != 0;
      }
    }
  } else if ( selector == Selector::DISPLAY ) {
    if ( op == "query" && id >= 0 ) {
      Ext::Fields r = head( "cap" );
      r.set( "id", id );
      r.set( "mode", int64_t( ext.display ) );
      for ( const auto& item : display_fields().items ) {
        r.set( item.first, item.second );
      }
      reply( selector, r, to_host );
    } else if ( op == "mode" ) {
      if ( mode < 0 ) {
        read_mode( ext.display );
      } else {
        set_display_mode( mode != 0 && client.grant( selector, 1 ), fb, to_host );
      }
    }
  } else if ( selector == Selector::INK ) {
    ink_program( op, f, fb, to_host );
  } else if ( selector == Selector::HAPTIC ) {
    haptic_program( op, f, payload, to_host );
  }
}

void ExtensionRelay::tablet_program( const std::string& op,
                                     const Ext::Fields& f,
                                     Framebuffer& fb,
                                     const ClientGeometry* geometry,
                                     std::string& to_host )
{
  ExtensionModes& ext = fb.ds.ext;
  int64_t id = -1, mode = -1;
  Ext::integer( f, "id", 0, INT32_MAX, id );
  Ext::integer( f, "mode", 0, INT32_MAX, mode );
  if ( op == "query" && id >= 0 ) {
    Ext::Fields r = head( "cap" );
    r.set( "id", id );
    r.set( "caps", client.cap_int( Selector::TABLET, "caps", 0 ) );
    r.set( "mode", int64_t( ext.tablet ) );
    /* Physical pixels of the cell area, as the client measures them. */
    if ( geometry && geometry->columns == unsigned( fb.ds.get_width() )
         && geometry->rows == unsigned( fb.ds.get_height() )
         && ( geometry->has_cell_size() || geometry->has_pixel_size() ) ) {
      const unsigned cw = geometry->has_cell_size() ? geometry->cell_width_px : geometry->width_px / geometry->columns;
      const unsigned ch = geometry->has_cell_size() ? geometry->cell_height_px
                                                    : geometry->height_px / geometry->rows;
      const unsigned w = std::min( 4096U, std::max( 1U, cw ) ), h = std::min( 4096U, std::max( 1U, ch ) );
      r.set( "w", int64_t( std::min<uint64_t>( 65535, uint64_t( geometry->columns ) * w ) ) );
      r.set( "h", int64_t( std::min<uint64_t>( 65535, uint64_t( geometry->rows ) * h ) ) );
      r.set( "cw", int64_t( w ) );
      r.set( "ch", int64_t( h ) );
    }
    reply( Selector::TABLET, r, to_host );
  } else if ( op == "mode" ) {
    if ( mode < 0 ) {
      Ext::Fields r = head( "mode" );
      r.set( "mode", int64_t( ext.tablet ) );
      reply( Selector::TABLET, r, to_host );
      return;
    }
    const uint32_t before = ext.tablet;
    cancel_tablet( fb, to_host );
    ext.tablet = uint32_t( mode ) & allowed( Selector::TABLET, TABLET_KNOWN );
    if ( !( ext.tablet & TABLET_PADS ) ) {
      ext.pad_labels.clear();
    }
    /* The terminal describes what is in proximity again when its mode
       changes. When it does not, because the program set the mode it had,
       the relay does, as the terminal would have. */
    if ( ext.tablet == before ) {
      describe_devices( ext.tablet, to_host );
    }
  } else if ( op == "label" ) {
    /* Accepted only from the program the pads report to. */
    int64_t value = 0, control_index = 0;
    uint64_t device = 0;
    uint32_t control = 0;
    unsigned controls = 0;
    for ( uint32_t which = 0; which < 4; which++ ) {
      const Ext::Value v = Ext::integer( f, PAD_CONTROLS[which], 0, 255, value );
      if ( v == Ext::Value::Invalid ) {
        return;
      }
      if ( v == Ext::Value::Valid ) {
        controls++;
        control = which;
        control_index = value;
      }
    }
    const std::string* text = f.find( "text" );
    if ( !( ext.tablet & TABLET_PADS ) || Ext::unsigned_integer( f, "device", UINT64_MAX, device ) != Ext::Value::Valid
         || controls != 1 || ( text && !valid_label( *text ) ) ) {
      return;
    }
    const PadControl key( device, control, uint32_t( control_index ) );
    if ( !text || text->empty() ) {
      ext.pad_labels.erase( key );
    } else {
      ext.pad_labels[key] = *text;
    }
  }
}

void ExtensionRelay::ink_program( const std::string& op, const Ext::Fields& f, Framebuffer& fb, std::string& to_host )
{
  ExtensionModes& ext = fb.ds.ext;
  int64_t id = -1, mode = -1;
  Ext::integer( f, "id", 0, INT32_MAX, id );
  Ext::integer( f, "mode", 0, INT32_MAX, mode );
  if ( op == "query" && id >= 0 ) {
    Ext::Fields r = head( "cap" );
    r.set( "id", id );
    r.set( "caps", client.cap_int( Selector::INK, "caps", 1 ) );
    r.set( "mode", int64_t( ext.ink ) );
    if ( ext.brush.present() ) {
      for ( const auto& item : ext.brush.fields().items ) {
        r.set( item.first, item.second );
      }
    }
    reply( Selector::INK, r, to_host );
  } else if ( op == "mode" ) {
    if ( mode < 0 ) {
      Ext::Fields r = head( "mode" );
      r.set( "mode", int64_t( ext.ink ) );
      reply( Selector::INK, r, to_host );
    } else if ( mode == 0 ) {
      /* What has been drawn stays, and no more is added to it. */
      ext.ink = false;
      ink_strokes.clear();
    } else if ( mode == 1 && ext.brush.present() ) {
      ext.ink = allowed( Selector::INK, 1 ) != 0; /* a refused mode reads back as off */
    }
  } else if ( op == "brush" ) {
    InkBrush brush;
    if ( parse_brush( f, brush ) ) {
      ext.brush = brush;
      ext.brush_gen++;
      ext.ink = allowed( Selector::INK, 1 ) != 0;
    }
  }
}

void ExtensionRelay::haptic_program( const std::string& op,
                                     const Ext::Fields& f,
                                     const std::string& payload,
                                     std::string& to_host )
{
  int64_t id = -1, intensity = 100;
  if ( Ext::integer( f, "intensity", 0, 100, intensity ) == Ext::Value::Invalid ) {
    return;
  }
  Ext::integer( f, "id", 0, INT32_MAX, id );
  if ( !one_of( f, "target", { "trackpad", "pen", "dial" }, false ) ) {
    return;
  }
  if ( op == "query" && id >= 0 ) {
    Ext::Fields r = head( "cap" );
    r.set( "id", id );
    r.set( "caps", client.cap_int( Selector::HAPTIC, "caps", 0 ) );
    reply( Selector::HAPTIC, r, to_host );
    return;
  }
  /* Feedback is played at the far end: the request is passed on as it came. */
  const bool play = op == "play" && one_of( f, "pattern", { "generic", "alignment", "level", "click", "complete" }, true );
  const bool start = op == "start" && one_of( f, "pattern", { "ink", "pencil", "marker", "brush", "eraser" }, true );
  const bool valid = play || start || op == "stop";
  if ( valid ) {
    outputs.push_back( "\033]" + payload + "\033\\" );
  }
}

/* A selection is a text attribute, and it is kept whatever the terminal behind
   the relay: the relay answers for the cells, and the display decides who is
   told of them. So the client's own copy of the screen takes one without
   having heard of any terminal. */
void ExtensionRelay::font_program( const Ext::Fields& f, Framebuffer& fb, std::string& to_host )
{
  int64_t version = 0, id = 0, weight = 0, slant = 0, italic = -1;
  if ( Ext::integer( f, "v", 0, INT32_MAX, version ) != Ext::Value::Valid || version != 1
       || Ext::integer( f, "id", 0, INT32_MAX, id ) == Ext::Value::Invalid ) {
    return;
  }
  const std::string& op = *f.find( "op" );
  Renditions& rendition = fb.ds.get_renditions();
  TextStyle style = text_style_of( rendition.get_text_style() );
  const std::string* role = f.find( "role" );
  if ( role ) {
    const std::string& name = *role;
    const auto letter = []( char c ) { return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ); };
    if ( name.empty() || !letter( name[0] ) ) {
      return;
    }
    for ( char c : name ) {
      if ( !letter( c ) && !( c >= '0' && c <= '9' ) && c != '_' && c != '.' && c != '-' ) {
        return;
      }
    }
    style.role = name;
  }
  const Ext::Value given_weight = Ext::integer( f, "weight", 0, 900, weight );
  if ( given_weight == Ext::Value::Invalid || ( given_weight == Ext::Value::Valid && weight != 0 && weight < 100 ) ) {
    return;
  }
  if ( given_weight == Ext::Value::Valid ) {
    style.weight = weight;
  }
  const std::string* given_slant = f.find( "slant" );
  if ( given_slant && *given_slant == "auto" ) {
    style.has_slant = false;
    style.slant = 0;
  } else if ( given_slant ) {
    if ( Ext::integer( f, "slant", -1500, 1500, slant ) != Ext::Value::Valid ) {
      return;
    }
    style.has_slant = true;
    style.slant = slant;
  }
  if ( Ext::integer( f, "italic", 0, 1, italic ) == Ext::Value::Invalid ) {
    return;
  }

  const auto roles = client.caps.find( Selector::FONT );
  const std::string* listed = roles == client.caps.end() ? nullptr : roles->second.find( "roles" );
  if ( op == "select" ) {
    if ( listed && style.role != "default" ) {
      const auto names = split( *listed, ',' );
      if ( std::find( names.begin(), names.end(), style.role ) == names.end() ) {
        style.role = "default";
      }
    }
    rendition.set_text_style( intern_text_style( style ) );
    if ( italic >= 0 ) {
      rendition.set_attribute( Renditions::italic, italic != 0 );
    }
  } else if ( op == "reset" ) {
    /* The font alone: the depth of the text is another protocol's. */
    TextStyle plain;
    plain.depth = style.depth;
    plain.depth_pixels = style.depth_pixels;
    plain.phase = style.phase;
    rendition.set_text_style( intern_text_style( plain ) );
  } else if ( client.speaks( Selector::FONT ) && ( op == "query" || op == "state" ) ) {
    Ext::Fields r = head( op == "query" ? "cap" : "state" );
    r.set( "id", id );
    if ( op == "query" ) {
      if ( roles != client.caps.end() ) {
        for ( const auto& item : roles->second.items ) {
          r.set( item.first, item.second );
        }
      }
      refresh( Selector::FONT ); /* the roles change when the user reloads the configuration */
    } else {
      const TextStyle& current = text_style_of( rendition.get_text_style() );
      r.set( "role", current.role );
      r.set( "weight", int64_t( current.weight ) );
      r.set( "slant", current.has_slant ? std::to_string( current.slant ) : std::string( "auto" ) );
      r.set( "italic", int64_t( rendition.get_attribute( Renditions::italic ) ) );
    }
    reply( Selector::FONT, r, to_host );
  }
}

/* Depth is a text attribute like the font: kept whatever the terminal, and
   read as Silkie reads it. A frame it would reject changes nothing. */
void ExtensionRelay::depth_program( const Ext::Fields& f, Framebuffer& fb )
{
  int64_t version = 0, depth = 0;
  if ( Ext::integer( f, "v", 0, INT32_MAX, version ) != Ext::Value::Valid || version != 1
       || Ext::integer( f, "d", INT32_MIN, INT32_MAX, depth ) == Ext::Value::Invalid
       || ( f.has( "p" ) && f.has( "d" ) ) ) {
    return;
  }
  float pixels = 0, phase = 0;
  {
    const Ext::CNumbers c_numbers;
    const std::pair<const char*, float*> floats[] = { { "p", &pixels }, { "y", &phase } };
    for ( const auto& item : floats ) {
      const std::string* value = f.find( item.first );
      if ( !value ) {
        continue;
      }
      if ( value->empty() || value->find_first_not_of( "-0123456789.eE+" ) != std::string::npos ) {
        return;
      }
      char* end = nullptr;
      const double number = strtod( value->c_str(), &end );
      /* A value a float cannot hold, or holds as nothing, is rejected. */
      if ( *end || !std::isfinite( number ) || !std::isfinite( static_cast<float>( number ) )
           || ( number != 0 && static_cast<float>( number ) == 0 ) ) {
        return;
      }
      *item.second = static_cast<float>( number );
    }
  }
  const std::string& op = *f.find( "op" );
  Renditions& rendition = fb.ds.get_renditions();
  TextStyle style = text_style_of( rendition.get_text_style() );
  if ( op == "reset" ) {
    style.depth = 0;
    style.depth_pixels = style.phase = 0;
  } else if ( op == "set" ) {
    if ( f.has( "d" ) || f.has( "p" ) ) {
      /* Either form of depth replaces the other. */
      style.depth = static_cast<int32_t>( depth );
      style.depth_pixels = pixels;
    }
    if ( f.has( "y" ) ) {
      style.phase = phase;
    }
  } else {
    return;
  }
  rendition.set_text_style( intern_text_style( style ) );
}

void ExtensionRelay::font_slot( Framebuffer& fb, int slot )
{
  /* SGR 10 is the default role whatever the terminal listed, and so is a
     slot it listed no role for. */
  const auto roles = client.caps.find( Selector::FONT );
  const std::string* listed = roles == client.caps.end() ? nullptr : roles->second.find( "roles" );
  const auto names = listed ? split( *listed, ',' ) : std::vector<std::string>();
  Renditions& rendition = fb.ds.get_renditions();
  TextStyle style = text_style_of( rendition.get_text_style() );
  style.role = size_t( slot ) < names.size() ? names[slot] : "default";
  rendition.set_text_style( intern_text_style( style ) );
}

void ExtensionRelay::vector_service( uint32_t id,
                                     uint32_t quiet,
                                     const std::string& json,
                                     Framebuffer& fb,
                                     std::string& to_host )
{
  if ( !client_known ) {
    service_error( id, quiet, "The terminal's capabilities are not known yet", to_host );
    return;
  }
  if ( !client.vector_service ) {
    service_error( id, quiet, "The terminal has no vector service", to_host );
    return;
  }
  ServiceRequest request;
  request.id = id;
  request.quiet = quiet;
  request.sent_ms = steady_ms();
  /* Uploaded fonts are terminal state the relay keeps; it answers a release
     itself, so the client releases the font after what still uses it. */
  if ( font_request( json, fb, request, to_host ) ) {
    return;
  }
  /* The fonts are the terminal's, and so are the answers. */
  service_requests[service_next] = request;
  outputs.push_back( encode_vector_service( service_next, json ) );
  /* Not the ids the client has for itself, whatever the program's are. */
  service_next = service_next >= 0xffffff00U ? 0x80000000U : service_next + 1;
}

void ExtensionRelay::service_answer( const std::string& reply, Framebuffer& fb, std::string& to_host )
{
  /* Gi=<id>;OK;<base64> or Gi=<id>;<error> */
  const size_t semi = reply.find( ';' );
  int64_t relay_id = 0;
  if ( semi == std::string::npos || reply.compare( 0, 3, "Gi=" ) != 0
       || !Ext::parse_integer( reply.substr( 3, semi - 3 ), 0, UINT32_MAX, relay_id ) ) {
    return;
  }
  const auto found = service_requests.find( uint32_t( relay_id ) );
  if ( found == service_requests.end() ) {
    return;
  }
  const ServiceRequest request = found->second;
  service_requests.erase( found );
  const bool ok = reply.compare( semi + 1, 3, "OK;" ) == 0 || reply.compare( semi + 1, std::string::npos, "OK" ) == 0;
  font_answer( uint32_t( relay_id ), request, reply.substr( semi + 1 ), ok, fb );
  if ( request.quiet == 0 || ( request.quiet == 1 && !ok ) ) {
    to_host += kitty_response( request.id, 0, reply.substr( semi + 1 ) );
  }
}

void ExtensionRelay::input_frame( const ExtensionInput& input, Framebuffer& fb, std::string& to_host )
{
  hear_clock( input.clock );
  if ( !input.service_reply.empty() ) {
    service_answer( input.service_reply, fb, to_host );
  }
  /* What the terminal drew as ink is in the table before the program hears
     of it, as in the terminal; whether or not the report goes on, since the
     terminal drew it either way. */
  if ( input.ink_image && !input.ink_records.empty() ) {
    Vector::Step step;
    step.records = input.ink_records;
    step.terminal_applied = true;
    std::string error;
    fb.edit_vector_image( input.ink_image, step, error );
  }
  Ext::Frame frame;
  if ( input.payload.empty() || !Ext::parse( input.payload, frame ) ) {
    return; /* a heartbeat */
  }
  Ext::Fields& f = frame.fields;
  const std::string* op = f.find( "op" );
  int64_t version = 0;
  if ( !op || Ext::integer( f, "v", 0, INT32_MAX, version ) != Ext::Value::Valid || version != 1 ) {
    return;
  }
  if (frame.selector == "ssh-agent") {
    if (*op == "cap" || *op == "reply") to_host += Ext::encode(frame);
    return;
  }
  hear_time( f );
  const ExtensionModes& ext = fb.ds.ext;
  const std::string& selector = frame.selector;
  bool forward = false;
  if ( selector == Selector::TABLET && ( *op == "event" || *op == "device" ) ) {
    forward = tablet_input( f, fb, input );
  } else if ( selector == Selector::SCROLL && *op == "event" ) {
    forward = ext.scroll;
  } else if ( selector == Selector::GESTURE && *op == "event" ) {
    int64_t kind = 0, phase = 0;
    Ext::integer( f, "kind", 1, INT32_MAX, kind );
    Ext::integer( f, "phase", 1, INT32_MAX, phase );
    const uint32_t bit = kind >= 1 && kind <= 4 ? 1 : kind == 5 ? 2 : 0;
    forward = ( ext.gesture & bit ) != 0;
    if ( forward && phase == 1 ) {
      suppressed_gestures.erase( kind );
      Gesture& g = gestures[kind];
      const std::string* fingers = f.find( "fingers" );
      g.fingers = fingers ? *fingers : std::string();
    } else if ( forward && ( suppressed_gestures.count( kind ) || !gestures.count( kind ) ) ) {
      forward = false; /* the rest of a gesture the relay cancelled, or never saw begin */
    }
    if ( forward && ( phase == 1 || phase == 2 ) ) {
      /* What a cancel of the gesture would carry. */
      Gesture& g = gestures[kind];
      const std::pair<const char*, std::string*> keep[]
        = { { "x", &g.x }, { "y", &g.y }, { "mods", &g.mods }, { "total", &g.total } };
      for ( const auto& item : keep ) {
        const std::string* value = f.find( item.first );
        if ( value ) {
          *item.second = *value;
        }
      }
    } else if ( forward && ( phase == 3 || phase == 4 ) ) {
      gestures.erase( kind );
    }
  } else if ( selector == Selector::FRAME && *op == "event" && ext.frame ) {
    /* The relay counts the reports of its own window. */
    f.set( "seq", int64_t( ++frame_seq ) );
    forward = true;
  }
  if ( forward ) {
    to_host += Ext::encode( selector, f );
  }
}

bool ExtensionRelay::tablet_input( Ext::Fields& f, Framebuffer& fb, const ExtensionInput& input )
{
  const uint32_t mode = fb.ds.ext.tablet;
  int64_t kind = 0;
  if ( Ext::integer( f, "kind", 1, INT32_MAX, kind ) != Ext::Value::Valid ) {
    return false;
  }
  if ( *f.find( "op" ) == "device" ) {
    const uint32_t need = kind == 1 ? TABLET_TOOLS : kind == 3 ? TABLET_PADS : 0;
    int64_t device;
    if ( need && Ext::integer( f, "device", 0, INT64_MAX / 2, device ) == Ext::Value::Valid ) {
      /* Whole, for a later mode: a tool's until it leaves, a pad's for as
         long as the terminal is attached. */
      devices[std::make_pair( kind, device )] = f;
    }
    if ( !( mode & need ) ) {
      return false;
    }
    if ( !( mode & TABLET_IDS ) ) {
      for ( const char* key : { "serial", "hwid", "vid", "pid" } ) {
        f.erase( key );
      }
    }
    return true;
  }
  const uint32_t need = kind == 1   ? TABLET_TOOLS
                        : kind == 2 ? TABLET_FINGERS
                        : kind == 3 ? TABLET_PADS
                        : kind == 4 ? TABLET_ACTIONS
                                    : 0;
  if ( !( mode & need ) ) {
    return false;
  }
  int64_t phase = -1, device = 0, contact = 0, t = 0;
  Ext::integer( f, "phase", 0, INT32_MAX, phase );
  Ext::integer( f, "device", 0, INT64_MAX / 2, device );
  Ext::integer( f, "contact", 0, INT64_MAX / 2, contact );
  Ext::integer( f, "t", 0, T_MAX, t );
  if ( !( mode & TABLET_ESTIMATES ) && ( phase == 7 || f.has( "pred" ) ) ) {
    return false;
  }
  if ( kind == 3 ) {
    int64_t button, state;
    if ( Ext::integer( f, "button", 0, INT32_MAX, button ) == Ext::Value::Valid
         && Ext::integer( f, "state", 0, 1, state ) == Ext::Value::Valid ) {
      const auto key = std::make_pair( device, button );
      if ( state ) {
        released_keys.erase( key );
        pad_keys[key] = t;
      } else {
        pad_keys.erase( key );
        if ( released_keys.erase( key ) ) {
          return false; /* the relay reported this release already */
        }
      }
    }
    return true;
  }
  if ( kind == 4 ) {
    return true;
  }
  const ContactKey key( kind, device, contact );
  switch ( phase ) {
    case 1: {
      suppressed.erase( key );
      Contact& c = contacts[key];
      int64_t valid = 0;
      Ext::integer( f, "tool", 0, INT32_MAX, c.tool );
      Ext::integer( f, "mods", 0, INT32_MAX, c.mods );
      Ext::integer( f, "valid", 0, INT32_MAX, valid );
      c.pressure = valid & 1;
      c.x = f.find( "x" ) ? *f.find( "x" ) : "0";
      c.y = f.find( "y" ) ? *f.find( "y" ) : "0";
      c.src = f.find( "src" ) ? *f.find( "src" ) : std::string();
    } break;
    case 2:
    case 3:
    case 4: {
      /* The rest of a stroke the relay cancelled, or of one it never saw
         begin, as after it reconnects: a program is told of a contact from
         its down. */
      if ( suppressed.count( key ) || !contacts.count( key ) ) {
        return false;
      }
      if ( phase == 2 ) {
        const auto it = contacts.find( key );
        if ( it != contacts.end() ) {
          Ext::integer( f, "mods", 0, INT32_MAX, it->second.mods );
          it->second.x = f.find( "x" ) ? *f.find( "x" ) : it->second.x;
          it->second.y = f.find( "y" ) ? *f.find( "y" ) : it->second.y;
        }
      } else {
        contacts.erase( key );
      }
    } break;
    case 5:
      for ( auto it = contacts.begin(); it != contacts.end(); ) {
        it = std::get<0>( it->first ) == kind && std::get<1>( it->first ) == device ? contacts.erase( it ) : ++it;
      }
      devices.erase( std::make_pair( kind, device ) );
      break;
    case 6:
      /* A frame follows a group only when something of it was reported. */
      if ( !finger_group.erase( device ) ) {
        return false;
      }
      break;
    case 7:
      if ( suppressed.count( key ) ) {
        return false; /* a correction of a stroke that was cancelled */
      }
      break;
    default:
      break;
  }
  if ( kind == 2 && phase != 6 ) {
    finger_group.insert( device );
  }
  /* The strokes the terminal inks, and the ids it uses up. */
  const auto stroke = std::make_pair( device, contact );
  int64_t element;
  if ( kind == 1 && phase == 1 ) {
    ink_strokes.erase( stroke );
    if ( Ext::integer( f, "ink", 1, UINT32_MAX, element ) == Ext::Value::Valid ) {
      InkBrush& brush = fb.ds.ext.brush;
      if ( brush.present() && uint64_t( element ) >= brush.next && uint64_t( element ) - brush.next < brush.remaining ) {
        brush.remaining -= uint32_t( element - brush.next + 1 );
        brush.next = uint32_t( element + 1 );
      }
      if ( input.ink_image ) {
        ink_strokes[stroke] = std::make_pair( input.ink_image, uint32_t( element ) );
      }
    }
  } else if ( kind == 1 && ( phase == 3 || phase == 4 ) ) {
    ink_strokes.erase( stroke );
  }
  return true;
}

void ExtensionRelay::pointer_shape( const std::string& payload, Framebuffer& fb, std::string& to_host )
{
  if ( client.pointer_shapes.empty() ) {
    return; /* the client's terminal has no pointer shapes */
  }
  /* What the terminal said of each name it was asked about. */
  std::vector<std::string> asked = { "__default__", "__grabbed__" };
  asked.insert( asked.end(), std::begin( POINTER_NAMES ), std::end( POINTER_NAMES ) );
  asked.push_back( "image:1:2:3" );
  const std::vector<std::string> answers = split( client.pointer_shapes, ',' );
  const auto answer_for = [&]( const std::string& name ) {
    for ( size_t i = 0; i < asked.size() && i < answers.size(); i++ ) {
      if ( asked[i] == name ) {
        return answers[i];
      }
    }
    return std::string( "0" );
  };
  /* A name the terminal takes, as kitty writes it; empty for one it does not. */
  const auto known = [&]( const std::string& name ) {
    const std::string image = pointer_image( name );
    if ( !image.empty() ) {
      return answer_for( "image:1:2:3" ) == "1" ? image : std::string();
    }
    for ( const char* alias : POINTER_ALIASES ) {
      if ( name == alias ) {
        return name;
      }
    }
    return answer_for( name ) == "1" && name.compare( 0, 2, "__" ) != 0 ? name : std::string();
  };

  ExtensionModes& ext = fb.ds.ext;
  std::vector<std::string>& stack = ext.pointer[ext.alternate ? 1 : 0];
  const char first = payload.empty() ? '=' : payload[0];
  const std::string names = first == '=' || first == '>' || first == '<' || first == '?' ? payload.substr( payload.empty() ? 0 : 1 ) : payload;
  if ( first == '?' ) {
    outputs.push_back( pointer_probe() ); /* asked again, for the next program that asks */
    std::string out;
    for ( const auto& name : split( names, ',' ) ) {
      std::string answer;
      if ( name == "__current__" ) {
        answer = ext.pointer_shape( &fb );
        answer = answer.empty() ? "0" : answer;
      } else if ( name == "__default__" || name == "__grabbed__" ) {
        answer = answer_for( name );
      } else {
        answer = !pointer_image( name ).empty() ? answer_for( "image:1:2:3" ) : answer_for( name );
      }
      out += ( out.empty() ? "" : "," ) + answer;
    }
    to_host += "\033]22;" + out + "\033\\";
  } else if ( first == '<' ) {
    if ( !stack.empty() ) {
      stack.pop_back();
    }
  } else {
    /* A set replaces the top, an empty name with no shape; a push adds. */
    for ( const auto& name : split( names, ',' ) ) {
      if ( name.empty() && first == '>' ) {
        continue;
      }
      const std::string shape = name.empty() ? std::string() : known( name );
      if ( !name.empty() && shape.empty() ) {
        continue; /* a name the terminal does not know is ignored */
      }
      if ( first == '>' || stack.empty() ) {
        stack.push_back( shape );
      } else {
        stack.back() = shape;
      }
    }
  }
}

bool ExtensionRelay::termcap( const std::string& name, std::string& value ) const
{
  /* The headroom of the display, 1 when there is none or nothing is known
     of it yet, as docs/hdr-protocol.rst says. */
  if ( name == "kitty-query-hdr_headroom" ) {
    const std::string* headroom = client.listing ? client.display.find( "headroom" ) : nullptr;
    value = headroom ? *headroom : "1";
    return true;
  }
  return false;
}

}
