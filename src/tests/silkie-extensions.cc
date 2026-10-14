/* Distributed under the GNU GPL, version 3 or later. */
/* Silkie's OSC 777 extensions, as Goblin Skiff relays them. */
#include <clocale>
#include <cmath>
#include <cstring>
#include <png.h>
#include <zlib.h>
#include <iostream>
#include <set>
#include <stdexcept>

#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/terminal/colorspace.h"
#include "src/terminal/extframe.h"
#include "src/terminal/gpusurface.h"
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
  return info;
}

void framing()
{
  Ext::Frame f;
  require( Ext::parse_program( "777;caps;v=1:op=query:id=42", f ) && f.selector == "caps", "a program frame" );
  require( !Ext::parse_program( "777;caps;v=1:op=query:id=42:id=43", f ), "a repeated key is rejected" );
  require( !Ext::parse_program( "777;caps;op=query", f ), "the version is required" );
  require( !Ext::parse_program( "777;caps;v=1:op=query:", f ), "a trailing separator is rejected" );
  require( !Ext::parse_program( "777;caps;v=1:op=query:pad=" + std::string( 120, 'x' ), f ),
           "fields beyond 127 bytes are rejected" );
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
             == "caps.1,display.1,frame.1,gesture.1,haptic.1,ink.1,latency.1,scroll.1,tablet-pointer.1",
           "the listing intersects the client's: " + field( reply[0], "ext" ) );
  require( field( reply[0], "format" ) == "24,32,64,100,200"
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
  require( reply.find( "EINVAL: Line 2: Bad curveto in path" ) != std::string::npos, "malformed scene: " + reply );
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
  require( reply.find( "OK" ) != std::string::npos && sdr && kitty_image_to_rgba( *sdr, rgba )
             && uint8_t( rgba[1] ) == 188,
           "an HDR image sent anyway is shown at SDR: " + reply );
  reply = server.act( kitty( "a=t,i=5,f=24,s=1,v=1,G=1", std::string( "\x64\x96\xc8", 3 ) ) );
  const KittyImage* converted = server.get_fb().find_kitty_image( 5 );
  require( reply.find( "OK" ) != std::string::npos && converted && kitty_image_to_rgba( *converted, rgba )
             && !( uint8_t( rgba[0] ) == 100 && uint8_t( rgba[1] ) == 150 ) && !converted->gamut,
           "a Display P3 image is converted" );
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

int main()
{
  try {
    sgr();
    framing();
    capabilities();
    reports();
    display_and_haptic();
    sync_and_render();
    vectors();
    ink();
    client_side();
    colors();
    review_fixes();
  } catch ( const std::exception& e ) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "Silkie extensions: framing, capabilities, reports, display, haptics, sync, vectors and ink passed.\n";
  return 0;
}
