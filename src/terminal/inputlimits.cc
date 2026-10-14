#include "src/terminal/inputlimits.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>

using namespace Terminal;

size_t Terminal::csi_byte_limit = 0;
size_t Terminal::escape_byte_limit = 0;
unsigned Terminal::vector_depth_limit = 0;

static bool parse_size( const char* text, unsigned long long& out )
{
  if ( !text || !*text ) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long n = strtoull( text, &end, 10 );
  if ( errno || !end || *end ) {
    return false;
  }
  out = n;
  return true;
}

static void from_env( const char* name, size_t& dest )
{
  const char* text = getenv( name );
  unsigned long long n = 0;
  if ( !text || !*text ) {
    return;
  }
  if ( !parse_size( text, n ) ) {
    std::cerr << name << " is not a non-negative integer, ignoring it\n";
    return;
  }
  dest = static_cast<size_t>( n );
}

void Terminal::load_input_limits_from_env( void )
{
  from_env( "MOSH_CSI_LIMIT", csi_byte_limit );
  from_env( "MOSH_ESCAPE_LIMIT", escape_byte_limit );
  size_t depth = vector_depth_limit;
  from_env( "MOSH_VECTOR_DEPTH_LIMIT", depth );
  vector_depth_limit = static_cast<unsigned>( depth );
}
