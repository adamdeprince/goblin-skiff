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
#include <cstdio>

#include "src/terminal/kittygraphics.h"
#include "src/terminal/sixel.h"
#include "src/terminal/terminalframebuffer.h"
#include "terminaldisplay.h"

using namespace Terminal;

/* Print a new "frame" to the terminal, using ANSI/ECMA-48 escape codes. */

static const Renditions& initial_rendition( void )
{
  const static Renditions blank = Renditions( 0 );
  return blank;
}

std::string Display::open() const
{
  return std::string( smcup ? smcup : "" ) + std::string( "\033[?1h" ) + ( render_keyboard ? "\033[>0u" : "" );
}

std::string Display::close() const
{
  return std::string( "\033[?1l\033[0m\033[?25h"
                      "\033[?1003l\033[?1002l\033[?1001l\033[?1000l"
                      "\033[?1015l\033[?1006l\033[?1005l" )
         + ( render_keyboard ? "\033[<u" : "" ) + ( render_clipboard ? "\033[?5522l" : "" )
         + std::string( rmcup ? rmcup : "" );
}

/* A plain shell initially uses only the top of its logical framebuffer.
   Keep the local prefix above it until output needs those physical rows.
   Graphics and mouse coordinates must stay in the unshifted coordinate space. */
static int startup_unused_rows( const Framebuffer& f )
{
  if ( !f.get_kitty_placements().empty() || f.ds.mouse_reporting_mode != DrawState::MOUSE_REPORTING_NONE
       || f.ds.get_scrolling_region_top_row() != 0
       || f.ds.get_scrolling_region_bottom_row() != f.ds.get_height() - 1 ) {
    return 0;
  }
  const Cell blank( 0 );
  int used = f.ds.get_cursor_row() + 1;
  for ( int row = f.ds.get_height() - 1; row >= used; row-- ) {
    for ( const Cell& cell : f.get_row( row )->cells ) {
      if ( cell != blank ) {
        return f.ds.get_height() - row - 1;
      }
    }
  }
  return f.ds.get_height() - used;
}

