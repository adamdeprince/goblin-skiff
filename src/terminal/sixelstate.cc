/* Distributed under the GNU GPL, version 3 or later. */
#include "sixel.h"
#include "terminalframebuffer.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace Terminal {
namespace Sixel {
KittyPlacement crop( const KittyPlacement& p, int row, int col, unsigned rows, unsigned columns,
                     unsigned image_width )
{
  KittyPlacement part( p );
  const unsigned cw = p.src_w / p.columns, ch = p.src_h / p.rows;
  part.src_x += unsigned( col - p.col ) * cw;
  part.src_y += unsigned( row - p.row ) * ch;
  part.src_w = columns * cw;
  part.src_h = rows * ch;
  part.row = row;
  part.col = col;
  part.rows = rows;
  part.columns = columns;
  part.placement_id = 1 + part.src_y * image_width + part.src_x;
  return part;
}

void place( Framebuffer& fb, const Bitmap& bitmap, const ClientGeometry* geometry, bool display_mode,
            bool cursor_right )
{
  if ( !bitmap.width || !bitmap.height ) { return; } // palette-only DCS
  unsigned cw = 8, ch = 16;
  if ( geometry && geometry->has_cell_size() && geometry->columns == unsigned( fb.ds.get_width() )
       && geometry->rows == unsigned( fb.ds.get_height() ) ) {
    cw = std::min( 1024U, geometry->cell_width_px );
    ch = std::min( 1024U, geometry->cell_height_px );
  }
  const unsigned columns = ( bitmap.width + cw - 1 ) / cw, rows = ( bitmap.height + ch - 1 ) / ch;
  const unsigned width = columns * cw, height = rows * ch;
  if ( width > MAX_DIMENSION || height > MAX_DIMENSION || size_t( width ) * height > MAX_PIXELS ) { return; }

  // Transparent padding makes the image's logical cell coverage exact. Crops
  // can then be split on cell boundaries without saving attachment geometry.
  std::string pixels( size_t( width ) * height * 4, '\0' );
  for ( unsigned y = 0; y < bitmap.height; y++ ) {
    memcpy( &pixels[size_t( y ) * width * 4], &bitmap.rgba[size_t( y ) * bitmap.width * 4], bitmap.width * 4 );
  }
  KittyImage image;
  std::string error;
  if ( !kitty_normalize_webp( KITTY_FORMAT_RGBA, width, height, pixels, *image.data, image.width, image.height, error ) ) {
    return;
  }
  image.origin = ImageOrigin::Sixel;
  int row = display_mode ? 0 : fb.ds.get_cursor_row(), col = display_mode ? 0 : fb.ds.get_cursor_col();
  int top = 0, bottom = fb.ds.get_height() - 1;
  int cursor_row = row + int( rows ) - 1, cursor_col = col;
  if ( !display_mode && cursor_right ) {
    cursor_col += columns;
    if ( cursor_col >= fb.ds.get_width() ) { cursor_col = 0; cursor_row++; }
  }
  if ( !display_mode && row >= fb.ds.get_scrolling_region_top_row()
       && row <= fb.ds.get_scrolling_region_bottom_row() ) {
    top = fb.ds.get_scrolling_region_top_row();
    bottom = fb.ds.get_scrolling_region_bottom_row();
    const int scroll = std::max( 0, cursor_row - bottom );
    fb.scroll( scroll );
    row -= scroll;
    cursor_row -= scroll;
  }
  if ( !display_mode ) {
    fb.ds.move_row( cursor_row - fb.ds.get_cursor_row(), true );
    fb.ds.move_col( cursor_col );
    fb.ds.next_print_will_wrap = false;
  }

  const int first = std::max( row, top ), last = std::min( row + int( rows ), bottom + 1 );
  const int right = std::min( col + int( columns ), fb.ds.get_width() );
  if ( first >= last || col >= right ) { return; }
  // Repeated opaque full-cell drawings replace their old coverage immediately.
  bool opaque = width == bitmap.width && height == bitmap.height;
  for ( size_t i = 3; opaque && i < bitmap.rgba.size(); i += 4 ) { opaque = uint8_t( bitmap.rgba[i] ) == 255; }
  if ( opaque ) { fb.erase_sixel_cells( first, col, last - first, right - col ); }
  image.id = fb.put_kitty_image( image );
  if ( !image.id ) { return; }
  KittyPlacement p;
  p.image_id = image.id;
  p.placement_id = 1;
  p.row = row;
  p.col = col;
  p.columns = columns;
  p.rows = rows;
  p.src_w = width;
  p.src_h = height;
  p.cursor_hold = true;
  fb.put_kitty_placement( crop( p, first, col, last - first, right - col, width ) );
  fb.prune_sixel_images();
}

static std::vector<KittyPlacement> placements( const Framebuffer& fb )
{
  std::vector<KittyPlacement> result;
  for ( const auto& p : fb.get_kitty_placements() ) {
    const auto* image = fb.find_kitty_image( p.image_id );
    if ( image && image->origin == ImageOrigin::Sixel ) { result.push_back( p ); }
  }
  return result;
}

bool changed( const Framebuffer& last, const Framebuffer& now )
{
  const auto before = placements( last ), after = placements( now );
  if ( before != after ) { return true; }
  for ( const auto& p : after ) {
    const auto* old = last.find_kitty_image( p.image_id );
    if ( !old || !( *old == *now.find_kitty_image( p.image_id ) ) ) { return true; }
  }
  return false;
}

void append_native_frame( std::string& output, const Framebuffer& fb, const ClientGeometry& geometry )
{
  std::map<uint32_t, Bitmap> decoded;
  std::string images;
  for ( const auto& p : placements( fb ) ) {
    const auto* image = fb.find_kitty_image( p.image_id );
    if ( !p.columns || !p.rows || !p.src_w || !p.src_h || p.row < 0 || p.col < 0 ) { continue; }
    auto& source = decoded[p.image_id];
    if ( source.rgba.empty() && !kitty_webp_to_rgba( *image->data, source.rgba, source.width, source.height ) ) { continue; }
    if ( uint64_t( p.src_x ) + p.src_w > source.width || uint64_t( p.src_y ) + p.src_h > source.height ) { continue; }
    const unsigned cw = geometry.has_cell_size() ? geometry.cell_width_px : p.src_w / p.columns;
    const unsigned ch = geometry.has_cell_size() ? geometry.cell_height_px : p.src_h / p.rows;
    const uint64_t width = uint64_t( p.columns ) * cw, height = uint64_t( p.rows ) * ch;
    if ( !width || !height || width > MAX_DIMENSION || height > MAX_DIMENSION || width * height > MAX_PIXELS ) { continue; }
    Bitmap part;
    part.width = width;
    part.height = height;
    part.rgba.resize( size_t( width * height ) * 4 );
    // Nearest-neighbor preserves binary alpha and palette colors when fonts
    // change. Both local protocols display the same logical cell footprint.
    for ( unsigned y = 0; y < part.height; y++ ) {
      for ( unsigned x = 0; x < part.width; x++ ) {
        const size_t source_pos = ( size_t( p.src_y + uint64_t( y ) * p.src_h / part.height ) * source.width
                                    + p.src_x + uint64_t( x ) * p.src_w / part.width ) * 4;
        memcpy( &part.rgba[( size_t( y ) * part.width + x ) * 4], &source.rgba[source_pos], 4 );
      }
    }
    std::string encoded, error;
    if ( encode( part, encoded, error, unsigned( p.col ) * cw, unsigned( p.row ) * ch ) ) { images += encoded; }
  }
  if ( !images.empty() ) {
    // Display mode is anchored at (0,0) and never scrolls the local terminal.
    // Transparent prefix runs encode the placement without a viewport bitmap.
    output += "\033[?80h\033[?1070h" + images + "\033[?80l";
  }
}
}

