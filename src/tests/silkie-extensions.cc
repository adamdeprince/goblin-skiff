/* Distributed under the GNU GPL, version 3 or later. */
/* Silkie's OSC 777 extensions, as Goblin Skiff relays them. */
#include <algorithm>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstring>
#include <png.h>
#include <zlib.h>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>

#include "src/protobufs/userinput.pb.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/extensions.h"
#include "src/statesync/user.h"
#include "src/terminal/colorspace.h"
#include "src/terminal/extframe.h"
#include "src/terminal/gpusurface.h"
#include "src/terminal/handmotion.h"
#include "src/terminal/inputlimits.h"
#include "src/terminal/motioncodec.h"
#include "src/terminal/packedimage.h"
#include "src/terminal/terminaldisplay.h"
#include "src/terminal/vectorscene.h"

using namespace Terminal;
namespace {
void require( bool ok, const std::string& message )
{
  if ( !ok ) {
    throw std::runtime_error( message );
  }
}

std::string osc( const std::string& selector, const std::string& fields )
{
  return "\033]777;" + selector + ";" + fields + "\033\\";
}

std::string base64( const std::string& in )
{
  static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for ( size_t i = 0; i < in.size(); i += 3 ) {
    unsigned v = ( unsigned char )in[i] << 16;
    if ( i + 1 < in.size() ) { v |= ( unsigned char )in[i + 1] << 8; }
    if ( i + 2 < in.size() ) { v |= ( unsigned char )in[i + 2]; }
    out += digits[v >> 18 & 63];
    out += digits[v >> 12 & 63];
    out += i + 1 < in.size() ? digits[v >> 6 & 63] : '=';
    out += i + 2 < in.size() ? digits[v & 63] : '=';
  }
  return out;
}

/* The frames in what the terminal sent a program, in order. */
std::vector<Ext::Frame> frames( const std::string& bytes )
{
  Ext::InputDemux demux;
  Ext::InputDemux::Output out;
  demux.feed( bytes, out );
  std::vector<Ext::Frame> result;
  for ( const auto& item : out.items ) {
    Ext::Frame f;
    if ( item.first && Ext::parse( Ext::frame_payload( item.second ), f ) ) {
      result.push_back( f );
    }
  }
  return result;
}

std::string field( const Ext::Frame& f, const std::string& key )
{
  const std::string* value = f.fields.find( key );
  return value ? *value : "<absent>";
}

/* Silkie's answers to the client's probe, as a client takes them in. */
ClientExtensions silkie()
{
  ClientExtensions info;
  const std::string replies[] = {
    osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1,display.1,frame.1,gesture.1,haptic.1,ink.1,latency.1,"
                 "scroll.1,tablet-pointer.1:media=d,f,t,s:format=24,32,64,100,200" ),
    osc( "tablet-pointer", "v=1:op=cap:id=7253:caps=101:mode=0:w=800:h=480:cw=10:ch=20" ),
    osc( "scroll", "v=1:op=cap:id=7253:caps=1:mode=0" ),
    osc( "gesture", "v=1:op=cap:id=7253:caps=15:mode=0" ),
    osc( "frame", "v=1:op=cap:id=7253:caps=1:mode=0:period=8333" ),
    osc( "latency", "v=1:op=cap:id=7253:caps=1:mode=0:input=3000:repaint=10000" ),
    osc( "display", "v=1:op=cap:id=7253:mode=0:kind=lcd:color=rgb:bits=8:gamut=p3:headroom=2:scale=2:period=8333:ppi=254" ),
    osc( "haptic", "v=1:op=cap:id=7253:caps=1" ),
    osc( "ink", "v=1:op=cap:id=7253:caps=1:mode=0" ),
  };
  for ( const auto& reply : replies ) {
    require( consume_probe_reply( reply, info ), "probe reply taken in: " + reply );
  }
  info.grants[Selector::TABLET] = 119; /* the user refused the hardware ids */
  info.grants[Selector::GESTURE] = 3;
  std::string shapes = "text,default";
  for ( int i = 0; i < 31; i++ ) {
    shapes += ",1";
  }
  require( consume_probe_reply( "\033]22;" + shapes + "\033\\", info ), "pointer shape reply" );
  require( !info.image_gamut, "the listing does not say whether G is taken" );
  info.image_gamut = true; /* its answer to the client's query of a pixel with G=1 */
  return info;
}

void framing()
{
  Ext::Frame f;
  require( Ext::parse_program( "777;caps;v=1:op=query:id=42", f ) && f.selector == "caps", "a program frame" );
  require( !Ext::parse_program( "777;caps;v=1:op=query:id=42:id=43", f ), "a repeated key is rejected" );
  require( !Ext::parse_program( "777;caps;op=query", f ), "the version is required" );
  require( !Ext::parse_program( "777;caps;v=1:op=query:", f ), "a trailing separator is rejected" );
  require( Ext::parse_program( "777;caps;v=1:op=query:pad=" + std::string( 100000, 'x' ), f ),
           "fields have no limit of their own" );
  int64_t n = 0;
  require( !Ext::parse_integer( "+1", 0, 9, n ) && Ext::parse_integer( "-5", -9, 9, n ) && n == -5
             && !Ext::parse_integer( "-5", 0, 9, n ),
           "integers as Silkie reads them" );
  require( Ext::number( 0.1 ) == "0.1" && Ext::number( 1 ) == "1", "shortest numbers" );

  /* Frames among keystrokes, split across reads, and the paste exception. */
  Ext::InputDemux demux;
  Ext::InputDemux::Output out;
  demux.feed( "ab\033]777;scroll;v=1:op=ev", out );
  demux.feed( "ent\033\\cd\033[200~\033]777;x\033[201~\033", out );
  require( out.items.size() == 3 && !out.items[0].first && out.items[0].second == "ab" && out.items[1].first
             && out.items[2].second == "cd\033[200~\033]777;x\033[201~",
           "demultiplexed input" );
  require( demux.pending() && !demux.in_frame(), "a lone escape is held" );
  Ext::InputDemux::Output expired;
  demux.expire( expired );
  require( expired.items.size() == 1 && expired.items[0].second == "\033", "and released when it expires" );
  Ext::InputDemux::Output cancelled;
  demux.feed( "\033]777;scroll;v=1\030x", cancelled );
  require( cancelled.items.size() == 1 && cancelled.items[0].second == "x", "CAN cancels a frame" );
}

void capabilities()
{
  Complete server( 80, 24 );
  ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  require( server.act( osc( "caps", "v=1:op=query:id=1" ) ).empty(), "nothing is answered before the client says" );
  server.set_client_extensions( silkie() );
  auto reply = frames( server.act( osc( "caps", "v=1:op=query:id=9" ) ) );
  require( reply.size() == 1 && field( reply[0], "id" ) == "9" && field( reply[0], "caps" ) == "3",
           "the listing" );
  require( field( reply[0], "ext" )
             == "caps.1,ssh-agent.1,display.1,frame.1,gesture.1,haptic.1,ink.1,latency.1,scroll.1,tablet-pointer.1",
           "the listing intersects the client's: " + field( reply[0], "ext" ) );
  require( field( reply[0], "format" ) == "301,302,304,308,516,24,32,64,100,200"
             && field( reply[0], "media" ) == ( gpu_surfaces_supported() ? "d,f,t,s,g" : "d,f,t,s" ),
           "formats and media" );

  reply = frames( server.act( osc( "tablet-pointer", "v=1:op=query:id=4" ), &geometry ) );
  require( reply.size() == 1 && field( reply[0], "caps" ) == "101" && field( reply[0], "w" ) == "800"
             && field( reply[0], "ch" ) == "20",
           "tablet capabilities and the cell area" );
  require( server.act( osc( "tablet-pointer", "v=2:op=query:id=4" ) ).empty(), "an unknown version is ignored" );

  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=255" ) );
  require( server.get_fb().ds.ext.tablet == 119, "refused and unknown bits are dropped" );
  reply = frames( server.act( osc( "tablet-pointer", "v=1:op=mode" ) ) );
  require( reply.size() == 1 && field( reply[0], "mode" ) == "119", "the mode reads back" );

  reply = frames( server.act( osc( "latency", "v=1:op=query:id=2" ) ) );
  require( field( reply[0], "input" ) == "11000" && field( reply[0], "repaint" ) == "110000",
           "latency adds the relay's waits to the terminal's" );
  server.act( osc( "latency", "v=1:op=mode:mode=1" ) );
  require( server.get_fb().ds.ext.latency, "latency mode" );

  reply = frames( server.act( osc( "frame", "v=1:op=query:id=3" ) ) );
  require( field( reply[0], "period" ) == "8333", "the refresh period" );

  /* Reset turns every mode off. */
  server.act( osc( "scroll", "v=1:op=mode:mode=1" ) + osc( "gesture", "v=1:op=mode:mode=3" ) );
  require( server.get_fb().ds.ext.scroll && server.get_fb().ds.ext.gesture == 3, "modes on" );
  server.act( osc( "caps", "v=1:op=reset" ) );
  require( !server.get_fb().ds.ext.reports_input() && !server.get_fb().ds.ext.latency, "caps reset" );
  server.act( osc( "scroll", "v=1:op=mode:mode=1" ) );
  server.act( "\033]133;A\033\\" );
  require( !server.get_fb().ds.ext.scroll, "a shell prompt resets the modes" );
  server.act( osc( "scroll", "v=1:op=mode:mode=1" ) + "\033c" );
  require( !server.get_fb().ds.ext.scroll, "RIS resets the modes" );
  server.act( "\033]777;scroll;v=1:op=mode:mode=1\033X" );
  require( !server.get_fb().ds.ext.scroll, "a frame without ST is not acted on" );
}

void reports()
{
  Complete server( 80, 24 );
  ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  server.set_client_extensions( silkie() );
  const std::string down = "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=1000:contact=0:x=2560:y=5120:"
                           "buttons=1:mods=0:tool=1:valid=1:p=100";
  const std::string move = "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:t=1004:contact=0:x=2600:y=5120:"
                           "buttons=1:mods=0:tool=1:valid=1:p=200";
  require( server.extension_input( ExtensionInput( down ) ).empty(), "nothing is reported before the mode" );
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" ) );
  require( server.extension_input( ExtensionInput( down ) ) == "\033]" + down + "\033\\", "a report reaches the program" );
  require( server.extension_contacts_live(), "the stroke is open" );

  /* Changing the mode cancels the stroke, and the rest of it is dropped. */
  auto cancel = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=3" ) ) );
  require( cancel.size() == 1 && field( cancel[0], "phase" ) == "4" && field( cancel[0], "buttons" ) == "0"
             && field( cancel[0], "p" ) == "0" && field( cancel[0], "x" ) == "2560",
           "the relay cancels a live contact" );
  require( server.extension_input( ExtensionInput( move ) ).empty(), "the rest of a cancelled stroke" );
  require( !server.extension_input( ExtensionInput( down ) ).empty(), "a fresh down begins again" );

  /* Hardware ids only with their bit. */
  const std::string device = "777;tablet-pointer;v=1:op=device:device=3:kind=1:tool=1:serial=1234:hwid=5:name=QQ==";
  auto described = frames( server.extension_input( ExtensionInput( device ) ) );
  require( described.size() == 1 && field( described[0], "serial" ) == "<absent>"
             && field( described[0], "name" ) == "QQ==",
           "serial numbers are kept from a program that did not ask" );

  /* A pad key that is down is released when the mode changes. */
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=5" ) );
  server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=3:device=5:t=2000:mods=0:button=2:state=1" ) );
  auto released = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=0" ) ) );
  require( released.size() == 1 && field( released[0], "button" ) == "2" && field( released[0], "state" ) == "0"
             && field( released[0], "t" ) == "2000",
           "pad keys are released with the time of their press" );

  /* Detaching cancels too. */
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=2" ) );
  server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=2:phase=1:device=0:t=10:contact=4:x=0:y=0:buttons=1:"
                          "mods=0:tool=0:valid=0" ) );
  auto detached = frames( server.extension_detach() );
  require( detached.size() == 2 && field( detached[0], "phase" ) == "4" && field( detached[1], "phase" ) == "6",
           "a finger is cancelled, then its frame" );

  /* Scroll and gesture reports follow their modes. */
  const std::string scroll = "777;scroll;v=1:op=event:t=5:dx=0:dy=250:x=0:y=0:mods=0:src=1:phase=2";
  require( server.extension_input( ExtensionInput( scroll ) ).empty(), "scroll without its mode" );
  server.act( osc( "scroll", "v=1:op=mode:mode=1" ) );
  require( !server.extension_input( ExtensionInput( scroll ) ).empty(), "scroll with its mode" );
  server.act( osc( "gesture", "v=1:op=mode:mode=1" ) );
  require( server.extension_input( ExtensionInput( "777;gesture;v=1:op=event:kind=5:phase=1:t=5:x=0:y=0:mods=0" ) ).empty(),
           "a hold needs its own bit" );
  require( !server.extension_input( ExtensionInput( "777;gesture;v=1:op=event:kind=1:phase=1:t=5:x=0:y=0:mods=0:scale=0:total=1000" ) )
              .empty(),
           "a pinch" );
  auto gesture_cancel = frames( server.act( osc( "gesture", "v=1:op=mode:mode=0" ) ) );
  require( gesture_cancel.size() == 1 && field( gesture_cancel[0], "phase" ) == "4"
             && field( gesture_cancel[0], "total" ) == "1000",
           "a gesture in progress is cancelled" );

  /* Frame reports are counted by the relay. */
  const std::string presented = "777;frame;v=1:op=event:seq=90:t=7:tu=3:period=8333";
  require( server.extension_input( ExtensionInput( presented ) ).empty(), "no frame reports without the mode" );
  server.act( osc( "frame", "v=1:op=mode:mode=1" ) );
  auto first = frames( server.extension_input( ExtensionInput( presented ) ) );
  auto second = frames( server.extension_input( ExtensionInput( presented ) ) );
  require( field( first[0], "seq" ) == "1" && field( second[0], "seq" ) == "2" && field( first[0], "tu" ) == "3",
           "frame reports are numbered from the mode" );
  const uint32_t before = server.get_fb().ds.ext.frame_output;
  server.extension_output_written();
  require( server.get_fb().ds.ext.frame_output == before + 1, "output while frame reports are on is counted" );
}

void display_and_haptic()
{
  Complete server( 80, 24 );
  server.set_client_extensions( silkie() );
  auto event = frames( server.act( osc( "display", "v=1:op=mode:mode=1" ) ) );
  require( event.size() == 1 && field( event[0], "op" ) == "event" && field( event[0], "gamut" ) == "p3"
             && field( event[0], "t" ) != "<absent>",
           "turning display reports on reports the display" );
  ClientExtensions moved = silkie();
  moved.display.set( "scale", "1" );
  event = frames( server.set_client_extensions( moved ) );
  require( event.size() == 1 && field( event[0], "scale" ) == "1", "a change of display is reported" );
  require( server.set_client_extensions( moved ).empty(), "and only a change" );

  server.act( osc( "haptic", "v=1:op=play:pattern=alignment" ) + osc( "haptic", "v=1:op=play:pattern=thud" )
              + osc( "haptic", "v=1:op=start:target=pen:pattern=pencil:intensity=60" )
              + osc( "haptic", "v=1:op=start:pattern=pencil:intensity=160" ) );
  auto outputs = server.take_extension_outputs();
  require( outputs.size() == 2 && outputs[0] == osc( "haptic", "v=1:op=play:pattern=alignment" ),
           "haptic requests are passed on, the malformed ones dropped" );
  auto cap = frames( server.act( osc( "haptic", "v=1:op=query:id=5" ) ) );
  require( cap.size() == 1 && field( cap[0], "caps" ) == "1", "haptic capabilities" );

  std::string reply = server.act( "\033P+q6b697474792d71756572792d6864725f68656164726f6f6d\033\\" );
  require( reply == "\033P1+r6b697474792d71756572792d6864725f68656164726f6f6d=32\033\\",
           "the HDR headroom through XTGETTCAP: " + reply );
}

void sync_and_render()
{
  Complete server( 80, 24 ), client( 80, 24 );
  server.set_client_extensions( silkie() );
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=3" ) + osc( "scroll", "v=1:op=mode:mode=1" )
              + osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5" ) + "\033]22;>pointer,wait\033\\" );
  client.apply_string( server.diff_from( client ) );
  require( client.get_fb().ds.ext == server.get_fb().ds.ext, "extension state reaches the client" );
  require( server.diff_from( client ).empty(), "and is not sent again" );

  Display display( false );
  display.set_extensions( silkie() );
  Framebuffer blank( 80, 24 );
  const std::string out = display.new_frame( false, blank, client.get_fb() );
  for ( const std::string& expected :
        { osc( "tablet-pointer", "v=1:op=mode:mode=3" ), osc( "scroll", "v=1:op=mode:mode=1" ),
          osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5" ), std::string( "\033]22;wait\033\\" ) } ) {
    require( out.find( expected ) != std::string::npos, "the client's terminal is sent " + expected );
  }
  require( out.find( osc( "ink", "v=1:op=mode:mode=1" ) ) == std::string::npos, "a brush turns ink on by itself" );
  const std::string again = display.new_frame( true, client.get_fb(), client.get_fb() );
  require( again.find( "777;" ) == std::string::npos, "unchanged modes are not sent again" );
  require( display.close().find( osc( "tablet-pointer", "v=1:op=mode:mode=0" ) ) != std::string::npos,
           "and turned off when the client lets go" );

  /* Pointer shapes */
  auto answer = server.act( "\033]22;?__current__,pointer,no-such-name,image:9:1:1,__default__\033\\" );
  require( answer == "\033]22;wait,1,0,1,text\033\\", "pointer shape queries: " + answer );
  server.act( "\033]22;<\033\\" );
  require( server.get_fb().ds.ext.pointer_shape() == "pointer", "pop" );
  server.act( "\033]22;image:9:24:24\033\\" );
  require( server.get_fb().ds.ext.pointer_shape() == "image:9:24:24", "an image shape replaces the top" );
  server.act( "\033[?1049h" );
  require( server.get_fb().ds.ext.pointer_shape().empty(), "the alternate screen has its own stack" );

  /* One-shot frames ride their own channel. */
  server.take_extension_outputs(); /* the refreshes the queries above asked for */
  server.act( osc( "haptic", "v=1:op=play:pattern=click" ) );
  for ( const auto& frame : server.take_extension_outputs() ) {
    server.push_extension_output( frame );
  }
  Complete received( 80, 24 );
  received.apply_string( server.diff_from( Complete( 80, 24 ) ) );
  require( received.get_extension_outputs().size() == 1, "haptic requests reach the client once" );

  /* Reports travel as their own events, never as keystrokes. */
  Network::UserStream input, empty, arrived;
  input.push_back( ExtensionInput( "777;scroll;v=1:op=event:t=1:dx=0:dy=1:x=0:y=0:mods=0:src=1:phase=0" ) );
  input.push_back( silkie() );
  arrived.apply_string( input.diff_from( empty ) );
  require( arrived.size() == 2 && arrived.is_extension_input( 0 ) && arrived.is_extensions_event( 1 )
             && arrived.get_extensions_event( 1 ) == silkie(),
           "user stream round trip" );
}

std::string& error_sink()
{
  static std::string error;
  return error;
}

std::string kitty( const std::string& controls, const std::string& payload )
{
  return "\033_G" + controls + ";" + base64( payload ) + "\033\\";
}

void vectors()
{
  Complete server( 80, 24 ), client( 80, 24 );
  require( server.act( kitty( "a=t,f=200,i=1,s=10,v=10", "p id=1 d=M0,0L1,1" ) ).find( "EINVAL" ) != std::string::npos,
           "vector images need a terminal that draws them" );
  server.set_client_extensions( silkie() );
  std::string reply = server.act( kitty( "a=t,f=200,i=7,s=400,v=300", "p id=1 d=M0,0L1,1 fill=#ff0000\np id=2 d=Q" ) );
  require( reply.find( "EINVAL:Line 2: Bad curveto in path" ) != std::string::npos, "malformed scene: " + reply );
  reply = server.act( kitty( "a=T,f=200,i=7,s=400,v=300", "p id=1 d=M0,0L1,1 fill=#ff0000\nk id=2 pts=1,2,3" ) );
  require( reply.find( "OK" ) != std::string::npos, "a scene" );
  client.apply_string( server.diff_from( client ) );
  const KittyImage* image = client.get_fb().find_kitty_image( 7 );
  require( image && image->format == KITTY_FORMAT_VECTOR
             && kitty_vector_records( *image ) == "p id=1 d=M0,0L1,1 fill=#ff0000\nk id=2 pts=1,2,3\n",
           "the scene reaches the client" );
  const Framebuffer shown = client.get_fb();

  reply = server.act( kitty( "a=e,i=7", "a id=2 pts=4,5,6\nx id=1" ) );
  require( reply.find( "OK" ) != std::string::npos, "a patch: " + reply );
  require( server.act( kitty( "a=e,i=8", "x all" ) ).find( "ENOENT" ) != std::string::npos, "no such image" );
  const std::string diff = server.diff_from( client );
  client.apply_string( diff );
  require( kitty_vector_records( *client.get_fb().find_kitty_image( 7 ) ) == "k id=2 pts=1,2,3,4,5,6\n",
           "the patch reaches the client" );
  require( diff.find( "p id=1" ) == std::string::npos, "as a patch, not the scene again" );

  /* The client's terminal is sent the patch, not the scene. */
  std::string out;
  append_kitty_frame( out, true, shown, client.get_fb() );
  require( out.find( "a=e" ) != std::string::npos && out.find( "a=d" ) == std::string::npos
             && out.find( base64( "a id=2 pts=4,5,6\nx id=1\n" ) ) != std::string::npos,
           "a=e to the client's terminal: " + out );

  /* Groups and transforms */
  Vector::Table table;
  std::vector<Vector::Record> records;
  std::string error;
  require( Vector::parse_records( "g id=0 t=2,0,0,2,0,0\ng id=5 t=1,0,0,1,10,20\ng id=6 p=5\np id=9 g=6 d=M0,0", records,
                                  error ),
           error );
  table.apply( records );
  double x = 30, y = 60;
  require( table.point_to_group( 6, x, y ) && x == 5 && y == 10, "a point into a nested group" );
  require( Vector::parse_records( "x g=5", records, error ) && table.apply( records ).empty() && table.size() == 0,
           "deleting a group deletes what is in it" );
}

/* The client side: it sees its terminal's reports against what that
   terminal shows, and the server applies what it works out. */
struct Pair
{
  Complete server { 80, 24 }, client { 80, 24 };
  ExtensionClient terminal;
  ClientGeometry geometry { 80, 24, 800, 480, 10, 20 };
  Framebuffer shown { 80, 24 };
  uint64_t now = 1000;

  Pair()
  {
    server.set_client_extensions( silkie() );
    terminal.info = silkie();
  }
  /* The client takes in the server's state and draws it to its terminal. */
  void draw()
  {
    client.apply_string( server.diff_from( client ) );
    Display display( false );
    display.set_extensions( silkie() );
    display.set_ink_hint( terminal.ink_hint( client.get_fb() ) );
    display.new_frame( true, shown, client.get_fb() );
    std::vector<std::pair<std::string, uint32_t>> modes;
    InkBrush brush;
    bool resent = false;
    display.take_drawn( modes, brush, resent );
    terminal.drawn( shown, client.get_fb(), modes, brush.present() ? &brush : nullptr, resent );
    shown = client.get_fb();
  }
  /* A report from the terminal, through the client to the server. */
  ExtensionClient::Result report( const std::string& payload, std::string* to_program = nullptr )
  {
    auto result = terminal.frame( "\033]" + payload + "\033\\", shown, geometry, now );
    if ( result.forward ) {
      const std::string sent = server.extension_input( result.input );
      if ( to_program ) {
        *to_program = sent;
      }
    }
    return result;
  }
};

std::vector<double> numbers_of( const std::string& pts )
{
  std::vector<double> out;
  const char* at = pts.c_str();
  while ( *at ) {
    char* end;
    out.push_back( strtod( at, &end ) );
    at = *end == ',' ? end + 1 : end;
  }
  return out;
}

void ink()
{
  /* The example of the ink protocol's specification. */
  Pair pair;
  pair.server.act( kitty( "a=t,f=200,i=7,s=400,v=300", "" ) );
  pair.server.act( "\033[3;4H" + kitty( "a=p,i=7", "" ), &pair.geometry ); /* 30 pixels across and 40 down */
  pair.server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" )
                   + osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5" ) );
  pair.draw();
  const std::string down = "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=408211:contact=0:x=25600:"
                           "y=51200:buttons=1:mods=0:tool=1:valid=1:p=13107:ink=1000";
  std::string program;
  auto result = pair.report( down, &program );
  require( result.forward && program == "\033]" + down + "\033\\", "the down keeps its ink" );
  require( result.input.ink_image == 7 && result.input.ink_records.compare( 0, 13, "k id=1000 pts" ) == 0,
           "the client works out the stroke: " + result.input.ink_records );
  require( result.to_terminal.empty(), "the terminal drew it; it is not told again" );
  pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:t=408215:contact=0:x=26880:y=51712:"
               "buttons=1:mods=0:tool=1:valid=1:p=39321" );
  pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=3:device=3:t=408219:contact=0:x=27136:y=51712:"
               "buttons=0:mods=0:tool=1:valid=1:p=0" );
  const std::string text = kitty_vector_records( *pair.server.get_fb().find_kitty_image( 7 ) );
  std::vector<Vector::Record> records;
  std::string error;
  require( Vector::parse_records( text, records, error ) && records.size() == 1 && records[0].op == 'k',
           "one stroke: " + text );
  const std::vector<double> expected = { 70, 160, 1.8, 75, 162, 3.4, 76, 162, 1 };
  const std::vector<double> got = numbers_of( records[0].pts );
  require( got.size() == expected.size(), "nine numbers: " + text );
  for ( size_t i = 0; i < got.size(); i++ ) {
    require( std::fabs( got[i] - expected[i] ) < 1e-9, "ink point " + std::to_string( got[i] ) + " in " + text );
  }
  require( pair.server.get_fb().ds.ext.brush.next == 1001 && pair.server.get_fb().ds.ext.brush.remaining == 49,
           "the id is used up at the server" );

  /* Drawn by the terminal itself, the stroke is not sent back to it. */
  const Framebuffer before = pair.client.get_fb();
  pair.client.apply_string( pair.server.diff_from( pair.client ) );
  std::string out;
  append_kitty_frame( out, true, before, pair.client.get_fb() );
  require( out.find( "a=e" ) == std::string::npos && out.find( "a=t" ) == std::string::npos,
           "ink is not sent back to the terminal that drew it: " + out );
  pair.draw();

  /* A cancelled stroke never happened: deleted at the server, and at the
     terminal too, in case it had stopped drawing it. */
  pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=5:contact=0:x=25600:y=51200:"
               "buttons=1:mods=0:tool=1:valid=0:ink=1001" );
  result = pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=4:device=3:t=6:contact=0:x=25600:y=51200:"
                        "buttons=0:mods=0:tool=1:valid=0" );
  require( kitty_vector_records( *pair.server.get_fb().find_kitty_image( 7 ) ).find( "id=1001" ) == std::string::npos,
           "a cancelled stroke is deleted" );
  require( result.to_terminal.find( base64( "x id=1001\n" ) ) != std::string::npos, "and at the terminal" );

  /* A stroke the client cannot follow is taken back from the terminal and
     left to the program. */
  result = pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=7:contact=0:x=0:y=0:"
                        "buttons=1:mods=0:tool=1:valid=0:ink=1002",
                        &program );
  require( frames( program ).size() == 1 && field( frames( program )[0], "ink" ) == "<absent>"
             && result.to_terminal.find( base64( "x id=1002\n" ) ) != std::string::npos,
           "an ink the client cannot follow" );

  /* After its image is sent whole, the terminal no longer draws a stroke in
     progress: the client writes the rest of it. */
  pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=8:contact=0:x=25600:y=51200:"
               "buttons=1:mods=0:tool=1:valid=0:ink=1003" );
  pair.terminal.drawn( pair.shown, pair.shown, {}, nullptr, true );
  result = pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:t=9:contact=0:x=26880:y=51200:"
                        "buttons=1:mods=0:tool=1:valid=0" );
  require( result.to_terminal.find( "a=e" ) != std::string::npos, "the rest of an interrupted stroke" );

  /* The terminal inks with the brush it was given, even when the program
     has given another since; and against where the image is on its screen. */
  Pair late;
  late.server.act( kitty( "a=t,f=200,i=7,s=400,v=300", "" ) );
  late.server.act( "\033[3;4H" + kitty( "a=p,i=7", "" ), &late.geometry );
  late.server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" )
                   + osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5" ) );
  late.draw();
  late.server.act( osc( "ink", "v=1:op=brush:i=7:k=2000:n=50:fill=#ff0000:w0=1:w1=5" ) + "\033[1;1H\033M" );
  result = late.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=1:contact=0:x=25600:y=51200:"
                        "buttons=1:mods=0:tool=1:valid=0:ink=1000" );
  require( result.input.ink_records.find( "fill=#1a1a80" ) != std::string::npos
             && result.input.ink_records.find( "pts=70,160," ) != std::string::npos,
           "the terminal's brush and screen: " + result.input.ink_records );

  /* A mouse a brush names is drawing: its points carry ink records, and the
     collector thins its stroke like any other, each kept point with its own. */
  Pair drawing;
  drawing.server.act( kitty( "a=t,f=200,i=7,s=400,v=300", "" ) );
  drawing.server.act( kitty( "a=p,i=7", "" ), &drawing.geometry );
  drawing.server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" )
                      + osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w1=5:tools=64" ) );
  drawing.draw();
  const std::string mouse = "777;tablet-pointer;v=1:op=event:kind=1:device=3:contact=0:y=2560:mods=0:tool=6:valid=0";
  result = drawing.report( mouse + ":phase=1:t=1:x=2560:buttons=1:ink=1000" );
  require( result.ink.present && result.ink.record == 'k', "the down of a stroke of a mouse begins its ink" );
  Motion::PointerCollector collector;
  std::vector<Motion::PointerCollector::Item> sent;
  collector.collect( result.input, 1000, 100, false, sent );
  for ( int x = 2816; x <= 3584; x += 256 ) {
    result = drawing.report( mouse + ":phase=2:t=" + std::to_string( x / 256 ) + ":x=" + std::to_string( x ) + ":buttons=1" );
    require( result.ink.present && result.ink.record == 'a' && result.input.ink_records.find( "a id=1000" ) == 0,
             "a point of it adds to the ink" );
    collector.collect( result.input, 1000, 100, false, sent );
  }
  result = drawing.report( mouse + ":phase=3:t=20:x=3584:buttons=0" );
  collector.collect( result.input, 1000, 100, false, sent );
  /* The down, the first move (the first position after a pause goes at once),
     the last move, and the up: the three merged away were on the line. */
  require( sent.size() == 4 && sent[1].input.payload.find( ":x=2816:" ) != std::string::npos
             && sent[2].input.payload.find( ":x=3584:" ) != std::string::npos
             && sent[2].input.ink_records.find( "a id=1000 pts=" ) == 0 && sent[3].input.payload.find( ":phase=3:" ) != std::string::npos,
           "a straight stroke of a mouse is thinned to its ends, each with its ink: " + std::to_string( sent.size() ) );
}