std::string Display::new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f,
                                StartupScreen* startup ) const
{
  const bool resized = f.ds.get_width() != last.ds.get_width() || f.ds.get_height() != last.ds.get_height();
  const bool attaching = startup && startup->row_offset < 0;
  int row_offset = 0, scroll_rows = 0;
  if ( startup ) {
    if ( attaching ) {
      // The cursor is already on the first free line below the splash, not
      // necessarily at the bottom of the window. Consume existing blank space
      // before scrolling anything into history. Unknown positions keep the
      // conservative bottom-anchored fallback used by terminals without CPR.
      const int origin = startup->cursor_row >= 0 && !resized
                           ? std::min( startup->cursor_row, f.ds.get_height() - 1 ) : f.ds.get_height();
      row_offset = std::min( origin, startup_unused_rows( f ) );
      scroll_rows = origin - row_offset;
    } else {
      row_offset = initialized && !resized && startup->row_offset > 0
                     ? std::min( startup->row_offset, startup_unused_rows( f ) ) : 0;
      scroll_rows = startup->row_offset - row_offset;
      /* A resize may already have moved rows in the physical terminal. Its
         old coordinates are unknown: preserve the viewport before repainting. */
      if ( resized && startup->row_offset > 0 ) {
        scroll_rows = std::max( f.ds.get_height(), last.ds.get_height() );
      }
    }
    startup->row_offset = row_offset;
  }
  FrameState frame( last, row_offset );
  bool have_sized_text = false, redraw_sized_text = !initialized || resized;
  for ( int y = 0; y < f.ds.get_height(); y++ ) {
    for ( int x = 0; x < f.ds.get_width(); x++ ) {
      const Cell& cell = *f.get_cell( y, x );
      const Cell* old = y < last.ds.get_height() && x < last.ds.get_width() ? last.get_cell( y, x ) : NULL;
      if ( cell.get_sized_text() || ( old && old->get_sized_text() ) ) {
        have_sized_text = true;
        if ( !old || cell != *old ) { redraw_sized_text = true; }
      }
    }
  }
  const bool redraw_sixel = render_sixel && ( !initialized || Sixel::changed( last, f ) );
  redraw_sized_text = have_sized_text && ( redraw_sized_text || redraw_sixel );
  std::vector<bool> graphics_dirty_rows( f.ds.get_height(), false );
  if ( redraw_sixel ) {
    // Native sixel has no delete-by-ID. Erase old cell coverage and repaint
    // those text rows before composing current images, without clearing the
    // viewport or scrollback. This also handles popup hide/restore.
    frame.update_rendition( initial_rendition(), true );
    frame.update_hyperlink( Hyperlink(), true );
    for ( const auto& p : last.get_kitty_placements() ) {
      const auto* image = last.find_kitty_image( p.image_id );
      if ( !image || image->origin != ImageOrigin::Sixel || p.col < 0 || p.col >= f.ds.get_width() ) { continue; }
      const unsigned width = std::min( p.columns, unsigned( f.ds.get_width() - p.col ) );
      const int bottom = std::min( int64_t( f.ds.get_height() ), int64_t( p.row ) + p.rows );
      for ( int y = std::max( p.row, 0 ); width && y < bottom; y++ ) {
        frame.append_silent_move( y, p.col );
        frame.str += "\033[" + std::to_string( width ) + "X";
        graphics_dirty_rows[y] = true;
      }
    }
  }

  char tmp[64];

  /* has bell been rung? */
  if ( f.get_bell_count() != frame.last_frame.get_bell_count() ) {
    frame.append( '\007' );
  }
  using title_type = Terminal::Framebuffer::title_type;

  /* has icon name or window title changed? */
  if ( has_title && f.is_title_initialized()
       && ( ( !initialized ) || ( f.get_icon_name() != frame.last_frame.get_icon_name() )
            || ( f.get_window_title() != frame.last_frame.get_window_title() ) ) ) {
    /* set icon name and window title */
    if ( f.get_icon_name() == f.get_window_title() ) {
      /* write combined Icon Name and Window Title */
      frame.append( "\033]0;" );
      const title_type& window_title( f.get_window_title() );
      for ( title_type::const_iterator i = window_title.begin(); i != window_title.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );
      /* ST is more correct, but BEL more widely supported */
    } else {
      /* write Icon Name */
      frame.append( "\033]1;" );
      const title_type& icon_name( f.get_icon_name() );
      for ( title_type::const_iterator i = icon_name.begin(); i != icon_name.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );

      frame.append( "\033]2;" );
      const title_type& window_title( f.get_window_title() );
      for ( title_type::const_iterator i = window_title.begin(); i != window_title.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );
    }
  }

  /* has reverse video state changed? */
  if ( ( !initialized ) || ( f.ds.reverse_video != frame.last_frame.ds.reverse_video ) ) {
    /* set reverse video */
    snprintf( tmp, 64, "\033[?5%c", ( f.ds.reverse_video ? 'h' : 'l' ) );
    frame.append( tmp );
  }

  /* has size changed? */
  if ( ( !initialized ) || ( f.ds.get_width() != frame.last_frame.ds.get_width() )
       || ( f.ds.get_height() != frame.last_frame.ds.get_height() ) ) {
    /* reset scrolling region */
    frame.append( "\033[r" );

    if ( attaching || scroll_rows ) {
      /* Allocate only the rows occupied by remote output. Scrolling a whole
         viewport here would immediately hide the inline mascot. */
      frame.append( "\033[0m\033]8;;\033\\" );
      if ( scroll_rows ) {
        snprintf( tmp, sizeof tmp, "\033[%d;1H\r", std::max( f.ds.get_height(), last.ds.get_height() ) );
        frame.append( tmp );
        frame.append( scroll_rows, '\n' );
      }
      snprintf( tmp, sizeof tmp, "\033[%d;1H", row_offset + 1 );
      frame.append( tmp );
    } else {
      frame.append( "\033[0m\033[H\033[2J" );
    }
    initialized = false;
    frame.cursor_x = frame.cursor_y = 0;
    frame.current_rendition = initial_rendition();
    frame.current_hyperlink = Hyperlink();
  } else {
    frame.cursor_x = frame.last_frame.ds.get_cursor_col();
    frame.cursor_y = frame.last_frame.ds.get_cursor_row();
    frame.current_rendition = frame.last_frame.ds.get_renditions();
    frame.current_hyperlink = frame.last_frame.ds.get_hyperlink();
    if ( scroll_rows ) {
      /* Grow the remote area with real full-screen scrolling, preserving the
         prefix in native history. The already-rendered remote rows move with
         it, so their logical coordinates and normal diff remain unchanged. */
      frame.append( "\033[0m\033]8;;\033\\\033[r" );
      snprintf( tmp, sizeof tmp, "\033[%d;1H\r", f.ds.get_height() );
      frame.append( tmp );
      frame.append( scroll_rows, '\n' );
      frame.cursor_x = frame.cursor_y = -1;
      frame.current_rendition = initial_rendition();
      frame.current_hyperlink = Hyperlink();
    }
  }

  if ( redraw_sized_text ) {
    // Erase old blocks first, including their lower rows in a plain-text
    // fallback. Draw current blocks after ordinary text so a later EL/ECH
    // cannot erase a multi-row glyph we just emitted.
    for ( const auto* screen : { &last, &f } ) {
      for ( int y = 0; y < std::min( screen->ds.get_height(), f.ds.get_height() ); y++ ) {
        for ( int x = 0; x < std::min( screen->ds.get_width(), f.ds.get_width() ); x++ ) {
          const auto* cell = screen->get_cell( y, x );
          if ( !cell->get_sized_text() || cell->get_text_x() || cell->get_text_y() ) { continue; }
          const auto& text = *cell->get_sized_text();
          frame.update_rendition( initial_rendition(), true );
          frame.update_hyperlink( Hyperlink(), true );
          for ( int row = y; row < std::min( f.ds.get_height(), y + int( text.scale ) ); row++ ) {
            frame.append_silent_move( row, x );
            frame.str += "\033[" + std::to_string( std::min( int( text.columns() ), f.ds.get_width() - x ) ) + "X";
            graphics_dirty_rows[row] = true;
          }
        }
      }
    }
  }

  /* is cursor visibility initialized? */
  if ( !initialized ) {
    frame.cursor_visible = false;
    frame.append( "\033[?25l" );
  }

  int frame_y = 0;
  Framebuffer::row_pointer blank_row;
  Framebuffer::rows_type rows( frame.last_frame.get_rows() );
  /* Extend rows if we've gotten a resize and new is wider than old */
  if ( frame.last_frame.ds.get_width() < f.ds.get_width() ) {
    for ( Framebuffer::rows_type::iterator p = rows.begin(); p != rows.end(); p++ ) {
      *p = std::make_shared<Row>( **p );
      ( *p )->cells.resize( f.ds.get_width(), Cell( f.ds.get_background_rendition() ) );
    }
  }
  /* Add rows if we've gotten a resize and new is taller than old */
  if ( static_cast<int>( rows.size() ) < f.ds.get_height() ) {
    // get a proper blank row
    const size_t w = f.ds.get_width();
    const color_type c = 0;
    blank_row = std::make_shared<Row>( w, c );
    rows.resize( f.ds.get_height(), blank_row );
  }

  /* shortcut -- has display moved up by a certain number of lines? */
  if ( initialized && !row_offset && !redraw_sixel && !have_sized_text ) {
    int lines_scrolled = 0;
    int scroll_height = 0;

    /* A shared Row pointer is proof that an unchanged logical row moved.
       Content equality alone is not: untouched blank rows deliberately share
       storage and can otherwise look like a scroll in sparse applications.
       Only use the shortcut when the new top row occurs once in each frame. */
    const Row* new_top_row = f.get_row( 0 );
    bool new_top_row_is_unique = true;
    for ( int row = 1; row < f.ds.get_height(); row++ ) {
      if ( f.get_row( row ) == new_top_row ) {
        new_top_row_is_unique = false;
        break;
      }
    }

    if ( new_top_row_is_unique ) {
      for ( int row = 1; row < f.ds.get_height(); row++ ) {
        if ( rows.at( row ).get() != new_top_row ) {
          continue;
        }

        bool old_row_is_unique = true;
        for ( int other_row = 0; other_row < f.ds.get_height(); other_row++ ) {
          if ( other_row != row && rows.at( other_row ).get() == new_top_row ) {
            old_row_is_unique = false;
            break;
          }
        }
        if ( !old_row_is_unique ) {
          continue;
        }

        /* found an unambiguous scroll */
        lines_scrolled = row;
        scroll_height = 1;

        /* how big is the region that was scrolled? */
        for ( int region_height = 1; lines_scrolled + region_height < f.ds.get_height(); region_height++ ) {
          if ( f.get_row( region_height ) == rows.at( lines_scrolled + region_height ).get() ) {
            scroll_height = region_height + 1;
          } else {
            break;
          }
        }

        break;
      }
    }

    if ( scroll_height ) {
      frame_y = scroll_height;

      if ( lines_scrolled ) {
        /* Now we need a proper blank row. */
        if ( blank_row.get() == NULL ) {
          const size_t w = f.ds.get_width();
          const color_type c = 0;
          blank_row = std::make_shared<Row>( w, c );
        }
        frame.update_rendition( initial_rendition(), true );
        frame.update_hyperlink( Hyperlink(), true );

        int top_margin = 0;
        int bottom_margin = top_margin + lines_scrolled + scroll_height - 1;

        assert( bottom_margin < f.ds.get_height() );

        /* Common case:  if we're already on the bottom line and we're scrolling the whole
         * screen, just do a CR and LFs.
         */
        if ( scroll_height + lines_scrolled == f.ds.get_height() && frame.cursor_y + 1 == f.ds.get_height() ) {
          frame.append( '\r' );
          frame.append( lines_scrolled, '\n' );
          frame.cursor_x = 0;
        } else {
          /* set scrolling region */
          snprintf( tmp, 64, "\033[%d;%dr", top_margin + 1, bottom_margin + 1 );
          frame.append( tmp );

          /* go to bottom of scrolling region */
          frame.cursor_x = frame.cursor_y = -1;
          frame.append_silent_move( bottom_margin, 0 );

          /* scroll */
          frame.append( lines_scrolled, '\n' );

          /* reset scrolling region */
          frame.append( "\033[r" );
          /* invalidate cursor position after unsetting scrolling region */
          frame.cursor_x = frame.cursor_y = -1;
        }

        /* do the move in our local index */
        for ( int i = top_margin; i <= bottom_margin; i++ ) {
          if ( i + lines_scrolled <= bottom_margin ) {
            rows.at( i ) = rows.at( i + lines_scrolled );
          } else {
            rows.at( i ) = blank_row;
          }
        }
      }
    }
  }

  /* Now update the display, row by row */
  bool wrap = false;
  for ( ; frame_y < f.ds.get_height() - row_offset; frame_y++ ) {
    wrap = put_row( initialized && !graphics_dirty_rows[frame_y], frame, f, frame_y, *rows.at( frame_y ), wrap );
  }

  if ( redraw_sized_text ) {
    for ( int y = 0; y < f.ds.get_height() - row_offset; y++ ) {
      for ( int x = 0; x < f.ds.get_width(); x++ ) {
        const auto* cell = f.get_cell( y, x );
        if ( !cell->get_sized_text() || cell->get_text_x() || cell->get_text_y() ) { continue; }
        frame.append_silent_move( y, x );
        frame.update_rendition( cell->get_renditions() );
        frame.update_hyperlink( cell->get_hyperlink() );
        cell->print_grapheme( frame.str, render_sized_text );
        frame.cursor_x = frame.cursor_y = -1;
      }
    }
  }

  if ( have_sized_text && render_sized_text && !frame.str.empty() ) {
    // Deferred multi-row composition must also reconstruct soft-wrap flags.
    // Reprint the last glyph and let the next glyph wrap naturally, including
    // the protocol's skip over lower continuation rows. CUP alone loses that
    // logical line relationship on the receiving state machine.
    for ( int y = 0; y + 1 < f.ds.get_height() - row_offset; y++ ) {
      if ( !f.get_row( y )->get_wrap() ) { continue; }
      int x = f.ds.get_width() - 1;
      const auto* end = f.get_cell( y, x );
      if ( end->get_sized_text() ) {
        if ( end->get_text_y() ) { continue; }
        x -= end->get_text_x();
      } else if ( x && f.get_cell( y, x - 1 )->get_wide() ) { x--; }
      end = f.get_cell( y, x );
      int next_y = y + 1, next_x = 0;
      while ( next_y < f.ds.get_height() ) {
        const auto* next = f.get_cell( next_y, next_x );
        if ( !next->get_text_y() ) { break; }
        next_x += next->get_width();
        if ( next_x >= f.ds.get_width() ) { next_y++; next_x = 0; }
      }
      if ( next_y >= f.ds.get_height() - row_offset ) { continue; }
      frame.append_silent_move( y, x );
      frame.update_rendition( end->get_renditions() );
      frame.update_hyperlink( end->get_hyperlink() );
      frame.append_cell( *end );
      const auto* next = f.get_cell( next_y, next_x );
      frame.update_rendition( next->get_renditions() );
      frame.update_hyperlink( next->get_hyperlink() );
      frame.append_cell( *next );
      frame.cursor_x = frame.cursor_y = -1;
    }
  }

  if ( render_kitty ) {
    const size_t before_kitty = frame.str.size();
    append_kitty_frame( frame.str, initialized && !redraw_sixel, frame.last_frame, f, convert_sixel );
    if ( frame.str.size() != before_kitty ) {
      frame.cursor_x = frame.cursor_y = -1;
    }
  }

  if ( redraw_sixel ) {
    Sixel::append_native_frame( frame.str, f, graphics_geometry );
    frame.cursor_x = frame.cursor_y = -1;
  }

  /* has cursor location changed? */
  if ( ( !initialized ) || ( f.ds.get_cursor_row() != frame.cursor_y )
       || ( f.ds.get_cursor_col() != frame.cursor_x ) ) {
    frame.append_move( f.ds.get_cursor_row(), f.ds.get_cursor_col() );
  }

  /* has cursor visibility changed? */
  if ( ( !initialized ) || ( f.ds.cursor_visible != frame.cursor_visible ) ) {
    if ( f.ds.cursor_visible ) {
      frame.append( "\033[?25h" );
    } else {
      frame.append( "\033[?25l" );
    }
  }

  /* have renditions changed? */
  frame.update_rendition( f.ds.get_renditions(), !initialized );
  /* has hyperlink changed? */
  frame.update_hyperlink( f.ds.get_hyperlink(), !initialized );

  /* has bracketed paste mode changed? */
  if ( render_clipboard && ( !initialized || f.ds.mime_paste != last.ds.mime_paste ) ) {
    frame.append( f.ds.mime_paste ? "\033[?5522h" : "\033[?5522l" );
  }
  if ( ( !initialized ) || ( f.ds.bracketed_paste != frame.last_frame.ds.bracketed_paste ) ) {
    frame.append( f.ds.bracketed_paste ? "\033[?2004h" : "\033[?2004l" );
  }

  /* has mouse reporting mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_reporting_mode != frame.last_frame.ds.mouse_reporting_mode ) ) {
    if ( f.ds.mouse_reporting_mode == DrawState::MOUSE_REPORTING_NONE ) {
      frame.append( "\033[?1003l" );
      frame.append( "\033[?1002l" );
      frame.append( "\033[?1001l" );
      frame.append( "\033[?1000l" );
    } else {
      if ( frame.last_frame.ds.mouse_reporting_mode != DrawState::MOUSE_REPORTING_NONE ) {
        snprintf( tmp, sizeof( tmp ), "\033[?%dl", frame.last_frame.ds.mouse_reporting_mode );
        frame.append( tmp );
      }
      snprintf( tmp, sizeof( tmp ), "\033[?%dh", f.ds.mouse_reporting_mode );
      frame.append( tmp );
    }
  }

  /* has mouse focus mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_focus_event != frame.last_frame.ds.mouse_focus_event ) ) {
    frame.append( f.ds.mouse_focus_event ? "\033[?1004h" : "\033[?1004l" );
  }

  /* has mouse encoding mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_encoding_mode != frame.last_frame.ds.mouse_encoding_mode ) ) {
    if ( f.ds.mouse_encoding_mode == DrawState::MOUSE_ENCODING_DEFAULT ) {
      frame.append( "\033[?1015l" );
      frame.append( "\033[?1006l" );
      frame.append( "\033[?1005l" );
    } else {
      if ( frame.last_frame.ds.mouse_encoding_mode != DrawState::MOUSE_ENCODING_DEFAULT ) {
        snprintf( tmp, sizeof( tmp ), "\033[?%dl", frame.last_frame.ds.mouse_encoding_mode );
        frame.append( tmp );
      }
      snprintf( tmp, sizeof( tmp ), "\033[?%dh", f.ds.mouse_encoding_mode );
      frame.append( tmp );
    }
  }

  if ( render_keyboard && ( !initialized || f.ds.kitty_keyboard_flags != last.ds.kitty_keyboard_flags ) ) {
    frame.str += "\033[=" + std::to_string( f.ds.kitty_keyboard_flags ) + "u";
  }
  return frame.str;
}

bool Display::put_row( bool initialized,
                       FrameState& frame,
                       const Framebuffer& f,
                       int frame_y,
                       const Row& old_row,
                       bool wrap ) const
{
  char tmp[64];
  int frame_x = 0;

  const Row& row = *f.get_row( frame_y );
  const Row::cells_type& cells = row.cells;
  const Row::cells_type& old_cells = old_row.cells;

  /* If we're forced to write the first column because of wrap, go ahead and do so. */
  if ( wrap && !cells.at( 0 ).get_sized_text() ) {
    const Cell& cell = cells.at( 0 );
    frame.update_rendition( cell.get_renditions() );
    frame.update_hyperlink( cell.get_hyperlink() );
    frame.append_cell( cell );
    frame_x += cell.get_width();
    frame.cursor_x += cell.get_width();
  } else if ( wrap ) {
    frame.cursor_x = frame.cursor_y = -1;
  }

  /* If rows are the same object, we don't need to do anything at all. */
  if ( initialized && &row == &old_row ) {
    return false;
  }

  const bool wrap_this = row.get_wrap();
  const int row_width = f.ds.get_width();
  int clear_count = 0;
  bool wrote_last_cell = false;
  Renditions blank_renditions = initial_rendition();
  Hyperlink blank_hyperlink;

  /* iterate for every cell */
  while ( frame_x < row_width ) {

    const Cell& cell = cells.at( frame_x );

    /* Does cell need to be drawn?  Skip all this. */
    if ( initialized && !clear_count && ( cell == old_cells.at( frame_x ) ) ) {
      frame_x += cell.get_width();
      continue;
    }

    /* Slurp up all the empty cells */
    if ( cell.empty() ) {
      if ( !clear_count ) {
        blank_renditions = cell.get_renditions();
        blank_hyperlink = cell.get_hyperlink();
      }
      if ( cell.get_renditions() == blank_renditions && cell.get_hyperlink() == blank_hyperlink ) {
        /* Remember run of blank cells */
        clear_count++;
        frame_x++;
        continue;
      }
    }

    /* Clear or write cells within the row (not to end). */
    if ( clear_count ) {
      /* Move to the right position. */
      frame.append_silent_move( frame_y, frame_x - clear_count );
      frame.update_rendition( blank_renditions );
      frame.update_hyperlink( blank_hyperlink );
      if ( can_use_erase( frame ) && has_ech && clear_count > 4 ) {
        snprintf( tmp, 64, "\033[%dX", clear_count );
        frame.append( tmp );
      } else {
        frame.append( clear_count, ' ' );
        frame.cursor_x = frame_x;
      }
      clear_count = 0;
      // If the current character is *another* empty cell in a different rendition,
      // we restart counting and continue here
      if ( cell.empty() ) {
        blank_renditions = cell.get_renditions();
        blank_hyperlink = cell.get_hyperlink();
        clear_count = 1;
        frame_x++;
        continue;
      }
    }

    if ( cell.get_sized_text() ) {
      frame_x += cell.get_width();
      continue; // composed after all ordinary cells and erases
    }

    /* Now draw a character cell. */
    /* Move to the right position. */
    const int cell_width = cell.get_width();
    /* If we are about to print the last character in a wrapping row,
       trash the cursor position to force explicit positioning.  We do
       this because our input terminal state may have the cursor on
       the autowrap column ("column 81"), but our output terminal
       states always snap the cursor to the true last column ("column
       80"), and we want to be able to apply the diff to either, for
       verification. */
    if ( wrap_this && frame_x + cell_width >= row_width ) {
      frame.cursor_x = frame.cursor_y = -1;
    }
    frame.append_silent_move( frame_y, frame_x );
    frame.update_rendition( cell.get_renditions() );
    frame.update_hyperlink( cell.get_hyperlink() );
    frame.append_cell( cell );
    frame_x += cell_width;
    frame.cursor_x += cell_width;
    if ( frame_x >= row_width ) {
      wrote_last_cell = true;
    }
  }

  /* End of line. */

  /* Clear or write empty cells at EOL. */
  if ( clear_count ) {
    /* Move to the right position. */
    frame.append_silent_move( frame_y, frame_x - clear_count );
    frame.update_rendition( blank_renditions );
    frame.update_hyperlink( blank_hyperlink );

    if ( can_use_erase( frame ) && !wrap_this ) {
      frame.append( "\033[K" );
    } else {
      frame.append( clear_count, ' ' );
      frame.cursor_x = frame_x;
      wrote_last_cell = true;
    }
  }

  if ( !( wrote_last_cell && ( frame_y + frame.row_offset < f.ds.get_height() - 1 ) ) ) {
    return false;
  }
  /* To hint that a word-select should group the end of one line
     with the beginning of the next, we let the real cursor
     actually wrap around in cases where it wrapped around for us. */
  if ( wrap_this ) {
    /* Update our cursor, and ask for wrap on the next row. */
    frame.cursor_x = 0;
    frame.cursor_y++;
    return true;
  }
  /* Resort to CR/LF and update our cursor. */
  frame.append( "\r\n" );
  frame.cursor_x = 0;
  frame.cursor_y++;
  return false;
}

