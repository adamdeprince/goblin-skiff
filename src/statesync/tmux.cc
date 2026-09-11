/* Copyright 2026. Distributed under the GNU GPL, version 3 or later. */

#include "tmux.h"

using namespace Terminal;

TmuxControlParser::Output TmuxControlParser::consume( const std::string& bytes )
{
  static const std::string entry = "\033P1000p";
  Output output;
  for ( const char c : bytes ) {
    switch ( state ) {
      case Normal:
        if ( c == '\033' ) {
          pending = c;
          state = Escape;
        } else {
          output.terminal += c;
        }
        break;
      case Escape:
        if ( c == 'P' ) {
          pending += c;
          state = Candidate;
        } else {
          output.terminal += pending;
          pending.clear();
          if ( c == '\033' ) {
            pending = c;
          } else {
            output.terminal += c;
            osc = c == ']';
            state = ( osc || c == '_' || c == '^' || c == 'X' ) ? String : Normal;
          }
        }
        break;
      case Candidate:
        pending += c;
        if ( entry.compare( 0, pending.size(), pending ) != 0 ) {
          output.terminal += pending;
          pending.clear();
          osc = false;
          state = c == '\033' ? StringEscape : ( c == '\030' || c == '\032' ? Normal : String );
        } else if ( pending == entry ) {
          output.control += pending;
          pending.clear();
          state = Control;
        }
        break;
      case String:
      case StringEscape:
        output.terminal += c;
        if ( ( state == StringEscape && c == '\\' ) || ( osc && c == '\007' ) || c == '\030' || c == '\032' ) {
          state = Normal;
        } else {
          state = c == '\033' && state != StringEscape ? StringEscape : String;
        }
        break;
      case Control:
      case ControlEscape:
        output.control += c;
        if ( state == ControlEscape && c == '\\' ) {
          state = Normal;
        } else {
          state = c == '\033' ? ControlEscape : Control;
        }
        break;
    }
  }
  return output;
}
