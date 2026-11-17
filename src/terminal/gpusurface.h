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
#include <memory>
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
/* A surface the relay holds: the dma-buf descriptors it took from the
   program, or a reference to the IOSurface. It is taken once, when the image
   is transmitted, and read again by every a=r, so a program that closes its
   own descriptors, or lets go of its surface, still has its image refreshed
   from the buffer it named. It is let go when the image is deleted. Never
   synchronized: it means nothing on the client's machine. */
class GpuSurface
{
public:
  struct Impl;
  explicit GpuSurface( Impl* held );
  ~GpuSurface();
  GpuSurface( const GpuSurface& ) = delete;
  GpuSurface& operator=( const GpuSurface& ) = delete;
  /* Lets go of the buffer now, before the last reference to this goes. */
  void release();
  bool held() const { return impl != nullptr; }
  Impl* get() const { return impl; }

private:
  Impl* impl;
};

/* Takes hold of the surface a t=g command names. width and height are the
   s and v keys, which a dma-buf needs and an IOSurface ignores. */
std::shared_ptr<GpuSurface> open_gpu_surface( const std::string& name, uint32_t width, uint32_t height,
                                              std::string& error );
/* Reads a surface into straight RGBA. */
bool read_gpu_surface( const GpuSurface& surface,
                       std::string& rgba,
                       uint32_t& out_width,
                       uint32_t& out_height,
                       std::string& error );
/* Takes hold of a surface and reads it once. */
bool read_gpu_surface( const std::string& name,
                       uint32_t width,
                       uint32_t height,
                       std::string& rgba,
                       uint32_t& out_width,
                       uint32_t& out_height,
                       std::string& error );

}

#endif