void client_side()
{
  /* Mode bits are read back after they are set. */
  ExtensionClient client;
  client.info = silkie();
  Framebuffer blank( 80, 24 ), on( 80, 24 );
  ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  on.ds.ext.tablet = 127;
  Display display( false );
  display.set_extensions( silkie() );
  const std::string out = display.new_frame( true, blank, on );
  require( out.find( osc( "tablet-pointer", "v=1:op=mode:mode=127" ) + osc( "tablet-pointer", "v=1:op=mode" ) )
             != std::string::npos,
           "a mode that is set is read back: " + out );
  std::vector<std::pair<std::string, uint32_t>> modes;
  InkBrush brush;
  bool resent;
  display.take_drawn( modes, brush, resent );
  client.drawn( blank, on, modes, nullptr, false );
  auto result = client.frame( osc( "tablet-pointer", "v=1:op=mode:mode=7" ), on, geometry, 1000 );
  require( result.info_changed && client.info.grants[Selector::TABLET] == 7, "what the terminal let through" );

  /* Frame reports follow the program's output, through a marker. */
  Framebuffer framing = on;
  framing.ds.ext.frame = true;
  Framebuffer output = framing;
  output.ds.ext.frame_output++;
  const std::string marker = client.drawn( framing, output, {}, nullptr, false );
  require( marker.find( "777;frame;v=1:op=query:id=" ) != std::string::npos, "a marker after the output" );
  const std::string presented = osc( "frame", "v=1:op=event:seq=4:t=20:period=8333" );
  require( !client.frame( presented, output, geometry, 1000 ).forward, "a frame before the output is not reported" );
  const std::string id = marker.substr( marker.find( "id=" ) + 3, marker.find( '\033', 4 ) - marker.find( "id=" ) - 3 );
  client.frame( osc( "frame", "v=1:op=cap:id=" + id + ":caps=1:mode=1:period=8333" ), output, geometry, 1000 );
  require( client.frame( presented, output, geometry, 1001 ).forward, "the frame that shows it is" );
  require( !client.frame( presented, output, geometry, 1002 ).forward, "once" );

  /* The client is heard from while something is in progress, and knows
     the terminal's clock. */
  result = client.frame( osc( "tablet-pointer", "v=1:op=event:kind=2:phase=1:device=0:t=5000:contact=1:x=0:y=0:"
                                                "buttons=1:mods=0:tool=0:valid=0" ),
                         on, geometry, 1000 );
  require( result.forward && result.input.clock == 5000 && client.clock( 1500 ) == 5500, "the terminal's clock" );
  require( !client.heartbeat_due( 2999 ) && client.heartbeat_due( 3000 ), "a heartbeat while a finger is down" );
  client.frame( osc( "tablet-pointer", "v=1:op=event:kind=2:phase=3:device=0:t=6000:contact=1:x=0:y=0:"
                                       "buttons=0:mods=0:tool=0:valid=0" ),
                on, geometry, 2000 );
  require( !client.heartbeat_due( 9000 ), "and none once it lifts" );

  /* The server stamps what it makes up on the terminal's clock, and asks
     the terminal again when a program asks what it can do. */
  Complete server( 80, 24 );
  server.set_client_extensions( silkie() );
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=2" ) );
  ExtensionInput finger( "777;tablet-pointer;v=1:op=event:kind=2:phase=1:device=0:t=10:contact=4:x=0:y=0:"
                         "buttons=1:mods=0:tool=0:valid=0" );
  finger.clock = 100000;
  server.extension_input( finger );
  auto cancelled = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=0" ) ) );
  const int64_t t = std::stoll( field( cancelled[0], "t" ) );
  require( t >= 100000 && t < 101000, "a cancel on the terminal's clock: " + field( cancelled[0], "t" ) );
  server.take_extension_outputs();
  server.act( osc( "haptic", "v=1:op=query:id=9" ) + "\033]22;?pointer\033\\" );
  const auto asked = server.take_extension_outputs();
  require( asked.size() == 2 && asked[0] == osc( "haptic", "v=1:op=query:id=7253" )
             && asked[1].compare( 0, 6, "\033]22;?" ) == 0,
           "the terminal is asked again for the next program" );
}

void colors()
{
  uint8_t r = 128, g = 128, b = 128;
  gamut_to_srgb( 1, r, g, b );
  require( r == 128 && g == 128 && b == 128, "gray is gray in every gamut" );
  r = 255, g = 0, b = 0;
  gamut_to_srgb( 2, r, g, b );
  require( r == 255 && g == 0 && b == 0, "a color beyond sRGB is clipped" );
  r = 100, g = 150, b = 200;
  gamut_to_srgb( 1, r, g, b );
  require( !( r == 100 && g == 150 && b == 200 ), "a Display P3 color is converted" );

  /* HDR to SDR: SDR white and above are white, linear light is encoded. */
  const auto half = []( float f ) {
    uint32_t bits;
    std::memcpy( &bits, &f, 4 );
    const uint16_t h = uint16_t( ( bits >> 16 & 0x8000 ) | ( ( ( bits >> 23 & 0xff ) - 112 ) << 10 ) | ( bits >> 13 & 0x3ff ) );
    return std::string( { char( h & 0xff ), char( h >> 8 ) } );
  };
  const std::string pixels = half( 1 ) + half( 0.5f ) + half( 4 ) + half( 1 ) + half( 2 ) + half( 0.25f ) + half( 0.25f )
                             + half( 1 );
  std::string rgba;
  require( hdr_to_srgb8( pixels, 2, 1, false, rgba ) && uint8_t( rgba[0] ) == 255 && uint8_t( rgba[1] ) == 188
             && uint8_t( rgba[2] ) == 255 && uint8_t( rgba[3] ) == 255,
           "clipped" );
  require( hdr_to_srgb8( pixels, 2, 1, true, rgba ) && uint8_t( rgba[0] ) < 255, "compressed into the headroom" );

  /* A terminal without them is given the nearest sRGB color, not the raw one. */
  Complete server( 20, 5 );
  server.act( "\033[38:2:1:100:150:200mX" );
  const std::string plain = server.get_fb().get_cell( 0, 0 )->get_renditions().sgr( false );
  require( plain.find( ";38;2;100;150;200" ) == std::string::npos && plain.find( ";38;2;" ) != std::string::npos,
           "converted SGR: " + plain );

  /* Images: HDR is shown at SDR, a gamut converted, for a terminal without them. */
  ClientExtensions narrow = silkie();
  narrow.format = "24,32,100";
  narrow.image_gamut = false;
  server.set_client_extensions( narrow );
  std::string reply = server.act( kitty( "a=q,i=3,f=64,s=1,v=1", half( 1 ) + half( 1 ) + half( 1 ) + half( 1 ) ) );
  require( reply.find( "EINVAL" ) != std::string::npos, "a query says HDR is not there" );
  reply = server.act( kitty( "a=t,i=4,f=64,s=2,v=1", pixels ) );
  const KittyImage* sdr = server.get_fb().find_kitty_image( 4 );
  require( reply.find( "OK" ) != std::string::npos && sdr && sdr->format == KITTY_FORMAT_HDR && *sdr->data == pixels,
           "an HDR image sent anyway is accepted, and kept to be shown at SDR when drawn: " + reply );
  reply = server.act( kitty( "a=t,i=5,f=24,s=1,v=1,G=1", std::string( "\x64\x96\xc8", 3 ) ) );
  const KittyImage* converted = server.get_fb().find_kitty_image( 5 );
  require( reply.find( "OK" ) != std::string::npos && converted && kitty_image_to_rgba( *converted, rgba )
             && uint8_t( rgba[0] ) == 100 && uint8_t( rgba[1] ) == 150 && converted->gamut == 1,
           "a Display P3 image is kept, to be converted when drawn" );
  server.set_client_extensions( silkie() );
  server.act( kitty( "a=t,i=6,f=24,s=1,v=1,G=1", std::string( "\x64\x96\xc8", 3 ) ) );
  require( server.get_fb().find_kitty_image( 6 )->gamut == 1, "and kept for a terminal that takes it" );

  /* A PNG that says what its colors are reaches the terminal as it came. */
  png_image image;
  std::memset( &image, 0, sizeof image );
  image.version = PNG_IMAGE_VERSION;
  image.width = 2;
  image.height = 1;
  image.format = PNG_FORMAT_RGBA;
  const unsigned char source[8] = { 1, 2, 3, 255, 4, 5, 6, 255 };
  png_alloc_size_t size = 0;
  require( png_image_write_to_memory( &image, nullptr, &size, 0, source, 0, nullptr ), "PNG size" );
  std::string png( size, '\0' );
  require( png_image_write_to_memory( &image, &png[0], &size, 0, source, 0, nullptr ), "PNG" );
  png.resize( size );
  const std::string body = std::string( "cICP" ) + std::string( "\x0c\x0d\x00\x01", 4 ); /* Display P3, sRGB curve */
  const uint32_t crc = uint32_t( crc32( 0, reinterpret_cast<const Bytef*>( body.data() ), body.size() ) );
  const std::string chunk = std::string( "\0\0\0\4", 4 ) + body
                            + std::string( { char( crc >> 24 ), char( crc >> 16 ), char( crc >> 8 ), char( crc ) } );
  const std::string tagged = png.substr( 0, 33 ) + chunk + png.substr( 33 );
  ImageEncoding encoding;
  encoding.png = true;
  KittyImage stored;
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, tagged, stored, encoding, error_sink() ) && stored.png_original
             && *stored.data == tagged,
           "a PNG with cICP is kept" );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, png, stored, encoding, error_sink() ) && !stored.png_original,
           "a plain PNG is not" );
  Framebuffer fb( 20, 5 );
  stored = KittyImage();
  kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, tagged, stored, encoding, error_sink() );
  stored.id = 9;
  fb.put_kitty_image( stored );
  std::string frame_out;
  append_kitty_frame( frame_out, false, Framebuffer( 20, 5 ), fb );
  require( frame_out.find( "f=100" ) != std::string::npos, "sent to the terminal as PNG" );
}

void review_fixes()
{
  /* Numbers on the wire do not follow the user's locale. */
  const char* previous = setlocale( LC_NUMERIC, nullptr );
  const std::string saved = previous ? previous : "C";
  for ( const char* name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8" } ) {
    if ( setlocale( LC_NUMERIC, name ) ) {
      break;
    }
  }
  std::vector<Vector::Record> records;
  std::string error;
  require( Vector::parse_records( "p id=1 d=M0.5,0L1.25,1", records, error ), "decimal points in any locale" );
  require( Ext::number( 1.5 ) == "1.5", "numbers written with a point: " + Ext::number( 1.5 ) );
  setlocale( LC_NUMERIC, saved.c_str() );

  /* x g= deletes the group it names, whatever references come with it. */
  Vector::Table table;
  require( Vector::parse_records( "g id=5\ng id=9\np id=1 g=5 d=M0,0\np id=2 g=9 d=M0,0", records, error ), error );
  table.apply( records );
  require( Vector::parse_records( "x g=5 s=9", records, error ) && table.apply( records ).empty(), "delete" );
  require( table.text() == "g id=9\np id=2 g=9 d=M0,0\n", "only group 5 is deleted: " + table.text() );
  /* A second d continues the first. */
  require( Vector::parse_records( "a id=2 d=L1,1 d=", records, error ) && table.apply( records ).empty(), "append" );
  require( table.text().find( "a id=2 d=L1,1\n" ) != std::string::npos, "both d of an append: " + table.text() );
  require( !Vector::color_paints( "#000000 zz=1" ) && !Vector::color_paints( "none" )
             && Vector::color_paints( "#1a1a80" ) && Vector::color_paints( "@3" ),
           "brush colors" );

  /* Erasing keeps the background's opacity, brightness and gamut. */
  Complete server( 20, 5 );
  server.act( "\033[48:2:1:255:0:0;70:40;57:200m\033[2J" );
  const Renditions& erased = server.get_fb().get_cell( 3, 3 )->get_renditions();
  require( erased.get_background_alpha() == 40 && erased.get_brightness( true ) == 200 && erased.has_extended(),
           "erased cells keep the background's attributes" );
  server.act( "\033[38:5m\033[31mX" );
  require( server.get_fb().get_cell( 0, 0 )->get_renditions() == server.get_fb().ds.get_renditions(), "38:5 alone" );

  /* OSC 22 as kitty has it. */
  Complete shapes( 20, 5 );
  shapes.set_client_extensions( silkie() );
  shapes.act( "\033]22;>pointer,bogus,wait\033\\" );
  require( shapes.get_fb().ds.ext.pointer[0] == std::vector<std::string>( { "pointer", "wait" } ),
           "unknown names are ignored" );
  shapes.act( "\033]22;=\033\\" );
  require( shapes.act( "\033]22;?__current__\033\\" ) == "\033]22;0\033\\", "an empty set is no shape" );
  shapes.act( "\033]22;<\033\\\033]22;>image:9\033\\" );
  require( shapes.get_fb().ds.ext.pointer_shape() == "image:9:0:0", "image names in full" );
  require( shapes.get_fb().ds.ext.pointer_shape( &shapes.get_fb() ) == "pointer",
           "a missing image passes to the shape below" );
  shapes.act( "\033]22;hand2\033\\" );
  require( shapes.get_fb().ds.ext.pointer_shape() == "hand2", "kitty's other names" );
}
}

void sgr()
{
  Complete server( 80, 24 ), client( 80, 24 );
  server.act( "\033[38;2;255;136;0m\033[56:250mA\033[38:2:1:255:0:0;48:2::1:2:3;57:1700;70:31mB\033[4:3mC\033[4:0mD" );
  const Renditions& a = server.get_fb().get_cell( 0, 0 )->get_renditions();
  const Renditions& b = server.get_fb().get_cell( 0, 1 )->get_renditions();
  require( a.get_brightness( false ) == 250 && !a.get_brightness( true ) && a.get_background_alpha() == 100,
           "foreground brightness" );
  require( b.get_brightness( false ) == 250 && b.get_brightness( true ) == 1675 && b.get_background_alpha() == 30,
           "background brightness is capped and alpha rounded down to even" );
  require( server.get_fb().get_cell( 0, 2 )->get_renditions().get_attribute( Renditions::underlined )
             && !server.get_fb().get_cell( 0, 3 )->get_renditions().get_attribute( Renditions::underlined ),
           "underline styles" );
  client.apply_string( server.diff_from( client ) );
  require( client.get_fb().get_cell( 0, 1 )->get_renditions() == b, "the attributes reach the client" );
  require( b.sgr().find( "\033[38:2:1:255:0:0;48:2:0:1:2:3;56:250;57:1675;70:30m" ) != std::string::npos
             || b.sgr().find( "\033[38:2:1:255:0:0;56:250;57:1675;70:30m" ) != std::string::npos,
           "extended SGR: " + b.sgr() );
  require( b.sgr( false ).find( "56" ) == std::string::npos && b.sgr( false ).find( ";38;2;255;0;0" ) != std::string::npos,
           "plain SGR for other terminals: " + b.sgr( false ) );

  /* As the client draws, without reading terminfo: a terminal that did not
     answer the listing, and Silkie. */
  Display plain( false ), extended( false );
  plain.set_extensions( ClientExtensions() );
  extended.set_extensions( silkie() );
  Framebuffer blank( 80, 24 );
  require( extended.new_frame( false, blank, client.get_fb() ).find( "56:250" ) != std::string::npos,
           "Silkie is sent the brightness" );
  require( plain.new_frame( false, blank, client.get_fb() ).find( "56:250" ) == std::string::npos,
           "other terminals are not" );

  server.act( "\033[56:110;70mE\033[57;38;5;9mF\033[0mG" );
  const Renditions& e = server.get_fb().get_cell( 0, 4 )->get_renditions();
  const Renditions& f = server.get_fb().get_cell( 0, 5 )->get_renditions();
  require( !e.get_brightness( false ) && e.get_background_alpha() == 100, "resets" );
  require( !f.get_brightness( true ) && !f.has_extended(), "a palette color has no gamut" );
  require( !server.get_fb().get_cell( 0, 6 )->get_renditions().has_extended(), "SGR 0" );

  server.act( "\033[1;1H\033[3:4H" );
  require( server.get_fb().ds.get_cursor_row() == 0 && server.get_fb().ds.get_cursor_col() == 0,
           "sub-parameters elsewhere than SGR are not taken" );
  server.act( "\033[" + std::string( 300, '0' ) + "1;" + std::string( 200, '0' ) + "3H" );
  require( server.get_fb().ds.get_cursor_row() == 0 && server.get_fb().ds.get_cursor_col() == 2,
           "parameters of any length" );
}

/* A fixture of a terminal with everything Silkie has added since the first. */
ClientExtensions silkie_now()
{
  ClientExtensions info = silkie();
  require( consume_probe_reply(
             osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1,display.1,font.1,frame.1,gesture.1,haptic.1,ink.1,"
                          "latency.1,scroll.1,tablet-pointer.1:media=d,f,t,s:format=24,32,64,100,200:graphics=viewport.1" ),
             info ),
           "the listing" );
  require( consume_probe_reply( osc( "font", "v=1:op=cap:id=7253:caps=1:roles=default,sans,serif:weight_min=100:"
                                             "weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1" ),
                                info ),
           "the font reply" );
  info.vector_service = true;
  return info;
}

void fonts()
{
  Complete server( 80, 24 );

  /* Discovery. */
  require( server.act( osc( "font", "v=1:op=query:id=42" ) ).empty(), "nothing is answered before the client is known" );
  server.set_client_extensions( silkie_now() );
  auto reply = frames( server.act( osc( "caps", "v=1:op=query:id=5" ) ) );
  require( reply.size() == 1 && field( reply[0], "ext" ).find( "font.1" ) != std::string::npos, "font.1 is listed" );
  reply = frames( server.act( osc( "font", "v=1:op=query:id=42" ) ) );
  require( reply.size() == 1 && field( reply[0], "op" ) == "cap" && field( reply[0], "id" ) == "42"
             && field( reply[0], "roles" ) == "default,sans,serif" && field( reply[0], "weight_max" ) == "900",
           "the capabilities are the terminal's" );
  reply = frames( server.act( osc( "font", "v=1:op=query" ) ) );
  require( reply.size() == 1 && field( reply[0], "id" ) == "0", "id defaults to 0" );
  server.take_extension_outputs();

  /* Selection goes with the text. */
  server.act( "\033[1;1H" + osc( "font", "v=1:op=select:role=serif:weight=300:slant=-500:italic=1" ) + "A" );
  const Renditions& a = server.get_fb().get_cell( 0, 0 )->get_renditions();
  require( text_style_of( a.get_text_style() ).role == "serif" && text_style_of( a.get_text_style() ).weight == 300
             && text_style_of( a.get_text_style() ).slant == -500 && a.get_attribute( Renditions::italic ),
           "a cell keeps what was selected" );
  reply = frames( server.act( osc( "font", "v=1:op=state:id=7" ) ) );
  require( reply.size() == 1 && field( reply[0], "op" ) == "state" && field( reply[0], "role" ) == "serif"
             && field( reply[0], "weight" ) == "300" && field( reply[0], "slant" ) == "-500"
             && field( reply[0], "italic" ) == "1",
           "the state reads back" );

  /* An omitted property keeps its value, and a bad frame changes nothing. */
  server.act( osc( "font", "v=1:op=select:slant=auto" ) );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "serif" && field( reply[0], "weight" ) == "300" && field( reply[0], "slant" ) == "auto",
           "omitted properties stay" );
  for ( const char* bad : { "weight=99", "weight=901", "slant=1501", "italic=2", "role=1x", "role=a!b", "v=2", "id=-1" } ) {
    server.act( osc( "font", std::string( "v=1:op=select:role=sans:" ) + bad ) );
    reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
    require( field( reply[0], "role" ) == "serif", std::string( "rejected whole: " ) + bad );
  }
  server.act( osc( "font", "v=1:op=select:role=" + std::string( 1000, 'a' ) ) );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "default", "an unknown role of any length selects the default" );

  /* Resets: SGR 0 and the reset request, which keeps italic. The caps reset
     leaves text attributes alone. */
  server.act( osc( "font", "v=1:op=select:role=serif:weight=700:italic=1" ) );
  server.act( osc( "caps", "v=1:op=reset" ) );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "serif", "caps reset is not for text attributes" );
  server.act( osc( "font", "v=1:op=reset" ) );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "default" && field( reply[0], "weight" ) == "0" && field( reply[0], "italic" ) == "1",
           "the reset request leaves italic" );
  server.act( osc( "font", "v=1:op=select:role=serif" ) + "\033[0m" );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "default" && field( reply[0], "italic" ) == "0", "SGR 0 resets" );

  /* Legacy slots keep weight and slant. */
  server.act( osc( "font", "v=1:op=select:weight=600" ) + "\033[12m" );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "serif" && field( reply[0], "weight" ) == "600", "SGR 12 is the second role" );
  server.act( "\033[19m" );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "default", "an unassigned slot is the default" );
  server.act( "\033[0m" );

  /* Cursor save and restore keep it. */
  server.act( osc( "font", "v=1:op=select:role=sans" ) + "\0337\033[0m\0338" );
  reply = frames( server.act( osc( "font", "v=1:op=state" ) ) );
  require( field( reply[0], "role" ) == "sans", "DECSC keeps the style" );
  server.act( "\033[0m" );

  /* Across the link, and to the terminal. */
  Complete writer( 20, 4 ), empty( 20, 4 );
  writer.set_client_extensions( silkie_now() );
  writer.act( "x" + osc( "font", "v=1:op=select:role=serif:weight=250:slant=300" ) + "styled" + "\033[0m" + "z" );
  Complete replica( 20, 4 );
  replica.apply_string( writer.diff_from( empty ) );
  for ( int y = 0; y < 4; y++ ) {
    for ( int x = 0; x < 20; x++ ) {
      require( *replica.get_fb().get_cell( y, x ) == *writer.get_fb().get_cell( y, x ), "the client's copy has the same cells" );
    }
  }
  const Renditions& far = replica.get_fb().get_cell( 0, 1 )->get_renditions();
  require( text_style_of( far.get_text_style() ).role == "serif" && text_style_of( far.get_text_style() ).weight == 250,
           "a style crosses the link" );
  require( !replica.get_fb().get_cell( 0, 0 )->get_renditions().get_text_style(), "and only where it was selected" );

  Display speaking( false ), plain( false );
  speaking.set_extensions( silkie_now() );
  plain.set_extensions( silkie() );
  Framebuffer blank( 20, 4 );
  const std::string out = speaking.new_frame( false, blank, replica.get_fb() );
  require( out.find( "\033]777;font;v=1:op=select:role=serif:weight=250:slant=300\033\\styled" ) != std::string::npos,
           "the terminal is told before the text" );
  require( plain.new_frame( false, blank, replica.get_fb() ).find( ";font;" ) == std::string::npos,
           "a terminal without the protocol is not" );

  /* A terminal without the protocol: the selector is not listed or answered,
     and it is the display that keeps it from the terminal. */
  Complete without( 80, 24 );
  without.set_client_extensions( silkie() );
  require( without.act( osc( "font", "v=1:op=query:id=1" ) ).empty(), "no answer without the protocol" );
  auto listed = frames( without.act( osc( "caps", "v=1:op=query:id=5" ) ) );
  require( field( listed[0], "ext" ).find( "font" ) == std::string::npos, "not listed without the protocol" );
  require( listed[0].fields.find( "graphics" ) == nullptr, "no graphics field without an extension" );
}

std::string apc( const std::string& controls, const std::string& payload = std::string() )
{
  return "\033_G" + controls + ( payload.empty() ? std::string() : ";" + base64( payload ) ) + "\033\\";
}

void vector_text()
{
  Complete server( 80, 24 );
  server.set_client_extensions( silkie_now() );
  const std::string layout = base64( "{\"font\":\"Iosevka\",\"size\":24,\"runs\":[{\"text\":\"Hi \\n\\u00e9\",\"fill\":\"p3:1/0.2/0*250\"}],"
                                     "\"axes\":{\"wght\":700},\"x\":1.5e1,\"ok\":true,\"none\":null}" );
  const std::string scene = "g id=1 t=1,0,0,1,50,30 box=0,0,400,240\n"
                            "g id=2 p=1 box=10,40,380,190\n"
                            "t id=3 g=2 text=" + layout + " t=1,0,0,1,0,0\n"
                            "p id=4 g=2 d=M0,0L1,1 fill=srgb:1/0/0/0.5 stroke=linear:-0.2/1.5/9 w=1\n"
                            "p id=5 d=M0,0L1,1 fill=rec2020:0/1/0*300\n"
                            "l id=6 x1=0 y1=0 x2=1 y2=1 stops=0,p3:1/0/0,1,#00ff00*150\n";
  auto reply = server.act( apc( "a=T,f=200,i=9,s=100,v=100,T=1", scene ) );
  require( reply.find( ";OK" ) != std::string::npos, "shaped text, boxes and new paints are accepted: " + reply );
  const KittyImage* image = server.get_fb().find_kitty_image( 9 );
  require( image && image->tone_map, "a vector image keeps T" );
  const std::string records = kitty_vector_records( *image );
  require( records.find( "t id=3 g=2 text=" + layout ) != std::string::npos, "the text record is kept whole" );
  require( records.find( "box=10,40,380,190" ) != std::string::npos, "and so is a box" );

  /* The scene crosses the link, and reaches the terminal, with T. */
  Complete empty( 80, 24 ), replica( 80, 24 );
  replica.apply_string( server.diff_from( empty ) );
  require( replica.get_fb().find_kitty_image( 9 ) && replica.get_fb().find_kitty_image( 9 )->tone_map, "T crosses the link" );
  std::string frame;
  append_kitty_frame( frame, false, empty.get_fb(), replica.get_fb() );
  require( frame.find( "f=200" ) != std::string::npos && frame.find( ",T=1" ) != std::string::npos, "and is sent on" );

  /* As Silkie checks them. */
  const struct { const char* record; const char* why; } refused[] = {
    { "p id=1 d=M0,0 fill=srgb:2/0/0", "Color component outside 0..1" },
    { "p id=1 d=M0,0 fill=linear:0/0/0/2", "Color component outside 0..1" },
    { "p id=1 d=M0,0 fill=cmyk:0/0/0", "Unknown color space" },
    { "p id=1 d=M0,0 fill=p3:1/1", "Color needs three or four components" },
    { "p id=1 d=M0,0 fill=p3:1/1/1/1/1", "Bad color components" },
    { "p id=1 d=M0,0 fill=p3:1/1/", "Bad color components" },
    { "p id=1 d=M0,0 fill=#ff0000*-1", "Bad brightness" },
    { "p id=1 d=M0,0 fill=@1.5", "Bad gradient reference" },
    { "g id=1 box=0,0,-1,1", "Bad clipping box" },
    { "g id=1 box=0,0,1", "Bad clipping box" },
    { "t id=1", "Text record without text" },
    { "t id=1 text=!!!!", "Invalid vector text, font or layout" },
  };
  for ( const auto& item : refused ) {
    const std::string answer = server.act( apc( "a=q,f=200,i=8,s=1,v=1", item.record ) );
    require( answer.find( "EINVAL:Line 1: " + std::string( item.why ) ) != std::string::npos,
             std::string( item.record ) + " gave " + answer );
  }
  for ( const std::string& json : { std::string( "[1]" ), std::string( "{\"a\":}" ), std::string( "{\"a\":1,}" ),
                                    std::string( "{\"a\":1} x" ), std::string( "{\"a\":\"\xff\"}" ),
                                    std::string( "{\"a\":[1,2}" ), std::string( "{" ) } ) {
    const std::string answer = server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + base64( json ) ) );
    require( answer.find( "Invalid vector text" ) != std::string::npos, "not a layout: " + json );
  }
  const std::string deep = std::string( 100000, '[' ) + std::string( 100000, ']' );
  require( server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + base64( "{\"a\":" + deep + "}" ) ) ).find( ";OK" ) != std::string::npos,
           "nesting has no limit" );
  require( server.act( apc( "a=q,f=200,i=8,s=1,v=1", "p id=1 d=M0.0" + std::string( 200, '0' ) + ",0" ) ).find( ";OK" ) != std::string::npos,
           "a number may be as long as it likes" );

  /* The ink's brush: no caps of its own. */
  server.act( osc( "ink", "v=1:op=brush:i=9:k=100:n=5:w1=5000000000:e=1000:fill=#ff0000*250.00000000000000000000" ) );
  auto ink = frames( server.act( osc( "ink", "v=1:op=query:id=1" ) ) );
  require( ink.size() == 1, "ink answers a query" );
  require( field( ink[0], "w1" ) == "5e+09" && field( ink[0], "e" ) == "1e+03"
             && field( ink[0], "fill" ) == "#ff0000*250.00000000000000000000",
           "a brush of any size" );
  /* A label of any length. */
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) );
  server.take_extension_outputs();
  server.act( osc( "tablet-pointer", "v=1:op=label:device=1:button=0:text=" + base64( std::string( 5000, 'x' ) ) ) );
  require( server.get_fb().ds.ext.pad_labels.size() == 1
             && server.get_fb().ds.ext.pad_labels.begin()->second == base64( std::string( 5000, 'x' ) ),
           "a long pad label is kept" );
  server.act( osc( "tablet-pointer", "v=1:op=label:device=1:button=0:text=" + base64( "a\nb" ) ) );
  require( server.get_fb().ds.ext.pad_labels.begin()->second == base64( std::string( 5000, 'x' ) ),
           "control characters are still refused" );
}

