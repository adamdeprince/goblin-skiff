/*
    Goblin Skiff: vector images (f=200) of the graphics protocol
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/inputlimits.h"
#include "src/terminal/osc52.h"
#include "src/terminal/vectorscene.h"
#include "src/terminal/extframe.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>

namespace Terminal {
namespace Vector {



Matrix multiply( const Matrix& m, const Matrix& n )
{
  Matrix r;
  r.a = m.a * n.a + m.c * n.b;
  r.b = m.b * n.a + m.d * n.b;
  r.c = m.a * n.c + m.c * n.d;
  r.d = m.b * n.c + m.d * n.d;
  r.e = m.a * n.e + m.c * n.f + m.e;
  r.f = m.b * n.e + m.d * n.f + m.f;
  return r;
}

bool invert( const Matrix& m, Matrix& out )
{
  const double det = m.a * m.d - m.b * m.c;
  if ( std::fabs( det ) < 1e-12 ) {
    return false;
  }
  out.a = m.d / det;
  out.b = -m.b / det;
  out.c = -m.c / det;
  out.d = m.a / det;
  out.e = ( m.c * m.f - m.d * m.e ) / det;
  out.f = ( m.b * m.e - m.a * m.f ) / det;
  return true;
}

void apply( const Matrix& m, double& x, double& y )
{
  const double ox = m.a * x + m.c * y + m.e;
  const double oy = m.b * x + m.d * y + m.f;
  x = ox;
  y = oy;
}

/* Record parsing. Each check mirrors one of the terminal's, with its message. */

