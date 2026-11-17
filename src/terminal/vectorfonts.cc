/*
    Goblin Skiff: the session fonts and text layouts of vector images (f=200)
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/vectorfonts.h"
#include "src/terminal/extframe.h"
#include "src/terminal/vectorscene.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

namespace Terminal {
namespace Vector {

namespace {

/* Decoding, as bytes.decode(encoding, 'surrogatepass') does it */

bool decode_utf8( const unsigned char* s, size_t n, std::vector<uint32_t>& out )
{
  size_t i = 0;
  while ( i < n ) {
    const unsigned c = s[i];
    if ( c < 0x80 ) {
      out.push_back( c );
      i++;
      continue;
    }
    unsigned need;
    uint32_t cp, least;
    if ( c >= 0xc2 && c <= 0xdf ) {
      need = 1, cp = c & 0x1f, least = 0x80;
    } else if ( c >= 0xe0 && c <= 0xef ) {
      need = 2, cp = c & 0x0f, least = 0x800;
    } else if ( c >= 0xf0 && c <= 0xf4 ) {
      need = 3, cp = c & 0x07, least = 0x10000;
    } else {
      return false;
    }
    if ( n - i <= need ) {
      return false;
    }
    for ( unsigned k = 1; k <= need; k++ ) {
      const unsigned d = s[i + k];
      if ( ( d & 0xc0 ) != 0x80 ) {
        return false;
      }
      cp = cp << 6 | ( d & 0x3f );
    }
    if ( cp < least || cp > 0x10ffff ) {
      return false;
    }
    out.push_back( cp ); /* a surrogate too: surrogatepass lets it through */
    i += need + 1;
  }
  return true;
}

uint32_t unit16( const unsigned char* s, bool big )
{
  return big ? uint32_t( s[0] ) << 8 | s[1] : uint32_t( s[1] ) << 8 | s[0];
}

bool decode_utf16( const unsigned char* s, size_t n, bool big, std::vector<uint32_t>& out )
{
  if ( n % 2 ) {
    return false;
  }
  for ( size_t i = 0; i < n; i += 2 ) {
    const uint32_t u = unit16( s + i, big );
    if ( u >= 0xd800 && u <= 0xdbff && i + 3 < n ) {
      const uint32_t v = unit16( s + i + 2, big );
      if ( v >= 0xdc00 && v <= 0xdfff ) {
        out.push_back( 0x10000 + ( ( u - 0xd800 ) << 10 ) + ( v - 0xdc00 ) );
        i += 2;
        continue;
      }
    }
    out.push_back( u );
  }
  return true;
}

bool decode_utf32( const unsigned char* s, size_t n, bool big, std::vector<uint32_t>& out )
{
  if ( n % 4 ) {
    return false;
  }
  for ( size_t i = 0; i < n; i += 4 ) {
    const uint32_t cp = big ? uint32_t( s[i] ) << 24 | uint32_t( s[i + 1] ) << 16 | uint32_t( s[i + 2] ) << 8 | s[i + 3]
                            : uint32_t( s[i + 3] ) << 24 | uint32_t( s[i + 2] ) << 16 | uint32_t( s[i + 1] ) << 8 | s[i];
    if ( cp > 0x10ffff ) {
      return false;
    }
    out.push_back( cp );
  }
  return true;
}

/* json.detect_encoding, then the decoding */
bool decode_json_bytes( const std::string& bytes, std::vector<uint32_t>& out )
{
  const unsigned char* b = reinterpret_cast<const unsigned char*>( bytes.data() );
  const size_t n = bytes.size();
  const auto starts = [&]( const char* prefix, size_t len ) { return n >= len && !memcmp( b, prefix, len ); };
  if ( starts( "\x00\x00\xfe\xff", 4 ) ) {
    return decode_utf32( b + 4, n - 4, true, out );
  }
  if ( starts( "\xff\xfe\x00\x00", 4 ) ) {
    return decode_utf32( b + 4, n - 4, false, out );
  }
  if ( starts( "\xfe\xff", 2 ) ) {
    return decode_utf16( b + 2, n - 2, true, out );
  }
  if ( starts( "\xff\xfe", 2 ) ) {
    return decode_utf16( b + 2, n - 2, false, out );
  }
  if ( starts( "\xef\xbb\xbf", 3 ) ) {
    return decode_utf8( b + 3, n - 3, out );
  }
  if ( n >= 4 ) {
    if ( !b[0] ) {
      return b[1] ? decode_utf16( b, n, true, out ) : decode_utf32( b, n, true, out );
    }
    if ( !b[1] ) {
      return b[2] || b[3] ? decode_utf16( b, n, false, out ) : decode_utf32( b, n, false, out );
    }
  } else if ( n == 2 ) {
    if ( !b[0] ) {
      return decode_utf16( b, n, true, out );
    }
    if ( !b[1] ) {
      return decode_utf16( b, n, false, out );
    }
  }
  return decode_utf8( b, n, out );
}