void Framebuffer::prune_sixel_images()
{
  if ( !sixel_mutations ) { return; }
  // Fragment count is also bounded, independently of the image byte quota.
  size_t count = 0;
  for ( const auto& p : kitty_placements ) {
    const auto* image = find_kitty_image( p.image_id );
    if ( image && image->origin == ImageOrigin::Sixel ) { count++; }
  }
  for ( auto it = kitty_placements.begin(); count > Sixel::MAX_PLACEMENTS && it != kitty_placements.end(); ) {
    const auto* image = find_kitty_image( it->image_id );
    if ( image && image->origin == ImageOrigin::Sixel ) { it = kitty_placements.erase( it ); count--; }
    else { ++it; }
  }
  std::set<uint32_t> used;
  for ( const auto& p : kitty_placements ) { used.insert( p.image_id ); }
  for ( auto it = kitty_images.begin(); it != kitty_images.end(); ) {
    if ( it->second.origin == ImageOrigin::Sixel && !used.count( it->first ) ) { it = kitty_images.erase( it ); }
    else { ++it; }
  }
}

void Framebuffer::erase_sixel_cells( int row, int col, int height, int width )
{
  if ( !sixel_mutations || height <= 0 || width <= 0 || kitty_images.empty() ) { return; }
  if ( row < 0 ) { row = ds.get_cursor_row(); }
  if ( col < 0 ) { col = ds.get_cursor_col(); }
  const int bottom = row + std::min( height, ds.get_height() ), right = col + std::min( width, ds.get_width() );
  std::vector<KittyPlacement> kept;
  for ( const auto& p : kitty_placements ) {
    const auto* image = find_kitty_image( p.image_id );
    if ( !image || image->origin != ImageOrigin::Sixel || !p.columns || !p.rows ) { kept.push_back( p ); continue; }
    const int pb = p.row + int( p.rows ), pr = p.col + int( p.columns );
    const int y = std::max( row, p.row ), x = std::max( col, p.col );
    const int b = std::min( bottom, pb ), r = std::min( right, pr );
    if ( y >= b || x >= r ) { kept.push_back( p ); continue; }
    if ( p.row < y ) { kept.push_back( Sixel::crop( p, p.row, p.col, y - p.row, p.columns, image->width ) ); }
    if ( b < pb ) { kept.push_back( Sixel::crop( p, b, p.col, pb - b, p.columns, image->width ) ); }
    if ( p.col < x ) { kept.push_back( Sixel::crop( p, y, p.col, b - y, x - p.col, image->width ) ); }
    if ( r < pr ) { kept.push_back( Sixel::crop( p, y, r, b - y, pr - r, image->width ) ); }
  }
  kitty_placements.swap( kept );
  prune_sixel_images();
}

