/* Copyright 2026. Distributed under the GNU GPL, version 3 or later. */
#ifndef GOBLIN_MASCOT_HPP
#define GOBLIN_MASCOT_HPP

#include <cstdint>
#include <string>

namespace Mascot {
enum class Format
{
  Auto,
  Kitty,
  Sixel,
  Ascii,
  None
};
Format parse_format( const std::string& name );
Format constrain_format( Format format, bool allow_kitty, bool allow_sixel );

struct Size
{
  unsigned columns, rows, cell_width, cell_height;
  Size( unsigned c = 80, unsigned r = 24, unsigned w = 0, unsigned h = 0 )
    : columns( c ), rows( r ), cell_width( w ), cell_height( h )
  {}
};

std::string asset();
std::string render( Format format, const Size& size );

/* All queries and their replies stay on the local tty. Ordinary input,
   including input interleaved with replies, is returned to the caller. */
class Splash
{
private:
  Format requested, displayed;
  Size size;
  bool visible, kitty_reply, da_reply, geometry_reply, kitty, sixel;
  bool allow_kitty, allow_sixel;
  bool keyboard_reply = false, keyboard = false;
  bool clipboard_reply = false, clipboard = false;
  unsigned sizing_replies = 0, sizing_row = 0, sizing_width_col = 0, text_sizing = 0;
  bool cursor_reply = false, cursor_invalid = false;
  int cursor_row = -1;
  uint64_t cursor_deadline = 0;
  std::string pending;
  uint64_t pending_since, probe_deadline;
  std::string query_cursor( uint64_t now );
  bool consume_reply();
  bool possible_reply() const;

public:
  explicit Splash( Format format = Format::Auto, bool kitty_supported = true, bool sixel_supported = true );
  std::string start( const Size& dimensions, bool interactive, uint64_t now = 0 );
  std::string paint( uint64_t now = 0, bool finish = false );
  std::string dismiss();
  bool active() const { return visible; }
  bool painted() const { return displayed != Format::None; }
  Format selected() const;
  std::string filter( const std::string& input, uint64_t now );
  std::string flush( uint64_t now );
  int wait_time( uint64_t now ) const;
  // Zero-based physical row immediately after our last startup output. A
  // bounded, local DEC CPR keeps this distinct from the OSC 66 probe replies.
  bool cursor_ready( uint64_t now ) const { return !cursor_reply || now >= cursor_deadline; }
  int start_row() const { return cursor_invalid ? -1 : cursor_row; }
  void invalidate_cursor() { cursor_invalid = true; }
  // Attachment detection is independent of whether the mascot is enabled or
  // forced to a particular format. Late replies are still filtered locally.
  bool probe_ready( uint64_t now ) const { return now >= probe_deadline || !( kitty_reply || da_reply || geometry_reply || keyboard_reply || clipboard_reply || sizing_replies ); }
  bool supports_kitty() const { return allow_kitty && kitty; }
  bool supports_sixel() const { return allow_sixel && sixel; }
  bool supports_keyboard() const { return keyboard; }
  bool supports_clipboard() const { return clipboard; }
  unsigned supports_text_sizing() const { return text_sizing; }
  const Size& dimensions() const { return size; }
};
}
#endif
