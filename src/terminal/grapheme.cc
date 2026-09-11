/* Distributed under the GNU GPL, version 3 or later. */
#include "grapheme.h"
#include <cstdint>
#include <iterator>

namespace Terminal { namespace Unicode16 {
namespace {
struct Range { uint32_t first, last; unsigned flags; };
#include "unicode16-data.h"
enum { Other, CR, LF, Control, Extend, ZWJ, RI, Prepend, SpacingMark, L, V, T, LV, LVT };
unsigned properties( uint32_t c )
{
  size_t low = 0, high = sizeof ranges / sizeof ranges[0];
  unsigned result = 0;
  while ( low < high ) {
    const size_t mid = low + ( high - low ) / 2;
    if ( c < ranges[mid].first ) { high = mid; }
    else if ( c > ranges[mid].last ) { low = mid + 1; }
    else { result = ranges[mid].flags; break; }
  }
  if ( c >= 0xac00 && c <= 0xd7a3 ) { result = ( result & ~15U ) | ( ( c - 0xac00 ) % 28 ? LVT : LV ); }
  return result;
}
bool boundary( const std::vector<unsigned>& p, size_t i )
{
  const unsigned a = p[i - 1] & 15, b = p[i] & 15;
  if ( a == CR && b == LF ) { return false; } // GB3
  if ( ( a >= CR && a <= Control ) || ( b >= CR && b <= Control ) ) { return true; }
  if ( a == L && ( b == L || b == V || b == LV || b == LVT ) ) { return false; }
  if ( ( a == LV || a == V ) && ( b == V || b == T ) ) { return false; }
  if ( ( a == LVT || a == T ) && b == T ) { return false; }
  if ( b == Extend || b == ZWJ || b == SpacingMark || a == Prepend ) { return false; }
  if ( p[i] & 512 ) { // GB9c: Indic consonant, linker, consonant
    size_t j = i;
    bool linker = false;
    while ( j && ( p[j - 1] & ( 1024 | 2048 ) ) ) { linker |= bool( p[--j] & 2048 ); }
    if ( linker && j && ( p[j - 1] & 512 ) ) { return false; }
  }
  if ( ( p[i] & 16 ) && a == ZWJ ) { // GB11: extended pictographic ZWJ sequence
    size_t j = i - 1;
    while ( j && ( p[j - 1] & 15 ) == Extend ) { j--; }
    if ( j && ( p[j - 1] & 16 ) ) { return false; }
  }
  if ( a == RI && b == RI ) {
    size_t count = 0, j = i;
    while ( j && ( p[--j] & 15 ) == RI ) { count++; }
    if ( count % 2 ) { return false; }
  }
  return true;
}
}

bool invalid( wchar_t c ) { return uint32_t( c ) > 0x10ffff || ( properties( c ) & 4096 ); }

std::vector<std::wstring> graphemes( const std::wstring& input )
{
  std::vector<std::wstring> result;
  std::vector<unsigned> p;
  p.reserve( input.size() );
  for ( wchar_t c : input ) { p.push_back( properties( c ) ); }
  size_t start = 0;
  for ( size_t i = 1; i <= input.size(); i++ ) {
    if ( i == input.size() || boundary( p, i ) ) { result.push_back( input.substr( start, i - start ) ); start = i; }
  }
  return result;
}

unsigned cell_width( const std::wstring& input )
{
  unsigned width = 0, previous = 0;
  for ( wchar_t c : input ) {
    const unsigned p = properties( c );
    if ( !width ) { width = ( p & 4096 ) ? 0 : ( p & 32 ) ? 2 : ( p & 64 ) ? 0 : 1; }
    if ( c == 0xfe0e && width == 2 && ( previous & 128 ) ) { width = 1; }
    if ( c == 0xfe0f && width == 1 && ( previous & 256 ) ) { width = 2; }
    previous = p;
  }
  return width;
}
} }
