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

#include "src/terminal/osc52.h"

#include <cstdint>

namespace Terminal {

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char b64_reverse[256] = {
  // clang-format off
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, 0xff, 0x3f,
  0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
  0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  // clang-format on
};

ClipboardEvent::ClipboardEvent()
  : op( ClipboardClear ), targets( ClipboardNone ), payload(), raw_pd(), truncated( false ), decode_ok( false )
{}

bool ClipboardEvent::operator==( const ClipboardEvent& other ) const
{
  return ( op == other.op ) && ( targets == other.targets ) && ( payload == other.payload )
         && ( raw_pd == other.raw_pd ) && ( truncated == other.truncated ) && ( decode_ok == other.decode_ok );
}

std::string base64_encode_osc52( const std::string& raw )
{
  std::string out;
  out.reserve( ( ( raw.size() + 2 ) / 3 ) * 4 );

  size_t i = 0;
  while ( i + 3 <= raw.size() ) {
    const uint32_t n = ( static_cast<uint8_t>( raw[i] ) << 16 ) | ( static_cast<uint8_t>( raw[i + 1] ) << 8 )
                       | static_cast<uint8_t>( raw[i + 2] );
    out.push_back( b64_table[( n >> 18 ) & 0x3f] );
    out.push_back( b64_table[( n >> 12 ) & 0x3f] );
    out.push_back( b64_table[( n >> 6 ) & 0x3f] );
    out.push_back( b64_table[n & 0x3f] );
    i += 3;
  }

  if ( i + 1 == raw.size() ) {
    const uint32_t n = static_cast<uint8_t>( raw[i] ) << 16;
    out.push_back( b64_table[( n >> 18 ) & 0x3f] );
    out.push_back( b64_table[( n >> 12 ) & 0x3f] );
    out.push_back( '=' );
    out.push_back( '=' );
  } else if ( i + 2 == raw.size() ) {
    const uint32_t n = ( static_cast<uint8_t>( raw[i] ) << 16 ) | ( static_cast<uint8_t>( raw[i + 1] ) << 8 );
    out.push_back( b64_table[( n >> 18 ) & 0x3f] );
    out.push_back( b64_table[( n >> 12 ) & 0x3f] );
    out.push_back( b64_table[( n >> 6 ) & 0x3f] );
    out.push_back( '=' );
  }

  return out;
}

bool base64_decode_osc52( const std::string& b64, std::string& raw )
{
  raw.clear();
  raw.reserve( ( b64.size() / 4 ) * 3 );

  int accum_bits = 0;
  uint32_t accum = 0;
  bool saw_pad = false;

  for ( size_t i = 0; i < b64.size(); i++ ) {
    const unsigned char ch = static_cast<unsigned char>( b64[i] );
    if ( ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ) {
      continue;
    }
    if ( ch == '=' ) {
      saw_pad = true;
      continue;
    }
    if ( saw_pad ) {
      return false;
    }
    const unsigned char six = b64_reverse[ch];
    if ( six > 0x3f ) {
      return false;
    }
    accum = ( accum << 6 ) | six;
    accum_bits += 6;
    if ( accum_bits >= 8 ) {
      accum_bits -= 8;
      raw.push_back( static_cast<char>( ( accum >> accum_bits ) & 0xff ) );
    }
  }

  /* leftover 1 leftover 6-bit group is not a valid encoding */
  return accum_bits != 6;
}

static unsigned parse_pc( const std::string& pc )
{
  unsigned targets = ClipboardNone;
  for ( size_t i = 0; i < pc.size(); i++ ) {
    const char ch = pc[i];
    if ( ch == 'c' ) {
      targets |= ClipboardClipboard;
    } else if ( ch == 'p' ) {
      targets |= ClipboardPrimary;
    } else if ( ch == 'q' ) {
      targets |= ClipboardSecondary;
    } else if ( ch == 's' ) {
      targets |= ClipboardSelect;
    } else if ( ch >= '0' && ch <= '7' ) {
      targets |= clipboard_cut_buffer( ch - '0' );
    }
  }
  return targets;
}

std::string osc52_targets_to_pc( unsigned targets )
{
  if ( targets == ClipboardNone ) {
    return std::string( "c" );
  }

  std::string pc;
  if ( targets & ClipboardClipboard ) {
    pc.push_back( 'c' );
  }
  if ( targets & ClipboardPrimary ) {
    pc.push_back( 'p' );
  }
  if ( targets & ClipboardSecondary ) {
    pc.push_back( 'q' );
  }
  if ( targets & ClipboardSelect ) {
    pc.push_back( 's' );
  }
  for ( int i = 0; i < 8; i++ ) {
    if ( targets & clipboard_cut_buffer( i ) ) {
      pc.push_back( static_cast<char>( '0' + i ) );
    }
  }
  return pc.empty() ? std::string( "c" ) : pc;
}

bool parse_osc52_string( const std::string& osc, ClipboardEvent& ev, bool truncated )
{
  ev = ClipboardEvent();
  ev.truncated = truncated;

  if ( osc.size() < 3 || osc[0] != '5' || osc[1] != '2' || osc[2] != ';' ) {
    return false;
  }

  const std::string rest = osc.substr( 3 );
  const size_t semi = rest.find( ';' );
  if ( semi == std::string::npos ) {
    return false;
  }

  const std::string pc = rest.substr( 0, semi );
  const std::string pd = rest.substr( semi + 1 );
  unsigned targets = parse_pc( pc );
  if ( pc.empty() ) {
    /* Empty Pc is what tmux emits. Treat it as CLIPBOARD so copy works
       without a terminal-overrides hack. xterm itself defaults to s0. */
    targets = ClipboardClipboard;
  } else if ( targets == ClipboardNone ) {
    return false;
  }

  ev.targets = targets;
  ev.raw_pd = pd;

  if ( pd == "?" ) {
    ev.op = ClipboardQuery;
    return true;
  }

  if ( pd.empty() ) {
    ev.op = ClipboardClear;
    return true;
  }

  if ( base64_decode_osc52( pd, ev.payload ) ) {
    ev.op = ClipboardSet;
    ev.decode_ok = true;
    return true;
  }

  /* xterm: Pd that is neither "?" nor valid base64 clears the selection. */
  ev.op = ClipboardClear;
  ev.payload.clear();
  ev.decode_ok = false;
  return true;
}

bool parse_osc52_string( const std::vector<wchar_t>& osc, ClipboardEvent& ev, bool truncated )
{
  std::string narrow;
  narrow.reserve( osc.size() );
  for ( size_t i = 0; i < osc.size(); i++ ) {
    if ( osc[i] < 0x20 || osc[i] > 0x7e ) {
      return false;
    }
    narrow.push_back( static_cast<char>( osc[i] ) );
  }
  return parse_osc52_string( narrow, ev, truncated );
}

std::string encode_osc52( const ClipboardEvent& ev, bool use_st )
{
  std::string out( "\033]" );
  out.append( "52;" );
  out.append( osc52_targets_to_pc( ev.targets ) );
  out.push_back( ';' );

  if ( ev.op == ClipboardQuery ) {
    out.push_back( '?' );
  } else if ( ev.op == ClipboardSet ) {
    if ( !ev.raw_pd.empty() && ev.decode_ok ) {
      out.append( ev.raw_pd );
    } else {
      out.append( base64_encode_osc52( ev.payload ) );
    }
  }

  if ( use_st ) {
    out.append( "\033\\" );
  } else {
    out.push_back( '\007' );
  }
  return out;
}

static bool is_osc52_prefix_so_far( const std::string& body )
{
  static const char want[] = "52;";
  const size_t n = body.size() < 3 ? body.size() : 3;
  return body.compare( 0, n, want, n ) == 0;
}

std::string Osc52InputFilter::osc_body( void ) const
{
  if ( !buf_.empty() && static_cast<unsigned char>( buf_[0] ) == 0x9d ) {
    return buf_.substr( 1 );
  }
  if ( buf_.size() >= 2 && buf_[0] == '\033' && buf_[1] == ']' ) {
    return buf_.substr( 2 );
  }
  return buf_;
}

void Osc52InputFilter::finish_sequence( Output& out )
{
  ClipboardEvent ev;
  if ( parse_osc52_string( osc_body(), ev, overflow_ ) ) {
    out.events.push_back( ev );
  } else {
    out.user_bytes.append( buf_ );
  }
  buf_.clear();
  overflow_ = false;
  state_ = Ground;
}

void Osc52InputFilter::abort_sequence( char ch, Output& out )
{
  buf_.push_back( ch );
  out.user_bytes.append( buf_ );
  buf_.clear();
  overflow_ = false;
  state_ = Ground;
}

void Osc52InputFilter::consume_one( char ch, Output& out )
{
  const unsigned char uch = static_cast<unsigned char>( ch );

  switch ( state_ ) {
    case Ground:
      if ( ch == '\033' ) {
        buf_.assign( 1, ch );
        state_ = Esc;
      } else if ( uch == 0x9d ) {
        buf_.assign( 1, ch );
        state_ = OscPrefix;
      } else {
        out.user_bytes.push_back( ch );
      }
      break;

    case Esc:
      if ( ch == ']' ) {
        buf_.push_back( ch );
        state_ = OscPrefix;
      } else {
        abort_sequence( ch, out );
      }
      break;

    case OscPrefix: {
      buf_.push_back( ch );
      const std::string body = osc_body();
      if ( !is_osc52_prefix_so_far( body ) ) {
        out.user_bytes.append( buf_ );
        buf_.clear();
        overflow_ = false;
        state_ = Ground;
      } else if ( body.size() >= 3 ) {
        state_ = OscBody;
      }
      break;
    }

    case OscBody:
      if ( ch == '\007' || uch == 0x9c ) {
        finish_sequence( out );
      } else if ( ch == '\033' ) {
        state_ = OscMaybeST;
      } else if ( uch >= 0x20 && uch <= 0x7e ) {
        if ( buf_.size() < OSC52_MAX_OSC_CHARS + 8 ) {
          buf_.push_back( ch );
        } else {
          overflow_ = true;
        }
      } else {
        abort_sequence( ch, out );
      }
      break;

    case OscMaybeST:
      if ( ch == '\\' ) {
        finish_sequence( out );
      } else {
        buf_.push_back( '\033' );
        abort_sequence( ch, out );
      }
      break;
  }
}

Osc52InputFilter::Output Osc52InputFilter::consume( const char* data, size_t len )
{
  Output out;
  for ( size_t i = 0; i < len; i++ ) {
    consume_one( data[i], out );
  }
  return out;
}

Osc52InputFilter::Output Osc52InputFilter::flush( void )
{
  Output out;
  if ( !buf_.empty() ) {
    out.user_bytes.append( buf_ );
    buf_.clear();
  }
  overflow_ = false;
  state_ = Ground;
  return out;
}

}
