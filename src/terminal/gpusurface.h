/*
    Goblin Skiff: GPU surfaces (t=g) of the graphics protocol
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef GPUSURFACE_HPP
#define GPUSURFACE_HPP

#include <cstdint>
#include <string>
#include <sys/types.h>

/* A program can hand the terminal a GPU surface by name instead of pixels.
   The surface cannot cross to the client's machine, but the server runs
   where the program does and can read it, so it sends the pixels. On macOS
   the name is an IOSurface id; on Linux a dma-buf named by the process that
   holds it and its descriptors, which is read only when it is linear and
   belongs to the program's session. */

namespace Terminal {

/* Whether this platform can read surfaces at all. */
bool gpu_surfaces_supported();
/* The session whose processes may name their dma-bufs: the shell's. */
void set_gpu_surface_session( pid_t session );
/* Reads the surface into straight RGBA. width and height are the s and v
   keys, which a dma-buf needs and an IOSurface ignores. */
bool read_gpu_surface( const std::string& name,
                       uint32_t width,
                       uint32_t height,
                       std::string& rgba,
                       uint32_t& out_width,
                       uint32_t& out_height,
                       std::string& error );

}

#endif
