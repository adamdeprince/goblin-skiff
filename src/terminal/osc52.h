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

#ifndef OSC52_HPP
#define OSC52_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace Terminal {

enum ClipboardOp
{
  ClipboardSet = 0,
  ClipboardQuery = 1,
  ClipboardClear = 2
};

enum ClipboardTarget
{
  ClipboardNone = 0,
  ClipboardClipboard = 1 << 0, /* c */
  ClipboardPrimary = 1 << 1,   /* p */
  ClipboardSecondary = 1 << 2, /* q */
  ClipboardSelect = 1 << 3,    /* s */
  ClipboardCut0 = 1 << 4,
  ClipboardCut1 = 1 << 5,
  ClipboardCut2 = 1 << 6,
  ClipboardCut3 = 1 << 7,
  ClipboardCut4 = 1 << 8,
  ClipboardCut5 = 1 << 9,
  ClipboardCut6 = 1 << 10,
  ClipboardCut7 = 1 << 11
};

inline unsigned clipboard_cut_buffer( int n )
{
  if ( n < 0 || n > 7 ) {
    return ClipboardNone;
  }
  return 1u << ( 4 + n );
}

/* Host OSC 52 collector cap. Payloads that fit are sent once on the
   reliable instruction channel; they are not stored in framebuffer state. */
const size_t OSC52_MAX_OSC_CHARS = 256 * 1024;

struct ClipboardEvent
{
  ClipboardOp op;
  unsigned targets;
  std::string payload; /* decoded SET bytes; empty for query/clear */
  std::string raw_pd;  /* original OSC Pd, still base64 for SET */
  bool truncated;
  bool decode_ok;

  ClipboardEvent();

  bool has_target( unsigned bit ) const { return ( targets & bit ) != 0; }
  bool includes_clipboard() const { return has_target( ClipboardClipboard ); }

  bool operator==( const ClipboardEvent& other ) const;
};

/* osc is the OSC body, e.g. "52;c;aGVsbG8=". Returns false if this is not OSC 52. */
bool parse_osc52_string( const std::string& osc, ClipboardEvent& ev, bool truncated = false );
bool parse_osc52_string( const std::vector<wchar_t>& osc, ClipboardEvent& ev, bool truncated = false );

std::string osc52_targets_to_pc( unsigned targets );
std::string encode_osc52( const ClipboardEvent& ev, bool use_st = false );

std::string base64_encode_osc52( const std::string& raw );
bool base64_decode_osc52( const std::string& b64, std::string& raw );

/* Pull OSC 52 replies out of a local-terminal stdin stream without treating
   the reply as keystrokes. Incomplete prefixes stay buffered. */
class Osc52InputFilter
{
public:
  struct Output
  {
    std::string user_bytes;
    std::vector<ClipboardEvent> events;
  };

  Osc52InputFilter() : state_( Ground ), buf_(), overflow_( false ) {}

  Output consume( const char* data, size_t len );
  Output flush( void );
  bool in_sequence( void ) const { return state_ != Ground; }

private:
  enum State
  {
    Ground,
    Esc,
    OscPrefix,
    OscBody,
    OscMaybeST
  };

  State state_;
  std::string buf_;
  bool overflow_;

  void consume_one( char ch, Output& out );
  void finish_sequence( Output& out );
  void abort_sequence( char ch, Output& out );
  std::string osc_body( void ) const;
};

}

#endif
