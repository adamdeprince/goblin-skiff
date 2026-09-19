/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "src/include/config.h"

#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <err.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#endif

#include "src/protobufs/clipboard.pb.h"
#include "src/protobufs/hostinput.pb.h"
#include "src/statesync/clipboard.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/util/fatal_assert.h"
#include "src/util/locale_utils.h"
#include "src/util/pty_compat.h"
#include "src/util/select.h"
#include "src/util/swrite.h"
#include "src/util/timestamp.h"
#include "stmclient.h"

#include "src/network/networktransport-impl.h"

static Terminal::ClientGeometry client_geometry_from_winsize( const struct winsize& window_size )
{
  const uint32_t columns = window_size.ws_col;
  const uint32_t rows = window_size.ws_row;
  const uint32_t width_px = window_size.ws_xpixel;
  const uint32_t height_px = window_size.ws_ypixel;
  const uint32_t cell_width_px = columns != 0 ? width_px / columns : 0;
  const uint32_t cell_height_px = rows != 0 ? height_px / rows : 0;

  return Terminal::ClientGeometry( columns, rows, width_px, height_px, cell_width_px, cell_height_px );
}

void STMClient::resume( void )
{
  mascot.invalidate_cursor();
  /* Restore termios state */
  if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  /* Put terminal in application-cursor-key mode */
  if ( !tmux_parser.active() ) {
    swrite( STDOUT_FILENO, display.open().c_str() );
  }

  /* Flag that outer terminal state is unknown */
  repaint_requested = true;
}