namespace {
typedef const char* cursor;

bool fail( std::string& error, const char* why )
{
  error = why;
  return false;
}

/* A number, after any commas: digits, a point, an exponent, and a sign that
   starts the next number unless it follows the exponent. */
bool parse_num( cursor& s, cursor end, double& out )
{
  cursor p = s;
  while ( p < end && *p == ',' ) {
    p++;
  }
  std::string text;
  while ( p < end
          && ( isdigit( static_cast<unsigned char>( *p ) ) || *p == '-' || *p == '+' || *p == '.' || *p == 'e'
               || *p == 'E' ) ) {
    if ( !text.empty() && ( *p == '-' || *p == '+' ) && text.back() != 'e' && text.back() != 'E' ) {
      break;
    }
    text += *p++;
  }
  if ( text.empty() ) {
    return false;
  }
  char* stop = nullptr;
  out = strtod( text.c_str(), &stop );
  if ( *stop || !std::isfinite( out ) ) {
    return false;
  }
  s = p;
  return true;
}

bool parse_nums( cursor& s, cursor end, unsigned n )
{
  double v;
  for ( unsigned i = 0; i < n; i++ ) {
    if ( !parse_num( s, end, v ) ) {
      return false;
    }
  }
  return true;
}

bool parse_path( cursor s, cursor end, size_t& commands, std::string& error )
{
  char cmd = 0;
  commands = 0;
  while ( s < end ) {
    if ( *s == ',' ) {
      s++;
      continue;
    }
    if ( isalpha( static_cast<unsigned char>( *s ) ) ) {
      cmd = *s++;
    } else if ( !cmd ) {
      return fail( error, "Number without a path command" );
    }
    const bool rel = islower( static_cast<unsigned char>( cmd ) );
    switch ( toupper( static_cast<unsigned char>( cmd ) ) ) {
      case 'M':
        if ( !parse_nums( s, end, 2 ) ) {
          return fail( error, "Bad moveto in path" );
        }
        cmd = rel ? 'l' : 'L'; /* further pairs are implicit linetos */
        break;
      case 'L':
        if ( !parse_nums( s, end, 2 ) ) {
          return fail( error, "Bad lineto in path" );
        }
        break;
      case 'H':
      case 'V':
        if ( !parse_nums( s, end, 1 ) ) {
          return fail( error, "Bad lineto in path" );
        }
        break;
      case 'C':
        if ( !parse_nums( s, end, 6 ) ) {
          return fail( error, "Bad curveto in path" );
        }
        break;
      case 'S':
      case 'Q':
        if ( !parse_nums( s, end, 4 ) ) {
          return fail( error, "Bad curveto in path" );
        }
        break;
      case 'T':
        if ( !parse_nums( s, end, 2 ) ) {
          return fail( error, "Bad curveto in path" );
        }
        break;
      case 'Z':
        cmd = 0; /* a number after a closepath would repeat it forever */
        break;
      default:
        return fail( error, "Unknown path command" );
    }
    commands++;
  }
  return true;
}

bool hex_digit( char c, unsigned& out )
{
  if ( c >= '0' && c <= '9' ) {
    out = c - '0';
  } else if ( c >= 'a' && c <= 'f' ) {
    out = c - 'a' + 10;
  } else if ( c >= 'A' && c <= 'F' ) {
    out = c - 'A' + 10;
  } else {
    return false;
  }
  return true;
}

enum class Paint
{
  None,
  Solid,
  Gradient
};

/* The terminal's pipeline for a color, in single precision as it keeps one:
   decoding the sRGB transfer curve, converting a wide gamut to linear
   extended sRGB, then the brightness. Whatever comes out of it that is not
   finite is an overflow. (Silkie's vector-color.h.) */
float decode_srgb( float x )
{
  return x <= .04045f ? x / 12.92f : powf( ( x + .055f ) / 1.055f, 2.4f );
}

void gamut_to_srgb( float* c, unsigned gamut )
{
  if ( !gamut || gamut > 2 ) {
    return;
  }
  /* D65 primaries, without clipping to the gamut. */
  static const double to_xyz[2][9] = {
    { .4865709, .2656677, .1982173, .2289746, .6917385, .0792869, 0, .0451134, 1.0439444 },
    { .6369580, .1446169, .1688810, .2627002, .6779981, .0593017, 0, .0280727, 1.0609851 }
  };
  static const double from_xyz[9]
    = { 3.2404542, -1.5371385, -.4985314, -.9692660, 1.8760108, .0415560, .0556434, -.2040259, 1.0572252 };
  double xyz[3] = { 0, 0, 0 }, rgb[3] = { 0, 0, 0 };
  for ( unsigned i = 0; i < 3; i++ ) {
    for ( unsigned j = 0; j < 3; j++ ) {
      xyz[i] += to_xyz[gamut - 1][3 * i + j] * c[j];
    }
  }
  for ( unsigned i = 0; i < 3; i++ ) {
    for ( unsigned j = 0; j < 3; j++ ) {
      rgb[i] += from_xyz[3 * i + j] * xyz[j];
    }
  }
  for ( unsigned i = 0; i < 3; i++ ) {
    c[i] = static_cast<float>( rgb[i] );
  }
}

bool parse_color( cursor s, cursor end, Paint& paint, std::string& error )
{
  const size_t n = end - s;
  if ( n == 4 && !memcmp( s, "none", 4 ) ) {
    paint = Paint::None;
    return true;
  }
  if ( n > 1 && *s == '@' ) {
    double id;
    s++;
    if ( !parse_num( s, end, id ) || s != end || id < 0 || id > UINT32_MAX || id != std::floor( id ) ) {
      return fail( error, "Bad gradient reference" );
    }
    paint = Paint::Gradient;
    return true;
  }
  const char* star = static_cast<const char*>( memchr( s, '*', n ) ); /* a brightness in percent */
  cursor color_end = star ? star : end;
  const char* colon = static_cast<const char*>( memchr( s, ':', color_end - s ) );
  float channels[4] = { 0, 0, 0, 1 };
  if ( colon ) {
    /* srgb:, p3:, rec2020: or linear:, with three or four components. */
    const std::string space( s, colon );
    const bool linear = space == "linear";
    unsigned gamut = 0;
    if ( space == "p3" ) {
      gamut = 1;
    } else if ( space == "rec2020" ) {
      gamut = 2;
    } else if ( space != "srgb" && !linear ) {
      return fail( error, "Unknown color space" );
    }
    cursor q = colon + 1;
    unsigned count = 0;
    while ( q < color_end ) {
      double v;
      if ( count == 4 || !parse_num( q, color_end, v ) ) {
        return fail( error, "Bad color components" );
      }
      if ( ( !linear || count == 3 ) && ( v < 0 || v > 1 ) ) {
        return fail( error, "Color component outside 0..1" );
      }
      channels[count++] = static_cast<float>( v );
      if ( q < color_end && ( *q++ != '/' || q == color_end ) ) {
        return fail( error, "Bad color components" );
      }
    }
    if ( count < 3 ) {
      return fail( error, "Color needs three or four components" );
    }
    if ( !linear ) {
      for ( unsigned i = 0; i < 3; i++ ) {
        channels[i] = decode_srgb( channels[i] );
      }
    }
    gamut_to_srgb( channels, gamut );
  } else {
    if ( n < 7 || *s != '#' ) {
      return fail( error, "Bad color" );
    }
    const size_t hexlen = ( star ? static_cast<size_t>( star - s ) : n ) - 1;
    if ( hexlen != 6 && hexlen != 8 ) {
      return fail( error, "Bad color" );
    }
    unsigned v[4] = { 0, 0, 0, 255 };
    for ( size_t i = 0; i < hexlen / 2; i++ ) {
      unsigned high, low;
      if ( !hex_digit( s[1 + 2 * i], high ) || !hex_digit( s[2 + 2 * i], low ) ) {
        return fail( error, "Bad color" );
      }
      v[i] = high << 4 | low;
    }
    for ( unsigned i = 0; i < 3; i++ ) {
      channels[i] = decode_srgb( v[i] / 255.f );
    }
    channels[3] = v[3] / 255.f;
  }
  if ( star ) {
    double brightness;
    cursor q = star + 1;
    if ( !parse_num( q, end, brightness ) || q != end || brightness < 0 ) {
      return fail( error, "Bad brightness" );
    }
    for ( unsigned i = 0; i < 3; i++ ) {
      channels[i] *= static_cast<float>( brightness / 100 );
    }
  }
  for ( float channel : channels ) {
    if ( !std::isfinite( channel ) ) {
      return fail( error, "Color overflow" );
    }
  }
  paint = Paint::Solid;
  return true;
}

bool parse_list( cursor s, cursor end, size_t& count, bool& negative, std::string& error )
{
  count = 0;
  negative = false;
  while ( s < end ) {
    double v;
    if ( !parse_num( s, end, v ) ) {
      return fail( error, "Bad number list" );
    }
    negative = negative || v < 0;
    count++;
  }
  return true;
}

bool parse_stops( cursor s, cursor end, std::string& error )
{
  unsigned stops = 0;
  while ( s < end ) {
    double offset;
    if ( !parse_num( s, end, offset ) ) {
      return fail( error, "Bad gradient stop" );
    }
    if ( s >= end || *s != ',' ) {
      return fail( error, "Bad gradient stop" );
    }
    s++;
    cursor c = static_cast<cursor>( memchr( s, ',', end - s ) );
    if ( !c ) {
      c = end;
    }
    Paint paint;
    if ( !parse_color( s, c, paint, error ) || paint != Paint::Solid ) {
      return fail( error, "Bad gradient stop color" );
    }
    s = c;
    stops++;
  }
  if ( !stops ) {
    return fail( error, "Gradient without stops" );
  }
  return true;
}

bool parse_choice( cursor s, cursor end, std::initializer_list<const char*> names, std::string& error )
{
  for ( const char* name : names ) {
    if ( strlen( name ) == static_cast<size_t>( end - s ) && !memcmp( s, name, end - s ) ) {
      return true;
    }
  }
  return fail( error, "Unknown keyword" );
}

bool parse_uint( cursor s, cursor end, uint32_t& out, std::string& error )
{
  double v;
  if ( !parse_num( s, end, v ) || s != end || v < 0 || v > UINT32_MAX || v != std::floor( v ) ) {
    return fail( error, "Bad integer" );
  }
  out = static_cast<uint32_t>( v );
  return true;
}

bool parse_number_field( cursor s, cursor end, std::string& error )
{
  double v;
  if ( !parse_num( s, end, v ) || s != end ) {
    return fail( error, "Bad number" );
  }
  return true;
}

struct Parsing
{
  Record record;
  bool have_id = false, have_g = false, have_fill = false, have_stroke = false;
  uint32_t ref = 0;          /* s, clip or img */
  uint32_t doomed_group = 0; /* g of a delete */
  unsigned stops = 0;
  bool have_text = false;
  std::string text; /* the last text field, laid out once the record is read */
};

bool parse_field( Parsing& p, const std::string& key, cursor v, cursor end, std::string& error )
{
  Record& r = p.record;
  if ( key == "id" ) {
    if ( !parse_uint( v, end, r.id, error ) ) {
      return false;
    }
    p.have_id = true;
  } else if ( key == "g" ) {
    uint32_t g;
    if ( !parse_uint( v, end, g, error ) ) {
      return false;
    }
    if ( r.op == 'x' ) {
      r.delete_group = true;
      p.doomed_group = g;
    } else {
      r.parent = g;
    }
    p.have_g = true;
  } else if ( key == "p" ) {
    if ( !parse_uint( v, end, r.parent, error ) ) {
      return false;
    }
  } else if ( key == "t" ) {
    double m[6];
    cursor q = v;
    for ( double& x : m ) {
      if ( !parse_num( q, end, x ) ) {
        return fail( error, "Bad transform" );
      }
    }
    if ( q != end ) {
      return fail( error, "Bad transform" );
    }
    r.t.a = m[0];
    r.t.b = m[1];
    r.t.c = m[2];
    r.t.d = m[3];
    r.t.e = m[4];
    r.t.f = m[5];
    r.has_t = true;
  } else if ( key == "depth" || key == "depth_px" || key == "phase" ) {
    /* The depth of a record or of a group's descendants, in millidegrees or
       in reference pixels, and its vertical phase: for the terminal, which
       draws the eyes; here they are only checked as it checks them. */
    if ( !parse_number_field( v, end, error ) ) {
      return false;
    }
  } else if ( key == "box" ) {
    /* A rectangular clip: x, y, width, height. */
    double b[4];
    cursor q = v;
    for ( double& x : b ) {
      if ( !parse_num( q, end, x ) ) {
        return fail( error, "Bad clipping box" );
      }
    }
    if ( q != end || b[2] < 0 || b[3] < 0 || !std::isfinite( b[0] + b[2] ) || !std::isfinite( b[1] + b[3] ) ) {
      return fail( error, "Bad clipping box" );
    }
  } else if ( key == "text" && r.op == 't' ) {
    p.text.assign( v, end );
    p.have_text = true;
  } else if ( key == "clip" || key == "s" || key == "img" ) {
    if ( !parse_uint( v, end, p.ref, error ) ) {
      return false;
    }
  } else if ( key == "o" || key == "v" || key == "w" || key == "ml" || key == "dp" || key == "x1" || key == "y1"
              || key == "x2" || key == "y2" || key == "cx" || key == "cy" || key == "r" || key == "fx"
              || key == "fy" ) {
    return parse_number_field( v, end, error );
  } else if ( key == "blend" ) {
    return parse_choice( v, end, { "normal", "multiply", "screen" }, error );
  } else if ( key == "d" ) {
    /* A second d continues the path of the first, as the terminal reads it. */
    size_t commands = 0;
    if ( !parse_path( v, end, commands, error ) ) {
      return false;
    }
    r.d_commands += commands;
    r.d_parts.emplace_back( v, end );
    r.has_d = true;
  } else if ( key == "fill" || key == "stroke" ) {
    Paint paint;
    if ( !parse_color( v, end, paint, error ) ) {
      return false;
    }
    ( key == "fill" ? p.have_fill : p.have_stroke ) = true;
  } else if ( key == "rule" ) {
    return parse_choice( v, end, { "nz", "eo" }, error );
  } else if ( key == "cap" ) {
    return parse_choice( v, end, { "butt", "round", "square" }, error );
  } else if ( key == "join" ) {
    return parse_choice( v, end, { "miter", "round", "bevel" }, error );
  } else if ( key == "dash" ) {
    size_t n;
    bool negative;
    if ( !parse_list( v, end, n, negative, error ) ) {
      return false;
    }
    if ( negative ) {
      return fail( error, "Negative dash length" );
    }
  } else if ( key == "pts" ) {
    size_t n;
    bool negative;
    if ( !parse_list( v, end, n, negative, error ) ) {
      return false;
    }
    if ( n % 3 ) {
      return fail( error, "Pen points must be x,y,width triples" );
    }
    r.pts.assign( v, end );
    r.pts_count = n / 3;
    r.has_pts = true;
  } else if ( key == "stops" ) {
    if ( !parse_stops( v, end, error ) ) {
      return false;
    }
    p.stops++;
  } /* unknown keys are ignored, so the format can grow */
  return true;
}

bool parse_record( cursor line, cursor end, Record& out, std::string& error, const FontCheck* fonts )
{
  out = Record();
  while ( line < end && *line == ' ' ) {
    line++;
  }
  if ( line >= end ) {
    return true; /* blank */
  }
  Parsing p;
  Record& r = p.record;
  r.op = *line++;
  if ( line < end && *line != ' ' ) {
    return fail( error, "Record type must be a single character" );
  }
  if ( !strchr( "gpsuikalrxt", r.op ) ) {
    r.op = 0; /* unknown record types are ignored, so the format can grow */
    return true;
  }
  while ( line < end ) {
    while ( line < end && *line == ' ' ) {
      line++;
    }
    if ( line >= end ) {
      break;
    }
    cursor token_end = static_cast<cursor>( memchr( line, ' ', end - line ) );
    if ( !token_end ) {
      token_end = end;
    }
    cursor eq = static_cast<cursor>( memchr( line, '=', token_end - line ) );
    if ( !eq ) {
      if ( r.op == 'x' && token_end - line == 3 && !memcmp( line, "all", 3 ) ) {
        r.all = true;
      } else {
        return fail( error, "Field without =" );
      }
    } else if ( !parse_field( p, std::string( line, eq ), eq + 1, token_end, error ) ) {
      return false;
    }
    line = token_end;
  }
  if ( r.op == 'x' ) {
    if ( !r.all && !p.have_id && !p.have_g ) {
      return fail( error, "Delete record without id, g or all" );
    }
    if ( r.delete_group ) {
      r.id = p.doomed_group;
    }
    out = r;
    return true;
  }
  if ( !p.have_id ) {
    return fail( error, "Record without id" );
  }
  if ( !r.id && r.op != 'g' ) {
    return fail( error, "Only the root group has id 0" );
  }
  switch ( r.op ) {
    case 't':
      /* As the terminal lays it out, when the whole record is read; what
         needs the fonts themselves is the terminal's to check. */
      if ( !p.have_text ) {
        return fail( error, "Text record without text" );
      }
      try {
        if ( !text_layout_ok( p.text, fonts, r.fonts ) ) {
          return fail( error, "Invalid vector text, font or layout" );
        }
      } catch ( const std::bad_alloc& ) {
        return fail( error, "Out of memory laying out vector text" ); /* Silkie's words for a MemoryError */
      }
      break;
    case 'p':
    case 's':
      if ( !r.has_d ) {
        return fail( error, "Path without d" );
      }
      break;
    case 'u':
      if ( !p.ref ) {
        return fail( error, "Use without s" );
      }
      break;
    case 'i':
      if ( !p.ref ) {
        return fail( error, "Image without img" );
      }
      break;
    case 'k':
      if ( !r.has_pts ) {
        return fail( error, "Pen without pts" );
      }
      break;
    case 'a':
      if ( !r.has_d && !r.has_pts ) {
        return fail( error, "Append without d or pts" );
      }
      break;
    case 'l':
    case 'r':
      if ( !p.stops ) {
        return fail( error, "Gradient without stops" );
      }
      break;
  }
  out = r;
  return true;
}

/* The record without its pts field, which a pen stroke keeps apart. */
std::string without_pts( const std::string& line )
{
  std::string out;
  size_t at = 0;
  while ( at < line.size() ) {
    size_t end = line.find( ' ', at );
    if ( end == std::string::npos ) {
      end = line.size();
    }
    if ( line.compare( at, 4, "pts=" ) != 0 || end - at < 4 ) {
      if ( !out.empty() ) {
        out += ' ';
      }
      out.append( line, at, end - at );
    }
    at = end;
    while ( at < line.size() && line[at] == ' ' ) {
      at++;
    }
  }
  return out;
}
}

