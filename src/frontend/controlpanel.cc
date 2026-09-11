/* Distributed under the GNU GPL, version 3 or later. */
#include "controlpanel.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace Control {
bool decode_key_event( const std::string& input, unsigned& code, unsigned& mods, unsigned& event )
{
  if ( input.size() < 4 || input.compare( 0, 2, "\033[" ) || input.back() != 'u' ) { return false; }
  size_t pos = 2;
  const auto number = [&]( unsigned& value ) {
    const size_t start = pos;
    value = 0;
    while ( pos < input.size() && input[pos] >= '0' && input[pos] <= '9' ) {
      if ( value > 1000000 ) { return false; }
      value = value * 10 + unsigned( input[pos++] - '0' );
    }
    return pos > start;
  };
  if ( !number( code ) ) { return false; }
  // Skip optional alternate key codes; the first code identifies the key.
  unsigned alternates = 0;
  while ( pos < input.size() && input[pos] == ':' ) {
    if ( ++alternates > 2 ) { return false; }
    pos++;
    unsigned ignored = 0;
    // Kitty can omit the shifted code while including the base-layout key.
    if ( pos < input.size() && input[pos] >= '0' && input[pos] <= '9' && !number( ignored ) ) { return false; }
  }
  mods = event = 1;
  if ( pos < input.size() && input[pos] == ';' ) {
    pos++;
    if ( pos < input.size() && input[pos] >= '0' && input[pos] <= '9' && !number( mods ) ) { return false; }
    if ( pos < input.size() && input[pos] == ':' ) {
      pos++;
      if ( !number( event ) ) { return false; }
    }
  }
  return pos < input.size() && ( input[pos] == ';' || input[pos] == 'u' ) && mods && event >= 1 && event <= 3;
}

int key_event_byte( unsigned code, unsigned modifiers )
{
  if ( !modifiers || code >= 128 ) { return -1; }
  const unsigned mods = modifiers - 1;
  if ( mods & ~( 1U | 4U | 64U | 128U ) ) { return -1; }
  if ( mods & 4 ) {
    if ( code >= 64 && code < 127 ) { return code & 31; }
    if ( code == ' ' || code == '2' ) { return 0; }
    if ( code >= '3' && code <= '7' ) { return code - '3' + 27; }
    if ( code == '8' ) { return 127; }
  }
  if ( mods & 1 ) {
    if ( code >= 'a' && code <= 'z' ) { return code - 'a' + 'A'; }
    const std::string plain = "`1234567890-=[]\\;',./", shifted = "~!@#$%^&*()_+{}|:\"<>?";
    const size_t at = plain.find( char( code ) );
    if ( at != std::string::npos ) { return shifted[at]; }
  }
  return code;
}

