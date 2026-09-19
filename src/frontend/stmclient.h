/*
    Modified for Goblin Skiff on 2026-09-19.
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

#ifndef STM_CLIENT_HPP
#define STM_CLIENT_HPP

#include <memory>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <termios.h>

#include "src/frontend/streamforward.h"
#include "src/frontend/mascot.h"
#include "src/frontend/controlpanel.h"
#include "src/frontend/mimeclipboard.h"
#include "src/frontend/download.h"
#include "src/frontend/terminaloverlay.h"
#include "src/network/bulkcontrol.h"
#include "src/network/networktransport.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include <set>

class STMClient
{
private:
  std::string ip;
  std::string port;
  std::string key;
  std::string relay_keys {};
  std::string socks5_proxy {};
  Crypto::Mode crypto_mode;
  bool compact_keepalive;
  bool tmux_control;
  Terminal::TmuxControlParser tmux_parser;
  Mascot::Splash mascot;
  bool graphics_sent = false;
  bool sixel_state = getenv( "GOBLIN_SKIFF_SIXEL_STATE" ) && !strcmp( getenv( "GOBLIN_SKIFF_SIXEL_STATE" ), "1" );
  bool mime_negotiated = getenv( "GOBLIN_SKIFF_CLIPBOARD" ) && !strcmp( getenv( "GOBLIN_SKIFF_CLIPBOARD" ), "1" );
  bool mime_enabled = false;
  bool downloads_enabled = getenv( "GOBLIN_SKIFF_DOWNLOADS" ) && !strcmp( getenv( "GOBLIN_SKIFF_DOWNLOADS" ), "1" );
  Download::Receiver downloads { Download::downloads_directory(), crypto_mode };
  Clipboard::Endpoint mime_clipboard { false };
  Terminal::Osc5522InputFilter mime_input {};
  Control::Panel control_panel;
  Files::Endpoint files;
  unsigned file_bulk_turn = 0;

  int escape_key;
  int escape_pass_key;
  int escape_pass_key2;
  bool escape_requires_lf;
  std::wstring escape_key_help;
  std::string keyboard_escape_packet {}, keyboard_escape_release {};
  unsigned keyboard_escape_code = 0;
  std::set<unsigned> keyboard_local_keys {};
  bool input_bracketed_paste = false;

  struct termios saved_termios, raw_termios;

  struct winsize window_size;

  Terminal::Framebuffer local_framebuffer, new_state;
  Overlay::OverlayManager overlays;
  using NetworkType = Network::Transport<Network::UserStream, Terminal::Complete>;
  using NetworkPointer = std::shared_ptr<NetworkType>;
  NetworkPointer network;
  Terminal::Display display;
  Terminal::StartupScreen startup_screen;
  Terminal::Osc52InputFilter osc52_input;
  bool expecting_osc52_reply;
  StreamForwarder forwarder;
  Network::Bulk::ControlServer bulk_control;
  std::string state_sample_log;
  unsigned int state_sample_min_size;

  std::wstring connecting_notification;
  bool screen_initialized, repaint_requested, lf_entered, quit_sequence_started;
  bool clean_shutdown;
  unsigned int verbose;

  void main_init( void );
  void process_network_input( void );
  bool process_user_input( int fd );
  bool process_terminal_bytes( const std::string& bytes );
  bool process_download_filtered_bytes( const std::string& bytes );
  bool process_user_bytes( const std::string& bytes, bool panel_filtered = false );
  bool process_user_keys( const std::string& bytes, bool paste );
  bool is_command_key( const std::string& key );
  void dismiss_mascot();
  bool process_resize( void );

  void output_new_frame( void );

  bool still_connecting( void ) const
  {
    /* Initially, network == NULL */
    return network && ( network->get_remote_state_num() == 0 );
  }

  void resume( void ); /* restore state after SIGCONT */

