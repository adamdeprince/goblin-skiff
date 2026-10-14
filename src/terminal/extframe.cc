/*
    Goblin Skiff: OSC 777 extension framing
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/extframe.h"

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <locale.h>
#if defined( __APPLE__ )
#include <xlocale.h>
#endif

namespace Terminal {
namespace Ext {

const std::string* Fields::find( const std::string& key ) const
{
  for ( const auto& item : items ) {
    if ( item.first == key ) {
      return &item.second;
    }
  }
  return nullptr;
}

void Fields::set( const std::string& key, const std::string& value )
{
  for ( auto& item : items ) {
    if ( item.first == key ) {
      item.second = value;
      return;
    }
  }
  items.emplace_back( key, value );
}

void Fields::set( const std::string& key, int64_t value )
{
  set( key, std::to_string( value ) );
}

void Fields::erase( const std::string& key )
{
  for ( auto it = items.begin(); it != items.end(); ++it ) {
    if ( it->first == key ) {
      items.erase( it );
      return;
    }
  }
}

std::string Fields::text() const
{
  std::string out;
  for ( const auto& item : items ) {
    if ( !out.empty() ) {
      out += ':';
    }
    out += item.first;
    out += '=';
    out += item.second;
  }
  return out;
}

bool parse_fields( const std::string& text, Fields& out )
{
  out.items.clear();
  if ( text.empty() ) {
    return true;
  }
  size_t start = 0;
  while ( true ) {
    const size_t end = std::min( text.find( ':', start ), text.size() );
    const std::string item = text.substr( start, end - start );
    const size_t eq = item.find( '=' );
    if ( eq == std::string::npos || eq == 0 ) {
      return false;
    }
    const std::string key = item.substr( 0, eq );
    if ( out.has( key ) ) {
      return false;
    }
    out.items.emplace_back( key, item.substr( eq + 1 ) );
    if ( end == text.size() ) {
      return true;
    }
    start = end + 1;
    if ( start == text.size() ) {
      return false; /* a trailing separator */
    }
  }
}

bool parse( const std::string& payload, Frame& out )
{
  if ( payload.compare( 0, 4, "777;" ) != 0 ) {
    return false;
  }
  for ( char c : payload ) {
    if ( c < 0x20 || c > 0x7e ) {
      return false;
    }
  }
  const size_t semi = payload.find( ';', 4 );
  out.selector = payload.substr( 4, semi == std::string::npos ? std::string::npos : semi - 4 );
  if ( out.selector.empty() ) {
    return false;
  }
  return parse_fields( semi == std::string::npos ? std::string() : payload.substr( semi + 1 ), out.fields );
}

bool parse_program( const std::string& payload, Frame& out )
{
  if ( !parse( payload, out ) ) {
    return false;
  }
  const size_t semi = payload.find( ';', 4 );
  if ( semi == std::string::npos || payload.size() - semi - 1 > PROGRAM_FIELDS_LIMIT ) {
    return false;
  }
  int64_t version = 0;
  return integer( out.fields, "v", 0, INT32_MAX, version ) == Value::Valid && out.fields.has( "op" );
}

bool parse_integer( const std::string& text, int64_t lo, int64_t hi, int64_t& out )
{
  /* As Silkie reads one: digits after an optional minus, no plus, and the
     range decides which values are allowed. */
  const bool negative = !text.empty() && text[0] == '-';
  size_t at = negative ? 1 : 0;
  if ( at == text.size() ) {
    return false;
  }
  int64_t magnitude = 0;
  for ( ; at < text.size(); at++ ) {
    if ( text[at] < '0' || text[at] > '9' ) {
      return false;
    }
    magnitude = magnitude * 10 + ( text[at] - '0' );
    if ( magnitude > 1000000000000000LL ) {
      return false;
    }
  }
  const int64_t value = negative ? -magnitude : magnitude;
  if ( value < lo || value > hi ) {
    return false;
  }
  out = value;
  return true;
}

Value integer( const Fields& fields, const std::string& key, int64_t lo, int64_t hi, int64_t& out )
{
  const std::string* text = fields.find( key );
  if ( !text ) {
    return Value::Absent;
  }
  return parse_integer( *text, lo, hi, out ) ? Value::Valid : Value::Invalid;
}

std::string encode( const std::string& selector, const Fields& fields )
{
  return "\033]777;" + selector + ";" + fields.text() + "\033\\";
}