void vector_service()
{
  Complete before( 80, 24 );
  const std::string unknown = before.act( apc( "a=v,f=200,i=77", "{\"op\":\"fonts\"}" ) );
  require( unknown == "\033_Gi=77;EINVAL:" + base64( "{\"error\": \"The terminal's capabilities are not known yet\"}" ) + "\033\\",
           "before anything is known, an error, not silence: " + unknown );
  require( before.act( apc( "a=v,f=200,i=77,q=2", "{\"op\":\"fonts\"}" ) ).empty(), "unless q=2" );

  Complete server( 80, 24 );
  server.set_client_extensions( silkie() );
  std::string none = server.act( apc( "a=v,f=200,i=77", "{\"op\":\"fonts\"}" ) );
  require( none.find( "\033_Gi=77;EINVAL:" ) == 0, "a terminal without the service: an error, not silence: " + none );
  require( server.take_extension_outputs().empty(), "and nothing is written to it" );

  server.set_client_extensions( silkie_now() );
  require( server.act( apc( "a=v,f=100,i=77", "{}" ) ).find( "EINVAL" ) != std::string::npos, "the format is required" );
  require( server.act( apc( "a=v,f=200", "{}" ) ).find( "EINVAL" ) != std::string::npos, "and a request id" );

  /* A request of any size, in chunks, is carried to the terminal in chunks of
     the size the protocol says, and it answers. */
  const std::string upload = "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"" + std::string( 20000, 'A' ) + "\"}";
  const std::string encoded = base64( upload );
  std::string chunks;
  for ( size_t off = 0; off < encoded.size(); off += 4096 ) {
    const bool more = off + 4096 < encoded.size();
    chunks += "\033_G" + std::string( off ? "" : "a=v,f=200,i=77," ) + "m=" + ( more ? "1" : "0" ) + ";"
              + encoded.substr( off, 4096 ) + "\033\\";
  }
  require( server.act( chunks ).empty(), "the answer is the terminal's to give" );
  auto outputs = server.take_extension_outputs();
  require( outputs.size() == 1, "the request is one output" );
  const std::string first = outputs[0].substr( 0, outputs[0].find( "\033\\" ) );
  require( first.find( "\033_Ga=v,f=200,i=" ) == 0 && first.find( ",m=1;" ) != std::string::npos && first.find( "q=" ) == std::string::npos,
           "the terminal is asked, not quietly: " + first.substr( 0, 40 ) );
  /* It goes through the terminal's parser intact. */
  std::string rebuilt;
  size_t at = 0;
  uint32_t relay_id = 0;
  while ( at < outputs[0].size() ) {
    const size_t end = outputs[0].find( "\033\\", at );
    KittyCommand command;
    require( parse_kitty_command( outputs[0].substr( at + 2, end - at - 2 ), command ), "a chunk parses" );
    require( outputs[0].size() && end - at - 2 < 4096 + 64, "chunks are of the protocol's size" );
    if ( !at ) {
      relay_id = command.image_id;
    }
    rebuilt += command.payload;
    at = end + 2;
  }
  require( rebuilt == upload, "what the program sent reaches the terminal" );
  require( relay_id != 77, "by a name of the relay's" );

  /* The client hears the answer, and only for what it wrote. */
  ExtensionClient client;
  client.service_written( outputs[0] );
  require( client.service_pending( 0 ), "an answer is expected" );
  bool forward = false, changed = false;
  ExtensionInput input;
  require( !client.service_reply( "\033_Gi=4294967294;OK\033\\", 0, forward, input, changed ), "another's reply is left alone" );
  const std::string answer = "Gi=" + std::to_string( relay_id ) + ";OK;" + base64( "{\"id\":\"body\"}" );
  require( client.service_reply( "\033_" + answer + "\033\\", 0, forward, input, changed ) && forward, "the answer goes on" );
  require( !client.service_pending( 0 ), "and no more are expected" );
  require( input.service_reply == answer, "as the terminal wrote it" );
  require( server.extension_input( input ) == "\033_Gi=77;OK;" + base64( "{\"id\":\"body\"}" ) + "\033\\", "under the program's id" );
  require( server.extension_input( input ).empty(), "once" );

  /* q as ever: 1 keeps quiet when all is well, 2 always. */
  const auto ask = [&]( const std::string& controls ) {
    server.act( apc( controls, "{\"op\":\"fonts\"}" ) );
    const auto out = server.take_extension_outputs();
    require( out.size() == 1, "a request is written" );
    KittyCommand command;
    parse_kitty_command( out[0].substr( 2, out[0].find( "\033\\" ) - 2 ), command );
    return "Gi=" + std::to_string( command.image_id );
  };
  ExtensionInput quiet;
  quiet.service_reply = ask( "a=v,f=200,i=5,q=1" ) + ";OK;e30=";
  require( server.extension_input( quiet ).empty(), "q=1 hides a success" );
  quiet.service_reply = ask( "a=v,f=200,i=5,q=1" ) + ";EINVAL:e30=";
  require( server.extension_input( quiet ) == "\033_Gi=5;EINVAL:e30=\033\\", "but not an error" );
  quiet.service_reply = ask( "a=v,f=200,i=5,q=2" ) + ";EINVAL:e30=";
  require( server.extension_input( quiet ).empty(), "q=2 hides both" );

  /* The answer crosses the link. */
  Network::UserStream sent, received;
  ExtensionInput carried;
  carried.service_reply = answer;
  sent.push_back( carried );
  received.apply_string( sent.init_diff() );
  require( received.size() == 1 && received.get_extension_input( 0 ) == carried, "over the wire" );

  /* Whether the terminal has the service is asked once it lists vectors. */
  ExtensionClient asking;
  Framebuffer shown( 80, 24 );
  ClientGeometry geometry;
  asking.probe();
  auto result = asking.frame( osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1:format=24,32,200" ), shown, geometry, 1000 );
  require( result.to_terminal.find( "a=v,f=200,i=7253" ) != std::string::npos, "the service is probed" );
  require( asking.service_pending( 1000 ), "and the answer is waited for" );
  require( asking.service_reply( "\033_Gi=7253;EINVAL:e30=\033\\", 1001, forward, input, changed ) && !forward && changed,
           "any answer means it is there" );
  require( asking.info.vector_service, "and the server is told" );
  ExtensionClient disabled;
  disabled.set_graphics_allowed( false );
  disabled.probe();
  const auto disabled_reply = disabled.frame( osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1:format=24,32,200" ),
                                               shown, geometry, 1000 );
  require( disabled_reply.to_terminal.empty() && !disabled.service_pending( 1000 ),
           "disabled Kitty graphics never trigger a vector service probe" );
  ExtensionClient silent;
  silent.probe();
  silent.frame( osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1:format=24,32,200" ), shown, geometry, 1000 );
  require( !silent.service_pending( 11001 ) && !silent.info.vector_service, "silence means it is not" );
  ClientExtensions wire = silkie_now();
  wire.graphics = "viewport.1";
  ClientBuffers::ClientExtensionsMessage message;
  client_extensions_to_proto( wire, &message );
  require( client_extensions_from_proto( message ) == wire, "what the terminal has crosses the link" );

  /* The demux leaves graphics replies in the input unless one is expected. */
  Ext::InputDemux demux;
  Ext::InputDemux::Output plain;
  demux.feed( "\033_Gi=1;OK\033\\", plain );
  require( plain.items.size() == 1 && !plain.items[0].first, "not looked at while none is expected" );
  demux.capture_graphics( true );
  Ext::InputDemux::Output held;
  demux.feed( "\033_Gi=1;O", held );
  demux.feed( "K\033\\a", held );
  require( held.items.size() == 2 && held.items[0].first && held.items[0].second == "\033_Gi=1;OK\033\\" && held.items[1].second == "a",
           "held whole while one is" );
}

void viewports()
{
  Complete server( 80, 24 );
  ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  const std::string pixel = base64( std::string( "\0\0\0\0", 4 ) );
  const auto act = [&]( const std::string& bytes ) { return server.act( bytes, &geometry ); };

  /* Without the extension in the terminal behind the relay. */
  server.set_client_extensions( silkie() );
  act( "\033_Ga=t,f=32,s=1,v=1,i=1,q=2;" + pixel + "\033\\" );
  require( act( "\033_Ga=p,i=1,p=1,c=80,r=20,F=2\033\\" ).find( "EINVAL" ) != std::string::npos, "not there, so refused" );
  require( server.get_fb().get_kitty_placements().empty(), "and the command changes nothing" );
  require( act( "\033_Ga=q,i=31,s=1,v=1,f=32,F=3;" + pixel + "\033\\" ).find( "EINVAL" ) != std::string::npos, "a query says so" );

  server.set_client_extensions( silkie_now() );
  auto listed = frames( act( osc( "caps", "v=1:op=query:id=5" ) ) );
  require( field( listed[0], "graphics" ) == "viewport.1", "listed once it is there" );
  require( act( "\033_Ga=q,i=31,s=1,v=1,f=32,F=3;" + pixel + "\033\\" ).find( ";OK" ) != std::string::npos, "the bits are known" );
  require( act( "\033_Ga=q,i=31,s=1,v=1,f=32,F=4;" + pixel + "\033\\" ).find( "EINVAL" ) != std::string::npos, "other bits are not" );
  require( act( "\033_Ga=p,i=1,p=9,U=1,F=1\033\\" ).find( "EINVAL" ) != std::string::npos, "nor on a virtual placement" );

  /* The scrolling world of the specification. */
  act( "\033[3;1H\033_Ga=p,i=1,p=1,c=80,r=20,C=1,F=2,q=2\033\\" );
  act( "\033_Ga=p,i=1,p=2,P=1,Q=1,F=1,q=2\033\\" );
  act( "\033_Ga=t,f=32,s=1,v=1,i=10,q=2;" + pixel + "\033\\" );
  act( "\033_Ga=p,i=10,p=1,P=1,Q=2,F=1,H=51,X=2,V=12,Y=16,c=10,r=5,q=2\033\\" );
  require( server.get_fb().get_kitty_placements().size() == 3, "viewport, camera and chunk" );

  const auto find = [&]( const Framebuffer& fb, uint32_t image, uint32_t placement ) {
    for ( const auto& p : fb.get_kitty_placements() ) {
      if ( p.image_id == image && p.placement_id == placement ) {
        return p;
      }
    }
    throw std::runtime_error( "no such placement" );
  };
  KittyPlacementRect rect;
  require( kitty_placement_rect( server.get_fb(), find( server.get_fb(), 10, 1 ), 10, 20, rect ), "the chunk has a place" );
  require( rect.left == 512 && rect.top == 40 + 256 && rect.clipped && rect.clip_left == 0 && rect.clip_right == 800
             && rect.clip_top == 40 && rect.clip_bottom == 440,
           "at the world's position, clipped to the field" );

  /* One command scrolls it: -1234, -87 is -124 columns and 6, -5 lines and 13. */
  act( "\033_Ga=p,i=1,p=2,P=1,Q=1,F=1,H=-124,X=6,V=-5,Y=13,q=2\033\\" );
  require( server.get_fb().get_kitty_placements().size() == 3, "placed again, not added" );
  require( kitty_placement_rect( server.get_fb(), find( server.get_fb(), 10, 1 ), 10, 20, rect ), "still placed" );
  require( rect.left == 512 - 1234 && rect.top == 40 + 256 - 87, "and every child moves with one command" );

  /* It reaches the terminal with F, across the link. */
  Complete empty( 80, 24 ), replica( 80, 24 );
  replica.apply_string( server.diff_from( empty ) );
  require( find( replica.get_fb(), 1, 2 ).flags == 1 && find( replica.get_fb(), 1, 1 ).flags == 2, "F crosses the link" );
  std::string frame, without;
  append_kitty_frame( frame, false, empty.get_fb(), replica.get_fb(), false, false, nullptr, true );
  require( frame.find( ",F=2" ) != std::string::npos && frame.find( ",F=1" ) != std::string::npos, "and is sent on" );
  append_kitty_frame( without, false, empty.get_fb(), replica.get_fb() );
  require( without.find( ",F=" ) == std::string::npos, "never to a terminal that would drop the command" );

  /* Scrolling leaves what is placed in pixels to its parent. */
  const KittyPlacement chunk = find( server.get_fb(), 10, 1 );
  act( "\033[24;1H\n\n" );
  require( find( server.get_fb(), 10, 1 ) == chunk, "the chunk scrolls with its parent only" );
  /* Deleting by position finds it where it is visible, not where it would be. */
  act( "\033_Ga=p,i=1,p=2,P=1,Q=1,F=1,q=2\033\\" ); /* the camera back at the origin */
  /* The viewport covers every cell, as kitty finds it: the chunk alone has z=5. */
  act( "\033_Ga=p,i=10,p=1,P=1,Q=2,F=1,H=51,X=2,V=12,Y=16,c=10,r=5,z=5,q=2\033\\" );
  KittyPlacementRect now;
  require( kitty_placement_rect( server.get_fb(), find( server.get_fb(), 10, 1 ), 10, 20, now ), "placed" );
  const int visible_col = int( now.left / 10 ) + 1, visible_row = int( now.top / 20 ) + 1;
  size_t count = server.get_fb().get_kitty_placements().size();
  act( "\033_Ga=d,d=q,x=" + std::to_string( visible_col ) + ",y=" + std::to_string( visible_row + 6 ) + ",z=5,q=2\033\\" );
  require( server.get_fb().get_kitty_placements().size() == count, "not found below where it is" );
  act( "\033_Ga=d,d=q,x=" + std::to_string( visible_col ) + ",y=" + std::to_string( visible_row ) + ",z=5,q=2\033\\" );
  require( server.get_fb().get_kitty_placements().size() == count - 1, "found in a cell it is visible in" );
  act( "\033_Ga=p,i=10,p=1,P=1,Q=2,F=1,H=51,X=2,V=12,Y=16,c=10,r=5,q=2\033\\" );

  /* A placement lives as long as its parent. */
  act( "\033_Ga=d,d=i,i=1,p=1,q=2\033\\" );
  require( server.get_fb().get_kitty_placements().empty(), "deleting the viewport deletes the world under it" );

  /* Without F the arithmetic stays kitty's. */
  act( "\033[3;1H\033_Ga=p,i=1,p=5,X=3,c=2,r=2,C=1,q=2\033\\" );
  act( "\033_Ga=p,i=1,p=6,P=1,Q=5,H=2,V=1,c=1,r=1,q=2\033\\" );
  require( kitty_placement_rect( server.get_fb(), find( server.get_fb(), 1, 6 ), 10, 20, rect ), "a relative placement" );
  require( rect.left == 20 && rect.top == 60 && !rect.clipped, "at the parent's cell plus the offset, ignoring its X" );
}

/* What the client does with a stream of reports, in order, with the link
   keeping up or not. */
struct Collected
{
  Motion::PointerCollector collector;
  std::vector<Motion::PointerCollector::Item> sent;
  void report( const std::string& payload, uint64_t now, bool backlog = false )
  {
    collector.collect( ExtensionInput( payload ), now, 100, backlog, sent );
  }
  std::vector<std::string> payloads() const
  {
    std::vector<std::string> out;
    for ( const auto& item : sent ) {
      out.push_back( item.bytes ? item.raw : item.input.payload );
    }
    return out;
  }
};

std::string tablet_report( int phase, int tool, int x, int buttons = 0, int contact = 0, const std::string& extra = "" )
{
  return "777;tablet-pointer;v=1:op=event:kind=1:phase=" + std::to_string( phase ) + ":device=3:t=" + std::to_string( 1000 + x )
         + ":contact=" + std::to_string( contact ) + ":x=" + std::to_string( x ) + ":y=7:buttons=" + std::to_string( buttons )
         + ":mods=0:tool=" + std::to_string( tool ) + ":valid=1:p=" + std::to_string( x % 65536 ) + extra;
}

void pointer_collection()
{
  /* A change of state is never merged, whatever the tool: a stroke that lifts
     and lands at every point arrives whole and in order, pen, eraser, brush,
     pencil, airbrush, lens, tools yet unknown, and a report that does not say.
     It makes no difference that the link is behind. */
  for ( int tool : { 1, 2, 3, 4, 5, 7, 8, 99 } ) {
    for ( bool backlog : { false, true } ) {
      Collected c;
      std::vector<std::string> expected;
      expected.push_back( tablet_report( 1, tool, 0, 1 ) );
      for ( int x = 1; x <= 5000; x++ ) {
        expected.push_back( tablet_report( x % 2 ? 2 : 0, tool, x, x % 2 ) );
      }
      expected.push_back( tablet_report( 3, tool, 5001 ) );
      for ( const auto& report : expected ) {
        c.report( report, 1000, backlog );
      }
      require( c.payloads() == expected, "every point of tool " + std::to_string( tool ) );
    }
  }
  /* A stroke is thinned to its bends: a point off the line between the ones
     kept, by more than the tolerance, is kept, with what was held before it. */
  {
    Collected c;
    const std::string pen = "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:contact=0:buttons=1:mods=0:tool=1:valid=0";
    const auto at = [&]( int t, int x, int y ) {
      return pen + ":t=" + std::to_string( t ) + ":x=" + std::to_string( x ) + ":y=" + std::to_string( y );
    };
    c.report( at( 1, 0, 0 ), 1000 );      /* sent at once */
    c.report( at( 2, 256, 0 ), 1000 );    /* held */
    c.report( at( 3, 512, 20 ), 1000 );   /* within a quarter pixel of the line: the one before is merged away */
    c.report( at( 4, 768, 40 ), 1000 );
    require( c.sent.size() == 1 && c.collector.holding(), "a straight stroke is held to its last point" );
    c.report( at( 5, 1024, 400 ), 1000 ); /* a bend: what was held is a point of the stroke */
    require( c.sent.size() == 2 && c.sent[1].input.payload.find( ":x=768:" ) != std::string::npos,
             "the point before a bend is kept" );
    c.collector.flush( c.sent );
    require( c.sent.size() == 3 && c.sent[2].input.payload.find( ":x=1024:" ) != std::string::npos, "and the bend itself" );
  }
  /* A finger that is down is thinned within the tolerances like a pen; a
     report that a correction may name, or that is a prediction, is never merged. */
  {
    Collected c;
    const std::string finger = "777;tablet-pointer;v=1:op=event:kind=2:phase=2:device=3:contact=0:buttons=1:mods=0:tool=0:valid=0";
    for ( int x = 0; x <= 2560; x += 256 ) {
      c.report( finger + ":t=" + std::to_string( 100 + x / 256 ) + ":x=" + std::to_string( x ) + ":y=" + std::to_string( x / 2 ), 1000 );
    }
    require( c.sent.size() == 1 && c.collector.holding(), "a finger moving in a straight line: its start, and the rest held" );
    c.report( tablet_report( 2, 6, 1, 0, 0, ":seq=4" ), 1000 );
    c.report( tablet_report( 2, 6, 2, 0, 0, ":pred=1" ), 1000 );
    require( c.sent.size() == 4 && c.sent[1].input.payload.find( ":x=2560:" ) != std::string::npos,
             "the end of the line goes before what is never merged" );
  }

  /* A mouse: the first position goes at once, then what piles up while the
     link has an interval to wait goes as one, with the last values. */
  {
    Collected c;
    c.report( tablet_report( 0, 6, 1 ), 1000 );
    require( c.sent.size() == 1, "the first position is not held" );
    for ( int x = 2; x <= 1000; x++ ) {
      c.report( tablet_report( 0, 6, x ), 1050 );
    }
    require( c.sent.size() == 1 && c.collector.holding(), "the rest wait" );
    require( c.collector.wait_time( 1050, false ) == 50, "for what is left of the interval" );
    c.collector.release( 1099, 100, false, c.sent );
    require( c.sent.size() == 1, "not before it is over" );
    c.collector.release( 1100, 100, true, c.sent );
    require( c.sent.size() == 1, "nor while the link is still taking what it has" );
    require( c.collector.wait_time( 1100, true ) == INT_MAX, "the link says when" );
    c.collector.release( 1100, 100, false, c.sent );
    require( c.sent.size() == 2 && c.sent[1].input.payload == tablet_report( 0, 6, 1000 ), "one position, the last" );
    require( !c.collector.holding(), "and nothing is left" );
  }
  /* Merged values: absolute of the last, sum of the relative, last t. */
  {
    Collected c;
    c.report( tablet_report( 2, 6, 0, 1 ), 1000 );
    c.report( tablet_report( 2, 6, 10, 1, 0, ":wheel=2:wclicks=1" ), 1001 );
    c.report( tablet_report( 2, 6, 20, 1, 0, ":wheel=3:wclicks=-4" ), 1002 );
    c.report( tablet_report( 2, 6, 30, 1 ), 1003 );
    c.collector.flush( c.sent );
    Ext::Frame merged;
    require( c.sent.size() == 2 && Ext::parse( c.sent[1].input.payload, merged ), "merged" );
    require( field( merged, "x" ) == "30" && field( merged, "t" ) == "1030" && field( merged, "wheel" ) == "5"
               && field( merged, "wclicks" ) == "-3" && field( merged, "valid" ) == "65",
             "absolute of the last, sum of the relative: " + c.sent[1].input.payload );
  }
  /* What is never merged keeps its place, with what came before it first. */
  {
    Collected c;
    c.report( tablet_report( 0, 6, 1 ), 1000 ); /* sent */
    c.report( tablet_report( 0, 6, 2 ), 1001 );
    c.report( tablet_report( 0, 6, 3 ), 1002 ); /* held */
    c.report( tablet_report( 1, 6, 4, 1 ), 1003 ); /* a down */
    c.report( tablet_report( 2, 6, 5, 1 ), 1200 ); /* the interval has passed */
    c.report( tablet_report( 2, 6, 6, 1 ), 1201 ); /* held */
    c.report( tablet_report( 2, 6, 7, 3 ), 1202 ); /* another button: a new state, and held */
    c.report( tablet_report( 3, 6, 8, 0 ), 1203 ); /* an up */
    const std::vector<std::string> expected = { tablet_report( 0, 6, 1 ), tablet_report( 0, 6, 3 ), tablet_report( 1, 6, 4, 1 ),
                                                tablet_report( 2, 6, 5, 1 ), tablet_report( 2, 6, 6, 1 ),
                                                tablet_report( 2, 6, 7, 3 ), tablet_report( 3, 6, 8, 0 ) };
    require( c.payloads() == expected, "nothing is reordered, no change of state is lost" );
  }
  /* A pen in the middle of a mouse's motion: the mouse goes first, the pen is whole. */
  {
    Collected c;
    c.report( tablet_report( 0, 6, 1 ), 1000 );
    c.report( tablet_report( 0, 6, 2 ), 1001 );
    c.report( tablet_report( 2, 1, 3, 1 ), 1002 );
    c.report( tablet_report( 0, 6, 4 ), 1003 );
    c.report( tablet_report( 2, 1, 5, 1 ), 1004 );
    c.collector.flush( c.sent );
    const std::vector<std::string> expected = { tablet_report( 0, 6, 1 ), tablet_report( 0, 6, 2 ), tablet_report( 2, 1, 3, 1 ),
                                                tablet_report( 0, 6, 4 ), tablet_report( 2, 1, 5, 1 ) };
    require( c.payloads() == expected, "a pen is not held behind a mouse" );
  }
  /* Two mice do not merge with one another. */
  {
    Collected c;
    c.report( tablet_report( 0, 6, 1, 0, 0 ), 1000 );
    c.report( tablet_report( 0, 6, 2, 0, 0 ), 1001 );
    c.report( tablet_report( 0, 6, 3, 0, 1 ), 1002 );
    c.report( tablet_report( 0, 6, 4, 0, 0 ), 1003 );
    c.collector.flush( c.sent );
    require( c.sent.size() == 3 && c.sent[1].input.payload == tablet_report( 0, 6, 3, 0, 1 )
               && c.sent[2].input.payload == tablet_report( 0, 6, 4, 0, 0 ),
             "one position for each contact, by when it was last heard" );
  }
  /* Scroll reports: distances add up, within a phase of one source. */
  {
    const auto scroll = []( int dx, int dy, int phase, int src = 1, int t = 1 ) {
      return "777;scroll;v=1:op=event:t=" + std::to_string( t ) + ":dx=" + std::to_string( dx ) + ":dy=" + std::to_string( dy )
             + ":x=1:y=2:mods=0:src=" + std::to_string( src ) + ":phase=" + std::to_string( phase );
    };
    Collected c;
    c.report( scroll( 0, 0, 1 ), 1000 ); /* began: never merged */
    c.report( scroll( 1, 2, 2 ), 1000 );
    c.report( scroll( 10, -20, 2, 1, 9 ), 1001 );
    c.report( scroll( 100, 200, 2, 1, 10 ), 1002 );
    c.report( scroll( 0, 0, 3 ), 1003 ); /* ended */
    require( c.payloads()[0] == scroll( 0, 0, 1 ) && c.payloads().back() == scroll( 0, 0, 3 ), "a change of phase stays" );
    long dx = 0, dy = 0;
    for ( const auto& payload : c.payloads() ) {
      Ext::Frame f;
      require( Ext::parse( payload, f ), "a scroll" );
      dx += std::stol( field( f, "dx" ) );
      dy += std::stol( field( f, "dy" ) );
    }
    require( dx == 111 && dy == 182, "no distance is lost" );
    require( c.sent.size() < 5, "and the middle is merged" );
    Collected wheel;
    wheel.report( scroll( 0, 100, 0, 0 ), 1000 );
    wheel.report( scroll( 0, 100, 0, 0 ), 1001 );
    wheel.report( scroll( 0, 100, 0, 1 ), 1002 );
    wheel.collector.flush( wheel.sent );
    require( wheel.sent.size() == 3, "two sources are two devices" );
  }

  /* xterm mouse reporting. */
  const int sgr = DrawState::MOUSE_ENCODING_SGR, x10 = DrawState::MOUSE_ENCODING_DEFAULT,
            urxvt = DrawState::MOUSE_ENCODING_URXVT, utf8 = DrawState::MOUSE_ENCODING_UTF8,
            pixels = DrawState::MOUSE_ENCODING_SGR_PIXELS;
  const auto split = []( const std::string& bytes, int encoding ) {
    Motion::PointerCollector c;
    std::vector<std::pair<bool, std::string>> pieces;
    c.split_motion( bytes, encoding, pieces );
    return pieces;
  };
  auto parts = split( "a\033[<35;10;20M\033[<35;11;20Mb\033[<0;11;20M\033[<0;11;20m\033[<65;1;1M", sgr );
  require( parts.size() == 4 && !parts[0].first && parts[0].second == "a" && parts[1].first && parts[2].first
             && parts[1].second == "\033[<35;10;20M" && !parts[3].first
             && parts[3].second == "b\033[<0;11;20M\033[<0;11;20m\033[<65;1;1M",
           "motion apart from presses, releases, wheels and keys" );
  require( split( "\033[<32;1;1M", pixels )[0].first, "with a button held, in pixels too" );
  require( split( "\033[M\x43\x21\x21", x10 )[0].first && !split( "\033[M\x20\x21\x21", x10 )[0].first, "the X10 encoding" );
  require( split( "\033[67;5;5M", urxvt )[0].first && !split( "\033[32;5;5M", urxvt )[0].first, "the urxvt encoding" );
  require( split( "\033[M\x43\xc2\x80\xc2\x81", utf8 )[0].first && split( "\033[M\x43\xc2\x80\xc2\x81", utf8 ).size() == 1,
           "the UTF-8 encoding" );
  require( !split( "\033[<35;10;20M", -1 )[0].first, "nothing is motion while it is not being reported" );
  require( !split( "\033[<35;10;2", sgr )[0].first, "a report cut in two reads goes on as it is" );
  /* A paste is literal, even when it spans reads. */
  {
    Motion::PointerCollector c;
    std::vector<std::pair<bool, std::string>> pieces;
    c.split_motion( "\033[200", sgr, pieces );
    c.split_motion( "~\033[<35;1;1M\033[201~\033[<35;2;2M", sgr, pieces );
    require( pieces.size() == 2 && !pieces[0].first && pieces[0].second == "\033[200~\033[<35;1;1M\033[201~" && pieces[1].first,
             "bracketed paste is not looked into" );
  }
  /* And merged with the same rules. */
  {
    Motion::PointerCollector c;
    std::vector<Motion::PointerCollector::Item> sent;
    c.collect_motion( "\033[<35;1;1M", sgr, 1000, 100, false, sent );
    c.collect_motion( "\033[<35;2;1M", sgr, 1001, 100, false, sent );
    c.collect_motion( "\033[<35;3;1M", sgr, 1002, 100, false, sent );
    require( sent.size() == 1 && sent[0].bytes && sent[0].raw == "\033[<35;1;1M", "the first is not held" );
    c.collect_motion( "\033[<32;4;1M", sgr, 1003, 100, false, sent ); /* a button: a new state */
    require( sent.size() == 2 && sent[1].raw == "\033[<35;3;1M", "the last before it goes first" );
    c.flush( sent );
    require( sent.size() == 3 && sent[2].raw == "\033[<32;4;1M", "and then it" );
  }

  /* The server gets what was merged like any other report. */
  Complete server( 80, 24 );
  server.set_client_extensions( silkie_now() );
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" ) + osc( "scroll", "v=1:op=mode:mode=1" ) );
  server.extension_input( ExtensionInput( tablet_report( 1, 6, 6, 1 ) ) );
  ExtensionInput merged( tablet_report( 2, 6, 7, 1, 0, ":wheel=5" ) );
  auto delivered = frames( server.extension_input( merged ) );
  require( delivered.size() == 1 && field( delivered[0], "wheel" ) == "5", "a merged report reaches the program" );

  /* End to end on the client: a pen stream that is inking, and a mouse that is
     too, since a brush can name it, are both kept whole. */
  ExtensionClient client;
  Framebuffer shown( 80, 24 );
  shown.ds.ext.tablet = 1;
  ClientGeometry geometry;
  client.probe();
  auto pen = client.frame( "\033]" + tablet_report( 2, 1, 4 ) + "\033\\", shown, geometry, 1000 );
  require( pen.forward, "the client leaves the deciding to the collector" );
  Collected c;
  c.report( pen.input.payload, 1000 );
  c.report( pen.input.payload, 1000 );
  c.report( pen.input.payload, 1000 );
  require( c.sent.size() == 1 && c.collector.holding(), "which merges a pen that has not moved" );
}

/* The relay's side of the tablet pointer protocol: what it keeps, what it
   says itself and what it does not pass on. */
void tablet_relay()
{
  /* Pad labels are terminal state: they follow the mode to the terminal. */
  {
    Complete server( 80, 24 ), client( 80, 24 );
    server.set_client_extensions( silkie() );
    const std::string undo = osc( "tablet-pointer", "v=1:op=label:device=5:button=2:text=" + base64( "Undo" ) );
    require( server.act( undo ).empty() && server.get_fb().ds.ext.pad_labels.empty(), "no label without pad reports" );
    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) + undo
                + osc( "tablet-pointer", "v=1:op=label:device=5:ring=0:text=" + base64( "Zoom" ) ) );
    require( server.get_fb().ds.ext.pad_labels.size() == 2 && server.take_extension_outputs().size() <= 1,
             "labels are kept, not sent once" );
    client.apply_string( server.diff_from( client ) );
    require( client.get_fb().ds.ext == server.get_fb().ds.ext, "labels reach the client" );

    Display display( false );
    display.set_extensions( silkie() );
    Framebuffer blank( 80, 24 );
    const std::string out = display.new_frame( false, blank, client.get_fb() );
    const size_t mode = out.find( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) ),
                 label = out.find( osc( "tablet-pointer", "v=1:op=label:device=5:button=2:text=" + base64( "Undo" ) ) );
    require( mode != std::string::npos && label != std::string::npos && mode < label,
             "a label is given after the mode that lets the terminal take it" );
    require( out.find( "ring=0" ) != std::string::npos, "every label is given" );
    require( display.new_frame( true, client.get_fb(), client.get_fb() ).find( "label" ) == std::string::npos,
             "and not again" );

    const Framebuffer before = client.get_fb();
    server.act( osc( "tablet-pointer", "v=1:op=label:device=5:button=2" ) );
    client.apply_string( server.diff_from( client ) );
    const std::string removed = display.new_frame( true, before, client.get_fb() );
    require( removed == osc( "tablet-pointer", "v=1:op=label:device=5:button=2" ), "a label taken away: " + removed );

    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" ) );
    require( server.get_fb().ds.ext.pad_labels.empty(), "pad reports off forget the labels" );
  }

  Complete server( 80, 24 );
  server.set_client_extensions( silkie() );
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=7" ) );

  /* Nothing of a stroke the relay never saw begin. */
  const std::string finger = "777;tablet-pointer;v=1:op=event:kind=2:device=0:contact=1:y=0:buttons=1:mods=0:tool=0:valid=0";
  require( server.extension_input( ExtensionInput( finger + ":phase=2:t=1:x=5" ) ).empty(), "a move of an unknown contact" );
  require( server.extension_input( ExtensionInput( finger + ":phase=3:t=2:x=5" ) ).empty(), "an up of an unknown contact" );
  require( server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=2:phase=6:device=0:t=2" ) ).empty(),
           "nor a frame of a group of which nothing was reported" );
  require( !server.extension_input( ExtensionInput( finger + ":phase=1:t=3:x=5:src=1" ) ).empty(), "a down" );
  require( !server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=2:phase=6:device=0:t=3" ) ).empty(),
           "and its frame" );

  /* A cancel says what the coordinates are relative to, as the contact did. */
  auto cancel = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=7" ) ) );
  require( cancel.size() == 2 && field( cancel[0], "phase" ) == "4" && field( cancel[0], "src" ) == "1"
             && field( cancel[1], "phase" ) == "6",
           "a finger on a touchpad is cancelled as one" );
  require( server.extension_input( ExtensionInput( finger + ":phase=4:t=4:x=5:src=1" ) ).empty(),
           "the terminal's own cancel is not passed on again" );
  require( server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=2:phase=6:device=0:t=4" ) ).empty(),
           "nor its frame" );

  /* Setting the mode the program has: the relay describes what is near. */
  const std::string pen = "777;tablet-pointer;v=1:op=device:device=3:kind=1:tool=1:serial=1234:name=QQ==";
  require( !server.extension_input( ExtensionInput( pen ) ).empty(), "a pen is described" );
  auto described = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=7" ) ) );
  require( described.size() == 1 && field( described[0], "op" ) == "device" && field( described[0], "serial" ) == "<absent>",
           "and described again, without what the program did not ask for" );
  require( frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=3" ) ) ).empty(),
           "a mode that changes leaves that to the terminal" );
  server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=1:phase=5:device=3:t=5:contact=0:x=0:y=0" ) );
  require( frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=3" ) ) ).empty(), "a pen that left is forgotten" );

  /* A pad key the relay released: the pad's own release is not passed on. */
  server.act( osc( "tablet-pointer", "v=1:op=mode:mode=7" ) );
  server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=3:device=5:t=10:mods=0:button=2:state=1" ) );
  auto released = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=7" ) ) );
  require( released.size() == 1 && field( released[0], "state" ) == "0", "the relay releases a key" );
  require( server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=3:device=5:t=11:mods=0:button=2:state=0" ) )
             .empty(),
           "and the pad's release is not reported" );
  require( !server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=3:device=5:t=12:mods=0:button=2:state=1" ) )
              .empty(),
           "the next press is" );

  /* A terminal that stops granting a mode: the mode reads back as off. */
  server.act( osc( "scroll", "v=1:op=mode:mode=1" ) + osc( "frame", "v=1:op=mode:mode=1" ) );
  ClientExtensions strict = silkie();
  strict.grants[Selector::SCROLL] = 0;
  strict.versions.erase( Selector::FRAME );
  strict.grants[Selector::TABLET] = 1;
  server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=3:device=5:t=13:mods=0:button=1:state=1" ) );
  auto refused = frames( server.set_client_extensions( strict ) );
  require( !server.get_fb().ds.ext.scroll && !server.get_fb().ds.ext.frame && server.get_fb().ds.ext.tablet == 1,
           "refused modes are off" );
  require( refused.size() == 2 && field( refused[0], "button" ) == "1" && field( refused[1], "button" ) == "2",
           "and the keys they covered are released" );
}