void append_wtf8( std::string& out, uint32_t cp )
{
  if ( cp < 0x80 ) {
    out += char( cp );
  } else if ( cp < 0x800 ) {
    out += char( 0xc0 | cp >> 6 );
    out += char( 0x80 | ( cp & 0x3f ) );
  } else if ( cp < 0x10000 ) {
    out += char( 0xe0 | cp >> 12 );
    out += char( 0x80 | ( cp >> 6 & 0x3f ) );
    out += char( 0x80 | ( cp & 0x3f ) );
  } else {
    out += char( 0xf0 | cp >> 18 );
    out += char( 0x80 | ( cp >> 12 & 0x3f ) );
    out += char( 0x80 | ( cp >> 6 & 0x3f ) );
    out += char( 0x80 | ( cp & 0x3f ) );
  }
}

/* The code points of a string kept as WTF-8. */
std::vector<uint32_t> code_points( const std::string& text )
{
  std::vector<uint32_t> out;
  decode_utf8( reinterpret_cast<const unsigned char*>( text.data() ), text.size(), out );
  return out;
}

bool has_surrogate( const std::string& text )
{
  for ( uint32_t cp : code_points( text ) ) {
    if ( cp >= 0xd800 && cp <= 0xdfff ) {
      return true;
    }
  }
  return false;
}

int hex_value( uint32_t c )
{
  if ( c >= '0' && c <= '9' ) {
    return int( c - '0' );
  }
  if ( c >= 'a' && c <= 'f' ) {
    return int( c - 'a' + 10 );
  }
  if ( c >= 'A' && c <= 'F' ) {
    return int( c - 'A' + 10 );
  }
  return -1;
}

bool is_digit( uint32_t c )
{
  return c >= '0' && c <= '9';
}

/* Python's json scanner, strict, over code points. */
class Scanner
{
  const std::vector<uint32_t>& s;
  size_t at = 0;
  Json& doc;

