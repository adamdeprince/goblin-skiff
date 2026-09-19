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

#include "src/include/config.h"

#include "src/fec/fec.h"
#include "src/moshcp/protocol.h"
#include "src/network/bulkcontrol.h"
#include "src/network/bulkdatagram.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

using Network::Bulk::Datagram;
using Network::Bulk::PacketType;

namespace {
const uint32_t DEFAULT_BLOCK_SIZE = 32768;
const uint16_t DEFAULT_SYMBOL_SIZE = 512;
const uint64_t DEFAULT_RATE = 2048;
const uint32_t DEFAULT_REDUNDANCY_PERCENT = 20;
const uint32_t MAX_REPAIR_REQUESTS_PER_ACK = 16;

struct Options
{
  std::string socket_path;
  FEC::CodecKind codec;
  uint32_t redundancy_percent;
  uint64_t rate_bytes_per_second;
  uint32_t block_size;
  uint16_t symbol_size;
  bool quiet;
  bool multi_receive;
  bool transfer_id_set;
  uint64_t transfer_id;
  bool recursive;
  bool preserve_permissions;
  bool zstd;
  int zstd_level;
  std::string dest_name;

  Options()
    : socket_path(), codec( FEC::CodecKind::ReedSolomon ), redundancy_percent( DEFAULT_REDUNDANCY_PERCENT ),
      rate_bytes_per_second( DEFAULT_RATE ), block_size( DEFAULT_BLOCK_SIZE ), symbol_size( DEFAULT_SYMBOL_SIZE ),
      quiet( false ), multi_receive( false ), transfer_id_set( false ), transfer_id( 0 ), recursive( false ),
      preserve_permissions( false ), zstd( false ), zstd_level( 3 ), dest_name()
  {}
};

struct SendBlock
{
  FEC::EncodedBlock encoded;
  size_t initial_symbols;
  size_t sent_symbols;
  bool decoded_by_receiver;

  SendBlock() : encoded(), initial_symbols( 0 ), sent_symbols( 0 ), decoded_by_receiver( false ) {}
};

struct ReceiveBlock
{
  std::vector<FEC::Symbol> symbols;
  std::set<uint32_t> seen_symbols;
  std::string decoded;
  bool decoded_ok;
  uint64_t last_repair_request_ms;
  size_t last_repair_request_symbols;

  ReceiveBlock()
    : symbols(), seen_symbols(), decoded(), decoded_ok( false ), last_repair_request_ms( 0 ),
      last_repair_request_symbols( 0 )
  {}
};

struct ReceiveStats
{
  uint64_t received_symbols;
  uint64_t duplicate_symbols;
  uint64_t repair_symbols_requested;
  uint64_t last_status_ms;

  ReceiveStats() : received_symbols( 0 ), duplicate_symbols( 0 ), repair_symbols_requested( 0 ), last_status_ms( 0 ) {}
};

class Pacer
{
private:
  uint64_t bytes_per_second;
  std::chrono::steady_clock::time_point next_send;

public:
  explicit Pacer( uint64_t s_bytes_per_second )
    : bytes_per_second( s_bytes_per_second ), next_send( std::chrono::steady_clock::now() )
  {}

  void wait_for( size_t bytes )
  {
    if ( bytes_per_second == 0 || bytes == 0 ) {
      return;
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if ( next_send > now ) {
      std::this_thread::sleep_until( next_send );
    } else {
      next_send = now;
    }

    const double seconds = static_cast<double>( bytes ) / static_cast<double>( bytes_per_second );
    next_send += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>( seconds ) );
  }
};

uint64_t now_ms( void )
{
  return static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch() )
                                 .count() );
}

void progress( const Options& options, const std::string& message )
{
  if ( !options.quiet ) {
    std::cerr << "goblin-skiffcp: " << message << "\n";
  }
}

[[noreturn]] void usage( const char* argv0 )
{
  std::cerr << "Usage:\n"
            << "  " << argv0 << " send [options] SOURCE...\n"
            << "  " << argv0 << " receive [options] [DEST|DESTDIR]\n"
            << "\nOptions:\n"
            << "  --socket=PATH          goblin-skiffcp control socket (default: GOBLIN_SKIFF_CP_SOCK or goblin-skiffcp.latest)\n"
            << "  --fec=reed-solomon|raptorq  RaptorQ requires a custom build\n"
            << "  --redundancy=PERCENT   initial repair symbols to send (default: 20)\n"
            << "  --rate=BYTES           bulk send rate, 0 for unlimited (default: 2048)\n"
            << "  --block-size=BYTES     FEC block size (default: 32768)\n"
            << "  --symbol-size=BYTES    FEC symbol/datagram payload size (default: 512)\n"
            << "  --transfer-id=HEX      pin a send or receive to a specific transfer id\n"
            << "  --multi                keep receive running and accept multiple transfers\n"
            << "  -r, --recursive        recursively send directories\n"
            << "  -p, --preserve         preserve file modes and mtimes\n"
            << "  --dest-name=NAME       destination basename for a single source\n"
            << "  -z, --zstd             compress payload with zstd before FEC\n"
            << "  --zstd-level=N         zstd compression level (default: 3, e.g. 22)\n"
            << "  --quiet                suppress periodic progress output\n";
  std::exit( 2 );
}

uint64_t parse_u64( const std::string& raw, const char* name )
{
  if ( raw.empty() ) {
    throw std::runtime_error( std::string( "empty " ) + name );
  }

  std::string number = raw;
  uint64_t multiplier = 1;
  const char suffix = number[number.size() - 1];
  if ( suffix == 'k' || suffix == 'K' ) {
    multiplier = 1024;
    number.resize( number.size() - 1 );
  } else if ( suffix == 'm' || suffix == 'M' ) {
    multiplier = 1024 * 1024;
    number.resize( number.size() - 1 );
  } else if ( suffix == 'g' || suffix == 'G' ) {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
    number.resize( number.size() - 1 );
  }

  errno = 0;
  char* end = NULL;
  const unsigned long long value = strtoull( number.c_str(), &end, 10 );
  if ( errno || end == number.c_str() || *end != '\0' ) {
    throw std::runtime_error( std::string( "bad " ) + name + ": " + raw );
  }
  if ( value > ULLONG_MAX / multiplier ) {
    throw std::runtime_error( std::string( name ) + " is too large" );
  }
  return static_cast<uint64_t>( value ) * multiplier;
}

uint32_t parse_percent( std::string raw )
{
  if ( !raw.empty() && raw[raw.size() - 1] == '%' ) {
    raw.resize( raw.size() - 1 );
  }
  const uint64_t value = parse_u64( raw, "redundancy" );
  if ( value > 1000 ) {
    throw std::runtime_error( "redundancy must be at most 1000%" );
  }
  return static_cast<uint32_t>( value );
}

uint64_t parse_transfer_id( std::string raw )
{
  if ( raw.size() > 2 && raw[0] == '0' && ( raw[1] == 'x' || raw[1] == 'X' ) ) {
    raw.erase( 0, 2 );
  }
  if ( raw.empty() ) {
    throw std::runtime_error( "empty transfer id" );
  }

  errno = 0;
  char* end = NULL;
  const unsigned long long value = strtoull( raw.c_str(), &end, 16 );
  if ( errno || end == raw.c_str() || *end != '\0' || value == 0 ) {
    throw std::runtime_error( "bad transfer id: " + raw );
  }
  return static_cast<uint64_t>( value );
}

