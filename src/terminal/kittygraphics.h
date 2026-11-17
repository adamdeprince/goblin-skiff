/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef KITTYGRAPHICS_HPP
#define KITTYGRAPHICS_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "src/terminal/vectorscene.h"

namespace Terminal {
class Framebuffer;
class GpuSurface;

static const size_t KITTY_CHUNK_B64 = 4096;
static const uint32_t KITTY_FORMAT_RGB = 24;
static const uint32_t KITTY_FORMAT_RGBA = 32;
static const uint32_t KITTY_FORMAT_PNG = 100;
/* Silkie's formats: RGBA as half floats in linear extended sRGB, and a scene
   of vector records. Both reach the terminal as they were sent. */
static const uint32_t KITTY_FORMAT_HDR = 64;
static const uint32_t KITTY_FORMAT_VECTOR = 200;
/* The client keeps an image it patched as plain RGBA. Internal, like WebP. */
static const uint32_t KITTY_FORMAT_RAW = 0x52415741U;
/* Internal codecs, never emitted to the terminal. The public Kitty protocol
   only defines RGB, RGBA, and PNG. */
static const uint32_t KITTY_FORMAT_WEBP = 0x57454250U;
static const uint32_t KITTY_FORMAT_DJVU = 0x444A5655U;
static const size_t GRAPHICS_MAX_PALETTE = 16;
/* The F key of a placement: bit 1 places a relative placement from its parent
   in pixels, bit 2 clips its descendants to it. */
static const uint32_t KITTY_PLACE_PIXEL_RELATIVE = 1;
static const uint32_t KITTY_PLACE_VIEWPORT = 2;
static const uint32_t KITTY_PLACE_KNOWN = 3;

struct ImageEncoding
{
  int quality = -1; // -1: lossless WebP; 0..100: lossy WebP
  bool palette_djvu = false; // enabled only after peer capability negotiation
  bool djvu_lossy = false; // permit cjb2 symbol substitution for bilevel images
  bool png = false; // png-state-v1: lossless fallback beyond WebP dimensions
  /* packed-state-v2: Silkie's packed formats (f=301, 302, 304, 308, 516) stay packed in
     image state, see packedimage.h; packed-zstd-v1: and may be compressed
     with zstd. Without the first they are expanded, and still kept exactly. */
  bool packed = false;
  bool packed_zstd = false;
  bool native = false; // native-image-v1: exact indexed, palette and precision pixels
};

enum KittyAction
{
  KittyTransmit = 't',
  KittyTransmitAndDisplay = 'T',
  KittyQuery = 'q',
  KittyPut = 'p',
  KittyDelete = 'd',
  KittyFrame = 'f',
  KittyAnimate = 'a',
  KittyCompose = 'c',
  KittyEdit = 'e',    /* a patch to a vector image */
  KittyRefresh = 'r', /* copy a GPU surface again */
  KittyService = 'v'  /* the fonts and measurements of f=200, asked of the terminal */
};

struct KittyCommand
{
  char action;
  char medium;
  char compression;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint64_t data_size;
  uint64_t data_offset;
  char feature = 0; // e: query features of this action without uploading
  uint32_t transfer = 0; // E: H.273 transfer function
  uint32_t image_id;
  uint32_t image_number;
  uint32_t placement_id;
  uint32_t more;
  uint32_t quiet;
  uint32_t columns;
  uint32_t rows;
  uint32_t src_x;
  uint32_t src_y;
  uint32_t src_w;
  uint32_t src_h;
  uint32_t cell_x;
  uint32_t cell_y;
  int32_t z;
  uint32_t cursor_hold;
  uint32_t unicode_placeholder;
  uint32_t parent_image;
  uint32_t parent_placement;
  int32_t H;
  int32_t V;
  uint32_t gamut;    /* G: 0 sRGB, 1 Display P3, 2 Rec. 2020 */
  bool gamut_set = false;
  uint32_t tone_map; /* T: compress what is brighter than the headroom */
  uint32_t flags;    /* F: see KittyPlacement */
  char delete_target;
  std::string payload;