bool Display::can_use_erase( const FrameState& frame ) const
{
  return has_bce || ( frame.current_rendition == initial_rendition() && frame.current_hyperlink.empty() );
}

FrameState::FrameState( const Framebuffer& s_last, int s_row_offset )
  : str(), cursor_x( 0 ), cursor_y( 0 ), row_offset( s_row_offset ), current_rendition( 0 ), current_hyperlink(),
    cursor_visible( s_last.ds.cursor_visible ), last_frame( s_last )
{
  /* Preallocate for better performance.  Make a guess-- doesn't matter for correctness */
  str.reserve( last_frame.ds.get_width() * last_frame.ds.get_height() * 4 );
}

void FrameState::append_silent_move( int y, int x )
{
  if ( cursor_x == x && cursor_y == y )
    return;
  /* turn off cursor if necessary before moving cursor */
  if ( cursor_visible ) {
    append( "\033[?25l" );
    cursor_visible = false;
  }
  append_move( y, x );
}

void FrameState::append_move( int y, int x )
{
  const int last_x = cursor_x;
  const int last_y = cursor_y;
  cursor_x = x;
  cursor_y = y;
  // Only optimize if cursor pos is known
  if ( last_x != -1 && last_y != -1 ) {
    // Can we use CR and/or LF?  They're cheap and easier to trace.
    if ( x == 0 && y - last_y >= 0 && y - last_y < 5 ) {
      if ( last_x != 0 ) {
        append( '\r' );
      }
      append( y - last_y, '\n' );
      return;
    }
    // Backspaces are good too.
    if ( y == last_y && x - last_x < 0 && x - last_x > -5 ) {
      append( last_x - x, '\b' );
      return;
    }
    // More optimizations are possible.
  }
  char tmp[64];
  snprintf( tmp, 64, "\033[%d;%dH", y + row_offset + 1, x + 1 );
  append( tmp );
}

void FrameState::update_rendition( const Renditions& r, bool force )
{
  if ( force || !( current_rendition == r ) ) {
    /* print renditions */
    append_string( r.sgr() );
    current_rendition = r;
  }
}

void FrameState::update_hyperlink( const Hyperlink& h, bool force )
{
  if ( force || current_hyperlink != h ) {
    /* print hyperlink */
    append_string( h.osc8() );
    current_hyperlink = h;
  }
}