CNumbers::CNumbers()
{
  static const locale_t c_locale = newlocale( LC_ALL_MASK, "C", static_cast<locale_t>( 0 ) );
  previous = c_locale ? uselocale( c_locale ) : static_cast<locale_t>( 0 );
}

CNumbers::~CNumbers()
{
  if ( previous ) {
    uselocale( static_cast<locale_t>( previous ) );
  }
}

std::string number( double value )
{
  CNumbers c_numbers;
  char buf[64];
  for ( int digits = 1; digits <= 17; digits++ ) {
    snprintf( buf, sizeof buf, "%.*g", digits, value );
    if ( strtod( buf, nullptr ) == value ) {
      break;
    }
  }
  return buf;
}

static const char* const FRAME_STARTS[] = { "\033]777;", "\033]22;" };

static bool frame_prefix( const std::string& s, bool& complete )
{
  bool prefix = false;
  complete = false;
  for ( const char* start : FRAME_STARTS ) {
    const std::string text( start );
    if ( s.size() <= text.size() && text.compare( 0, s.size(), s ) == 0 ) {
      prefix = true;
      complete = complete || s.size() == text.size();
    }
  }
  return prefix;
}

static void append_plain( InputDemux::Output& out, const std::string& text )
{
  if ( text.empty() ) {
    return;
  }
  if ( !out.items.empty() && !out.items.back().first ) {
    out.items.back().second += text;
  } else {
    out.items.emplace_back( false, text );
  }
}

void InputDemux::feed( const std::string& bytes, Output& out )
{
  std::string input = bytes;
  for ( size_t i = 0; i < input.size(); i++ ) {
    const char c = input[i];
    if ( !matched_ ) {
      /* Bracketed paste is literal data, even when it holds something that
         looks like a report. Its delimiters may span reads. */
      paste_marker_ += c;
      if ( paste_marker_.size() > 6 ) {
        paste_marker_.erase( 0, 1 );
      }
      const bool was_paste = paste_;
      if ( paste_marker_ == "\033[200~" ) {
        paste_ = true;
      } else if ( paste_marker_ == "\033[201~" ) {
        paste_ = false;
      }
      if ( was_paste || paste_ ) {
        append_plain( out, pending_ + std::string( 1, c ) );
        pending_.clear();
        continue;
      }
      if ( pending_.empty() && c != '\033' ) {
        append_plain( out, std::string( 1, c ) );
        continue;
      }
      pending_ += c;
      bool complete = false;
      if ( frame_prefix( pending_, complete ) ) {
        matched_ = complete;
        continue;
      }
      /* Keep a second ESC as the possible start of the next frame. */
      if ( c == '\033' && pending_.size() > 1 ) {
        append_plain( out, pending_.substr( 0, pending_.size() - 1 ) );
        pending_ = "\033";
      } else {
        append_plain( out, pending_ );
        pending_.clear();
      }
      continue;
    }
    pending_ += c;
    const bool end = c == '\a' || ( escape_ && c == '\\' );
    if ( end ) {
      out.items.emplace_back( true, pending_ );
      pending_.clear();
      matched_ = escape_ = false;
      continue;
    }
    if ( c == '\030' || c == '\032' ) {
      /* CAN and SUB cancel the control string. */
      pending_.clear();
      matched_ = escape_ = false;
      continue;
    }
    if ( escape_ ) {
      /* ESC that does not form ST ends the string and begins a new sequence. */
      pending_.clear();
      matched_ = escape_ = false;
      input.insert( i + 1, std::string( "\033" ) + c );
      continue;
    }
    escape_ = c == '\033';
  }
}

void InputDemux::expire( Output& out )
{
  if ( !matched_ && !pending_.empty() ) {
    append_plain( out, pending_ );
    pending_.clear();
  }
}

std::string frame_payload( const std::string& frame )
{
  if ( frame.size() < 3 || frame.compare( 0, 2, "\033]" ) != 0 ) {
    return {};
  }
  size_t end = frame.size();
  if ( frame.back() == '\a' ) {
    end -= 1;
  } else if ( frame.size() >= 4 && frame.compare( frame.size() - 2, 2, "\033\\" ) == 0 ) {
    end -= 2;
  }
  return frame.substr( 2, end - 2 );
}

}
}
