/* Distributed under the GNU GPL, version 3 or later. */
#include "download.h"
#include "src/crypto/prng.h"
#include "src/terminal/osc52.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

namespace Download {
namespace {
constexpr size_t MAX_RECORD = 8192;
// Count container overhead too: tiny DATA records must not turn a bounded
// file into millions of unbounded pending allocations before approval.
constexpr size_t MAX_WAITING_BYTES = 96 * 1024 * 1024;
constexpr size_t WAITING_OVERHEAD = 128;
Record result( uint64_t token, const std::string& state, const std::string& error = "" )
{
  Record out;
  out.set_kind( Record::STATUS ); out.set_token( token ); out.set_status( state ); out.set_error( error );
  return out;
}
uint32_t checksum( uint32_t previous, const std::string& bytes )
{
  return crc32( previous, reinterpret_cast<const Bytef*>( bytes.data() ), bytes.size() );
}
bool queue( Clipboard::Channel& channel, const Record& record, Clipboard::Priority priority )
{
  return channel.enqueue( { encode_record( record ) }, priority );
}
}

bool decode_record( const std::string& bytes, Record& record )
{
  record.Clear();
  if ( bytes.empty() || bytes.size() > MAX_RECORD || !record.ParseFromString( bytes )
       || !record.has_kind() || !record.token() ) { return false; }
  if ( record.data().size() > MAX_CHUNK || record.name().size() > 255 || record.error().size() > 1024 ) { return false; }
  switch ( record.kind() ) {
    case Record::BEGIN: return record.name().size() <= 220 && safe_basename( record.name() ) && record.has_size() && record.size() <= MAX_SIZE && record.data().empty();
    case Record::DATA: return !record.data().empty();
    case Record::END: return record.has_checksum() && record.data().empty();
    case Record::CANCEL: return record.data().empty();
    case Record::STATUS: return record.status() == "progress" || record.status() == "approved" || record.status() == "saved" || record.status() == "error" || record.status() == "canceled";
  }
  return false;
}

std::string encode_record( const Record& record ) { return record.SerializeAsString(); }

void Sender::reply( uint32_t id, const std::string& state, const std::string& extra )
{
  if ( replies.size() < 128 ) { replies.push_back( status( id, state, extra ) ); }
}

void Sender::cancel( uint32_t id, const std::string& reason )
{
  const auto it = jobs.find( id );
  if ( it == jobs.end() ) { return; }
  const uint64_t token = it->second.token;
  channel.discard_queued( [token]( const std::string& bytes ) { Record r; return decode_record( bytes, r ) && r.token() == token; } );
  Record r; r.set_kind( Record::CANCEL ); r.set_token( token );
  queue( channel, r, Clipboard::Priority::Background );
  if ( it->second.reply ) { reply( id, "error", ":message=" + Terminal::base64_encode_osc52( reason ) ); }
  for ( const auto& bytes : it->second.waiting ) { waiting_bytes -= bytes.size() + WAITING_OVERHEAD; }
  reserved -= it->second.size; jobs.erase( it );
}

void Sender::submit( const std::string& body, uint64_t now )
{
  Command command;
  const bool valid = parse( body, command );
  auto it = jobs.find( command.id );
  if ( !valid ) {
    if ( it != jobs.end() && command.op != "begin" && command.op != "query" ) { cancel( command.id, "Invalid download command" ); }
    else if ( command.reply && command.id ) { reply( command.id, "error", ":message=" + Terminal::base64_encode_osc52( "Invalid download command or file size" ) ); }
    return;
  }
  if ( command.op == "query" ) {
    reply( command.id, enabled ? "supported" : "unsupported",
           enabled ? ":max_chunk=4096:max_size=" + std::to_string( MAX_SIZE ) + ":approval=1" : "" );
    return;
  }
  if ( !enabled ) {
    if ( command.reply ) { reply( command.id, "error", ":message=" + Terminal::base64_encode_osc52( "Downloads not negotiated" ) ); }
    return;
  }
  if ( command.op == "begin" ) {
    if ( it != jobs.end() || jobs.size() >= MAX_TRANSFERS || command.size > MAX_SIZE - reserved || next_token == UINT64_MAX ) {
      if ( command.reply ) { reply( command.id, "error", ":message=" + Terminal::base64_encode_osc52( "Download ID busy or queue limit exceeded" ) ); }
      return;
    }
    Job job; job.token = ++next_token; job.size = command.size; job.reply = command.reply; job.confirm = command.confirm; job.updated = now;
    Record r; r.set_kind( Record::BEGIN ); r.set_token( job.token ); r.set_name( command.name ); r.set_size( command.size );
    if ( !queue( channel, r, Clipboard::Priority::Background ) ) {
      if ( job.reply ) { reply( command.id, "error", ":message=" + Terminal::base64_encode_osc52( "Download queue full" ) ); }
      return;
    }
    reserved += job.size; jobs.emplace( command.id, job ); return;
  }
  if ( it == jobs.end() ) { return; }
  if ( command.op == "cancel" ) { cancel( command.id, "Download canceled" ); return; }
  auto& job = it->second;
  if ( job.ended ) { cancel( command.id, "Data after download end" ); return; }
  Record r; r.set_token( job.token ); job.updated = now;
  if ( command.op == "data" ) {
    if ( command.data.size() > job.size - job.received ) { cancel( command.id, "Download exceeds declared size" ); return; }
    job.received += command.data.size(); job.checksum = checksum( job.checksum, command.data );
    r.set_kind( Record::DATA ); r.set_data( command.data );
  } else {
    if ( job.received != job.size ) { cancel( command.id, "Download size mismatch" ); return; }
    r.set_kind( Record::END ); r.set_checksum( job.checksum ); job.ended = true;
  }
  // File payload remains on the server until the client user consents.
  auto bytes = encode_record( r );
  const size_t cost = bytes.size() + WAITING_OVERHEAD;
  if ( cost > MAX_WAITING_BYTES - waiting_bytes ) { cancel( command.id, "Download approval buffer full" ); return; }
  waiting_bytes += cost;
  job.waiting.push_back( std::move( bytes ) );
  if ( job.ended && job.reply ) { reply( command.id, "queued" ); }
}

void Sender::tick( uint64_t now )
{
  std::string bytes;
  for ( unsigned n = 0; n < 8 && channel.take_output( bytes ); n++ ) {
    Record r;
    if ( !decode_record( bytes, r ) || r.kind() != Record::STATUS || r.status() == "progress" ) { continue; }
    for ( auto it = jobs.begin(); it != jobs.end(); ++it ) {
      if ( it->second.token != r.token() ) { continue; }
      if ( r.status() == "approved" ) {
        if ( !it->second.approved && it->second.reply && it->second.confirm ) { reply( it->first, "approved" ); }
        it->second.approved = true;
      }
      else if ( r.status() == "saved" && it->second.approved && it->second.ended && safe_basename( r.name() ) ) {
        if ( it->second.reply ) { reply( it->first, "saved", ":name=" + Terminal::base64_encode_osc52( r.name() ) ); }
        for ( const auto& pending : it->second.waiting ) { waiting_bytes -= pending.size() + WAITING_OVERHEAD; }
        reserved -= it->second.size; jobs.erase( it );
      } else { cancel( it->first, r.error().empty() ? "Download failed" : r.error() ); }
      break;
    }
  }
  std::vector<uint32_t> expired;
  for ( const auto& entry : jobs ) {
    if ( !entry.second.ended && now - entry.second.updated >= 120000 ) { expired.push_back( entry.first ); }
  }
  for ( auto id : expired ) { cancel( id, "Incomplete download timed out" ); }
  unsigned budget = 8;
  for ( auto& entry : jobs ) {
    auto& job = entry.second;
    while ( budget && job.approved && !job.waiting.empty() ) {
      if ( !channel.enqueue( { job.waiting.front() }, Clipboard::Priority::Background ) ) { break; }
      waiting_bytes -= job.waiting.front().size() + WAITING_OVERHEAD;
      job.waiting.pop_front(); --budget;
    }
  }
}

std::string Sender::take_replies()
{
  std::string out;
  // Bound PTY writeback per event-loop iteration, even with many queries.
  while ( !replies.empty() && out.size() + replies.front().size() <= MAX_RECORD ) {
    out += replies.front(); replies.pop_front();
  }
  return out;
}

int Sender::wait_time( uint64_t now, bool background ) const
{
  int wait = channel.wait_time( now, background );
  if ( !replies.empty() ) { return 0; }
  for ( const auto& entry : jobs ) {
    if ( entry.second.approved && !entry.second.waiting.empty() ) { wait = std::min( wait, 5 ); }
    if ( !entry.second.ended ) {
      const uint64_t elapsed = now - entry.second.updated;
      wait = std::min( wait, elapsed >= 120000 ? 0 : int( 120000 - elapsed ) );
    }
  }
  return wait;
}

void Sink::remove( std::map<uint64_t, File>::iterator it )
{
  if ( it->second.fd >= 0 ) { close( it->second.fd ); }
  unlinkat( root, it->second.temporary.c_str(), 0 );
  reserved -= it->second.size; files.erase( it );
}

Sink::~Sink()
{
  while ( !files.empty() ) { remove( files.begin() ); }
  if ( root >= 0 ) { close( root ); }
}

Record Sink::handle( const Record& r )
{
  const auto invalid = result( r.token(), "error", "Invalid download record" );
  Record checked;
  if ( !decode_record( encode_record( r ), checked ) || r.kind() == Record::STATUS ) { return invalid; }
  auto it = files.find( r.token() );
  if ( r.kind() == Record::BEGIN ) {
    if ( r.token() <= last_token ) { return invalid; }
    last_token = r.token();
    if ( files.size() >= MAX_TRANSFERS || r.size() > MAX_SIZE - reserved ) { return result( r.token(), "error", "Download storage limit exceeded" ); }
    if ( root < 0 ) {
      // The directory is selected locally, never from a remote request.
      root = open( directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC );
      if ( root < 0 ) { return result( r.token(), "error", "Cannot open local Downloads directory" ); }
    }
    File file; file.name = r.name(); file.size = r.size();
    PRNG random( mode );
    for ( unsigned tries = 0; tries < 8; ++tries ) {
      file.temporary = ".goblin-download." + std::to_string( random.uint64() ) + ".part";
      file.fd = openat( root, file.temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
      if ( file.fd >= 0 || errno != EEXIST ) { break; }
    }
    if ( file.fd < 0 ) { return result( r.token(), "error", "Cannot create download temporary file" ); }
    reserved += file.size; files.emplace( r.token(), std::move( file ) );
    return result( r.token(), "progress" );
  }
  if ( r.kind() == Record::CANCEL ) {
    if ( it != files.end() ) { remove( it ); }
    return result( r.token(), "canceled" );
  }
  if ( it == files.end() ) { return result( r.token(), "error", "Download is not active" ); }
  auto& file = it->second;
  std::string error;
  if ( r.kind() == Record::DATA ) {
    if ( r.data().size() > file.size - file.written ) { error = "Download exceeds declared size"; }
    size_t at = 0;
    while ( error.empty() && at < r.data().size() ) {
      const ssize_t n = write( file.fd, r.data().data() + at, r.data().size() - at );
      if ( n < 0 && errno == EINTR ) { continue; }
      if ( n <= 0 ) { error = "Writing download failed"; break; }
      at += n;
    }
    if ( error.empty() ) {
      file.written += r.data().size(); file.checksum = checksum( file.checksum, r.data() );
      return result( r.token(), "progress" );
    }
  } else if ( r.kind() == Record::END ) {
    if ( file.written != file.size || file.checksum != r.checksum() ) { error = "Download size or checksum mismatch"; }
    else if ( fsync( file.fd ) != 0 ) { error = "Flushing download failed"; }
    else if ( can_publish && !can_publish() ) { error = "Download session closed before completion"; }
    else {
      // linkat atomically publishes without replacing ANY existing entry,
      // including symlinks and directories. The staging name is unlinked next.
      std::string chosen;
      for ( unsigned suffix = 0; suffix < 10000; ++suffix ) {
        chosen = file.name + ( suffix ? "." + std::to_string( suffix ) : "" );
        if ( linkat( root, file.temporary.c_str(), root, chosen.c_str(), 0 ) == 0 ) {
          Record out = result( r.token(), "saved" ); out.set_name( chosen );
          remove( it ); return out;
        }
        if ( errno != EEXIST ) { error = "Publishing download failed"; break; }
      }
      if ( error.empty() ) { error = "No unused download filename available"; }
    }
  }
  remove( it ); return result( r.token(), "error", error );
}

namespace {
std::string frame( const Record& record )
{
  const auto bytes = encode_record( record );
  if ( bytes.empty() || bytes.size() > MAX_RECORD ) { throw std::runtime_error( "Invalid download worker frame" ); }
  std::string out;
  for ( int shift = 24; shift >= 0; shift -= 8 ) { out += char( bytes.size() >> shift ); }
  return out + bytes;
}
bool unframe( std::string& bytes, Record& record )
{
  if ( bytes.size() < 4 ) { return false; }
  uint32_t size = 0;
  for ( unsigned i = 0; i < 4; i++ ) { size = ( size << 8 ) | uint8_t( bytes[i] ); }
  if ( !size || size > MAX_RECORD ) { throw std::runtime_error( "Invalid download worker frame" ); }
  if ( bytes.size() < size + 4 ) { return false; }
  if ( !decode_record( bytes.substr( 4, size ), record ) ) { throw std::runtime_error( "Invalid download worker record" ); }
  bytes.erase( 0, size + 4 ); return true;
}
ssize_t send_bytes( int fd, const std::string& bytes )
{
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  return send( fd, bytes.data(), bytes.size(), flags );
}
void worker( int fd, const std::string& directory, Crypto::Mode mode )
{
  const int max_fd = getdtablesize();
  for ( int i = 0; i < max_fd; i++ ) { if ( i != fd ) { close( i ); } }
  sigset_t mask; sigemptyset( &mask ); sigprocmask( SIG_SETMASK, &mask, nullptr );
  for ( int signum : { SIGINT, SIGTERM, SIGHUP, SIGALRM, SIGPIPE } ) {
    struct sigaction action {}; action.sa_handler = SIG_IGN; sigemptyset( &action.sa_mask ); sigaction( signum, &action, nullptr );
  }
  // Normal disconnect closes the IPC socket. Leaving this scope removes all
  // incomplete files before exiting; completed downloads are never removed.
  try {
    Sink sink( directory, mode, [fd]() {
      char byte;
      const ssize_t n = recv( fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT );
      return n > 0 || ( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) );
    } );
    std::string incoming;
    while ( true ) {
      Record request;
      while ( !unframe( incoming, request ) ) {
        char bytes[MAX_RECORD];
        const ssize_t n = read( fd, bytes, sizeof bytes );
        if ( n < 0 && errno == EINTR ) { continue; }
        if ( n <= 0 ) { return; }
        incoming.append( bytes, n );
      }
      std::string outgoing = frame( sink.handle( request ) );
      while ( !outgoing.empty() ) {
        const ssize_t n = send_bytes( fd, outgoing );
        if ( n < 0 && errno == EINTR ) { continue; }
        if ( n <= 0 ) { return; }
        outgoing.erase( 0, n );
      }
    }
  } catch ( const std::exception& ) { return; }
}
}

bool Writer::spawn()
{
  int pair[2];
  if ( socketpair( AF_UNIX, SOCK_STREAM, 0, pair ) < 0 ) { return false; }
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  setsockopt( pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled );
  setsockopt( pair[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled );
#endif
  child = fork();
  if ( child == 0 ) { close( pair[0] ); worker( pair[1], directory, mode ); _exit( 0 ); }
  close( pair[1] );
  if ( child < 0 ) { close( pair[0] ); return false; }
  socket = pair[0];
  if ( fcntl( socket, F_SETFL, O_NONBLOCK ) < 0 || fcntl( socket, F_SETFD, FD_CLOEXEC ) < 0 ) {
    close( socket ); socket = -1; return false;
  }
  return true;
}

Writer::~Writer()
{
  if ( socket >= 0 ) { close( socket ); }
  if ( child > 0 ) { waitpid( child, nullptr, WNOHANG ); }
}

void Writer::fail( const std::string& message )
{
  if ( socket >= 0 ) { close( socket ); socket = -1; }
  if ( busy ) { replies.push_back( result( token, "error", message ) ); }
  busy = false; incoming.clear(); outgoing.clear();
}

bool Writer::submit( const Record& record, uint64_t now )
{
  if ( !ready() ) { return false; }
  if ( socket < 0 && !spawn() ) { replies.push_back( result( record.token(), "error", "Could not start download writer" ) ); return true; }
  token = record.token(); started = now; busy = true; outgoing = frame( record ); return true;
}

void Writer::tick( uint64_t now )
{
  if ( child > 0 ) {
    const pid_t reaped = waitpid( child, nullptr, WNOHANG );
    if ( reaped == child || ( reaped < 0 && errno == ECHILD ) ) { child = -1; fail( "Download writer exited" ); }
  }
  if ( socket < 0 ) { return; }
  if ( busy && now - started >= 30000 ) { fail( "Download storage did not respond within 30 seconds" ); return; }
  if ( !outgoing.empty() ) {
    const ssize_t n = send_bytes( socket, outgoing );
    if ( n > 0 ) { outgoing.erase( 0, n ); }
    else if ( n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) ) { fail( "Download worker write failed" ); return; }
  }
  char bytes[MAX_RECORD];
  const ssize_t n = read( socket, bytes, sizeof bytes );
  if ( n > 0 ) { incoming.append( bytes, n ); }
  else if ( n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) ) { fail( "Download worker read failed" ); return; }
  try {
    Record response;
    if ( unframe( incoming, response ) ) {
      if ( !busy || response.kind() != Record::STATUS || response.token() != token ) { fail( "Unexpected download worker response" ); return; }
      replies.push_back( response ); busy = false;
    }
  } catch ( const std::exception& ) { fail( "Invalid download worker response" ); }
}

