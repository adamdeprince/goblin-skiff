/* Copyright 2026. Distributed under the GNU GPL, version 3 or later. */
#include "mascot.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <zlib.h>

#include "src/frontend/goblin-webp.inc"
#include "src/terminal/kittygraphics.h"

namespace Mascot {
namespace {
const std::string query_prefix = "\033_Gi=4294967294;";

struct Pixels
{
  std::string rgba;
  uint32_t width, height;
  Pixels() : rgba(), width( 0 ), height( 0 )
  {
    if ( !Terminal::kitty_webp_to_rgba( asset(), rgba, width, height ) ) {
      throw std::runtime_error( "Cannot decode bundled goblin" );
    }
  }

  unsigned gray( unsigned x, unsigned y ) const
  {
    const unsigned char* pixel = reinterpret_cast<const unsigned char*>( rgba.data() ) + 4 * ( y * width + x );
    const unsigned luminance = ( 77 * pixel[0] + 150 * pixel[1] + 29 * pixel[2] ) / 256;
    return ( luminance * pixel[3] + 255 * ( 255 - pixel[3] ) ) / 255;
  }

  std::vector<unsigned> scaled( unsigned w, unsigned h ) const
  {
    std::vector<unsigned> out( w * h );
    for ( unsigned y = 0; y < h; y++ ) {
      for ( unsigned x = 0; x < w; x++ ) {
        unsigned sum = 0, count = 0;
        for ( unsigned sy = y * height / h; sy < std::max( y * height / h + 1, ( y + 1 ) * height / h ); sy++ ) {
          for ( unsigned sx = x * width / w; sx < std::max( x * width / w + 1, ( x + 1 ) * width / w ); sx++ ) {
            sum += gray( sx, sy );
            count++;
          }
        }
        out[y * w + x] = sum / count;
      }
    }
    return out;
  }
};

void run_length( std::string& out, char value, unsigned count )
{
  if ( count > 3 ) {
    out += "!" + std::to_string( count ) + value;
  } else {
    out.append( count, value );
  }
}

std::string sixel_image( const Pixels& image, unsigned side )
{
  auto pixels = image.scaled( side, side );
  for ( unsigned& pixel : pixels ) {
    pixel = ( pixel * 15 + 127 ) / 255;
  }
  // Square pixels, opaque background, private per-image palette definitions.
  std::string out = "\033P0;0;0q\"1;1;" + std::to_string( side ) + ";" + std::to_string( side );
  for ( unsigned color = 0; color < 16; color++ ) {
    const std::string intensity = std::to_string( ( color * 100 + 7 ) / 15 );
    out += "#" + std::to_string( color ) + ";2;" + intensity + ";" + intensity + ";" + intensity;
  }
  for ( unsigned y = 0; y < side; y += 6 ) {
    for ( unsigned color = 0; color < 16; color++ ) {
      out += "#" + std::to_string( color );
      char previous = 0;
      unsigned count = 0;
      for ( unsigned x = 0; x < side; x++ ) {
        unsigned bits = 0;
        for ( unsigned dy = 0; dy < 6 && y + dy < side; dy++ ) {
          if ( pixels[( y + dy ) * side + x] == color ) {
            bits |= 1U << dy;
          }
        }
        const char value = static_cast<char>( 63 + bits );
        if ( count && previous != value ) {
          run_length( out, previous, count );
          count = 0;
        }
        previous = value;
        count++;
      }
      run_length( out, previous, count );
      out += '$';
    }
    if ( y + 6 < side ) {
      out += '-';
    }
  }
  return out + "\033\\";
}

bool prefix_of( const std::string& a, const std::string& b )
{
  return a.size() <= b.size() && b.compare( 0, a.size(), a ) == 0;
}

std::vector<unsigned> numbers( const std::string& text )
{
  std::vector<unsigned> result( 1, 0 );
  for ( char c : text ) {
    if ( c == ';' ) {
      result.push_back( 0 );
    } else if ( c >= '0' && c <= '9' && result.back() <= 10000 ) {
      result.back() = result.back() * 10 + c - '0';
    } else {
      return {};
    }
  }
  return result;
}
}

Format parse_format( const std::string& name )
{
  if ( name.empty() || name == "auto" ) {
    return Format::Auto;
  }
  if ( name == "kitty" ) {
    return Format::Kitty;
  }
  if ( name == "sixel" ) {
    return Format::Sixel;
  }
  if ( name == "ascii" ) {
    return Format::Ascii;
  }
  if ( name == "none" ) {
    return Format::None;
  }
  throw std::invalid_argument( "--mascot must be auto, kitty, sixel, ascii, or none" );
}

Format constrain_format( Format format, bool allow_kitty, bool allow_sixel )
{
  if ( ( format == Format::Kitty && !allow_kitty ) || ( format == Format::Sixel && !allow_sixel ) ) {
    return Format::Ascii;
  }
  return format;
}

std::string asset()
{
  return std::string( reinterpret_cast<const char*>( goblin_webp ), sizeof goblin_webp );
}

std::string render( Format format, const Size& size )
{
  if ( format == Format::None || size.columns < 8 || size.rows < 6 ) {
    return {};
  }
  const Pixels image;
  unsigned cols = std::min( format == Format::Kitty ? 16U : format == Format::Sixel ? 20U : 32U, size.columns - 1 );
  unsigned rows = std::min( cols / 2, size.rows - 3 );
  cols = rows * 2;
  std::string out;
  if ( format == Format::Kitty ) {
    // Deflate applies only to the local terminal payload; the stored artifact
    // is lossy WebP and is never put in the mosh transport state.
    std::string compressed( compressBound( image.rgba.size() ), '\0' );
    uLongf length = compressed.size();
    if ( compress2( reinterpret_cast<Bytef*>( &compressed[0] ),
                    &length,
                    reinterpret_cast<const Bytef*>( image.rgba.data() ),
                    image.rgba.size(),
                    6 )
         != Z_OK ) {
      throw std::runtime_error( "Cannot compress goblin terminal image" );
    }
    compressed.resize( length );
    // Anonymous images do not replace earlier mascots (or remote images)
    // with the same id when several sessions share the local scrollback.
    out = Terminal::encode_kitty_chunks( "a=T,t=d,f=32,o=z,s=" + std::to_string( image.width ) + ",v="
                                           + std::to_string( image.height ) + ",q=2,C=1,c=" + std::to_string( cols )
                                           + ",r=" + std::to_string( rows ),
                                         compressed );
  } else if ( format == Format::Sixel ) {
    const unsigned cw = size.cell_width ? size.cell_width : 8;
    const unsigned ch = size.cell_height ? size.cell_height : 16;
    const unsigned side = std::min( 512U, std::min( cols * cw, rows * ch ) );
    out = sixel_image( image, side );
  } else {
    const auto pixels = image.scaled( cols, rows );
    static const char ramp[] = "@%#*+=-:. ";
    for ( unsigned y = 0; y < rows; y++ ) {
      for ( unsigned x = 0; x < cols; x++ ) {
        out += ramp[( pixels[y * cols + x] * 9 + 127 ) / 255];
      }
      out += "\r\n";
    }
    return out;
  }
  // Reserve real lines before placing graphics. This works at any cursor
  // position, including the bottom of the screen, without erasing history.
  // Save/restore also isolates terminals' different sixel cursor semantics.
  std::string space;
  for ( unsigned row = 0; row < rows; row++ ) {
    space += "\r\n";
  }
  return space + "\033[" + std::to_string( rows ) + "A\r\0337" + out + "\0338\033[" + std::to_string( rows )
         + "B\r";
}

Splash::Splash( Format format, bool kitty_supported, bool sixel_supported )
  : requested( constrain_format( format, kitty_supported, sixel_supported ) ), displayed( Format::None ), size(),
    visible( false ), kitty_reply( false ), da_reply( false ), geometry_reply( false ), kitty( false ),
    sixel( false ), allow_kitty( kitty_supported ), allow_sixel( sixel_supported ), pending(), pending_since( 0 ),
    probe_deadline( 0 )
{}

std::string Splash::start( const Size& dimensions, bool interactive, uint64_t now )
{
  size = dimensions;
  probe_deadline = interactive ? now + 250 : now;
  visible = interactive && requested != Format::None && size.columns >= 8 && size.rows >= 6;
  if ( !interactive ) {
    return {};
  }
  std::string queries;
  keyboard_reply = true;
  queries += "\033[?u";
  clipboard_reply = true;
  queries += "\033[?5522$p";
  {
    kitty_reply = allow_kitty;
    da_reply = allow_sixel;
    if ( kitty_reply ) {
      queries += "\033_Gi=4294967294,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";
    }
    if ( da_reply ) {
      queries += "\033[c";
    }
  }
  if ( ( allow_sixel || allow_kitty ) && ( !size.cell_width || !size.cell_height ) ) {
    geometry_reply = true;
    queries += "\033[16t";
  }
  if ( size.columns >= 6 && size.rows >= 2 ) {
    // OSC 66 has no query opcode. Test cursor advances in two freshly
    // allocated blank rows, then erase only our own probe and leave history
    // untouched. CPR replies are consumed here, never sent over the link.
    sizing_replies = 3;
    queries += "\r\n\r\n\033[1A\r\033[6n\033]66;w=2; \033\\\033[6n"
               "\033]66;s=2; \033\\\033[6n\r\033[2K\033[1B\r\033[2K";
  }
  if ( !visible ) { queries += query_cursor( now ); }
  return queries;
}

std::string Splash::query_cursor( uint64_t now )
{
  if ( cursor_invalid ) { return {}; }
  cursor_reply = true;
  cursor_deadline = now + 250;
  return "\033[?6n";
}

Format Splash::selected() const
{
  if ( requested != Format::Auto ) {
    return requested;
  }
  return allow_kitty && kitty ? Format::Kitty : allow_sixel && sixel ? Format::Sixel : Format::Ascii;
}

std::string Splash::paint( uint64_t now, bool finish )
{
  if ( !visible || painted() ) {
    return {};
  }
  // Draw once, after a bounded local probe, instead of flashing ASCII and
  // clearing/replacing it when a graphics reply arrives. Late replies are
  // still consumed but cannot redraw a banner already in scrollback.
  if ( !finish && now < probe_deadline && ( kitty_reply || da_reply || geometry_reply ) ) {
    return {};
  }
  displayed = selected();
  return "\033[0m\r\n" + render( displayed, size ) + "Goblin Skiff\r\n"
         + ( finish ? std::string() : query_cursor( now ) );
}

std::string Splash::dismiss()
{
  if ( !visible ) {
    return {};
  }
  visible = false;
  // The banner belongs to local scrollback now, not a temporary overlay.
  return {};
}

bool Splash::consume_reply()
{
  if ( cursor_reply && pending.compare( 0, 3, "\033[?" ) == 0 && pending.back() == 'R' ) {
    const auto params = numbers( pending.substr( 3, pending.size() - 4 ) );
    if ( ( params.size() != 2 && params.size() != 3 ) || !params[0] || params[0] > size.rows
         || !params[1] || params[1] > size.columns || ( params.size() == 3 && params[2] != 1 ) ) {
      return false;
    }
    // Our banner ends at column one. Do not trust a report if something else
    // moved the cursor, or if a resize/resume invalidated the physical layout.
    cursor_row = params[1] == 1 ? int( params[0] ) - 1 : -1;
    cursor_reply = false;
    return true;
  }
  if ( clipboard_reply && pending.compare( 0, 8, "\033[?5522;" ) == 0
       && pending.size() == 11 && pending.substr( 9 ) == "$y" ) {
    clipboard = pending[8] >= '1' && pending[8] <= '3';
    clipboard_reply = false;
    return true;
  }
  if ( sizing_replies && pending.compare( 0, 2, "\033[" ) == 0 && pending.back() == 'R' ) {
    const auto params = numbers( pending.substr( 2, pending.size() - 3 ) );
    if ( params.size() != 2 || !params[0] || params[0] > size.rows || !params[1] || params[1] > size.columns ) {
      return false;
    }
    if ( sizing_replies == 3 ) { sizing_row = params[1] == 1 ? params[0] : 0; }
    if ( params[0] != sizing_row ) { sizing_row = 0; }
    if ( sizing_replies == 2 ) { sizing_width_col = params[1]; }
    if ( sizing_replies == 1 && sizing_row ) {
      text_sizing = sizing_width_col == 3 ? ( params[1] == 5 ? 2 : 1 ) : 0;
    }
    sizing_replies--;
    return true;
  }
  if ( keyboard_reply && pending.compare( 0, 3, "\033[?" ) == 0 && pending.back() == 'u' ) {
    const auto params = numbers( pending.substr( 3, pending.size() - 4 ) );
    if ( params.size() == 1 ) { keyboard = true; keyboard_reply = false; return true; }
  }
  if ( kitty_reply && pending.compare( 0, query_prefix.size(), query_prefix ) == 0
       && pending.size() >= query_prefix.size() + 2 && pending.substr( pending.size() - 2 ) == "\033\\" ) {
    kitty = pending.substr( query_prefix.size(), pending.size() - query_prefix.size() - 2 ) == "OK";
    kitty_reply = false;
    return true;
  }
  if ( da_reply && pending.compare( 0, 3, "\033[?" ) == 0 && pending.back() == 'c' ) {
    const auto params = numbers( pending.substr( 3, pending.size() - 4 ) );
    if ( params.empty() ) {
      return false;
    }
    sixel = std::find( params.begin() + 1, params.end(), 4 ) != params.end();
    da_reply = false;
    return true;
  }
  if ( geometry_reply && pending.compare( 0, 4, "\033[6;" ) == 0 && pending.back() == 't' ) {
    const auto params = numbers( pending.substr( 2, pending.size() - 3 ) );
    if ( params.size() != 3 || params[1] == 0 || params[2] == 0 || params[1] > 1024 || params[2] > 1024 ) {
      return false;
    }
    size.cell_height = params[1];
    size.cell_width = params[2];
    geometry_reply = false;
    return true;
  }
  return false;
}

bool Splash::possible_reply() const
{
  if ( pending.size() > 256 ) {
    return false;
  }
  if ( clipboard_reply && ( prefix_of( pending, "\033[?5522;" )
       || ( prefix_of( "\033[?5522;", pending ) && pending.find_first_not_of( "01234$", 8 ) == std::string::npos ) ) ) { return true; }
  if ( kitty_reply && ( prefix_of( pending, query_prefix ) || prefix_of( query_prefix, pending ) ) ) {
    return true;
  }
  if ( sizing_replies && ( prefix_of( pending, "\033[" )
       || ( prefix_of( "\033[", pending ) && pending.find_first_not_of( "0123456789;", 2 ) == std::string::npos ) ) ) {
    return true;
  }
  for ( const std::string& prefix : { std::string( "\033[?" ), std::string( "\033[6;" ) } ) {
    if ( prefix[2] == '?' ? !( da_reply || keyboard_reply || cursor_reply ) : !geometry_reply ) {
      continue;
    }
    if ( prefix_of( pending, prefix ) ) {
      return true;
    }
    if ( prefix_of( prefix, pending )
         && pending.find_first_not_of( "0123456789;", prefix.size() ) == std::string::npos ) {
      return true;
    }
  }
  return false;
}

std::string Splash::filter( const std::string& input, uint64_t now )
{
  std::string output;
  for ( char c : input ) {
    if ( pending.empty() ) {
      pending_since = now;
    }
    pending += c;
    if ( consume_reply() ) {
      pending.clear();
      continue;
    }
    while ( !pending.empty() && !possible_reply() ) {
      output += pending[0];
      pending.erase( 0, 1 );
    }
  }
  return output;
}

std::string Splash::flush( uint64_t now )
{
  if ( pending.empty() || now < pending_since + 50 ) {
    return {};
  }
  std::string result;
  result.swap( pending );
  return result;
}

int Splash::wait_time( uint64_t now ) const
{
  int wait = INT_MAX;
  if ( !probe_ready( now ) ) {
    wait = static_cast<int>( probe_deadline - now );
  }
  if ( visible && !painted() ) {
    wait = now >= probe_deadline ? 0 : static_cast<int>( probe_deadline - now );
  }
  if ( !cursor_ready( now ) ) {
    wait = std::min( wait, static_cast<int>( cursor_deadline - now ) );
  }
  if ( pending.empty() ) {
    return wait;
  }
  return std::min( wait, now >= pending_since + 50 ? 0 : static_cast<int>( pending_since + 50 - now ) );
}
}