bool color_paints( const std::string& color )
{
  Ext::CNumbers c_numbers;
  Paint paint;
  std::string error;
  return color.find( ' ' ) == std::string::npos && parse_color( color.data(), color.data() + color.size(), paint, error )
         && paint != Paint::None;
}

bool parse_records( const std::string& text, std::vector<Record>& out, std::string& error, const FontCheck* fonts )
{
  Ext::CNumbers c_numbers;
  out.clear();
  cursor s = text.data(), end = s + text.size();
  unsigned lineno = 0;
  while ( s < end ) {
    cursor nl = static_cast<cursor>( memchr( s, '\n', end - s ) );
    cursor le = nl ? nl : end;
    if ( le > s && le[-1] == '\r' ) {
      le--;
    }
    lineno++;
    Record record;
    std::string why;
    if ( !parse_record( s, le, record, why, fonts ) ) {
      error = "Line " + std::to_string( lineno ) + ": " + ( why.empty() ? std::string( "Bad record" ) : why );
      out.clear();
      return false;
    }
    if ( record.op ) {
      cursor start = s;
      while ( start < le && *start == ' ' ) {
        start++;
      }
      record.line.assign( start, le );
      out.push_back( std::move( record ) );
    }
    s = nl ? nl + 1 : end;
  }
  return true;
}

