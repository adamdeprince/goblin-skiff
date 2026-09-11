/* Distributed under the GNU GPL, version 3 or later. */
#include "src/frontend/download.h"
#include "src/statesync/completeterminal.h"
#include "src/terminal/osc52.h"
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

using Download::Record;
using Clipboard::Priority;
using Network::Bulk::Datagram;
using Network::Bulk::PacketType;
namespace {
void require( bool ok, const std::string& message ) { if ( !ok ) { throw std::runtime_error( message ); } }
std::string b64( const std::string& s ) { return Terminal::base64_encode_osc52( s ); }
std::string command( const std::string& meta, const std::string& payload = "" )
{ return "777;goblin-download;v=1:" + meta + ( payload.empty() ? "" : ";" + payload ); }
std::string begin( unsigned id, const std::string& name, size_t size, bool reply = true )
{ return command( "op=begin:id=" + std::to_string( id ) + ":name=" + b64( name ) + ":size=" + std::to_string( size ) + ( reply ? ":reply=1" : "" ) ); }
struct Temp {
  std::string path {};
  Temp() { char name[] = "/tmp/goblin-download-test.XXXXXX"; require( mkdtemp( name ), "temporary test directory" ); path = name; }
  ~Temp() {
    // Every entry in this private fixture is created by this test. No globs,
    // recursive deletion, symlink following, or real Downloads are involved.
    DIR* dir = opendir( path.c_str() );
    if ( dir ) { while ( auto* entry = readdir( dir ) ) { if ( std::string( entry->d_name ) != "." && std::string( entry->d_name ) != ".." ) { unlinkat( dirfd( dir ), entry->d_name, 0 ); } } closedir( dir ); }
    rmdir( path.c_str() );
  }
  std::set<std::string> names() const {
    std::set<std::string> out;
    DIR* dir = opendir( path.c_str() ); require( dir, "list test directory" );
    while ( auto* entry = readdir( dir ) ) { if ( std::string( entry->d_name ) != "." && std::string( entry->d_name ) != ".." ) { out.insert( entry->d_name ); } }
    closedir( dir ); return out;
  }
};
std::string read_file( const std::string& path )
{ std::ifstream file( path, std::ios::binary ); return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() }; }
Record record( Record::Kind kind, uint64_t token ) { Record r; r.set_kind( kind ); r.set_token( token ); return r; }
Record start( uint64_t token, const std::string& name, uint64_t size )
{ auto r = record( Record::BEGIN, token ); r.set_name( name ); r.set_size( size ); return r; }
Record data( uint64_t token, const std::string& bytes )
{ auto r = record( Record::DATA, token ); r.set_data( bytes ); return r; }
Record finish( uint64_t token, const std::string& bytes )
{ auto r = record( Record::END, token ); r.set_checksum( crc32( 0, reinterpret_cast<const Bytef*>( bytes.data() ), bytes.size() ) ); return r; }