void parse_options( int argc, char* argv[], Options& options )
{
  const option long_options[] = {
    { "socket", required_argument, NULL, 's' },
    { "fec", required_argument, NULL, 'f' },
    { "redundancy", required_argument, NULL, 256 },
    { "rate", required_argument, NULL, 'R' },
    { "block-size", required_argument, NULL, 'b' },
    { "symbol-size", required_argument, NULL, 'S' },
    { "transfer-id", required_argument, NULL, 'i' },
    { "multi", no_argument, NULL, 'm' },
    { "recursive", no_argument, NULL, 'r' },
    { "preserve", no_argument, NULL, 'p' },
    { "dest-name", required_argument, NULL, 'd' },
    { "zstd", no_argument, NULL, 'z' },
    { "zstd-level", required_argument, NULL, 'Z' },
    { "quiet", no_argument, NULL, 'q' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 },
  };

  while ( true ) {
    const int opt = getopt_long( argc, argv, "s:f:R:b:S:i:mrpd:zqZ:h", long_options, NULL );
    if ( opt < 0 ) {
      return;
    }

    switch ( opt ) {
      case 's':
        options.socket_path = optarg;
        break;
      case 'f':
        options.codec = FEC::parse_codec_name( optarg );
        break;
      case 256:
        options.redundancy_percent = parse_percent( optarg );
        break;
      case 'R':
        options.rate_bytes_per_second = parse_u64( optarg, "rate" );
        break;
      case 'b': {
        const uint64_t value = parse_u64( optarg, "block size" );
        if ( value == 0 || value > UINT32_MAX ) {
          throw std::runtime_error( "block size is out of range" );
        }
        options.block_size = static_cast<uint32_t>( value );
        break;
      }
      case 'S': {
        const uint64_t value = parse_u64( optarg, "symbol size" );
        if ( value == 0 || value > UINT16_MAX ) {
          throw std::runtime_error( "symbol size is out of range" );
        }
        options.symbol_size = static_cast<uint16_t>( value );
        break;
      }
      case 'i':
        options.transfer_id = parse_transfer_id( optarg );
        options.transfer_id_set = true;
        break;
      case 'm':
        options.multi_receive = true;
        break;
      case 'r':
        options.recursive = true;
        break;
      case 'p':
        options.preserve_permissions = true;
        break;
      case 'd':
        options.dest_name = optarg;
        break;
      case 'z':
        options.zstd = true;
        break;
      case 'Z': {
        const uint64_t value = parse_u64( optarg, "zstd level" );
        if ( value > 22 ) {
          throw std::runtime_error( "zstd level must be between 0 and 22" );
        }
        options.zstd = true;
        options.zstd_level = static_cast<int>( value );
        break;
      }
      case 'q':
        options.quiet = true;
        break;
      case 'h':
      default:
        usage( argv[0] );
    }
  }
}

std::string control_socket_path( const Options& options )
{
  if ( !options.socket_path.empty() ) {
    return options.socket_path;
  }
  return Network::Bulk::discover_control_socket();
}

std::string base_name( std::string path )
{
  while ( path.size() > 1 && path[path.size() - 1] == '/' ) {
    path.resize( path.size() - 1 );
  }

  const size_t slash = path.find_last_of( '/' );
  std::string name = slash == std::string::npos ? path : path.substr( slash + 1 );
  for ( size_t i = 0; i < name.size(); i++ ) {
    if ( name[i] == '/' || name[i] == '\0' ) {
      name[i] = '_';
    }
  }
  if ( name.empty() || name == "." || name == ".." ) {
    return "goblin-skiffcp.out";
  }
  return name;
}

std::string transfer_id_hex( uint64_t transfer_id )
{
  std::ostringstream out;
  out << std::hex << std::setfill( '0' ) << std::setw( 16 ) << transfer_id;
  return out.str();
}

uint64_t random_transfer_id( void )
{
  uint64_t id = ( static_cast<uint64_t>( now_ms() ) << 21 ) ^ static_cast<uint64_t>( getpid() );
  std::ifstream urandom( "/dev/urandom", std::ios::in | std::ios::binary );
  if ( urandom ) {
    uint64_t random = 0;
    urandom.read( reinterpret_cast<char*>( &random ), sizeof random );
    if ( urandom.gcount() == static_cast<std::streamsize>( sizeof random ) ) {
      id ^= random;
    }
  }
  return id ? id : 1;
}

struct TransferPayload
{
  std::string payload;
  std::string filename;
  uint64_t uncompressed_size;
  uint32_t mode;
  int64_t mtime;
  bool archive;

  TransferPayload() : payload(), filename(), uncompressed_size( 0 ), mode( 0644 ), mtime( 0 ), archive( false ) {}
};

struct ArchiveEntry
{
  bool directory;
  std::string path;
  uint32_t mode;
  int64_t mtime;
  std::string data;

  ArchiveEntry() : directory( false ), path(), mode( 0644 ), mtime( 0 ), data() {}
};

void append_archive_u8( std::string& out, uint8_t value )
{
  out.push_back( static_cast<char>( value ) );
}

void append_archive_u32( std::string& out, uint32_t value )
{
  out.push_back( static_cast<char>( ( value >> 24 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 16 ) & 0xff ) );
  out.push_back( static_cast<char>( ( value >> 8 ) & 0xff ) );
  out.push_back( static_cast<char>( value & 0xff ) );
}

void append_archive_u64( std::string& out, uint64_t value )
{
  for ( int shift = 56; shift >= 0; shift -= 8 ) {
    out.push_back( static_cast<char>( ( value >> shift ) & 0xff ) );
  }
}

void append_archive_i64( std::string& out, int64_t value )
{
  append_archive_u64( out, static_cast<uint64_t>( value ) );
}

void append_archive_string( std::string& out, const std::string& value )
{
  if ( value.size() > UINT32_MAX ) {
    throw std::runtime_error( "archive string too large" );
  }
  append_archive_u32( out, static_cast<uint32_t>( value.size() ) );
  out.append( value );
}

bool read_archive_u8( const std::string& in, size_t& offset, uint8_t& value )
{
  if ( offset + 1 > in.size() ) {
    return false;
  }
  value = static_cast<uint8_t>( in[offset++] );
  return true;
}

bool read_archive_u32( const std::string& in, size_t& offset, uint32_t& value )
{
  if ( offset + 4 > in.size() ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 4; i++ ) {
    value = ( value << 8 ) | static_cast<uint8_t>( in[offset + i] );
  }
  offset += 4;
  return true;
}

bool read_archive_u64( const std::string& in, size_t& offset, uint64_t& value )
{
  if ( offset + 8 > in.size() ) {
    return false;
  }
  value = 0;
  for ( size_t i = 0; i < 8; i++ ) {
    value = ( value << 8 ) | static_cast<uint8_t>( in[offset + i] );
  }
  offset += 8;
  return true;
}

bool read_archive_i64( const std::string& in, size_t& offset, int64_t& value )
{
  uint64_t raw = 0;
  if ( !read_archive_u64( in, offset, raw ) ) {
    return false;
  }
  value = static_cast<int64_t>( raw );
  return true;
}