  void space()
  {
    while ( at < s.size() && ( s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r' ) ) {
      at++;
    }
  }
  bool word( const char* w ) const
  {
    const size_t n = strlen( w );
    if ( s.size() - at < n ) {
      return false;
    }
    for ( size_t i = 0; i < n; i++ ) {
      if ( s[at + i] != uint32_t( static_cast<unsigned char>( w[i] ) ) ) {
        return false;
      }
    }
    return true;
  }
  bool hex4( size_t from, uint32_t& out ) const
  {
    if ( s.size() - from < 4 ) {
      return false;
    }
    out = 0;
    for ( size_t i = 0; i < 4; i++ ) {
      const int v = hex_value( s[from + i] );
      if ( v < 0 ) {
        return false;
      }
      out = out << 4 | uint32_t( v );
    }
    return true;
  }
  /* After the opening quote. */
  bool string( std::string& out )
  {
    while ( true ) {
      if ( at >= s.size() ) {
        return false;
      }
      const uint32_t c = s[at++];
      if ( c == '"' ) {
        return true;
      }
      if ( c < 0x20 ) {
        return false; /* "Invalid control character" */
      }
      if ( c != '\\' ) {
        append_wtf8( out, c );
        continue;
      }
      if ( at >= s.size() ) {
        return false;
      }
      const uint32_t e = s[at++];
      switch ( e ) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          uint32_t u;
          if ( !hex4( at, u ) ) {
            return false;
          }
          at += 4;
          if ( u >= 0xd800 && u <= 0xdbff && s.size() - at > 6 && s[at] == '\\' && s[at + 1] == 'u' ) {
            uint32_t low;
            if ( !hex4( at + 2, low ) ) {
              return false;
            }
            if ( low >= 0xdc00 && low <= 0xdfff ) {
              u = 0x10000 + ( ( u - 0xd800 ) << 10 ) + ( low - 0xdc00 );
              at += 6;
            }
          }
          append_wtf8( out, u );
        } break;
        default:
          return false; /* "Invalid \escape" */
      }
    }
  }
  /* A number as _match_number_unicode reads one. */
  bool number( Json::Node& node )
  {
    const size_t start = at;
    size_t i = at;
    if ( s[i] == '-' ) {
      i++;
      if ( i >= s.size() ) {
        return false;
      }
    }
    size_t digits = 0;
    if ( s[i] >= '1' && s[i] <= '9' ) {
      while ( i < s.size() && is_digit( s[i] ) ) {
        i++, digits++;
      }
    } else if ( s[i] == '0' ) {
      i++, digits++;
    } else {
      return false;
    }
    bool is_float = false;
    if ( i + 1 < s.size() && s[i] == '.' && is_digit( s[i + 1] ) ) {
      is_float = true;
      i += 2;
      while ( i < s.size() && is_digit( s[i] ) ) {
        i++;
      }
    }
    if ( i + 1 < s.size() && ( s[i] == 'e' || s[i] == 'E' ) ) {
      size_t j = i + 1;
      if ( j + 1 < s.size() && ( s[j] == '-' || s[j] == '+' ) ) {
        j++;
      }
      const size_t exponent = j;
      while ( j < s.size() && is_digit( s[j] ) ) {
        j++;
      }
      if ( j > exponent ) {
        is_float = true;
        i = j;
      }
    }
    /* Python's int refuses a string of more than 4300 digits. */
    if ( !is_float && digits > 4300 ) {
      return false;
    }
    std::string literal;
    literal.reserve( i - start );
    for ( size_t k = start; k < i; k++ ) {
      literal += char( s[k] );
    }
    node.kind = Json::Number;
    node.number = strtod( literal.c_str(), nullptr ); /* correctly rounded, infinite past the range */
    at = i;
    return true;
  }
  /* A value that is not a container. */
  bool scalar( Json::Node& node )
  {
    const uint32_t c = s[at];
    if ( c == '"' ) {
      at++;
      node.kind = Json::String;
      return string( node.text );
    }
    if ( c == 'n' && word( "null" ) ) {
      node.kind = Json::Null;
      at += 4;
      return true;
    }
    if ( c == 't' && word( "true" ) ) {
      node.kind = Json::True;
      at += 4;
      return true;
    }
    if ( c == 'f' && word( "false" ) ) {
      node.kind = Json::False;
      at += 5;
      return true;
    }
    if ( c == 'N' && word( "NaN" ) ) {
      node.kind = Json::Number;
      node.number = NAN;
      at += 3;
      return true;
    }
    if ( c == 'I' && word( "Infinity" ) ) {
      node.kind = Json::Number;
      node.number = INFINITY;
      at += 8;
      return true;
    }
    if ( c == '-' && word( "-Infinity" ) ) {
      node.kind = Json::Number;
      node.number = -INFINITY;
      at += 9;
      return true;
    }
    return number( node );
  }

