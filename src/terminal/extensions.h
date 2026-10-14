/*
    Goblin Skiff: Silkie's OSC 777 extensions, relayed
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef EXTENSIONS_HPP
#define EXTENSIONS_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "src/terminal/extframe.h"

/* Skiff resynthesizes the screen, so to a program it is the terminal: it
   answers the queries of the extensions itself, from what the client learnt
   of the terminal it is attached to, and keeps their modes with the rest of
   the terminal state. The client turns the same modes on in its terminal,
   and passes the reports that come back to the server, which hands them to
   the program. The specifications are Silkie's, see
   https://github.com/adamdeprince/silkie/tree/silkie/docs */

namespace Terminal {

class Framebuffer;
struct ClientGeometry;

namespace Selector {
const char* const CAPS = "caps";
const char* const TABLET = "tablet-pointer";
const char* const SCROLL = "scroll";
const char* const GESTURE = "gesture";
const char* const FRAME = "frame";
const char* const LATENCY = "latency";
const char* const DISPLAY = "display";
const char* const HAPTIC = "haptic";
const char* const INK = "ink";
}

/* The mode bits of the tablet pointer protocol */
enum TabletMode : uint32_t
{
  TABLET_TOOLS = 1,
  TABLET_FINGERS = 2,
  TABLET_PADS = 4,
  TABLET_IDS = 8,
  TABLET_ACTIONS = 16,
  TABLET_ESTIMATES = 32,
  TABLET_ACCIDENTAL = 64,
  TABLET_KNOWN = 127
};

struct InkBrush
{
  uint32_t image = 0, group = 0, next = 0, remaining = 0, tools = 58;
  double w0 = 0, w1 = 0, exponent = 1;
  std::string fill = "#000000";

  bool present() const { return image != 0; }
  Ext::Fields fields() const;
  bool operator==( const InkBrush& other ) const
  {
    return image == other.image && group == other.group && next == other.next && remaining == other.remaining
           && tools == other.tools && w0 == other.w0 && w1 == other.w1 && exponent == other.exponent
           && fill == other.fill;
  }
};

/* The per-window state of the extensions. It is terminal state: the client
   mirrors it and turns the same modes on in its own terminal. */
struct ExtensionModes
{
  uint32_t tablet = 0;
  uint32_t gesture = 0;
  bool scroll = false;
  bool frame = false;
  bool latency = false;
  bool display = false; /* answered by the server; the client never sends it on */
  bool ink = false;
  InkBrush brush;
  uint32_t brush_gen = 0;    /* a brush given again is passed on again, however alike */
  uint32_t frame_output = 0; /* counts the server's writes while frame reports are on */
  std::vector<std::string> pointer[2]; /* OSC 22 shape stacks: main and alternate screen */
  bool alternate = false;

  /* The shape in effect: the top of the stack of the screen in use, passing
     over images that do not exist (pass the framebuffer to know which do);
     empty for none. */
  std::string pointer_shape( const Framebuffer* fb = nullptr ) const;
  bool reports_input() const { return tablet || gesture || scroll; }
  bool operator==( const ExtensionModes& other ) const
  {
    return tablet == other.tablet && gesture == other.gesture && scroll == other.scroll && frame == other.frame
           && latency == other.latency && display == other.display && ink == other.ink && brush == other.brush
           && brush_gen == other.brush_gen && frame_output == other.frame_output && pointer[0] == other.pointer[0]
           && pointer[1] == other.pointer[1] && alternate == other.alternate;
  }
  bool operator!=( const ExtensionModes& other ) const { return !( *this == other ); }
};

/* What the client's terminal said of the extensions when the client asked. */
struct ClientExtensions
{
  bool listing = false; /* it answered the capabilities listing */
  std::map<std::string, uint32_t> versions;
  std::map<std::string, Ext::Fields> caps;  /* each selector's cap reply, without id and mode */
  std::map<std::string, uint32_t> grants;   /* mode bits the terminal keeps, where it was asked */
  std::string media, format;                /* the graphics media and formats it listed */
  Ext::Fields display;                      /* the display, as last reported */
  bool image_gamut = false;                 /* it accepts the G key of the graphics protocol */
  std::string pointer_shapes;               /* its answer to the OSC 22 support query */