bool read_archive_string( const std::string& in, size_t& offset, std::string& value )
{
  uint32_t size = 0;
  if ( !read_archive_u32( in, offset, size ) || offset + size > in.size() ) {
    return false;
  }
  value.assign( in.data() + offset, size );
  offset += size;
  return true;
}

std::string clean_relative_path( std::string path )
{
  while ( !path.empty() && path[0] == '/' ) {
    path.erase( 0, 1 );
  }
  std::vector<std::string> parts;
  std::string current;
  for ( size_t i = 0; i <= path.size(); i++ ) {
    if ( i == path.size() || path[i] == '/' ) {
      if ( !current.empty() && current != "." && current != ".." ) {
        parts.push_back( current );
      }
      current.clear();
    } else if ( path[i] != '\0' ) {
      current.push_back( path[i] );
    }
  }
  std::string cleaned;
  for ( size_t i = 0; i < parts.size(); i++ ) {
    if ( i ) {
      cleaned += "/";
    }
    cleaned += parts[i];
  }
  if ( cleaned.empty() ) {
    return "goblin-skiffcp.out";
  }
  return cleaned;
}

std::string join_path( const std::string& left, const std::string& right )
{
  if ( left.empty() || left == "." ) {
    return right;
  }
  if ( left[left.size() - 1] == '/' ) {
    return left + right;
  }
  return left + "/" + right;
}

uint32_t source_symbol_count( size_t block_size, uint16_t symbol_size )
{
  if ( block_size == 0 ) {
    return 0;
  }
  return static_cast<uint32_t>( ( block_size + symbol_size - 1 ) / symbol_size );
}

uint32_t repair_symbols_for_percent( uint32_t source_symbols, uint32_t percent )
{
  if ( source_symbols == 0 || percent == 0 ) {
    return 0;
  }
  return static_cast<uint32_t>( ( static_cast<uint64_t>( source_symbols ) * percent + 99 ) / 100 );
}

uint32_t repair_pool_size( FEC::CodecKind kind, uint32_t source_symbols, uint32_t initial_repair )
{
  if ( source_symbols == 0 ) {
    return 0;
  }

  uint32_t desired = initial_repair + std::max<uint32_t>( 8, source_symbols );
  if ( kind == FEC::CodecKind::ReedSolomon ) {
    if ( source_symbols > 256 ) {
      throw std::runtime_error( "Reed-Solomon supports at most 256 source symbols per block" );
    }
    desired = std::min<uint32_t>( desired, 256 - source_symbols );
  }
  return desired;
}

std::string read_file_bytes( const std::string& path )
{
  std::ifstream in( path.c_str(), std::ios::in | std::ios::binary );
  if ( !in ) {
    throw std::runtime_error( "could not open input file: " + path );
  }
  return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
}

std::vector<std::string> split_blocks( const std::string& payload, uint32_t block_size )
{
  std::vector<std::string> blocks;
  for ( size_t offset = 0; offset < payload.size(); offset += block_size ) {
    blocks.push_back( payload.substr( offset, block_size ) );
  }
  if ( payload.empty() ) {
    blocks.push_back( std::string() );
  }
  return blocks;
}

void collect_archive_entries( const std::string& fs_path,
                              const std::string& archive_path,
                              const Options& options,
                              std::vector<ArchiveEntry>& entries,
                              uint64_t& total_file_bytes )
{
  struct stat st;
  if ( lstat( fs_path.c_str(), &st ) < 0 ) {
    throw std::runtime_error( std::string( "stat: " ) + strerror( errno ) + ": " + fs_path );
  }

  if ( S_ISDIR( st.st_mode ) ) {
    if ( !options.recursive ) {
      throw std::runtime_error( "omitting directory without -r: " + fs_path );
    }

    ArchiveEntry entry;
    entry.directory = true;
    entry.path = clean_relative_path( archive_path );
    entry.mode = static_cast<uint32_t>( st.st_mode & 0777 );
    entry.mtime = static_cast<int64_t>( st.st_mtime );
    entries.push_back( entry );

    DIR* dir = opendir( fs_path.c_str() );
    if ( dir == NULL ) {
      throw std::runtime_error( std::string( "opendir: " ) + strerror( errno ) + ": " + fs_path );
    }
    std::vector<std::string> names;
    while ( struct dirent* dent = readdir( dir ) ) {
      const std::string name = dent->d_name;
      if ( name != "." && name != ".." ) {
        names.push_back( name );
      }
    }
    closedir( dir );
    std::sort( names.begin(), names.end() );
    for ( std::vector<std::string>::const_iterator it = names.begin(); it != names.end(); ++it ) {
      collect_archive_entries( join_path( fs_path, *it ), join_path( archive_path, *it ), options, entries, total_file_bytes );
    }
    return;
  }

  if ( !S_ISREG( st.st_mode ) ) {
    throw std::runtime_error( "unsupported input file type: " + fs_path );
  }

  ArchiveEntry entry;
  entry.directory = false;
  entry.path = clean_relative_path( archive_path );
  entry.mode = static_cast<uint32_t>( st.st_mode & 0777 );
  entry.mtime = static_cast<int64_t>( st.st_mtime );
  entry.data = read_file_bytes( fs_path );
  total_file_bytes += entry.data.size();
  entries.push_back( entry );
}

std::string encode_archive( const std::vector<ArchiveEntry>& entries )
{
  std::string out( "MCPA", 4 );
  append_archive_u8( out, 1 );
  append_archive_u32( out, static_cast<uint32_t>( entries.size() ) );
  for ( std::vector<ArchiveEntry>::const_iterator it = entries.begin(); it != entries.end(); ++it ) {
    append_archive_u8( out, it->directory ? 2 : 1 );
    append_archive_string( out, it->path );
    append_archive_u32( out, it->mode );
    append_archive_i64( out, it->mtime );
    append_archive_u64( out, it->data.size() );
    out.append( it->data );
  }
  return out;
}

std::vector<ArchiveEntry> decode_archive( const std::string& payload )
{
  if ( payload.size() < 9 || memcmp( payload.data(), "MCPA", 4 ) != 0 || static_cast<uint8_t>( payload[4] ) != 1 ) {
    throw std::runtime_error( "bad moshcp archive" );
  }
  size_t offset = 5;
  uint32_t count = 0;
  if ( !read_archive_u32( payload, offset, count ) ) {
    throw std::runtime_error( "truncated moshcp archive" );
  }
  std::vector<ArchiveEntry> entries;
  entries.reserve( count );
  for ( uint32_t i = 0; i < count; i++ ) {
    uint8_t type = 0;
    uint64_t size = 0;
    ArchiveEntry entry;
    if ( !read_archive_u8( payload, offset, type ) || !read_archive_string( payload, offset, entry.path )
         || !read_archive_u32( payload, offset, entry.mode ) || !read_archive_i64( payload, offset, entry.mtime )
         || !read_archive_u64( payload, offset, size ) || offset + size > payload.size() ) {
      throw std::runtime_error( "truncated moshcp archive" );
    }
    if ( type != 1 && type != 2 ) {
      throw std::runtime_error( "bad moshcp archive entry type" );
    }
    entry.directory = type == 2;
    entry.path = clean_relative_path( entry.path );
    if ( !entry.directory ) {
      entry.data.assign( payload.data() + offset, size );
    }
    offset += size;
    entries.push_back( entry );
  }
  if ( offset != payload.size() ) {
    throw std::runtime_error( "trailing data in moshcp archive" );
  }
  return entries;
}