size_t Element::bytes() const
{
  size_t total = line.size() + pts.size() + 1;
  for ( const auto& path : paths ) {
    total += path.size() + 16;
  }
  return total;
}

const Element* Table::lookup( uint32_t id ) const
{
  const auto it = elements.find( id );
  return it == elements.end() ? nullptr : &it->second.second;
}

bool Table::is_descendant( const Element& e, uint32_t group ) const
{
  /* As the terminal walks it: the group is compared before the walk stops
     at the root, so everything is under group 0, and no depth limit cuts
     it short (a chain of parents can be no longer than the scene). */
  uint32_t p = e.parent;
  for ( size_t remaining = live; remaining; remaining-- ) {
    if ( p == group ) {
      return true;
    }
    if ( !p ) {
      return false;
    }
    const Element* g = lookup( p );
    if ( !g || g->type != 'g' ) {
      return false;
    }
    p = g->parent;
  }
  return false;
}

void Table::count_fonts( const Element& e, bool adding )
{
  for ( const auto& name : e.fonts ) {
    if ( adding ) {
      font_refs[name]++;
    } else {
      const auto it = font_refs.find( name );
      if ( it != font_refs.end() && !--it->second ) {
        font_refs.erase( it );
      }
    }
  }
}

void Table::remove( uint32_t id )
{
  const auto it = elements.find( id );
  if ( it == elements.end() ) {
    return;
  }
  order[it->second.first] = 0;
  count_fonts( it->second.second, false );
  element_bytes -= it->second.second.bytes();
  elements.erase( it );
  live--;
}