void parser()
{
  Download::Command c;
  require( Download::parse( begin( 1, "résumé.pdf", 0 ), c ) && c.id == 1 && c.name == "résumé.pdf", "UTF-8 name and empty file" );
  for ( const auto& name : { "../escape", "/absolute", "sub/file", "sub\\file", ".", "..", "bad\nname", "\xc0\xaf",
                           "\xd8\x9c", "\xe2\x80\x8e", "\xe2\x80\x8f", "\xe2\x80\xae", "\xe2\x81\xa6" } ) {
    require( !Download::parse( begin( 1, name, 0 ), c ), "unsafe filename rejected" );
  }
  require( !Download::parse( begin( 1, std::string( "null\0file", 9 ), 0 ), c ), "NUL name" );
  require( Download::parse( begin( 1, std::string( 220, 'n' ), 0 ), c )
             && !Download::parse( begin( 1, std::string( 221, 'n' ), 0 ), c ), "filename length limit" );
  require( !Download::parse( begin( 0, "x", 0 ), c ), "zero ID" );
  require( !Download::parse( begin( 1, "x", Download::MAX_SIZE + 1 ), c ), "file size cap" );
  require( !Download::parse( command( "op=begin:id=1:name=eA==:size=184467440737095516160:reply=1" ), c ), "size overflow" );
  require( !Download::parse( command( "op=end:id=1:id=2" ), c ), "duplicate metadata" );
  require( Download::parse( command( "op=data:id=1", b64( std::string( 4096, '\0' ) ) ), c ) && c.data.size() == 4096, "binary 4 KiB chunk" );
  require( !Download::parse( command( "op=data:id=1", b64( std::string( 4097, '\0' ) ) ), c ), "oversized chunk" );
  require( !Download::parse( command( "op=data:id=1", "!!!!" ), c ), "malformed base64" );
  const auto body = command( "op=query:id=42" );
  const auto sequence = "\033]" + body + "\033\\";
  for ( size_t step : { 1U, 3U, 17U, 4096U } ) {
    Terminal::Complete source( 80, 24 ), blank( 80, 24 );
    for ( size_t at = 0; at < sequence.size(); at += step ) { source.act( sequence.substr( at, step ) ); }
    auto events = source.take_parser_download_events();
    require( events == std::vector<std::string> { body }, "split OSC collected once" );
    require( source.take_parser_download_events().empty(), "one-shot collector drains" );
    require( source.diff_from( blank ).find( "goblin-download" ) == std::string::npos, "no download in screen diff" );
  }
  for ( const auto& canceled : { "\030", "\032", "\033[0m", "\033]0;other\007" } ) {
    Terminal::Complete source( 80, 24 );
    source.act( "\033]" + body + canceled );
    require( source.take_parser_download_events().empty(), "aborted OSC cannot trigger a download" );
  }
  Terminal::Complete deferred( 80, 24 );
  deferred.act( "\033]" + body + "\033" );
  require( deferred.take_parser_download_events().empty(), "wait for the complete ST terminator" );
  deferred.act( "\\" );
  require( deferred.take_parser_download_events().size() == 1, "split ST completes exactly once" );
}

void sink()
{
  Temp temp;
  const std::string bytes( "binary\0data\xff", 12 );
  Download::Sink sink( temp.path );
  require( sink.handle( start( 1, "report.bin", bytes.size() ) ).status() == "progress", "begin local file" );
  require( !temp.names().count( "report.bin" ), "final filename not exposed early" );
  require( sink.handle( data( 1, bytes ) ).status() == "progress", "write binary" );
  auto done = sink.handle( finish( 1, bytes ) );
  require( done.status() == "saved" && done.name() == "report.bin" && read_file( temp.path + "/report.bin" ) == bytes, "atomic save" );
  struct stat info;
  require( stat( ( temp.path + "/report.bin" ).c_str(), &info ) == 0 && ( info.st_mode & 0777 ) == 0600, "private file permissions" );
  require( sink.handle( start( 2, "report.bin", 0 ) ).status() == "progress", "begin collision" );
  done = sink.handle( finish( 2, "" ) );
  require( done.status() == "saved" && done.name() == "report.bin.1" && read_file( temp.path + "/report.bin" ) == bytes, "never overwrite" );
  require( symlink( "report.bin", ( temp.path + "/link" ).c_str() ) == 0, "test symlink" );
  sink.handle( start( 3, "link", 0 ) );
  require( sink.handle( finish( 3, "" ) ).name() == "link.1", "existing symlink cannot redirect writes" );
  sink.handle( start( 4, "bad.bin", 1 ) );
  require( sink.handle( finish( 4, "" ) ).status() == "error" && !temp.names().count( "bad.bin" ), "short file never published" );
  sink.handle( start( 5, "bad.bin", 1 ) ); sink.handle( data( 5, "x" ) );
  require( sink.handle( finish( 5, "y" ) ).status() == "error", "checksum mismatch" );
  sink.handle( start( 6, "canceled", 9 ) );
  require( sink.handle( record( Record::CANCEL, 6 ) ).status() == "canceled", "cancel partial" );
  require( sink.handle( start( 6, "replay", 0 ) ).status() == "error", "old wire token never replayed" );
  {
    Download::Sink abandoned( temp.path ); abandoned.handle( start( 1, "abandoned", 5 ) );
  }
  require( temp.names() == std::set<std::string> { "report.bin", "report.bin.1", "link", "link.1" }, "all incomplete staging files cleaned" );
  Download::Sink missing( temp.path + "/not-created" );
  require( missing.handle( start( 1, "x", 0 ) ).status() == "error", "missing destination reports error" );
}

