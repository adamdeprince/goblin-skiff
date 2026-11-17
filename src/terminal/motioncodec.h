/*
    Goblin Skiff: the reports of a hand as binary records on the wire
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef MOTIONCODEC_HPP
#define MOTIONCODEC_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "src/terminal/extensions.h"
#include "src/terminal/extframe.h"

/* A report of the tablet pointer protocol is a hundred and some bytes of
   text, most of it the same as the report before: the same keys in the same
   order, a time a few milliseconds on, a position a little further along the
   line the hand was already drawing. The client writes a run of reports as
   one record instead, and the server writes the text again, byte for byte.

   The record is a stream of varints. A shape is a selector and its keys in
   order, each key either a number or text; a track is one contact. What each
   of a track's numbers did lately predicts the next, in integer arithmetic
   that comes out the same on every machine: a position from a constant
   acceleration, everything else from a constant velocity, scaled by the
   report's own time. What is written is the difference, zigzag varints, and
   only for the numbers the prediction missed and the texts that changed; a
   mask says which. The ink the terminal drew for a point is sent as the map
   from the point to the stroke's coordinates, once per stroke, and the server
   renders the record from the point. Anything the record cannot say exactly,
   a report whose numbers are not written the plain way or one with a service
   reply, goes in it as text.

   The shapes, the tracks and their last samples are the context, and the
   context is part of the user stream's state: every report that goes into a
   stream, at either end and by whichever path, is noted in it the same way,
   so a diff is written from the context of the state the receiver has and
   read in that same context. A shape is defined once per session, an ink map
   once per stroke, and the first report of a packet is predicted from the
   last of the one before. */

namespace Terminal {
namespace Motion {

/* LEB128 varints; signed ones are zigzagged. */
void put_unsigned( std::string& out, uint64_t value );
void put_signed( std::string& out, int64_t value );
bool get_unsigned( const std::string& in, size_t& pos, uint64_t& value );
bool get_signed( const std::string& in, size_t& pos, int64_t& value );

/* The last few samples of one number, and what they say the next one is. The
   arithmetic is integer, and any step that would overflow falls back to a
   simpler model, so both ends agree to the bit. */
struct Series
{
  int count = 0;
  int64_t t[3] = { 0, 0, 0 }, v[3] = { 0, 0, 0 }; /* oldest first */
  void push( int64_t time, int64_t value );
  /* order 1: a constant velocity through the last two; 2: a constant
     acceleration through the last three. Rounded to the nearest integer. */
  int64_t predict( int64_t time, int order ) const;
};

/* The key order of the prediction: 2 for a position, 1 for anything else. */
int prediction_order( const std::string& key );

struct RecordShape
{
  std::string selector;
  std::vector<std::string> keys;
  std::vector<bool> numeric;
  int time_index = -1; /* the key `t`, when it is a number */
};
struct RecordTrack
{
  std::map<std::string, Series> series;
  std::map<std::string, std::string> text;
  uint64_t samples = 0;
  bool has_ink = false; /* the stroke's ink, while it is being inked */
  uint32_t ink_image = 0;
  InkMap ink;
};

/* What both ends know of the reports so far. */
class RecordContext
{
public:
  std::vector<RecordShape> shapes;
  std::vector<RecordTrack> tracks;
  std::map<std::string, size_t> track_index;
  bool have_clock = false, have_time = false;
  int64_t clock = 0, time = 0;

  /* A report worked out against the context: whether a record can say it
     exactly, and if so as what. */
  struct Prepared
  {
    Ext::Frame frame;
    size_t shape = 0, track = 0;
    int time_index = -1;
    bool new_shape = false, new_ink = false, has_ink = false, has_time = false, has_clock = false;
    int64_t when = 0, clock_residual = 0;
    std::vector<int64_t> values;
    std::vector<std::string> texts;
    std::vector<bool> numeric, changed;
    std::vector<uint64_t> residual;
  };
  bool prepare( const ExtensionInput& input, Prepared& out ) const;
  /* Takes the report in, as prepared. */
  void commit( const ExtensionInput& input, const Prepared& prepared );
  /* Takes a report in by whichever path it came. */
  void note( const ExtensionInput& input );
};

/* Writes a run of reports as one record, in a context it moves along. Every
   report goes in, exactly. */
class RecordEncoder
{
public:
  explicit RecordEncoder( RecordContext& s_context ) : context( s_context ) {}
  void add( const ExtensionInput& input );
  const std::string& bytes() const { return out; }
  size_t count() const { return added; }

private:
  RecordContext& context;
  std::string out;
  size_t added = 0;

  void literal( const ExtensionInput& input );
};

/* Reads a record back into the reports it was made of, moving the context
   along as the encoder did. False when the bytes are not a record. */
bool decode_records( const std::string& bytes, RecordContext& context, std::vector<ExtensionInput>& out );

}
}

#endif