void Framebuffer::scroll_sixel( int first_row, int count, bool inserting )
{
  if ( !sixel_mutations ) { return; }
  const int bottom = ds.get_scrolling_region_bottom_row() + 1;
  std::vector<KittyPlacement> kept;
  for ( const auto& p : kitty_placements ) {
    const auto* image = find_kitty_image( p.image_id );
    if ( !image || image->origin != ImageOrigin::Sixel || !p.columns || !p.rows ) { kept.push_back( p ); continue; }
    const int pb = p.row + int( p.rows );
    // Split at the scroll-region edges. Content outside it must not move.
    const int before = std::min( pb, first_row );
    if ( p.row < before ) { kept.push_back( Sixel::crop( p, p.row, p.col, before - p.row, p.columns, image->width ) ); }
    const int after = std::max( p.row, bottom );
    if ( after < pb ) { kept.push_back( Sixel::crop( p, after, p.col, pb - after, p.columns, image->width ) ); }
    const int start = std::max( p.row, first_row + ( inserting ? 0 : count ) );
    const int end = std::min( pb, bottom - ( inserting ? count : 0 ) );
    if ( start < end ) {
      auto moved = Sixel::crop( p, start, p.col, end - start, p.columns, image->width );
      moved.row += inserting ? count : -count;
      kept.push_back( moved );
    }
  }
  kitty_placements.swap( kept );
  prune_sixel_images();
}
}
