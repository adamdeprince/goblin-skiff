/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_GRAPHEME_H
#define MOSH_GRAPHEME_H
#include <string>
#include <vector>
namespace Terminal { namespace Unicode16 {
std::vector<std::wstring> graphemes( const std::wstring& input );
unsigned cell_width( const std::wstring& input );
bool invalid( wchar_t c );
} }
#endif
