/*
    Goblin Skiff: OSC 777 extension framing
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef EXTFRAME_HPP
#define EXTFRAME_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* The terminal extensions of Silkie share one framing:

     OSC 777 ; <selector> ; key=value:key=value ST

   Fields are ASCII, unordered and case sensitive. A program's frame is
   rejected as a whole when a key repeats, a value is malformed or out of
   range, a required field is missing, or the fields exceed 127 bytes. Unknown
   keys are ignored, which is how the extensions grow without new versions. */

namespace Terminal {
namespace Ext {

/* The limit the framing puts on what a program sends, counted from after
   the selector. It keeps relays and terminals in agreement about which frames
   exist; nothing here stores a frame in a buffer of this size. */
const size_t PROGRAM_FIELDS_LIMIT = 127;

class Fields
{
public:
  std::vector<std::pair<std::string, std::string>> items;

  const std::string* find( const std::string& key ) const;
  bool has( const std::string& key ) const { return find( key ) != nullptr; }
  void set( const std::string& key, const std::string& value );
  void set( const std::string& key, int64_t value );
  void erase( const std::string& key );
  bool empty() const { return items.empty(); }
  std::string text() const;
  bool operator==( const Fields& other ) const { return items == other.items; }
  bool operator!=( const Fields& other ) const { return !( *this == other ); }
};

struct Frame
{
  std::string selector;
  Fields fields;
};

enum class Value
{
  Absent,
  Valid,
  Invalid
};

/* Parses "key=value:key=value". Duplicate keys and malformed pairs fail. */
bool parse_fields( const std::string& text, Fields& out );
/* Parses the payload of an OSC 777, which begins with "777;". */
bool parse( const std::string& payload, Frame& out );
/* Program frames: the shared rules above, plus "v" and "op" being present. */
bool parse_program( const std::string& payload, Frame& out );

/* A decimal integer from lo to hi, with no leading +. */
bool parse_integer( const std::string& text, int64_t lo, int64_t hi, int64_t& out );
Value integer( const Fields& fields, const std::string& key, int64_t lo, int64_t hi, int64_t& out );

/* The frame as the terminal writes it, terminated by ST. */
std::string encode( const std::string& selector, const Fields& fields );
inline std::string encode( const Frame& frame ) { return encode( frame.selector, frame.fields ); }

/* The shortest decimal text that reads back as the same double. */
std::string number( double value );

/* Numbers on the wire are written with a decimal point whatever the user's
   locale says; while one of these exists, this thread formats and parses
   them in the C locale. */
class CNumbers
{
  void* previous;

public:
  CNumbers();
  ~CNumbers();
  CNumbers( const CNumbers& ) = delete;
  CNumbers& operator=( const CNumbers& ) = delete;
};

/* Separates OSC 777 and OSC 22 frames a terminal writes from the rest of
   its input, in order. Inside bracketed paste everything is literal. Frames
   are held whole, however long, until they end; a prefix that does not become
   a frame, such as a lone Escape key, is released by expire(). */
class InputDemux
{
  std::string pending_;
  std::string paste_marker_;
  bool paste_ = false;
  bool matched_ = false;
  bool escape_ = false;

public:
  struct Output
  {
    /* Each item is either ordinary input or a whole frame, in input order. */
    std::vector<std::pair<bool, std::string>> items; // first: is a frame
  };
  void feed( const std::string& bytes, Output& out );
  /* Releases a held prefix that never became a frame. Never a partial frame. */
  void expire( Output& out );
  bool pending() const { return !pending_.empty(); }
  bool in_frame() const { return matched_; }
};

/* The payload of a frame from InputDemux: the text between OSC and ST. */
std::string frame_payload( const std::string& frame );

}
}

#endif