namespace {
std::string panel_key( const std::string& key )
{
  if ( key == "\033[Z" ) { return "\t"; } // Legacy Shift-Tab.
  if ( key.size() == 3 && key.compare( 0, 2, "\033O" ) == 0
       && std::string( "ABCDH" ).find( key.back() ) != std::string::npos ) {
    return std::string( "\033[" ) + key.back();
  }
  if ( key.size() < 4 || key.compare( 0, 2, "\033[" ) ) { return key; }
  const char final = key.back();
  if ( std::string( "ABCDHZ~" ).find( final ) == std::string::npos ) { return key; }
  // Kitty keeps arrows and function keys in CSI letter/tilde form while
  // adding modifiers and event types. Normalize only inside the popup,
  // never in application input or Mosh's command-prefix detection.
  unsigned code = 0, mods = 1, event = 1;
  if ( !decode_key_event( key.substr( 0, key.size() - 1 ) + "u", code, mods, event )
       || event == 3 || ( ( mods - 1 ) & ~( 64U | 128U | ( final == 'Z' ? 1U : 0U ) ) ) ) { return {}; }
  if ( final == '~' ) {
    if ( code == 15 && event == 2 ) { return {}; } // Held F5 must not queue repeated copies.
    return code == 5 || code == 6 || code == 15 ? "\033[" + std::to_string( code ) + "~" : "";
  }
  if ( code != 1 ) { return {}; }
  return final == 'Z' ? "\t" : std::string( "\033[" ) + final;
}

// Names are arbitrary filesystem bytes, never trusted terminal commands.
// Escaping non-ASCII bytes also prevents invalid UTF-8 / bidi spoofing.
std::string safe( const std::string& input )
{
  std::string output;
  for ( unsigned char c : input ) {
    if ( c >= 32 && c <= 126 && c != '\\' ) {
      output += char( c );
    } else {
      char escaped[5];
      snprintf( escaped, sizeof escaped, "\\x%02x", c );
      output += escaped;
    }
  }
  return output;
}

void line( Terminal::Framebuffer& fb, int row, int col, int width, const std::string& text, bool selected = false )
{
  Terminal::Renditions renditions( 4 );
  renditions.set_foreground_color( selected ? 16 : 255 );
  renditions.set_background_color( selected ? 117 : 17 );
  for ( int i = 0; i < width; i++ ) {
    auto* cell = fb.get_mutable_cell( row, col + i );
    cell->reset( 4 );
    cell->set_renditions( renditions );
    cell->append( i < int( text.size() ) ? text[i] : ' ' );
  }
}

void glyph( Terminal::Framebuffer& fb, int row, int col, wchar_t value )
{
  line( fb, row, col, 1, "" );
  auto* cell = fb.get_mutable_cell( row, col );
  cell->reset( 4 );
  Terminal::Renditions colors( 4 );
  colors.set_foreground_color( 117 ); colors.set_background_color( 17 );
  cell->set_renditions( colors ); cell->append( value );
}

void box( Terminal::Framebuffer& fb, int row, int col, int width, int height )
{
  for ( int x = 1; x < width - 1; ++x ) {
    glyph( fb, row, col + x, L'─' ); glyph( fb, row + height - 1, col + x, L'─' );
  }
  for ( int y = 1; y < height - 1; ++y ) {
    glyph( fb, row + y, col, L'│' ); glyph( fb, row + y, col + width - 1, L'│' );
  }
  glyph( fb, row, col, L'╭' ); glyph( fb, row, col + width - 1, L'╮' );
  glyph( fb, row + height - 1, col, L'╰' ); glyph( fb, row + height - 1, col + width - 1, L'╯' );
}

std::string size_label( uint64_t bytes )
{
  char text[32];
  if ( bytes < 1024 ) { return std::to_string( bytes ); }
  const char* units[] = { "K", "M", "G", "T", "P", "E" };
  double size = bytes;
  for ( const auto* unit : units ) {
    size /= 1024;
    if ( size < 1024 || unit == units[5] ) { snprintf( text, sizeof text, "%.1f%s", size, unit ); return text; }
  }
  return {};
}
}

void Panel::request( unsigned side, Message message, uint64_t now )
{
  Pane& pane = panes[side];
  message.set_request( ++serial );
  pane.request = serial;
  pane.loading = true;
  pane.opening = message.kind() == Message::OPEN;
  message.set_live( side == 0 || remote_live );
  pane.error.clear();
  if ( side == 0 ) {
    local.request( message, now );
  } else if ( remote_enabled ) {
    pending_request = message;
    remote_pending = true;
  } else {
    pane.loading = false;
    pane.error = "Remote server has no directory-v1 support";
  }
}

void Panel::open( unsigned side, const std::string& path, uint64_t now )
{
  Message message;
  message.set_kind( Message::OPEN );
  message.set_path( path );
  panes[side].prepend = panes[side].advance = false;
  request( side, message, now );
}

void Panel::fetch( unsigned side, uint64_t cursor, bool backwards, bool advance, uint64_t now )
{
  auto& pane = panes[side];
  if ( pane.loading ) {
    if ( !pane.opening && pane.prepend == backwards ) { pane.advance = pane.advance || advance; }
    return;
  }
  Message message;
  message.set_kind( Message::PAGE );
  message.set_path( pane.path );
  message.set_cursor( cursor );
  if ( !pane.pages.empty() ) { message.set_revision( pane.pages[pane.page].listing.revision() ); }
  pane.prepend = backwards;
  pane.advance = advance;
  request( side, message, now );
}