void limits()
{
  Temp temp;
  {
    Download::Sink sink( temp.path );
    for ( unsigned id = 1; id <= Download::MAX_TRANSFERS; id++ ) {
      require( sink.handle( start( id, "file", 0 ) ).status() == "progress", "four concurrent sink jobs accepted" );
    }
    require( sink.handle( start( 5, "overflow", 0 ) ).status() == "error" && temp.names().size() == 4,
             "fifth sink job rejected without creating a file" );
    sink.handle( record( Record::CANCEL, 1 ) );
    require( sink.handle( start( 6, "replacement", 0 ) ).status() == "progress", "cancel releases sink job slot" );
  }
  require( temp.names().empty(), "concurrent partials cleaned" );
  {
    Download::Sink sink( temp.path );
    require( sink.handle( start( 1, "full", Download::MAX_SIZE ) ).status() == "progress", "maximum declared size accepted" );
    require( sink.handle( start( 2, "extra", 1 ) ).status() == "error", "aggregate sink byte limit" );
    sink.handle( record( Record::CANCEL, 1 ) );
    require( sink.handle( start( 3, "replacement", Download::MAX_SIZE ) ).status() == "progress", "cancel releases reserved bytes" );
  }
  require( temp.names().empty(), "reserved but unwritten files cleaned" );
  {
    Download::Sink sink( temp.path );
    const std::string name( 220, 'n' );
    sink.handle( start( 1, name, 0 ) );
    require( sink.handle( finish( 1, "" ) ).status() == "saved", "long basename saved" );
    sink.handle( start( 2, name, 0 ) );
    const auto saved = sink.handle( finish( 2, "" ) );
    Record decoded;
    require( saved.status() == "saved" && saved.name() == name + ".1"
               && Download::decode_record( Download::encode_record( saved ), decoded )
               && Download::safe_basename( saved.name() ), "collision suffix fits and is valid in completion reply" );
  }
  {
    Download::Sink closed_session( temp.path, Crypto::Mode::LegacyOCB, []() { return false; } );
    closed_session.handle( start( 1, "closed", 0 ) );
    require( closed_session.handle( finish( 1, "" ) ).status() == "error"
               && temp.names().size() == 2 && !temp.names().count( "closed" ), "session closure prevents final publication" );
  }
  Download::Sender sender;
  sender.set_enabled( true );
  for ( unsigned id = 1; id <= Download::MAX_TRANSFERS; id++ ) { sender.submit( begin( id, "file", 0 ), 0 ); }
  require( sender.take_replies().empty(), "four sender jobs accepted" );
  sender.submit( begin( 5, "overflow", 0 ), 0 );
  require( sender.take_replies().find( "status=error" ) != std::string::npos, "fifth sender job rejected" );
  sender.submit( command( "op=cancel:id=1" ), 0 ); sender.take_replies();
  sender.submit( begin( 6, "replacement", 0 ), 0 );
  require( sender.take_replies().empty(), "cancel releases sender job slot" );
  Download::Sender sized;
  sized.set_enabled( true );
  sized.submit( begin( 1, "full", Download::MAX_SIZE ), 0 );
  require( sized.take_replies().empty(), "maximum sender declared size accepted" );
  sized.submit( begin( 2, "extra", 1 ), 0 );
  require( sized.take_replies().find( "status=error" ) != std::string::npos, "aggregate sender byte limit" );
  sized.submit( command( "op=cancel:id=1" ), 0 ); sized.take_replies();
  sized.submit( begin( 3, "replacement", Download::MAX_SIZE ), 0 );
  require( sized.take_replies().empty(), "cancel releases sender reserved bytes" );
}

