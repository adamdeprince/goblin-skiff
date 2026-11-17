/*
    Goblin Skiff: the session fonts and text layouts of vector images (f=200)
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef VECTORFONTS_HPP
#define VECTORFONTS_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* Silkie lays out text records and keeps uploaded fonts in Python
   (kitty/fonts/vector.py). The relay cannot shape text, having no fonts, but
   it reads the JSON as Python's json module does and makes every check of
   the layout that needs no font file, so a text record it accepts is one the
   terminal can lay out. The fonts a program uploads are terminal state: the
   relay keeps them, synchronized with the client, so a terminal that is sent
   a scene whole again (a new client, a repaint, a client far behind) is
   given the fonts its text names first. */

namespace Terminal {
namespace Vector {

/* Python's json.loads of bytes: the encoding found as json does it (UTF-8,
   with or without a byte order mark, UTF-16 or UTF-32), lone surrogates let
   through, NaN and the infinities taken, and integers of at most 4300
   digits. Strings are kept as UTF-8 that may hold surrogates (WTF-8). The
   values live in one flat vector, so no depth of nesting costs the stack. */
struct Json
{
  enum Kind
  {
    Null,
    False,
    True,
    Number,
    String,
    Array,
    Object
  };
  struct Node
  {
    Kind kind = Null;
    double number = 0;          /* Number; may be infinite, as Python's float */
    std::string text;           /* String */
    std::vector<size_t> items;  /* Array and Object: the values, in order */
    std::vector<std::string> keys; /* Object: the key of each value */
  };
  std::vector<Node> nodes; /* nodes[0] is the document */

  const Node& root() const { return nodes[0]; }
  /* A member of an object as a dict has it: the last of the same key. */
  const Node* get( const Node& object, const std::string& key ) const;
};
bool json_loads( const std::string& bytes, Json& out );
/* json.dumps of a string, with ensure_ascii, quotes included. */
std::string json_dumps_string( const std::string& text );
/* base64.b64decode(..., validate=True), as Python 3.14 has it. */
bool base64_strict( const std::string& in, std::string& out );
/* HarfBuzz's hb_feature_from_string, of a string as Python gives it to C. */
bool harfbuzz_feature( const std::string& text );

/* What the relay knows of an uploaded font: from the terminal's answer to
   the upload, its variation axes. */
struct FontInfo
{
  struct Axis
  {
    double minimum = 0, maximum = 0;
  };
  std::map<std::string, Axis> axes;
};

/* The session fonts a layout may name with "@id", as the program sees them:
   those uploaded and not released, and those whose upload is on its way. A
   font without info has its axes checked by the terminal alone. */
struct FontCheck
{
  std::map<std::string, std::shared_ptr<const FontInfo>> fonts;
};

/* The checks of Silkie's layout() that need no font, of a text record's
   text field. The session fonts it names with "@" go to handles; a font
   check of null takes any name. */
bool text_layout_ok( const std::string& field, const FontCheck* fonts, std::vector<std::string>& handles );

/* A font a program uploaded, as the relay and its client keep it. */
struct SessionFont
{
  std::shared_ptr<const std::string> upload; /* the JSON of the upload request, as the program sent it */
  uint64_t generation = 0;                   /* changes with each upload under the name */
  bool released = false;                     /* kept only because text in a scene names it */
  uint32_t request = 0; /* the relay's id of the request that uploaded it, which the client wrote */
  std::shared_ptr<const FontInfo> info; /* the server's, from the answer; not synchronized */

  bool operator==( const SessionFont& other ) const
  {
    return generation == other.generation && released == other.released && request == other.request;
  }
  bool operator!=( const SessionFont& other ) const { return !( *this == other ); }
};
typedef std::map<std::string, SessionFont> SessionFonts;

/* The request id the client gives the uploads and releases it makes itself,
   quietly, to bring its terminal's fonts in line. */
const uint32_t FONT_REPLAY_ID = 7254;
/* The JSON of a request to release a session font. */
std::string font_release_request( const std::string& name );

}
}

#endif