  KittyCommand();
};

// A sixel image must prefer native sixel output, even on dual-capable clients.
enum class ImageOrigin { Kitty, Sixel };

struct ImageDelta;

struct KittyImage
{
  uint32_t id;
  uint32_t number;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  ImageOrigin origin;
  std::shared_ptr<std::string> data;
  std::string palette; // DjVu: 1..16 exact RGBA entries, including alpha
  uint64_t serial;
  std::shared_ptr<const Vector::Doc> vector; // KITTY_FORMAT_VECTOR: the scene
  uint32_t gamut = 0;                         // of the RGB pixels, as the G key
  bool tone_map = false;                      // the T key of an HDR image
  /* The GPU surface it copies, held from the first t=g until the image is
     deleted (server only, never synchronized). */
  std::shared_ptr<GpuSurface> surface;
  bool png_original = false;                  // a PNG with a color profile, passed on as it came
  bool native = false; // data is the original raw payload, including any palette
  uint32_t transfer = 0;
  /* What changed from the version before, when only a rectangle did: a peer
     that has that version is sent the rectangle instead of the image. */
  std::shared_ptr<const ImageDelta> delta;

  KittyImage();
  bool operator==( const KittyImage& other ) const;
};

struct ImageDelta
{
  uint64_t base_serial = 0; /* the serial of the version it changes */
  uint32_t x = 0, y = 0;
  KittyImage rect; /* encoded like any image; plain RGBA on the client */
};

struct KittyPlacement
{
  uint32_t image_id;
  uint32_t placement_id;
  int row;
  int col;
  uint32_t columns;
  uint32_t rows;
  uint32_t src_x;
  uint32_t src_y;
  uint32_t src_w;
  uint32_t src_h;
  uint32_t cell_x;
  uint32_t cell_y;
  int32_t z;
  bool cursor_hold;
  bool unicode_placeholder;
  uint32_t parent_image;
  uint32_t parent_placement;
  int32_t H;
  int32_t V;
  uint32_t flags; /* the F key of Silkie's viewport placements */
  /* The depth and phase of the text it was placed amid (OSC 777 depth),
     which a placement keeps: see TextStyle. */
  int32_t depth = 0;
  float depth_pixels = 0, phase = 0;