std::string random_data( size_t size )
{
  uint32_t state = 0x91fadcU; std::string bytes;
  while ( bytes.size() < size ) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; bytes += char( state ); }
  return bytes;
}

void channel_loss()
{
  Clipboard::Channel tx( true ), rx( true );
  const std::string bytes = random_data( 4096 );
  std::vector<std::string> expected;
  for ( unsigned i = 1; i <= 20; i++ ) { expected.push_back( Download::encode_record( data( i, bytes ) ) ); }
  require( tx.enqueue( expected, Priority::Background ), "queue binary transfer" );
  Datagram packet;
  for ( uint64_t now = 0; now < 100; now++ ) {
    require( !tx.take_packet( Priority::Interactive, now, 200, packet ), "bulk data never uses fast path" );
  }
  std::vector<std::string> received;
  unsigned packets = 0, acknowledgments = 0, repairs = 0;
  std::vector<Datagram> delayed;
  bool saw_zstd = false;
  for ( uint64_t now = 100; now < 30000 && !tx.idle(); now += 5 ) {
    if ( tx.take_packet( Priority::Background, now, 200, packet ) ) {
      packets++;
      require( packet.type == PacketType::DownloadSymbol && packet.transfer_id == 2, "dedicated download packet type/lane" );
      require( Network::Bulk::encode_datagram( packet ).size() + 44 <= 500, "fits fallback MTU including FIPS packet overhead" );
      const unsigned n = ( uint8_t( packet.payload[0] ) << 8 ) | uint8_t( packet.payload[1] );
      const unsigned k = ( n + 383 ) / 384;
      if ( packet.symbol_id >= k ) { repairs++; }
      if ( packet.symbol_id == 0 ) {
        require( packet.payload[2] == 1 && uint8_t( packet.payload[3] ) == 0x28 && uint8_t( packet.payload[4] ) == 0xb5,
                 "even incompressible download data uses a zstd frame" );
        saw_zstd = true;
      }
      if ( packets % 7 == 0 ) { continue; }
      if ( packets % 5 == 0 ) { delayed.push_back( packet ); }
      else { rx.receive( packet ); rx.receive( packet ); }
    }
    if ( now % 50 == 0 ) { std::reverse( delayed.begin(), delayed.end() ); for ( const auto& p : delayed ) { rx.receive( p ); } delayed.clear(); }
    std::string out;
    if ( rx.take_output( out ) ) { received.push_back( out ); }
    if ( rx.take_packet( Priority::Interactive, now, 200, packet ) ) {
      require( packet.type == PacketType::DownloadAck, "bulk acknowledgment is fast" );
      if ( ++acknowledgments % 3 ) { tx.receive( packet ); }
    }
  }
  require( received == expected && tx.idle() && repairs && saw_zstd, "loss, duplicates, reordering, lost ACKs recover exactly once" );
}