bool path_exists( const std::string& path )
{
  struct stat st;
  return stat( path.c_str(), &st ) == 0;
}

bool path_is_directory( const std::string& path )
{
  struct stat st;
  return stat( path.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

bool source_is_directory( const std::string& path )
{
  struct stat st;
  if ( lstat( path.c_str(), &st ) < 0 ) {
    throw std::runtime_error( std::string( "stat: " ) + strerror( errno ) + ": " + path );
  }
  return S_ISDIR( st.st_mode );
}

void ensure_directory( const std::string& path, mode_t mode, bool preserve_permissions )
{
  if ( path.empty() || path == "." ) {
    return;
  }
  if ( mkdir( path.c_str(), preserve_permissions ? mode : 0777 ) < 0 ) {
    if ( errno != EEXIST || !path_is_directory( path ) ) {
      throw std::runtime_error( std::string( "mkdir: " ) + strerror( errno ) + ": " + path );
    }
  }
  if ( preserve_permissions ) {
    chmod( path.c_str(), mode );
  }
}

void ensure_parent_directories( const std::string& path )
{
  size_t slash = path.find( '/' );
  while ( slash != std::string::npos ) {
    const std::string parent = path.substr( 0, slash );
    if ( !parent.empty() ) {
      ensure_directory( parent, 0777, false );
    }
    slash = path.find( '/', slash + 1 );
  }
}

void set_mtime( const std::string& path, int64_t mtime )
{
  struct utimbuf times;
  times.actime = static_cast<time_t>( mtime );
  times.modtime = static_cast<time_t>( mtime );
  utime( path.c_str(), &times );
}

std::string compress_zstd_payload( const std::string& payload, int level )
{
#ifdef HAVE_ZSTD
  const size_t bound = ZSTD_compressBound( payload.size() );
  std::string compressed( bound, '\0' );
  const void* src = payload.empty() ? NULL : payload.data();
  const size_t written = ZSTD_compress( &compressed[0], compressed.size(), src, payload.size(), level );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd compression failed: " ) + ZSTD_getErrorName( written ) );
  }
  compressed.resize( written );
  return compressed;
#else
  (void)payload;
  (void)level;
  throw std::runtime_error( "zstd support was not enabled at configure time" );
#endif
}

std::string decompress_zstd_payload( const std::string& payload, uint64_t expected_size )
{
#ifdef HAVE_ZSTD
  if ( expected_size > static_cast<uint64_t>( std::numeric_limits<size_t>::max() ) ) {
    throw std::runtime_error( "zstd payload is too large for this host" );
  }
  const unsigned long long frame_size = ZSTD_getFrameContentSize( payload.data(), payload.size() );
  if ( frame_size == ZSTD_CONTENTSIZE_ERROR ) {
    throw std::runtime_error( "zstd payload is not a valid frame" );
  }
  if ( frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size != expected_size ) {
    throw std::runtime_error( "zstd uncompressed size does not match manifest" );
  }

  std::string decompressed( static_cast<size_t>( expected_size ), '\0' );
  void* dst = decompressed.empty() ? NULL : &decompressed[0];
  const size_t written = ZSTD_decompress( dst, decompressed.size(), payload.data(), payload.size() );
  if ( ZSTD_isError( written ) ) {
    throw std::runtime_error( std::string( "zstd decompression failed: " ) + ZSTD_getErrorName( written ) );
  }
  if ( written != decompressed.size() ) {
    throw std::runtime_error( "zstd decompressed size does not match manifest" );
  }
  return decompressed;
#else
  (void)payload;
  (void)expected_size;
  throw std::runtime_error( "zstd support was not enabled at configure time" );
#endif
}

std::vector<std::string> send_sources_from_args( const Options& options,
                                                 int argc,
                                                 char* argv[],
                                                 std::string& destination_name )
{
  std::vector<std::string> sources;
  while ( optind < argc ) {
    sources.push_back( argv[optind++] );
  }
  if ( sources.empty() ) {
    usage( argv[0] );
  }

  destination_name = options.dest_name;
  if ( !destination_name.empty() && sources.size() != 1 ) {
    throw std::runtime_error( "--dest-name can only be used with one source" );
  }

  if ( destination_name.empty() && sources.size() == 2 && path_exists( sources[0] ) && !path_exists( sources[1] ) ) {
    destination_name = sources[1];
    sources.pop_back();
  }

  return sources;
}

TransferPayload prepare_transfer_payload( const Options& options,
                                          const std::vector<std::string>& sources,
                                          const std::string& destination_name )
{
  bool archive = sources.size() > 1;
  for ( std::vector<std::string>::const_iterator it = sources.begin(); it != sources.end(); ++it ) {
    if ( source_is_directory( *it ) ) {
      archive = true;
    }
  }

  TransferPayload transfer;
  if ( !archive ) {
    const std::string& input_path = sources.front();
    struct stat st;
    if ( lstat( input_path.c_str(), &st ) < 0 ) {
      throw std::runtime_error( std::string( "stat: " ) + strerror( errno ) + ": " + input_path );
    }
    if ( !S_ISREG( st.st_mode ) ) {
      throw std::runtime_error( "input is not a regular file: " + input_path );
    }

    transfer.payload = read_file_bytes( input_path );
    transfer.filename = destination_name.empty() ? base_name( input_path ) : base_name( destination_name );
    transfer.uncompressed_size = transfer.payload.size();
    transfer.mode = static_cast<uint32_t>( st.st_mode & 0777 );
    transfer.mtime = static_cast<int64_t>( st.st_mtime );
    transfer.archive = false;
    return transfer;
  }

  std::vector<ArchiveEntry> entries;
  uint64_t total_file_bytes = 0;
  for ( std::vector<std::string>::const_iterator it = sources.begin(); it != sources.end(); ++it ) {
    std::string archive_name = base_name( *it );
    if ( sources.size() == 1 && !destination_name.empty() ) {
      archive_name = clean_relative_path( destination_name );
    }
    collect_archive_entries( *it, archive_name, options, entries, total_file_bytes );
  }

  (void)total_file_bytes;
  transfer.payload = encode_archive( entries );
  transfer.filename = destination_name.empty() ? "goblin-skiffcp-archive" : base_name( destination_name );
  transfer.uncompressed_size = transfer.payload.size();
  transfer.mode = 0755;
  transfer.mtime = static_cast<int64_t>( std::time( NULL ) );
  transfer.archive = true;
  return transfer;
}

void send_datagram( Network::Bulk::ControlClient& client, const Datagram& datagram, Pacer* pacer )
{
  if ( pacer ) {
    pacer->wait_for( Network::Bulk::encode_datagram( datagram ).size() );
  }
  client.send( datagram );
}

void send_payload_fragments( Network::Bulk::ControlClient& client,
                             PacketType type,
                             uint64_t transfer_id,
                             const std::string& payload,
                             size_t payload_per_datagram,
                             Pacer* pacer )
{
  payload_per_datagram = std::max<size_t>( 128, payload_per_datagram );
  const uint32_t fragments
    = std::max<uint32_t>( 1, static_cast<uint32_t>( ( payload.size() + payload_per_datagram - 1 ) / payload_per_datagram ) );

  for ( uint32_t fragment = 0; fragment < fragments; fragment++ ) {
    const size_t offset = static_cast<size_t>( fragment ) * payload_per_datagram;
    Datagram datagram;
    datagram.type = type;
    datagram.transfer_id = transfer_id;
    datagram.block_id = fragment;
    datagram.symbol_id = fragments;
    datagram.payload = payload.substr( offset, payload_per_datagram );
    send_datagram( client, datagram, pacer );
  }
}

void send_symbol( Network::Bulk::ControlClient& client,
                  uint64_t transfer_id,
                  uint32_t block_id,
                  const FEC::Symbol& symbol,
                  Pacer& pacer )
{
  Datagram datagram;
  datagram.type = PacketType::Symbol;
  datagram.transfer_id = transfer_id;
  datagram.block_id = block_id;
  datagram.symbol_id = symbol.id;
  datagram.payload = symbol.payload;
  send_datagram( client, datagram, &pacer );
}

void send_ack( Network::Bulk::ControlClient& client, uint64_t transfer_id, const MoshCP::Ack& ack )
{
  Datagram datagram;
  datagram.type = PacketType::Ack;
  datagram.transfer_id = transfer_id;
  datagram.payload = MoshCP::encode_ack( ack );
  send_datagram( client, datagram, NULL );
}

void send_finish( Network::Bulk::ControlClient& client,
                  uint64_t transfer_id,
                  bool success,
                  const std::string& message )
{
  MoshCP::Finish finish;
  finish.success = success;
  finish.message = message;

  Datagram datagram;
  datagram.type = PacketType::Finish;
  datagram.transfer_id = transfer_id;
  datagram.payload = MoshCP::encode_finish( finish );

  for ( unsigned int i = 0; i < 5; i++ ) {
    send_datagram( client, datagram, NULL );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
  }
}

bool wait_for_datagram( Network::Bulk::ControlClient& client, Datagram& datagram, int timeout_ms )
{
  fd_set rfds;
  FD_ZERO( &rfds );
  FD_SET( client.fd(), &rfds );

  struct timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = ( timeout_ms % 1000 ) * 1000;

  int ret = 0;
  do {
    ret = select( client.fd() + 1, &rfds, NULL, NULL, &timeout );
  } while ( ret < 0 && errno == EINTR );

  if ( ret < 0 ) {
    throw std::runtime_error( std::string( "select: " ) + strerror( errno ) );
  }
  if ( ret == 0 ) {
    return false;
  }
  return client.recv( datagram );
}

FEC::BlockMetadata metadata_from_manifest( const MoshCP::Manifest& manifest, const MoshCP::BlockInfo& block )
{
  FEC::BlockMetadata metadata;
  metadata.codec = manifest.codec;
  metadata.original_size = block.original_size;
  metadata.symbol_size = manifest.symbol_size;
  metadata.source_symbols = block.source_symbols;
  metadata.raptorq_oti_common = block.raptorq_oti_common;
  metadata.raptorq_oti_scheme = block.raptorq_oti_scheme;
  return metadata;
}

size_t decoded_block_count( const std::vector<ReceiveBlock>& blocks )
{
  size_t decoded = 0;
  for ( std::vector<ReceiveBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it ) {
    if ( it->decoded_ok ) {
      decoded++;
    }
  }
  return decoded;
}

MoshCP::Ack build_ack( std::vector<ReceiveBlock>& blocks,
                       const MoshCP::Manifest& manifest,
                       bool request_repairs,
                       ReceiveStats& stats )
{
  MoshCP::Ack ack;

  uint32_t range_start = 0;
  bool in_range = false;
  for ( uint32_t i = 0; i < blocks.size(); i++ ) {
    if ( blocks[i].decoded_ok ) {
      if ( !in_range ) {
        range_start = i;
        in_range = true;
      }
    } else if ( in_range ) {
      ack.decoded_ranges.push_back( std::make_pair( range_start, i - 1 ) );
      in_range = false;
    }
  }
  if ( in_range ) {
    ack.decoded_ranges.push_back( std::make_pair( range_start, static_cast<uint32_t>( blocks.size() - 1 ) ) );
  }

  if ( request_repairs ) {
    const uint64_t now = now_ms();
    for ( uint32_t i = 0; i < blocks.size() && ack.repair_requests.size() < MAX_REPAIR_REQUESTS_PER_ACK; i++ ) {
      ReceiveBlock& block = blocks[i];
      if ( block.decoded_ok ) {
        continue;
      }

      const uint32_t source_symbols = manifest.blocks[i].source_symbols;
      if ( source_symbols == 0 ) {
        continue;
      }
      if ( now - block.last_repair_request_ms < 1000 && block.last_repair_request_symbols == block.symbols.size() ) {
        continue;
      }

      uint32_t extra = 4;
      if ( block.symbols.size() < source_symbols ) {
        extra += source_symbols - static_cast<uint32_t>( block.symbols.size() );
      }
      ack.repair_requests.push_back( MoshCP::RepairRequest( i, extra ) );
      stats.repair_symbols_requested += extra;
      block.last_repair_request_ms = now;
      block.last_repair_request_symbols = block.symbols.size();
    }
  }

  ack.decoded_blocks = static_cast<uint32_t>( decoded_block_count( blocks ) );
  ack.received_symbols = stats.received_symbols;
  ack.duplicate_symbols = stats.duplicate_symbols;
  ack.repair_symbols_requested = stats.repair_symbols_requested;
  return ack;
}

bool all_decoded( const std::vector<ReceiveBlock>& blocks )
{
  for ( std::vector<ReceiveBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it ) {
    if ( !it->decoded_ok ) {
      return false;
    }
  }
  return true;
}

bool all_acknowledged( const std::vector<SendBlock>& blocks )
{
  for ( std::vector<SendBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it ) {
    if ( !it->decoded_by_receiver ) {
      return false;
    }
  }
  return true;
}

void apply_ack_to_send_blocks( const MoshCP::Ack& ack, std::vector<SendBlock>& blocks )
{
  if ( blocks.empty() ) {
    return;
  }

  for ( std::vector<std::pair<uint32_t, uint32_t>>::const_iterator it = ack.decoded_ranges.begin();
        it != ack.decoded_ranges.end();
        ++it ) {
    const uint32_t last = std::min<uint32_t>( it->second, static_cast<uint32_t>( blocks.size() - 1 ) );
    for ( uint32_t block = it->first; block <= last; block++ ) {
      blocks[block].decoded_by_receiver = true;
      if ( block == UINT32_MAX ) {
        break;
      }
    }
  }
}

void write_all_fd( int fd, const char* data, size_t size )
{
  size_t offset = 0;
  while ( offset < size ) {
    ssize_t written = write( fd, data + offset, size - offset );
    if ( written < 0 && errno == EINTR ) {
      continue;
    }
    if ( written < 0 ) {
      throw std::runtime_error( std::string( "write: " ) + strerror( errno ) );
    }
    if ( written == 0 ) {
      throw std::runtime_error( "short write" );
    }
    offset += written;
  }
}

std::string output_path_for_manifest( const MoshCP::Manifest& manifest, const std::string& destination )
{
  const std::string name = base_name( manifest.filename );
  if ( destination.empty() ) {
    return name;
  }

  struct stat st;
  if ( stat( destination.c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) ) {
    return destination + "/" + name;
  }
  if ( !destination.empty() && destination[destination.size() - 1] == '/' ) {
    return destination + name;
  }
  return destination;
}

void write_output_bytes( const std::string& output_path,
                         const std::string& bytes,
                         uint32_t mode,
                         int64_t mtime,
                         bool preserve_permissions,
                         uint64_t transfer_id )
{
  const std::string tmp_path = output_path + ".goblin-skiffcp.tmp." + transfer_id_hex( transfer_id );
  ensure_parent_directories( tmp_path );

  const mode_t write_mode = preserve_permissions ? static_cast<mode_t>( mode & 0777 ) : 0666;
  int fd = open( tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, write_mode ? write_mode : 0600 );
  if ( fd < 0 ) {
    throw std::runtime_error( std::string( "open: " ) + strerror( errno ) );
  }

  try {
    write_all_fd( fd, bytes.data(), bytes.size() );
  } catch ( const std::exception& ) {
    close( fd );
    unlink( tmp_path.c_str() );
    throw;
  }

  if ( close( fd ) < 0 ) {
    unlink( tmp_path.c_str() );
    throw std::runtime_error( std::string( "close: " ) + strerror( errno ) );
  }

  if ( preserve_permissions ) {
    chmod( tmp_path.c_str(), write_mode ? write_mode : 0600 );
    set_mtime( tmp_path, mtime );
  }

  if ( rename( tmp_path.c_str(), output_path.c_str() ) < 0 ) {
    const int saved_errno = errno;
    unlink( tmp_path.c_str() );
    throw std::runtime_error( std::string( "rename: " ) + strerror( saved_errno ) );
  }
}

std::string blocks_to_payload( const MoshCP::Manifest& manifest, const std::vector<ReceiveBlock>& blocks )
{
  std::string payload;
  for ( std::vector<ReceiveBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it ) {
    payload.append( it->decoded );
  }
  if ( payload.size() != manifest.size ) {
    throw std::runtime_error( "decoded payload size does not match manifest" );
  }
  if ( manifest.zstd_compressed ) {
    payload = decompress_zstd_payload( payload, manifest.uncompressed_size );
  } else if ( manifest.uncompressed_size != 0 && payload.size() != manifest.uncompressed_size ) {
    throw std::runtime_error( "decoded uncompressed size does not match manifest" );
  }
  return payload;
}

std::string write_archive_payload( const std::string& destination,
                                   const MoshCP::Manifest& manifest,
                                   const std::string& payload,
                                   uint64_t transfer_id )
{
  const std::string root = destination.empty() ? "." : destination;
  if ( !destination.empty() ) {
    ensure_directory( root, 0777, false );
  }

  const std::vector<ArchiveEntry> entries = decode_archive( payload );
  for ( std::vector<ArchiveEntry>::const_iterator it = entries.begin(); it != entries.end(); ++it ) {
    if ( it->directory ) {
      const std::string output_path = join_path( root, it->path );
      ensure_directory( output_path, static_cast<mode_t>( it->mode & 0777 ), manifest.preserve_permissions );
    }
  }

  for ( std::vector<ArchiveEntry>::const_iterator it = entries.begin(); it != entries.end(); ++it ) {
    const std::string output_path = join_path( root, it->path );
    if ( it->directory ) {
      continue;
    }
    ensure_parent_directories( output_path );
    write_output_bytes(
      output_path, it->data, it->mode, it->mtime, manifest.preserve_permissions, transfer_id );
  }

  if ( manifest.preserve_permissions ) {
    for ( std::vector<ArchiveEntry>::const_reverse_iterator it = entries.rbegin(); it != entries.rend(); ++it ) {
      if ( it->directory ) {
        set_mtime( join_path( root, it->path ), it->mtime );
      }
    }
  }

  return root;
}

std::string write_decoded_output( const std::string& destination,
                                  const MoshCP::Manifest& manifest,
                                  const std::vector<ReceiveBlock>& blocks,
                                  uint64_t transfer_id )
{
  const std::string payload = blocks_to_payload( manifest, blocks );
  if ( manifest.archive ) {
    return write_archive_payload( destination, manifest, payload, transfer_id );
  }

  const std::string output_path = output_path_for_manifest( manifest, destination );
  write_output_bytes(
    output_path, payload, manifest.mode, manifest.mtime, manifest.preserve_permissions, transfer_id );
  return output_path;
}

class ManifestAssembler
{
private:
  uint32_t total_fragments;
  std::map<uint32_t, std::string> fragments;

public:
  ManifestAssembler() : total_fragments( 0 ), fragments() {}

  bool add( const Datagram& datagram, std::string& payload )
  {
    if ( datagram.symbol_id == 0 ) {
      payload = datagram.payload;
      return true;
    }
    if ( datagram.block_id >= datagram.symbol_id ) {
      return false;
    }
    if ( total_fragments != 0 && total_fragments != datagram.symbol_id ) {
      total_fragments = 0;
      fragments.clear();
    }
    total_fragments = datagram.symbol_id;
    fragments[datagram.block_id] = datagram.payload;
    if ( fragments.size() != total_fragments ) {
      return false;
    }

    payload.clear();
    for ( uint32_t i = 0; i < total_fragments; i++ ) {
      std::map<uint32_t, std::string>::const_iterator it = fragments.find( i );
      if ( it == fragments.end() ) {
        return false;
      }
      payload.append( it->second );
    }
    return true;
  }
};

struct ReceiveTransfer
{
  ManifestAssembler assembler;
  bool manifest_ready;
  MoshCP::Manifest manifest;
  std::vector<ReceiveBlock> blocks;
  std::unique_ptr<FEC::BlockCodec> codec;
  std::vector<Datagram> pending_symbols;
  ReceiveStats stats;
  uint64_t last_ack_ms;

  ReceiveTransfer()
    : assembler(), manifest_ready( false ), manifest(), blocks(), codec(), pending_symbols(), stats(), last_ack_ms( 0 )
  {}
};

void validate_receive_manifest( const MoshCP::Manifest& manifest )
{
  if ( !FEC::codec_available( manifest.codec ) ) {
    throw std::runtime_error( std::string( FEC::codec_name( manifest.codec ) ) + " is not available in this build" );
  }
#ifndef HAVE_ZSTD
  if ( manifest.zstd_compressed ) {
    throw std::runtime_error( "zstd support was not enabled at configure time" );
  }
#endif
}

void report_receive_status( const Options& options, uint64_t transfer_id, const ReceiveTransfer& transfer )
{
  size_t decoded = 0;
  for ( std::vector<ReceiveBlock>::const_iterator it = transfer.blocks.begin(); it != transfer.blocks.end(); ++it ) {
    if ( it->decoded_ok ) {
      decoded++;
    }
  }
  std::ostringstream msg;
  msg << "transfer " << transfer_id_hex( transfer_id ) << " receiving " << transfer.manifest.filename << ", decoded "
      << decoded << "/" << transfer.blocks.size() << " blocks, rx " << transfer.stats.received_symbols
      << " symbols, dup " << transfer.stats.duplicate_symbols << ", repairs requested "
      << transfer.stats.repair_symbols_requested;
  progress( options, msg.str() );
}

void try_decode_block( const MoshCP::Manifest& manifest,
                       std::vector<ReceiveBlock>& blocks,
                       uint32_t block_id,
                       const FEC::BlockCodec& codec )
{
  if ( block_id >= blocks.size() || blocks[block_id].decoded_ok ) {
    return;
  }

  ReceiveBlock& block = blocks[block_id];
  const FEC::BlockMetadata metadata = metadata_from_manifest( manifest, manifest.blocks[block_id] );
  std::string decoded;
  if ( codec.decode( metadata, block.symbols, decoded ) ) {
    block.decoded.swap( decoded );
    block.decoded_ok = true;
  }
}

std::vector<ReceiveBlock> initialize_receive_blocks( const MoshCP::Manifest& manifest )
{
  std::vector<ReceiveBlock> blocks( manifest.blocks.size(), ReceiveBlock() );
  for ( size_t i = 0; i < blocks.size(); i++ ) {
    if ( manifest.blocks[i].original_size == 0 ) {
      blocks[i].decoded_ok = true;
      blocks[i].decoded.clear();
    }
  }
  return blocks;
}

void add_received_symbol( const Datagram& datagram,
                          const MoshCP::Manifest& manifest,
                          std::vector<ReceiveBlock>& blocks,
                          const FEC::BlockCodec& codec,
                          ReceiveStats& stats )
{
  if ( datagram.block_id >= blocks.size() ) {
    return;
  }
  ReceiveBlock& block = blocks[datagram.block_id];
  if ( !block.seen_symbols.insert( datagram.symbol_id ).second ) {
    stats.duplicate_symbols++;
    return;
  }
  stats.received_symbols++;
  block.symbols.push_back( FEC::Symbol( datagram.symbol_id, datagram.payload ) );
  try_decode_block( manifest, blocks, datagram.block_id, codec );
}

uint64_t sent_symbol_count( const std::vector<SendBlock>& blocks )
{
  uint64_t count = 0;
  for ( std::vector<SendBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it ) {
    count += it->sent_symbols;
  }
  return count;
}

int run_send( const Options& options, int argc, char* argv[] )
{
  if ( !FEC::codec_available( options.codec ) ) {
    throw std::runtime_error( std::string( FEC::codec_name( options.codec ) ) + " is not available in this build" );
  }

  std::string destination_name;
  const std::vector<std::string> sources = send_sources_from_args( options, argc, argv, destination_name );
  TransferPayload transfer = prepare_transfer_payload( options, sources, destination_name );
  const uint64_t uncompressed_size = transfer.uncompressed_size;
  if ( options.zstd ) {
    transfer.payload = compress_zstd_payload( transfer.payload, options.zstd_level );
  }

  const std::vector<std::string> input_blocks = split_blocks( transfer.payload, options.block_size );
  const uint64_t transfer_id = options.transfer_id_set ? options.transfer_id : random_transfer_id();
  std::unique_ptr<FEC::BlockCodec> codec = FEC::make_codec( options.codec );

  MoshCP::Manifest manifest;
  manifest.filename = transfer.filename;
  manifest.size = transfer.payload.size();
  manifest.uncompressed_size = uncompressed_size;
  manifest.mode = transfer.mode;
  manifest.mtime = transfer.mtime;
  manifest.block_size = options.block_size;
  manifest.symbol_size = options.symbol_size;
  manifest.codec = options.codec;
  manifest.archive = transfer.archive;
  manifest.preserve_permissions = options.preserve_permissions;
  manifest.zstd_compressed = options.zstd;
  manifest.zstd_level = options.zstd ? static_cast<uint32_t>( options.zstd_level ) : 0;

  std::vector<SendBlock> send_blocks;
  send_blocks.reserve( input_blocks.size() );
  for ( size_t i = 0; i < input_blocks.size(); i++ ) {
    const uint32_t source_symbols = source_symbol_count( input_blocks[i].size(), options.symbol_size );
    const uint32_t initial_repair = repair_symbols_for_percent( source_symbols, options.redundancy_percent );
    const uint32_t repair_pool = repair_pool_size( options.codec, source_symbols, initial_repair );

    SendBlock block;
    block.encoded = codec->encode( input_blocks[i], options.symbol_size, repair_pool );
    const size_t desired_initial = static_cast<size_t>( block.encoded.metadata.source_symbols ) + initial_repair;
    block.initial_symbols = std::min<size_t>( desired_initial, block.encoded.symbols.size() );
    block.sent_symbols = block.initial_symbols;

    MoshCP::BlockInfo info;
    info.original_size = block.encoded.metadata.original_size;
    info.source_symbols = block.encoded.metadata.source_symbols;
    info.raptorq_oti_common = block.encoded.metadata.raptorq_oti_common;
    info.raptorq_oti_scheme = block.encoded.metadata.raptorq_oti_scheme;
    manifest.blocks.push_back( info );
    send_blocks.push_back( block );
  }

  {
    std::ostringstream msg;
    msg << "sending transfer " << transfer_id_hex( transfer_id ) << " as " << manifest.filename << ", "
        << sources.size() << " source" << ( sources.size() == 1 ? "" : "s" ) << ", "
        << manifest.blocks.size() << " block" << ( manifest.blocks.size() == 1 ? "" : "s" ) << ", "
        << manifest.size << " wire bytes";
    if ( manifest.zstd_compressed ) {
      msg << " (" << manifest.uncompressed_size << " before zstd level " << manifest.zstd_level << ")";
    }
    msg << ", fec=" << FEC::codec_name( manifest.codec );
    progress( options, msg.str() );
  }

  Network::Bulk::ControlClient client( control_socket_path( options ) );
  Pacer pacer( options.rate_bytes_per_second );
  const std::string manifest_payload = MoshCP::encode_manifest( manifest );
  const size_t manifest_payload_per_datagram = options.symbol_size;

  send_payload_fragments( client, PacketType::Manifest, transfer_id, manifest_payload, manifest_payload_per_datagram, &pacer );

  for ( uint32_t block_id = 0; block_id < send_blocks.size(); block_id++ ) {
    SendBlock& block = send_blocks[block_id];
    for ( size_t symbol = 0; symbol < block.initial_symbols; symbol++ ) {
      send_symbol( client, transfer_id, block_id, block.encoded.symbols[symbol], pacer );
    }
  }

  uint64_t last_manifest_ms = now_ms();
  uint64_t last_status_ms = 0;
  while ( true ) {
    Datagram datagram;
    if ( wait_for_datagram( client, datagram, 500 ) ) {
      if ( datagram.transfer_id != transfer_id ) {
        continue;
      }

      if ( datagram.type == PacketType::Ack ) {
        MoshCP::Ack ack;
        if ( !MoshCP::decode_ack( datagram.payload, ack ) ) {
          continue;
        }
        apply_ack_to_send_blocks( ack, send_blocks );
        const uint64_t now = now_ms();
        if ( now - last_status_ms >= 1000 ) {
          std::ostringstream msg;
          msg << "transfer " << transfer_id_hex( transfer_id ) << " receiver decoded " << ack.decoded_blocks << "/"
              << send_blocks.size() << " blocks, sent " << sent_symbol_count( send_blocks ) << " symbols, receiver rx "
              << ack.received_symbols << ", dup " << ack.duplicate_symbols << ", repairs requested "
              << ack.repair_symbols_requested;
          progress( options, msg.str() );
          last_status_ms = now;
        }
        for ( std::vector<MoshCP::RepairRequest>::const_iterator it = ack.repair_requests.begin();
              it != ack.repair_requests.end();
              ++it ) {
          if ( it->block_id >= send_blocks.size() ) {
            continue;
          }
          SendBlock& block = send_blocks[it->block_id];
          for ( uint32_t sent = 0; sent < it->extra_symbols && block.sent_symbols < block.encoded.symbols.size(); sent++ ) {
            send_symbol( client, transfer_id, it->block_id, block.encoded.symbols[block.sent_symbols], pacer );
            block.sent_symbols++;
          }
        }
        if ( all_acknowledged( send_blocks ) ) {
          progress( options, "receiver decoded transfer " + transfer_id_hex( transfer_id ) );
          return EXIT_SUCCESS;
        }
      } else if ( datagram.type == PacketType::Finish ) {
        MoshCP::Finish finish;
        if ( MoshCP::decode_finish( datagram.payload, finish ) ) {
          if ( !finish.success && !finish.message.empty() ) {
            std::cerr << "goblin-skiffcp: " << finish.message << "\n";
          } else if ( !finish.message.empty() ) {
            progress( options, finish.message );
          }
          return finish.success ? EXIT_SUCCESS : EXIT_FAILURE;
        }
      }
    }

    if ( now_ms() - last_manifest_ms >= 1000 ) {
      send_payload_fragments(
        client, PacketType::Manifest, transfer_id, manifest_payload, manifest_payload_per_datagram, &pacer );
      last_manifest_ms = now_ms();
    }
  }
}

int complete_receive( Network::Bulk::ControlClient& client,
                      uint64_t transfer_id,
                      const MoshCP::Manifest& manifest,
                      const std::string& destination,
                      const std::vector<ReceiveBlock>& blocks,
                      ReceiveStats& stats,
                      const Options& options )
{
  try {
    const std::string output_path = write_decoded_output( destination, manifest, blocks, transfer_id );
    std::vector<ReceiveBlock> ack_blocks( blocks );
    MoshCP::Ack ack = build_ack( ack_blocks, manifest, false, stats );
    send_ack( client, transfer_id, ack );
    send_finish( client, transfer_id, true, "received " + output_path );
    progress( options, "received " + output_path );
    return EXIT_SUCCESS;
  } catch ( const std::exception& e ) {
    send_finish( client, transfer_id, false, e.what() );
    std::cerr << "goblin-skiffcp: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}

int run_receive( const Options& options, int argc, char* argv[] )
{
  const std::string destination = optind < argc ? argv[optind++] : "";
  if ( optind != argc ) {
    usage( argv[0] );
  }

  Network::Bulk::ControlClient client( control_socket_path( options ) );
  std::map<uint64_t, ReceiveTransfer> transfers;

  while ( true ) {
    Datagram datagram;
    if ( wait_for_datagram( client, datagram, 250 ) ) {
      if ( options.transfer_id_set && datagram.transfer_id != options.transfer_id ) {
        continue;
      }
      if ( datagram.type == PacketType::Ack ) {
        continue;
      }
      if ( datagram.type == PacketType::Finish ) {
        MoshCP::Finish finish;
        if ( MoshCP::decode_finish( datagram.payload, finish ) && !finish.success ) {
          std::cerr << "goblin-skiffcp: sender failed: " << finish.message << "\n";
          transfers.erase( datagram.transfer_id );
          if ( !options.multi_receive ) {
            return EXIT_FAILURE;
          }
        }
        continue;
      }

      ReceiveTransfer& transfer = transfers[datagram.transfer_id];
      if ( datagram.type == PacketType::Manifest ) {
        if ( !transfer.manifest_ready ) {
          std::string manifest_payload;
          if ( transfer.assembler.add( datagram, manifest_payload ) && MoshCP::decode_manifest( manifest_payload, transfer.manifest ) ) {
            try {
              validate_receive_manifest( transfer.manifest );
              transfer.codec = FEC::make_codec( transfer.manifest.codec );
              transfer.blocks = initialize_receive_blocks( transfer.manifest );
              transfer.manifest_ready = true;
              for ( size_t i = 0; i < transfer.pending_symbols.size(); i++ ) {
                add_received_symbol(
                  transfer.pending_symbols[i], transfer.manifest, transfer.blocks, *transfer.codec, transfer.stats );
              }
              transfer.pending_symbols.clear();

              std::ostringstream msg;
              msg << "accepting transfer " << transfer_id_hex( datagram.transfer_id ) << " as "
                  << transfer.manifest.filename << ", " << transfer.manifest.blocks.size() << " blocks, "
                  << transfer.manifest.size << " wire bytes";
              if ( transfer.manifest.zstd_compressed ) {
                msg << " (" << transfer.manifest.uncompressed_size << " before zstd level "
                    << transfer.manifest.zstd_level << ")";
              }
              progress( options, msg.str() );
            } catch ( const std::exception& e ) {
              send_finish( client, datagram.transfer_id, false, e.what() );
              transfers.erase( datagram.transfer_id );
              if ( !options.multi_receive ) {
                std::cerr << "goblin-skiffcp: " << e.what() << "\n";
                return EXIT_FAILURE;
              }
              continue;
            }
          }
        }
      } else if ( datagram.type == PacketType::Symbol ) {
        if ( transfer.manifest_ready ) {
          add_received_symbol( datagram, transfer.manifest, transfer.blocks, *transfer.codec, transfer.stats );
        } else if ( transfer.pending_symbols.size() < 4096 ) {
          transfer.pending_symbols.push_back( datagram );
        }
      }

      std::map<uint64_t, ReceiveTransfer>::iterator it = transfers.find( datagram.transfer_id );
      if ( it != transfers.end() && it->second.manifest_ready && all_decoded( it->second.blocks ) ) {
        const int result = complete_receive(
          client, it->first, it->second.manifest, destination, it->second.blocks, it->second.stats, options );
        transfers.erase( it );
        if ( !options.multi_receive ) {
          return result;
        }
      }
    }

    const uint64_t now = now_ms();
    for ( std::map<uint64_t, ReceiveTransfer>::iterator it = transfers.begin(); it != transfers.end(); ++it ) {
      ReceiveTransfer& transfer = it->second;
      if ( !transfer.manifest_ready ) {
        continue;
      }
      if ( now - transfer.last_ack_ms >= 1000 ) {
        MoshCP::Ack ack = build_ack( transfer.blocks, transfer.manifest, true, transfer.stats );
        send_ack( client, it->first, ack );
        transfer.last_ack_ms = now;
      }
      if ( now - transfer.stats.last_status_ms >= 1000 ) {
        report_receive_status( options, it->first, transfer );
        transfer.stats.last_status_ms = now;
      }
    }
  }
}
}

int main( int argc, char* argv[] )
{
  try {
    if ( argc < 2 ) {
      usage( argv[0] );
    }

    const std::string command = argv[1];
    optind = 2;

    Options options;
    parse_options( argc, argv, options );

    if ( command == "send" ) {
      return run_send( options, argc, argv );
    }
    if ( command == "receive" || command == "recv" ) {
      return run_receive( options, argc, argv );
    }

    usage( argv[0] );
  } catch ( const std::exception& e ) {
    std::cerr << "goblin-skiffcp: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