void Panel::move( unsigned side, int amount, uint64_t now )
{
  auto& pane = panes[side];
  while ( amount && !pane.pages.empty() ) {
    auto& page = pane.pages[pane.page];
    if ( amount > 0 ) {
      if ( page.selected + 1 < size_t( page.listing.entry_size() ) ) { ++page.selected; }
      else if ( pane.page + 1 < pane.pages.size() ) { pane.pages[++pane.page].selected = 0; }
      else {
        if ( !page.listing.eof() ) { fetch( side, page.listing.cursor(), false, true, now ); }
        break;
      }
      --amount;
    } else {
      if ( page.selected ) { --page.selected; }
      else if ( pane.page ) {
        auto& previous = pane.pages[--pane.page];
        previous.selected = std::max( 0, previous.listing.entry_size() - 1 );
      } else {
        if ( page.listing.has_previous_cursor() ) { fetch( side, page.listing.previous_cursor(), true, true, now ); }
        break;
      }
      ++amount;
    }
    // A late window may still be useful in the cache, but must not move
    // selection back to an edge the user has already moved away from.
    pane.advance = false;
    keep_visible( pane );
  }
}

void Panel::keep_visible( const Pane& pane ) const
{
  if ( pane.pages.empty() ) { pane.scroll_top = 0; return; }
  size_t selected = pane.pages[pane.page].selected;
  for ( size_t i = 0; i < pane.page; ++i ) { selected += pane.pages[i].listing.entry_size(); }
  // Reversing direction moves the highlight immediately. The list moves
  // only when selection crosses the viewport's top or bottom edge.
  if ( selected < pane.scroll_top ) { pane.scroll_top = selected; }
  else if ( selected - pane.scroll_top >= size_t( viewport_rows ) ) {
    pane.scroll_top = selected - viewport_rows + 1;
  }
}

void Panel::toggle( uint64_t now )
{
  visible = !visible;
  if ( visible && download.token ) {
    download_view = true;
    download_popup_pending = false;
    download_reviewed = download_save_selected = download_save_reviewed = false;
  }
  if ( visible && !initialized && !download_view ) {
    initialized = true;
    open( 0, ".", now );
    open( 1, ".", now );
  }
}

void Panel::offer_download( uint64_t token, const std::string& name, uint64_t bytes, const std::string& directory, bool popup )
{
  if ( download.token == token ) { return; }
  download.token = token; download.name = name; download.bytes = bytes; download.directory = directory;
  download_reviewed = download_save_selected = download_save_reviewed = false;
  download_popup_pending = token && popup;
  if ( !token ) { if ( download_view ) { visible = false; } download_view = false; }
}

bool Panel::take_download_decision( uint64_t& token, bool& allow )
{
  if ( !decided_download ) { return false; }
  token = decided_download; allow = download_allowed; decided_download = 0; return true;
}

void Panel::accept( unsigned side, const Message& message )
{
  Pane& pane = panes[side];
  if ( message.kind() != Message::REPLY || message.request() != pane.request || ( !pane.loading && !message.update() ) ) {
    return;
  }
  if ( !message.update() ) { pane.loading = false; }
  if ( !message.error().empty() ) {
    pane.error = message.error();
    return;
  }
  if ( message.entry_size() > int( PAGE_ENTRIES ) || message.path().size() > 3072 ) {
    pane.error = "Invalid directory page";
    return;
  }
  for ( const auto& entry : message.entry() ) {
    if ( entry.name().empty() || entry.name() == "." || entry.name() == ".."
         || entry.name().find( '/' ) != std::string::npos || entry.name().find( '\0' ) != std::string::npos ) {
      pane.error = "Invalid directory entry";
      return;
    }
  }
  if ( message.delta() ) {
    for ( auto& page : pane.pages ) {
      if ( page.listing.start_cursor() != message.start_cursor() || page.listing.revision() != message.revision() ) { continue; }
      for ( const auto& changed : message.entry() ) {
        for ( auto& entry : *page.listing.mutable_entry() ) {
          if ( entry.name() == changed.name() ) { entry = changed; break; }
        }
      }
    }
    return;
  }
  std::string selected;
  if ( !pane.pages.empty() && pane.pages[pane.page].listing.entry_size() ) {
    selected = pane.pages[pane.page].listing.entry( pane.pages[pane.page].selected ).name();
  }
  if ( ( pane.opening && !message.update() ) || message.reset() ) {
    pane.pages.clear();
    pane.page = 0;
    pane.first_page = 0;
    pane.scroll_top = 0;
    pane.prepend = pane.advance = false;
  }
  pane.path = message.path();
  Page page;
  page.listing = message;
  // The worker sorts the complete name snapshot before returning bounded
  // windows. Preserve its order across invisible chunk boundaries.
  if ( message.reset() ) {
    for ( int i = 0; i < message.entry_size(); ++i ) {
      if ( message.entry( i ).name() == selected ) { page.selected = i; }
    }
    pane.error = "Directory changed; list renewed";
  }
  if ( pane.prepend ) {
    pane.scroll_top += message.entry_size();
    pane.pages.push_front( page );
    if ( pane.first_page ) { --pane.first_page; }
    if ( pane.advance ) {
      pane.page = 0;
      pane.pages[0].selected = std::max( 0, message.entry_size() - 1 );
    } else { ++pane.page; }
  } else {
    pane.pages.push_back( page );
    if ( pane.advance ) { pane.page = pane.pages.size() - 1; }
  }
  if ( pane.pages.size() > 8 ) {
    // A fast reversal can reach the opposite end before a fetch arrives.
    // Never evict the selected window to make room for a late prefetch.
    if ( pane.prepend ? pane.page + 1 < pane.pages.size() : pane.page == 0 ) { pane.pages.pop_back(); }
    else {
      const size_t removed = pane.pages.front().listing.entry_size();
      pane.scroll_top -= std::min( pane.scroll_top, removed );
      pane.pages.pop_front(); pane.first_page++; if ( pane.page ) { --pane.page; }
    }
  }
  keep_visible( pane );
  pane.watching = true;
  pane.watched_cursor = message.start_cursor();
  pane.opening = pane.prepend = pane.advance = false;
}