void STMClient::init( void )
{
  if ( !is_utf8_locale() ) {
    LocaleVar native_ctype = get_ctype();
    std::string native_charset( locale_charset() );

    fprintf( stderr,
             "goblin-skiff-client needs a UTF-8 native locale to run.\n\n"
             "Unfortunately, the client's environment (%s) specifies\n"
             "the character set \"%s\".\n\n",
             native_ctype.str().c_str(),
             native_charset.c_str() );
    int unused __attribute( ( unused ) ) = system( "locale" );
    exit( 1 );
  }

  /* Verify terminal configuration */
  if ( tcgetattr( STDIN_FILENO, &saved_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  /* Put terminal driver in raw mode */
  raw_termios = saved_termios;

#ifdef HAVE_IUTF8
  if ( !( raw_termios.c_iflag & IUTF8 ) ) {
    //    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    /* Probably not really necessary since we are putting terminal driver into raw mode anyway. */
    raw_termios.c_iflag |= IUTF8;
  }
#endif /* HAVE_IUTF8 */

  cfmakeraw( &raw_termios );

  if ( tcsetattr( STDIN_FILENO, TCSANOW, &raw_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  /* Put terminal in application-cursor-key mode */
  swrite( STDOUT_FILENO, display.open().c_str() );

  /* Add our name to window title */
  if ( !getenv( "MOSH_TITLE_NOPREFIX" ) ) {
    overlays.set_title_prefix( std::wstring( L"[goblin-skiff] " ) );
  }

  /* Set terminal escape key. */
  const char* escape_key_env;
  if ( ( escape_key_env = getenv( "MOSH_ESCAPE_KEY" ) ) != NULL ) {
    if ( strlen( escape_key_env ) == 1 ) {
      escape_key = (int)escape_key_env[0];
      if ( escape_key > 0 && escape_key < 128 ) {
        if ( escape_key < 32 ) {
          /* If escape is ctrl-something, pass it with repeating the key without ctrl. */
          escape_pass_key = escape_key + (int)'@';
        } else {
          /* If escape is something else, pass it with repeating the key itself. */
          escape_pass_key = escape_key;
        }
        if ( escape_pass_key >= 'A' && escape_pass_key <= 'Z' ) {
          /* If escape pass is an upper case character, define optional version
             as lower case of the same. */
          escape_pass_key2 = escape_pass_key + (int)'a' - (int)'A';
        } else {
          escape_pass_key2 = escape_pass_key;
        }
      } else {
        escape_key = 0x1E;
        escape_pass_key = '^';
        escape_pass_key2 = '^';
      }
    } else if ( strlen( escape_key_env ) == 0 ) {
      escape_key = -1;
    } else {
      escape_key = 0x1E;
      escape_pass_key = '^';
      escape_pass_key2 = '^';
    }
  } else {
    escape_key = 0x1E;
    escape_pass_key = '^';
    escape_pass_key2 = '^';
  }

  /* There are so many better ways to shoot oneself into leg than
     setting escape key to Ctrl-C, Ctrl-D, NewLine, Ctrl-L or CarriageReturn
     that we just won't allow that. */
  if ( escape_key == 0x03 || escape_key == 0x04 || escape_key == 0x0A || escape_key == 0x0C
       || escape_key == 0x0D ) {
    escape_key = 0x1E;
    escape_pass_key = '^';
    escape_pass_key2 = '^';
  }

  /* Adjust escape help differently if escape is a control character. */
  if ( escape_key > 0 ) {
    char escape_pass_name_buf[16];
    char escape_key_name_buf[16];
    snprintf( escape_pass_name_buf, sizeof escape_pass_name_buf, "\"%c\"", escape_pass_key );
    if ( escape_key < 32 ) {
      snprintf( escape_key_name_buf, sizeof escape_key_name_buf, "Ctrl-%c", escape_pass_key );
      escape_requires_lf = false;
    } else {
      snprintf( escape_key_name_buf, sizeof escape_key_name_buf, "\"%c\"", escape_key );
      escape_requires_lf = true;
    }
    std::string tmp;
    tmp = std::string( escape_pass_name_buf );
    std::wstring escape_pass_name = std::wstring( tmp.begin(), tmp.end() );
    tmp = std::string( escape_key_name_buf );
    std::wstring escape_key_name = std::wstring( tmp.begin(), tmp.end() );
    escape_key_help
      = L"Commands: 0 panel, Ctrl-Z suspends, \".\" quits, " + escape_pass_name + L" gives literal " + escape_key_name;
    overlays.get_notification_engine().set_escape_key_string( tmp );
    control_panel.set_command_hint( ( escape_requires_lf ? "Enter " : "" ) + tmp + " 0" );
  } else {
    control_panel.set_command_hint( "" );
  }
  wchar_t tmp[128];
  swprintf( tmp, 128, L"Nothing received from server on UDP port %s.", port.c_str() );
  connecting_notification = std::wstring( tmp );
}

void STMClient::shutdown( void )
{
  dismiss_mascot();
  const auto download_cancel = downloads.close_forwarding();
  swrite( STDOUT_FILENO, download_cancel.data(), download_cancel.size() );
  if ( tmux_parser.active() ) {
    /* Release the local integration even if the remote PTY disappeared before
       tmux could send its own exit. Never put display escapes inside its DCS. */
    const std::string end = "\r\n%exit\r\n\033\\";
    swrite( STDOUT_FILENO, end.data(), end.size() );
    tmux_parser.consume( end );
    repaint_requested = true;
  }
  /* Restore screen state */
  overlays.get_notification_engine().set_notification_string( std::wstring( L"" ) );
  overlays.get_notification_engine().server_heard( timestamp() );
  overlays.set_title_prefix( std::wstring( L"" ) );
  output_new_frame();

  /* Restore terminal and terminal-driver state */
  swrite( STDOUT_FILENO, display.close().c_str() );

  if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
    perror( "tcsetattr" );
    exit( 1 );
  }

  if ( still_connecting() ) {
    fprintf( stderr,
             "\nmosh did not make a successful connection to %s:%s.\n"
             "Please verify that UDP port %s is not firewalled and can reach the server.\n\n"
             "(By default, goblin-skiff uses a UDP port between 60000 and 61000. The -p option\n"
             "selects a specific UDP port number.)\n",
             ip.c_str(),
             port.c_str(),
             port.c_str() );
  } else if ( network && !clean_shutdown ) {
    fputs( "\n\nmosh did not shut down cleanly. Please note that the\n"
           "goblin-skiff-server process may still be running on the server.\n",
           stderr );
  }
}

void STMClient::main_init( void )
{
  Select& sel = Select::get_instance();
  sel.add_signal( SIGWINCH );
  sel.add_signal( SIGTERM );
  sel.add_signal( SIGINT );
  sel.add_signal( SIGHUP );
  sel.add_signal( SIGPIPE );
  sel.add_signal( SIGCONT );

  /* get initial window size */
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return;
  }

  /* local state */
  local_framebuffer = Terminal::Framebuffer( window_size.ws_col, window_size.ws_row );
  new_state = Terminal::Framebuffer( 1, 1 );

  /* Leave the local screen alone during connection setup. Remote output
     gradually takes over the viewport as it needs more rows. */

  /* open network */
  Network::UserStream blank;
  Terminal::Complete local_terminal( window_size.ws_col, window_size.ws_row );
  network = NetworkPointer(
    new NetworkType( blank, local_terminal, key.c_str(), ip.c_str(), port.c_str(), compact_keepalive, crypto_mode, socks5_proxy ) );
  if ( !relay_keys.empty() ) { network->set_relay_keys( relay_keys ); relay_keys.clear(); }
  network->enable_link_budget( compact_keepalive && getenv( "MOSH_LINK_BUDGET" ) && !strcmp( getenv( "MOSH_LINK_BUDGET" ), "1" ) );

  if ( !state_sample_log.empty() ) {
    network->set_state_sample_log( state_sample_log, state_sample_min_size );
  }
  network->set_send_delay( 1 ); /* minimal delay on outgoing keystrokes */

  /* Tell the server about this attachment before releasing the remote PTY.
     Pixel geometry is an ephemeral control event, not terminal state. */
  network->get_current_state().push_back( client_geometry_from_winsize( window_size ) );
  network->get_current_state().push_back( Parser::Resize( window_size.ws_col, window_size.ws_row ) );
  network->request_immediate_send();

  display.set_graphics( Terminal::ClientGraphics(), false );
  display.set_graphics_geometry( client_geometry_from_winsize( window_size ) );

  const unsigned cw = window_size.ws_col ? window_size.ws_xpixel / window_size.ws_col : 0;
  const unsigned ch = window_size.ws_row ? window_size.ws_ypixel / window_size.ws_row : 0;
  const char* term = getenv( "TERM" );
  // Local, once per client process: never sent to the remote PTY or repeated
  // after a network reconnect/SIGCONT. The project page links release sources
  // and license information; packages still need the actual license files.
  swrite( STDOUT_FILENO, "[Goblin Skiff GPLv3+ | https://github.com/adamdeprince/goblin-skiff]\r\n" );
  std::string panel_notice;
  if ( tmux_control ) {
    panel_notice = "Control panel shortcuts are unavailable in tmux control mode.";
  } else if ( escape_key < 0 || escape_key == '0' ) {
    panel_notice = "Control panel shortcut disabled by MOSH_ESCAPE_KEY.";
  } else if ( escape_key == 0x1e ) {
    panel_notice = "Control panel: Ctrl-^ then 0 (Kitty: Ctrl-6, release, then 0).";
  } else if ( escape_key < 32 ) {
    panel_notice = "Control panel: Ctrl-" + std::string( 1, char( escape_key + '@' ) ) + ", release, then 0.";
  } else {
    const std::string prefix = escape_key == 127 ? "Backspace (DEL)" : "\"" + std::string( 1, char( escape_key ) ) + "\"";
    panel_notice = "Control panel: Enter, then " + prefix + ", then 0.";
  }
  swrite( STDOUT_FILENO, ( panel_notice + "\r\n" ).c_str() );
  const std::string queries = mascot.start( Mascot::Size( window_size.ws_col, window_size.ws_row, cw, ch ),
                                          isatty( STDIN_FILENO ) && isatty( STDOUT_FILENO )
                                            && term && strcmp( term, "dumb" ) != 0,
                                          timestamp() );
  swrite( STDOUT_FILENO, queries.data(), queries.size() );
  if ( downloads_enabled && !tmux_control && isatty( STDIN_FILENO ) && isatty( STDOUT_FILENO ) ) {
    downloads.enable_forwarding( timestamp() );
  }

  /* be noisy as necessary */
  network->set_verbose( verbose );
  Select::set_verbose( verbose );

  std::string error;
  if ( !forwarder.listen( error ) ) {
    fprintf( stderr, "Cannot set up stream forwarding: %s\n", error.c_str() );
    exit( 1 );
  }

  setenv( "GOBLIN_MOSHCP_SOCK", bulk_control.socket_path().c_str(), true );
  if ( verbose ) {
    fprintf( stderr, "goblin-skiffcp control socket: %s\n", bulk_control.socket_path().c_str() );
  }
}

void STMClient::dismiss_mascot()
{
  const std::string banner = mascot.paint( timestamp(), true );
  swrite( STDOUT_FILENO, banner.data(), banner.size() );
  const std::string cleanup = mascot.dismiss();
  if ( !cleanup.empty() ) {
    swrite( STDOUT_FILENO, cleanup.data(), cleanup.size() );
  }
}

void STMClient::output_new_frame( void )
{
  if ( !network ) { /* clean shutdown even when not initialized */
    return;
  }
  if ( tmux_parser.active() ) {
    return;
  }
  if ( mascot.active() ) {
    const std::string banner = mascot.paint( timestamp() );
    swrite( STDOUT_FILENO, banner.data(), banner.size() );
    if ( !network->shutdown_in_progress() && ( still_connecting() || !mascot.painted() ) ) {
      return;
    }
    dismiss_mascot();
  }
  if ( still_connecting() ) {
    return;
  }

  if ( !screen_initialized && !display.uses_alternate_screen() ) {
    // The local splash can end anywhere in a fresh window. Wait only for a
    // bounded local cursor report; network service continues in the main loop.
    // On shutdown, always flush the final remote output even without a reply.
    if ( !network->shutdown_in_progress() && !network->counterparty_shutdown_ack_sent()
         && !mascot.cursor_ready( timestamp() ) ) {
      return;
    }
    startup_screen.set_cursor_row( mascot.start_row() );
  }

  /* fetch target state */
  new_state = network->get_latest_remote_state().state.get_fb();

  /* apply local overlays */
  overlays.apply( new_state );
  control_panel.paint( new_state );

  /* calculate minimal difference from where we are */
  const std::string diff( display.new_frame( screen_initialized && !repaint_requested,
                                           local_framebuffer, new_state,
                                           display.uses_alternate_screen() ? NULL : &startup_screen ) );
  swrite( STDOUT_FILENO, diff.data(), diff.size() );

  repaint_requested = false;
  screen_initialized = true;

  local_framebuffer = new_state;
}

void STMClient::process_network_input( void )
{
  network->recv();

  const std::string remote_diff( network->get_remote_diff() );
  if ( !remote_diff.empty() ) {
    HostBuffers::HostMessage input;
    fatal_assert( input.ParseFromString( remote_diff ) );
    for ( int i = 0; i < input.instruction_size(); i++ ) {
      if ( input.instruction( i ).HasExtension( HostBuffers::stream ) ) {
        const auto event = Network::stream_event_from_proto( input.instruction( i ).GetExtension( HostBuffers::stream ) );
        if ( event.stream_id == Control::STREAM_ID ) { control_panel.receive( event ); }
        else { forwarder.handle_remote_event( event ); }
      } else if ( input.instruction( i ).HasExtension( HostBuffers::clipboard ) ) {
        const Terminal::ClipboardEvent ev = Terminal::clipboard_event_from_proto(
          input.instruction( i ).GetExtension( HostBuffers::clipboard ) );
        if ( !tmux_parser.active() && ( ev.op == Terminal::ClipboardQuery || ev.op == Terminal::ClipboardSet
             || ev.op == Terminal::ClipboardClear ) ) {
          const std::string seq = Terminal::encode_osc52( ev );
          swrite( STDOUT_FILENO, seq.data(), seq.size() );
          if ( ev.op == Terminal::ClipboardQuery ) {
            expecting_osc52_reply = true;
          }
        }
      } else if ( tmux_control && input.instruction( i ).HasExtension( HostBuffers::tmux_output ) ) {
        dismiss_mascot();
        const Terminal::TmuxControlParser::Output output
          = tmux_parser.consume( input.instruction( i ).GetExtension( HostBuffers::tmux_output ) );
        fatal_assert( output.terminal.empty() );
        swrite( STDOUT_FILENO, output.control.data(), output.control.size() );
        overlays.get_prediction_engine().reset();
        quit_sequence_started = false;
        expecting_osc52_reply = false;
        repaint_requested = true;
      }
    }
  }

  Network::Bulk::Datagram bulk;
  while ( network->pop_bulk( bulk ) ) {
    if ( bulk.type == Network::Bulk::PacketType::FileSymbol || bulk.type == Network::Bulk::PacketType::FileAck ) {
      if ( files.supported() ) { files.channel.receive( bulk ); }
      continue;
    }
    if ( bulk.type == Network::Bulk::PacketType::DownloadSymbol || bulk.type == Network::Bulk::PacketType::DownloadAck ) {
      if ( downloads_enabled && !tmux_control ) { downloads.channel.receive( bulk ); }
      continue;
    }
    if ( !mime_clipboard.channel.receive( bulk ) ) { bulk_control.broadcast( bulk ); }
  }

  /* Now give hints to the overlays */
  overlays.get_notification_engine().server_heard( network->get_latest_remote_state().timestamp );
  uint64_t last_reply = network->get_sent_state_acked_timestamp();
  const uint64_t last_roundtrip = network->get_last_roundtrip_success();
  if ( last_roundtrip != uint64_t( -1 ) && ( last_reply == uint64_t( -1 ) || last_roundtrip > last_reply ) ) {
    last_reply = last_roundtrip;
  }
  overlays.get_notification_engine().server_acked( last_reply );

  overlays.get_prediction_engine().set_local_frame_acked( network->get_sent_state_acked() );
  overlays.get_prediction_engine().set_send_interval( network->send_interval() );
  overlays.get_prediction_engine().set_local_frame_late_acked(
    network->get_latest_remote_state().state.get_echo_ack() );
}

bool STMClient::process_user_input( int fd )
{
  const int buf_size = 16384;
  char buf[buf_size];

  /* fill buffer if possible */
  ssize_t bytes_read = read( fd, buf, buf_size );
  if ( bytes_read == 0 ) { /* EOF */
    return false;
  } else if ( bytes_read < 0 ) {
    perror( "read" );
    return false;
  }

  return process_terminal_bytes( mascot.filter( std::string( buf, bytes_read ), timestamp() ) );
}

bool STMClient::process_terminal_bytes( const std::string& bytes )
{
  return process_download_filtered_bytes( downloads_enabled && !tmux_parser.active()
                                           ? downloads.filter_input( bytes, timestamp() ) : bytes );
}

bool STMClient::process_download_filtered_bytes( const std::string& bytes )
{
  if ( mime_enabled && !tmux_parser.active() ) {
    auto extracted = mime_input.consume( bytes, timestamp() );
    for ( const auto& body : extracted.messages ) { mime_clipboard.submit( body, timestamp() ); }
    return process_user_bytes( extracted.user );
  }
  return process_user_bytes( bytes );
}

bool STMClient::is_command_key( const std::string& key )
{
  if ( quit_sequence_started ) { return true; }
  unsigned code = 0, mods = 1, event = 1;
  int value = key.size() == 1 ? static_cast<unsigned char>( key[0] ) : -1;
  if ( Control::decode_key_event( key, code, mods, event ) ) {
    if ( event != 1 && keyboard_local_keys.count( code ) ) { return true; }
    if ( event == 3 || ( code >= 57441 && code <= 57454 ) ) { return false; }
    value = Control::key_event_byte( code, mods );
  }
  if ( escape_key > 0 && value == escape_key && ( lf_entered || !escape_requires_lf ) ) { return true; }
  // The popup consumes Enter itself; printable prefixes must still be able
  // to follow that Enter, without losing normal directory navigation.
  if ( control_panel.active() ) { lf_entered = value == 10 || value == 13; }
  return false;
}

bool STMClient::process_user_bytes( const std::string& bytes, bool panel_filtered )
{
  if ( bytes.empty() ) { return true; }
  const char* buf = bytes.data();
  const ssize_t bytes_read = bytes.size();
  NetworkType& net = *network;

  if ( net.shutdown_in_progress() ) {
    return true;
  }
  if ( tmux_parser.active() ) {
    /* These are tmux commands from the local integration, not keystrokes.
       Do not filter OSC replies, interpret escape keys, or predict them. */
    net.get_current_state().push_back( Terminal::TmuxBytes( std::string( buf, bytes_read ) ) );
    net.request_immediate_send();
    return true;
  }
  overlays.get_prediction_engine().set_local_frame_sent( net.get_sent_state_last() );

  const char* input = buf;
  ssize_t input_len = bytes_read;
  std::string filtered_input;
  if ( expecting_osc52_reply ) {
    Terminal::Osc52InputFilter::Output extracted = osc52_input.consume( buf, static_cast<size_t>( bytes_read ) );
    if ( !extracted.events.empty() ) {
      expecting_osc52_reply = false;
      for ( size_t ev_i = 0; ev_i < extracted.events.size(); ev_i++ ) {
        if ( Terminal::clipboard_should_transmit( extracted.events[ev_i] ) ) {
          net.get_current_state().push_back( extracted.events[ev_i] );
        }
      }
      if ( osc52_input.in_sequence() ) {
        Terminal::Osc52InputFilter::Output rest = osc52_input.flush();
        extracted.user_bytes.append( rest.user_bytes );
      }
    }
    filtered_input.swap( extracted.user_bytes );
    input = filtered_input.data();
    input_len = static_cast<ssize_t>( filtered_input.size() );
  }

  /* Don't predict for bulk data. */
  const bool paste = input_len > 100;
  if ( paste ) { overlays.get_prediction_engine().reset(); }
  if ( panel_filtered ) { return process_user_keys( std::string( input, input_len ), paste ); }

  const Control::Panel::CommandFilter command = [this]( const std::string& key ) { return is_command_key( key ); };
  // Assemble one key at a time. A command may open/close the panel halfway
  // through a read; the remaining keys must go to the newly active recipient.
  for ( ssize_t i = 0; i < input_len; i++ ) {
    const bool was_visible = control_panel.active();
    const std::string key = control_panel.input( std::string( 1, input[i] ), timestamp(), command );
    if ( !process_user_keys( key, paste ) ) { return false; }
    if ( was_visible != control_panel.active() ) {
      overlays.get_prediction_engine().reset();
      quit_sequence_started = false;
      /* The popup uses physical window coordinates, unlike an initial shell
         prompt. Release any retained startup prefix before opening it. */
      if ( control_panel.active() ) {
        repaint_requested = true;
      }
    }
    if ( net.shutdown_in_progress() ) { break; }
  }
  return true;
}

bool STMClient::process_user_keys( const std::string& bytes, bool paste )
{
  NetworkType& net = *network;
  const char* input = bytes.data();
  const ssize_t input_len = bytes.size();

  for ( int i = 0; i < input_len; i++ ) {
    char the_byte = input[i];
    if ( input_len - i >= 6 && !memcmp( input + i, "\033[200~", 6 ) ) { input_bracketed_paste = true; }
    if ( input_len - i >= 6 && !memcmp( input + i, "\033[201~", 6 ) ) { input_bracketed_paste = false; }
    if ( input_bracketed_paste ) {
      net.get_current_state().push_back( Parser::UserByte( the_byte ) );
      continue;
    }
    std::string key_packet;
    unsigned key_code = 0, key_modifiers = 1, key_event = 1;
    const auto send_bytes = [&]( const std::string& bytes_to_send ) {
      for ( char c : bytes_to_send ) { net.get_current_state().push_back( Parser::UserByte( c ) ); }
    };
    const auto send_escape = [&]() {
      if ( keyboard_escape_packet.empty() ) { net.get_current_state().push_back( Parser::UserByte( escape_key ) ); }
      else {
        send_bytes( keyboard_escape_packet );
        send_bytes( keyboard_escape_release );
        keyboard_local_keys.erase( keyboard_escape_code ); // forward a not-yet-received release too
      }
    };
    // The panel filter has already assembled ordinary CSI-u packets. Treat
    // those as atomic keys: modifier/release events must not cancel Mosh's
    // local escape prefix, and report-all mode must not disable Ctrl-^ .
    if ( the_byte == '\033' && i + 2 < input_len && input[i + 1] == '[' ) {
      int end = i + 2;
      while ( end < input_len && end - i < 128 && input[end] >= 0x20 && input[end] <= 0x3f ) { end++; }
      if ( end < input_len && input[end] == 'u' ) {
        const std::string packet( input + i, end - i + 1 );
        if ( Control::decode_key_event( packet, key_code, key_modifiers, key_event ) ) {
          i = end;
          if ( key_event == 3 && keyboard_local_keys.erase( key_code ) ) {
            if ( quit_sequence_started && key_code == keyboard_escape_code ) { keyboard_escape_release = packet; }
            continue;
          }
          if ( key_event == 2 && keyboard_local_keys.count( key_code ) ) { continue; }
          const int value = Control::key_event_byte( key_code, key_modifiers );
          if ( key_event == 3 || ( key_code >= 57441 && key_code <= 57454 ) ) { send_bytes( packet ); continue; }
          if ( !quit_sequence_started && !( escape_key > 0 && value == escape_key && ( lf_entered || !escape_requires_lf ) ) ) {
            // A new remote press supersedes a local press from an earlier
            // keyboard mode that did not report releases.
            keyboard_local_keys.erase( key_code );
            send_bytes( packet );
            lf_entered = value == 10 || value == 13;
            if ( value == 12 ) { repaint_requested = true; }
            continue;
          }
          key_packet = packet;
          the_byte = value >= 0 ? char( value ) : '\0';
        }
      }
    }

    if ( !paste ) {
      overlays.get_prediction_engine().new_user_byte( the_byte, local_framebuffer );
    }

    if ( quit_sequence_started ) {
      if ( the_byte == '.' ) { /* Quit sequence is Ctrl-^ . */
        if ( net.has_remote_addr() && ( !net.shutdown_in_progress() ) ) {
          overlays.get_notification_engine().set_notification_string( std::wstring( L"Exiting on user request..." ),
                                                                      true );
          net.start_shutdown();
          return true;
        }
        return false;
      } else if ( the_byte == 0x1a ) { /* Suspend sequence is escape_key Ctrl-Z */
        if ( !key_packet.empty() ) { keyboard_local_keys.insert( key_code ); }
        /* Restore terminal and terminal-driver state */
        swrite( STDOUT_FILENO, display.close().c_str() );

        if ( tcsetattr( STDIN_FILENO, TCSANOW, &saved_termios ) < 0 ) {
          perror( "tcsetattr" );
          exit( 1 );
        }

        fputs( "\n\033[37;44m[goblin-skiff is suspended.]\033[m\n", stdout );

        fflush( NULL );

        /* actually suspend */
        kill( 0, SIGSTOP );

        resume();
      } else if ( ( the_byte == escape_pass_key ) || ( the_byte == escape_pass_key2 ) ) {
        /* Emulation sequence to type escape_key is escape_key +
           escape_pass_key (that is escape key without Ctrl) */
        send_escape();
        if ( !key_packet.empty() ) { keyboard_local_keys.insert( key_code ); }
      } else if ( the_byte == '0' ) {
        if ( !key_packet.empty() ) { keyboard_local_keys.insert( key_code ); }
        control_panel.toggle( timestamp() );
      } else {
        /* Unknown commands are sent literally, including the escape prefix. */
        send_escape();
        if ( key_packet.empty() ) { net.get_current_state().push_back( Parser::UserByte( the_byte ) ); }
        else { send_bytes( key_packet ); }
      }

      quit_sequence_started = false;
      keyboard_escape_packet.clear();
      keyboard_escape_release.clear();

      if ( overlays.get_notification_engine().get_notification_string() == escape_key_help ) {
        overlays.get_notification_engine().set_notification_string( L"" );
      }

      continue;
    }

    quit_sequence_started
      = ( escape_key > 0 ) && ( the_byte == escape_key ) && ( lf_entered || ( !escape_requires_lf ) );
    if ( quit_sequence_started ) {
      keyboard_escape_packet = key_packet;
      keyboard_escape_release.clear();
      keyboard_escape_code = key_code;
      if ( !key_packet.empty() ) { keyboard_local_keys.insert( key_code ); }
      lf_entered = false;
      overlays.get_notification_engine().set_notification_string( escape_key_help, true, false );
      continue;
    }

    lf_entered = ( ( the_byte == 0x0A )
                   || ( the_byte == 0x0D ) ); /* LineFeed, Ctrl-J, '\n' or CarriageReturn, Ctrl-M, '\r' */

    if ( the_byte == 0x0C ) { /* Ctrl-L */
      repaint_requested = true;
    }

    net.get_current_state().push_back( Parser::UserByte( the_byte ) );
  }

  return true;
}

bool STMClient::process_resize( void )
{
  mascot.invalidate_cursor();
  dismiss_mascot();
  /* get new size */
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    return false;
  }

  /* Tell the server about presentation geometry before the logical resize so
     it can update the PTY atomically before delivering SIGWINCH. */
  Terminal::ClientGeometry geometry = client_geometry_from_winsize( window_size );
  Parser::Resize res( window_size.ws_col, window_size.ws_row );
  display.set_graphics_geometry( geometry );
  repaint_requested = true;

  if ( !network->shutdown_in_progress() ) {
    network->get_current_state().push_back( geometry );
    network->get_current_state().push_back( res );
    network->request_immediate_send();
  }

  /* note remote emulator will probably reply with its own Resize to adjust our state */

  /* tell prediction engine */
  overlays.get_prediction_engine().reset();

  return true;
}

bool STMClient::main( void )
{
  /* initialize signal handling and structures */
  main_init();

  /* Drop unnecessary privileges */
#ifdef HAVE_PLEDGE
  /* OpenBSD pledge() syscall */
  if ( pledge( "stdio inet unix tty rpath proc", NULL ) ) {
    perror( "pledge() failed" );
    exit( 1 );
  }
#endif

  /* prepare to poll for events */
  Select& sel = Select::get_instance();

  while ( 1 ) {
    try {
      if ( compact_keepalive ) {
        overlays.get_notification_engine().set_keepalive_interval( network->get_keepalive_interval() );
      }
      process_terminal_bytes( mascot.flush( timestamp() ) );
      if ( !graphics_sent && mascot.probe_ready( timestamp() ) ) {
        Terminal::ClientGraphics caps( mascot.supports_kitty(), mascot.supports_sixel(), mascot.supports_keyboard(),
                                        mascot.supports_text_sizing(), mime_negotiated && mascot.supports_clipboard() );
        if ( const char* cutoff = getenv( "MOSH_CLIPBOARD_FAST_THRESHOLD" ) ) {
          char* end = NULL;
          const unsigned long value = strtoul( cutoff, &end, 10 );
          if ( *cutoff && !*end && value <= Terminal::OSC5522_MAX_TRANSFER ) { caps.clipboard_fast_threshold = value; }
        }
        mime_enabled = caps.clipboard && !tmux_control;
        caps.clipboard = mime_enabled;
        caps.downloads = downloads_enabled && !tmux_control;
        // The first open() preceded capability discovery. Balance this one
        // initial push with close(); subsequent suspend/resume uses open().
        if ( sixel_state && caps.keyboard ) { swrite( STDOUT_FILENO, "\033[>0u" ); }
        display.set_graphics( caps, sixel_state );
        Terminal::ClientGeometry geometry = client_geometry_from_winsize( window_size );
        if ( !geometry.has_cell_size() ) {
          geometry.cell_width_px = mascot.dimensions().cell_width;
          geometry.cell_height_px = mascot.dimensions().cell_height;
          geometry.width_px = std::min( 65535U, geometry.columns * geometry.cell_width_px );
          geometry.height_px = std::min( 65535U, geometry.rows * geometry.cell_height_px );
          network->get_current_state().push_back( geometry );
          network->get_current_state().push_back( Parser::Resize( window_size.ws_col, window_size.ws_row ) );
        }
        display.set_graphics_geometry( geometry );
        if ( sixel_state ) { network->get_current_state().push_back( caps ); }
        network->request_immediate_send();
        graphics_sent = true;
      }
      if ( !tmux_parser.active() ) {
        process_user_bytes( control_panel.flush_input( timestamp(),
          [this]( const std::string& key ) { return is_command_key( key ); } ), true );
      }
      const bool panel_was_visible = control_panel.active();
      const auto& link = network->link_budget();
      files.channel.set_external_pacing( link.active() );
      forwarder.adapt_link_budget( link.active() ? link.budget() : 0 );
      if ( link.active() ) {
        char traffic[192];
        snprintf( traffic, sizeof traffic, "kbit/s  Up %.1f / %.1f  Down %.1f / %.1f  loss %.0f%% (%s)",
                  link.upload_rate( timestamp() ) * .008, link.budget() * .008,
                  link.download_rate( timestamp() ) * .008, link.remote_budget() * .008,
                  link.loss_fraction() * 100, link.has_measurement() ? "traffic / budget" : "learning" );
        control_panel.set_traffic( traffic );
      } else { control_panel.set_traffic( "Link auto-pacing unavailable with this peer" ); }
      control_panel.tick( timestamp() );
      if ( panel_was_visible != control_panel.active() ) {
        overlays.get_prediction_engine().reset();
        quit_sequence_started = false;
        if ( control_panel.active() ) { repaint_requested = true; }
      }
      output_new_frame();

      uint64_t now = timestamp();
      if ( downloads_enabled && !tmux_control ) { downloads.tick( now ); }
      if ( downloads_enabled && !tmux_control ) {
        process_download_filtered_bytes( downloads.expire_input( now ) );
        // Whole, bounded OSC records; never interleave a display repaint into
        // a transfer frame, and never put these events into screen state.
        for ( unsigned n = 0; n < 2; n++ ) {
          const auto output = downloads.take_terminal_output();
          if ( output.empty() ) { break; }
          swrite( STDOUT_FILENO, output.data(), output.size() );
        }
        Download::Record offered;
        if ( downloads.pending( offered ) ) {
          control_panel.offer_download( offered.token(), offered.name(), offered.size(), downloads.directory(), true );
        } else { control_panel.offer_download( 0, "", 0, "" ); }
        uint64_t token = 0; bool allow = false;
        if ( control_panel.take_download_decision( token, allow ) ) { downloads.decide( token, allow ); }
      }
      process_user_bytes( mime_input.expire( now ).user );
      std::string mime_body;
      if ( mime_enabled && mime_clipboard.take_output( mime_body ) ) {
        const auto sequence = "\033]" + mime_body + "\033\\";
        swrite( STDOUT_FILENO, sequence.data(), sequence.size() );
      }
      const bool reliable_data_pending = network->has_unsent_data();
      int wait_time = network->wait_time();
      wait_time = std::min( wait_time, std::max( network->bulk_wait_time(), files.wait_time( now, !forwarder.has_pending_network_data() ) ) );
      wait_time = std::min( wait_time, mascot.wait_time( now ) );
      wait_time = std::min( wait_time, mime_input.wait_time( now ) );
      if ( downloads_enabled && !tmux_control ) { wait_time = std::min( wait_time, downloads.wait_time( now, network->bulk_wait_time() ) ); }
      if ( mime_enabled ) { wait_time = std::min( wait_time, std::max( network->bulk_wait_time(), mime_clipboard.channel.wait_time( now, !forwarder.has_pending_network_data() && !reliable_data_pending ) ) ); }
      if ( !network->shutdown_in_progress() ) {
        wait_time = std::min( wait_time, control_panel.wait_time( now, network->get_sent_state_acked() ) );
      }
      if ( !tmux_parser.active() ) {
        wait_time = std::min( wait_time, overlays.wait_time() );
      }
      if ( !reliable_data_pending ) {
        wait_time = std::min( wait_time, forwarder.wait_time( now ) );
      }

      /* Handle startup "Connecting..." message */
      if ( still_connecting() ) {
        wait_time = std::min( 250, wait_time );
      }
      if ( bulk_control.has_outgoing() && !reliable_data_pending && !forwarder.has_pending_network_data()
           && wait_time > 20 ) {
        wait_time = std::min( wait_time, std::max( 20, network->bulk_wait_time() ) );
      }

      /* poll for events */
      /* network->fd() can in theory change over time */
      sel.clear_fds();
      if ( downloads.fd() >= 0 ) { sel.add_fd( downloads.fd() ); }
      std::vector<int> fd_list( network->fds() );
      for ( std::vector<int>::const_iterator it = fd_list.begin(); it != fd_list.end(); it++ ) {
        sel.add_fd( *it );
      }
      std::vector<int> forward_fds( forwarder.fds() );
      for ( std::vector<int>::const_iterator it = forward_fds.begin(); it != forward_fds.end(); it++ ) {
        sel.add_fd( *it );
      }
      std::vector<int> bulk_fds( bulk_control.fds() );
      for ( std::vector<int>::const_iterator it = bulk_fds.begin(); it != bulk_fds.end(); it++ ) {
        sel.add_fd( *it );
      }
      const bool monitor_stdin = !tmux_parser.active() || network->shutdown_in_progress()
                                 || network->get_current_state().tmux_input_size() < Terminal::TMUX_QUEUE_LIMIT;
      if ( monitor_stdin ) {
        sel.add_fd( STDIN_FILENO );
      }
      if ( control_panel.fd() >= 0 ) { sel.add_fd( control_panel.fd() ); }
      if ( files.fd() >= 0 ) { sel.add_fd( files.fd() ); }

      int active_fds = sel.select( wait_time );
      if ( active_fds < 0 ) {
        perror( "select" );
        break;
      }

      bool network_ready_to_read = false;

      for ( std::vector<int>::const_iterator it = fd_list.begin(); it != fd_list.end(); it++ ) {
        if ( sel.read( *it ) ) {
          /* packet received from the network */
          /* we only read one socket each run */
          network_ready_to_read = true;
        }
      }

      if ( network_ready_to_read ) {
        try { process_network_input(); }
        catch ( const Network::NetworkException& e ) {
          // Proxy control readiness need not produce an application packet.
          if ( e.the_errno != EAGAIN && e.the_errno != EWOULDBLOCK ) { throw; }
        }
      }

      for ( std::vector<int>::const_iterator it = forward_fds.begin(); it != forward_fds.end(); it++ ) {
        if ( sel.read( *it ) ) {
          forwarder.process_readable_fd( *it );
        }
      }

      for ( std::vector<int>::const_iterator it = bulk_fds.begin(); it != bulk_fds.end(); it++ ) {
        if ( sel.read( *it ) ) {
          bulk_control.process_readable_fd( *it );
        }
      }

      if ( monitor_stdin && sel.read( STDIN_FILENO )
           && !process_user_input( STDIN_FILENO ) ) { /* input from the user needs to be fed to the network */
        if ( !network->has_remote_addr() ) {
          break;
        } else if ( !network->shutdown_in_progress() ) {
          overlays.get_notification_engine().set_notification_string( std::wstring( L"Exiting..." ), true );
          network->start_shutdown();
        }
      }

      if ( sel.signal( SIGWINCH ) && !process_resize() ) { /* resize */
        return false;
      }

      if ( sel.signal( SIGCONT ) ) {
        resume();
      }

      if ( sel.signal( SIGTERM ) || sel.signal( SIGINT ) || sel.signal( SIGHUP ) || sel.signal( SIGPIPE ) ) {
        /* shutdown signal */
        if ( !network->has_remote_addr() ) {
          break;
        } else if ( !network->shutdown_in_progress() ) {
          overlays.get_notification_engine().set_notification_string(
            std::wstring( L"Signal received, shutting down..." ), true );
          network->start_shutdown();
        }
      }

      /* quit if our shutdown has been acknowledged */
      if ( network->shutdown_in_progress() && network->shutdown_acknowledged() ) {
        clean_shutdown = true;
        break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network->shutdown_in_progress() && network->shutdown_ack_timed_out() ) {
        break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network->counterparty_shutdown_ack_sent() ) {
        clean_shutdown = true;
        break;
      }

      /* write diagnostic message if can't reach server */
      if ( still_connecting() && ( !network->shutdown_in_progress() )
           && ( timestamp() - network->get_latest_remote_state().timestamp > 250 ) ) {
        if ( timestamp() - network->get_latest_remote_state().timestamp > 15000 ) {
          if ( !network->shutdown_in_progress() ) {
            overlays.get_notification_engine().set_notification_string(
              std::wstring( L"Timed out waiting for server..." ), true );
            network->start_shutdown();
          }
        } else {
          overlays.get_notification_engine().set_notification_string( connecting_notification );
        }
      } else if ( ( network->get_remote_state_num() != 0 )
                  && ( overlays.get_notification_engine().get_notification_string() == connecting_notification ) ) {
        overlays.get_notification_engine().set_notification_string( L"" );
      }

      if ( !network->shutdown_in_progress() ) {
        Network::StreamEvent event;
        if ( control_panel.take( timestamp(), network->get_sent_state_last(), network->get_sent_state_acked(), event ) ) {
          network->get_current_state().push_back( event );
          network->request_immediate_send();
        }
      }
      if ( !network->shutdown_in_progress() && !network->has_unsent_data() && !mime_clipboard.channel.has_interactive() ) {
        forwarder.flush(
          network->get_current_state(), timestamp(), network->send_interval(), network->max_datagram_payload() );
      }
      const int interactive_wait_before_tick = network->wait_time();
      network->tick();

      Network::Bulk::Datagram bulk;
      const bool sent_clipboard = mime_enabled && !network->shutdown_in_progress() && !network->bulk_wait_time()
        && mime_clipboard.channel.take_packet( Clipboard::Priority::Interactive, timestamp(), network->get_SRTT(), bulk );
      if ( sent_clipboard ) { network->send_bulk( bulk ); }
      const bool sent_download = !sent_clipboard && downloads_enabled && !tmux_control && !network->shutdown_in_progress() && !network->bulk_wait_time()
        && downloads.channel.take_packet( Clipboard::Priority::Interactive, timestamp(), network->get_SRTT(), bulk );
      if ( sent_download ) { network->send_bulk( bulk ); }
      const bool sent_files = !sent_clipboard && !sent_download && !network->shutdown_in_progress() && !network->bulk_wait_time()
        && files.take_packet( Clipboard::Priority::Interactive, timestamp(), network->get_SRTT(), bulk );
      if ( sent_files ) { network->send_bulk( bulk ); }
      // A dirty screen/input state may be waiting for its frame timer. Do
      // not reserve that entire interval: due foreground work ran above,
      // and the shared wire-byte pacer still bounds this one bulk packet.
      if ( interactive_wait_before_tick > 0 && network->wait_time() > 0
           && !sent_clipboard && !sent_download && !sent_files && !network->shutdown_in_progress()
           && !forwarder.has_pending_network_data() && !network->bulk_wait_time() ) {
        bool sent = false;
        const unsigned first = file_bulk_turn++ % 2;
        for ( unsigned i = 0; i < 2 && !sent; ++i ) {
          sent = ( first + i ) % 2 ? files.take_packet( Clipboard::Priority::Background, timestamp(), network->get_SRTT(), bulk )
                                   : bulk_control.pop_outgoing( bulk );
        }
        if ( sent ) { network->send_bulk( bulk ); }
      }

      std::string& send_error = network->get_send_error();
      if ( !send_error.empty() ) {
        overlays.get_notification_engine().set_network_error( send_error );
        send_error.clear();
      } else {
        overlays.get_notification_engine().clear_network_error();
      }
    } catch ( const Network::NetworkException& e ) {
      if ( !network->shutdown_in_progress() ) {
        overlays.get_notification_engine().set_network_error( e.what() );
      }

      struct timespec req;
      req.tv_sec = 0;
      req.tv_nsec = 200000000; /* 0.2 sec */
      nanosleep( &req, NULL );
      freeze_timestamp();
    } catch ( const Crypto::CryptoException& e ) {
      if ( e.fatal ) {
        throw;
      } else {
        wchar_t tmp[128];
        swprintf( tmp, 128, L"Crypto exception: %s", e.what() );
        overlays.get_notification_engine().set_notification_string( std::wstring( tmp ) );
      }
    }
  }
  return clean_shutdown;
}
