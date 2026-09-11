/* Distributed under the GNU GPL, version 3 or later. */
#include "config.h"
#include "src/frontend/filetransfer.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <unistd.h>

namespace {
using Files::Command;
using Files::Record;
using Clipboard::Priority;
using Network::Bulk::Datagram;
using Network::Bulk::PacketType;
namespace fs = std::filesystem;
void require( bool ok, const std::string& text ) { if ( !ok ) { throw std::runtime_error( text ); } }
uint64_t clock_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}
struct Temp {
  std::string path;
  Temp() {
    char pattern[] = "/tmp/goblin-file-test.XXXXXX";
    require( mkdtemp( pattern ), "create private fixture" ); path = fs::canonical( pattern ).string();
    fs::create_directory( path + "/local" ); fs::create_directory( path + "/remote" );
  }
  ~Temp() { std::error_code error; fs::remove_all( path, error ); }
};
void write( const std::string& path, const std::string& bytes )
{
  std::ofstream file( path, std::ios::binary ); file.write( bytes.data(), bytes.size() ); require( bool( file ), "write fixture" );
}
std::string read( const std::string& path )
{
  std::ifstream file( path, std::ios::binary ); require( bool( file ), "read result: " + path );
  return { std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() };
}
std::string noise( size_t length )
{
  uint32_t state = 987654321; std::string data( length, '\0' );
  for ( auto& c : data ) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; c = char( state ); }
  return data;
}

struct Link {
  Files::Endpoint client, server;
  explicit Link( Crypto::Mode mode = Crypto::Mode::LegacyOCB ) : client( false, true, mode ), server( true, true, mode ) {}
  uint64_t now = 1000;
  unsigned packets[2] = {}, symbols[2] = {}, acks[2] = {}, drops[2] = {};
  std::deque<std::pair<uint64_t, Datagram>> delayed[2];
  bool loss = true;
  void packets_to( Files::Endpoint& from, Files::Endpoint& to, unsigned direction ) {
    for ( const auto priority : { Priority::Interactive, Priority::Background } ) {
      Datagram packet;
      if ( !from.take_packet( priority, now, 100, packet ) ) { continue; }
      require( packet.type == PacketType::FileSymbol || packet.type == PacketType::FileAck, "dedicated file packet types" );
      require( priority == Priority::Interactive ? packet.type == PacketType::FileAck : packet.type == PacketType::FileSymbol,
               "signatures and file bytes stay on background FEC; ACKs interactive" );
      if ( packet.type == PacketType::FileSymbol ) { ++symbols[direction]; } else { ++acks[direction]; }
      Datagram decoded;
      require( Network::Bulk::decode_datagram( Network::Bulk::encode_datagram( packet ), decoded ), "wire packet roundtrip" );
      const unsigned n = ++packets[direction];
      if ( loss && n % 11 == 0 ) { ++drops[direction]; continue; }
      if ( loss && n % 7 == 0 ) { delayed[direction].push_back( { now + 100, decoded } ); }
      else { require( to.channel.receive( decoded ), "receive file packet" ); }
      if ( loss && n % 17 == 0 ) { require( to.channel.receive( decoded ), "duplicate file packet" ); }
    }
    while ( !delayed[direction].empty() && delayed[direction].front().first <= now ) {
      to.channel.receive( delayed[direction].front().second ); delayed[direction].pop_front();
    }
  }
  void tick( bool network = true ) {
    now += 20; client.tick( now ); server.tick( now );
    if ( network ) {
      Command c;
      while ( client.take_control( c ) ) { server.receive_control( c, now ); }
      while ( server.take_control( c ) ) { client.receive_control( c, now ); }
      packets_to( client, server, 0 ); packets_to( server, client, 1 );
    }
    require( client.channel.pending_bytes() <= 256 * 1024 + Files::MAX_RECORD, "bounded client queue" );
    require( server.channel.pending_bytes() <= 256 * 1024 + Files::MAX_RECORD, "bounded server queue" );
    usleep( 200 );
  }
  Command finish( size_t jobs = 1, bool failure = false ) {
    const auto deadline = clock_ms() + 60000;
    while ( true ) {
      tick();
      require( clock_ms() < deadline, "transfer deadline; last status: " + ( client.jobs().empty() ? "none" : client.jobs().back().status() ) );
      bool done = client.jobs().size() == jobs;
      for ( const auto& job : client.jobs() ) { done = done && job.done(); }
      if ( done && client.pid() < 0 && server.pid() < 0 && client.channel.idle() && server.channel.idle() ) {
        const auto result = client.jobs().back();
        require( result.failed() == failure, "unexpected outcome: " + result.status() ); return result;
      }
    }
  }
};