void Table::insert( Element e )
{
  const auto it = elements.find( e.id );
  if ( it != elements.end() ) {
    /* A replacement keeps its place in the order. */
    element_bytes -= it->second.second.bytes();
    element_bytes += e.bytes();
    count_fonts( it->second.second, false );
    count_fonts( e, true );
    it->second.second = std::move( e );
    return;
  }
  count_fonts( e, true );
  element_bytes += e.bytes();
  order.push_back( e.id );
  const uint32_t id = e.id;
  elements.emplace( id, std::make_pair( order.size() - 1, std::move( e ) ) );
  live++;
}

void Table::compact()
{
  if ( order.size() < 64 || live * 2 > order.size() ) {
    return;
  }
  size_t out = 0;
  for ( size_t i = 0; i < order.size(); i++ ) {
    if ( !order[i] ) {
      continue;
    }
    elements[order[i]].first = out;
    order[out++] = order[i];
  }
  order.resize( out );
}

std::string Table::apply( const std::vector<Record>& records )
{
  std::string error;
  for ( const Record& r : records ) {
    switch ( r.op ) {
      case 'x':
        if ( r.all ) {
          order.clear();
          elements.clear();
          font_refs.clear();
          live = 0;
          element_bytes = 0;
          root_line.clear();
          root_has_t = false;
          root_t = Matrix();
        } else if ( r.delete_group ) {
          /* Mark first: the chains of parents run through the groups that go. */
          std::vector<uint32_t> doomed;
          for ( uint32_t id : order ) {
            const Element* e = id ? lookup( id ) : nullptr;
            if ( e && ( e->id == r.id || is_descendant( *e, r.id ) ) ) {
              doomed.push_back( id );
            }
          }
          for ( uint32_t id : doomed ) {
            remove( id );
          }
        } else {
          remove( r.id );
        }
        break;
      case 'a': {
        const auto it = elements.find( r.id );
        if ( it == elements.end() ) {
          break;
        }
        Element& target = it->second.second;
        element_bytes -= target.bytes();
        /* Only geometry is appended: a d or pts without any is no append. */
        if ( r.d_commands ) {
          if ( target.type != 'p' && target.type != 's' ) {
            error = "Append to an element of another kind";
          } else {
            target.paths.insert( target.paths.end(), r.d_parts.begin(), r.d_parts.end() );
          }
        }
        if ( error.empty() && r.pts_count ) {
          if ( target.type != 'k' ) {
            error = "Append to an element of another kind";
          } else {
            target.pts += target.pts.empty() ? r.pts : "," + r.pts;
          }
        }
        element_bytes += target.bytes();
      } break;
      case 'g':
        if ( !r.id ) { /* the root group */
          root_line = r.line;
          root_has_t = r.has_t;
          root_t = r.has_t ? r.t : Matrix();
          break;
        }
        /* fallthrough */
      default: {
        Element e;
        e.type = r.op;
        e.id = r.id;
        e.parent = r.parent;
        e.has_t = r.has_t;
        e.t = r.t;
        e.fonts = r.fonts;
        if ( r.op == 'k' ) {
          e.line = without_pts( r.line );
          e.pts = r.pts;
        } else {
          e.line = r.line;
        }
        insert( std::move( e ) );
      } break;
    }
    if ( !error.empty() ) {
      break;
    }
  }
  compact();
  return error;
}