bool Writer::pop( Record& record )
{
  if ( replies.empty() ) { return false; }
  record = std::move( replies.front() ); replies.pop_front(); return true;
}
int Writer::wait_time() const { return !replies.empty() ? 0 : !outgoing.empty() ? 5 : busy || ( child > 0 && socket < 0 ) ? 100 : INT_MAX; }

void Receiver::erase( uint64_t token )
{
  const auto it = offers.find( token );
  if ( it != offers.end() ) { reserved -= it->second.begin.size(); offers.erase( it ); }
}

bool Receiver::pending( Record& request ) const
{
  if ( parent.selected() != Parent::Route::Local ) { return false; }
  for ( const auto& entry : offers ) {
    if ( !entry.second.approved ) { request = entry.second.begin; return true; }
  }
  return false;
}

void Receiver::decide( uint64_t token, bool allow )
{
  if ( parent.selected() != Parent::Route::Local ) { return; }
  const auto it = offers.find( token );
  if ( it == offers.end() || it->second.approved ) { return; }
  if ( allow ) { it->second.approved = true; }
  else {
    pending_status.push_back( result( token, "error", "Download declined by local user" ) );
    erase( token );
  }
}

void Receiver::tick( uint64_t now )
{
  parent.tick( now );
  writer.tick( now );
  Record status;
  while ( parent.pop( status ) ) {
    const auto it = offers.find( status.token() );
    if ( it == offers.end() || !it->second.forwarded ) { continue; }
    if ( status.status() == "approved" ) { it->second.active = true; }
    else { erase( status.token() ); }
    pending_status.push_back( status );
  }
  while ( writer.pop( status ) ) {
    // Approvals can arrive in a different order from offers. Give the worker
    // its own monotonic tokens so its duplicate/replay guard remains valid.
    const auto it = std::find_if( offers.begin(), offers.end(), [&]( const auto& entry ) {
      return entry.second.local_token && entry.second.local_token == status.token();
    } );
    if ( it == offers.end() ) { continue; }
    status.set_token( it->first );
    if ( status.status() != "progress" ) { pending_status.push_back( status ); erase( status.token() ); }
    else if ( it != offers.end() && it->second.starting && !it->second.active ) {
      it->second.active = true;
      pending_status.push_back( result( status.token(), "approved" ) );
    }
  }
  std::vector<uint64_t> expired;
  for ( const auto& entry : offers ) {
    if ( parent.selected() == Parent::Route::Local && !entry.second.approved
         && now - entry.second.created >= 120000 ) { expired.push_back( entry.first ); }
  }
  for ( auto token : expired ) { decide( token, false ); }
  if ( !pending_status.empty() && queue( channel, pending_status.front(), Clipboard::Priority::Interactive ) ) { pending_status.pop_front(); }
  if ( !writer.ready() || !parent.ready() || pending_status.size() >= 16 ) { return; }
  for ( auto& entry : offers ) {
    if ( parent.forwarding() && !entry.second.starting ) {
      if ( parent.submit( entry.second.begin, now ) ) {
        entry.second.forwarded = entry.second.approved = entry.second.starting = true;
      }
      return;
    }
    if ( entry.second.approved && !entry.second.starting ) {
      if ( next_local_token == UINT64_MAX ) {
        pending_status.push_back( result( entry.first, "error", "Download worker token limit reached" ) );
        erase( entry.first ); return;
      }
      entry.second.starting = true;
      entry.second.local_token = ++next_local_token;
      auto local = entry.second.begin; local.set_token( entry.second.local_token );
      writer.submit( local, now ); return;
    }
  }
  std::string bytes;
  if ( channel.take_output( bytes ) ) {
    Record record;
    if ( !decode_record( bytes, record ) || record.kind() == Record::STATUS ) { return; }
    const auto it = offers.find( record.token() );
    if ( record.kind() == Record::BEGIN ) {
      if ( record.token() <= last_token ) { return; }
      last_token = record.token();
      if ( offers.size() >= MAX_TRANSFERS || record.size() > MAX_SIZE - reserved ) {
        pending_status.push_back( result( record.token(), "error", "Download approval queue full" ) ); return;
      }
      Offer offer; offer.begin = record; offer.created = now;
      reserved += record.size(); offers.emplace( record.token(), std::move( offer ) );
    } else if ( record.kind() == Record::CANCEL ) {
      const uint64_t remote_token = record.token();
      if ( it != offers.end() && it->second.forwarded ) { parent.submit( record, now ); }
      else if ( it != offers.end() && it->second.starting ) {
        record.set_token( it->second.local_token ); writer.submit( record, now );
      }
      erase( remote_token );
    } else if ( it != offers.end() && it->second.active ) {
      if ( it->second.forwarded ) { parent.submit( record, now ); }
      else { record.set_token( it->second.local_token ); writer.submit( record, now ); }
    }
    else if ( it != offers.end() ) { decide( record.token(), false ); }
  }
}