  KittyPlacement();
  bool has_depth() const { return depth || depth_pixels != 0 || phase != 0; }
  bool pixel_relative() const { return parent_image != 0 && ( flags & KITTY_PLACE_PIXEL_RELATIVE ); }
  bool operator==( const KittyPlacement& other ) const;
};

/* The depth and phase the terminal's cursor has, which a placement made
   there takes: what the caller knows it to be going in, and what the
   placements left it at coming out. */
struct CursorDepth
{
  bool known = false;
  int32_t depth = 0;
  float depth_pixels = 0, phase = 0;
};

/* Where a placement is shown, in pixels from the top left of the cell area,
   as the terminal lays it out: the whole rectangle, the source rectangle it
   shows, and what the viewports above it leave visible. */
struct KittyPlacementRect
{
  double left = 0, top = 0, width = 0, height = 0, sx = 0, sy = 0, sw = 0, sh = 0;
  bool clipped = false;
  double clip_left = 0, clip_top = 0, clip_right = 0, clip_bottom = 0;
};
bool kitty_placement_rect( const Framebuffer& fb, const KittyPlacement& placement, double cell_width,
                           double cell_height, KittyPlacementRect& out );

bool parse_kitty_command( const std::string& apc, KittyCommand& cmd );
std::string encode_kitty_chunks( const std::string& controls, const std::string& binary );
/* A request of the vector service for the terminal, which answers it. */
std::string encode_vector_service( uint32_t request_id, const std::string& json, uint32_t quiet = 0 );
/* frame: the r key of a reply to a=f or a=a, as kitty writes it; 0 for none. */
std::string kitty_response( uint32_t image_id, uint32_t placement_id, const std::string& message, uint32_t frame = 0 );
bool kitty_read_medium( const KittyCommand& cmd, std::string& data, std::string& error );
bool kitty_inflate( const std::string& input, std::string& output, size_t hint );
// Check representability, without imposing an image/storage quota.
bool graphics_pixel_size( uint32_t width, uint32_t height, size_t bytes_per_pixel, size_t& size );
bool kitty_normalize_webp( uint32_t format,
                           uint32_t width,
                           uint32_t height,
                           const std::string& input,
                           std::string& webp,
                           uint32_t& output_width,
                           uint32_t& output_height,
                           std::string& error );
bool kitty_webp_dimensions( const std::string& webp, uint32_t& width, uint32_t& height );
bool kitty_webp_to_rgba( const std::string& webp, std::string& rgba, uint32_t& width, uint32_t& height );
bool kitty_normalize_image( uint32_t format, uint32_t width, uint32_t height, const std::string& input,
                            KittyImage& image, const ImageEncoding& encoding, std::string& error );
bool kitty_image_to_rgba( const KittyImage& image, std::string& rgba );
/* An image kept in one of Silkie's packed formats, whose format is the f key
   it was sent with: its pixels as the program packed them. */
bool kitty_image_is_packed( const KittyImage& image );
bool kitty_image_packed_bytes( const KittyImage& image, std::string& packed );
/* The encoding for what must stay exact whatever the user asked of WebP. */
ImageEncoding kitty_exact_encoding( const ImageEncoding& encoding );
/* The packed formats in a terminal's listing of formats ("301,302,304,308,516,24"),
   as internal bits returned by kitty_format_bit (not the f values). */
uint32_t kitty_packed_formats( const std::string& listing );
uint32_t kitty_format_bit( uint32_t format );
bool kitty_palette_format( uint32_t format );
bool kitty_native_format( uint32_t format );
bool kitty_transfer_valid( uint32_t format, uint32_t transfer );
bool kitty_native_size( uint32_t format, uint32_t width, uint32_t height, size_t& size );
bool kitty_native_to_hdr( const KittyImage& image, std::string& half );
/* A PNG to straight RGBA with its colors as Silkie reads them: in the gamut
   (1 Display P3, 2 Rec. 2020) of a cICP chunk or an ICC profile that has its
   primaries, with the sRGB curve; otherwise sRGB, converted from a profile or
   a gamma it carries. */
bool kitty_png_to_rgba( const std::string& png, std::string& rgba, uint32_t& width, uint32_t& height,
                        uint32_t& gamut );
/* The changed rectangle between two images of one size: false when nothing
   changed. */
bool kitty_dirty_rect( const std::string& before, const std::string& after, uint32_t width, uint32_t height,
                       uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h );
/* vector_resent: set when a vector image is sent whole, which ends the
   strokes of ink the terminal is drawing into it. State keeps an image as
   it was sent: an HDR image (f=64) is converted to SDR for a terminal
   without hdr_supported, and RGB pixels in a wide gamut to the nearest sRGB
   colors for one without gamut_supported, as they are drawn. An image in a
   packed format is written packed to a terminal whose listing has that
   format (packed_formats, see kitty_packed_formats) and as RGB to any other.
   depth_supported: the terminal keeps the depth selector, so a placement is
   made at the depth and phase it has: the terminal's cursor is given them
   first, from cursor_depth, which says what it has and is left saying what
   it has after, for the text drawn next to give its own again. */
void append_kitty_frame( std::string& out, bool initialized, const Framebuffer& last, const Framebuffer& current,
                         bool convert_sixel = false, bool gamut_supported = false, bool* vector_resent = nullptr,
                         bool viewport_supported = false, bool hdr_supported = false, uint32_t packed_formats = 0,
                         bool depth_supported = false, CursorDepth* cursor_depth = nullptr );
/* Whether drawing an image depends on whether the terminal takes the G key
   (gamut) or HDR pixels (hdr): when that changes, it is sent again. */
bool kitty_image_needs_gamut( const KittyImage& image );
bool kitty_image_needs_hdr( const KittyImage& image );

/* Animation frames as far as an image of one frame has them: a=f editing
   the root frame (r=1), and a=c within it. Pixels are straight RGBA, or RGBA
   half floats for an HDR image. blend: the over operator, as kitty blends. */
void kitty_compose( std::string& under, uint32_t under_width, uint32_t under_height, const std::string& over,
                    uint32_t over_width, uint32_t over_height, uint32_t x, uint32_t y, bool blend, bool half );
/* Copies the rectangle w x h at (sx, sy) of an image to (dx, dy) of the same
   image, which must not overlap. */
void kitty_compose_within( std::string& pixels, uint32_t width, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                           uint32_t w, uint32_t h, bool blend, bool half );

/* Kitty's rules for a placement relative to another (P, Q), checked before
   it is made: the reply when it may not be (ENOPARENT, ECYCLE, ETOODEEP, as
   kitty words them), empty when it may. A missing Q is resolved to the
   placement kitty would take, and written to placement.parent_placement. */
std::string kitty_check_parent( const Framebuffer& fb, KittyPlacement& placement );
/* The longest chain of parents a placement may have: a rule of the protocol. */
static const unsigned KITTY_PARENT_DEPTH_LIMIT = 8;
/* The cell a placement starts in, as the terminal lays it out: a relative
   placement's is that of the root of its tree, or of the placeholders of a
   virtual parent, plus the H and V of each link. False when it has none. */
bool kitty_placement_cell( const Framebuffer& fb, const KittyPlacement& placement, int64_t& row, int64_t& col );
/* The whole scene of a vector image, as records. */
std::string kitty_vector_records( const KittyImage& image );

}

#endif
