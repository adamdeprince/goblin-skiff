/* Distributed under the GNU GPL, version 3 or later. */
#include "downloadforward.h"
#include "src/crypto/prng.h"
#include "src/terminal/download.h"
#include "src/terminal/osc52.h"
#include <algorithm>
#include <climits>
#include <vector>
#include <zlib.h>

namespace Download {
namespace {
constexpr uint64_t PROBE_TIMEOUT = 5000, APPROVAL_TIMEOUT = 120000, TRANSFER_TIMEOUT = 300000;
constexpr size_t OUTPUT_LIMIT = 32768, INPUT_LIMIT = 8192;
const std::string goblin_prefix = "\033]777;goblin-download;";
const std::string kitty_prefix = "\033]5113;";
const std::string st = "\033\\";
std::string b64( const std::string& bytes ) { return Terminal::base64_encode_osc52( bytes ); }
bool number( const std::string& s, uint64_t max, uint64_t& out )
{
  out = 0;
  if ( s.empty() ) { return false; }
  for ( unsigned char c : s ) {
    if ( c < '0' || c > '9' || out > ( max - ( c - '0' ) ) / 10 ) { return false; }
    out = out * 10 + c - '0';
  }
  return out <= max;
}
bool metadata( const std::string& s, char separator, std::map<std::string, std::string>& fields )
{
  if ( s.size() > INPUT_LIMIT ) { return false; }
  for ( unsigned char c : s ) { if ( c < 32 || c > 126 ) { return false; } }
  for ( size_t at = 0; at < s.size(); ) {
    const size_t next = s.find( separator, at ), end = next == std::string::npos ? s.size() : next;
    const size_t equal = s.find( '=', at );
    if ( equal >= end || equal == at || !fields.emplace( s.substr( at, equal - at ), s.substr( equal + 1, end - equal - 1 ) ).second ) { return false; }
    at = end + 1;
  }
  return true;
}
int remaining( uint64_t now, uint64_t deadline ) { return now >= deadline ? 0 : int( std::min<uint64_t>( INT_MAX, deadline - now ) ); }
}

std::string Parent::goblin( uint32_t id, const std::string& meta ) const
{ return goblin_prefix + "v=1:id=" + std::to_string( id ) + ":" + meta + st; }
std::string Parent::kitty_id( uint32_t id ) const { return "goblin-" + nonce + "-" + std::to_string( id ); }
std::string Parent::kitty( uint32_t id, const std::string& meta ) const
{ return kitty_prefix + "id=" + kitty_id( id ) + ";" + meta + st; }

void Parent::emit( uint64_t token, const std::string& bytes, bool cleanup )
{ output.push_back( { token, bytes, cleanup } ); output_bytes += bytes.size(); }

void Parent::response( uint64_t token, const std::string& state, const std::string& error, const std::string& name )
{
  Record r; r.set_kind( Record::STATUS ); r.set_token( token ); r.set_status( state );
  r.set_error( error.substr( 0, 1024 ) ); r.set_name( name ); replies.push_back( std::move( r ) );
}

void Parent::discover( uint64_t now )
{
  if ( route != Route::Unknown ) { return; }
  PRNG random( mode );
  first_id = ( random.uint32() & 0x7fffffffU ) + 1; next_id = first_id;
  nonce = std::to_string( random.uint64() );
  route = Route::ProbeGoblin; deadline = now + PROBE_TIMEOUT;
  emit( 0, goblin( first_id, "op=query" ) );
}

void Parent::probe_kitty( uint64_t now )
{
  route = Route::ProbeKitty; deadline = now + PROBE_TIMEOUT;
  // OSC 5113 has no capability query. Kitty's check_bypass/start_receive
  // rejects this impossible SHA256 digest before opening a confirmation UI.
  // Never send a name or payload in the probe, even if a peer answers OK.
  // See kitty/file_transmission.py and the published file-transfer protocol.
  // Despite the documentation's safe_string table, Kitty serializes pw as
  // base64 UTF-8 (FileTransmissionCommand.bypass). Match the implementation.
  emit( 0, kitty( first_id, "ac=send;pw=" + b64( "sha256:0" ) ) );
}

bool Parent::ready() const { return output_bytes < OUTPUT_LIMIT - 10000 && replies.size() < 16; }

void Parent::kitty_file( uint64_t token, Job& job )
{
  job.name = job.begin.name() + ( job.collision ? "." + std::to_string( job.collision ) : "" );
  emit( token, kitty( job.id, "ac=file;fid=f" + std::to_string( job.collision ) + ";n=" + b64( "~/Downloads/" + job.name )
                            + ";sz=" + std::to_string( job.begin.size() ) + ";prm=384;tt=simple;zip=none" ) );
  job.stage = Stage::KittyFile;
}

void Parent::cancel( uint64_t token, const std::string& reason )
{
  const auto it = jobs.find( token );
  if ( it == jobs.end() ) { return; }
  for ( auto out = output.begin(); out != output.end(); ) {
    if ( out->token == token ) { output_bytes -= out->bytes.size(); out = output.erase( out ); }
    else { ++out; }
  }
  emit( 0, route == Route::Goblin ? goblin( it->second.id, "op=cancel" ) : kitty( it->second.id, "ac=cancel" ), true );
  if ( !reason.empty() ) { response( token, "error", reason ); }
  jobs.erase( it );
}

bool Parent::submit( const Record& r, uint64_t now )
{
  if ( !forwarding() || !ready() ) { return false; }
  if ( r.kind() == Record::BEGIN ) {
    if ( jobs.count( r.token() ) || jobs.size() >= MAX_TRANSFERS || next_id == UINT32_MAX
         || !safe_basename( r.name() ) || r.size() > MAX_SIZE || ( route == Route::Goblin && r.size() > goblin_limit ) ) {
      response( r.token(), "error", "Parent download limits exceeded" ); return true;
    }
    Job job; job.begin = r; job.id = ++next_id; job.updated = now; job.name = r.name();
    if ( route == Route::Goblin ) {
      emit( r.token(), goblin( job.id, "op=begin:name=" + b64( r.name() ) + ":size=" + std::to_string( r.size() )
                                      + ":reply=1" + ( confirmation ? ":confirm=1" : "" ) ) );
      job.stage = confirmation ? Stage::GoblinApproval : Stage::Data;
      // Older Goblin parents buffer the stream themselves while awaiting
      // their user's decision. New parents propagate approval end-to-end.
      if ( !confirmation ) { response( r.token(), "approved" ); }
    } else {
      emit( r.token(), kitty( job.id, "ac=send" ) ); job.stage = Stage::KittyApproval;
    }
    jobs.emplace( r.token(), std::move( job ) ); return true;
  }
  const auto it = jobs.find( r.token() );
  if ( it == jobs.end() ) { return true; }
  auto& job = it->second;
  if ( r.kind() == Record::CANCEL ) { cancel( r.token(), "" ); return true; }
  if ( job.stage != Stage::Data ) { cancel( r.token(), "Download data arrived before parent approval or after end" ); return true; }
  job.updated = now;
  if ( r.kind() == Record::DATA ) {
    if ( r.data().empty() || r.data().size() > MAX_CHUNK || r.data().size() > job.begin.size() - job.received ) {
      cancel( r.token(), "Invalid forwarded download size" ); return true;
    }
    job.received += r.data().size();
    job.checksum = crc32( job.checksum, reinterpret_cast<const Bytef*>( r.data().data() ), r.data().size() );
    if ( route == Route::Goblin ) {
      for ( size_t at = 0; at < r.data().size(); at += goblin_chunk ) {
        emit( r.token(), goblin( job.id, "op=data;" + b64( r.data().substr( at, goblin_chunk ) ) ) );
      }
    } else { emit( r.token(), kitty( job.id, "ac=data;fid=f" + std::to_string( job.collision ) + ";d=" + b64( r.data() ) ) ); }
  } else if ( r.kind() == Record::END ) {
    if ( job.received != job.begin.size() || !r.has_checksum() || job.checksum != r.checksum() ) {
      cancel( r.token(), "Forwarded download checksum/size mismatch" ); return true;
    }
    emit( r.token(), route == Route::Goblin ? goblin( job.id, "op=end" )
                                          : kitty( job.id, "ac=end_data;fid=f" + std::to_string( job.collision ) ) );
    job.stage = Stage::End;
  }
  return true;
}

bool Parent::handle( const std::string& sequence, uint64_t now )
{
  const bool is_goblin = sequence.compare( 0, goblin_prefix.size(), goblin_prefix ) == 0;
  const size_t prefix = is_goblin ? goblin_prefix.size() : kitty_prefix.size();
  const size_t suffix = sequence.back() == '\007' ? 1 : 2;
  std::map<std::string, std::string> fields;
  if ( !metadata( sequence.substr( prefix, sequence.size() - prefix - suffix ), is_goblin ? ':' : ';', fields ) ) { return false; }
  uint64_t id;
  if ( is_goblin ) {
    if ( fields["v"] != "1" || fields["op"] != "status" || !number( fields["id"], UINT32_MAX, id ) ) { return false; }
  } else {
    const std::string id_prefix = "goblin-" + nonce + "-";
    if ( fields["ac"] != "status" || fields["id"].compare( 0, id_prefix.size(), id_prefix )
         || !number( fields["id"].substr( id_prefix.size() ), UINT32_MAX, id ) ) { return false; }
  }
  // Own only our allocated namespace, including late/duplicate replies.
  if ( !first_id || id < first_id || id > next_id ) { return false; }
  std::string state = fields["status"];
  if ( !is_goblin && !Terminal::base64_decode_osc52( fields["st"], state ) ) { return true; }
  if ( id == first_id ) {
    if ( is_goblin && route == Route::ProbeGoblin ) {
      if ( state == "supported" ) {
        uint64_t chunk = MAX_CHUNK, limit = MAX_SIZE;
        if ( ( fields.count( "max_chunk" ) && ( !number( fields["max_chunk"], UINT32_MAX, chunk ) || chunk < 256 ) )
             || ( fields.count( "max_size" ) && !number( fields["max_size"], UINT64_MAX, limit ) ) ) {
          route = Route::Local; return true;
        }
        goblin_chunk = std::min<uint64_t>( MAX_CHUNK, chunk ); goblin_limit = std::min<uint64_t>( MAX_SIZE, limit );
        confirmation = fields["approval"] == "1"; route = Route::Goblin;
      } else if ( state == "unsupported" ) { probe_kitty( now ); }
    } else if ( !is_goblin && route == Route::ProbeKitty && !state.empty() ) {
      // EPERM is the expected answer to the *probe*. EPERM for an actual
      // transfer below is always a terminal refusal, never a fallback trigger.
      if ( state == "OK" || state.compare( 0, 5, "EPERM" ) == 0 ) { route = Route::Kitty; }
      else { route = Route::Local; }
      emit( 0, kitty( first_id, "ac=cancel" ), true );
    }
    return true;
  }
  auto it = std::find_if( jobs.begin(), jobs.end(), [&]( const auto& entry ) { return entry.second.id == id; } );
  if ( it == jobs.end() || is_goblin != ( route == Route::Goblin ) ) { return true; }
  const uint64_t token = it->first;
  auto& job = it->second;
  if ( is_goblin ) {
    if ( state == "approved" && job.stage == Stage::GoblinApproval ) {
      job.updated = now; job.stage = Stage::Data; response( token, "approved" );
    } else if ( state == "saved" && job.stage == Stage::End ) {
      std::string name;
      if ( !Terminal::base64_decode_osc52( fields["name"], name ) || !safe_basename( name ) ) {
        cancel( token, "Invalid parent download completion" ); return true;
      }
      response( token, "saved", "", name ); jobs.erase( it );
    } else if ( state == "error" || state == "unsupported" ) {
      std::string error;
      if ( !Terminal::base64_decode_osc52( fields["message"], error ) || error.empty() ) { error = "Parent declined or failed the download"; }
      cancel( token, error );
    }
    return true;
  }
  const auto fid = fields["fid"];
  if ( !fid.empty() && fid != "f" + std::to_string( job.collision ) ) { return true; }
  if ( job.stage == Stage::KittyApproval && state == "OK" && fid.empty() ) {
    if ( output_bytes > OUTPUT_LIMIT - 1024 ) { cancel( token, "Parent download control buffer full" ); return true; }
    job.updated = now; kitty_file( token, job );
  } else if ( job.stage == Stage::KittyFile && state == "STARTED" && !fid.empty() ) {
    // Kitty reports the existing destination's size, or -1 if absent. Don't
    // knowingly overwrite: try a suffix before sending any bytes for this fid.
    if ( fields.count( "sz" ) && fields["sz"] != "-1" ) {
      uint64_t existing;
      if ( !number( fields["sz"], UINT64_MAX, existing ) || ++job.collision > 1000 ) {
        cancel( token, "Parent download destination collision limit" ); return true;
      }
      if ( output_bytes > OUTPUT_LIMIT - 1024 ) { cancel( token, "Parent download control buffer full" ); return true; }
      kitty_file( token, job ); return true;
    }
    if ( !fields["tt"].empty() && fields["tt"] != "simple" ) { cancel( token, "Unsupported parent file transmission type" ); return true; }
    job.updated = now; job.stage = Stage::Data; response( token, "approved" );
  } else if ( job.stage == Stage::End && state == "OK" && !fid.empty() ) {
    uint64_t size;
    if ( !number( fields["sz"], UINT64_MAX, size ) || size != job.begin.size() ) {
      cancel( token, "Parent download completion size mismatch" ); return true;
    }
    emit( token, kitty( job.id, "ac=finish" ), true );
    // For a regular file Kitty applies metadata and closes the file before
    // this OK. finish has no success reply; it releases the session.
    response( token, "saved", "", job.name ); jobs.erase( it );
  } else if ( state != "OK" && state != "STARTED" && state != "PROGRESS" ) {
    cancel( token, state.empty() ? "Invalid Kitty transfer response" : "Kitty transfer: " + state );
  }
  return true;
}

void Parent::tick( uint64_t now )
{
  if ( route == Route::ProbeGoblin && now >= deadline ) { probe_kitty( now ); }
  else if ( route == Route::ProbeKitty && now >= deadline ) {
    emit( 0, kitty( first_id, "ac=cancel" ), true ); route = Route::Local;
  }
  std::vector<uint64_t> expired;
  for ( const auto& entry : jobs ) {
    const auto& job = entry.second;
    const auto timeout = job.stage == Stage::GoblinApproval || job.stage == Stage::KittyApproval ? APPROVAL_TIMEOUT : TRANSFER_TIMEOUT;
    if ( now - job.updated >= timeout ) { expired.push_back( entry.first ); }
  }
  for ( auto token : expired ) { cancel( token, "Parent download response timed out" ); }
}

bool Parent::pop( Record& r )
{ if ( replies.empty() ) { return false; } r = std::move( replies.front() ); replies.pop_front(); return true; }

std::string Parent::take_output()
{
  if ( output.empty() ) { return {}; }
  auto bytes = std::move( output.front().bytes ); output_bytes -= bytes.size(); output.pop_front(); return bytes;
}

std::string Parent::close()
{
  std::string bytes;
  for ( const auto& item : output ) { if ( item.cleanup ) { bytes += item.bytes; } }
  output.clear(); output_bytes = 0;
  while ( !jobs.empty() ) { cancel( jobs.begin()->first, "" ); }
  if ( route == Route::ProbeKitty ) { emit( 0, kitty( first_id, "ac=cancel" ) ); }
  while ( !output.empty() ) { bytes += take_output(); }
  return bytes;
}

std::string Parent::filter( const std::string& bytes, uint64_t now )
{
  if ( !first_id ) { return bytes; }
  std::string user;
  const std::string paste_start = "\033[200~", paste_end = "\033[201~";
  for ( char c : bytes ) {
    input_updated = now;
    if ( input.empty() ) {
      if ( c == '\033' ) { input += c; } else { user += c; }
      continue;
    }
    if ( !input_matched ) {
      input += c;
      if ( !paste && ( input == goblin_prefix || input == kitty_prefix ) ) { input_matched = true; continue; }
      if ( input == ( paste ? paste_end : paste_start ) ) { user += input; input.clear(); paste = !paste; continue; }
      if ( ( !paste && ( goblin_prefix.compare( 0, input.size(), input ) == 0 || kitty_prefix.compare( 0, input.size(), input ) == 0 ) )
           || ( paste ? paste_end : paste_start ).compare( 0, input.size(), input ) == 0 ) { continue; }
      if ( c == '\033' ) { user.append( input, 0, input.size() - 1 ); input = "\033"; }
      else { user += input; input.clear(); }
      continue;
    }
    const bool terminated = c == '\007' || ( c == '\\' && input.back() == '\033' );
    if ( terminated ) {
      if ( !input_overflow ) { input += c; if ( !handle( input, now ) ) { user += input; } }
      input.clear(); input_matched = input_overflow = false;
    } else if ( c == '\030' || c == '\032' ) { input.clear(); input_matched = input_overflow = false; }
    else if ( input.back() == '\033' ) {
      // An ESC other than ST aborts this OSC; don't swallow subsequent keys.
      input = std::string( "\033" ) + c; input_matched = input_overflow = false;
    } else if ( input.size() < INPUT_LIMIT ) { input += c; }
    else { input_overflow = true; input.back() = c; }
  }
  return user;
}

std::string Parent::expire_input( uint64_t now )
{
  std::string user;
  if ( !input.empty() && now - input_updated >= ( input_matched ? 10000U : 25U ) ) {
    if ( !input_matched ) { user.swap( input ); }
    input.clear(); input_matched = input_overflow = false;
  }
  return user;
}

int Parent::wait_time( uint64_t now ) const
{
  int wait = output.empty() && replies.empty() ? INT_MAX : 0;
  if ( route == Route::ProbeGoblin || route == Route::ProbeKitty ) { wait = std::min( wait, remaining( now, deadline ) ); }
  for ( const auto& entry : jobs ) {
    const auto& job = entry.second;
    const auto timeout = job.stage == Stage::GoblinApproval || job.stage == Stage::KittyApproval ? APPROVAL_TIMEOUT : TRANSFER_TIMEOUT;
    wait = std::min( wait, remaining( now, job.updated + timeout ) );
  }
  if ( !input.empty() ) { wait = std::min( wait, remaining( now, input_updated + ( input_matched ? 10000 : 25 ) ) ); }
  return wait;
}
}