public:
  Scanner( const std::vector<uint32_t>& text, Json& out ) : s( text ), doc( out ) {}

  bool document()
  {
    if ( !s.empty() && s[0] == 0xfeff ) {
      return false; /* "Unexpected UTF-8 BOM" */
    }
    doc.nodes.clear();
    std::vector<size_t> open; /* the containers being read, however deep */
    space();
    bool want_value = true;
    while ( true ) {
      if ( want_value ) {
        if ( at >= s.size() ) {
          return false;
        }
        const size_t index = doc.nodes.size();
        doc.nodes.emplace_back();
        if ( !open.empty() ) {
          doc.nodes[open.back()].items.push_back( index );
        }
        const uint32_t c = s[at];
        if ( c == '{' || c == '[' ) {
          at++;
          doc.nodes[index].kind = c == '{' ? Json::Object : Json::Array;
          open.push_back( index );
          space();
          if ( at < s.size() && s[at] == ( c == '{' ? '}' : ']' ) ) {
            at++;
            open.pop_back();
            want_value = false;
          } else if ( c == '{' ) {
            if ( at >= s.size() || s[at] != '"' ) {
              return false; /* "Expecting property name enclosed in double quotes" */
            }
            at++;
            std::string key;
            if ( !string( key ) ) {
              return false;
            }
            doc.nodes[index].keys.push_back( std::move( key ) );
            space();
            if ( at >= s.size() || s[at] != ':' ) {
              return false;
            }
            at++;
            space();
          }
          continue;
        }
        Json::Node node;
        if ( !scalar( node ) ) {
          return false;
        }
        doc.nodes[index] = std::move( node );
        want_value = false;
        continue;
      }
      /* After a value */
      if ( open.empty() ) {
        space();
        return at == s.size(); /* "Extra data" */
      }
      space();
      if ( at >= s.size() ) {
        return false;
      }
      Json::Node& container = doc.nodes[open.back()];
      const bool object = container.kind == Json::Object;
      if ( s[at] == ( object ? '}' : ']' ) ) {
        at++;
        open.pop_back();
        continue;
      }
      if ( s[at] != ',' ) {
        return false;
      }
      at++;
      space();
      if ( object ) {
        if ( at >= s.size() || s[at] != '"' ) {
          return false;
        }
        at++;
        std::string key;
        if ( !string( key ) ) {
          return false;
        }
        doc.nodes[open.back()].keys.push_back( std::move( key ) );
        space();
        if ( at >= s.size() || s[at] != ':' ) {
          return false;
        }
        at++;
        space();
      }
      want_value = true;
    }
  }
};

}

const Json::Node* Json::get( const Node& object, const std::string& key ) const
{
  if ( object.kind != Object ) {
    return nullptr;
  }
  for ( size_t i = object.keys.size(); i-- > 0; ) {
    if ( object.keys[i] == key ) {
      return &nodes[object.items[i]];
    }
  }
  return nullptr;
}

bool json_loads( const std::string& bytes, Json& out )
{
  std::vector<uint32_t> text;
  if ( !decode_json_bytes( bytes, text ) ) {
    return false;
  }
  Ext::CNumbers c_numbers;
  Scanner scanner( text, out );
  return scanner.document();
}

std::string json_dumps_string( const std::string& text )
{
  static const char* hex = "0123456789abcdef";
  std::string out = "\"";
  const auto unit = [&]( uint32_t u ) {
    out += "\\u";
    for ( int shift = 12; shift >= 0; shift -= 4 ) {
      out += hex[u >> shift & 15];
    }
  };
  for ( uint32_t cp : code_points( text ) ) {
    switch ( cp ) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if ( cp >= ' ' && cp <= '~' ) {
          out += char( cp );
        } else if ( cp >= 0x10000 ) {
          unit( 0xd800 + ( ( cp - 0x10000 ) >> 10 ) );
          unit( 0xdc00 + ( ( cp - 0x10000 ) & 0x3ff ) );
        } else {
          unit( cp );
        }
    }
  }
  out += '"';
  return out;
}

bool base64_strict( const std::string& in, std::string& out )
{
  /* Python 3.14: only the alphabet, padding only at the end and only as
     much as the data lacks. */
  size_t data = 0, pad = 0;
  for ( char c : in ) {
    const bool letter = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '+'
                        || c == '/';
    if ( c == '=' ) {
      pad++;
    } else if ( !letter || pad ) {
      return false;
    } else {
      data++;
    }
  }
  if ( ( pad && !data ) || data % 4 == 1 || ( data % 4 == 0 && pad ) || ( data % 4 == 2 && pad != 2 )
       || ( data % 4 == 3 && pad != 1 ) ) {
    return false;
  }
  out.clear();
  out.reserve( data / 4 * 3 + 2 );
  uint32_t bits = 0;
  unsigned count = 0;
  for ( size_t i = 0; i < data; i++ ) {
    const char c = in[i];
    const uint32_t v = c >= 'A' && c <= 'Z'   ? uint32_t( c - 'A' )
                       : c >= 'a' && c <= 'z' ? uint32_t( c - 'a' + 26 )
                       : c >= '0' && c <= '9' ? uint32_t( c - '0' + 52 )
                       : c == '+'             ? 62U
                                              : 63U;
    bits = bits << 6 | v;
    count += 6;
    if ( count >= 8 ) {
      count -= 8;
      out += char( bits >> count & 0xff );
    }
  }
  return true;
}