/* What the relay does of the other modes when the terminal behind it changes
   or is reset. */
void relay_modes()
{
  Complete server( 80, 24 );
  server.set_client_extensions( silkie() );

  /* A gesture the relay never saw begin is not passed on. */
  server.act( osc( "gesture", "v=1:op=mode:mode=1" ) );
  require( server.extension_input( ExtensionInput( "777;gesture;v=1:op=event:kind=1:phase=2:t=5:x=0:y=0:mods=0:scale=10:total=1010" ) )
             .empty(),
           "an update of a pinch that did not begin here" );

  /* RIS stops continuous feedback at the terminal. */
  server.take_extension_outputs();
  server.act( "\033c" );
  auto outputs = server.take_extension_outputs();
  require( outputs.size() == 1 && outputs[0] == osc( "haptic", "v=1:op=stop" ), "RIS stops haptic feedback" );

  /* SGR 10 is the default role even where no roles are listed. */
  Complete plain( 80, 24 );
  ClientExtensions without_fonts = silkie();
  without_fonts.caps.erase( Selector::FONT );
  plain.set_client_extensions( without_fonts );
  plain.act( osc( "font", "v=1:op=select:role=serif" ) );
  plain.act( "\033[10m" );
  require( text_style_of( plain.get_fb().ds.get_renditions().get_text_style() ).role == "default",
           "SGR 10 picks the default role" );

  /* A terminal that refuses ink: the mode reads back as off. */
  ClientExtensions no_ink = silkie();
  no_ink.grants[Selector::INK] = 0;
  server.set_client_extensions( no_ink );
  server.act( osc( "ink", "v=1:op=brush:i=7:k=1:n=5:w1=4" ) );
  require( !server.get_fb().ds.ext.ink && server.get_fb().ds.ext.brush.present(), "the brush is kept, ink stays off" );
  auto read = frames( server.act( osc( "ink", "v=1:op=mode" ) ) );
  require( read.size() == 1 && field( read[0], "mode" ) == "0", "and reads back as off" );
  /* The terminal is asked once it has the brush. */
  Complete granted( 80, 24 );
  granted.set_client_extensions( silkie() );
  granted.act( osc( "ink", "v=1:op=brush:i=7:k=1:n=5:w1=4" ) );
  std::vector<std::pair<std::string, uint32_t>> sent;
  Framebuffer blank( 80, 24 );
  const std::string out = render_extension_modes( false, blank, granted.get_fb(), silkie(), 0, &sent );
  require( out.find( osc( "ink", "v=1:op=mode" ) ) != std::string::npos && !sent.empty() && sent.back().first == Selector::INK,
           "ink is read back after a brush" );
}

std::string unbase64( const std::string& in )
{
  static const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned value = 0;
  int bits = 0;
  for ( char c : in ) {
    const size_t at = digits.find( c );
    if ( at == std::string::npos ) {
      continue;
    }
    value = value << 6 | unsigned( at );
    bits += 6;
    if ( bits >= 8 ) {
      bits -= 8;
      out += char( ( value >> bits ) & 0xff );
    }
  }
  return out;
}

/* The pixels the terminal is sent for an image, and the keys they came with. */
std::string sent_pixels( const std::string& frame, const std::string& action, uint32_t id, std::string* controls = nullptr )
{
  std::string pixels;
  bool found = false;
  for ( size_t at = frame.find( "\033_G" ); at != std::string::npos; at = frame.find( "\033_G", at + 1 ) ) {
    const size_t semi = frame.find( ';', at ), end = frame.find( "\033\\", at );
    if ( semi == std::string::npos || end == std::string::npos || semi > end ) {
      continue;
    }
    const std::string keys = frame.substr( at + 3, semi - at - 3 );
    if ( !found ) {
      if ( keys.find( "a=" + action ) == std::string::npos
           || ( "," + keys + "," ).find( ",i=" + std::to_string( id ) + "," ) == std::string::npos ) {
        continue;
      }
      found = true;
      if ( controls ) {
        *controls = keys;
      }
    }
    pixels += unbase64( frame.substr( semi + 1, end - semi - 1 ) );
    if ( keys.find( "m=1" ) == std::string::npos ) {
      break;
    }
  }
  return pixels;
}

std::string half( float f )
{
  const uint16_t h = float_to_half( f );
  return std::string( { char( h & 0xff ), char( h >> 8 ) } );
}

const KittyPlacement* placement( const Framebuffer& fb, uint32_t image, uint32_t id )
{
  for ( const auto& p : fb.get_kitty_placements() ) {
    if ( p.image_id == image && p.placement_id == id ) {
      return &p;
    }
  }
  return nullptr;
}

/* Silkie's packed formats: grayscale of 1, 2, 4 and 8 bits and RGB565. They
   stay packed and exact across the link, and are compressed on it. */
void packed_images()
{
  /* The container gives back every byte, for any size and whatever is in it. */
  uint32_t seed = 12345;
  const auto random = [&]() { return seed = seed * 1664525u + 1013904223u; };
  for ( uint32_t format : { 301u, 302u, 304u, 308u, 516u } ) {
    for ( const auto& size : { std::pair<uint32_t, uint32_t>( 1, 1 ), { 9, 3 }, { 17, 5 }, { 64, 64 }, { 301, 7 } } ) {
      for ( int kind = 0; kind < 3; kind++ ) {
        size_t row = 0, bytes = 0;
        require( packed_size( format, size.first, size.second, row, bytes ), "a packed size" );
        std::string pixels( bytes, '\0' );
        for ( size_t i = 0; i < bytes; i++ ) {
          pixels[i] = kind == 0 ? char( random() >> 24 ) : kind == 1 ? char( ( i % row ) * 3 + i / row ) : char( 0xaa );
        }
        for ( bool zstd : { false, true } ) {
          std::string container, back;
          require( packed_encode( format, size.first, size.second, pixels, zstd, container )
                     && packed_decode( format, size.first, size.second, container, back ) && back == pixels,
                   "packed pixels come back exactly, f=" + std::to_string( format ) );
          require( !packed_decode( format, size.first + 8, size.second, container, back ),
                   "a container of another size is not this image" );
        }
      }
    }
  }
  size_t row = 0, bytes = 0;
  require( packed_size( 301, 9, 2, row, bytes ) && row == 2 && bytes == 4 && packed_size( 516, 3, 2, row, bytes ) && row == 6
             && !packed_size( 301, 0, 4, row, bytes ) && !packed_size( 3, 4, 4, row, bytes ),
           "rows start at a byte; other formats are not packed" );

  /* Expanded as Silkie expands them. */
  std::string rgb;
  require( packed_expand( 302, 4, 1, "\x1b", false, rgb ) && rgb == std::string( "\0\0\0\x55\x55\x55\xaa\xaa\xaa\xff\xff\xff", 12 ),
           "two bits of gray are 0, 85, 170 and 255" );
  require( packed_expand( 301, 9, 1, std::string( "\x00\x80", 2 ), false, rgb ) && uint8_t( rgb[24] ) == 255
             && uint8_t( rgb[21] ) == 0,
           "the ninth pixel is the high bit of the second byte" );
  require( packed_expand( 516, 3, 1, std::string( "\x00\xf8\xe0\x07\x1f\x00", 6 ), false, rgb )
             && rgb == std::string( "\xff\0\0\0\xff\0\0\0\xff", 9 ),
           "RGB565 is a little-endian word" );
  require( packed_expand( 516, 1, 1, std::string( "\x21\x08", 2 ), false, rgb ) && uint8_t( rgb[0] ) == 8
             && uint8_t( rgb[1] ) == 4 && uint8_t( rgb[2] ) == 8,
           "each channel rounded to nearest" );

  /* A page of text, one bit a pixel: far smaller on the link than its bits. */
  const uint32_t width = 640, height = 400;
  packed_size( 301, width, height, row, bytes );
  std::string page( bytes, '\0' );
  for ( uint32_t y = 0; y < height; y++ ) {
    for ( uint32_t x = 0; x < width; x++ ) {
      const bool ink = y % 20 > 4 && y % 20 < 14 && ( ( x / 3 + ( y / 20 ) * 7 ) % 5 < 2 ) && ( x % 97 ) > 8;
      if ( ink ) {
        page[y * row + x / 8] |= char( 0x80 >> ( x % 8 ) );
      }
    }
  }
  ImageEncoding packing;
  packing.packed = true;
  packing.packed_zstd = packed_zstd_available();
  packing.quality = 40; /* the user asked for lossy WebP: packed pixels are exact all the same */
  Complete server( 80, 24 ), client( 80, 24 );
  server.set_image_encoding( packing );
  server.act( osc( "caps", "v=1:op=reset" ) );
  require( server.act( kitty( "a=T,i=7,f=301,s=640,v=400", page ) ).find( ";OK" ) != std::string::npos,
           "a one-bit image is accepted" );
  const KittyImage* kept = server.get_fb().find_kitty_image( 7 );
  std::string back;
  require( kept && kept->format == 301 && kitty_image_is_packed( *kept ) && kitty_image_packed_bytes( *kept, back )
             && back == page,
           "and kept as it was packed" );
  require( kept->data->size() * 8 < bytes, "compressed: " + std::to_string( kept->data->size() ) + " of "
                                              + std::to_string( bytes ) + " packed bytes" );
  const std::string diff = server.diff_from( Complete( 80, 24 ) );
  require( diff.size() < bytes / 4, "the link carries the container, not pixels" );
  client.apply_string( diff );
  const KittyImage* got = client.get_fb().find_kitty_image( 7 );
  require( got && got->format == 301 && kitty_image_packed_bytes( *got, back ) && back == page,
           "the client has the same bits" );

  /* The other formats, and compression of the upload, alike. */
  std::string gray( 16 * 16, '\0' ), words( 16 * 16 * 2, '\0' );
  for ( size_t i = 0; i < gray.size(); i++ ) {
    gray[i] = char( i * 7 );
    words[2 * i] = char( i * 13 );
    words[2 * i + 1] = char( i * 5 );
  }
  std::string deflated( compressBound( gray.size() ), '\0' );
  uLongf deflated_size = deflated.size();
  compress( reinterpret_cast<Bytef*>( &deflated[0] ), &deflated_size, reinterpret_cast<const Bytef*>( gray.data() ),
            gray.size() );
  deflated.resize( deflated_size );
  require( server.act( kitty( "a=t,i=8,f=308,s=16,v=16,o=z", deflated ) ).find( ";OK" ) != std::string::npos
             && server.act( kitty( "a=t,i=9,f=516,s=16,v=16,G=1", words ) ).find( ";OK" ) != std::string::npos
             && server.act( kitty( "a=t,i=10,f=304,s=3,v=2", std::string( "\x12\x30\xfe\xd0", 4 ) ) ).find( ";OK" )
                  != std::string::npos
             && server.act( kitty( "a=t,i=11,f=302,s=5,v=1", std::string( "\x1b\x40", 2 ) ) ).find( ";OK" )
                  != std::string::npos,
           "eight bits of gray with o=z, RGB565 with G, four bits and two" );
  require( server.act( kitty( "a=t,i=12,f=301,s=9,v=2", "abc" ) ).find( "EINVAL" ) != std::string::npos
             && !server.get_fb().find_kitty_image( 12 ),
           "pixels that are not the size of the image are refused" );
  require( server.act( kitty( "a=q,i=42,f=301,s=8,v=1", "U" ) ).find( "i=42;OK" ) != std::string::npos
             && !server.get_fb().find_kitty_image( 42 ),
           "a query says the format is taken and keeps nothing" );
  client.apply_string( server.diff_from( client ) );
  require( kitty_image_packed_bytes( *client.get_fb().find_kitty_image( 8 ), back ) && back == gray
             && kitty_image_packed_bytes( *client.get_fb().find_kitty_image( 9 ), back ) && back == words
             && client.get_fb().find_kitty_image( 9 )->gamut == 1 && client.get_fb().find_kitty_image( 8 )->gamut == 0,
           "all exact at the client, RGB565 with its gamut" );

  /* Drawn packed for a terminal that lists the format, as RGB for another. */
  const Framebuffer blank( 80, 24 );
  std::string out, keys;
  append_kitty_frame( out, false, blank, client.get_fb(), false, true, nullptr, false, false,
                      kitty_packed_formats( "301,302,304,308,516,24,32,100" ) );
  require( sent_pixels( out, "t", 7, &keys ) == page && ( "," + keys + "," ).find( ",f=301," ) != std::string::npos,
           "a terminal with f=1 is sent the bits: " + keys );
  require( sent_pixels( out, "t", 9, &keys ) == words && keys.find( "f=516" ) != std::string::npos
             && keys.find( "G=1" ) != std::string::npos,
           "and RGB565 with its G" );
  out.clear();
  append_kitty_frame( out, false, blank, client.get_fb(), false, false, nullptr, false, false,
                      kitty_packed_formats( "24,32,100" ) );
  std::string expanded;
  require( packed_expand( 301, width, height, page, false, expanded ) && sent_pixels( out, "t", 7, &keys ) == expanded
             && keys.find( "f=24" ) != std::string::npos,
           "a terminal without it is sent the same pixels as RGB" );
  require( sent_pixels( out, "t", 11, &keys ) == std::string( "\0\0\0\x55\x55\x55\xaa\xaa\xaa\xff\xff\xff\x55\x55\x55", 15 ),
           "two bits of gray, expanded" );
  require( kitty_packed_formats( "24,32" ) == ( kitty_format_bit( 24 ) | kitty_format_bit( 32 ) ) && kitty_packed_formats( "301,302,304,308,516,116" ) == 31
             && kitty_packed_formats( "" ) == 0,
           "the packed formats of a listing" );

  /* The listing has them whatever the terminal, since the relay expands. */
  std::string to_host;
  Complete listed( 80, 24 );
  listed.set_client_extensions( silkie() );
  const auto reply = frames( listed.act( osc( "caps", "v=1:op=query:id=3" ) ) );
  require( reply.size() == 1 && field( reply[0], "format" ).find( "301,302,304,308,516,24,32," ) == 0,
           "the listing: " + ( reply.empty() ? std::string( "none" ) : field( reply[0], "format" ) ) );

  /* A client from before packed state: expanded, and exact whatever --lossy. */
  ImageEncoding old;
  old.quality = 40;
  Complete old_server( 80, 24 ), old_client( 80, 24 );
  old_server.set_image_encoding( old );
  require( old_server.act( kitty( "a=t,i=8,f=308,s=16,v=16", gray ) ).find( ";OK" ) != std::string::npos
             && old_server.act( kitty( "a=t,i=9,f=516,s=16,v=16", words ) ).find( ";OK" ) != std::string::npos,
           "accepted for an older client too" );
  old_client.apply_string( old_server.diff_from( Complete( 80, 24 ) ) );
  std::string rgba, want;
  require( !kitty_image_is_packed( *old_client.get_fb().find_kitty_image( 8 ) )
             && kitty_image_to_rgba( *old_client.get_fb().find_kitty_image( 8 ), rgba )
             && packed_expand( 308, 16, 16, gray, true, want ) && rgba == want,
           "as exact pixels, not lossy ones" );
  require( kitty_image_to_rgba( *old_client.get_fb().find_kitty_image( 9 ), rgba )
             && packed_expand( 516, 16, 16, words, true, want ) && rgba == want,
           "RGB565 too" );

  /* A frame in a packed format edits an image; a packed image stays exact. */
  require( server.act( kitty( "a=f,i=8,r=1,f=301,s=8,v=1,x=0,y=0", std::string( "\xf0", 1 ) ) ).find( "EINVAL" )
             == std::string::npos,
           "a packed frame edits a packed image" );
  require( kitty_image_to_rgba( *server.get_fb().find_kitty_image( 8 ), rgba ) && uint8_t( rgba[0] ) == 255
             && uint8_t( rgba[4 * 4] ) == 0 && uint8_t( rgba[4 * 8] ) == uint8_t( gray[8] ),
           "its pixels written over the root frame, the rest untouched" );
}

/* Silkie's depth protocol: the divergence of the two eyes' views, a text
   attribute that placements and vector records share. */
void native_images()
{
  ImageEncoding encoding;
  encoding.native = true;
  encoding.packed = true;
  encoding.quality = 25;
  Complete server( 80, 24 ), client( 80, 24 );
  server.set_image_encoding( encoding );
  ClientExtensions terminal = silkie_now();
  terminal.format += ",8,16,301,302,304,308,316,401,402,404,408,516,632,664";
  server.set_client_extensions( terminal );
  const std::string formats = terminal.format;
  uint32_t id = 40;
  for ( uint32_t format : { 8u, 16u, 316u, 401u, 402u, 404u, 408u, 632u, 664u } ) {
    size_t size = 0;
    require( kitty_native_size( format, 9, 3, size ), "native payload size" );
    std::string bytes( size, '\0' );
    for ( size_t i = 0; i < size; ++i ) { bytes[i] = char( i * 53 ); }
    const uint32_t image_id = id++;
    const std::string controls = "a=T,i=" + std::to_string( image_id ) + ",f=" + std::to_string( format ) + ",s=9,v=3";
    require( server.act( kitty( controls, bytes ) ).find( ";OK" ) != std::string::npos, "accept native f=" + std::to_string( format ) );
    client.apply_string( server.diff_from( client ) );
    const KittyImage* image = client.get_fb().find_kitty_image( image_id );
    require( image && image->native && image->format == format && *image->data == bytes, "native source bits survive synchronization" );
    std::string out, keys;
    append_kitty_frame( out, false, Framebuffer( 80, 24 ), client.get_fb(), false, true, nullptr, false, true,
                        kitty_packed_formats( formats ) );
    require( sent_pixels( out, "t", image_id, &keys ) == bytes && keys.find( "f=" + std::to_string( format ) ) != std::string::npos,
             "native bytes reach a supporting terminal" );
    if ( format != 8 && format != 16 ) {
      out.clear();
      append_kitty_frame( out, false, Framebuffer( 80, 24 ), client.get_fb() );
      require( !sent_pixels( out, "t", image_id, &keys ).empty() && keys.find( "f=32" ) != std::string::npos,
               "an ordinary terminal gets rendered RGBA" );
    }
    require( server.act( kitty( "a=f,i=" + std::to_string( image_id ) + ",r=1,f=32,s=1,v=1", std::string( 4, '\0' ) ) ).find( "EINVAL" ) != std::string::npos,
             "native images refuse animation" );
  }
  // Palette updates are atomic and leave every packed index intact.
  const KittyImage before = *server.get_fb().find_kitty_image( 44 ); // f=402
  require( server.act( kitty( "a=e,i=44,x=1,w=1", std::string( "\0\xff\0\x80", 4 ) ) ).find( ";OK" ) != std::string::npos,
           "edit one palette slot" );
  client.apply_string( server.diff_from( client ) );
  const KittyImage* edited = client.get_fb().find_kitty_image( 44 );
  require( edited && edited->data->substr( 16 ) == before.data->substr( 16 )
             && edited->data->substr( 4, 4 ) == std::string( "\0\xff\0\x80", 4 ), "only palette changes across a diff" );
  const auto unchanged = edited->data;
  require( server.act( kitty( "a=e,i=44,x=3,w=2", std::string( 8, '\0' ) ) ).find( "EINVAL" ) != std::string::npos,
           "palette edit cannot overrun" );
  require( server.act( kitty( "a=e,i=44,x=1,w=1", "abc" ) ).find( "ENODATA" ) != std::string::npos,
           "short palette edit is refused" );
  client.apply_string( server.diff_from( client ) );
  require( *client.get_fb().find_kitty_image( 44 )->data == *unchanged, "failed edit leaves palette unchanged" );

  for ( uint32_t transfer : { 8u, 13u, 16u, 18u } ) {
    require( server.act( kitty( "a=t,i=80,f=632,s=1,v=1,E=" + std::to_string( transfer ) + ",G=2,T=1", std::string( "\0\x02\x08\xe0", 4 ) ) ).find( ";OK" ) != std::string::npos,
             "RGB10A2 accepts each curve" );
    client.apply_string( server.diff_from( client ) );
    const KittyImage* image = client.get_fb().find_kitty_image( 80 );
    require( image && image->native && image->transfer == transfer && image->gamut == 2 && image->tone_map,
             "curve, gamut and tone mapping survive synchronization" );
    std::string half;
    require( kitty_native_to_hdr( *image, half ) && half.size() == 8, "precision rendering decodes in linear light" );
  }
  double white[4] = { 0.58068888, 0.58068888, 0.58068888, 0.5 };
  decode_image_color( 16, 2, white );
  require( std::abs( white[0] - 1 ) < 0.001 && white[3] == 0.5, "203 nit PQ is SDR white, alpha unchanged" );
  for ( uint32_t format : { 8u, 16u, 100u, 200u, 401u, 64u } ) {
    require( server.act( kitty( "a=q,i=90,f=" + std::to_string( format ) + ",s=1,v=1,E=16", "" ) ).find( "EINVAL" ) != std::string::npos,
             "curves refused for formats that define their own colors" );
  }
  for ( uint32_t format : { 1u, 2u, 4u } ) {
    require( server.act( kitty( "a=q,i=90,f=" + std::to_string( format ) + ",s=1,v=1", "x" ) ).find( "EINVAL" ) != std::string::npos,
             "old gray numbers are no longer accepted" );
  }
  const std::string wide( size_t( 20001 ) * 8, '\x80' );
  require( server.act( kitty( "a=t,i=99,f=664,s=20001,v=1", wide ) ).find( ";OK" ) != std::string::npos,
           "precision images exceed old texture and WebP dimension limits" );
  client.apply_string( server.diff_from( client ) );
  require( client.get_fb().find_kitty_image( 99 ) && *client.get_fb().find_kitty_image( 99 )->data == wide,
           "wide precision pixels survive synchronization exactly" );
  Complete legacy( 80, 24 );
  require( legacy.act( kitty( "a=t,i=1,f=664,s=1,v=1", std::string( 8, '\0' ) ) ).find( "ENOTSUP" ) != std::string::npos,
           "new state is negotiated before use" );

  // Feature queries have no payload and can be interleaved into an upload.
  server.act( kitty( "a=t,i=95,f=32,s=1,v=1,m=1", std::string( 2, '\xff' ) ) );
  std::string answer = server.act( kitty( "a=q,i=96,e=t,f=632,E=16,G=2,s=20001,O=4294967296,t=f", "" ) );
  require( answer.find( "e=t" ) != std::string::npos && answer.find( "f=632" ) != std::string::npos
             && answer.find( "E=16" ) != std::string::npos && answer.find( "O=4294967296" ) != std::string::npos,
           "feature query reports format, curve and 64-bit offsets" );
  require( server.act( kitty( "m=0", std::string( 2, '\xff' ) ) ).find( "i=95;OK" ) != std::string::npos,
           "query did not disturb upload" );
  KittyCommand cmd;
  require( parse_kitty_command( "Ga=t,t=f,S=4294967297,O=4294967296", cmd ) && cmd.data_size == 4294967297ULL
             && cmd.data_offset == 4294967296ULL && !parse_kitty_command( "GS=18446744073709551616", cmd ),
           "file offsets are 64-bit and overflow is malformed" );
  require( server.act( kitty( "a=q,i=90,t=f,O=9223372036854775807,S=1", "/tmp/not-opened" ) ).find( "EOVERFLOW" ) != std::string::npos,
           "file range overflow refused before opening" );
  char path[] = "/tmp/skiff-image-XXXXXX"; // mkstemp requires a writable filename template.
  const int fd = mkstemp( path );
  require( fd >= 0, "create sparse image file" );
  const off_t offset = off_t( uint64_t( 1 ) << 32 );
  const bool written = pwrite( fd, "rgb", 3, offset ) == 3;
  close( fd );
  cmd = KittyCommand();
  cmd.medium = 'f'; cmd.data_offset = uint64_t( 1 ) << 32; cmd.data_size = 3; cmd.payload = path;
  std::string file_data, file_error;
  const bool read = kitty_read_medium( cmd, file_data, file_error );
  unlink( path );
  require( written && read && file_data == "rgb", "read actual image data beyond a 32-bit file offset" );

}