std::string Table::text() const
{
  return text( []( const std::string& ) { return true; } );
}

std::string Table::text( const std::function<bool( const std::string& )>& font_ok ) const
{
  std::string out;
  if ( !root_line.empty() ) {
    out += root_line;
    out += '\n';
  }
  for ( uint32_t id : order ) {
    const Element* e = id ? lookup( id ) : nullptr;
    if ( !e || !std::all_of( e->fonts.begin(), e->fonts.end(), font_ok ) ) {
      continue;
    }
    out += e->line;
    if ( e->type == 'k' ) {
      out += " pts=";
      out += e->pts;
    }
    out += '\n';
    for ( const auto& path : e->paths ) {
      out += "a id=" + std::to_string( e->id ) + " d=" + path + "\n";
    }
  }
  return out;
}

/* The transform from the coordinate system of e to that of the image: 0 when
   e hangs off something that is not a group, -1 when the groups nest too deep. */
int Table::matrix_of( const Element& e, Matrix& out ) const
{
  Matrix m = e.has_t ? e.t : Matrix();
  uint32_t p = e.parent;
  size_t guard = elements.size();
  unsigned depth = 0;
  while ( p && guard-- ) {
    if ( vector_depth_limit && ++depth > vector_depth_limit ) {
      return -1;
    }
    const Element* g = lookup( p );
    if ( !g || g->type != 'g' ) {
      return 0;
    }
    if ( g->has_t ) {
      m = multiply( g->t, m );
    }
    p = g->parent;
  }
  if ( p ) {
    return -1; /* the parent chain is a cycle */
  }
  out = root_has_t ? multiply( root_t, m ) : m;
  return 1;
}