public:
  STMClient( const char* s_ip,
             const char* s_port,
             const char* s_key,
             const char* predict_mode,
             unsigned int s_verbose,
             const char* predict_overwrite,
             const std::vector<std::string>& local_forwards,
             const std::vector<std::string>& dynamic_forwards,
             bool agent_forwarding,
             bool x11_forwarding,
             unsigned int stream_delay_ms,
             unsigned int stream_rate_bytes_per_second,
             const std::string& s_state_sample_log,
             unsigned int s_state_sample_min_size,
             bool s_compact_keepalive,
             Crypto::Mode s_crypto_mode,
             bool s_tmux_control,
             Mascot::Format mascot_format,
             bool allow_kitty,
             bool allow_sixel )
    : ip( s_ip ? s_ip : "" ), port( s_port ? s_port : "" ), key( s_key ? s_key : "" ),
      crypto_mode( s_crypto_mode ), compact_keepalive( s_compact_keepalive ),
      tmux_control( s_tmux_control ), tmux_parser(), mascot( mascot_format, allow_kitty, allow_sixel ),
      control_panel( getenv( "GOBLIN_SKIFF_DIRECTORY" ) && ( !strcmp( getenv( "GOBLIN_SKIFF_DIRECTORY" ), "1" ) || !strcmp( getenv( "GOBLIN_SKIFF_DIRECTORY" ), "2" ) ),
                     getenv( "GOBLIN_SKIFF_DIRECTORY" ) && !strcmp( getenv( "GOBLIN_SKIFF_DIRECTORY" ), "2" ) ),
      files( false, !s_tmux_control && getenv( "GOBLIN_SKIFF_FILES" ) && !strcmp( getenv( "GOBLIN_SKIFF_FILES" ), "1" ), s_crypto_mode ), escape_key( 0x1E ),
      escape_pass_key( '^' ), escape_pass_key2( '^' ), escape_requires_lf( false ), escape_key_help( L"?" ),
      saved_termios(), raw_termios(), window_size(), local_framebuffer( 1, 1 ), new_state( 1, 1 ), overlays(),
      network(), display( true, allow_kitty ) /* use TERM environment var to initialize display */,
      startup_screen(), osc52_input(), expecting_osc52_reply( false ),
      forwarder( StreamForwarder::ClientSide, stream_delay_ms, stream_rate_bytes_per_second, s_crypto_mode ),
      bulk_control( "client" ), state_sample_log( s_state_sample_log ), state_sample_min_size( s_state_sample_min_size ),
      connecting_notification(),
      screen_initialized( false ), repaint_requested( false ), lf_entered( false ), quit_sequence_started( false ), clean_shutdown( false ),
      verbose( s_verbose )
  {
    control_panel.set_files( &files );
    overlays.get_notification_engine().set_keepalive_interval(
      compact_keepalive ? Network::KEEPALIVE_INTERVAL_MIN : Network::ACK_INTERVAL );

    std::string error;
    for ( std::vector<std::string>::const_iterator it = local_forwards.begin(); it != local_forwards.end(); it++ ) {
      if ( !forwarder.add_tcp_forward( *it, error ) ) {
        fprintf( stderr, "Bad -L forwarding spec: %s\n", error.c_str() );
        exit( 1 );
      }
    }
    for ( std::vector<std::string>::const_iterator it = dynamic_forwards.begin(); it != dynamic_forwards.end(); it++ ) {
      if ( !forwarder.add_dynamic_forward( *it, error ) ) {
        fprintf( stderr, "Bad -D forwarding spec: %s\n", error.c_str() );
        exit( 1 );
      }
    }
    if ( agent_forwarding && !forwarder.enable_agent_forwarding( error ) ) {
      fprintf( stderr, "Cannot enable agent forwarding: %s\n", error.c_str() );
      exit( 1 );
    }
    if ( x11_forwarding && !forwarder.enable_x11_forwarding( error ) ) {
      fprintf( stderr, "Cannot enable X11 forwarding: %s\n", error.c_str() );
      exit( 1 );
    }

    if ( predict_mode ) {
      if ( !strcmp( predict_mode, "always" ) ) {
        overlays.get_prediction_engine().set_display_preference( Overlay::PredictionEngine::Always );
      } else if ( !strcmp( predict_mode, "never" ) ) {
        overlays.get_prediction_engine().set_display_preference( Overlay::PredictionEngine::Never );
      } else if ( !strcmp( predict_mode, "adaptive" ) ) {
        overlays.get_prediction_engine().set_display_preference( Overlay::PredictionEngine::Adaptive );
      } else if ( !strcmp( predict_mode, "experimental" ) ) {
        overlays.get_prediction_engine().set_display_preference( Overlay::PredictionEngine::Experimental );
      } else {
        fprintf( stderr, "Unknown prediction mode %s.\n", predict_mode );
        exit( 1 );
      }
    }
    if ( predict_overwrite && !strcmp( predict_overwrite, "yes" ) ) {
      overlays.get_prediction_engine().set_predict_overwrite( true );
    }
  }

  void init( void );
  void set_relay_keys( const std::string& keys ) { relay_keys = keys; }
  void set_socks5_proxy( const std::string& proxy ) { socks5_proxy = proxy; }
  void shutdown( void );
  bool main( void );

  /* unused */
  STMClient( const STMClient& );
  STMClient& operator=( const STMClient& );
};

#endif
