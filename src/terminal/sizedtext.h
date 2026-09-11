/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_SIZED_TEXT_H
#define MOSH_SIZED_TEXT_H
#include <string>
#include <vector>

namespace Terminal {
class Framebuffer;
struct SizedText
{
  unsigned scale = 1, width = 1, numerator = 0, denominator = 0, vertical = 0, horizontal = 0;
  std::string text {}, fallback {};
  unsigned columns() const { return scale * width; }
  std::string sequence() const;
  void update_fallback();
  bool operator==( const SizedText& other ) const
  {
    return scale == other.scale && width == other.width && numerator == other.numerator
           && denominator == other.denominator && vertical == other.vertical && horizontal == other.horizontal
           && text == other.text;
  }
};
void parse_sized_text( const std::vector<wchar_t>& osc, Framebuffer& fb );
}
#endif
