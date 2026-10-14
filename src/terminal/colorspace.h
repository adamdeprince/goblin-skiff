/*
    Goblin Skiff: wide gamut and HDR colors for terminals without them
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef COLORSPACE_HPP
#define COLORSPACE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

/* A terminal that draws in sRGB shows a Display P3 or Rec. 2020 color as the
   nearest sRGB color, and an HDR image at SDR. When the client's terminal
   cannot be given the color as it is, Skiff converts it the same way. */

namespace Terminal {

/* Gamut ids as the SGR and the G key write them: 1 Display P3, 2 Rec. 2020.
   Components are encoded with the sRGB curve in every gamut. */
void gamut_to_srgb( unsigned gamut, uint8_t& red, uint8_t& green, uint8_t& blue );
/* Pixels of 3 or 4 bytes, alpha untouched. */
void gamut_pixels_to_srgb( unsigned gamut, std::string& pixels, size_t bytes_per_pixel );
/* RGBA half floats in linear extended sRGB (f=64) to 8 bit sRGB RGBA, with
   the brightness above SDR white clipped or, with tone_map, compressed as
   the HDR protocol says for a headroom of 1. */
bool hdr_to_srgb8( const std::string& half_floats, uint32_t width, uint32_t height, bool tone_map, std::string& rgba );

}

#endif
