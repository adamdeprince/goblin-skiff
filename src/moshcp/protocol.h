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

#ifndef MOSHCP_PROTOCOL_H
#define MOSHCP_PROTOCOL_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "src/fec/fec.h"

namespace MoshCP {
struct BlockInfo
{
  uint64_t original_size;
  uint16_t source_symbols;
  uint64_t raptorq_oti_common;
  uint32_t raptorq_oti_scheme;

  BlockInfo();
};

struct Manifest
{
  std::string filename;
  uint64_t size;
  uint64_t uncompressed_size;
  uint32_t mode;
  int64_t mtime;
  uint32_t block_size;
  uint16_t symbol_size;
  FEC::CodecKind codec;
  bool archive;
  bool preserve_permissions;
  bool zstd_compressed;
  uint32_t zstd_level;
  std::string sha256_hex;
  std::vector<BlockInfo> blocks;

  Manifest();
};

struct RepairRequest
{
  uint32_t block_id;
  uint32_t extra_symbols;

  RepairRequest();
  RepairRequest( uint32_t s_block_id, uint32_t s_extra_symbols );
};

struct Ack
{
  std::vector<std::pair<uint32_t, uint32_t>> decoded_ranges;
  std::vector<RepairRequest> repair_requests;
  uint32_t decoded_blocks;
  uint64_t received_symbols;
  uint64_t duplicate_symbols;
  uint64_t repair_symbols_requested;

  Ack();
};

struct Finish
{
  bool success;
  std::string sha256_hex;
  std::string message;

  Finish();
};

std::string encode_manifest( const Manifest& manifest );
bool decode_manifest( const std::string& payload, Manifest& manifest );

std::string encode_ack( const Ack& ack );
bool decode_ack( const std::string& payload, Ack& ack );

std::string encode_finish( const Finish& finish );
bool decode_finish( const std::string& payload, Finish& finish );
}

#endif
