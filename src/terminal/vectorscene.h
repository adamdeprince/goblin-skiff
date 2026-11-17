/*
    Goblin Skiff: vector images (f=200) of the graphics protocol
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef VECTORSCENE_HPP
#define VECTORSCENE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/terminal/vectorfonts.h"

/* A vector image is a scene of records, one per line, that the terminal at
   the far end rasterizes. Skiff never draws one: it keeps the scene as a table
   of records keyed by id, so the whole scene can be written out to a terminal
   that attaches later, and passes edits on as the patches they arrived as.
   The rules of what is accepted follow Silkie's own parser, so a scene the
   relay accepts is one the terminal accepts. */

namespace Terminal {
namespace Vector {

struct Matrix
{
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; /* x' = a x + c y + e, y' = b x + d y + f */
};
Matrix multiply( const Matrix& m, const Matrix& n ); /* n, then m */
bool invert( const Matrix& m, Matrix& out );
void apply( const Matrix& m, double& x, double& y );

struct Record
{
  char op = 0; /* 0 for a blank line or a record type this version ignores */
  uint32_t id = 0;
  uint32_t parent = 0;
  bool has_t = false;
  Matrix t;
  bool all = false, delete_group = false;
  bool has_d = false, has_pts = false; /* the fields are present */
  size_t d_commands = 0, pts_count = 0;  /* the geometry they hold */
  std::vector<std::string> d_parts;      /* each d of the record */
  std::string pts;
  std::string line;
  std::vector<std::string> fonts; /* the session fonts a text record names with "@" */
};

/* A color as the records write one, that paints something: not none. */
bool color_paints( const std::string& color );
/* Every record or none: the error of the first malformed one is "Line N: why".
   With fonts, a text record may name only those session fonts. */
bool parse_records( const std::string& text,
                    std::vector<Record>& out,
                    std::string& error,
                    const FontCheck* fonts = nullptr );

struct Element
{
  char type = 0;
  uint32_t id = 0;
  uint32_t parent = 0;
  bool has_t = false;
  Matrix t;
  std::string line;               /* the record as given, without pts for a pen stroke */
  std::string pts;                /* a pen stroke's points, with every append */
  std::vector<std::string> paths; /* path data appended to a path or a symbol */
  std::vector<std::string> fonts; /* the session fonts a text record names */
  size_t bytes() const;
};

class Table
{
  std::vector<uint32_t> order; /* the order of creation; 0 where an element was removed */
  std::unordered_map<uint32_t, std::pair<size_t, Element>> elements;
  size_t live = 0;
  std::string root_line; /* the last record of the root group, empty for the default */
  bool root_has_t = false;
  Matrix root_t;
  size_t element_bytes = 0;
  std::map<std::string, size_t> font_refs; /* session font: the text records that name it */

  void count_fonts( const Element& e, bool adding );
  const Element* lookup( uint32_t id ) const;
  bool is_descendant( const Element& e, uint32_t group ) const;
  void remove( uint32_t id );
  void insert( Element e );
  void compact();
  int matrix_of( const Element& e, Matrix& out ) const;

public:
  /* Applies records in order. A record that cannot be applied stops it, with
     the ones before it applied, as the terminal does; the error says why. */
  std::string apply( const std::vector<Record>& records );
  std::string text() const;
  /* Without the text records that name a session font for which font_ok
     is false. */
  std::string text( const std::function<bool( const std::string& )>& font_ok ) const;
  size_t bytes() const { return element_bytes + root_line.size(); }
  size_t size() const { return live; }
  /* Whether text in the scene names a session font. */
  bool names_font( const std::string& name ) const { return font_refs.count( name ) != 0; }
  const std::map<std::string, size_t>& fonts() const { return font_refs; }
  /* Takes a point in the user units of the image into the coordinate system
     of a group, through the transforms of the groups above it. */
  bool point_to_group( uint32_t group, double& x, double& y ) const;
  /* The matrix that takes a point in the user units of the image into the
     coordinate system of a group; false when the group is not one, or the
     transforms on the way down cannot be inverted. */
  bool group_inverse( uint32_t group, Matrix& out ) const;
};

struct Step
{
  std::string records;
  /* Applied by the terminal itself, as ink: on the terminal already, so it is
     passed on to the client's state but never written to its terminal. */
  bool terminal_applied = false;
  bool operator==( const Step& other ) const
  {
    return records == other.records && terminal_applied == other.terminal_applied;
  }
};

/* One version of a vector image. Versions are immutable once made, so the
   framebuffers that a relay keeps for its diffs share them. A version knows
   the steps from its parent, so a peer holding an older version of the same
   transmission (the lineage) is sent only the steps it lacks. */
class Doc
{
  friend std::shared_ptr<const Doc> make_doc( uint64_t, uint64_t, uint32_t, uint32_t, std::shared_ptr<Table> );
  friend std::shared_ptr<const Doc> edit_doc( const std::shared_ptr<const Doc>&, const Step&, std::string& );

  mutable std::shared_ptr<const Doc> parent_;
  std::vector<Step> steps_;
  size_t chain_bytes_ = 0;
  /* The table of the newest version moves to the version made from it; a
     checkpoint also keeps a copy of its own, from which any later version
     can be made again. */
  mutable std::shared_ptr<Table> table_;
  std::shared_ptr<const Table> checkpoint_;

public:
  uint64_t lineage = 0, version = 0;
  uint32_t width = 0, height = 0;

  const std::vector<Step>& steps() const { return steps_; }
  const std::shared_ptr<const Doc>& parent() const { return parent_; }
  std::shared_ptr<const Table> table() const;
};

uint64_t new_lineage();
std::shared_ptr<const Doc> make_doc( uint64_t lineage,
                                     uint64_t version,
                                     uint32_t width,
                                     uint32_t height,
                                     std::shared_ptr<Table> table );
/* A new version with one more step. The step is applied as the terminal
   would; an error is reported, but the version exists all the same, since the
   records before the failing one were applied. */
std::shared_ptr<const Doc> edit_doc( const std::shared_ptr<const Doc>& base, const Step& step, std::string& error );
/* The steps from version `from` of the same lineage up to `to`. */
bool steps_between( uint64_t lineage, uint64_t from, const std::shared_ptr<const Doc>& to, std::vector<Step>& out );

}
}

#endif