void Panel::tick( uint64_t now )
{
  if ( files ) { files->tick( now ); files->flush_controls( channel ); }
  // Never interrupt an in-progress bracketed paste or split key sequence.
  // Show each offer once; the panel command can hide/reopen it without a popup loop.
  if ( download_popup_pending && !paste && escape.empty() ) {
    visible = download_view = true;
    download_popup_pending = false;
    download_reviewed = download_save_selected = download_save_reviewed = false;
  }
  local.tick( now );
  Message message;
  if ( local.pop( message ) ) {
    accept( 0, message );
  }
  if ( remote_pending && channel.queue( pending_request ) ) {
    remote_pending = false;
  }
  for ( unsigned side = 0; side < 2; ++side ) {
    auto& pane = panes[side];
    const bool live = visible && !download_view;
    if ( pane.loading || pane.pages.empty() || ( side && ( !remote_live || remote_pending ) ) ) { continue; }
    const auto& page = pane.pages[pane.page];
    if ( pane.watching != live || ( live && pane.watched_cursor != page.listing.start_cursor() ) ) {
      Message watch;
      watch.set_kind( Message::WATCH ); watch.set_request( pane.request );
      watch.set_path( pane.path ); watch.set_cursor( page.listing.start_cursor() );
      watch.set_revision( page.listing.revision() ); watch.set_live( live );
      if ( side == 0 ) { local.request( watch, now ); }
      else if ( !channel.queue( watch ) ) { continue; }
      pane.watching = live; pane.watched_cursor = page.listing.start_cursor();
    }
    // Only one window ahead, only when the selection approaches its end.
    if ( live && side == focused && pane.page + 1 == pane.pages.size() && !page.listing.eof()
         && page.selected + size_t( viewport_rows / 2 ) >= size_t( page.listing.entry_size() ) ) {
      fetch( side, page.listing.cursor(), false, false, now );
    }
  }
}

void Panel::receive( const Network::StreamEvent& event )
{
  if ( !remote_enabled || event.type != Network::StreamDataType ) {
    return;
  }
  channel.receive( event.data );
  Message message;
  while ( channel.pop( message ) ) {
    if ( message.kind() == Message::FILE_CONTROL ) {
      if ( files && message.has_file() ) { files->receive_control( message.file(), 0 ); }
    } else { accept( 1, message ); }
  }
}

int Panel::wait_time( uint64_t now, uint64_t acked ) const
{
  if ( download_popup_pending && !paste && escape.empty() ) { return 0; }
  int wait = std::min( local.wait_time(), channel.wait_time( now, acked ) );
  if ( !escape.empty() ) {
    wait = std::min( wait, now >= escape_started + 50 ? 0 : int( escape_started + 50 - now ) );
  }
  return wait;
}