void program_status()
{
  Complete server( 40, 5 );
  const std::string body = "7501;state=working:app=build:id=" + std::string( 12000, 'x' );
  server.act( "\033]" + body + "\033" );
  require( server.take_notices().empty(), "status waits for complete ST" );
  server.act( "\\" );
  require( server.take_notices() == std::vector<std::string>{ "\033]" + body + "\033\\" }, "long status uses priority notices" );
  server.act( "\033]7501;state=done\007\033]7501;state=idle\033\\" );
  const auto notices = server.take_notices();
  require( notices.size() == 2 && notices[0] == "\033]7501;state=done\033\\" && notices[1] == "\033]7501;state=idle\033\\",
           "status is ordered and normalized" );
  require( server.take_extension_outputs().empty() && server.diff_from( Complete( 40, 5 ) ).find( "7501" ) == std::string::npos,
           "status never becomes screen state or normal priority output" );
  server.act( "\033]7501;state=error\033X" );
  require( server.take_notices().empty(), "aborted ST emits no status" );
  server.act( "\033]133;A\033\\\033c" );
  require( server.take_notices() == std::vector<std::string>{ "\033]7501;process-exited\033\\", "\033]7501;state=clear\033\\" },
           "prompt and hard reset preserve status lifetime ordering" );
  const std::string answer = "\033]7501;?:states=idle,working,done,blocked,error:kinds=permission\033\\";
  Ext::InputDemux demux;
  Ext::InputDemux::Output demuxed;
  for ( char c : answer ) { demux.feed( std::string( 1, c ), demuxed ); }
  require( demuxed.items.size() == 1 && demuxed.items[0].first && demuxed.items[0].second == answer,
           "fragmented status replies are frames, not keystrokes" );
  ExtensionClient input;
  ClientGeometry geometry;
  const auto result = input.frame( answer, server.get_fb(), geometry, 1000 );
  require( result.notice == answer && program_status_answer_ok( answer )
             && !program_status_answer_ok( "\033]7501;?\033]0;injected\033\\" ),
           "status query replies use the validated priority return channel" );

}

void depth()
{
  Complete server( 40, 5 ), client( 40, 5 );
  const auto style = []( const Complete& c, int col ) {
    return text_style_of( c.get_fb().get_cell( 0, col )->get_renditions().get_text_style() );
  };
  server.act( "a" + osc( "depth", "v=1:op=set:d=-150" ) + "b" + osc( "depth", "v=1:op=set:y=0.125" ) + "c"
              + osc( "depth", "v=1:op=set:p=-0.25" ) + "d" + osc( "depth", "v=1:op=set:d=0" ) + "e"
              + osc( "depth", "v=1:op=reset" ) + "f" );
  require( !style( server, 0 ).has_depth() && style( server, 1 ).depth == -150 && style( server, 1 ).phase == 0,
           "millidegrees, for the text that follows" );
  require( style( server, 2 ).depth == -150 && style( server, 2 ).phase == 0.125f, "a phase alone keeps the depth" );
  require( style( server, 3 ).depth == 0 && style( server, 3 ).depth_pixels == -0.25f && style( server, 3 ).phase == 0.125f,
           "pixels replace millidegrees" );
  require( style( server, 4 ).depth_pixels == 0 && style( server, 4 ).phase == 0.125f && !style( server, 5 ).has_depth(),
           "d=0 resets the depth alone, a reset both" );

  /* Rejected as Silkie rejects them: nothing changes. */
  server.act( "\r" + osc( "depth", "v=1:op=set:d=100" ) );
  for ( const char* bad : { "v=1:op=set:d=5:p=1", "v=1:op=set:p=", "v=1:op=set:p=1e40", "v=1:op=set:y=1e-50",
                            "v=1:op=set:y=nan", "v=1:op=set:p=0x10", "v=1:op=set:d=2147483648", "v=2:op=set:d=5",
                            "v=1:op=set:d=5:d=6", "v=1:op=reset:y=abc", "v=1:op=other:d=5" } ) {
    server.act( osc( "depth", bad ) );
    const TextStyle& now = text_style_of( server.get_fb().ds.get_renditions().get_text_style() );
    require( now.depth == 100 && now.depth_pixels == 0 && now.phase == 0, std::string( "rejected whole: " ) + bad );
  }
  server.act( osc( "depth", "v=1:op=set:p=1.5e0:y=-2.5" ) );
  require( text_style_of( server.get_fb().ds.get_renditions().get_text_style() ).depth_pixels == 1.5f,
           "numbers as strtod reads them" );

  /* It is kept through what keeps the other attributes, and is not the font's. */
  server.act( osc( "font", "v=1:op=select:role=serif:weight=300" ) + osc( "depth", "v=1:op=set:d=250:y=0" ) );
  TextStyle both = text_style_of( server.get_fb().ds.get_renditions().get_text_style() );
  require( both.role == "serif" && both.depth == 250 && both.depth_pixels == 0, "depth and font together" );
  server.act( "\033[1;31m" + osc( "font", "v=1:op=reset" ) );
  both = text_style_of( server.get_fb().ds.get_renditions().get_text_style() );
  require( both.role == "default" && both.weight == 0 && both.depth == 250,
           "colors and a font reset leave the depth alone" );
  server.act( "\0337" + osc( "depth", "v=1:op=reset" ) + "\0338" );
  require( text_style_of( server.get_fb().ds.get_renditions().get_text_style() ).depth == 250,
           "cursor save and restore carry it" );
  server.act( "\033[0m" );
  require( !server.get_fb().ds.get_renditions().has_depth(), "SGR 0 resets it" );
  for ( const std::string& reset : { osc( "caps", "v=1:op=reset" ), std::string( "\033]133;A\033\\" ),
                                     std::string( "\033[!p" ), std::string( "\033c" ) } ) {
    server.act( osc( "depth", "v=1:op=set:d=40:y=1" ) );
    require( server.get_fb().ds.get_renditions().has_depth(), "set again" );
    server.act( reset );
    require( !server.get_fb().ds.get_renditions().has_depth(), "the mode reset, a prompt and a terminal reset return to the plane" );
  }

  /* Across the link: the cells, exactly, floats included. */
  Complete far( 40, 5 ), near( 40, 5 );
  far.act( osc( "depth", "v=1:op=set:p=0.1:y=-0.3" ) + "x" + osc( "depth", "v=1:op=set:d=-150:y=0" ) + "y"
           + osc( "depth", "v=1:op=reset" ) + "z" );
  near.apply_string( far.diff_from( Complete( 40, 5 ) ) );
  require( style( near, 0 ).depth_pixels == 0.1f && style( near, 0 ).phase == -0.3f && style( near, 1 ).depth == -150
             && style( near, 1 ).phase == 0 && !style( near, 2 ).has_depth(),
           "the client's cells have the depth of the server's" );
  require( float_text( 0.1f ) == "0.1" && float_text( -0.25f ) == "-0.25" && float_text( 0 ) == "0"
             && strtof( float_text( 1.0f / 3 ).c_str(), nullptr ) == 1.0f / 3,
           "floats are written as the shortest text that reads back the same" );

  /* Drawn for a terminal that has the selector, and not for another. */
  Display display( false );
  display.set_extensions( ClientExtensions() );
  std::string frame = display.new_frame( false, Framebuffer( 40, 5 ), near.get_fb() );
  require( frame.find( "777;depth" ) == std::string::npos, "a terminal without depth is sent none" );
  ClientExtensions spatial = silkie();
  require( consume_probe_reply( osc( "caps", "v=1:op=cap:id=7253:caps=3:ext=caps.1,depth.1,font.1:media=d:format=24,32" ),
                                spatial )
             && spatial.speaks( Selector::DEPTH ),
           "the listing names the selector" );
  require( display.needs_repaint( spatial, near.get_fb() ), "text with a depth is drawn again once the terminal has it" );
  require( !display.needs_repaint( spatial, Complete( 40, 5 ).get_fb() ), "and nothing else is" );
  display.set_extensions( spatial );
  frame = display.new_frame( false, Framebuffer( 40, 5 ), near.get_fb() );
  const size_t first = frame.find( osc( "depth", "v=1:op=set:p=0.1:y=-0.3" ) + "x" );
  const size_t second = frame.find( osc( "depth", "v=1:op=set:d=-150:y=0" ) + "y" );
  require( first != std::string::npos && second != std::string::npos && first < second
             && frame.find( "z", second ) != std::string::npos
             && frame.find( "777;depth", second + 20 ) == std::string::npos,
           "each run is given its depth, and SGR 0 returns to the plane" );

  /* The listing has the selector when the terminal does. */
  Complete listed( 40, 5 );
  listed.set_client_extensions( spatial );
  auto reply = frames( listed.act( osc( "caps", "v=1:op=query:id=9" ) ) );
  require( reply.size() == 1 && ( "," + field( reply[0], "ext" ) + "," ).find( ",depth.1," ) != std::string::npos,
           "the relay lists depth for a terminal that has it" );
  listed.set_client_extensions( silkie() );
  reply = frames( listed.act( osc( "caps", "v=1:op=query:id=9" ) ) );
  require( reply.size() == 1 && field( reply[0], "ext" ).find( "depth" ) == std::string::npos, "and not otherwise" );

  /* A placement keeps the depth the text had when it was made. */
  Complete images( 40, 5 ), shown( 40, 5 );
  images.act( kitty( "a=t,i=3,f=24,s=1,v=1", std::string( "\x10\x20\x30", 3 ) ) );
  images.act( osc( "depth", "v=1:op=set:d=-150:y=0.5" ) + kitty( "a=p,i=3,p=1,c=1,r=1", "" )
              + osc( "depth", "v=1:op=reset" ) + kitty( "a=p,i=3,p=2,c=1,r=1", "" )
              + osc( "depth", "v=1:op=set:p=0.75" ) + kitty( "a=p,i=3,p=3,c=1,r=1", "" ) );
  shown.apply_string( images.diff_from( Complete( 40, 5 ) ) );
  const KittyPlacement *one = placement( shown.get_fb(), 3, 1 ), *two = placement( shown.get_fb(), 3, 2 ),
                       *three = placement( shown.get_fb(), 3, 3 );
  require( one && two && three && one->depth == -150 && one->phase == 0.5f && !two->has_depth()
             && three->depth_pixels == 0.75f && three->depth == 0,
           "placements reach the client with their depth" );
  std::string out;
  CursorDepth cursor;
  append_kitty_frame( out, false, Framebuffer( 40, 5 ), shown.get_fb(), false, false, nullptr, false, false, 0, true,
                      &cursor );
  const bool touched = cursor.known && cursor.depth_pixels == 0.75f;
  const size_t at1 = out.find( osc( "depth", "v=1:op=set:d=-150:y=0.5" ) ), put1 = out.find( "a=p,C=1,q=2,i=3,p=1" );
  const size_t at2 = out.find( osc( "depth", "v=1:op=reset" ) ), put2 = out.find( "a=p,C=1,q=2,i=3,p=2" );
  const size_t at3 = out.find( osc( "depth", "v=1:op=set:p=0.75:y=0" ) ), put3 = out.find( "a=p,C=1,q=2,i=3,p=3" );
  require( touched && at1 < put1 && put1 < at2 && at2 < put2 && put2 < at3 && at3 < put3 && put3 != std::string::npos,
           "each is placed at its own depth: " + out );
  out.clear();
  append_kitty_frame( out, false, Framebuffer( 40, 5 ), shown.get_fb() );
  require( out.find( "777;depth" ) == std::string::npos && out.find( "a=p,C=1,q=2,i=3,p=1" ) != std::string::npos,
           "a terminal without depth is given the placements alone" );
  /* The text drawn after them has its own depth again. */
  Display graphics( false );
  graphics.set_graphics( ClientGraphics( true ), true );
  graphics.set_extensions( spatial );
  Complete mixed( 40, 5 ), mixed_shown( 40, 5 );
  mixed.act( kitty( "a=t,i=3,f=24,s=1,v=1", std::string( "\x10\x20\x30", 3 ) ) + kitty( "a=p,i=3,p=1,c=1,r=1", "" )
             + osc( "depth", "v=1:op=set:d=77" ) );
  mixed_shown.apply_string( mixed.diff_from( Complete( 40, 5 ) ) );
  frame = graphics.new_frame( false, Framebuffer( 40, 5 ), mixed_shown.get_fb() );
  const size_t placed = frame.find( "a=p,C=1,q=2,i=3,p=1" );
  require( placed != std::string::npos && frame.find( "777;depth" ) > placed
             && frame.find( osc( "depth", "v=1:op=set:d=77:y=0" ), placed ) != std::string::npos,
           "a placement on the plane needs nothing said; the cursor is then given the depth of the text" );
  Complete deep( 40, 5 ), deep_shown( 40, 5 );
  deep.act( kitty( "a=t,i=3,f=24,s=1,v=1", std::string( "\x10\x20\x30", 3 ) ) + osc( "depth", "v=1:op=set:d=77" )
            + kitty( "a=p,i=3,p=1,c=1,r=1", "" ) + osc( "depth", "v=1:op=reset" ) );
  deep_shown.apply_string( deep.diff_from( Complete( 40, 5 ) ) );
  frame = graphics.new_frame( false, Framebuffer( 40, 5 ), deep_shown.get_fb() );
  const size_t deep_placed = frame.find( "a=p,C=1,q=2,i=3,p=1" );
  const size_t given = frame.find( osc( "depth", "v=1:op=set:d=77:y=0" ) );
  require( deep_placed != std::string::npos && given < deep_placed
             && frame.find( osc( "depth", "v=1:op=reset" ), deep_placed ) != std::string::npos,
           "a placement with a depth is given it, and the cursor returned to the plane of the text" );
  require( graphics.needs_repaint( silkie(), mixed_shown.get_fb() ) == false
             && Display( false ).needs_repaint( spatial, shown.get_fb() ) == false,
           "nothing to draw again where no depth is shown or nothing changed" );

  /* Vector records name a depth and a phase; they are checked as numbers. */
  Complete vector( 40, 5 );
  vector.set_client_extensions( spatial );
  ClientExtensions drawing = spatial;
  drawing.format = "24,32,200";
  vector.set_client_extensions( drawing );
  require( vector.act( kitty( "a=t,f=200,i=5,s=100,v=100",
                              "g id=1 depth=-150\np id=2 g=1 d=M10,10h80v40h-80Z fill=#4488ff depth_px=0.25 phase=-0.5" ) )
             .find( ";OK" ) != std::string::npos,
           "depth, depth_px and phase on records" );
  require( kitty_vector_records( *vector.get_fb().find_kitty_image( 5 ) ).find( "depth_px=0.25" ) != std::string::npos,
           "kept for the terminal, which draws them" );
  require( vector.act( kitty( "a=t,f=200,i=6,s=100,v=100", "g id=1 depth=near" ) ).find( "EINVAL:Line 1: Bad number" )
             != std::string::npos,
           "a depth that is not a number is refused" );
}

/* Desktop notifications: OSC 9, OSC 99 with Silkie's return route, and OSC
   777 notify. They are not state; the terminal is given each one once, and
   what it answers goes back to the program. */
void notifications()
{
  const std::string answer = "a=focus,report:o=always,unfocused,invisible:u=0,1,2:p=title,body,?,close,icon,alive,buttons:"
                             "c=1:w=1:s=system,silent,error";
  Complete server( 40, 5 );
  const auto one = [&]() {
    const auto taken = server.take_notices();
    return taken.size() == 1 ? taken[0] : "<" + std::to_string( taken.size() ) + " notifications>";
  };
  require( server.act( "\033]9;build finished; all green\007" ).empty()
             && one() == "\033]9;build finished; all green\033\\",
           "OSC 9, semicolons and all, whatever ended it" );
  server.act( "\033]777;notify;Tests;42 pass\033\\" );
  require( one() == "\033]777;notify;Tests;42 pass\033\\", "OSC 777 notify" );
  {
    /* Its text is the program's, in the UTF-8 of a session. */
    const char* previous = setlocale( LC_CTYPE, nullptr );
    const std::string saved = previous ? previous : "C";
    bool utf8 = false;
    for ( const char* name : { "C.UTF-8", "en_US.UTF-8", "C.utf8", "en_US.utf8" } ) {
      if ( ( utf8 = setlocale( LC_CTYPE, name ) != nullptr ) ) {
        break;
      }
    }
    if ( utf8 ) {
      server.act( "\033]777;notify;Tests;42 pass\xe2\x80\x94none fail\033\\\033]9;\xe5\xae\x8c\xe6\x88\x90\007" );
      const auto taken = server.take_notices();
      require( taken.size() == 2 && taken[0] == "\033]777;notify;Tests;42 pass\xe2\x80\x94none fail\033\\"
                 && taken[1] == "\033]9;\xe5\xae\x8c\xe6\x88\x90\033\\",
               "text beyond ASCII crosses as it was written" );
      setlocale( LC_CTYPE, saved.c_str() );
    } else {
      std::cout << "No UTF-8 locale; notifications beyond ASCII skipped.\n";
    }
  }
  const std::string routed = "\033]99;i=foo:r=odin-seat/sessionA/3,odin-seat/sessionB/8:a=focus,report;Hello\033\\";
  server.act( routed );
  require( one() == routed, "OSC 99 as the program wrote it, its return route untouched" );
  server.act( "\033]99;i=foo:d=0:p=body;" + std::string( 5000, 'x' ) + "\033\\\033]99;i=foo:d=1:p=close;\033\\" );
  require( server.take_notices().size() == 2, "each fragment of a notification, and its close, in order" );
  server.act( "\033]99;i=live:p=alive;\033\\" );
  require( one() == "\033]99;i=live:p=alive;\033\\", "what only the terminal can answer is the terminal's to answer" );

  /* Progress is not a notification: it goes with the other frames. */
  server.take_extension_outputs();
  server.act( "\033]9;4;1;50\033\\\033]9;4\033\\" );
  const auto progress = server.take_extension_outputs();
  require( server.take_notices().empty() && progress.size() == 2 && progress[0] == "\033]9;4;1;50\033\\"
             && progress[1] == "\033]9;4\033\\",
           "OSC 9;4 reports progress" );

  /* Nothing of a notification is screen or state. */
  server.act( "\033]9;secret\033\\visible" );
  server.take_notices();
  const std::string diff = server.diff_from( Complete( 40, 5 ) );
  require( diff.find( "secret" ) == std::string::npos && diff.find( "visible" ) != std::string::npos
             && server.get_fb().get_window_title().empty(),
           "a notification is neither drawn nor kept" );
  server.act( "\033]9;a\001b\033\\" );
  for ( const auto& notice : server.take_notices() ) {
    require( notice == "\033]9;ab\033\\", "no control character reaches the terminal inside a notification" );
  }

  /* The query is answered here, from what the client's terminal answered. */
  require( server.act( "\033]99;i=q:p=?;\033\\" ).empty() && server.take_notices().empty(),
           "no answer before the terminal is known, and the query is not passed on" );
  ClientExtensions terminal = silkie();
  require( terminal.notifications.empty() && !terminal.notification_routes, "a terminal that has not answered" );
  server.set_client_extensions( terminal );
  require( server.act( "\033]99;i=q:p=?;\033\\" ).empty(), "nor for a terminal that gave none" );
  require( consume_probe_reply( "\033]99;i=7253:p=?:r=goblin-skiff/probe/0;" + answer + ":r=1\033\\", terminal )
             && terminal.notifications == answer && terminal.notification_routes,
           "Silkie's answer to the client's query, and that it keeps routes" );
  ClientBuffers::ClientExtensionsMessage message;
  client_extensions_to_proto( terminal, &message );
  require( client_extensions_from_proto( message ) == terminal, "which cross the link with the rest" );
  server.set_client_extensions( terminal );
  server.take_extension_outputs();
  require( server.act( "\033]99;i=q 1!:p=?;\033\\" ) == "\033]99;i=q1:p=?;" + answer + "\033\\",
           "the query answered at once, its identifier as kitty keeps one" );
  require( server.act( "\033]99;p=?;\033\\" ) == "\033]99;i=0:p=?;" + answer + "\033\\", "and 0 for none" );
  require( server.act( "\033]99;i=q2:p=?:r=odin-seat/s/3,odin-seat/t/4;\033\\" )
             == "\033]99;i=q2:p=?:r=odin-seat/s/3,odin-seat/t/4;" + answer + ":r=1\033\\",
           "with a route: echoed, and the answer says routes are kept" );
  require( server.act( "\033]99;i=q3:p=?:r=not a route;\033\\" ).empty()
             && server.act( "\033]99;i=q3:p=?:r=a/b/c;\033\\" ).empty() && server.act( "\033]99;i=q3:p=?:novalue;\033\\" ).empty(),
           "a malformed route or metadata is not answered, as Silkie does not" );
  require( server.take_notices().empty(), "no query reaches the terminal as a notification" );
  const auto asked = server.take_extension_outputs();
  require( !asked.empty() && asked[0] == notification_probe(), "the terminal is asked again, for the next program" );

  /* kitty answers the query and keeps no routes. */
  ClientExtensions kitty_terminal = silkie();
  require( consume_probe_reply( "\033]99;i=7253:p=?;" + answer + "\033\\", kitty_terminal )
             && kitty_terminal.notifications == answer && !kitty_terminal.notification_routes,
           "an answer without r=1" );
  server.set_client_extensions( kitty_terminal );
  require( server.act( "\033]99;i=q4:p=?:r=odin-seat/s/3;\033\\" ) == "\033]99;i=q4:p=?;" + answer + "\033\\",
           "a terminal without routes answers as kitty would" );
  require( !consume_probe_reply( "\033]99;i=foo;2\033\\", kitty_terminal ), "an activation is not an answer to the query" );

  /* The client: its terminal's input, and what becomes of each frame. */
  Ext::InputDemux demux;
  Ext::InputDemux::Output out;
  const std::string clicked = "\033]99;i=foo:r=odin-seat/sessionA/3,odin-seat/sessionB/8,silkie/terminalC/12:a=focus,report;2\033\\";
  demux.feed( "ab" + clicked.substr( 0, 20 ), out );
  demux.feed( clicked.substr( 20 ) + "cd", out );
  require( out.items.size() == 3 && !out.items[0].first && out.items[1].first && out.items[1].second == clicked
             && out.items[2].second == "cd",
           "the terminal's answers are taken out of its input" );
  ExtensionClient client;
  const Framebuffer shown( 40, 5 );
  const ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
  auto result = client.frame( clicked, shown, geometry, 1000 );
  require( result.notice == clicked && !result.forward && !result.info_changed,
           "a click, with the whole route, is for the program" );
  result = client.frame( "\033]99;i=foo:p=close;untracked\007", shown, geometry, 1000 );
  require( result.notice == "\033]99;i=foo:p=close;untracked\033\\", "a close too, whatever ended it" );
  result = client.frame( "\033]99;i=7253:p=?:r=goblin-skiff/probe/0;" + answer + ":r=1\033\\", shown, geometry, 1000 );
  require( result.notice.empty() && result.info_changed && client.info.notifications == answer
             && client.info.notification_routes,
           "the answer to the client's own query is the client's" );
  require( client.probe().find( notification_probe() ) != std::string::npos, "which it asks with the rest of its probe" );
  require( notification_answer_ok( clicked ) && !notification_answer_ok( "\033]99;i=a;\033\\x" )
             && !notification_answer_ok( "\033]99;i=a\033]0;title\007;\033\\" ) && !notification_answer_ok( "\033]52;c;AAAA\033\\" )
             && !notification_answer_ok( "\033]99;i=a\r;\033\\" ),
           "only a whole OSC 99 with nothing else in it is handed to a program" );
  require( is_notification( "9;x" ) && is_notification( "99;;x" ) && is_notification( "777;notify;a;b" )
             && !is_notification( "777;caps;v=1" ) && !is_notification( "90;x" ) && is_progress_report( "9;4;3" )
             && !is_progress_report( "9;40" ),
           "what a notification is" );

  /* An extension frame is still an extension frame. */
  server.set_client_extensions( terminal );
  require( frames( server.act( osc( "caps", "v=1:op=query:id=1" ) ) ).size() == 1 && server.take_notices().empty(),
           "OSC 777 with another selector is not a notification" );
}

/* Images and attributes are kept as they were sent, whatever the client has
   said so far, and drawn for the terminal as it is when they are drawn. */
void late_capabilities()
{
  Complete server( 20, 5 ), client( 20, 5 );
  const std::string hdr = half( 1 ) + half( 0.5f ) + half( 4 ) + half( 1 ) + half( 2 ) + half( 0.25f ) + half( 0.25f )
                          + half( 1 );
  require( server.act( kitty( "a=T,i=4,f=64,s=2,v=1,T=1", hdr ) ).find( ";OK" ) != std::string::npos,
           "an HDR image before the client has said anything" );
  require( server.act( kitty( "a=T,i=5,f=24,s=1,v=1,G=1", std::string( "\x64\x96\xc8", 3 ) ) ).find( ";OK" )
             != std::string::npos,
           "and a Display P3 one" );
  require( server.get_fb().find_kitty_image( 4 )->format == KITTY_FORMAT_HDR
             && server.get_fb().find_kitty_image( 4 )->tone_map && *server.get_fb().find_kitty_image( 4 )->data == hdr,
           "the HDR pixels are kept as sent, with T" );
  std::string rgba;
  require( server.get_fb().find_kitty_image( 5 )->gamut == 1
             && kitty_image_to_rgba( *server.get_fb().find_kitty_image( 5 ), rgba ) && uint8_t( rgba[0] ) == 0x64
             && uint8_t( rgba[1] ) == 0x96 && uint8_t( rgba[2] ) == 0xc8,
           "the wide pixels are kept as sent, with G" );
  client.apply_string( server.diff_from( Complete( 20, 5 ) ) );
  require( client.get_fb().find_kitty_image( 4 )->format == KITTY_FORMAT_HDR
             && client.get_fb().find_kitty_image( 5 )->gamut == 1,
           "and reach the client so" );

  /* Drawn for a terminal without HDR or G: converted then. */
  const Framebuffer blank( 20, 5 );
  std::string out, keys;
  append_kitty_frame( out, false, blank, client.get_fb() );
  std::string sdr = sent_pixels( out, "t", 4, &keys );
  require( keys.find( "f=32" ) != std::string::npos && keys.find( "T=" ) == std::string::npos && sdr.size() == 8
             && uint8_t( sdr[2] ) == 255 && uint8_t( sdr[0] ) < 200,
           "an HDR image is shown at SDR, compressed with T=1: " + keys );
  std::string plain = sent_pixels( out, "t", 5, &keys );
  require( keys.find( "G=" ) == std::string::npos && plain.size() == 4
             && !( uint8_t( plain[0] ) == 0x64 && uint8_t( plain[1] ) == 0x96 ),
           "a Display P3 image as the nearest sRGB colors" );
  out.clear();
  append_kitty_frame( out, false, blank, client.get_fb(), false, true, nullptr, false, true );
  require( sent_pixels( out, "t", 4, &keys ) == hdr && keys.find( "f=64" ) != std::string::npos
             && keys.find( "T=1" ) != std::string::npos,
           "and as sent to a terminal that has HDR" );
  require( sent_pixels( out, "t", 5, &keys ).substr( 0, 3 ) == "\x64\x96\xc8" && keys.find( "G=1" ) != std::string::npos,
           "and G" );

  /* The listing arriving later: what it changes is drawn again. */
  Display display( false );
  display.set_graphics( ClientGraphics( true ), true );
  display.set_extensions( ClientExtensions() );
  ClientExtensions later = silkie();
  require( display.needs_repaint( later, client.get_fb() ), "images in a wide gamut or HDR are sent again" );
  later.image_gamut = false;
  later.format = "24,32,100";
  require( !display.needs_repaint( later, client.get_fb() ), "not when nothing they need changed" );
  Complete text( 20, 5 );
  text.act( "\033[56:200mX" );
  require( display.needs_repaint( later, text.get_fb() ), "Silkie's attributes are drawn again for the listing" );
  Complete plain_text( 20, 5 );
  plain_text.act( "\033[1mX" );
  require( !display.needs_repaint( later, plain_text.get_fb() ), "other text is not" );
  display.set_extensions( silkie() );
  require( !display.needs_repaint( silkie(), client.get_fb() ) && !display.needs_repaint( silkie(), text.get_fb() ),
           "and nothing once it is known" );

  /* A rectangle of an image in a wide gamut, written over it in place. */
  Framebuffer last( 20, 5 );
  KittyImage image;
  require( kitty_normalize_image( KITTY_FORMAT_RGBA, 2, 1, std::string( "\x64\x96\xc8\xff\x64\x96\xc8\xff", 8 ), image,
                                  ImageEncoding(), error_sink() ),
           "an image" );
  image.id = 9;
  image.gamut = 1;
  last.put_kitty_image( image );
  Framebuffer now( last );
  KittyImage changed = *now.find_kitty_image( 9 );
  auto delta = std::make_shared<ImageDelta>();
  delta->base_serial = changed.serial;
  delta->x = 1;
  require( kitty_normalize_image( KITTY_FORMAT_RGBA, 1, 1, std::string( "\x10\x80\xf0\xff", 4 ), delta->rect,
                                  ImageEncoding(), error_sink() ),
           "a rectangle" );
  changed.delta = delta;
  now.refresh_kitty_image( changed );
  out.clear();
  append_kitty_frame( out, true, last, now, false, true );
  require( sent_pixels( out, "f", 9, &keys ) == std::string( "\x10\x80\xf0\xff", 4 ) && keys.find( "G=1" ) != std::string::npos,
           "the rectangle is in the gamut of the image: " + keys );
  out.clear();
  append_kitty_frame( out, true, last, now );
  std::string converted = sent_pixels( out, "f", 9, &keys );
  require( keys.find( "G=" ) == std::string::npos && converted.size() == 4 && converted != std::string( "\x10\x80\xf0\xff", 4 ),
           "or converted, as the image was, for a terminal without G" );
}

