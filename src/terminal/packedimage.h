/*
    Goblin Skiff: Silkie's packed image formats, kept packed
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef PACKEDIMAGE_HPP
#define PACKEDIMAGE_HPP

#include <cstdint>
#include <string>

/* Silkie's graphics protocol takes grayscale of 1, 2, 4 and 8 bits a pixel
   (f=301, 302, 304, 308) and RGB565 (f=516), see docs/packed-image-protocol.rst in
   its repository. A program chose such a format to spend few bytes, so the
   relay keeps the pixels as they were packed and never expands them on the
   link: image state holds them in a container that is exact, a row filter
   followed by a general compressor. Only what draws an image expands it. */

namespace Terminal {

bool packed_format( uint32_t format );

/* The bytes of a row and of an image as the program sends them: each row
   starts at a byte, leftmost pixel in the high bits; RGB565 is two bytes a
   pixel. False when the dimensions are zero or the size cannot be held. */
bool packed_size( uint32_t format, uint32_t width, uint32_t height, size_t& row, size_t& size );

/* The pixels as Silkie expands them, three bytes a pixel, or four with an
   opaque alpha. False when packed is not the size of the image. */
bool packed_expand( uint32_t format, uint32_t width, uint32_t height, const std::string& packed, bool alpha,
                    std::string& pixels );

/* Whether this build can read and write the zstd form of the container. */
bool packed_zstd_available( void );

/* The container of an image's packed bytes. zstd: the reader takes zstd;
   otherwise only zlib is written. False when packed is not the size of the
   image. */
bool packed_encode( uint32_t format, uint32_t width, uint32_t height, const std::string& packed, bool zstd,
                    std::string& container );
/* The packed bytes again, exactly. False for anything that is not the
   container of an image of this format and size. */
bool packed_decode( uint32_t format, uint32_t width, uint32_t height, const std::string& container,
                    std::string& packed );

}

#endif