namespace {
/* hb-common.cc's feature parser, on the C string Python hands it. */
typedef const char* hb_cursor;

bool hb_space_char( char c )
{
  return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

void hb_space( hb_cursor& p, hb_cursor end )
{
  while ( p < end && hb_space_char( *p ) ) {
    p++;
  }
}

bool hb_char( hb_cursor& p, hb_cursor end, char c )
{
  hb_space( p, end );
  if ( p == end || *p != c ) {
    return false;
  }
  p++;
  return true;
}

/* hb_parse_int: strtol over at most 31 bytes, which fails on overflow. */
bool hb_int( hb_cursor& p, hb_cursor end )
{
  const size_t len = std::min<size_t>( 31, end - p );
  const std::string buf( p, len );
  char* stop = nullptr;
  errno = 0;
  strtol( buf.c_str(), &stop, 10 );
  if ( errno || stop == buf.c_str() ) {
    return false;
  }
  p += stop - buf.c_str();
  return true;
}

bool hb_bool( hb_cursor& p, hb_cursor end )
{
  hb_space( p, end );
  const hb_cursor start = p;
  while ( p < end && ( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ) ) {
    p++;
  }
  const size_t n = p - start;
  const auto lower = []( char c ) { return c >= 'A' && c <= 'Z' ? char( c - 'A' + 'a' ) : c; };
  return ( n == 2 && lower( start[0] ) == 'o' && lower( start[1] ) == 'n' )
         || ( n == 3 && lower( start[0] ) == 'o' && lower( start[1] ) == 'f' && lower( start[2] ) == 'f' );
}

bool hb_tag( hb_cursor& p, hb_cursor end )
{
  hb_space( p, end );
  char quote = 0;
  if ( p < end && ( *p == '\'' || *p == '"' ) ) {
    quote = *p++;
  }
  const hb_cursor start = p;
  while ( p < end && *p != ' ' && *p != '=' && *p != '[' && *p != quote ) {
    p++;
  }
  if ( p == start || p - start > 4 ) {
    return false;
  }
  if ( quote ) {
    if ( p - start != 4 || p == end || *p != quote ) {
      return false;
    }
    p++;
  }
  return true;
}

bool hb_indices( hb_cursor& p, hb_cursor end )
{
  hb_space( p, end );
  if ( !hb_char( p, end, '[' ) ) {
    return true;
  }
  hb_int( p, end );
  if ( hb_char( p, end, ':' ) || hb_char( p, end, ';' ) ) {
    hb_int( p, end );
  }
  return hb_char( p, end, ']' );
}

bool hb_value( hb_cursor& p, hb_cursor end )
{
  const bool had_equal = hb_char( p, end, '=' );
  const bool had_value = hb_int( p, end ) || hb_bool( p, end );
  return !had_equal || had_value;
}
}

bool harfbuzz_feature( const std::string& text )
{
  if ( has_surrogate( text ) ) {
    return false; /* PyUnicode_AsUTF8 cannot encode it */
  }
  hb_cursor p = text.c_str(); /* to the first NUL, as C reads it */
  const hb_cursor end = p + strlen( p );
  if ( !hb_char( p, end, '-' ) ) {
    hb_char( p, end, '+' );
  }
  if ( !hb_tag( p, end ) || !hb_indices( p, end ) || !hb_value( p, end ) ) {
    return false;
  }
  hb_space( p, end );
  return p == end;
}

namespace {
/* The checks of kitty/fonts/vector.py's layout(), resolve() and segments(),
   and of what compile_text and vector_shape make of their results, that hold
   whatever font is chosen. */
class Layout
{
  const Json& doc;
  const Json::Node& obj;
  const FontCheck* fonts;
  std::vector<std::string>& handles;