void Panel::key( const std::string& input_key, uint64_t now )
{
  const auto key = panel_key( input_key );
  if ( download_view && download.token ) {
    if ( key == "\033" || key == "f" ) {
      download_view = false; download_reviewed = download_save_selected = download_save_reviewed = false;
      if ( !initialized ) { initialized = true; open( 0, ".", now ); open( 1, ".", now ); }
      return;
    }
    if ( key == "y" && download_reviewed ) {
      download_save_selected = true; download_save_reviewed = false; return;
    }
    if ( key == "n" || key == "\r" || key == "\n" ) {
      // A popup can appear while the user is typing. Saving requires two
      // different keys with a rendered confirmation between them, so a held
      // key, pasted text, or already queued y+Enter cannot accept an offer.
      if ( key != "n" && download_save_selected && !download_save_reviewed ) { return; }
      decided_download = download.token; download_allowed = key != "n" && download_save_reviewed;
      visible = false; download_view = false;
      download_reviewed = download_save_selected = download_save_reviewed = false;
    }
    return;
  }
  if ( key == "d" && download.token ) { download_view = true; download_reviewed = false; return; }
  if ( key == "\033" ) {
    visible = false;
    return;
  }
  if ( key == "\t" ) {
    focused = 1 - focused;
    return;
  }
  Pane& pane = panes[focused];
  if ( key == "x" && files ) { files->cancel(); return; }
  if ( key == "r" || key == "\033[H" || key == "\033OH" ) {
    open( focused, pane.path, now );
    return;
  }
  if ( key == "\177" || key == "\010" || key == "\033[D" ) {
    open( focused, pane.path + "/..", now );
    return;
  }
  if ( pane.pages.empty() ) {
    return;
  }
  Page& page = pane.pages[pane.page];
  if ( ( key == "\r" || key == "\n" || key == "c" || key == "\033[15~" ) && page.listing.entry_size() ) {
    const auto& entry = page.listing.entry( page.selected );
    if ( !files || !files->supported() ) { pane.error = "Copy requires librsync support at both ends"; }
    else if ( pane.loading || panes[1 - focused].loading || panes[1 - focused].pages.empty() ) {
      pane.error = "Wait for both directories to load";
    } else if ( entry.type() != Message::Entry::FILE && entry.type() != Message::Entry::DIRECTORY ) {
      pane.error = "Copy supports regular files and directories, not links or devices";
    } else {
      const auto source = pane.path + ( pane.path == "/" ? "" : "/" ) + entry.name();
      if ( !files->queue( focused == 0, source, panes[1 - focused].path, entry.name() ) ) {
        pane.error = "Copy unavailable or queue full (8 waiting jobs)";
      } else { pane.error.clear(); }
    }
  } else if ( key == "\033[A" || key == "\033OA" || key == "k" ) {
    move( focused, -1, now );
  } else if ( key == "\033[B" || key == "\033OB" || key == "j" ) {
    move( focused, 1, now );
  } else if ( key == "\033[5~" || key == "p" ) {
    move( focused, -viewport_rows, now );
  } else if ( key == "\033[6~" || key == "n" ) {
    move( focused, viewport_rows, now );
  } else if ( key == "\033[C" && page.listing.entry_size() && !pane.loading ) {
    const auto& entry = page.listing.entry( page.selected );
    if ( entry.type() == Message::Entry::DIRECTORY ) {
      open( focused, pane.path + ( pane.path == "/" ? "" : "/" ) + entry.name(), now );
    }
  }
}

std::string Panel::input( const std::string& bytes, uint64_t now, const CommandFilter& command )
{
  std::string output;
  for ( char c : bytes ) {
    if ( escape.empty() && c == '\033' ) {
      escape_started = now;
      escape += c;
      continue;
    }
    std::string key( 1, c );
    if ( !escape.empty() ) {
      escape += c;
      if ( escape.size() == 2 && ( c == '[' || c == 'O' ) ) {
        continue;
      }
      if ( escape.size() > 2 && escape[1] == '[' && c >= 0x20 && c <= 0x3f && escape.size() < 32 ) {
        continue;
      }
      key.swap( escape );
      escape.clear();
    }
    if ( key == "\033[200~" ) {
      paste = true;
    } else if ( key == "\033[201~" ) {
      paste = false;
      if ( !visible ) {
        output += key;
      }
      continue;
    }
    if ( paste ) {
      if ( !visible ) {
        output += key;
      }
      continue;
    }
    // Mosh's command prefix belongs to the client, even while the panel is
    // visible. Bracketed paste above deliberately bypasses command detection.
    if ( command && command( key ) ) { output += key; continue; }
    unsigned code = 0, mods = 1, event = 1;
    const bool extended = decode_key_event( key, code, mods, event );
    if ( visible ) {
      // Keep the application's requested keyboard mode active, so an Alt
      // press forwarded before opening the popup still gets its release.
      if ( extended && code >= 57441 && code <= 57454 ) { output += key; }
      else if ( !extended ) { this->key( key, now ); }
      else if ( event != 3 && key_event_byte( code, mods ) >= 0 ) {
        const int byte = key_event_byte( code, mods );
        if ( event == 2 && ( byte == '\r' || byte == '\n' || byte == 'c' ) ) { continue; }
        this->key( std::string( 1, char( byte ) ), now );
      }
    } else {
      output += key;
    }
  }
  return output;
}

