/*
    Goblin Skiff: the motion of a hand, smoothed and thinned before it crosses the link
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef HANDMOTION_HPP
#define HANDMOTION_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "src/terminal/extensions.h"
#include "src/terminal/extframe.h"

/* Everything a hand moves is reported many times a second, as the sensor
   measured it: a pen or a finger of the tablet pointer protocol, a mouse on a
   tablet, a scroll, a gesture, xterm's mouse motion, Skiff's own pointer
   protocol. A hand cannot move at the rate it is sampled, and no sensor is
   silent, so the client does two things to that motion before it goes out, in
   this order:

   1. Smoothing. The noise of the sensor is taken out of positions, pressure and
      tilt with a Kalman filter (fast-kalman, src/kalman) whose model is a hand:
      position, velocity and acceleration per axis. A report is held back for a
      short lag, and the reports that arrive meanwhile refine it with a
      Rauch-Tung-Striebel pass before it is sent, so the estimate does not lag
      at a bend as a filter alone would. Only the client runs this, so nothing
      about it has to agree between machines.

   2. Thinning. Consecutive reports of one contact are merged, as the framing
      allows a relay to, for as long as the stroke the program will see stays
      within a tolerance of every report merged away, and every measurement
      within its own tolerance of the straight line between the reports kept.
      The bends of the hand are kept; its straight runs are not, except that a
      hand in motion is reported at least every so many milliseconds however
      straight its path, so that a program sees the pace of the motion and its
      shape, and not one position per send of the link. Nothing the framing
      says may not be merged is: a down, an up, a cancel, a change of buttons,
      a correction, a prediction, a description, a frame.

   The settings come from the environment: GOBLIN_SKIFF_HAND_SMOOTHING, the
   lag in milliseconds (0 turns smoothing off); GOBLIN_SKIFF_HAND_TOLERANCE,
   in 1/256 of a pixel (0 drops nothing but what is exactly on the line);
   GOBLIN_SKIFF_HAND_INTERVAL, the most milliseconds a moving hand goes
   unreported (0 keeps every report); and GOBLIN_SKIFF_EXACT_MOUSE, which the
   client's --exact-mouse sets, under which every report goes as it came. */

namespace Terminal {
namespace Motion {

struct Settings
{
  uint64_t lag_ms = 15;
  /* The sensor, as a standard deviation, and the hand, as the spectral density
     of what its model does not predict. Positions are in 1/256 of a pixel and
     time in milliseconds. */
  double position_noise = 13;  /* about a twentieth of a pixel */
  double jerk = 0.05;          /* (1/256 px)^2 / ms^5: how acceleration wanders */
  double pressure_noise = 200; /* of 65535 */
  double pressure_drift = 10;  /* (1/65535)^2 / ms^3: how the rate of pressure wanders */
  double tilt_noise = 50;      /* hundredths of a degree */
  double tilt_drift = 0.5;
  /* How far a report that is merged away may lie from the stroke that is sent. */
  double tolerance = 64;            /* 1/256 px, a quarter pixel */
  double pad_tolerance = 64;        /* of 65535 across a touchpad */
  double pressure_tolerance = 1311; /* 2 percent of 65535 */
  double tilt_tolerance = 200;      /* 2 degrees */
  double angle_tolerance = 500;     /* 5 degrees, for twist and the shape of a contact */
  double fraction_tolerance = 0.02; /* of the range of any other measurement */
  /* A hand in motion is reported at least this often, in milliseconds, however
     straight its path: about forty times a second, twice what the hand's own
     bandwidth asks for. A report merged away is within half of this of one
     kept, the one before it or the one it merged into, as a point merged away
     is within the tolerance of the path kept. 0 keeps every report. */
  uint64_t keep_ms = 25;
  /* Every report goes as it came: nothing smoothed, nothing merged, nothing held. */
  bool exact = false;

  static Settings from_environment();
};

/* The kind of a report, as far as merging goes. */
enum class Merge
{
  Never,   /* goes as it came, after whatever is held */
  Always,  /* may be merged with the next of its contact however far apart */
  Bounded  /* may be merged while the stroke stays within the tolerances */
};

/* What may be done with a report, and under which contact. `compatible` names
   the state that must be the same for two reports to merge. */
Merge merge_policy( const Ext::Frame& frame, std::string& contact, std::string& compatible );

/* Whether the reports of `dropped`, and `held`, may all be merged into `next`
   while the stroke from `last` to `next` stays within the tolerances. A
   position tolerance given here (in the units of x and y) replaces the
   settings' own. */
bool within_tolerances( const Settings& settings,
                        const Ext::Fields& last,
                        const std::vector<Ext::Fields>& dropped,
                        const Ext::Fields& held,
                        const Ext::Fields& next,
                        double position_tolerance = -1 );

/* `later` becomes the two reports merged: the absolute values of `later`, the
   relative ones added up as each extension says. */
void merge_into( const Ext::Frame& earlier, Ext::Frame& later );

/* The collector: holds what may be merged until the link can take it, merges
   what the tolerances allow, and lets nothing overtake anything. Reports of
   the extensions, xterm mouse motion and the frames of Skiff's own pointer
   protocol all go through it, so that what is sent stays in the order it
   happened. */
class PointerCollector
{
public:
  explicit PointerCollector( const Settings& s_settings = Settings() ) : settings( s_settings ) {}