bool Table::group_inverse( uint32_t group, Matrix& out ) const
{
  Matrix m = root_has_t ? root_t : Matrix();
  if ( group ) {
    const Element* g = lookup( group );
    if ( !g || g->type != 'g' || matrix_of( *g, m ) != 1 ) {
      return false;
    }
  }
  return invert( m, out );
}

bool Table::point_to_group( uint32_t group, double& x, double& y ) const
{
  Matrix inverse;
  if ( !group_inverse( group, inverse ) ) {
    return false;
  }
  Vector::apply( inverse, x, y );
  return std::isfinite( x ) && std::isfinite( y );
}

/* Versions */

std::shared_ptr<const Table> Doc::table() const
{
  if ( table_ ) {
    return table_;
  }
  if ( checkpoint_ ) {
    return checkpoint_;
  }
  /* Made again from the newest checkpoint before this version. */
  std::vector<const Doc*> path;
  const Doc* at = this;
  while ( at && !at->table_ && !at->checkpoint_ ) {
    path.push_back( at );
    at = at->parent_.get();
  }
  if ( !at ) {
    return std::make_shared<Table>();
  }
  auto table = std::make_shared<Table>( at->table_ ? *at->table_ : *at->checkpoint_ );
  for ( auto it = path.rbegin(); it != path.rend(); ++it ) {
    for ( const Step& step : ( *it )->steps_ ) {
      std::vector<Record> records;
      std::string error;
      if ( parse_records( step.records, records, error ) ) {
        table->apply( records );
      }
    }
  }
  return table;
}