/* a=f, a=c and a=a, as far as an image of one frame has them; and chunks of
   them are never taken for images. */
void animation_frames()
{
  Complete server( 20, 5 );
  const std::string blue( "\0\0\xff\xff\0\0\xff\xff", 8 );
  server.act( kitty( "a=T,i=1,f=32,s=2,v=1,q=2", blue ) );
  /* A chunked a=f: the chunks after the first carry no action. */
  const std::string red = base64( std::string( "\xff\0\0\x80", 4 ) );
  std::string reply = server.act( "\033_Ga=f,i=1,r=1,f=32,s=1,v=1,x=1,m=1;" + red.substr( 0, 4 ) + "\033\\" );
  require( reply.empty(), "the first chunk is held" );
  reply = server.act( "\033_Gm=0;" + red.substr( 4 ) + "\033\\" );
  require( reply == "\033_Gi=1,r=1;OK\033\\", "the root frame is edited: " + reply );
  require( server.get_fb().get_kitty_images().size() == 1 && server.get_fb().get_kitty_placements().size() == 1,
           "no image is made of the chunks, and the placement stays" );
  std::string rgba;
  require( kitty_image_to_rgba( *server.get_fb().find_kitty_image( 1 ), rgba )
             && rgba == std::string( "\0\0\xff\xff\x80\0\x7f\xff", 8 ),
           "blended over the root frame as kitty blends" );
  Complete client( 20, 5 );
  client.apply_string( server.diff_from( Complete( 20, 5 ) ) );
  const Complete acked = server; /* what the server knows the client has */
  const Framebuffer shown = client.get_fb();
  reply = server.act( kitty( "a=f,i=1,r=1,f=24,s=1,v=1,X=1", std::string( "\0\xff\0", 3 ) ) );
  require( reply == "\033_Gi=1,r=1;OK\033\\" && kitty_image_to_rgba( *server.get_fb().find_kitty_image( 1 ), rgba )
             && rgba == std::string( "\0\xff\0\xff\x80\0\x7f\xff", 8 ),
           "X=1 replaces" );
  client.apply_string( server.diff_from( acked ) );
  std::string out;
  append_kitty_frame( out, true, shown, client.get_fb() );
  require( out.find( "a=f,r=1" ) != std::string::npos && out.find( "a=t" ) == std::string::npos,
           "the terminal is sent the rectangle that changed: " + out );

  /* What an image of one frame has not. */
  reply = server.act( "\033_Ga=f,i=1,f=32,s=1,v=1,m=1;" + red.substr( 0, 4 ) + "\033\\\033_Gm=0;" + red.substr( 4 ) + "\033\\" );
  require( reply == "\033_Gi=1,r=2;EINVAL:Animation frames other than the root frame are not supported\033\\",
           "a new frame is refused, after its chunks: " + reply );
  require( server.get_fb().get_kitty_images().size() == 1, "and its chunks are not an image" );
  reply = server.act( kitty( "a=f,i=7,r=1,f=24,s=1,v=1", std::string( 3, '\0' ) ) );
  require( reply == "\033_Gi=7,r=1;ENOENT:Animation command refers to non-existent image with id: 7 and number: 0\033\\",
           "no such image: " + reply );
  reply = server.act( kitty( "a=f,i=1,r=1,f=64,s=1,v=1", std::string( 8, '\0' ) ) );
  require( reply.find( "EINVAL:Animation frame format must match the image format" ) != std::string::npos, reply );
  reply = server.act( kitty( "a=f,i=1,r=1,f=32,s=3,v=1", std::string( 12, '\0' ) ) );
  require( reply.find( "EINVAL:Frame width 3 larger than image width: 2" ) != std::string::npos, reply );
  reply = server.act( kitty( "a=f,i=1,r=1,f=32,s=2,v=1", std::string( 4, '\0' ) ) );
  require( reply.find( "ENODATA:Insufficient image data 4 < 8" ) != std::string::npos, reply );

  /* a=c within the root frame. */
  reply = server.act( "\033_Ga=c,i=1,r=1,c=1,x=1,X=0,w=1,h=1,C=1\033\\" );
  require( reply == "\033_Gi=1;OK\033\\" && kitty_image_to_rgba( *server.get_fb().find_kitty_image( 1 ), rgba )
             && rgba == std::string( "\0\xff\0\xff\0\xff\0\xff", 8 ),
           "a rectangle copied within the frame: " + reply );
  reply = server.act( "\033_Ga=c,i=1,r=1,c=1,x=0,X=0,w=1,h=1\033\\" );
  require( reply.find( "EINVAL:The source and destination rectangles overlap" ) != std::string::npos, reply );
  reply = server.act( "\033_Ga=c,i=1,r=2,c=1\033\\" );
  require( reply == "\033_Gi=1;ENOENT:No source frame number 2 exists in image id: 1\n\033\\", "kitty's words: " + reply );
  reply = server.act( "\033_Ga=c,i=1,r=1,c=1,x=1,w=2\033\\" );
  require( reply.find( "EINVAL:The destination rectangle is out of bounds" ) != std::string::npos, reply );

  /* a=a: nothing to animate, and kitty says nothing unless the image is not there. */
  require( server.act( "\033_Ga=a,i=1,s=3,v=1\033\\" ).empty(), "a=a on an image" );
  reply = server.act( "\033_Ga=a,i=8,s=3\033\\" );
  require( reply.find( "ENOENT:Animation command refers to non-existent image with id: 8" ) != std::string::npos, reply );

  /* An HDR image is edited in linear light. */
  server.act( kitty( "a=t,i=2,f=64,s=1,v=1,q=2", half( 1 ) + half( 0 ) + half( 0 ) + half( 1 ) ) );
  reply = server.act( kitty( "a=f,i=2,r=1,f=64,s=1,v=1", half( 0 ) + half( 0 ) + half( 2 ) + half( 0.5f ) ) );
  const std::string& edited = *server.get_fb().find_kitty_image( 2 )->data;
  require( reply.find( ";OK" ) != std::string::npos && edited == half( 0.5f ) + half( 0 ) + half( 1 ) + half( 1 ),
           "an HDR frame blended in linear light: " + reply );
}

/* Relative placements: what kitty refuses is refused, and a tree moves with
   its root. */
void relative_placements()
{
  Complete server( 20, 10 );
  ClientGeometry geometry( 20, 10, 200, 200, 10, 20 );
  const auto act = [&]( const std::string& bytes ) { return server.act( bytes, &geometry ); };
  const std::string pixel = base64( std::string( "\0\0\0\xff", 4 ) );
  for ( int id = 1; id <= 11; id++ ) {
    act( "\033_Ga=t,f=32,s=1,v=1,i=" + std::to_string( id ) + ",q=2;" + pixel + "\033\\" );
  }
  std::string reply = act( "\033_Ga=p,i=2,p=1,P=99\033\\" );
  require( reply == "\033_Gi=2,p=1;ENOPARENT:Put command refers to a parent image with id: 99 that does not exist\033\\",
           "no parent image: " + reply );
  reply = act( "\033_Ga=p,i=2,p=1,P=1\033\\" );
  require( reply.find( "ENOPARENT:Put command refers to a parent image with id: 1 that has no placements" )
             != std::string::npos,
           reply );
  act( "\033[3;4H\033_Ga=p,i=1,p=5,C=1,q=2\033\\" );
  reply = act( "\033_Ga=p,i=2,p=1,P=1,Q=7\033\\" );
  require( reply.find( "ENOPARENT:Put command refers to a parent image placement with id: 1 and placement id: 7 that "
                       "does not exist" )
             != std::string::npos,
           reply );
  require( server.get_fb().get_kitty_placements().size() == 1, "none of these is placed" );
  require( act( "\033_Ga=p,i=2,p=1,P=1,H=2,V=3\033\\" ).find( ";OK" ) != std::string::npos, "a child" );
  require( placement( server.get_fb(), 2, 1 ) && placement( server.get_fb(), 2, 1 )->parent_placement == 5,
           "without Q, the first placement of the parent" );
  reply = act( "\033_Ga=p,i=1,p=5,P=2,Q=1\033\\" );
  require( reply.find( "ECYCLE:This parent reference creates a cycle" ) != std::string::npos, "a cycle: " + reply );
  reply = act( "\033_Ga=p,i=1,p=5,P=1,Q=5\033\\" );
  require( reply.find( "EINVAL:Put command refers to itself as its own parent" ) != std::string::npos, reply );
  require( !placement( server.get_fb(), 1, 5 )->parent_image, "the root is left as it was" );
  for ( int id = 3; id <= 9; id++ ) { /* image k has k - 1 ancestors */
    require( act( "\033_Ga=p,i=" + std::to_string( id ) + ",p=1,P=" + std::to_string( id - 1 ) + ",Q=1\033\\" ).find( ";OK" )
               != std::string::npos,
             "eight ancestors are allowed" );
  }
  reply = act( "\033_Ga=p,i=10,p=1,P=9,Q=1\033\\" );
  require( reply.find( "ETOODEEP:Too many levels of parent references" ) != std::string::npos, "nine are not: " + reply );
  reply = act( "\033_Ga=p,i=10,p=1,U=1,P=9\033\\" );
  require( reply.find( "EINVAL:Put command creating a virtual placement cannot refer to a parent" ) != std::string::npos,
           reply );
  /* a=T has its answer for the image; a placement kitty may not make is not made. */
  reply = act( "\033_Ga=T,f=32,s=1,v=1,i=12,P=99;" + pixel + "\033\\" );
  require( reply == "\033_Gi=12;OK\033\\" && server.get_fb().find_kitty_image( 12 )
             && server.get_fb().get_kitty_placements().size() == 9,
           "a=T with a parent that is not there: " + reply );

  /* The tree moves with its root, whatever the cursor was when it was placed. */
  int64_t row = 0, col = 0;
  require( kitty_placement_cell( server.get_fb(), *placement( server.get_fb(), 2, 1 ), row, col ) && row == 5 && col == 5,
           "the child is at its root plus H and V" );
  act( "\033[10;1H\n" );
  require( placement( server.get_fb(), 1, 5 )->row == 1 && placement( server.get_fb(), 2, 1 )
             && kitty_placement_cell( server.get_fb(), *placement( server.get_fb(), 2, 1 ), row, col ) && row == 4,
           "scrolled with its root" );
  const size_t count = server.get_fb().get_kitty_placements().size();
  act( "\033_Ga=d,d=p,x=1,y=10,q=2\033\\" ); /* where the cursor was when it was placed, scrolled */
  require( server.get_fb().get_kitty_placements().size() == count, "not found where it is not" );
  server.act( Parser::Resize( 20, 3 ) );
  require( placement( server.get_fb(), 2, 1 ), "a child below the screen stays while its root does" );
  server.act( Parser::Resize( 20, 10 ) );
  act( "\033_Ga=d,d=p,x=6,y=5,z=0,q=2\033\\" );
  require( !placement( server.get_fb(), 2, 1 ) && !placement( server.get_fb(), 3, 1 ),
           "found where it is shown, and its descendants go with it" );
  act( "\033_Ga=p,i=2,p=1,P=1,Q=5,H=2,V=3,q=2\033\\" );
  act( "\033[1;1H\033[3M" );
  require( !placement( server.get_fb(), 1, 5 ) && !placement( server.get_fb(), 2, 1 ),
           "the root scrolled away takes its tree" );

  /* A virtual parent is where the cells of its placeholders are. Cells keep
     their text in the locale's encoding, which is UTF-8 in a session. */
  const char* previous = setlocale( LC_CTYPE, nullptr );
  const std::string saved = previous ? previous : "C";
  bool utf8 = false;
  for ( const char* name : { "C.UTF-8", "en_US.UTF-8", "C.utf8", "en_US.utf8" } ) {
    if ( ( utf8 = setlocale( LC_CTYPE, name ) != nullptr ) ) {
      break;
    }
  }
  if ( !utf8 ) {
    std::cout << "No UTF-8 locale; virtual parents skipped.\n";
    return;
  }
  act( "\033_Ga=p,i=11,p=1,U=1,q=2\033\\" );
  act( "\033[4;5H\033[38;5;11m\xf4\x8e\xbb\xae\033[m" );
  require( act( "\033_Ga=p,i=2,p=2,P=11,Q=1,H=1,V=1\033\\" ).find( ";OK" ) != std::string::npos, "a virtual parent" );
  require( kitty_placement_cell( server.get_fb(), *placement( server.get_fb(), 2, 2 ), row, col ) && row == 4 && col == 5,
           "at its placeholders plus H and V" );
  KittyPlacementRect rect;
  require( kitty_placement_rect( server.get_fb(), *placement( server.get_fb(), 2, 2 ), 10, 20, rect ) && rect.left == 50
             && rect.top == 80,
           "and laid out there" );
  act( "\033[4;5H\033[K" );
  require( placement( server.get_fb(), 2, 2 )
             && !kitty_placement_cell( server.get_fb(), *placement( server.get_fb(), 2, 2 ), row, col ),
           "without its placeholders it is nowhere, but stays, as in kitty" );
  setlocale( LC_CTYPE, saved.c_str() );
}

/* XTGETTCAP and SGR 2. */
void termcap_and_dim()
{
  Complete server( 20, 5 ), client( 20, 5 );
  const std::string headroom = "6b697474792d71756572792d6864725f68656164726f6f6d";
  std::string reply = server.act( "\033P+q" + headroom + ";544e\033\\" );
  require( reply == "\033P1+r" + headroom + "=31\033\\\033P0+r544e\033\\",
           "the headroom is 1 before anything is known, and an unknown name is answered as kitty does: " + reply );
  server.set_client_extensions( silkie() );
  reply = server.act( "\033P+q" + headroom + "\033\\" );
  require( reply == "\033P1+r" + headroom + "=32\033\\", "then the display's: " + reply );

  server.act( "\033[2mA\033[1mB\033[22mC\033[2;56:200mD" );
  const Framebuffer& fb = server.get_fb();
  require( fb.get_cell( 0, 0 )->get_renditions().get_attribute( Renditions::faint )
             && fb.get_cell( 0, 1 )->get_renditions().get_attribute( Renditions::faint )
             && fb.get_cell( 0, 1 )->get_renditions().get_attribute( Renditions::bold )
             && !fb.get_cell( 0, 2 )->get_renditions().get_attribute( Renditions::faint )
             && !fb.get_cell( 0, 2 )->get_renditions().get_attribute( Renditions::bold ),
           "dim is kept, and 22 ends it with bold" );
  const Renditions& d = fb.get_cell( 0, 3 )->get_renditions();
  require( d.get_attribute( Renditions::faint ) && d.get_brightness( false ) == 200, "dim and brightness are apart" );
  client.apply_string( server.diff_from( client ) );
  require( client.get_fb().get_cell( 0, 3 )->get_renditions() == d, "and reach the client" );
  require( d.sgr().find( "\033[0;2" ) == 0, "and its terminal: " + d.sgr() );
}

/* PNG colors without png-state-v1, as Silkie reads them. */
std::string png_chunk( const std::string& type, const std::string& body )
{
  const std::string data = type + body;
  const uint32_t crc = uint32_t( crc32( 0, reinterpret_cast<const Bytef*>( data.data() ), data.size() ) );
  const uint32_t n = uint32_t( body.size() );
  return std::string( { char( n >> 24 ), char( n >> 16 ), char( n >> 8 ), char( n ) } ) + data
         + std::string( { char( crc >> 24 ), char( crc >> 16 ), char( crc >> 8 ), char( crc ) } );
}

std::string be32( uint32_t v )
{
  return std::string( { char( v >> 24 ), char( v >> 16 ), char( v >> 8 ), char( v ) } );
}

std::string s15( double v )
{
  return be32( uint32_t( int32_t( std::lround( v * 65536 ) ) ) );
}

/* A matrix and curves profile: the colorants adapted to D50, and the sRGB
   curve or, linear, none. */
std::string icc_profile( const double colorants[3][3], bool linear )
{
  std::vector<std::pair<std::string, std::string>> tags;
  const char* names[3] = { "rXYZ", "gXYZ", "bXYZ" };
  for ( int c = 0; c < 3; c++ ) {
    tags.emplace_back( names[c], "XYZ " + be32( 0 ) + s15( colorants[c][0] ) + s15( colorants[c][1] ) + s15( colorants[c][2] ) );
  }
  const std::string curve = linear ? "curv" + be32( 0 ) + be32( 0 )
                                   : "para" + be32( 0 ) + std::string( "\0\3\0\0", 4 ) + s15( 2.4 ) + s15( 1 / 1.055 )
                                       + s15( 0.055 / 1.055 ) + s15( 1 / 12.92 ) + s15( 0.04045 );
  for ( const char* name : { "rTRC", "gTRC", "bTRC" } ) {
    tags.emplace_back( name, curve );
  }
  std::string table = be32( uint32_t( tags.size() ) ), data;
  size_t offset = 128 + 4 + tags.size() * 12;
  for ( const auto& tag : tags ) {
    table += tag.first + be32( uint32_t( offset + data.size() ) ) + be32( uint32_t( tag.second.size() ) );
    data += tag.second;
    while ( data.size() % 4 ) {
      data += '\0';
    }
  }
  std::string header( 128, '\0' );
  const std::string body = table + data;
  header.replace( 0, 4, be32( uint32_t( 128 + body.size() ) ) );
  header.replace( 8, 4, be32( 0x02100000 ) );
  header.replace( 12, 4, "mntr" );
  header.replace( 68, 12, s15( 0.9642 ) + s15( 1.0 ) + s15( 0.8249 ) ); /* the D50 white libpng checks for */
  header.replace( 16, 4, "RGB " );
  header.replace( 20, 4, "XYZ " );
  header.replace( 36, 4, "acsp" );
  return header + body;
}

void png_colors()
{
  png_image image;
  std::memset( &image, 0, sizeof image );
  image.version = PNG_IMAGE_VERSION;
  image.width = 1;
  image.height = 1;
  image.format = PNG_FORMAT_RGBA;
  const unsigned char source[4] = { 0x80, 0x40, 0x20, 255 };
  png_alloc_size_t size = 0;
  require( png_image_write_to_memory( &image, nullptr, &size, 0, source, 0, nullptr ), "PNG size" );
  std::string png( size, '\0' );
  require( png_image_write_to_memory( &image, &png[0], &size, 0, source, 0, nullptr ), "PNG" );
  png.resize( size );
  /* Without the sRGB, gAMA and cHRM chunks libpng writes, which come first. */
  std::string bare = png.substr( 0, 33 );
  for ( size_t at = 33; at + 12 <= png.size(); ) {
    const size_t length = size_t( uint8_t( png[at] ) ) << 24 | size_t( uint8_t( png[at + 1] ) ) << 16
                          | size_t( uint8_t( png[at + 2] ) ) << 8 | uint8_t( png[at + 3] );
    const std::string type = png.substr( at + 4, 4 );
    if ( type != "sRGB" && type != "gAMA" && type != "cHRM" ) {
      bare += png.substr( at, length + 12 );
    }
    at += length + 12;
  }
  const auto with_chunk = [&]( const std::string& chunk ) { return bare.substr( 0, 33 ) + chunk + bare.substr( 33 ); };

  KittyImage stored;
  std::string rgba;
  const std::string p3 = with_chunk( png_chunk( "cICP", std::string( "\x0c\x0d\x00\x01", 4 ) ) );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, p3, stored, ImageEncoding(), error_sink() ) && stored.gamut == 1
             && kitty_image_to_rgba( stored, rgba ) && rgba == std::string( "\x80\x40\x20\xff", 4 ),
           "cICP Display P3: the pixels as they are, tagged" );
  const std::string pq = with_chunk( png_chunk( "cICP", std::string( "\x09\x10\x00\x01", 4 ) ) );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, pq, stored, ImageEncoding(), error_sink() ) && !stored.gamut,
           "cICP with another curve is ignored" );

  const double display_p3[3][3] = { { 0.515121, 0.241196, -0.001053 },
                                    { 0.291977, 0.692245, 0.041885 },
                                    { 0.157104, 0.066574, 0.784073 } };
  const double srgb[3][3] = { { 0.436066, 0.222488, 0.013916 },
                              { 0.385147, 0.716873, 0.097076 },
                              { 0.143066, 0.060608, 0.714096 } };
  const auto iccp = [&]( const std::string& profile ) {
    uLongf length = compressBound( profile.size() );
    std::string compressed( length, '\0' );
    compress( reinterpret_cast<Bytef*>( &compressed[0] ), &length, reinterpret_cast<const Bytef*>( profile.data() ),
              profile.size() );
    compressed.resize( length );
    return png_chunk( "iCCP", std::string( "profile\0\0", 9 ) + compressed );
  };
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, with_chunk( iccp( icc_profile( display_p3, false ) ) ), stored,
                                  ImageEncoding(), error_sink() )
             && stored.gamut == 1 && kitty_image_to_rgba( stored, rgba ) && std::abs( uint8_t( rgba[0] ) - 0x80 ) <= 1
             && std::abs( uint8_t( rgba[1] ) - 0x40 ) <= 1,
           "a Display P3 profile: tagged, on the sRGB curve" );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, with_chunk( png_chunk( "sRGB", std::string( 1, '\0' ) ) + iccp( icc_profile( display_p3, false ) ) ),
                                  stored, ImageEncoding(), error_sink() )
             && !stored.gamut,
           "with an sRGB chunk the profile is not looked at, as in Silkie" );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, with_chunk( iccp( icc_profile( srgb, true ) ) ), stored, ImageEncoding(),
                                  error_sink() )
             && !stored.gamut && kitty_image_to_rgba( stored, rgba ) && std::abs( uint8_t( rgba[0] ) - 188 ) <= 1,
           "any other profile is converted to sRGB: " + std::to_string( uint8_t( rgba[0] ) ) );
  Framebuffer fb( 20, 5 );
  require( kitty_normalize_image( KITTY_FORMAT_PNG, 0, 0, p3, stored, ImageEncoding(), error_sink() ), "again" );
  stored.id = 3;
  fb.put_kitty_image( stored );
  std::string out, keys;
  append_kitty_frame( out, false, Framebuffer( 20, 5 ), fb, false, true );
  require( sent_pixels( out, "t", 3, &keys ) == std::string( "\x80\x40\x20\xff", 4 ) && keys.find( "G=1" ) != std::string::npos,
           "and sent with G to a terminal that takes it" );
}

/* The relay id the server gave a request of the vector service it wrote. */
uint32_t relay_id_of( const std::string& output )
{
  KittyCommand command;
  require( parse_kitty_command( output.substr( 2, output.find( "\033\\" ) - 2 ), command ), "a request parses" );
  return command.image_id;
}

std::string service_reply( uint32_t relay, const std::string& body )
{
  return "Gi=" + std::to_string( relay ) + ";" + body;
}

/* Where a needle is in a haystack, or npos. */
size_t at( const std::string& haystack, const std::string& needle )
{
  return haystack.find( needle );
}

void vector_colors_and_groups()
{
  /* Colors overflow as the terminal's floats do: after the transfer curve,
     the gamut and the brightness. */
  for ( const char* color : { "#000000*1e41", "srgb:0/0/0*4e40", "p3:1/0/0*3e40", "linear:1e300/0/0*0",
                              "linear:1e39/0/0", "rec2020:1/1/1*1e41" } ) {
    require( !Vector::color_paints( color ), std::string( "an overflow: " ) + color );
    std::vector<Vector::Record> records;
    std::string error;
    require( !Vector::parse_records( std::string( "p id=1 d=M0,0 fill=" ) + color, records, error )
               && error == "Line 1: Color overflow",
             std::string( color ) + " gave " + error );
  }
  for ( const char* color : { "#ff0000*250", "#000000*1e30", "p3:1/0.2/0*300", "linear:3e38/0/0", "srgb:1/1/1*1e38",
                              "#00000000*1e40" } ) {
    require( Vector::color_paints( color ), std::string( "a color: " ) + color );
  }
  /* A black is 0 however bright, until the brightness itself overflows. */
  require( !Vector::color_paints( "#00000000*1e41" ), "0 times infinity is not a number" );

  /* x g=0 deletes everything under the root, which keeps its own state. */
  Vector::Table table;
  std::vector<Vector::Record> records;
  std::string error;
  require( Vector::parse_records( "g id=0 t=2,0,0,2,0,0\np id=1 d=M0,0\ng id=2\np id=3 g=2 d=M0,0\nk id=4 pts=1,2,3", records,
                                  error ),
           error );
  table.apply( records );
  require( Vector::parse_records( "x g=0", records, error ) && table.apply( records ).empty(), "x g=0 applies" );
  require( table.size() == 0 && table.text() == "g id=0 t=2,0,0,2,0,0\n", "and leaves only the root: " + table.text() );

  /* However deep, and whatever vector_depth_limit says. */
  const unsigned saved = vector_depth_limit;
  vector_depth_limit = 2;
  std::string chain = "g id=1\n";
  for ( int i = 2; i <= 6; i++ ) {
    chain += "g id=" + std::to_string( i ) + " p=" + std::to_string( i - 1 ) + "\n";
  }
  chain += "p id=7 g=6 d=M0,0\np id=8 d=M0,0\n";
  Vector::Table deep;
  require( Vector::parse_records( chain, records, error ), error );
  deep.apply( records );
  require( Vector::parse_records( "x g=1", records, error ) && deep.apply( records ).empty(), "x g=1 applies" );
  vector_depth_limit = saved;
  require( deep.size() == 1 && deep.text() == "p id=8 d=M0,0\n", "a deep group goes whole: " + deep.text() );
}

