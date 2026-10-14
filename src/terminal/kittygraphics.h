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

struct ImageEncoding
{
  int quality = -1; // -1: lossless WebP; 0..100: lossy WebP
  bool palette_djvu = false; // enabled only after peer capability negotiation
  bool djvu_lossy = false; // permit cjb2 symbol substitution for bilevel images
  bool png = false; // png-state-v1: lossless fallback beyond WebP dimensions
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
  KittyEdit = 'e',   /* a patch to a vector image */
  KittyRefresh = 'r' /* copy a GPU surface again */
};

struct KittyCommand
{
  char action;
  char medium;
  char compression;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t data_size;
  uint32_t data_offset;
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
  uint32_t tone_map; /* T: compress what is brighter than the headroom */
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
  std::string surface;                        // the GPU surface it copies (server only)
  uint32_t surface_gamut = 0;                 // the gamut its pixels came in (server only)
  bool png_original = false;                  // a PNG with a color profile, passed on as it came
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

  KittyPlacement();
  bool operator==( const KittyPlacement& other ) const;
};

bool parse_kitty_command( const std::string& apc, KittyCommand& cmd );
std::string encode_kitty_chunks( const std::string& controls, const std::string& binary );
std::string kitty_response( uint32_t image_id, uint32_t placement_id, const std::string& message );
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
/* The changed rectangle between two images of one size: false when nothing
   changed. */
bool kitty_dirty_rect( const std::string& before, const std::string& after, uint32_t width, uint32_t height,
                       uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h );
/* vector_resent: set when a vector image is sent whole, which ends the
   strokes of ink the terminal is drawing into it. */
void append_kitty_frame( std::string& out, bool initialized, const Framebuffer& last, const Framebuffer& current,
                         bool convert_sixel = false, bool gamut_supported = false, bool* vector_resent = nullptr );
/* The whole scene of a vector image, as records. */
std::string kitty_vector_records( const KittyImage& image );

}

#endif