std::string Panel::flush_input( uint64_t now, const CommandFilter& command )
{
  if ( escape.empty() || now < escape_started + 50 ) {
    return {};
  }
  std::string bytes;
  bytes.swap( escape );
  if ( command && command( bytes ) ) { return bytes; }
  if ( visible ) {
    key( bytes, now );
    return {};
  }
  return bytes;
}

void Panel::paint( Terminal::Framebuffer& fb ) const
{
  download_save_reviewed = false;
  if ( !visible ) {
    if ( download.token && fb.ds.get_height() > 0 ) {
      const int row = fb.ds.get_height() - 1;
      fb.erase_sized_text( row, 0, 1, fb.ds.get_width() );
      line( fb, row, 0, fb.ds.get_width(), "Remote download waiting: "
            + ( command_hint.empty() ? "enable MOSH_ESCAPE_KEY to review" : command_hint + " to review" ) + " (nothing saved)" );
      // An image must not obscure the local permission notice.
      std::vector<Terminal::KittyPlacement> placements;
      for ( const auto& placement : fb.get_kitty_placements() ) {
        if ( placement.rows && placement.row + int( placement.rows ) <= row ) { placements.push_back( placement ); }
      }
      fb.replace_kitty_placements( placements );
    }
    return;
  }
  const int width = fb.ds.get_width(), height = fb.ds.get_height();
  if ( width < 40 || height < ( download_view ? 12 : 16 ) ) {
    download_reviewed = false;
    if ( height > 2 ) {
      line( fb, 1, 0, width, "Enlarge terminal; " + ( command_hint.empty() ? "Esc" : command_hint ) + " closes" );
    }
    return;
  }
  const int left = 2, top = 2, w = width - 4, h = height - 4, pane_width = w / 2;
  fb.erase_sized_text( top, left, h, w );
  for ( int row = top; row < top + h; row++ ) {
    line( fb, row, left, w, "" );
  }
  if ( download_view && download.token ) {
    line( fb, top, left, w, " May we save this file to Downloads?" );
    const std::string name = safe( download.name );
    int row = top + 1;
    size_t at = 0;
    for ( ; at < name.size() && row < top + h - 5; at += w, row++ ) {
      line( fb, row, left, w, name.substr( at, w ) );
    }
    download_reviewed = at >= name.size();
    line( fb, top + h - 5, left, w, std::to_string( download.bytes ) + " bytes; existing files never overwritten" );
    line( fb, top + h - 4, left, w, "To: " + safe( download.directory ) );
    download_save_reviewed = download_reviewed && download_save_selected;
    line( fb, top + h - 3, left, w, !download_reviewed ? "Enlarge window to see full name before allowing"
                                  : download_save_selected ? "Enter: save this file   n: decline"
                                                           : "y: select Save   n/Enter: decline" );
    line( fb, top + h - 2, left, w, "f/Esc: files  "
          + ( command_hint.empty() ? "" : command_hint + ": hide; " ) + "request stays pending" );
    line( fb, top + h - 1, left, w, "Only a local key can approve. Pasted text cannot." );
  } else {
  line( fb, top, left, w, std::string( download.token ? " Files   d: review download   " : " Files   " )
        + ( command_hint.empty() ? "Esc: close" : command_hint + ": hide / resume" ) );
  const int jobs_rows = height >= 20 && files && !files->jobs().empty() ? 3 : 0;
  const int pane_bottom = top + h - jobs_rows;
  viewport_rows = std::max( 1, h - 10 - jobs_rows );
  for ( unsigned side = 0; side < 2; side++ ) {
    const auto& pane = panes[side];
    const int border_col = left + int( side ) * pane_width;
    const int box_width = side ? w - pane_width : pane_width;
    const int col = border_col + 1, inner = box_width - 2;
    box( fb, top + 1, border_col, box_width, h - 4 - jobs_rows );
    line( fb,
          top + 2,
          col,
          inner,
          std::string( side ? "Remote: " : "Local: " ) + safe( pane.path ),
          focused == side );
    line( fb, top + 3, col, inner, "  Name" + std::string( std::max( 0, inner - 10 ), ' ' ) + "Size" );
    if ( !pane.pages.empty() ) {
      std::vector<const Message::Entry*> entries;
      size_t selected = 0;
      for ( size_t i = 0; i < pane.pages.size(); ++i ) {
        if ( i == pane.page ) { selected = entries.size() + pane.pages[i].selected; }
        for ( const auto& entry : pane.pages[i].listing.entry() ) { entries.push_back( &entry ); }
      }
      keep_visible( pane );
      const size_t start = pane.scroll_top;
      for ( int row = 0; row < viewport_rows && start + row < entries.size(); row++ ) {
        const auto& entry = *entries[start + row];
        const char* type = entry.type() == Message::Entry::DIRECTORY ? "/ "
                           : entry.type() == Message::Entry::LINK    ? "@ "
                           : entry.type() == Message::Entry::FILE    ? "  "
                                                                     : "? ";
        const auto size = entry.type() == Message::Entry::DIRECTORY ? "<DIR>"
                          : entry.has_size() ? size_label( entry.size() ) : "-";
        const int name_width = std::max( 1, inner - 8 );
        auto name = type + safe( entry.name() );
        if ( int( name.size() ) > name_width ) { name = name.substr( 0, name_width - 1 ) + "~"; }
        name.resize( inner - size.size(), ' ' );
        line( fb, top + 4 + row, col, inner, name + size, focused == side && start + row == selected );
      }
      line( fb,
            pane_bottom - 6,
            col,
            inner,
            std::to_string( entries.size() ) + " cached"
              + ( pane.pages.back().listing.eof() ? " | end" : " | scroll for more" ) );
    }
    if ( pane.loading ) {
      line( fb, pane_bottom - 6, col, inner, "Loading... terminal remains live" );
    } else if ( !pane.error.empty() ) {
      line( fb, pane_bottom - 6, col, inner, safe( pane.error ) );
    }
  }
  if ( jobs_rows ) {
    const auto& jobs = files->jobs();
    const Files::Command* active = &jobs.back();
    for ( const auto& job : jobs ) { if ( !job.done() ) { active = &job; break; } }
    line( fb, top + h - 6, left, w, " Transfers  " + std::to_string( files->queue_size() ) + " queued   x: cancel active + queue" );
    line( fb, top + h - 5, left, w, safe( active->status() ) );
    line( fb, top + h - 4, left, w, std::to_string( active->files() ) + " saved | data " + size_label( active->payload() )
          + " | delta " + size_label( active->deltas() ) + " | sig " + size_label( active->signatures() ) );
  }
  line( fb, top + h - 3, left, w, traffic );
  line( fb, top + h - 2, left, w, "Tab/Shift-Tab pane | Left/Bksp parent | Right enter folder" );
  line( fb, top + h - 1, left, w, files && files->supported()
          ? "Enter/F5/c transfer | Up/Down/PgUp/PgDn scroll | r refresh | Esc close"
          : "Enter: transfer needs librsync | Up/Down scroll | r refresh | Esc close" );
  }
  fb.ds.cursor_visible = false;
  fb.ds.bracketed_paste = true;
  fb.ds.mouse_reporting_mode = Terminal::DrawState::MOUSE_REPORTING_NONE;
  fb.ds.mouse_focus_event = false;
  fb.ds.mouse_alternate_scroll = false;
  fb.ds.application_mode_cursor_keys = false;
  // Graphics may cover text at a positive z-order; hide overlapping
  // placements only in this composite, never in the synchronized state.
  std::vector<Terminal::KittyPlacement> placements;
  for ( const auto& placement : fb.get_kitty_placements() ) {
    if ( placement.row >= top + h || placement.col >= left + w
         || ( placement.rows && placement.row + int( placement.rows ) <= top )
         || ( placement.columns && placement.col + int( placement.columns ) <= left ) ) {
      placements.push_back( placement );
    }
  }
  fb.replace_kitty_placements( placements );
}
}
