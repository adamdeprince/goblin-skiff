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
#include <string>
#include <vector>

namespace Terminal {
class Framebuffer;

static const size_t KITTY_MAX_APC_CHARS = 8192;
static const size_t KITTY_CHUNK_B64 = 4096;
// WebP's dimension ceiling is 16383. Keep a separate decoded-pixel limit and
// an aggregate storage budget; a retina-sized canvas can exceed 32 MiB RGBA.
static const uint32_t GRAPHICS_MAX_DIMENSION = 16383;
static const size_t GRAPHICS_MAX_PIXELS = 32 * 1024 * 1024;
static const size_t KITTY_IMAGE_QUOTA = 256 * 1024 * 1024;
// Retained encoded images must fit a recovery snapshot even at the 500-byte
// fallback MTU (15-bit fragment count). This is separate from decoded pixels.
static const size_t KITTY_ENCODED_QUOTA = 12 * 1024 * 1024;
static const uint32_t KITTY_FORMAT_RGB = 24;
static const uint32_t KITTY_FORMAT_RGBA = 32;
static const uint32_t KITTY_FORMAT_PNG = 100;
/* Canonical in-memory and state-sync representation.  This value is never
   emitted to the user's terminal; the public Kitty protocol only defines
   RGB, RGBA, and PNG. */
static const uint32_t KITTY_FORMAT_WEBP = 0x57454250U;

enum KittyAction
{
  KittyTransmit = 't',
  KittyTransmitAndDisplay = 'T',
  KittyQuery = 'q',
  KittyPut = 'p',
  KittyDelete = 'd',
  KittyFrame = 'f',
  KittyAnimate = 'a',
  KittyCompose = 'c'
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
  char delete_target;
  std::string payload;

  KittyCommand();
};

// The stored payload is WebP in either case. Origin is needed because a
// sixel image must prefer native sixel output, even on dual-capable clients.
enum class ImageOrigin { Kitty, Sixel };

struct KittyImage
{
  uint32_t id;
  uint32_t number;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  ImageOrigin origin;
  std::shared_ptr<std::string> data;
  uint64_t serial;

  KittyImage();
  bool operator==( const KittyImage& other ) const;
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
void append_kitty_frame( std::string& out, bool initialized, const Framebuffer& last, const Framebuffer& current,
                         bool convert_sixel = false );

}

#endif