void copies()
{
  Temp temp; const auto local = temp.path + "/local", remote = temp.path + "/remote";
  const auto original = noise( 128 * 1024 ); auto changed = original; changed.replace( 48000, 128, 128, 'z' );
  write( local + "/large.bin", changed ); write( remote + "/large.bin", original );
  Link link;
  require( link.client.queue( true, local + "/large.bin", remote, "large.bin" ), "queue delta upload" );
  const auto uploaded = link.finish();
  require( read( remote + "/large.bin" ) == changed, "delta upload exact bytes" );
  require( uploaded.signatures() > 12 && uploaded.deltas() > 0 && uploaded.payload() < 4096, "existing file uses a small delta" );
  require( link.symbols[0] && link.symbols[1] && link.acks[0] && link.acks[1] && link.drops[0] && link.drops[1], "loss and FEC in both directions" );
  std::cout << "128 KiB edited file: " << uploaded.signatures() << " signature bytes, " << uploaded.deltas()
            << " delta bytes; recovered loss, reordering and duplication both ways\n";

  fs::create_directories( remote + "/folder/nested/empty" ); fs::create_directories( local + "/folder/nested" );
  write( remote + "/folder/nested/same.bin", changed ); write( local + "/folder/nested/same.bin", original );
  write( remote + "/folder/new.txt", "new file\n" ); write( local + "/folder/keep.txt", "not a mirror" );
  write( remote + "/folder/zero", "" );
  require( link.client.queue( false, remote + "/folder", local, "folder" ), "queue recursive download" );
  // A disconnect must not throw away in-flight records or virtual worker state.
  for ( unsigned i = 0; i < 100; ++i ) { link.tick(); }
  const auto pid = link.client.pid();
  for ( unsigned i = 0; i < 100; ++i ) { link.tick( false ); }
  require( pid == link.client.pid(), "worker survives disconnection" );
  const auto downloaded = link.finish( 2 );
  require( downloaded.files() == 3 && downloaded.deltas() > 0 && downloaded.signatures() > 0, "recursive mix of new and delta files" );
  require( read( local + "/folder/nested/same.bin" ) == changed && read( local + "/folder/new.txt" ) == "new file\n", "recursive download bytes" );
  require( fs::is_directory( local + "/folder/nested/empty" ) && read( local + "/folder/zero" ).empty(), "empty files and directories" );
  require( read( local + "/folder/keep.txt" ) == "not a mirror", "extra destination file kept" );

  write( local + "/one", "one" ); write( local + "/two", "two" );
  require( link.client.queue( true, local + "/one", remote, "one" ) && link.client.queue( true, local + "/two", remote, "two" ), "queue multiple transfers" );
  link.finish( 4 ); require( read( remote + "/one" ) == "one" && read( remote + "/two" ) == "two", "queued files complete" );
  require( link.client.queue( true, local + "/one", remote, "one" ), "queue identical file" );
  link.finish( 5 ); require( read( remote + "/one" ) == "one", "identical tiny file fallback" );
}

void pacing()
{
  Clipboard::Channel legacy( Clipboard::Channel::Records::Files ), paced( Clipboard::Channel::Records::Files );
  const auto data = noise( Files::CHUNK );
  require( legacy.enqueue( { data }, Priority::Background ) && paced.enqueue( { data }, Priority::Background ), "enqueue pacing fixtures" );
  Datagram packet;
  require( legacy.take_packet( Priority::Background, 1000, 100, packet ), "legacy first symbol" );
  require( !legacy.take_packet( Priority::Background, 1001, 100, packet ) && legacy.wait_time( 1001, true ) == 19,
           "peers without adaptive pacing retain the safe fixed delay" );
  paced.set_external_pacing( true );
  require( paced.take_packet( Priority::Background, 1000, 100, packet ) && paced.wait_time( 1000, true ) == 0
             && paced.take_packet( Priority::Background, 1000, 100, packet ),
           "connection-paced files have no duplicate 20ms throttle" );
  paced.set_external_pacing( false );
  require( paced.take_packet( Priority::Background, 1000, 100, packet ) && paced.wait_time( 1000, true ) == 20,
           "fallback restores legacy pacing" );
}

void safety()
{
  Temp temp; const auto local = temp.path + "/local", remote = temp.path + "/remote";
  write( local + "/file", noise( 512 * 1024 ) ); write( remote + "/original", "preserve me" );
  fs::create_symlink( remote + "/original", remote + "/file" );
  Link rejected;
  require( rejected.client.queue( true, local + "/file", remote, "file" ), "queue destination symlink test" );
  rejected.finish( 1, true ); require( read( remote + "/original" ) == "preserve me", "never follow destination symlinks" );
  require( fs::is_symlink( remote + "/file" ), "symlink preserved" );
  fs::create_symlink( local + "/file", local + "/link" );
  Link source;
  require( source.client.queue( true, local + "/link", remote, "link" ), "queue source symlink test" );
  source.finish( 1, true ); require( !fs::exists( remote + "/link" ), "source symlink not copied" );

  Link cancel;
  require( cancel.client.queue( true, local + "/file", remote, "file" ), "queue cancellation" );
  cancel.tick( false ); const auto pid = cancel.client.pid(); require( pid > 0, "worker spawned" );
  require( kill( pid, SIGSTOP ) == 0, "stop only test worker" );
  const auto start = clock_ms();
  for ( unsigned i = 0; i < 1000; ++i ) { cancel.client.tick( cancel.now + i ); }
  require( clock_ms() - start < 1000 && cancel.client.pid() == pid, "stalled worker cannot block session or spawn more workers" );
  cancel.client.cancel(); cancel.finish( 1, true );
  require( !cancel.client.queue( true, local + "/file", remote, "../escape" ), "reject traversal in selected name" );
  Command unauthorized; unauthorized.set_kind( Command::START ); unauthorized.set_id( 100 ); unauthorized.set_send( false );
  unauthorized.set_path( local ); unauthorized.set_name( "unsolicited" );
  cancel.client.receive_control( unauthorized, cancel.now ); cancel.tick();
  require( cancel.client.pid() < 0 && !fs::exists( local + "/unsolicited" ), "server cannot authorize local copy" );
}

