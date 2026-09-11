/* Distributed under the GNU GPL, version 3 or later. */
#include "sizedtext.h"
#include "grapheme.h"
#include "terminalframebuffer.h"
#include <algorithm>
#include <cwchar>
#include <set>

namespace Terminal {
void SizedText::update_fallback()
{
  std::wstring decoded;
  mbstate_t state {};
  for ( size_t at = 0; at < text.size(); ) {
    wchar_t c = 0;
    const size_t used = mbrtowc( &c, text.data() + at, text.size() - at, &state );
    if ( !used || used > text.size() - at ) { break; }
    decoded += c;
    at += used;
  }
  fallback.clear();
  unsigned used = 0;
  for ( const auto& cluster : Unicode16::graphemes( decoded ) ) {
    const unsigned width = Unicode16::cell_width( cluster );
    if ( used + width > columns() ) { break; }
    for ( wchar_t c : cluster ) { Cell::append_to_str( fallback, c ); }
    used += width;
  }
  fallback.append( columns() - used, ' ' );
}

std::string SizedText::sequence() const
{
  std::string metadata = "s=" + std::to_string( scale ) + ":w=" + std::to_string( width );
  if ( numerator ) { metadata += ":n=" + std::to_string( numerator ); }
  if ( denominator ) { metadata += ":d=" + std::to_string( denominator ); }
  if ( vertical ) { metadata += ":v=" + std::to_string( vertical ); }
  if ( horizontal ) { metadata += ":h=" + std::to_string( horizontal ); }
  return "\033]66;" + metadata + ";" + text + "\033\\";
}

void Framebuffer::erase_sized_text( int row, int col, int height, int width )
{
  if ( height <= 0 || width <= 0 ) { return; }
  if ( row < 0 ) { row = ds.get_cursor_row(); }
  if ( col < 0 ) { col = ds.get_cursor_col(); }
  std::set<std::pair<int, int>> origins;
  const int bottom = std::min( ds.get_height(), row + height ), right = std::min( ds.get_width(), col + width );
  for ( int y = row; y < bottom; y++ ) {
    for ( int x = col; x < right; x++ ) {
      const auto* cell = get_cell( y, x );
      if ( cell->get_sized_text() ) { origins.emplace( y - cell->get_text_y(), x - cell->get_text_x() ); }
    }
  }
  for ( const auto& origin : origins ) {
    if ( origin.first < 0 || origin.second < 0 ) { continue; }
    const auto text = get_cell( origin.first, origin.second )->get_sized_text();
    if ( !text ) { continue; }
    for ( int y = origin.first; y < std::min( ds.get_height(), origin.first + int( text->scale ) ); y++ ) {
      for ( int x = origin.second; x < std::min( ds.get_width(), origin.second + int( text->columns() ) ); x++ ) {
        get_mutable_cell( y, x )->reset( ds.get_background_rendition() );
      }
    }
  }
}

void Framebuffer::erase_sized_text_boundary( int row )
{
  if ( row < 0 || row >= ds.get_height() ) { return; }
  for ( int x = 0; x < ds.get_width(); x++ ) {
    if ( get_cell( row, x )->get_text_y() ) { erase_sized_text( row, x, 1, 1 ); }
  }
}

void Framebuffer::edit_sized_text_row( int row, int col, bool inserting )
{
  // Horizontal edits can move a single-height block as a unit, but must not
  // shear a multi-height block or leave a partial block at the right edge.
  for ( int x = col; x < ds.get_width(); x++ ) {
    const auto* cell = get_cell( row, x );
    const auto text = cell->get_sized_text();
    if ( text && ( text->scale > 1 || ( x == col && ( !inserting || cell->get_text_x() ) )
                   || ( inserting && x == ds.get_width() - 1 ) ) ) {
      erase_sized_text( row, x, 1, 1 );
    }
  }
}

void Framebuffer::skip_sized_continuations()
{
  for ( int attempts = 0; attempts <= ds.get_height() * ds.get_width(); attempts++ ) {
    const auto* cell = get_cell();
    if ( !cell->get_sized_text() || !cell->get_text_y() ) { return; }
    const int next = ds.get_cursor_col() + cell->get_width();
    if ( next < ds.get_width() ) { ds.move_col( next ); }
    else {
      const int previous = ds.get_cursor_row();
      ds.move_col( 0 ); move_rows_autoscroll( 1 );
      if ( ds.get_cursor_row() == previous && previous != ds.get_scrolling_region_bottom_row() ) { return; }
    }
    ds.next_print_will_wrap = false;
  }
}

void Framebuffer::put_sized_text( const SizedText& text )
{
  const int width = text.columns(), height = text.scale;
  if ( width > ds.get_width() || height > ds.get_height() ) { return; }
  skip_sized_continuations();
  if ( ds.auto_wrap_mode && ( ds.next_print_will_wrap || ds.get_cursor_col() + width > ds.get_width() ) ) {
    get_mutable_row( -1 )->set_wrap( true );
    ds.move_col( 0 );
    move_rows_autoscroll( 1 );
    skip_sized_continuations();
  } else if ( ds.get_cursor_col() + width > ds.get_width() ) {
    ds.move_col( ds.get_width() - width );
  }
  int row = ds.get_cursor_row(), col = ds.get_cursor_col();
  const int bottom = row <= ds.get_scrolling_region_bottom_row() ? ds.get_scrolling_region_bottom_row() : ds.get_height() - 1;
  if ( height > bottom - ds.get_scrolling_region_top_row() + 1 ) { return; }
  if ( row + height > bottom + 1 ) {
    const int n = row + height - bottom - 1;
    scroll( n );
    ds.move_row( -n, true );
    row = ds.get_cursor_row();
  }
  erase_sized_text( row, col, height, width );
  erase_sixel_cells( row, col, height, width );
  auto shared = std::make_shared<SizedText>( text );
  for ( int y = 0; y < height; y++ ) {
    for ( int x = 0; x < width; x++ ) {
      auto* cell = get_mutable_cell( row + y, col + x );
      cell->reset( ds.get_background_rendition() );
      apply_renditions_to_cell( cell );
      apply_hyperlink_to_cell( cell );
      cell->set_sized_text( shared, x, y );
    }
  }
  ds.move_col( width, true, true );
}

bool Framebuffer::append_sized_combining( Cell* cell, wchar_t ch )
{
  const auto old = cell->get_sized_text();
  if ( !old ) { return false; }
  if ( old->text.size() > 4092 ) { return true; }
  auto combined = std::make_shared<SizedText>( *old );
  Cell::append_to_str( combined->text, ch );
  combined->update_fallback();
  // Preserve the whole block's identity, including its continuation cells.
  for ( int y = 0; y < ds.get_height(); y++ ) {
    for ( int x = 0; x < ds.get_width(); x++ ) {
      const auto* current = get_cell( y, x );
      if ( current->get_sized_text() == old ) {
        const auto dx = current->get_text_x(), dy = current->get_text_y();
        get_mutable_cell( y, x )->set_sized_text( combined, dx, dy );
      }
    }
  }
  return true;
}

void parse_sized_text( const std::vector<wchar_t>& osc, Framebuffer& fb )
{
  const auto separator = std::find( osc.begin() + 3, osc.end(), ';' );
  if ( separator == osc.end() || separator - osc.begin() > 128 ) { return; }
  SizedText params;
  unsigned specified_width = 0;
  size_t at = 3;
  const size_t end = separator - osc.begin();
  while ( at < end ) {
    const wchar_t key = osc[at++];
    if ( at >= end || osc[at++] != '=' ) { return; }
    unsigned value = 0;
    const size_t start = at;
    while ( at < end && osc[at] >= '0' && osc[at] <= '9' ) {
      if ( value > 6553 ) { return; }
      value = value * 10 + osc[at++] - '0';
    }
    if ( at == start || ( at < end && osc[at++] != ':' ) ) { return; }
    switch ( key ) {
      case 's': if ( !value || value > 7 ) { return; } params.scale = value; break;
      case 'w': if ( value > 7 ) { return; } specified_width = value; break;
      case 'n': if ( value > 15 ) { return; } params.numerator = value; break;
      case 'd': if ( value > 15 ) { return; } params.denominator = value; break;
      case 'v': if ( value > 2 ) { return; } params.vertical = value; break;
      case 'h': if ( value > 2 ) { return; } params.horizontal = value; break;
      default: break; // unknown metadata is forward-compatible
    }
  }
  if ( params.denominator && params.numerator >= params.denominator ) { return; }
  std::wstring decoded;
  size_t bytes = 0;
  for ( auto it = separator + 1; it != osc.end(); ++it ) {
    const wchar_t c = *it;
    if ( c == 0 || Unicode16::invalid( c ) ) { continue; }
    std::string encoded;
    Cell::append_to_str( encoded, c );
    bytes += encoded.size();
    if ( bytes > 4096 ) { return; }
    decoded += c;
  }
  const auto graphemes = specified_width ? std::vector<std::wstring> { decoded } : Unicode16::graphemes( decoded );
  std::vector<std::pair<std::wstring, unsigned>> clusters;
  for ( const auto& cluster : graphemes ) {
    if ( cluster.empty() ) { continue; }
    const unsigned width = specified_width ? specified_width : Unicode16::cell_width( cluster );
    if ( !width ) {
      if ( !clusters.empty() ) { clusters.back().first += cluster; }
      else {
        for ( wchar_t c : cluster ) {
          auto* previous = fb.get_combining_cell();
          if ( previous && !fb.append_sized_combining( previous, c ) && !previous->empty() && !previous->full() ) {
            previous->append( c );
          }
        }
      }
    } else { clusters.emplace_back( cluster, width ); }
  }
  for ( const auto& cluster : clusters ) {
    SizedText text( params );
    text.width = cluster.second;
    for ( wchar_t c : cluster.first ) { Cell::append_to_str( text.text, c ); }
    text.update_fallback();
    fb.put_sized_text( text );
  }
}
}