void sender_and_loss()
{
  Download::Sender tx;
  tx.submit( command( "op=query:id=1" ), 0 );
  require( tx.take_replies().find( "status=unsupported" ) != std::string::npos, "old/disabled peer is not advertised" );
  tx.set_enabled( true );
  tx.submit( command( "op=query:id=2" ), 0 );
  require( tx.take_replies().find( "status=supported:max_chunk=4096" ) != std::string::npos, "capability response" );
  Temp temp;
  Download::Sink sink( temp.path );
  Clipboard::Channel receiver( true );
  const auto bytes = random_data( 30 * 1024 );
  tx.submit( begin( 3, "alpine.bin", bytes.size() ), 0 );
  for ( size_t at = 0; at < bytes.size(); at += 4096 ) { tx.submit( command( "op=data:id=3", b64( bytes.substr( at, 4096 ) ) ), 0 ); }
  tx.submit( command( "op=end:id=3" ), 0 );
  require( tx.take_replies().find( "status=queued" ) != std::string::npos && !temp.names().count( "alpine.bin" ), "queued is not saved" );
  unsigned sequence = 0;
  std::string replies;
  for ( uint64_t now = 0; now < 20000; now += 5 ) {
    Datagram p;
    if ( tx.channel.take_packet( Priority::Interactive, now, 100, p ) ) { receiver.receive( p ); }
    if ( tx.channel.take_packet( Priority::Background, now, 100, p ) && ++sequence % 9 ) { receiver.receive( p ); }
    std::string out;
    if ( receiver.take_output( out ) ) {
      Record r; require( Download::decode_record( out, r ), "download record decodes" );
      auto status = sink.handle( r );
      if ( r.kind() == Record::BEGIN && status.status() == "progress" ) { status.set_status( "approved" ); }
      if ( status.status() != "progress" ) { receiver.enqueue( { Download::encode_record( status ) }, Priority::Interactive ); }
    }
    if ( receiver.take_packet( Priority::Interactive, now, 100, p ) ) { tx.channel.receive( p ); }
    tx.tick( now ); replies += tx.take_replies();
    if ( replies.find( "status=saved" ) != std::string::npos && tx.channel.idle() && receiver.idle() ) { break; }
  }
  require( replies.find( "status=saved:name=" + b64( "alpine.bin" ) ) != std::string::npos, "saved only after receiver completion" );
  require( read_file( temp.path + "/alpine.bin" ) == bytes, "30 KB download exact bytes" );
  tx.submit( begin( 4, "bad", 9 ), 0 ); tx.submit( command( "op=data:id=4", b64( "short" ) ), 0 ); tx.submit( command( "op=end:id=4" ), 0 );
  require( tx.take_replies().find( "status=error" ) != std::string::npos, "size mismatch replies" );
  tx.submit( begin( 5, "silent", 1, false ), 0 ); tx.submit( command( "op=cancel:id=5" ), 0 );
  require( tx.take_replies().empty(), "fire-and-forget never injects a terminal reply" );
  tx.submit( begin( 6, "incomplete", 1 ), 0 ); tx.tick( 120001 );
  require( tx.take_replies().find( "status=error" ) != std::string::npos, "abandoned OSC transaction expires" );
}