void vector_layouts()
{
  Complete server( 80, 24 );
  server.set_client_extensions( silkie_now() );
  const auto answer = [&]( const std::string& json, const std::string& rest = std::string() ) {
    return server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + base64( json ) + rest ) );
  };
  const std::string invalid = "EINVAL:Line 1: Invalid vector text, font or layout";
  /* What Silkie's layout refuses whatever the font. */
  for ( const std::string& json : std::vector<std::string> {
          std::string( "{\"text\":\"x\",\"size\":0}" ), "{\"text\":\"x\",\"size\":-1}", "{\"text\":\"x\",\"size\":\"9\"}",
          "{\"text\":\"x\",\"size\":true}", "{\"text\":\"x\",\"size\":NaN}", "{\"text\":\"x\",\"size\":1e999}",
          "{\"text\":\"x\",\"weight\":950}", "{\"text\":\"x\",\"weight\":99}", "{\"text\":\"x\",\"slant\":16}",
          "{\"text\":\"x\",\"italic\":1}", "{\"text\":\"x\",\"strike\":\"yes\"}", "{\"text\":\"x\",\"overline\":null}",
          "{\"text\":\"x\",\"features\":\"liga\"}", "{\"text\":\"x\",\"features\":[1]}", "{\"text\":\"x\",\"features\":[\"liga=\"]}",
          "{\"text\":\"x\",\"features\":[\"ligatures\"]}", "{\"text\":\"x\",\"direction\":\"up\"}", "{\"text\":\"x\",\"direction\":1}",
          "{\"text\":\"x\",\"underline\":\"wavy\"}", "{\"text\":\"x\",\"axes\":{\"wghtx\":1}}", "{\"text\":\"x\",\"axes\":[1]}",
          "{\"text\":\"x\",\"axes\":{\"wght\":\"700\"}}", "{\"text\":\"x\",\"fill\":\"#zz\"}", "{\"text\":\"x\",\"fill\":null}",
          "{\"text\":\"x\",\"stroke\":\"red\"}", "{\"text\":\"x\",\"fill\":\"#000000*1e41\"}", "{\"text\":\"x\",\"stroke_width\":-1}",
          "{\"text\":\"x\",\"background\":\"#zz\"}", "{\"text\":\"x\",\"background\":1}", "{\"text\":\"x\",\"strike\":true,\"underline_color\":\"x\"}",
          "{\"text\":\"x\",\"x\":NaN}", "{\"text\":\"x\",\"angle\":\"0\"}", "{\"text\":1}", "{\"runs\":{}}", "{\"runs\":[{}]}",
          "{\"runs\":[{\"text\":5}]}", "{\"runs\":[1]}", "{\"text\":\"x\",\"font\":5}", "{\"text\":\"x\",\"font\":\"@missing\"}",
          "{\"text\":\"x\",\"role\":[]}", "{\"text\":\"x\",\"language\":\"e\\u0000n\"}", "{\"text\":\"x\",\"script\":7}",
          "{\"text\":\"x\",\"fallback\":\"Iosevka\"}", "{\"text\":\"x\",\"fallback\":[1]}", "{\"text\":\"x\",\"y\":1e400}",
          "{\"a\":" + std::string( 4301, '1' ) + "}", "\xef\xbb\xbf\xef\xbb\xbf{}", "{\"text\":\"x\"} \x0c" } ) {
    require( answer( json ).find( invalid ) != std::string::npos, "refused: " + json );
  }
  /* And what it takes. */
  for ( const std::string& json : std::vector<std::string> {
          std::string( "{}" ), "{\"text\":\"x\",\"slant\":null,\"weight\":100,\"size\":1e-300}", "{\"runs\":[]}",
          "{\"runs\":[],\"size\":0,\"font\":5}", "{\"text\":\"\\n\",\"fallback\":5}", "{\"text\":\"x\",\"background\":0}",
          "{\"text\":\"x\",\"background\":\"\",\"x\":1E5,\"y\":-0,\"angle\":-1e-5}", "{\"text\":\"x\",\"underline\":\"curly\",\"underline_color\":\"bad\"}",
          "{\"text\":\"x\",\"features\":[\"-liga\",\"kern=0\",\"ss01[3:5]=on\",\"'aalt' 2\"],\"direction\":\"\"}",
          "{\"text\":\"x\",\"axes\":{\"wght\":\"no\",\"wght\":700}}", "{\"text\":\"x\",\"font\":\"Iosevka\",\"role\":[]}",
          "{\"text\":\"x\",\"nan\":NaN,\"inf\":-Infinity,\"big\":" + std::string( 4300, '9' ) + "}",
          "{\"runs\":[{\"text\":\"a\",\"fill\":\"@5\",\"stroke\":\"p3:1/0/0*300\"}],\"strike\":true}",
          "\xef\xbb\xbf{\"text\":\"\\ud800\"}", std::string( "\xff\xfe{\0}\0", 6 ) } ) {
    const std::string reply = answer( json );
    require( reply.find( ";OK" ) != std::string::npos, "taken: " + json + " gave " + reply );
  }
  /* Strict base64, as b64decode(validate=True). */
  for ( const std::string& field : std::vector<std::string> { std::string( "e30" ), "e30==", "e3=0", "=e30", "e30=e30=" } ) {
    require( server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + field ) ).find( invalid ) != std::string::npos,
             "not strict base64: " + field );
  }
  require( server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=e30=" ) ).find( ";OK" ) != std::string::npos, "e30= is {}" );
  /* The layout is read once the record is: a bad field before it says so. */
  require( server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=!!!! t=1" ) ).find( "EINVAL:Line 1: Bad transform" )
             != std::string::npos,
           "a field's error comes first" );
}

/* The program's side of an upload: the request, the terminal's answer. */
struct FontSession
{
  Complete server { 80, 24 };
  std::string to_program;

  FontSession() { server.set_client_extensions( silkie_now() ); }
  uint32_t request( const std::string& json, const std::string& controls = "a=v,f=200,i=5" )
  {
    to_program = server.act( apc( controls, json ) );
    const auto outputs = server.take_extension_outputs();
    require( outputs.size() == 1, "the request goes to the terminal: " + json );
    return relay_id_of( outputs[0] );
  }
  void answer( uint32_t relay, const std::string& body )
  {
    ExtensionInput input;
    input.service_reply = service_reply( relay, body );
    to_program = server.extension_input( input );
  }
};

void vector_fonts()
{
  const std::string font_data = base64( std::string( 3000, 'F' ) );
  const std::string upload = "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"" + font_data + "\"}";
  const std::string info = "{\"id\":\"body\",\"upem\":1000,\"glyphs\":3,\"axes\":[{\"tag\":\"wght\",\"minimum\":100.0,"
                           "\"default\":400.0,\"maximum\":900.0}],\"weight\":400,\"slant\":0.0}";
  FontSession s;
  const uint32_t relay = s.request( upload );
  require( s.to_program.empty(), "the terminal answers an upload" );
  const auto& fonts = s.server.get_fb().get_vector_fonts();
  require( fonts.count( "body" ) && *fonts.at( "body" ).upload == upload && fonts.at( "body" ).request == relay,
           "an upload on its way is kept at once" );
  /* Text may name it already, as the terminal will have it by then. */
  require( s.server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + base64( "{\"font\":\"@body\",\"axes\":{\"wght\":2000}}" ) ) )
             .find( ";OK" ) != std::string::npos,
           "its axes are the terminal's to check until it answers" );
  s.answer( relay, "OK;" + base64( info ) );
  require( s.to_program == "\033_Gi=5;OK;" + base64( info ) + "\033\\", "the answer reaches the program" );
  const auto layout = [&]( const std::string& json ) {
    return s.server.act( apc( "a=q,f=200,i=8,s=1,v=1", "t id=1 text=" + base64( json ) ) );
  };
  require( layout( "{\"font\":\"@body\",\"axes\":{\"wght\":700}}" ).find( ";OK" ) != std::string::npos, "an axis in range" );
  require( layout( "{\"font\":\"@body\",\"axes\":{\"wght\":901}}" ).find( "Invalid vector text" ) != std::string::npos,
           "an axis out of the font's range" );
  require( layout( "{\"font\":\"@body\",\"axes\":{\"wdth\":100}}" ).find( "Invalid vector text" ) != std::string::npos,
           "an axis the font does not have" );
  require( layout( "{\"font\":\"@other\"}" ).find( "Invalid vector text" ) != std::string::npos, "a font never uploaded" );

  /* A scene with text in the font, then the font released. */
  const std::string text = "t id=2 text=" + base64( "{\"font\":\"@body\",\"text\":\"Hi\"}" );
  require( s.server.act( apc( "a=T,f=200,i=9,s=100,v=100", "p id=1 d=M0,0L1,1\n" + text ) ).find( ";OK" ) != std::string::npos,
           "text in an uploaded font" );
  Complete client( 80, 24 );
  client.apply_string( s.server.diff_from( client ) );
  require( client.get_fb().get_vector_fonts() == s.server.get_fb().get_vector_fonts()
             && *client.get_fb().get_vector_fonts().at( "body" ).upload == upload,
           "the fonts cross the link" );
  const Complete before_release = client;

  s.to_program = s.server.act( apc( "a=v,f=200,i=6", "{\"op\":\"release\",\"id\":\"body\"}" ) );
  require( s.server.take_extension_outputs().empty(), "a release is the relay's to answer" );
  require( s.to_program == "\033_Gi=6;OK;" + base64( "{\"id\":\"body\"}" ) + "\033\\", "as Silkie does: " + s.to_program );
  require( fonts.count( "body" ) && fonts.at( "body" ).released, "kept while text names it" );
  require( layout( "{\"font\":\"@body\"}" ).find( "Invalid vector text" ) != std::string::npos, "but no new text may" );
  s.to_program = s.server.act( apc( "a=v,f=200,i=7", "{\"op\":\"release\",\"id\":\"body\"}" ) );
  auto outputs = s.server.take_extension_outputs();
  require( s.to_program.empty() && outputs.size() == 1, "a second release is the terminal's to refuse" );
  s.answer( relay_id_of( outputs[0] ), "EINVAL:" + base64( "{\"error\": \"'body'\"}" ) );
  require( s.to_program == "\033_Gi=7;EINVAL:" + base64( "{\"error\": \"'body'\"}" ) + "\033\\", "and it does" );

  const std::string released_diff = s.server.diff_from( before_release );
  require( released_diff.find( font_data ) == std::string::npos, "a release does not send the font again" );
  Complete replica = before_release;
  replica.apply_string( released_diff );
  require( replica.get_fb().get_vector_fonts().at( "body" ).released
             && *replica.get_fb().get_vector_fonts().at( "body" ).upload == upload,
           "the release crosses the link" );

  /* A client that had it releases it after what it draws. */
  std::string out;
  append_kitty_frame( out, true, before_release.get_fb(), replica.get_fb() );
  const std::string release = base64( "{\"op\":\"release\",\"id\":\"body\"}" );
  require( out.find( "a=v,f=200,i=7254,q=2" ) != std::string::npos && out.find( release ) != std::string::npos
             && out.find( base64( upload ).substr( 0, 64 ) ) == std::string::npos,
           "a client that has the font releases it, quietly: " + out );

  /* A new client, or a repaint, is given the font before the scene that
     names it, and releases it after. */
  Complete fresh( 80, 24 );
  fresh.apply_string( s.server.diff_from( fresh ) );
  out.clear();
  append_kitty_frame( out, false, Complete( 80, 24 ).get_fb(), fresh.get_fb() );
  const size_t up = at( out, "a=v,f=200,i=7254,q=2,m=" ), scene = at( out, "a=t,q=2,f=200" ), down = at( out, release );
  require( up != std::string::npos && scene != std::string::npos && down != std::string::npos && up < scene && scene < down,
           "upload, scene, release: " + out.substr( 0, 200 ) );
  std::string rebuilt;
  for ( size_t from = up - 3; from + 3 < scene; ) {
    const size_t end = out.find( "\033\\", from );
    KittyCommand command;
    require( parse_kitty_command( out.substr( from + 2, end - from - 2 ), command ), "the upload parses: " + out.substr( from, 120 ) );
    rebuilt += command.payload;
    from = end + 2;
  }
  require( rebuilt == upload, "the upload as the program made it" );

  /* A client far behind is sent the scene whole: the same. */
  Complete behind( 80, 24 );
  behind.apply_string( s.server.diff_from( behind ) );
  Framebuffer stale = behind.get_fb();
  stale.erase_kitty_image( 9 );
  out.clear();
  append_kitty_frame( out, true, stale, behind.get_fb() );
  require( at( out, "a=v,f=200,i=7254,q=2" ) < at( out, "a=t,q=2,f=200" ) && at( out, release ) > at( out, "a=t,q=2,f=200" )
             && at( out, release ) != std::string::npos,
           "a scene sent again has its fonts: " + out.substr( 0, 200 ) );

  /* Once nothing names it, it is forgotten. */
  s.server.act( apc( "a=d,d=I,i=9" ) );
  require( !fonts.count( "body" ), "a released font no text names is forgotten" );
  const std::string forget = s.server.diff_from( replica );
  replica.apply_string( forget );
  require( replica.get_fb().get_vector_fonts().empty(), "on both sides" );
}

void vector_font_edges()
{
  const std::string upload = "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"AAAA\"}";
  /* A refused upload is not kept; the font before it is kept again. */
  {
    FontSession s;
    const uint32_t first = s.request( upload );
    s.answer( first, "OK;" + base64( "{\"id\":\"body\"}" ) );
    const Vector::SessionFont kept = s.server.get_fb().get_vector_fonts().at( "body" );
    const uint32_t second = s.request( "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"BBBB\"}" );
    const uint32_t third = s.request( "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"CCCC\"}" );
    s.answer( second, "EINVAL:" + base64( "{\"error\": \"Font has no glyphs\"}" ) );
    require( s.server.get_fb().get_vector_fonts().at( "body" ).request == third, "a later upload stands" );
    s.answer( third, "EINVAL:" + base64( "{\"error\": \"Font has no glyphs\"}" ) );
    require( s.server.get_fb().get_vector_fonts().at( "body" ) == kept, "both refused: the first is kept again" );
    const uint32_t none = s.request( "{\"op\":\"upload\",\"id\":\"other\",\"data\":\"!\"}" );
    s.answer( none, "EINVAL:" + base64( "{\"error\": \"x\"}" ) );
    require( !s.server.get_fb().get_vector_fonts().count( "other" ), "a refused upload of a new name is not kept" );
    /* A release while an upload of the name is on its way is the terminal's. */
    s.request( upload );
    s.request( "{\"op\":\"release\",\"id\":\"body\"}" );
  }

  /* A patch that names a font, then its release, in one frame: the release
     comes after the patch. */
  {
    FontSession s;
    s.answer( s.request( upload ), "OK;" + base64( "{\"id\":\"body\"}" ) );
    s.server.act( apc( "a=T,f=200,i=9,s=100,v=100", "p id=1 d=M0,0L1,1" ) );
    Complete client( 80, 24 );
    client.apply_string( s.server.diff_from( client ) );
    const Framebuffer shown = client.get_fb();
    require( s.server.act( apc( "a=e,i=9", "t id=2 text=" + base64( "{\"font\":\"@body\",\"text\":\"x\"}" ) ) ).find( ";OK" )
               != std::string::npos,
             "a patch with text" );
    s.server.act( apc( "a=v,f=200,i=6", "{\"op\":\"release\",\"id\":\"body\"}" ) );
    client.apply_string( s.server.diff_from( client ) );
    std::string out;
    append_kitty_frame( out, true, shown, client.get_fb() );
    const size_t patch = at( out, "a=e,q=2" ), release = at( out, base64( "{\"op\":\"release\",\"id\":\"body\"}" ) );
    require( patch != std::string::npos && release != std::string::npos && patch < release
               && out.find( base64( "{\"op\":\"upload\"" ).substr( 0, 16 ) ) == std::string::npos,
             "patch, then release, and no upload: " + out );
  }

  /* The program's own upload reaches the terminal as it came: drawing the
     state that keeps it does not upload it again. */
  {
    FontSession s;
    Complete client( 80, 24 );
    const Framebuffer empty = client.get_fb();
    s.answer( s.request( upload ), "OK;" + base64( "{\"id\":\"body\"}" ) );
    client.apply_string( s.server.diff_from( client ) );
    std::string out;
    append_kitty_frame( out, true, empty, client.get_fb() );
    require( out.empty(), "nothing to write: " + out );
    append_kitty_frame( out, false, empty, client.get_fb() );
    require( out.find( "a=v,f=200,i=7254,q=2" ) != std::string::npos, "but a repaint uploads it" );
  }

  /* Text that names a font nobody can give the terminal is left out of a
     scene sent whole, rather than cost the whole scene. */
  {
    Framebuffer fb( 80, 24 );
    KittyImage image;
    image.id = 3;
    image.format = KITTY_FORMAT_VECTOR;
    image.width = image.height = 10;
    image.data = std::make_shared<std::string>();
    auto table = std::make_shared<Vector::Table>();
    std::vector<Vector::Record> records;
    std::string error;
    require( Vector::parse_records( "p id=1 d=M0,0\nt id=2 text=" + base64( "{\"font\":\"@gone\"}" ), records, error ), error );
    table->apply( records );
    image.vector = Vector::make_doc( 1, 0, 10, 10, table );
    fb.put_kitty_image( image );
    std::string out;
    append_kitty_frame( out, false, Framebuffer( 80, 24 ), fb );
    require( out.find( base64( "p id=1 d=M0,0\n" ) ) != std::string::npos, "the rest of the scene is sent" );
  }
}

void vector_service_answers()
{
  /* A client that goes away owes no answer any more: the program gets one. */
  FontSession s;
  const uint32_t pending = s.request( "{\"op\":\"fonts\"}" );
  const uint32_t quiet = s.request( "{\"op\":\"fonts\"}", "a=v,f=200,i=6,q=2" );
  const uint32_t upload = s.request( "{\"op\":\"upload\",\"id\":\"body\",\"data\":\"AAAA\"}", "a=v,f=200,i=7,q=1" );
  require( s.server.extension_service_overdue( 0 ) && !s.server.extension_service_overdue( 3600000 ),
           "a request waits as long as it has" );
  const std::string gone = s.server.extension_detach();
  const std::string error = "EINVAL:" + base64( "{\"error\": \"The terminal went away before it answered\"}" );
  require( gone == "\033_Gi=5;" + error + "\033\\" + "\033_Gi=7;" + error + "\033\\", "each answered, as q says: " + gone );
  require( !s.server.get_fb().get_vector_fonts().count( "body" ), "an upload not answered is not kept" );
  require( !s.server.extension_service_overdue( 0 ), "nothing is owed any more" );
  s.answer( pending, "OK;e30=" );
  require( s.to_program.empty(), "a late answer is dropped" );
  (void)quiet;
  (void)upload;

  /* Silkie's words for the edits and scenes it refuses. */
  Complete server( 80, 24 );
  server.set_client_extensions( silkie_now() );
  require( server.act( apc( "a=e,i=8", "x all" ) )
             == "\033_Gi=8;ENOENT:Vector edit command refers to non-existent image with id: 8 and number: 0\033\\",
           "no such image" );
  require( server.act( apc( "a=e", "x all" ) ).empty(), "no image named: only logged" );
  server.act( apc( "a=t,f=32,i=4,s=1,v=1", std::string( 4, '\xff' ) ) );
  require( server.act( apc( "a=e,i=4", "x all" ) ) == "\033_Gi=4;EINVAL:Not a vector image\033\\", "a raster image" );
  require( server.act( apc( "a=t,f=200,i=5,v=1", "x all" ) ) == "\033_Gi=5;EINVAL:Zero width/height not allowed\033\\",
           "no size" );
  require( server.act( apc( "a=t,f=200,i=5,s=10001,v=1", "x all" ) )
             == "\033_Gi=5;OK\033\\",
           "large vector dimensions are accepted" );
  require( server.act( apc( "a=t,f=200,i=5,s=1,v=1", "p id=1 d=M0,0 fill=#ff00" ) ) == "\033_Gi=5;EINVAL:Line 1: Bad color\033\\",
           "a malformed scene" );
  server.act( apc( "a=t,f=200,i=5,s=1,v=1", "p id=1 d=M0,0" ) );
  require( server.act( apc( "a=e,i=5", "p id=1 d=M0,0\np id=2 d=Q" ) ) == "\033_Gi=5;EINVAL:Line 2: Bad curveto in path\033\\",
           "a malformed patch" );
  require( server.act( apc( "a=v,f=100,i=77", "{}" ) )
             == "\033_Gi=77;EINVAL:Vector service requires a=v,f=200 and a request id\033\\",
           "a malformed request" );
}

/* What Silkie does that the relay was found not to: a soft reset, SGR 221
   and 222, the listing for a terminal without one, pads described again, the
   labels a reset forgets, the text and device of a label, predicted samples
   and the clock, and ink turned off during a stroke. */
void silkie_parity()
{
  /* DECSTR resets the extensions as RIS does. */
  {
    Complete server( 80, 24 );
    server.set_client_extensions( silkie() );
    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=5" ) + osc( "scroll", "v=1:op=mode:mode=1" )
                + osc( "latency", "v=1:op=mode:mode=1" ) + osc( "ink", "v=1:op=brush:i=7:k=1:n=5:w1=4" )
                + "\033]22;>pointer\033\\" + "\033[31m" + osc( "font", "v=1:op=select:role=serif" ) );
    server.extension_input( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=1:contact=0:x=0:y=0:"
                                            "buttons=1:mods=0:tool=1:valid=0" ) );
    server.act( osc( "tablet-pointer", "v=1:op=label:device=5:button=2:text=" + base64( "Undo" ) ) );
    server.take_extension_outputs();
    auto cancelled = frames( server.act( "\033[!p" ) );
    require( cancelled.size() == 1 && field( cancelled[0], "phase" ) == "4", "a soft reset cancels the contacts" );
    const ExtensionModes& ext = server.get_fb().ds.ext;
    require( !ext.tablet && !ext.scroll && !ext.latency && !ext.ink && !ext.brush.present() && ext.pad_labels.empty()
               && ext.pointer_shape().empty(),
             "and turns the modes off, forgets the brush, the labels and the pointer shapes" );
    require( !server.get_fb().ds.get_renditions().get_text_style(), "and the font selection, with the other attributes" );
    auto outputs = server.take_extension_outputs();
    require( outputs.size() == 1 && outputs[0] == osc( "haptic", "v=1:op=stop" ), "and stops continuous feedback" );
  }

  /* SGR 221 and 222 reset bold and dim apart. */
  {
    Complete server( 80, 24 );
    server.act( "\033[1;2mA\033[221mB\033[222mC\033[1;2;22mD" );
    const auto cell = [&]( int x ) { return server.get_fb().get_cell( 0, x )->get_renditions(); };
    require( cell( 0 ).get_attribute( Renditions::bold ) && cell( 0 ).get_attribute( Renditions::faint ), "bold and dim" );
    require( !cell( 1 ).get_attribute( Renditions::bold ) && cell( 1 ).get_attribute( Renditions::faint ), "SGR 221" );
    require( !cell( 2 ).get_attribute( Renditions::bold ) && !cell( 2 ).get_attribute( Renditions::faint ), "SGR 222" );
    require( !cell( 3 ).get_attribute( Renditions::bold ) && !cell( 3 ).get_attribute( Renditions::faint ), "SGR 22" );
  }

  /* The listing is the relay's own: a terminal without one still gets it,
     with what reaches that terminal. A reset needs no terminal at all. */
  {
    Complete server( 80, 24 );
    server.act( osc( "latency", "v=1:op=mode:mode=1" ) );
    require( server.get_fb().ds.ext.latency, "latency is the relay's whatever the terminal" );
    server.act( osc( "caps", "v=1:op=reset" ) );
    require( !server.get_fb().ds.ext.latency, "a reset before the terminal is known" );
    require( server.act( osc( "caps", "v=1:op=query:id=1" ) ).empty(), "no listing before the terminal is known" );
    server.set_client_extensions( ClientExtensions() );
    auto reply = frames( server.act( osc( "caps", "v=1:op=query:id=1" ) ) );
    require( reply.size() == 1 && field( reply[0], "ext" ) == "caps.1,ssh-agent.1,latency.1" && field( reply[0], "format" ) == "301,302,304,308,516,24,32,100",
             "the listing for a terminal without one: " + ( reply.empty() ? "nothing" : field( reply[0], "ext" ) ) );
  }

  /* Pads, like tools, are described again to a program that sets the mode it
     has; a reset forgets the labels; a label's text is UTF-8 and its device
     any 64 bit number. */
  {
    Complete server( 80, 24 );
    server.set_client_extensions( silkie() );
    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) );
    const std::string pad = "777;tablet-pointer;v=1:op=device:device=5:kind=3:buttons=9:rings=1:strips=0:dials=0:groups=1:"
                            "modes=4:vid=1386:pid=222:name=QQ==";
    auto described = frames( server.extension_input( ExtensionInput( pad ) ) );
    require( described.size() == 1 && field( described[0], "vid" ) == "<absent>", "a pad is described without its ids" );
    described = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) ) );
    require( described.size() == 1 && field( described[0], "kind" ) == "3" && field( described[0], "groups" ) == "1"
               && field( described[0], "pid" ) == "<absent>",
             "and again when the program sets the mode it has" );
    require( frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" ) ) ).empty(),
             "not to a program that does not want pads" );
    ClientExtensions ids = silkie();
    ids.grants[Selector::TABLET] = 127; /* the user allows the hardware ids */
    server.set_client_extensions( ids );
    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=12" ) );
    described = frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=12" ) ) );
    require( described.size() == 1 && field( described[0], "vid" ) == "1386", "with the ids a program asked for" );
    server.act( osc( "tablet-pointer", "v=1:op=label:device=18446744073709551615:button=1:text=" + base64( "Big" ) )
                + osc( "tablet-pointer", "v=1:op=label:device=5:ring=0:text=" + base64( "\xff" ) )
                + osc( "tablet-pointer", "v=1:op=label:device=5:strip=0:text=" + base64( "\xc0\x80" ) )
                + osc( "tablet-pointer", "v=1:op=label:device=5:dial=0:text=" + base64( "\xc3\xa9" ) ) );
    const auto& labels = server.get_fb().ds.ext.pad_labels;
    require( labels.size() == 2 && labels.count( PadControl( UINT64_MAX, 0, 1 ) ) && labels.count( PadControl( 5, 3, 0 ) ),
             "a device of any size, text that is UTF-8 and not what is not" );
    server.act( osc( "caps", "v=1:op=reset" ) );
    require( labels.empty(), "a reset forgets the labels" );
    server.extension_detach();
    server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) );
    require( frames( server.act( osc( "tablet-pointer", "v=1:op=mode:mode=4" ) ) ).empty(),
             "a client that went took its pads with it" );
  }

  /* A predicted sample says nothing of the terminal's clock. */
  {
    ExtensionClient client;
    client.info = silkie();
    Framebuffer on( 80, 24 );
    on.ds.ext.tablet = 127;
    ClientGeometry geometry( 80, 24, 800, 480, 10, 20 );
    const std::string pen = "v=1:op=event:kind=1:device=3:contact=0:x=0:y=0:buttons=1:mods=0:tool=1:valid=0";
    client.frame( osc( "tablet-pointer", pen + ":phase=1:t=5000" ), on, geometry, 1000 );
    auto predicted = client.frame( osc( "tablet-pointer", pen + ":phase=2:t=9000:pred=1" ), on, geometry, 1001 );
    require( predicted.forward && client.clock( 1500 ) == 5500, "a predicted sample is passed on and leaves the clock alone" );
    Motion::PointerCollector collector;
    std::vector<Motion::PointerCollector::Item> ready;
    collector.collect( predicted.input, 1001, 50, false, ready );
    require( ready.size() == 1 && !collector.holding(), "and is never held back" );
  }

  /* Ink turned off during a stroke: what was drawn stays, nothing is added. */
  {
    Pair pair;
    pair.server.act( kitty( "a=t,f=200,i=7,s=400,v=300", "" ) );
    pair.server.act( "\033[3;4H" + kitty( "a=p,i=7", "" ), &pair.geometry );
    pair.server.act( osc( "tablet-pointer", "v=1:op=mode:mode=1" )
                     + osc( "ink", "v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5" ) );
    pair.draw();
    auto result = pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:t=1:contact=0:x=25600:y=51200:"
                               "buttons=1:mods=0:tool=1:valid=0:ink=1000" );
    require( result.input.ink_image == 7, "a stroke begins" );
    pair.server.act( osc( "ink", "v=1:op=mode:mode=0" ) );
    pair.draw();
    result = pair.report( "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:t=2:contact=0:x=26880:y=51200:"
                          "buttons=1:mods=0:tool=1:valid=0" );
    require( result.forward && !result.input.ink_image && result.input.ink_records.empty() && result.to_terminal.empty(),
             "the terminal stopped drawing, and so does the relay" );
    require( kitty_vector_records( *pair.server.get_fb().find_kitty_image( 7 ) ).find( "pts=70,160,5\n" ) != std::string::npos,
             "what was drawn stays as it was" );
  }
}

/* The motion of a hand on its way out: smoothed for its lag, with its ink
   record rendered again, thinned by the tolerances, and every source of it
   treated alike. */