  bool speaks( const std::string& selector ) const { return versions.count( selector ) != 0; }
  bool has_format( const std::string& format ) const;
  uint32_t grant( const std::string& selector, uint32_t known ) const;
  int64_t cap_int( const std::string& selector, const std::string& key, int64_t fallback ) const;
  bool operator==( const ClientExtensions& other ) const
  {
    return listing == other.listing && versions == other.versions && caps == other.caps && grants == other.grants
           && media == other.media && format == other.format && display == other.display
           && image_gamut == other.image_gamut && pointer_shapes == other.pointer_shapes;
  }
  bool operator!=( const ClientExtensions& other ) const { return !( *this == other ); }
};

/* The id of the client's own queries to its terminal. */
const int64_t PROBE_ID = 7253;

/* A report from the client's terminal on its way to the program. Without a
   report, it is the client saying it is still there while something it
   reported is in progress. */
struct ExtensionInput
{
  std::string payload;    /* the OSC payload, from "777;"; empty for a heartbeat */
  uint32_t ink_image = 0; /* the records the terminal applied as ink, to this image */
  std::string ink_records;
  int64_t clock = -1; /* the terminal's clock in ms, as the client reckons it when it sent this */
  ExtensionInput( const std::string& s_payload = std::string() ) : payload( s_payload ) {}
  bool operator==( const ExtensionInput& other ) const
  {
    return payload == other.payload && ink_image == other.ink_image && ink_records == other.ink_records
           && clock == other.clock;
  }
};

/* A stroke of terminal-side ink, and the point a report adds to it, worked
   out as the ink protocol says from what the terminal shows. */
struct InkStroke
{
  uint32_t image = 0, element = 0, group = 0, placement = 0;
  uint64_t lineage = 0;
  size_t ordinal = 0;
  double w0 = 0, w1 = 0, exponent = 1;
  bool client_draws = false; /* the terminal stopped drawing it; the client writes the ink */
};
/* 1 with the point, 0 when this report adds none, -1 when the stroke is over. */
int ink_point( const InkStroke& stroke,
               const Ext::Fields& report,
               const Framebuffer& fb,
               const ClientGeometry& geometry,
               bool starting,
               InkStroke* found,
               std::string& point );

/* The queries a client sends its terminal when it starts, to fill in a
   ClientExtensions from the replies. Setting a mode and reading it back is
   how a terminal says which bits its user lets programs have; the client
   leaves the display reports on, so it hears of a change of display. */
std::string extension_probe();
/* The modes the probe sets before each read, in order. */
std::vector<std::pair<std::string, uint32_t>> extension_probe_modes();
/* The OSC 22 query of the names the relay answers for. */
std::string pointer_probe();
/* Takes in a capability reply, a display event or an OSC 22 answer. False
   when the frame is none of these. */
bool consume_probe_reply( const std::string& frame, ClientExtensions& info );

/* The frames that bring the client's terminal to the modes of now. A mode
   that is set is read back, and modes_sent says which, in order; brush_sent
   is the brush the terminal was given, if it was. */
std::string render_extension_modes( bool initialized,
                                    const Framebuffer& last,
                                    const Framebuffer& now,
                                    const ClientExtensions& terminal,
                                    uint32_t ink_next_hint,
                                    std::vector<std::pair<std::string, uint32_t>>* modes_sent = nullptr,
                                    InkBrush* brush_sent = nullptr );
/* Turns off, in the client's terminal, everything the client turned on. */
std::string extension_close( const ClientExtensions& terminal );
std::string extension_open( const ClientExtensions& terminal );

/* The client's side: the replies and reports of its terminal. */
class ExtensionClient
{
public:
  ClientExtensions info;

  struct Result
  {
    bool info_changed = false;
    bool forward = false; /* input goes to the server */
    ExtensionInput input;
    std::string to_terminal; /* written back to the terminal at once */
  };
  /* The probe, to write to the terminal once. */
  std::string probe();
  /* A frame from the terminal, against what the terminal shows. */
  Result frame( const std::string& frame, const Framebuffer& shown, const ClientGeometry& geometry, uint64_t now );
  /* After a frame was drawn: what it set and gave, and whether it sent a
     vector image whole. Returns what to write to the terminal after it. */
  std::string drawn( const Framebuffer& before,
                     const Framebuffer& after,
                     const std::vector<std::pair<std::string, uint32_t>>& modes_sent,
                     const InkBrush* brush_sent,
                     bool vector_resent );
  /* While a stroke, a key or a gesture is in progress the client is heard
     from at least this often, so the server can tell it has gone. */
  static const uint64_t HEARTBEAT_MS = 2000;
  bool heartbeat_due( uint64_t now ) const;
  ExtensionInput heartbeat( uint64_t now );
  void sent( uint64_t now ) { last_sent = now; }
  int wait_time( uint64_t now ) const;
  int64_t clock( uint64_t now ) const { return clock_known ? int64_t( now ) + clock_offset : -1; }
  /* The next ink id the terminal will hand out, before a frame of next is drawn. */
  uint32_t ink_hint( const Framebuffer& next );
  bool live() const { return !strokes.empty() || !keys.empty() || !gestures.empty(); }

private:
  std::map<std::string, std::vector<uint32_t>> mode_reads; /* requests whose reads are on their way */
  bool clock_known = false;
  int64_t clock_offset = 0;
  uint64_t last_sent = 0;
  uint32_t marker = 0;
  bool frame_output_pending = false;
  InkBrush brush;        /* the brush the terminal has, and the ids it has left */
  uint32_t brush_gen = 0;
  uint32_t ink_next_id = 0;
  std::set<std::tuple<int64_t, int64_t, int64_t>> strokes; /* kind, device, contact */
  std::set<std::pair<int64_t, int64_t>> keys;               /* device, button */
  std::set<int64_t> gestures;
  std::map<std::pair<int64_t, int64_t>, InkStroke> ink; /* (device, contact) */