void consent()
{
  Temp temp;
  Download::Receiver receiver( temp.path, Crypto::Mode::LegacyOCB );
  Clipboard::Channel peer( true );
  uint64_t now = 0;
  std::vector<Record> responses;
  auto send = [&]( const Record& r ) {
    require( peer.enqueue( { Download::encode_record( r ) }, Priority::Background ), "queue consent test record" );
  };
  auto pump = [&]( unsigned count ) {
    for ( unsigned i = 0; i < count; i++, now += 5 ) {
      Datagram packet;
      for ( auto priority : { Priority::Interactive, Priority::Background } ) {
        if ( peer.take_packet( priority, now, 100, packet ) ) { receiver.channel.receive( packet ); }
        if ( receiver.channel.take_packet( priority, now, 100, packet ) ) { peer.receive( packet ); }
      }
      receiver.tick( now );
      std::string bytes;
      while ( peer.take_output( bytes ) ) {
        Record r; require( Download::decode_record( bytes, r ), "consent response decodes" ); responses.push_back( r );
      }
      if ( receiver.fd() >= 0 ) { usleep( 1000 ); }
    }
  };
  auto replied = [&]( uint64_t token, const std::string& state ) {
    return std::any_of( responses.begin(), responses.end(), [&]( const Record& r ) {
      return r.token() == token && r.status() == state;
    } );
  };
  auto await_reply = [&]( uint64_t token, const std::string& state ) {
    // A worker closes inherited descriptors before opening the destination.
    // Large container RLIMIT_NOFILE values can take longer than a fixed
    // 200 ms pump. Wait for its actual response with a real, bounded deadline;
    // the permission assertions below are unchanged, including error cases.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while ( !replied( token, state ) && !replied( token, "error" ) ) {
      require( std::chrono::steady_clock::now() < deadline, "consent worker response deadline" );
      pump( 20 );
    }
  };
  send( start( 1, "first", 0 ) ); send( start( 2, "second", 0 ) ); pump( 200 );
  Record offer;
  require( receiver.pending( offer ) && offer.token() == 1 && offer.name() == "first", "pending metadata available to local UI" );
  require( receiver.fd() < 0 && temp.names().empty() && responses.empty(), "unapproved offer creates no worker or file and grants no permission" );
  receiver.decide( 2, true ); await_reply( 2, "approved" );
  require( replied( 2, "approved" ) && !replied( 1, "approved" ), "approval is scoped to one offer" );
  send( finish( 2, "" ) ); await_reply( 2, "saved" );
  require( replied( 2, "saved" ) && temp.names() == std::set<std::string> { "second" }, "only approved file published" );
  receiver.decide( 1, true ); await_reply( 1, "approved" );
  send( finish( 1, "" ) ); await_reply( 1, "saved" );
  require( replied( 1, "saved" ) && temp.names() == std::set<std::string> { "first", "second" }, "out-of-order approvals preserve worker replay protection" );
  send( start( 1, "replay", 0 ) ); pump( 200 );
  require( !receiver.pending( offer ), "completed offer cannot be replayed" );
  send( start( 3, "declined", 1 ) ); pump( 200 ); receiver.decide( 3, false ); pump( 200 );
  send( data( 3, "x" ) ); send( finish( 3, "x" ) ); pump( 200 );
  require( replied( 3, "error" ) && !replied( 3, "approved" ) && temp.names().size() == 2, "declined offer rejects late payload" );
  send( start( 4, "expired", 0 ) ); pump( 200 ); now += 120001; pump( 200 );
  receiver.decide( 4, true ); pump( 200 );
  require( replied( 4, "error" ) && !receiver.pending( offer ) && !replied( 4, "approved" ), "expired approval cannot be revived" );
  send( start( 5, "canceled", 0 ) ); pump( 200 ); send( record( Record::CANCEL, 5 ) ); pump( 200 );
  receiver.decide( 5, true ); pump( 200 );
  require( !receiver.pending( offer ) && !replied( 5, "approved" ) && temp.names().size() == 2, "remote cancellation revokes pending offer" );
}