  /* base | run: the run's keys, then the layout's but runs and text. */
  const Json::Node* style( const Json::Node* run, const std::string& key ) const
  {
    if ( run ) {
      if ( const Json::Node* value = doc.get( *run, key ) ) {
        return value;
      }
    }
    return key == "runs" || key == "text" ? nullptr : doc.get( obj, key );
  }
  static bool number( const Json::Node* value, double fallback, double& out )
  {
    if ( !value ) {
      out = fallback;
      return true;
    }
    out = value->number;
    return value->kind == Json::Number && std::isfinite( value->number );
  }
  static bool is_string( const Json::Node* value ) { return value && value->kind == Json::String; }
  static bool is_bool( const Json::Node* value, bool absent_ok = true )
  {
    return value ? value->kind == Json::True || value->kind == Json::False : absent_ok;
  }
  static bool truthy( const Json::Node* value )
  {
    if ( !value ) {
      return false;
    }
    switch ( value->kind ) {
      case Json::Null:
      case Json::False:
        return false;
      case Json::True:
        return true;
      case Json::Number:
        return value->number != 0; /* NaN is true */
      case Json::String:
        return !value->text.empty();
      default:
        return !value->items.empty();
    }
  }
  /* PyArg's "s": a str that is valid UTF-8 and has no NUL. */
  static bool c_string( const Json::Node* value )
  {
    return is_string( value ) && value->text.find( '\0' ) == std::string::npos && !has_surrogate( value->text );
  }
  static bool paint( const Json::Node* value, const char* fallback )
  {
    if ( !value ) {
      return std::string( fallback ) == "none" || color_paints( fallback );
    }
    return value->kind == Json::String && !has_surrogate( value->text )
           && ( value->text == "none" || color_paints( value->text ) );
  }
  void handle( const Json::Node* font )
  {
    if ( is_string( font ) && !font->text.empty() && font->text[0] == '@' ) {
      const std::string name = font->text.substr( 1 );
      for ( const auto& known : handles ) {
        if ( known == name ) {
          return;
        }
      }
      handles.push_back( name );
    }
  }
  /* resolve() of the run's primary font */
  bool resolve( const Json::Node* run )
  {
    const Json::Node* font = style( run, "font" );
    if ( font && font->kind != Json::String ) {
      return false; /* "font must be a family name or @session-font-id" */
    }
    double weight;
    if ( !number( style( run, "weight" ), 400, weight ) || !( weight >= 100 && weight <= 900 ) ) {
      return false;
    }
    if ( !is_bool( style( run, "italic" ) ) ) {
      return false;
    }
    std::map<std::string, double> axes;
    if ( const Json::Node* given = style( run, "axes" ) ) {
      if ( given->kind != Json::Object ) {
        return false;
      }
      std::map<std::string, size_t> last; /* a dict keeps the last value of a key */
      for ( size_t i = 0; i < given->keys.size(); i++ ) {
        if ( code_points( given->keys[i] ).size() != 4 ) {
          return false; /* "axes must map four-character OpenType tags to numbers" */
        }
        last[given->keys[i]] = given->items[i];
      }
      for ( const auto& item : last ) {
        double value;
        if ( !number( &doc.nodes[item.second], 0, value ) ) {
          return false;
        }
        axes[item.first] = value;
      }
    }
    handle( font );
    if ( !font || font->text.empty() ) {
      /* The configured role is looked up in a dict, so it must be hashable. */
      const Json::Node* role = style( run, "role" );
      if ( role && ( role->kind == Json::Array || role->kind == Json::Object ) ) {
        return false;
      }
    }
    if ( font && !font->text.empty() && font->text[0] == '@' && fonts ) {
      const auto entry = fonts->fonts.find( font->text.substr( 1 ) );
      if ( entry == fonts->fonts.end() ) {
        return false; /* a KeyError: never uploaded, or released */
      }
      if ( entry->second ) {
        /* "Unsupported value for font axis" */
        for ( const auto& axis : axes ) {
          const auto range = entry->second->axes.find( axis.first );
          if ( range == entry->second->axes.end()
               || !( range->second.minimum <= axis.second && axis.second <= range->second.maximum ) ) {
            return false;
          }
        }
      }
    }
    if ( const Json::Node* slant = style( run, "slant" ) ) {
      double value;
      if ( slant->kind != Json::Null && ( !number( slant, 0, value ) || !( value >= -15 && value <= 15 ) ) ) {
        return false;
      }
    }
    return true;
  }

public:
  Layout( const Json& d, const FontCheck* f, std::vector<std::string>& h )
    : doc( d ), obj( d.root() ), fonts( f ), handles( h )
  {}