  void track( const Ext::Frame& frame );
  void ink_report( Ext::Frame& frame, const Framebuffer& shown, const ClientGeometry& geometry, Result& result );
};

/* The server's side: a program's frames, and the reports the client
   forwards. Everything a program is sent goes to `to_host`. */
class ExtensionRelay
{
public:
  ClientExtensions client;
  bool client_known = false;
  uint64_t relay_input_us = 8000;    /* how long the relay waits for more output */
  uint64_t relay_repaint_us = 100000; /* and between the frames it sends */
  uint64_t relay_rtt_ms = 0;          /* the round trip to the client */
  std::vector<std::string> outputs;   /* frames for the client's terminal */

  /* A frame a program wrote, the payload of its OSC from "777;". */
  void program_frame( const std::string& payload,
                      Framebuffer& fb,
                      const ClientGeometry* geometry,
                      std::string& to_host );
  /* A report the client forwarded from its terminal, or a heartbeat. */
  void input_frame( const ExtensionInput& input, Framebuffer& fb, std::string& to_host );
  void set_client( const ClientExtensions& info, Framebuffer& fb, std::string& to_host );
  /* Every mode off, as the caps reset request and a shell prompt do. */
  void reset_modes( Framebuffer& fb, std::string& to_host );
  /* RIS: the modes off, the brush and the pointer shapes forgotten. */
  void hard_reset( Framebuffer& fb, std::string& to_host );
  /* The client has gone: what it was in the middle of is cancelled. */
  void detach( Framebuffer& fb, std::string& to_host );
  bool live_contacts() const { return !contacts.empty() || !gestures.empty() || !pad_keys.empty(); }
  /* The program wrote something while frame reports are on. */
  void note_output( Framebuffer& fb );
  void set_alternate( Framebuffer& fb, bool alternate );
  /* OSC 22, the payload after "22;". */
  void pointer_shape( const std::string& payload, Framebuffer& fb, std::string& to_host );
  /* The value of an XTGETTCAP name the relay answers, false for others. */
  bool termcap( const std::string& name, std::string& value ) const;
  bool accepts_format( uint32_t format ) const;

private:
  typedef std::tuple<int64_t, int64_t, int64_t> ContactKey; /* kind, device, contact */
  struct Contact
  {
    int64_t tool = 1, mods = 0;
    std::string x = "0", y = "0";
    bool pressure = false;
  };
  struct Gesture
  {
    std::string x = "0", y = "0", mods = "0", total, fingers;
  };
  std::map<ContactKey, Contact> contacts;
  std::set<ContactKey> suppressed;
  std::map<std::pair<int64_t, int64_t>, int64_t> pad_keys; /* (device, button): t of the press */
  std::map<int64_t, Gesture> gestures;
  std::set<int64_t> suppressed_gestures;
  /* (device, contact): the image and element of a stroke the terminal inks */
  std::map<std::pair<int64_t, int64_t>, std::pair<uint32_t, uint32_t>> ink_strokes;
  uint32_t frame_seq = 0;
  Ext::Fields display_sent;
  int64_t clock_t = -1;   /* the terminal's clock, as last heard */
  int64_t clock_at = 0;   /* when, on ours */

  int64_t now_t() const;
  void hear_time( const Ext::Fields& fields );
  void hear_clock( int64_t clock );
  bool clock_from_client = false;
  void reply( const std::string& selector, Ext::Fields fields, std::string& to_host ) const;
  void cancel_tablet( Framebuffer& fb, std::string& to_host );
  void cancel_gestures( std::string& to_host );
  void set_display_mode( bool on, Framebuffer& fb, std::string& to_host );
  Ext::Fields display_fields() const;
  void tablet_program( const std::string& op, const Ext::Fields& f, Framebuffer& fb, const ClientGeometry* geometry,
                       std::string& to_host );
  void ink_program( const std::string& op, const Ext::Fields& f, Framebuffer& fb, std::string& to_host );
  void haptic_program( const std::string& op, const Ext::Fields& f, const std::string& payload,
                       std::string& to_host );
  bool tablet_input( Ext::Fields& f, Framebuffer& fb, const ExtensionInput& input );
  void end_ink( std::pair<int64_t, int64_t> key, Framebuffer& fb );
  void refresh( const std::string& selector );
};

}

#endif