int Receiver::wait_time( uint64_t now, int pacing_wait ) const
{
  // A stopped worker must not turn a decoded, flow-controlled record into a
  // busy loop. Its socket or periodic worker timeout wakes us instead.
  const bool ready = writer.ready() && parent.ready();
  int wait = std::min( writer.wait_time(), ready ? std::max( pacing_wait, channel.wait_time( now, false ) ) : 100 );
  wait = std::min( wait, parent.wait_time( now ) );
  if ( !pending_status.empty() ) { wait = std::min( wait, 5 ); }
  for ( const auto& entry : offers ) {
    if ( ( entry.second.approved || parent.forwarding() ) && !entry.second.starting && ready ) { wait = 0; }
    if ( parent.selected() == Parent::Route::Local && !entry.second.approved ) {
      const uint64_t elapsed = now - entry.second.created;
      wait = std::min( wait, elapsed >= 120000 ? 0 : int( 120000 - elapsed ) );
    }
  }
  return wait;
}

std::string downloads_directory()
{
  const char* configured = getenv( "MOSH_DOWNLOAD_DIR" );
  if ( configured && *configured ) { return configured; }
  const char* user_home = getenv( "HOME" );
  if ( user_home && *user_home ) { return std::string( user_home ) + "/Downloads"; }
  const auto* account = getpwuid( getuid() );
  return account ? std::string( account->pw_dir ) + "/Downloads" : std::string();
}
}
