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

#ifndef COMPRESSOR_H
#define COMPRESSOR_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace Network {
/* Chooses the zstd level of a state update. These sessions are for slow links,
   where every byte counts, so the level is 22 unless the CPU cannot keep up with
   the wire: unless making an update would take longer than the link takes to
   carry it, and longer than the time until the next send, so that the wire
   stands idle waiting for the compressor while input and acks pile up behind
   it. Then the highest level that does keep up is used, as predicted from how
   fast each level has been lately. While the speed of the link is not known
   nothing says the CPU is behind it, and the level stays at 22. A level only
   changes how the sender spends its time; a zstd frame decodes the same
   whatever level made it, so the peer need not be told. */
class CompressionPacer
{
public:
  static const int TOP_LEVEL = 22;

  /* How long the next send is away, and how fast the link carries what is
     compressed, as measured (0 when it has not been). */
  void set_budget( uint64_t send_interval_ms, double link_bytes_per_second );
  int level_for( size_t input_size ) const;
  /* What compressing took at a level. */
  void note( int level, size_t input_size, size_t output_size, double elapsed_ms );
  CompressionPacer();

private:
  static const size_t RUNGS = 8;
  /* From the highest level down. */
  double cost_ns_per_byte[RUNGS];
  double ratio[RUNGS]; /* of output to input */
  uint64_t interval_ms = 100;
  double link_rate = 0;
};

class Compressor
{
private:
  std::string zstd_dictionary;
  std::string zstd_dictionary_id_value;
  std::map<int, void*> zstd_cdicts; /* by level; each is made when first used */
  void* zstd_ddict;
  CompressionPacer pacer;
  int last_level = CompressionPacer::TOP_LEVEL;

public:
  Compressor();
  ~Compressor();

  /* A frame that is not compressed at all begins with this byte, which no
     zlib stream (whose first byte names deflate in its low bits) and no zstd
     frame begins with. It is sent only to a peer that said it takes one. */
  static const unsigned char RAW_FRAME = 0x00;

  std::string compress_str( const std::string& input );
  /* With allow_raw, an input that compression would only lengthen goes as it is. */
  std::string compress_str( const std::string& input, bool allow_zstd, bool allow_zstd_dictionary, bool allow_raw = false );
  std::string uncompress_str( const std::string& input );
  bool zstd_available( void ) const;
  void pace( uint64_t send_interval_ms, double link_bytes_per_second ) { pacer.set_budget( send_interval_ms, link_bytes_per_second ); }
  int last_zstd_level( void ) const { return last_level; }
  void set_zstd_dictionary( const std::string& dictionary );
  void set_zstd_dictionary_from_file( const std::string& path );
  bool has_zstd_dictionary( void ) const { return !zstd_dictionary.empty(); }
  const std::string& zstd_dictionary_id( void ) const { return zstd_dictionary_id_value; }

  /* unused */
  Compressor( const Compressor& );
  Compressor& operator=( const Compressor& );
};

Compressor& get_compressor( void );
}

#endif