  struct Item
  {
    bool bytes = false;   /* raw is for the program: xterm mouse motion, or a frame of Skiff's pointer protocol */
    bool direct = false;  /* with bytes: a frame that goes as it is, never taken for keys */
    std::string raw;
    ExtensionInput input; /* otherwise */
  };
  /* The pieces of ordinary input, with xterm motion reports apart from the
     rest (first: is motion). Inside bracketed paste everything is literal. A
     negative encoding: motion is not being reported. */
  void split_motion( const std::string& bytes, int encoding, std::vector<std::pair<bool, std::string>>& pieces );
  /* Holds a report, a motion or a pointer frame; `ready` gets what is to be
     sent now. `link_interval` is the time between the link's sends. */
  void collect( const ExtensionInput& input, uint64_t now, uint64_t link_interval, bool backlog, std::vector<Item>& ready );
  void collect_motion( const std::string& raw, int encoding, uint64_t now, uint64_t link_interval, bool backlog,
                       std::vector<Item>& ready );
  void collect_pointer( const std::string& frame, uint64_t now, uint64_t link_interval, bool backlog,
                        std::vector<Item>& ready );
  /* Lets go of what is held once it is time: at most once a link interval,
     and not while the link has not yet taken what it was already given. */
  void release( uint64_t now, uint64_t link_interval, bool backlog, std::vector<Item>& ready );
  /* Something else is about to be sent. */
  void flush( std::vector<Item>& ready );
  int wait_time( uint64_t now, bool backlog ) const;
  bool holding() const { return !held.empty(); }

private:
  struct Held
  {
    std::string contact, compatible;
    Merge policy = Merge::Never;
    Ext::Frame frame; /* the report, for merging and the tolerances; x and y in 1/256 px */
    Item item;
    double tolerance = -1; /* a position tolerance of its own, or the settings' */
    uint64_t arrived = 0;    /* when it came */
    bool uncovered = false;  /* a report merged into it, or it, is too long after the one last kept */
    uint64_t since = 0;      /* when the oldest such report came: the next kept must be near it */
  };
  /* What the program has of a contact: when it was last sent a report, and
     for a contact kept to the tolerances, that report and the reports merged
     away since, which the next merge must keep within reach. */
  struct Track
  {
    bool have_kept = false;
    uint64_t kept_at = 0;
    bool have_last = false;
    Ext::Fields last;
    std::vector<Ext::Fields> dropped;
  };
  Settings settings;
  std::vector<Held> held;
  std::map<std::string, Track> tracks;
  uint64_t next_release = 0;
  std::string paste_marker;
  bool paste = false;

  void hold( Held entry, uint64_t now, uint64_t link_interval, bool backlog, std::vector<Item>& ready );
  void emit( Held& entry, std::vector<Item>& ready );
};

/* The smoother. */
class Smoother
{
public:
  explicit Smoother( const Settings& s_settings = Settings() );

  struct Report
  {
    ExtensionInput input; /* as the client has it, its ink record rendered from the raw point */
    InkMap ink;           /* how to render that record again once the point is smoothed */
  };
  /* Takes in a report the client is forwarding. What is ready goes to `ready`
     in order: a report that is not a contact's motion at once, a contact's
     once its lag has passed. */
  void feed( const Report& report, uint64_t now, std::vector<ExtensionInput>& ready );
  /* Lets go of what has waited its lag. */
  void release( uint64_t now, std::vector<ExtensionInput>& ready );
  /* Lets go of everything, as when the client is leaving. */
  void flush( std::vector<ExtensionInput>& ready );
  int wait_time( uint64_t now ) const;
  bool pending() const { return !queue.empty(); }
  const Settings& get_settings() const { return settings; }

private:
  struct Pending
  {
    ExtensionInput input;
    Ext::Frame frame;
    InkMap ink;
    uint64_t arrived = 0;
    std::string key;        /* the contact: selector, kind, device, contact */
    bool measured = false;  /* has a position the smoother refines */
    bool ends = false;      /* an up, a cancel or an out: the contact's filter starts over after it */
  };
  struct Track; /* the filters of one contact; the Kalman types stay in handmotion.cc */
  Settings settings;
  std::deque<Pending> queue;
  std::map<std::string, std::unique_ptr<Track>> tracks;

  Track& track( const std::string& key );
  void emit( Pending& front, std::vector<ExtensionInput>& ready );
  void smooth( Pending& front );

public:
  ~Smoother();
  Smoother( const Smoother& ) = delete;
  Smoother& operator=( const Smoother& ) = delete;
};

}
}

#endif
