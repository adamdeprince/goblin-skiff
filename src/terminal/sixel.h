/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_SIXEL_H
#define MOSH_SIXEL_H

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>

#include "src/terminal/kittygraphics.h"
#include "src/terminal/terminalgeometry.h"

namespace Terminal {
namespace Sixel {
constexpr size_t MAX_ENCODED_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_PIXELS = GRAPHICS_MAX_PIXELS;
constexpr uint32_t MAX_DIMENSION = GRAPHICS_MAX_DIMENSION;
constexpr unsigned MAX_COLORS = 1024;
constexpr size_t MAX_PLACEMENTS = 8192;

struct Palette
{
  std::array<uint32_t, MAX_COLORS> colors;
  Palette();
};

struct Bitmap
{
  uint32_t width, height;
  std::string rgba;
  Bitmap() : width( 0 ), height( 0 ), rgba() {}
};

// Decode one complete DCS. Limits are checked before allocating pixels;
// malformed/truncated/oversized input leaves the output empty. This codec
// uses private per-image palettes; terminal mode/lifetime handling is separate.
bool decode( const std::string& dcs, Bitmap& output, std::string& error, uint32_t background_rgb = 0,
             Palette* shared_palette = nullptr );
// Encode binary-alpha RGBA as an independent square-pixel sixel DCS.
// No cursor moves, screen clearing, terminal queries or network I/O here.
bool encode( const Bitmap& input, std::string& dcs, std::string& error, unsigned x = 0, unsigned y = 0 );

enum class Renderer
{
  None,
  Sixel,
  Kitty
};
Renderer select_renderer( bool local_sixel, bool local_kitty );
// Bridge into the existing synchronized image state. No terminal cell size
// or local capability information is stored in the image.
bool normalize( const std::string& dcs, KittyImage& image, std::string& error, uint32_t background_rgb = 0 );
// Encode one image for the local terminal. Placement, scrolling and erasure
// must be handled by the compositor; this does not replay terminal commands.
bool render( const KittyImage& image, bool local_sixel, bool local_kitty, std::string& bytes, std::string& error );
// Logical cell coverage plus pixel crops; no attachment resolution is stored.
void place( Framebuffer& fb, const Bitmap& bitmap, const ClientGeometry* geometry, bool display_mode, bool cursor_right );
KittyPlacement crop( const KittyPlacement& p, int row, int col, unsigned rows, unsigned columns, unsigned image_width );
bool changed( const Framebuffer& last, const Framebuffer& now );
void append_native_frame( std::string& output, const Framebuffer& fb, const ClientGeometry& geometry );
}
}
#endif