uint64_t new_lineage()
{
  static std::atomic<uint64_t> counter { 0 };
  static const uint64_t seed = std::random_device()() ^ static_cast<uint64_t>(
                                 std::chrono::steady_clock::now().time_since_epoch().count() );
  return ( seed << 20 ) ^ ++counter;
}

std::shared_ptr<const Doc> make_doc( uint64_t lineage,
                                     uint64_t version,
                                     uint32_t width,
                                     uint32_t height,
                                     std::shared_ptr<Table> table )
{
  auto doc = std::make_shared<Doc>();
  doc->lineage = lineage;
  doc->version = version;
  doc->width = width;
  doc->height = height;
  doc->checkpoint_ = std::make_shared<const Table>( *table );
  doc->table_ = std::move( table );
  return doc;
}

std::shared_ptr<const Doc> edit_doc( const std::shared_ptr<const Doc>& base, const Step& step, std::string& error )
{
  auto doc = std::make_shared<Doc>();
  doc->lineage = base->lineage;
  doc->version = base->version + 1;
  doc->width = base->width;
  doc->height = base->height;
  doc->parent_ = base;
  doc->steps_.push_back( step );
  doc->chain_bytes_ = base->chain_bytes_ + step.records.size();
  /* The newest table moves on to the newest version; anything older is
     made again on demand from a checkpoint, which only a relay that diffs
     against an older state of its own ever needs. */
  if ( base->table_ && base->table_.use_count() == 1 ) {
    doc->table_ = std::move( base->table_ );
  } else {
    doc->table_ = std::make_shared<Table>( *base->table() );
  }
  std::vector<Record> records;
  error.clear();
  if ( parse_records( step.records, records, error ) ) {
    error = doc->table_->apply( records );
  }
  /* Keep at least as many steps as the scene is large, so a peer a little
     behind is sent steps rather than everything, then start a checkpoint and
     forget the steps before the previous one. */
  if ( doc->chain_bytes_ > std::max<size_t>( 64 * 1024, 2 * doc->table_->bytes() ) ) {
    const Doc* previous = base.get();
    while ( previous && !previous->checkpoint_ ) {
      previous = previous->parent_.get();
    }
    if ( previous ) {
      previous->parent_.reset();
    }
    doc->checkpoint_ = std::make_shared<const Table>( *doc->table_ );
    doc->chain_bytes_ = 0;
  }
  return doc;
}

bool steps_between( uint64_t lineage, uint64_t from, const std::shared_ptr<const Doc>& to, std::vector<Step>& out )
{
  out.clear();
  if ( !to || to->lineage != lineage || to->version < from ) {
    return false;
  }
  std::vector<const Doc*> path;
  const Doc* at = to.get();
  while ( at && at->version > from ) {
    path.push_back( at );
    at = at->parent().get();
  }
  if ( !at || at->version != from ) {
    return false;
  }
  for ( auto it = path.rbegin(); it != path.rend(); ++it ) {
    out.insert( out.end(), ( *it )->steps().begin(), ( *it )->steps().end() );
  }
  return true;
}

}
}
