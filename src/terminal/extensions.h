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
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "src/terminal/extframe.h"
#include "src/terminal/vectorfonts.h"

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
const char* const FONT = "font";
const char* const DEPTH = "depth";
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

/* A control of a pad that a program labelled: device, control (0 button,
   1 ring, 2 strip, 3 dial) and the control's number. */
typedef std::tuple<uint64_t, uint32_t, uint32_t> PadControl;
const char* const PAD_CONTROLS[] = { "button", "ring", "strip", "dial" };

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
  /* What the program says each control of a pad does: base64 of UTF-8, as
     given. Kept while pad reports are on, and given to the terminal after the
     mode, so a terminal that turns pads on again, or that attaches later,
     still has them. */
  std::map<PadControl, std::string> pad_labels;

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
           && pointer[1] == other.pointer[1] && alternate == other.alternate && pad_labels == other.pad_labels;
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
  std::string graphics;                     /* the extensions of the graphics protocol it listed */
  bool vector_service = false;              /* it answers a=v, the vector image service */
  /* Its answer to the query of the notification protocol (OSC 99 with p=?),
     as it wrote it after the semicolon; empty when it gave none. And whether
     it keeps the return route of a notification, Silkie's r key. */
  std::string notifications;
  bool notification_routes = false;

  bool speaks( const std::string& selector ) const { return versions.count( selector ) != 0; }
  bool has_format( const std::string& format ) const;
  bool has_graphics( const std::string& extension ) const;
  uint32_t grant( const std::string& selector, uint32_t known ) const;
  int64_t cap_int( const std::string& selector, const std::string& key, int64_t fallback ) const;
  bool operator==( const ClientExtensions& other ) const
  {
    return listing == other.listing && versions == other.versions && caps == other.caps && grants == other.grants
           && media == other.media && format == other.format && display == other.display
           && image_gamut == other.image_gamut && pointer_shapes == other.pointer_shapes
           && graphics == other.graphics && vector_service == other.vector_service
           && notifications == other.notifications && notification_routes == other.notification_routes;
  }
  bool operator!=( const ClientExtensions& other ) const { return !( *this == other ); }
};

/* The id of the client's own queries to its terminal. */
const int64_t PROBE_ID = 7253;

/* How a report's position and pressure become the record the terminal applied:
   the affine map from the report's (x, y), in pixels, to the coordinates of the
   stroke's group, and the brush. With it a record can be rendered again for a
   point the client has since smoothed. */
struct InkMap
{
  bool present = false;
  char record = 0; /* 'k' begins the stroke, 'a' adds to it */
  uint32_t element = 0, group = 0;
  std::string fill;
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; /* X = a px + c py + e, Y = b px + d py + f */
  double w0 = 0, w1 = 0, exponent = 1;
  bool pressure = false; /* the report measures pressure (valid bit 1) */
  /* The record, as ink_point writes one, for a report at x, y with pressure p. */
  std::string render( int64_t x, int64_t y, int64_t p ) const;
  bool operator==( const InkMap& other ) const = default;
};

/* A report from the client's terminal on its way to the program. Without a
   report, it is the client saying it is still there while something it
   reported is in progress. */