  bool check()
  {
    if ( obj.kind != Json::Object ) {
      return false; /* "Expected a JSON object" */
    }
    const Json::Node* runs = doc.get( obj, "runs" );
    std::vector<const Json::Node*> list; /* null: the run made of the layout's text */
    if ( runs ) {
      if ( runs->kind != Json::Array ) {
        return false;
      }
      for ( size_t item : runs->items ) {
        const Json::Node& run = doc.nodes[item];
        if ( run.kind != Json::Object || !is_string( doc.get( run, "text" ) ) ) {
          return false; /* "runs must contain objects with a text string" */
        }
        list.push_back( &run );
      }
    } else {
      const Json::Node* text = doc.get( obj, "text" );
      if ( text && text->kind != Json::String ) {
        return false;
      }
      list.push_back( nullptr );
    }
    for ( const Json::Node* run : list ) {
      const Json::Node* text = run ? doc.get( *run, "text" ) : doc.get( obj, "text" );
      if ( !is_bool( style( run, "strike" ) ) || !is_bool( style( run, "overline" ) ) ) {
        return false;
      }
      double size;
      if ( !number( style( run, "size" ), 16, size ) || !( size > 0 ) ) {
        return false;
      }
      const Json::Node* features = style( run, "features" );
      if ( features ) {
        if ( features->kind != Json::Array ) {
          return false;
        }
        for ( size_t item : features->items ) {
          if ( !is_string( &doc.nodes[item] ) ) {
            return false;
          }
        }
      }
      const Json::Node* direction = style( run, "direction" );
      if ( direction
           && ( direction->kind != Json::String
                || ( direction->text != "" && direction->text != "ltr" && direction->text != "rtl"
                     && direction->text != "ttb" && direction->text != "btt" ) ) ) {
        return false;
      }
      /* Every line of the run is laid out in the run's style, with its
         primary font resolved for each. */
      if ( !resolve( run ) ) {
        return false;
      }
      const bool printing = text && text->text.find_first_not_of( '\n' ) != std::string::npos;
      if ( printing ) {
        const Json::Node* fallback = style( run, "fallback" );
        if ( fallback ) {
          if ( fallback->kind != Json::Array ) {
            return false;
          }
          for ( size_t item : fallback->items ) {
            if ( !is_string( &doc.nodes[item] ) ) {
              return false;
            }
            handle( &doc.nodes[item] );
          }
        }
      }
      /* vector_shape(), for each segment: its C strings and features */
      const Json::Node* language = style( run, "language" );
      const Json::Node* script = style( run, "script" );
      if ( ( language && !c_string( language ) ) || ( script && !c_string( script ) ) ) {
        return false;
      }
      if ( features ) {
        for ( size_t item : features->items ) {
          if ( !harfbuzz_feature( doc.nodes[item].text ) ) {
            return false;
          }
        }
      }
      double stroke_width;
      if ( !number( style( run, "stroke_width" ), 1, stroke_width ) || stroke_width < 0 ) {
        return false;
      }
      const Json::Node* underline = style( run, "underline" );
      const std::string u = underline && underline->kind == Json::String ? underline->text : "none";
      if ( underline
           && ( underline->kind != Json::String
                || ( u != "none" && u != "single" && u != "double" && u != "curly" && u != "dotted"
                     && u != "dashed" ) ) ) {
        return false;
      }
      /* The paints of what is certain to be drawn, whatever the font */
      const Json::Node* fill = style( run, "fill" );
      if ( truthy( style( run, "background" ) ) && !paint( style( run, "background" ), "none" ) ) {
        return false;
      }
      if ( !paint( fill, "#000000" ) || !paint( style( run, "stroke" ), "none" ) ) {
        return false;
      }
      const bool decorated = u == "single" || u == "double" || ( style( run, "strike" ) && style( run, "strike" )->kind == Json::True )
                             || ( style( run, "overline" ) && style( run, "overline" )->kind == Json::True );
      if ( decorated ) {
        const Json::Node* color = style( run, "underline_color" );
        if ( color ? !paint( color, "none" ) : !paint( fill, "#000000" ) ) {
          return false;
        }
      }
    }
    double ignored;
    return number( doc.get( obj, "x" ), 0, ignored ) && number( doc.get( obj, "y" ), 0, ignored )
           && number( doc.get( obj, "angle" ), 0, ignored );
  }
};
}

bool text_layout_ok( const std::string& field, const FontCheck* fonts, std::vector<std::string>& handles )
{
  handles.clear();
  std::string raw;
  Json doc;
  if ( !base64_strict( field, raw ) || !json_loads( raw, doc ) ) {
    return false;
  }
  Layout layout( doc, fonts, handles );
  return layout.check();
}

std::string font_release_request( const std::string& name )
{
  return "{\"op\":\"release\",\"id\":" + json_dumps_string( name ) + "}";
}

}
}
