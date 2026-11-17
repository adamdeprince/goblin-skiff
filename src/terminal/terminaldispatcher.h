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

#ifndef TERMINALDISPATCHER_HPP
#define TERMINALDISPATCHER_HPP
#include "goblin_pointer.hpp"

#include <map>
#include <string>
#include <vector>

#include "src/terminal/extensions.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/osc52.h"
#include "src/terminal/terminalgeometry.h"
#include "src/terminal/sixel.h"

namespace Parser {
class Action;
class Param;
class Collect;
class Clear;
class Esc_Dispatch;
class CSI_Dispatch;
class Execute;
class OSC_Start;
class OSC_Put;
class OSC_End;
class APC_Start;
class APC_Put;
class APC_End;
}

namespace Terminal {
class Framebuffer;
class Dispatcher;

enum Function_Type
{
  ESCAPE,
  CSI,
  CONTROL
};

class Function
{
public:
  Function() : function( NULL ), clears_wrap_state( true ) {}
  Function( Function_Type type,
            const std::string& dispatch_chars,
            void ( *s_function )( Framebuffer*, Dispatcher* ),
            bool s_clears_wrap_state = true );
  void ( *function )( Framebuffer*, Dispatcher* );
  bool clears_wrap_state;
};

using dispatch_map_t = std::map<std::string, Function>;

class DispatchRegistry
{
public:
  dispatch_map_t escape;
  dispatch_map_t CSI;
  dispatch_map_t control;

  DispatchRegistry() : escape(), CSI(), control() {}
};

DispatchRegistry& get_global_dispatch_registry( void );

class Dispatcher
{
private:
  std::string params;
  std::vector<int> parsed_params;
  std::vector<std::vector<int>> parsed_subparams;
  bool parsed;

  std::string dispatch_chars;
  std::vector<wchar_t> OSC_string;
  bool OSC_overflow;
  bool csi_overflow = false;
  bool APC_overflow = false;
  std::vector<ClipboardEvent> clipboard_events;
  std::string APC_string;
  bool kitty_uploading;
  bool kitty_chunked = false;
  KittyCommand kitty_partial;
  std::string kitty_payload;
  const ClientGeometry* client_geometry;
  std::string DCS_string {};
  bool DCS_active = false, DCS_escape = false, DCS_just_escaped = false;
  bool DCS_termcap = false; /* collecting an XTGETTCAP request, not sixel */
  Sixel::Palette sixel_palette {};

  void parse_params( void );
  void finish_kitty_upload( Framebuffer* fb );
  void finish_vector_edit( const KittyCommand& cmd, const std::string& data, Framebuffer* fb );
  void refresh_gpu_surface( const KittyCommand& cmd, Framebuffer* fb );
  /* a=f, a=c and a=a: the frames of an image, as far as an image of one
     frame has them. */
  void finish_animation_frame( const KittyCommand& cmd, const std::string& data, Framebuffer* fb );
  void compose_frames( const KittyCommand& cmd, Framebuffer* fb );
  void control_animation( const KittyCommand& cmd, Framebuffer* fb );
  void finish_sixel( Framebuffer* fb );
  void finish_dcs( Framebuffer* fb );
  void finish_termcap();

public:
  /* An image whose pixels were edited in place, sent as the rectangle that
     changed to a peer that has the version before. */
  bool replace_image_pixels( const KittyImage& existing, const std::string& before, const std::string& after,
                             Framebuffer* fb, std::string& error );
  bool sixel_enabled = false;
  bool mime_clipboard_enabled = false;
  bool pointer_available = false;
  std::string pending_pointer {};
  void pointer_dispatch( const std::string& body, Framebuffer* fb );
  /* OSC frames that are acted on only once ST completes them: the pointer
     protocol and Silkie's extensions. */
  void finish_st_frame( bool complete, Framebuffer* fb );
  std::string pending_extension {};
  ExtensionRelay ext_relay;
  std::vector<std::string> mime_clipboard_events {};
  std::vector<std::string> download_events {};
  std::string pending_download {};
  void finish_download( bool complete )
  {
    if ( complete && !pending_download.empty() ) { download_events.push_back( pending_download ); }
    pending_download.clear();
  }
  bool text_sizing_enabled = true; // state decoder always understands OSC 66
  bool keyboard_enabled = false;
  bool keyboard_alt = false;
  std::vector<unsigned> keyboard_stack[2] {};
  void keyboard_mode( Framebuffer* fb, char operation );
  void keyboard_screen( Framebuffer* fb, bool alternate );
  bool sixel_display_mode = false, sixel_private_palette = true, sixel_cursor_right = false;
  void DCS_start( wchar_t final );
  void DCS_put( wchar_t ch );
  void DCS_end( wchar_t ch, Framebuffer* fb );
  void DCS_escape_end( wchar_t ch, Framebuffer* fb );
  void reset_sixel();
  /* A CSI or OSC/APC/DCS longer than csi_byte_limit / escape_byte_limit
     (when those are not zero) is ignored. Zero means no limit. */

  std::string terminal_to_host; /* this is the reply string */

  Dispatcher();
  int getparam( size_t N, int defaultval );
  int param_count( void );
  bool has_subparams( void );
  int subparam_count( size_t N );
  /* Sub-parameter K of parameter N; an empty one is defaultval. */
  int getsubparam( size_t N, size_t K, int defaultval );

  void newparamchar( const Parser::Param* act );
  void collect( const Parser::Collect* act );
  void clear( const Parser::Clear* act );

  std::string str( void );

  void dispatch( Function_Type type, const Parser::Action* act, Framebuffer* fb );
  std::string get_dispatch_chars( void ) const { return dispatch_chars; }
  std::vector<wchar_t> get_OSC_string( void ) const { return OSC_string; }

  void OSC_put( const Parser::OSC_Put* act );
  void OSC_start( const Parser::OSC_Start* act );
  void OSC_dispatch( const Parser::OSC_End* act, Framebuffer* fb );

  void APC_put( const Parser::APC_Put* act );
  void APC_start( const Parser::APC_Start* act );
  void APC_dispatch( const Parser::APC_End* act, Framebuffer* fb );

  std::vector<ClipboardEvent> take_clipboard_events( void );

  void set_client_geometry( const ClientGeometry* geometry ) { client_geometry = geometry; }
  const ClientGeometry* get_client_geometry( void ) const { return client_geometry; }

  bool operator==( const Dispatcher& x ) const;
};
}

#endif