struct ExtensionInput
{
  std::string payload;    /* the OSC payload, from "777;"; empty for a heartbeat */
  uint32_t ink_image = 0; /* the records the terminal applied as ink, to this image */
  std::string ink_records;
  std::string service_reply; /* the terminal's answer to a request of the vector service */
  int64_t clock = -1; /* the terminal's clock in ms, as the client reckons it when it sent this */
  InkMap ink; /* how ink_records were rendered from the report, when they were; not compared */
  ExtensionInput( const std::string& s_payload = std::string() ) : payload( s_payload ) {}
  bool operator==( const ExtensionInput& other ) const
  {
    return payload == other.payload && ink_image == other.ink_image && ink_records == other.ink_records
           && service_reply == other.service_reply && clock == other.clock;
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
/* A field as an integer, or 0 when it is not one. */
int64_t integer_or_zero( const Ext::Fields& f, const char* key );
/* 1 with the point, 0 when this report adds none, -1 when the stroke is over. */
int ink_point( const InkStroke& stroke,
               const Ext::Fields& report,
               const Framebuffer& fb,
               const ClientGeometry& geometry,
               bool starting,
               InkStroke* found,
               std::string& point,
               InkMap* map = nullptr );

/* The queries a client sends its terminal when it starts, to fill in a
   ClientExtensions from the replies. Setting a mode and reading it back is
   how a terminal says which bits its user lets programs have; the client
   leaves the display reports on, so it hears of a change of display. */
std::string extension_probe();
/* The modes the probe sets before each read, in order. */
std::vector<std::pair<std::string, uint32_t>> extension_probe_modes();
/* The OSC 22 query of the names the relay answers for. */
std::string pointer_probe();
/* The query of the notification protocol, OSC 99, with a return route, so
   that the answer says whether the terminal keeps one. */
std::string notification_probe();
/* The terminal's answer to that query: true when the frame is one. */
bool notification_probe_reply( const std::string& payload, ClientExtensions& info );
/* Whether an OSC payload is a desktop notification a program may write: OSC
   9, OSC 99 or OSC 777 notify. */
bool is_notification( const std::string& payload );
/* OSC 9;4, which reports progress and notifies nobody. */
bool is_progress_report( const std::string& payload );
/* Whether a frame from the client is an answer of the notification protocol
   that a program may be handed: OSC 99, whole, with nothing in it that could
   end it early or begin something else. */
bool notification_answer_ok( const std::string& frame );
bool program_status_answer_ok( const std::string& frame );
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
    bool forward = false; /* input goes to the server, through the smoother and the collector */
    ExtensionInput input;
    InkMap ink;              /* how to render the ink record again once the point is smoothed */
    std::string to_terminal; /* written back to the terminal at once */
    /* The terminal's answer about a notification (it was clicked, closed, is
       still shown), for the program, ahead of everything else. */
    std::string notice;
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
  /* A request of the vector service on its way to the terminal, seen as the
     client writes it; the terminal's answer goes back to the server. */
  void service_written( const std::string& frame );
  /* While true the graphics replies in the input are looked at. */
  bool service_pending( uint64_t now );
  /* A graphics reply from the terminal. True when it answers a request: if it
     is the server's, in forward; if it is the client's own probe, in info. */
  bool service_reply( const std::string& frame, uint64_t now, bool& forward, ExtensionInput& input, bool& info_changed );

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
  std::set<uint32_t> service_ids; /* the requests the terminal has yet to answer */
  bool service_probed = false, service_probe_open = false;
  uint64_t service_probe_at = 0;
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
  /* Desktop notifications for the client's terminal. They are what a person
     who is looking elsewhere is told, so they go ahead of everything else,
     the screen included: see src/network/noticechannel.h. */
  std::vector<std::string> notices;

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
  /* A notification a program wrote: the payload of its OSC 9, OSC 99 or OSC
     777 notify, which is UTF-8. The query of OSC 99 is answered here. */
  void notification( const std::string& payload, std::string& to_host );
  /* OSC 22, the payload after "22;". */
  void pointer_shape( const std::string& payload, Framebuffer& fb, std::string& to_host );
  /* SGR 10 to 19: the default font role and the first nine configured. */
  void font_slot( Framebuffer& fb, int slot );
  /* a=v: a request of the vector service, for the terminal to answer. A
     font's upload and release change the session fonts in fb. */
  void vector_service( uint32_t id, uint32_t quiet, const std::string& json, Framebuffer& fb, std::string& to_host );
  /* The session fonts a text record may name, as the program sees them. */
  Vector::FontCheck font_check( const Framebuffer& fb ) const;
  /* A request of the vector service has waited at least this long. */
  bool service_overdue( uint64_t ms ) const;
  /* The value of an XTGETTCAP name the relay answers, false for others. */
  bool termcap( const std::string& name, std::string& value ) const;
  bool accepts_format( uint32_t format ) const;

private:
  typedef std::tuple<int64_t, int64_t, int64_t> ContactKey; /* kind, device, contact */
  struct Contact
  {
    int64_t tool = 1, mods = 0;
    std::string x = "0", y = "0";
    std::string src; /* what x,y are relative to, for a touchpad */
    bool pressure = false;
  };
  struct Gesture
  {
    std::string x = "0", y = "0", mods = "0", total, fingers;
  };
  std::map<ContactKey, Contact> contacts;
  std::set<ContactKey> suppressed;
  std::map<std::pair<int64_t, int64_t>, int64_t> pad_keys; /* (device, button): t of the press */
  std::set<std::pair<int64_t, int64_t>> released_keys;     /* released by the relay; the pad's own release is not passed on */
  std::set<int64_t> finger_group;                           /* finger devices with something reported since their last frame */
  /* The last description of each tool in proximity, (kind, device),
     for a program that sets the mode it already has: the terminal, whose mode
     does not change, does not describe them again. */
  std::map<std::pair<int64_t, int64_t>, Ext::Fields> devices;
  std::map<int64_t, Gesture> gestures;
  std::set<int64_t> suppressed_gestures;
  /* (device, contact): the image and element of a stroke the terminal inks */
  std::map<std::pair<int64_t, int64_t>, std::pair<uint32_t, uint32_t>> ink_strokes;
  uint32_t frame_seq = 0;
  /* Requests of the vector service the terminal has yet to answer, by what
     the relay calls one. */
  struct ServiceRequest
  {
    uint32_t id = 0, quiet = 0; /* as the program gave them */
    int64_t sent_ms = 0;
    char font_op = 0;                         /* 'u' an upload, 'r' a release of a session font */
    std::string font;                         /* its name */
    std::shared_ptr<const std::string> upload; /* the upload's JSON */
    /* The font of that name before the upload, which is kept at once but
       taken back if the terminal refuses it; null for none. */
    std::shared_ptr<const Vector::SessionFont> previous;
  };
  std::map<uint32_t, ServiceRequest> service_requests;
  uint32_t service_next = 0x80000000U;
  Ext::Fields display_sent;
  int64_t clock_t = -1;   /* the terminal's clock, as last heard */
  int64_t clock_at = 0;   /* when, on ours */

  int64_t now_t() const;
  void hear_time( const Ext::Fields& fields );
  void hear_clock( int64_t clock );
  bool clock_from_client = false;
  void reply( const std::string& selector, Ext::Fields fields, std::string& to_host ) const;
  void cancel_tablet( Framebuffer& fb, std::string& to_host );
  void describe_devices( uint32_t mode, std::string& to_host ) const;
  /* The mode bits of a selector the client's terminal lets programs have. */
  uint32_t allowed( const char* selector, uint32_t known ) const;
  void cancel_gestures( std::string& to_host );
  void set_display_mode( bool on, Framebuffer& fb, std::string& to_host );
  Ext::Fields display_fields() const;
  void tablet_program( const std::string& op, const Ext::Fields& f, Framebuffer& fb, const ClientGeometry* geometry,
                       std::string& to_host );
  void ink_program( const std::string& op, const Ext::Fields& f, Framebuffer& fb, std::string& to_host );
  void haptic_program( const std::string& op, const Ext::Fields& f, const std::string& payload,
                       std::string& to_host );
  void font_program( const Ext::Fields& f, Framebuffer& fb, std::string& to_host );
  void depth_program( const Ext::Fields& f, Framebuffer& fb );
  void service_answer( const std::string& reply, Framebuffer& fb, std::string& to_host );
  /* An EINVAL as Silkie writes one, unless q keeps it quiet. */
  static void service_error( uint32_t id, uint32_t quiet, const std::string& message, std::string& to_host );
  /* The session fonts' part of a request: true when it is answered here. */
  bool font_request( const std::string& json, Framebuffer& fb, ServiceRequest& request, std::string& to_host );
  void font_answer( uint32_t relay_id, const ServiceRequest& request, const std::string& body, bool ok,
                    Framebuffer& fb );
  void font_refused( uint32_t relay_id, const ServiceRequest& request, Framebuffer& fb );
  bool tablet_input( Ext::Fields& f, Framebuffer& fb, const ExtensionInput& input );
  void end_ink( std::pair<int64_t, int64_t> key, Framebuffer& fb );
  void refresh( const std::string& selector );
};

}

#endif
