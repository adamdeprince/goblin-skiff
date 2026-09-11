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

#ifndef TERMINALDISPLAY_HPP
#define TERMINALDISPLAY_HPP

#include "src/terminal/terminalframebuffer.h"
#include "src/terminal/terminalgeometry.h"

namespace Terminal {
/* variables used within a new_frame */
class FrameState
{
public:
  std::string str;

  int cursor_x, cursor_y;
  const int row_offset;
  Renditions current_rendition;
  Hyperlink current_hyperlink;
  bool cursor_visible;

  const Framebuffer& last_frame;

  FrameState( const Framebuffer& s_last, int s_row_offset = 0 );

  void append( char c ) { str.append( 1, c ); }
  void append( size_t s, char c ) { str.append( s, c ); }
  void append( wchar_t wc ) { Cell::append_to_str( str, wc ); }
  void append( const char* s ) { str.append( s ); }
  void append_string( const std::string& append ) { str.append( append ); }

  void append_cell( const Cell& cell ) { cell.print_grapheme( str ); }
  void append_silent_move( int y, int x );
  void append_move( int y, int x );
  void update_rendition( const Renditions& r, bool force = false );
  void update_hyperlink( const Hyperlink& h, bool force = false );
};

/* Client-local startup layout. The wire framebuffer always keeps its real
   dimensions; only the initial, unused rows of the physical screen are held
   for the user's existing output. Once consumed, they are never reserved again. */
class StartupScreen
{
  friend class Display;
  int row_offset;
  int cursor_row;

public:
  StartupScreen() : row_offset( -1 ), cursor_row( -1 ) {}
  void set_cursor_row( int row ) { if ( row_offset < 0 ) { cursor_row = row; } }
};

class Display
{
private:
  bool has_ech; /* erase character is part of vt200 but not supported by tmux
                   (or by "screen" terminfo entry, which is what tmux advertises) */

  bool has_bce; /* erases result in cell filled with background color */

  bool has_title; /* supports window title and icon name */

  bool render_kitty; /* true only for bytes sent to the user's terminal */
  bool render_sixel = false;
  bool convert_sixel = false;
  bool render_keyboard = true; // state-delta encoder; local client sets from its probe
  bool render_clipboard = true;
  bool render_sized_text = true;
  ClientGeometry graphics_geometry {};

  const char *smcup, *rmcup; /* enter and exit alternate screen mode */

  bool put_row( bool initialized,
                FrameState& frame,
                const Framebuffer& f,
                int frame_y,
                const Row& old_row,
                bool wrap ) const;

  bool can_use_erase( const FrameState& frame ) const;

public:
  std::string open() const;
  std::string close() const;

  std::string new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f,
                         StartupScreen* startup = NULL ) const;

  bool uses_alternate_screen() const { return smcup != NULL; }
  Display( bool use_environment, bool kitty_supported = true );
  void set_graphics( const ClientGraphics& caps, bool sixel_state )
  {
    render_kitty = caps.kitty;
    render_sixel = sixel_state && caps.sixel;
    convert_sixel = sixel_state && !caps.sixel && caps.kitty;
    render_keyboard = sixel_state && caps.keyboard;
    render_sized_text = sixel_state && caps.text_sizing >= 2;
    render_clipboard = caps.clipboard;
  }
  void set_graphics_geometry( const ClientGeometry& geometry ) { graphics_geometry = geometry; }
};
}

#endif
