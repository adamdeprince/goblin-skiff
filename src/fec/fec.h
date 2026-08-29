/*
    Mosh: the mobile shell
    Copyright 2026

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
*/

#ifndef FEC_FEC_H
#define FEC_FEC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FEC {
enum class CodecKind
{
  ReedSolomon,
  RaptorQ
};

struct Symbol
{
  uint32_t id;
  std::string payload;

  Symbol();
  Symbol( uint32_t s_id, const std::string& s_payload );
};

struct BlockMetadata
{
  CodecKind codec;
  uint64_t original_size;
  uint16_t symbol_size;
  uint16_t source_symbols;

  /* RFC6330 receiver parameters. Reed-Solomon leaves these as zero. */
  uint64_t raptorq_oti_common;
  uint32_t raptorq_oti_scheme;

  BlockMetadata();
};

struct EncodedBlock
{
  BlockMetadata metadata;
  std::vector<Symbol> symbols;

  EncodedBlock() : metadata(), symbols() {}
};

class BlockCodec
{
public:
  virtual ~BlockCodec() {}

  virtual CodecKind kind( void ) const = 0;
  virtual EncodedBlock encode( const std::string& block, uint16_t symbol_size, uint32_t repair_symbols ) const = 0;
  virtual bool decode( const BlockMetadata& metadata, const std::vector<Symbol>& symbols, std::string& block ) const = 0;
};

const char* codec_name( CodecKind kind );
CodecKind parse_codec_name( const std::string& name );
bool codec_available( CodecKind kind );
std::unique_ptr<BlockCodec> make_codec( CodecKind kind );
}

#endif
