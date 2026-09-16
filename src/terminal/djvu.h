/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_DJVU_HPP
#define MOSH_DJVU_HPP

#include <cstdint>
#include <string>

namespace Terminal {
// The DjVu page contains vertically stacked palette-index bitplanes, least
// significant bit first. Black pixels are set bits. Even a one-color image
// has one plane. The palette contains exact, unpremultiplied RGBA bytes.
// On encoder failure a nonempty palette means it was eligible for DjVu;
// callers must use a lossless fallback so WebP's lossy flag stays independent.
bool encode_palette_djvu( const unsigned char* pixels, uint32_t width, uint32_t height, bool alpha, bool lossy,
                          std::string& palette, std::string& djvu );
bool decode_palette_djvu( const std::string& palette, const std::string& djvu,
                          uint32_t width, uint32_t height, std::string& rgba );
}

#endif
