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

#include "src/moshcp/protocol.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}
}

int main( void )
{
  try {
    MoshCP::Manifest manifest;
    manifest.filename = "file.bin";
    manifest.size = 12345;
    manifest.uncompressed_size = 54321;
    manifest.mode = 0640;
    manifest.mtime = 1710000000;
    manifest.block_size = 32768;
    manifest.symbol_size = 512;
    manifest.codec = FEC::CodecKind::RaptorQ;
    manifest.archive = true;
    manifest.preserve_permissions = true;
    manifest.zstd_compressed = true;
    manifest.zstd_level = 22;
    manifest.sha256_hex = "optional";

    MoshCP::BlockInfo block;
    block.original_size = 12345;
    block.source_symbols = 25;
    block.raptorq_oti_common = 0x0102030405060708ULL;
    block.raptorq_oti_scheme = 0x11223344;
    manifest.blocks.push_back( block );

    MoshCP::Manifest decoded_manifest;
    require( MoshCP::decode_manifest( MoshCP::encode_manifest( manifest ), decoded_manifest ),
             "manifest did not decode" );
    require( decoded_manifest.filename == manifest.filename, "manifest filename mismatch" );
    require( decoded_manifest.size == manifest.size, "manifest size mismatch" );
    require( decoded_manifest.uncompressed_size == manifest.uncompressed_size, "manifest uncompressed size mismatch" );
    require( decoded_manifest.mode == manifest.mode, "manifest mode mismatch" );
    require( decoded_manifest.mtime == manifest.mtime, "manifest mtime mismatch" );
    require( decoded_manifest.block_size == manifest.block_size, "manifest block size mismatch" );
    require( decoded_manifest.symbol_size == manifest.symbol_size, "manifest symbol size mismatch" );
    require( decoded_manifest.codec == manifest.codec, "manifest codec mismatch" );
    require( decoded_manifest.archive == manifest.archive, "manifest archive mismatch" );
    require( decoded_manifest.preserve_permissions == manifest.preserve_permissions, "manifest preserve mismatch" );
    require( decoded_manifest.zstd_compressed == manifest.zstd_compressed, "manifest zstd mismatch" );
    require( decoded_manifest.zstd_level == manifest.zstd_level, "manifest zstd level mismatch" );
    require( decoded_manifest.blocks.size() == 1, "manifest block count mismatch" );
    require( decoded_manifest.blocks[0].raptorq_oti_common == block.raptorq_oti_common,
             "manifest RaptorQ common OTI mismatch" );

    MoshCP::Ack ack;
    ack.decoded_ranges.push_back( std::make_pair( 0U, 3U ) );
    ack.decoded_ranges.push_back( std::make_pair( 8U, 8U ) );
    ack.repair_requests.push_back( MoshCP::RepairRequest( 4, 12 ) );
    ack.decoded_blocks = 5;
    ack.received_symbols = 100;
    ack.duplicate_symbols = 7;
    ack.repair_symbols_requested = 12;

    MoshCP::Ack decoded_ack;
    require( MoshCP::decode_ack( MoshCP::encode_ack( ack ), decoded_ack ), "ack did not decode" );
    require( decoded_ack.decoded_ranges == ack.decoded_ranges, "ack ranges mismatch" );
    require( decoded_ack.repair_requests.size() == 1, "ack repair count mismatch" );
    require( decoded_ack.repair_requests[0].block_id == 4, "ack repair block mismatch" );
    require( decoded_ack.repair_requests[0].extra_symbols == 12, "ack repair count mismatch" );
    require( decoded_ack.decoded_blocks == ack.decoded_blocks, "ack decoded block count mismatch" );
    require( decoded_ack.received_symbols == ack.received_symbols, "ack received symbols mismatch" );
    require( decoded_ack.duplicate_symbols == ack.duplicate_symbols, "ack duplicate symbols mismatch" );
    require( decoded_ack.repair_symbols_requested == ack.repair_symbols_requested, "ack repair symbols mismatch" );

    MoshCP::Finish finish;
    finish.success = true;
    finish.sha256_hex = "hash";
    finish.message = "done";

    MoshCP::Finish decoded_finish;
    require( MoshCP::decode_finish( MoshCP::encode_finish( finish ), decoded_finish ), "finish did not decode" );
    require( decoded_finish.success == finish.success, "finish success mismatch" );
    require( decoded_finish.sha256_hex == finish.sha256_hex, "finish hash mismatch" );
    require( decoded_finish.message == finish.message, "finish message mismatch" );
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