void hand_motion()
{
  Motion::Settings settings;
  settings.lag_ms = 15;
  const std::string pen = "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:contact=0:buttons=1:mods=0:tool=1:valid=3";
  const auto at = [&]( int64_t t, int64_t x, int64_t y, int64_t p, int64_t tx ) {
    return pen + ":t=" + std::to_string( t ) + ":x=" + std::to_string( x ) + ":y=" + std::to_string( y ) + ":p="
           + std::to_string( p ) + ":tx=" + std::to_string( tx ) + ":ty=0";
  };
  std::vector<ExtensionInput> ready;

  /* Order and lag: a contact's motion waits its lag; anything else goes on at once, behind what is due. */
  {
    Motion::Smoother smoother( settings );
    Motion::Smoother::Report report;
    report.input = ExtensionInput( at( 100, 0, 0, 1000, 0 ) );
    smoother.feed( report, 1000, ready );
    require( ready.empty() && smoother.pending() && smoother.wait_time( 1000 ) == 15, "a move is held for the lag" );
    report.input = ExtensionInput( "777;tablet-pointer;v=1:op=device:device=3:kind=1:tool=1" );
    smoother.feed( report, 1005, ready );
    require( ready.size() == 1 && ready[0].payload.find( "op=device" ) != std::string::npos, "a description does not wait" );
    smoother.release( 1014, ready );
    require( ready.size() == 1, "not before the lag" );
    smoother.release( 1015, ready );
    require( ready.size() == 2 && ready[1].payload.find( ":x=0:" ) != std::string::npos && !smoother.pending(), "then it goes" );
    report.input = ExtensionInput( at( 200, 0, 0, 1000, 0 ) );
    smoother.feed( report, 2000, ready );
    smoother.flush( ready );
    require( ready.size() == 3 && !smoother.pending(), "a flush lets everything go" );
  }

  /* Smoothing: a noisy straight stroke comes out closer to the line, times
     and everything but positions, pressure and tilt untouched. */
  {
    Motion::Smoother smoother( settings );
    std::vector<ExtensionInput> out;
    double in_error = 0, out_error = 0;
    const int64_t noise[] = { 13, -11, 7, -15, 9, -6, 14, -12, 3, -9, 11, -4, 8, -13, 5, -7, 12, -10, 6, -14 };
    for ( int i = 0; i < 40; i++ ) {
      Motion::Smoother::Report report;
      const int64_t n = noise[i % 20];
      report.input = ExtensionInput( at( 1000 + 5 * i, 300 * i + n, 100 * i - n, 20000 + 100 * i + 10 * n, 500 + n ) );
      in_error += double( n * n ) * 2;
      smoother.feed( report, 10000 + 5 * i, out );
    }
    smoother.flush( out );
    require( out.size() == 40, "every report comes out" );
    for ( int i = 0; i < 40; i++ ) {
      Ext::Frame f;
      require( Ext::parse( out[i].payload, f ) && field( f, "t" ) == std::to_string( 1000 + 5 * i )
                 && field( f, "valid" ) == "3" && field( f, "buttons" ) == "1" && field( f, "ty" ) == "0",
               "the rest of a report is as it was: " + out[i].payload );
      const double dx = std::stod( field( f, "x" ) ) - 300 * i, dy = std::stod( field( f, "y" ) ) - 100 * i;
      out_error += dx * dx + dy * dy;
    }
    require( out_error < in_error / 2, "the noise is taken out: " + std::to_string( out_error ) + " vs " + std::to_string( in_error ) );
  }

  /* An ink record follows the smoothed point. */
  {
    Motion::Smoother smoother( settings );
    InkMap map;
    map.present = true;
    map.record = 'a';
    map.element = 1000;
    map.a = map.d = 1;
    map.w0 = 1;
    map.w1 = 5;
    map.pressure = true;
    std::vector<ExtensionInput> out;
    for ( int i = 0; i < 6; i++ ) {
      Motion::Smoother::Report report;
      report.input = ExtensionInput( at( 1000 + 5 * i, 256 * i + ( i == 2 ? 40 : 0 ), 0, 30000, 0 ) );
      report.input.ink_image = 7;
      report.input.ink_records = map.render( 256 * i + ( i == 2 ? 40 : 0 ), 0, 30000 );
      report.ink = map;
      smoother.feed( report, 10000 + 5 * i, out );
    }
    smoother.flush( out );
    Ext::Frame f;
    require( out.size() == 6 && Ext::parse( out[2].payload, f ), "six points" );
    const int64_t x = std::stoll( field( f, "x" ) ), p = std::stoll( field( f, "p" ) );
    require( x != 552 && out[2].ink_image == 7 && out[2].ink_records == map.render( x, 0, p ),
             "the record is rendered for the point that is sent: " + out[2].ink_records );
    require( map.render( 256, 512, 65535 ) == "a id=1000 pts=1,2,5\n", "a record as ink_point writes one: " + map.render( 256, 512, 65535 ) );
  }

  /* No smoothing: everything goes on as it came, at once. */
  {
    Motion::Settings off;
    off.lag_ms = 0;
    Motion::Smoother smoother( off );
    Motion::Smoother::Report report;
    report.input = ExtensionInput( at( 1, 7, 9, 100, 3 ) );
    std::vector<ExtensionInput> out;
    smoother.feed( report, 1000, out );
    require( out.size() == 1 && out[0].payload == at( 1, 7, 9, 100, 3 ) && smoother.wait_time( 1000 ) == INT_MAX, "smoothing off" );
  }

  /* Gesture updates merge as the framing says: scale multiplied, angle summed. */
  {
    Collected c;
    const auto pinch = [&]( int phase, int scale, int total, int t ) {
      return "777;gesture;v=1:op=event:kind=1:phase=" + std::to_string( phase ) + ":t=" + std::to_string( t )
             + ":x=100:y=200:mods=0:scale=" + std::to_string( scale ) + ":total=" + std::to_string( total ) + ":fingers=2";
    };
    c.report( pinch( 1, 0, 1000, 1 ), 1000 );
    c.report( pinch( 2, 100, 1100, 2 ), 1000 );
    c.report( pinch( 2, 100, 1210, 3 ), 1000 );
    c.report( pinch( 2, -50, 1150, 4 ), 1000 );
    c.report( pinch( 3, 0, 1150, 5 ), 1000 );
    Ext::Frame merged;
    /* The begin, the first update at once, then the two others merged: a tenth
       larger and then a twentieth smaller is 1.1 x 0.95, 45 thousandths. */
    require( c.sent.size() == 4 && Ext::parse( c.sent[2].input.payload, merged ) && field( merged, "scale" ) == "45"
               && field( merged, "total" ) == "1150" && field( merged, "t" ) == "4",
             "updates of a pinch: the product of the changes, the total of the last: " + c.sent[2].input.payload );
    const auto turn = [&]( int angle, int t ) {
      return "777;gesture;v=1:op=event:kind=2:phase=2:t=" + std::to_string( t ) + ":x=0:y=0:mods=0:angle=" + std::to_string( angle )
             + ":total=" + std::to_string( angle * t );
    };
    Collected r;
    r.report( "777;gesture;v=1:op=event:kind=2:phase=1:t=0:x=0:y=0:mods=0:angle=0:total=0", 1000 );
    r.report( turn( 10, 1 ), 1000 );
    r.report( turn( 20, 2 ), 1000 );
    r.report( turn( 30, 3 ), 1000 );
    r.collector.flush( r.sent );
    require( r.sent.size() == 3 && Ext::parse( r.sent[2].input.payload, merged ) && field( merged, "angle" ) == "50",
             "angles add up: " + r.sent[2].input.payload );
  }

  /* Skiff's own pointer protocol is a hand too: its frames are thinned, and
     go on as bytes in the order they came. */
  {
    Motion::PointerCollector c;
    std::vector<Motion::PointerCollector::Item> sent;
    const auto frame = [&]( int phase, int x, int y, int buttons ) {
      return "\033]777;goblin-pointer;v=1:op=event:kind=1:phase=" + std::to_string( phase ) + ":device=0:contact=0:x="
             + std::to_string( x ) + ":y=" + std::to_string( y ) + ":buttons=" + std::to_string( buttons ) + ":mods=0:tool=1:valid=0\033\\";
    };
    c.collect_pointer( frame( 1, 0, 0, 1 ), 1000, 100, false, sent );
    for ( int x = 256; x <= 1280; x += 256 ) {
      c.collect_pointer( frame( 2, x, 0, 1 ), 1000, 100, false, sent );
    }
    c.collect_pointer( frame( 3, 1280, 0, 0 ), 1000, 100, false, sent );
    require( sent.size() == 4 && sent[0].bytes && sent[0].direct && sent[0].raw == frame( 1, 0, 0, 1 )
               && sent[2].raw.find( ":x=1280:" ) != std::string::npos && sent[3].raw == frame( 3, 1280, 0, 0 ),
             "a stroke of the pointer protocol: down, first move, last move, up: " + std::to_string( sent.size() ) );
  }

  /* xterm mouse motion with a button held is a drag, kept within a pixel of
     its path in pixels and within half a cell in cells; with no button it is
     pointing, and only the pace of the hand is kept. */
  {
    Motion::PointerCollector c;
    std::vector<Motion::PointerCollector::Item> sent;
    const int pixels = DrawState::MOUSE_ENCODING_SGR_PIXELS, cells = DrawState::MOUSE_ENCODING_SGR;
    const auto drag = []( int x, int y ) { return "\033[<32;" + std::to_string( x ) + ";" + std::to_string( y ) + "M"; };
    const auto motion = []( int x, int y ) { return "\033[<35;" + std::to_string( x ) + ";" + std::to_string( y ) + "M"; };
    c.collect_motion( drag( 10, 10 ), pixels, 1000, 100, false, sent );
    c.collect_motion( drag( 20, 10 ), pixels, 1000, 100, false, sent );
    c.collect_motion( drag( 30, 10 ), pixels, 1000, 100, false, sent );
    c.collect_motion( drag( 40, 11 ), pixels, 1000, 100, false, sent );
    require( sent.size() == 1 && c.holding(), "a drag on a line is held" );
    c.collect_motion( drag( 50, 40 ), pixels, 1000, 100, false, sent );
    require( sent.size() == 2 && sent[1].raw == drag( 40, 11 ), "a bend keeps the point before it" );
    Motion::PointerCollector pointing;
    std::vector<Motion::PointerCollector::Item> pointed;
    pointing.collect_motion( motion( 10, 10 ), pixels, 1000, 100, false, pointed );
    pointing.collect_motion( motion( 20, 10 ), pixels, 1000, 100, false, pointed );
    pointing.collect_motion( motion( 40, 11 ), pixels, 1000, 100, false, pointed );
    pointing.collect_motion( motion( 50, 40 ), pixels, 1000, 100, false, pointed );
    require( pointed.size() == 1 && pointing.holding(), "pointing is merged whatever its path" );
    Motion::PointerCollector staircase;
    std::vector<Motion::PointerCollector::Item> steps;
    staircase.collect_motion( drag( 1, 1 ), cells, 1000, 100, false, steps );
    staircase.collect_motion( drag( 2, 1 ), cells, 1000, 100, false, steps );
    staircase.collect_motion( drag( 3, 1 ), cells, 1000, 100, false, steps );
    require( steps.size() == 1 && staircase.holding(), "cells of a drag on a line are held" );
    staircase.collect_motion( drag( 3, 2 ), cells, 1000, 100, false, steps );
    require( steps.size() == 2 && steps[1].raw == drag( 3, 1 ), "a cell off the line by more than half a cell is a bend" );
    Motion::PointerCollector in_cells;
    std::vector<Motion::PointerCollector::Item> pointed_cells;
    for ( int x = 1; x <= 5; x++ ) {
      in_cells.collect_motion( motion( x, x * 7 ), cells, 1000, 100, false, pointed_cells );
    }
    require( pointed_cells.size() == 1 && in_cells.holding(), "pointing in cells is merged whatever its path" );
  }

  /* The pace of the hand: a hand in motion is reported at least every keep_ms
     however straight its path, the link's frame rate notwithstanding. Every
     report merged away is within half of keep_ms of one kept. */
  {
    Motion::PointerCollector c;
    std::vector<Motion::PointerCollector::Item> sent;
    const auto motion = []( int x ) { return "\033[<35;" + std::to_string( x ) + ";1M"; };
    for ( int i = 0; i <= 10; i++ ) {
      c.collect_motion( motion( i ), DrawState::MOUSE_ENCODING_SGR, 1000 + 10 * i, 100, false, sent );
    }
    std::vector<std::string> raws;
    for ( const auto& item : sent ) {
      raws.push_back( item.raw );
    }
    /* 1000 goes at once; 1010 is within 12 ms of it, 1020 and 1030 are not,
       so 1030 is kept when 1040 (not within 12 ms of 1020) comes; and so on.
       1100 goes with the link's interval. */
    require( raws == std::vector<std::string> { motion( 0 ), motion( 3 ), motion( 6 ), motion( 9 ), motion( 10 ) },
             "a mouse sampled every 10 ms is reported every 30, a sample over the 25 asked: " + std::to_string( sent.size() ) );
    /* The same for a straight stroke of a pen, which the tolerances alone would thin to its ends. */
    Collected pen;
    const std::string down = "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:contact=0:buttons=1:mods=0:tool=1:valid=0";
    for ( int i = 0; i <= 10; i++ ) {
      pen.report( down + ":t=" + std::to_string( i ) + ":x=" + std::to_string( 256 * i ) + ":y=0", 1000 + 10 * i );
    }
    require( pen.sent.size() == 5 && pen.sent[1].input.payload.find( ":x=768:" ) != std::string::npos,
             "a straight stroke keeps the pace of the hand: " + std::to_string( pen.sent.size() ) );
    /* A pause, then motion again: the first report after it is kept. */
    c.collect_motion( motion( 11 ), DrawState::MOUSE_ENCODING_SGR, 3000, 100, false, sent );
    c.collect_motion( motion( 12 ), DrawState::MOUSE_ENCODING_SGR, 3005, 100, false, sent );
    require( sent.size() == 6 && sent[5].raw == motion( 11 ), "the first position after a pause goes at once" );
    /* A burst at one instant long after the last kept merges to its last: no
       report merged away is further from the one kept than it was. */
    Motion::PointerCollector burst;
    std::vector<Motion::PointerCollector::Item> bursts;
    burst.collect_motion( motion( 0 ), DrawState::MOUSE_ENCODING_SGR, 1000, 100, false, bursts );
    for ( int i = 1; i <= 20; i++ ) {
      burst.collect_motion( motion( i ), DrawState::MOUSE_ENCODING_SGR, 1050, 100, false, bursts );
    }
    require( bursts.size() == 1 && burst.holding(), "a burst at one instant is held as one" );
    burst.collect_motion( motion( 21 ), DrawState::MOUSE_ENCODING_SGR, 1063, 100, false, bursts );
    require( bursts.size() == 2 && bursts[1].raw == motion( 20 ), "and kept once the next is too far from it" );
    /* With an interval of 0, every report is kept, in order and still held for the link. */
    Motion::Settings every;
    every.keep_ms = 0;
    Motion::PointerCollector all( every );
    std::vector<Motion::PointerCollector::Item> kept;
    for ( int i = 0; i < 3; i++ ) {
      all.collect_motion( motion( i ), DrawState::MOUSE_ENCODING_SGR, 1000, 100, false, kept );
    }
    all.flush( kept );
    require( kept.size() == 3 && kept[1].raw == motion( 1 ), "an interval of 0 merges nothing" );
  }

  /* --exact-mouse: every report as it came, at once, in order; nothing smoothed. */
  {
    Motion::Settings exact;
    exact.exact = true;
    exact.lag_ms = 0;
    Motion::PointerCollector c( exact );
    std::vector<Motion::PointerCollector::Item> sent;
    const auto motion = []( int x ) { return "\033[<35;" + std::to_string( x ) + ";1M"; };
    for ( int i = 0; i < 4; i++ ) {
      c.collect_motion( motion( i ), DrawState::MOUSE_ENCODING_SGR, 1000, 100, true, sent );
    }
    c.collect( ExtensionInput( at( 1, 7, 9, 100, 3 ) ), 1000, 100, true, sent );
    c.collect( ExtensionInput( at( 2, 8, 9, 100, 3 ) ), 1000, 100, true, sent );
    require( sent.size() == 6 && !c.holding() && sent[3].raw == motion( 3 ) && sent[5].input.payload == at( 2, 8, 9, 100, 3 )
               && c.wait_time( 1000, true ) == INT_MAX,
             "exact: everything at once, the link's backlog notwithstanding: " + std::to_string( sent.size() ) );
  }

  /* Settings from the environment. */
  {
    setenv( "GOBLIN_SKIFF_HAND_SMOOTHING", "0", 1 );
    setenv( "GOBLIN_SKIFF_HAND_TOLERANCE", "128", 1 );
    setenv( "GOBLIN_SKIFF_HAND_INTERVAL", "40", 1 );
    const Motion::Settings env = Motion::Settings::from_environment();
    require( env.lag_ms == 0 && env.tolerance == 128 && env.pad_tolerance == 128 && env.keep_ms == 40 && !env.exact,
             "settings from the environment" );
    setenv( "GOBLIN_SKIFF_HAND_SMOOTHING", "15", 1 );
    setenv( "GOBLIN_SKIFF_EXACT_MOUSE", "1", 1 );
    const Motion::Settings exact = Motion::Settings::from_environment();
    require( exact.exact && exact.lag_ms == 0 && exact.keep_ms == 0, "--exact-mouse turns smoothing and merging off" );
    unsetenv( "GOBLIN_SKIFF_HAND_SMOOTHING" );
    unsetenv( "GOBLIN_SKIFF_HAND_TOLERANCE" );
    unsetenv( "GOBLIN_SKIFF_HAND_INTERVAL" );
    unsetenv( "GOBLIN_SKIFF_EXACT_MOUSE" );
  }
}


/* The reports of a run come back from a record byte for byte, from a
   prediction both ends make the same way. */
void motion_codec()
{
  using namespace Motion;

  /* Varints */
  {
    std::string s;
    put_unsigned( s, 0 );
    put_unsigned( s, 127 );
    put_unsigned( s, 128 );
    put_unsigned( s, UINT64_MAX );
    put_signed( s, -1 );
    put_signed( s, INT64_MIN );
    put_signed( s, 300 );
    size_t pos = 0;
    uint64_t u = 1;
    int64_t v = 0;
    require( get_unsigned( s, pos, u ) && u == 0 && get_unsigned( s, pos, u ) && u == 127 && get_unsigned( s, pos, u ) && u == 128
               && get_unsigned( s, pos, u ) && u == UINT64_MAX,
             "unsigned varints" );
    require( get_signed( s, pos, v ) && v == -1 && get_signed( s, pos, v ) && v == INT64_MIN && get_signed( s, pos, v ) && v == 300
               && pos == s.size(),
             "signed varints" );
    const std::string bad( 10, char( 0xff ) );
    pos = 0;
    require( !get_unsigned( bad, pos, u ), "a varint past 64 bits is refused" );
    pos = 0;
    require( !get_unsigned( std::string( "\x80" ), pos, u ), "a varint cut short is refused" );
  }

  /* The predictor */
  {
    Series s;
    require( s.predict( 5, 2 ) == 0, "nothing predicts zero" );
    s.push( 0, 0 );
    require( s.predict( 5, 2 ) == 0, "one sample holds" );
    s.push( 1, 1 );
    require( s.predict( 3, 1 ) == 3 && s.predict( 3, 2 ) == 3, "two samples: a constant velocity" );
    s.push( 2, 4 );
    require( s.predict( 3, 2 ) == 9 && s.predict( 3, 1 ) == 7, "three: a constant acceleration, or the velocity alone" );
    s.push( 3, 9 );
    require( s.count == 3 && s.v[0] == 1 && s.predict( 5, 2 ) == 25, "the oldest sample drops off" );
    Series r;
    r.push( 0, 0 );
    r.push( 3, 10 );
    require( r.predict( 4, 1 ) == 13, "rounded to the nearest" );
    Series q;
    q.push( 0, 0 );
    q.push( 3, -10 );
    require( q.predict( 4, 1 ) == -13, "and away from zero" );
    Series big;
    big.push( 0, INT64_MAX - 1 );
    big.push( 1, INT64_MAX );
    require( big.predict( 2, 1 ) == INT64_MAX, "a step that overflows holds the last value" );
    Series back;
    back.push( 5, 1 );
    back.push( 3, 2 );
    require( back.predict( 4, 1 ) == 2, "time that does not advance holds the last value" );
    Series uneven;
    uneven.push( 0, 0 );
    uneven.push( 10, 100 );
    uneven.push( 15, 200 );
    require( uneven.predict( 20, 2 ) == 333, "the acceleration is scaled by the report's own time" );
  }

  /* A run comes back exactly, and much smaller than its text. */
  struct Point
  {
    int64_t t, x, y, p;
    int phase;
  };
  const auto pen = []( const Point& at ) {
    return "777;tablet-pointer;v=1:op=event:kind=1:phase=" + std::to_string( at.phase ) + ":device=3:contact=0:t="
           + std::to_string( at.t ) + ":buttons=1:mods=0:tool=1:valid=3:x=" + std::to_string( at.x ) + ":y="
           + std::to_string( at.y ) + ":p=" + std::to_string( at.p ) + ":tx=600:ty=-300";
  };
  InkMap map;
  map.present = true;
  map.record = 'k';
  map.element = 1000;
  map.group = 7;
  map.fill = "#ff0000";
  map.a = 0.5;
  map.d = 0.5;
  map.e = 10.25;
  map.f = -3;
  map.w0 = 1;
  map.w1 = 5;
  map.exponent = 1;
  map.pressure = true;
  std::vector<ExtensionInput> in;
  size_t text_bytes = 0;
  for ( int i = 0; i < 40; i++ ) {
    const Point at { 1000 + 5 * i, 256 * ( 10 + 3 * i ) + i * i, 256 * ( 20 + 2 * i ) - 3 * i, 20000 + 300 * i,
                     i == 0 ? 1 : i == 39 ? 3 : 2 };
    ExtensionInput r( pen( at ) );
    r.clock = 5000 + 5 * i + ( i % 3 );
    if ( i > 0 && i < 39 ) {
      r.ink_image = 42;
      r.ink = map;
      r.ink.record = i == 1 ? 'k' : 'a';
      r.ink_records = r.ink.render( at.x, at.y, at.p );
    }
    text_bytes += r.payload.size() + r.ink_records.size();
    in.push_back( r );
    if ( i % 10 == 5 ) {
      /* A finger of its own, a scroll, and things a record cannot say. */
      in.push_back( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=2:phase=2:device=4:contact=" + std::to_string( i / 10 )
                                    + ":t=" + std::to_string( 1002 + 5 * i ) + ":x=" + std::to_string( 70000 + 500 * i )
                                    + ":y=9000:major=400:minor=300" ) );
      in.push_back( ExtensionInput( "777;scroll;v=1:op=event:src=2:phase=2:dx=0:dy=" + std::to_string( -12 * i ) ) );
      in.push_back( ExtensionInput( "777;tablet-pointer;v=1:op=event:kind=1:phase=2:x=+5:y=007:p=-0" ) );
      in.push_back( ExtensionInput() );
      in.back().clock = 1;
      ExtensionInput reply( "777;vector;v=1:op=reply" );
      reply.service_reply = "a=v,r=1";
      in.push_back( reply );
      ExtensionInput odd( "777;tablet-pointer;v=1:op=event:x=1:x=2" );
      in.push_back( odd );
      ExtensionInput inked( "777;tablet-pointer;v=1:op=event:kind=1:phase=2:x=+5:y=1:p=2" );
      inked.ink_image = 42;
      inked.ink_records = "a id=1 pts=1,2,3\n";
      in.push_back( inked );
    }
  }
  RecordContext writing;
  RecordEncoder encoder( writing );
  for ( const ExtensionInput& r : in ) {
    encoder.add( r );
  }
  require( encoder.count() == in.size(), "every report is counted" );
  std::vector<ExtensionInput> back;
  RecordContext reading;
  require( decode_records( encoder.bytes(), reading, back ), "a record decodes" );
  require( back.size() == in.size(), "into as many reports" );
  for ( size_t i = 0; i < in.size(); i++ ) {
    if ( !( back[i] == in[i] ) ) {
      std::cerr << "report " << i << " came back as\n  " << back[i].payload << "\n  ink " << back[i].ink_records << "\n  clock "
                << back[i].clock << "\nfor\n  " << in[i].payload << "\n  ink " << in[i].ink_records << "\n  clock " << in[i].clock
                << "\n";
      require( false, "a report round trips byte for byte" );
    }
    if ( in[i].ink.present ) {
      require( back[i].ink == in[i].ink, "the map the ink was rendered with comes along" );
    }
  }
  require( encoder.bytes().size() * 4 < text_bytes, "a record is under a quarter of the text" );
  /* The pen, four fingers, the scroll, and the report whose numbers are not
     plain, which is a shape and a track of its own with its numbers as text. */
  require( reading.shapes.size() == writing.shapes.size() && reading.tracks.size() == writing.tracks.size()
             && reading.tracks.size() == 7 && reading.shapes.size() == 4 && reading.have_clock == writing.have_clock
             && reading.clock == writing.clock,
           "both ends end up with the same context" );
  for ( size_t k = 0; k < reading.tracks.size(); k++ ) {
    require( reading.tracks[k].samples == writing.tracks[k].samples && reading.tracks[k].text == writing.tracks[k].text
               && reading.tracks[k].has_ink == writing.tracks[k].has_ink,
             "track for track" );
    for ( const auto& series : writing.tracks[k].series ) {
      const auto other = reading.tracks[k].series.find( series.first );
      require( other != reading.tracks[k].series.end() && other->second.count == series.second.count
                 && std::equal( other->second.t, other->second.t + 3, series.second.t )
                 && std::equal( other->second.v, other->second.v + 3, series.second.v ),
               "sample for sample" );
    }
  }
  std::cerr << "motion record: " << text_bytes << " bytes of text as " << encoder.bytes().size() << " bytes for " << in.size()
            << " reports\n";

  /* A second run in the same context: no shape, no map, and the first
     report predicted from the last of the run before. */
  {
    RecordEncoder more( writing );
    std::vector<ExtensionInput> next;
    for ( int i = 0; i < 10; i++ ) {
      const Point at { 1200 + 5 * i, 256 * ( 130 + 3 * i ) + ( 40 + i ) * ( 40 + i ), 256 * ( 100 + 2 * i ), 32000 + 300 * i, 2 };
      ExtensionInput r( pen( at ) );
      r.clock = 5200 + 5 * i;
      r.ink_image = 42;
      r.ink = map;
      r.ink.record = 'a';
      r.ink_records = r.ink.render( at.x, at.y, at.p );
      next.push_back( r );
      more.add( r );
    }
    std::vector<ExtensionInput> again;
    require( decode_records( more.bytes(), reading, again ), "the next run decodes in the context the first left" );
    require( again.size() == next.size(), "all of it" );
    for ( size_t i = 0; i < next.size(); i++ ) {
      require( again[i] == next[i] && again[i].ink == next[i].ink, "and byte for byte" );
    }
    require( more.bytes().size() < 10 * 12, "ten inked reports in under 120 bytes" );
    std::cerr << "motion record: ten more inked reports as " << more.bytes().size() << " bytes\n";
    RecordContext fresh;
    std::vector<ExtensionInput> nothing;
    require( !decode_records( more.bytes(), fresh, nothing ), "and not in a fresh one" );
  }

  /* What is not a record */
  {
    std::vector<ExtensionInput> none;
    RecordContext empty;
    require( !decode_records( std::string( "\x03\x00\x00", 3 ), empty, none ), "a report of no shape is refused" );
    require( !decode_records( std::string( "\x07", 1 ), empty, none ), "an unknown record is refused" );
    require( !decode_records( std::string( "\x01\x05", 2 ) + "ab", empty, none ), "a shape cut short is refused" );
    require( decode_records( std::string(), empty, none ) && none.empty(), "nothing is an empty run" );
    std::string cut = encoder.bytes();
    cut.resize( cut.size() / 2 );
    std::vector<ExtensionInput> partial;
    RecordContext half;
    require( !decode_records( cut, half, partial ), "a record cut short is refused" );
  }

  /* Through a user stream: runs between other events, in order, and the
     text when the peer has not asked for records. */
  {
    Network::UserStream a, plain;
    a.set_motion_records( true );
    for ( Network::UserStream* stream : { &a, &plain } ) {
      stream->push_back( Parser::UserByte( 'q' ) );
      for ( int i = 0; i < 5; i++ ) {
        stream->push_back( in[i] );
      }
      stream->push_back( Parser::Resize( 80, 24 ) );
      stream->push_back( in[5] );
      stream->push_back( in[6] );
    }
    const std::string diff = a.diff_from( Network::UserStream() );
    const std::string text_diff = plain.diff_from( Network::UserStream() );
    require( diff.size() < text_diff.size(), "the record is the smaller diff" );
    ClientBuffers::UserMessage message;
    require( message.ParseFromString( diff ), "the diff is a user message" );
    int runs = 0, texts = 0;
    for ( int i = 0; i < message.instruction_size(); i++ ) {
      runs += message.instruction( i ).HasExtension( ClientBuffers::motion_records );
      texts += message.instruction( i ).HasExtension( ClientBuffers::ext_input );
    }
    require( runs == 2 && texts == 0, "one record for each run" );
    Network::UserStream b;
    b.apply_string( diff );
    require( a == b, "the stream round trips through the record" );
    Network::UserStream c;
    c.apply_string( text_diff );
    require( a == c && plain == b, "and reads the same as the text" );
    require( !plain.motion_records_enabled() && a.motion_records_enabled(), "the choice is the stream's" );

    /* The next diff is written from the state the receiver has, whichever
       path its reports came by, and read in it. */
    const Network::UserStream sent = a;
    for ( int i = 7; i < 20; i++ ) {
      a.push_back( in[i] );
      c.push_back( in[i] );
    }
    a.push_back( Parser::UserByte( 'z' ) );
    c.push_back( Parser::UserByte( 'z' ) );
    const std::string next = a.diff_from( sent );
    b.apply_string( next );
    require( a == b, "a second diff carries on from the first" );
    Network::UserStream d = c;
    d.set_motion_records( true );
    const std::string same = d.diff_from( sent );
    require( same == next, "a stream that took the text writes the same record" );
    Network::UserStream fresh;
    fresh.set_motion_records( true );
    for ( int i = 7; i < 20; i++ ) {
      fresh.push_back( in[i] );
    }
    fresh.push_back( Parser::UserByte( 'z' ) );
    require( fresh.diff_from( Network::UserStream() ).size() > next.size() + 40, "what the context saves: a shape, a map, a first point" );
  }
}

int main()
{
  try {
    sgr();
    silkie_parity();
    hand_motion();
    motion_codec();
    framing();
    capabilities();
    reports();
    display_and_haptic();
    sync_and_render();
    vectors();
    ink();
    client_side();
    colors();
    late_capabilities();
    packed_images();
    native_images();
    program_status();
    depth();
    notifications();
    animation_frames();
    relative_placements();
    termcap_and_dim();
    png_colors();
    review_fixes();
    fonts();
    vector_text();
    vector_service();
    vector_colors_and_groups();
    vector_layouts();
    vector_fonts();
    vector_font_edges();
    vector_service_answers();
    viewports();
    pointer_collection();
    tablet_relay();
    relay_modes();
  } catch ( const std::exception& e ) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "Silkie extensions: framing, capabilities, reports, display, haptics, sync, vectors, ink, fonts, vector "
               "text, the vector service, viewports, pointer collection, late capabilities, animation frames, relative "
               "placements, XTGETTCAP, dim, PNG colors, packed images, depth and notifications passed.\n";
  return 0;
}