void nested_forwarding()
{
  Temp intermediate, destination;
  Download::Sender origin, outer_sender;
  origin.set_enabled( true ); outer_sender.set_enabled( true );
  Download::Receiver middle( intermediate.path, Crypto::Mode::LegacyOCB ), outer( destination.path, Crypto::Mode::LegacyOCB );
  middle.enable_forwarding( 0 );
  const auto bytes = random_data( 12 * 1024 );
  origin.submit( begin( 1, "nested.bin", bytes.size() ), 0 );
  for ( size_t at = 0; at < bytes.size(); at += 4096 ) { origin.submit( command( "op=data:id=1", b64( bytes.substr( at, 4096 ) ) ), 0 ); }
  origin.submit( command( "op=end:id=1" ), 0 );
  std::string status;
  unsigned packets = 0, dropped = 0;
  uint64_t now = 0;
  auto link = [&]( Clipboard::Channel& source, Clipboard::Channel& target ) {
    for ( auto priority : { Priority::Interactive, Priority::Background } ) {
      Datagram p;
      if ( source.take_packet( priority, now, 100, p ) ) {
        if ( ++packets % 7 == 0 ) { dropped++; }
        else { target.receive( p ); target.receive( p ); }
      }
    }
  };
  auto pump = [&]() {
    link( origin.channel, middle.channel ); link( middle.channel, origin.channel );
    link( outer_sender.channel, outer.channel ); link( outer.channel, outer_sender.channel );
    middle.tick( now ); outer.tick( now ); outer_sender.tick( now ); origin.tick( now );
    // The intermediate client's terminal is the outer server's PTY. Replies
    // from that PTY terminate at the intermediate client, not in the app.
    for ( unsigned n = 0; n < 2; n++ ) {
      const auto output = middle.take_terminal_output();
      if ( output.empty() ) { break; }
      require( output.substr( 0, 2 ) == "\033]" && output.substr( output.size() - 2 ) == "\033\\", "nested output is an atomic OSC" );
      outer_sender.submit( output.substr( 2, output.size() - 4 ), now );
    }
    require( middle.filter_input( outer_sender.take_replies(), now ).empty(), "nested status never reaches remote keyboard input" );
    require( middle.fd() < 0, "nested handoff never spawns a filesystem worker" );
    Record hidden; require( !middle.pending( hidden ), "nested handoff never prompts in intermediate terminal" );
    status += origin.take_replies();
    if ( outer.fd() >= 0 ) { usleep( 1000 ); }
    now += 5;
  };
  Record offer;
  while ( now < 3000 && !outer.pending( offer ) ) { pump(); }
  require( outer.pending( offer ) && offer.name() == "nested.bin", "offer reaches outermost Downloads handler" );
  for ( unsigned i = 0; i < 100; i++ ) { pump(); }
  require( intermediate.names().empty() && destination.names().empty() && outer.fd() < 0,
           "two FEC hops awaiting consent create no files on either host" );
  outer.decide( offer.token(), true );
  while ( now < 25000 && status.find( "status=saved" ) == std::string::npos ) { pump(); }
  require( dropped > 0 && status.find( "status=saved" ) != std::string::npos
             && read_file( destination.path + "/nested.bin" ) == bytes && intermediate.names().empty(),
           "nested two-hop FEC loss recovery, end-to-end consent and completion without an intermediate copy" );
}

void worker()
{
  Temp temp;
  struct Resume {
    pid_t child = -1;
    ~Resume() { if ( child > 0 ) { kill( child, SIGCONT ); } }
  } resume;
  {
    Download::Writer writer( temp.path, Crypto::Mode::LegacyOCB );
    require( writer.submit( start( 1, "worker", 1 ), 0 ), "submit asynchronous worker" );
    const pid_t child = writer.pid(); require( child > 0, "worker process exists" );
    Record reply;
    for ( uint64_t now = 0; now < 5000 && !writer.pop( reply ); now += 5 ) { writer.tick( now ); usleep( 1000 ); }
    require( reply.status() == "progress", "worker accepts begin" );
    require( kill( child, SIGSTOP ) == 0, "stop writer for nonblocking test" );
    resume.child = child;
    require( writer.submit( data( 1, "x" ), 10 ), "queue write to stopped worker" );
    const auto started = std::chrono::steady_clock::now();
    for ( uint64_t now = 10; now < 1000; now++ ) { writer.tick( now ); }
    require( std::chrono::steady_clock::now() - started < std::chrono::seconds( 1 ), "blocked storage never blocks event loop" );
    writer.tick( 30011 );
    require( writer.pop( reply ) && reply.status() == "error" && writer.fd() < 0, "worker timeout closes IPC" );
    kill( child, SIGCONT );
    for ( uint64_t now = 30012; now < 35000 && writer.pid() > 0; now += 5 ) { writer.tick( now ); usleep( 1000 ); }
    require( writer.pid() < 0, "worker reaped after disconnect cleanup" ); resume.child = -1;
  }
  require( temp.names().empty(), "worker disconnect removes partial file" );
}
}

int main()
{
  try { parser(); sink(); limits(); channel_loss(); sender_and_loss(); consent(); nested_forwarding(); worker(); }
  catch ( const std::exception& e ) { std::cerr << "download: " << e.what() << '\n'; return 1; }
  std::cout << "Download parser, consent, safety, binary zstd/FEC, loss recovery and worker tests passed\n";
}