void invalid_peer( unsigned scenario )
{
  Temp temp; const auto destination = temp.path + "/remote";
  const bool new_file = scenario == 4;
  if ( !new_file ) { write( destination + "/file", "old" ); }
  Files::Endpoint receiver( true, true, Crypto::Mode::LegacyOCB );
  Clipboard::Channel peer( Clipboard::Channel::Records::Files );
  Command start; start.set_kind( Command::START ); start.set_id( 1 ); start.set_send( false );
  start.set_path( destination ); start.set_name( "file" ); receiver.receive_control( start, 1000 );
  const auto record = []( Record::Kind kind ) {
    Record r; r.set_kind( kind ); r.set_job( 1 ); r.set_serial( 1 ); return r;
  };
  auto offer = record( Record::OFFER ); offer.set_path( scenario == 0 ? "../outside" : "file" ); offer.set_size( 3 ); offer.set_mode( 0600 );
  require( peer.enqueue( { offer.SerializeAsString() }, Priority::Background ), "malicious offer" );
  bool failed = false, sent = false;
  const auto deadline = clock_ms() + 10000;
  for ( uint64_t now = 1000; clock_ms() < deadline; now += 20 ) {
    receiver.tick( now ); Command control;
    while ( receiver.take_control( control ) ) { failed = failed || control.failed(); }
    for ( const auto priority : { Priority::Interactive, Priority::Background } ) {
      Datagram p;
      if ( peer.take_packet( priority, now, 100, p ) ) { receiver.channel.receive( p ); }
      if ( receiver.take_packet( priority, now, 100, p ) ) { peer.receive( p ); }
    }
    std::string body;
    while ( peer.take_output( body ) ) {
      Record r; require( r.ParseFromString( body ), "signature output" );
      if ( r.kind() != Record::SIGNATURE_END || sent ) { continue; }
      sent = true;
      if ( scenario == 3 || scenario == 4 ) { write( destination + "/file", "user edit" ); }
      auto data = record( Record::DATA ); data.set_data( scenario == 2 ? "truncated delta" : "abc" ); data.set_delta( scenario == 2 );
      auto end = record( Record::END ); end.set_delta( scenario == 2 );
      const unsigned char sha256[] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
      end.set_sha256( scenario == 1 ? std::string( 32, '\0' ) : std::string( reinterpret_cast<const char*>( sha256 ), 32 ) );
      require( peer.enqueue( { data.SerializeAsString(), end.SerializeAsString() }, Priority::Background ), "invalid transfer records" );
    }
    if ( failed && receiver.pid() < 0 && receiver.channel.idle() ) { break; }
    usleep( 200 );
  }
  require( failed, "reject traversal, wrong SHA256, malformed delta or concurrent destination change" );
  require( read( destination + "/file" ) == ( scenario >= 3 ? "user edit" : "old" ), "failed validation preserves destination" );
  require( !fs::exists( temp.path + "/outside" ), "no path escape" );
  for ( const auto& item : fs::directory_iterator( destination ) ) {
    require( item.path().filename() == "file", "failed transfer leaves no publishing temporary" );
  }
}

void fips_digest()
{
  bool installed = true;
  try { Crypto::ensure_mode_available( Crypto::Mode::FipsAES128GCM ); }
  catch ( const Crypto::CryptoException& ) { installed = false; }
  Temp temp; const auto local = temp.path + "/local", remote = temp.path + "/remote";
  write( local + "/file", "replacement" ); write( remote + "/file", "original" );
  Link link( Crypto::Mode::FipsAES128GCM );
  require( link.client.queue( true, local + "/file", remote, "file" ), "queue FIPS digest test" );
  link.finish( 1, !installed );
  require( read( remote + "/file" ) == ( installed ? "replacement" : "original" ), "FIPS digest uses configured provider or fails closed" );
  std::cout << ( installed ? "FIPS provider digest transfer passed\n" : "Missing FIPS provider fails closed; no digest fallback\n" );
}
}

int main()
{
  if ( !Files::available() ) { return 77; }
  try {
    pacing(); copies(); safety();
    for ( unsigned scenario = 0; scenario < 5; ++scenario ) { invalid_peer( scenario ); }
    fips_digest();
    std::cout << "File synchronization tests passed\n";
  }
  catch ( const std::exception& error ) { std::cerr << error.what() << '\n'; return 1; }
}
