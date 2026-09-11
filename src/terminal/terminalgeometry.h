/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef TERMINAL_GEOMETRY_HPP
#define TERMINAL_GEOMETRY_HPP

#include <cstdint>

namespace Terminal {

/* Local presentation capabilities, sent once per attachment, never replicated
   as terminal state. A sixel source can use Kitty as a client-side fallback. */
struct ClientGraphics
{
  bool kitty, sixel, keyboard;
  unsigned text_sizing;
  bool clipboard;
  uint32_t clipboard_fast_threshold;
  bool downloads = false;
  ClientGraphics( bool k = false, bool s = false, bool kb = false, unsigned ts = 0, bool cb = false, uint32_t ct = 65536 )
    : kitty( k ), sixel( s ), keyboard( kb ), text_sizing( ts ), clipboard( cb ), clipboard_fast_threshold( ct ) {}
  bool operator==( const ClientGraphics& other ) const
  { return kitty == other.kitty && sixel == other.sixel && keyboard == other.keyboard && text_sizing == other.text_sizing
           && clipboard == other.clipboard && clipboard_fast_threshold == other.clipboard_fast_threshold && downloads == other.downloads; }
};

/* Presentation geometry for the currently attached client. This is control
   metadata, not part of the replicated terminal or framebuffer state. */
struct ClientGeometry
{
  uint32_t columns;
  uint32_t rows;
  uint32_t width_px;
  uint32_t height_px;
  uint32_t cell_width_px;
  uint32_t cell_height_px;

  ClientGeometry()
    : columns( 0 ), rows( 0 ), width_px( 0 ), height_px( 0 ), cell_width_px( 0 ), cell_height_px( 0 )
  {}

  ClientGeometry( uint32_t s_columns,
                  uint32_t s_rows,
                  uint32_t s_width_px,
                  uint32_t s_height_px,
                  uint32_t s_cell_width_px,
                  uint32_t s_cell_height_px )
    : columns( s_columns ), rows( s_rows ), width_px( s_width_px ), height_px( s_height_px ),
      cell_width_px( s_cell_width_px ), cell_height_px( s_cell_height_px )
  {}

  bool has_grid( void ) const { return columns != 0 && rows != 0; }
  bool has_pixel_size( void ) const { return width_px != 0 && height_px != 0; }
  bool has_cell_size( void ) const { return cell_width_px != 0 && cell_height_px != 0; }

  bool operator==( const ClientGeometry& other ) const
  {
    return columns == other.columns && rows == other.rows && width_px == other.width_px
           && height_px == other.height_px && cell_width_px == other.cell_width_px
           && cell_height_px == other.cell_height_px;
  }
};

}

#endif
