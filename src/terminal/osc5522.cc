/* Distributed under the GNU GPL, version 3 or later. */
#include "osc5522.h"
#include "osc52.h"
#include <climits>
#include <map>

namespace Terminal {
bool parse_osc5522( const std::string& body, MimeClipboardMessage& message )
{
  if ( body.size() > OSC5522_MAX_FRAME || body.compare( 0, 5, "5522;" ) ) { return false; }
  for ( unsigned char c : body ) { if ( c < 32 || c > 126 ) { return false; } }
  const size_t end = body.find( ';', 5 );
  const size_t meta_end = end == std::string::npos ? body.size() : end;
  if ( meta_end > 4096 ) { return false; }
  std::map<std::string, std::string> meta;
  for ( size_t at = 5; at < meta_end; ) {
    const size_t colon = body.find( ':', at );
    const size_t stop = colon < meta_end ? colon : meta_end;
    const size_t equal = body.find( '=', at );
    if ( equal >= stop || equal == at || !meta.emplace( body.substr( at, equal - at ), body.substr( equal + 1, stop - equal - 1 ) ).second ) { return false; }
    at = stop + 1;
  }
  MimeClipboardMessage m;
  m.body = body; m.type = meta["type"]; m.status = meta["status"]; m.id = meta["id"];
  if ( m.type != "read" && m.type != "write" && m.type != "wdata" && m.type != "walias" ) { return false; }
  if ( m.id.size() > 256 || m.id.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+." ) != std::string::npos ) { return false; }
  if ( !base64_decode_osc52( meta["mime"], m.mime ) || m.mime.size() > 1024 ) { return false; }
  if ( end != std::string::npos && !base64_decode_osc52( body.substr( end + 1 ), m.data ) ) { return false; }
  if ( m.data.size() > 4096 ) { return false; }
  if ( m.type == "walias" && m.mime.empty() ) { return false; }
  if ( m.type == "wdata" && m.mime.empty() && !m.data.empty() ) { return false; }
  if ( m.type == "read" && m.status == "DATA" && m.mime.empty() ) { return false; }
  message = std::move( m );
  return true;
}

std::string osc5522_error( const MimeClipboardMessage& request, const std::string& status )
{
  return "5522;type=" + std::string( request.type == "read" ? "read" : "write" )
         + ":status=" + status + ( request.id.empty() ? "" : ":id=" + request.id );
}

Osc5522InputFilter::Output Osc5522InputFilter::consume( const std::string& bytes, uint64_t now )
{
  Output out;
  const std::string osc = "\033]5522;", start = "\033[200~", finish = "\033[201~";
  for ( char c : bytes ) {
    updated = now;
    if ( pending.empty() ) {
      if ( c == '\033' ) { pending += c; }
      else { out.user += c; }
      continue;
    }
    if ( !matched ) {
      pending += c;
      if ( !paste && pending == osc ) { matched = true; continue; }
      if ( pending == ( paste ? finish : start ) ) { out.user += pending; pending.clear(); paste = !paste; continue; }
      if ( ( !paste && osc.compare( 0, pending.size(), pending ) == 0 )
           || ( paste ? finish : start ).compare( 0, pending.size(), pending ) == 0 ) { continue; }
      if ( c == '\033' ) { out.user.append( pending, 0, pending.size() - 1 ); pending = "\033"; }
      else { out.user += pending; pending.clear(); }
      continue;
    }
    const bool st = c == '\\' && pending.back() == '\033';
    if ( c == '\007' || st ) {
      if ( !overflow ) {
        if ( st ) { pending.pop_back(); }
        MimeClipboardMessage parsed;
        const auto body = pending.substr( 2 );
        if ( parse_osc5522( body, parsed ) ) { out.messages.push_back( body ); }
      }
      pending.clear(); matched = overflow = false;
    } else if ( c == '\030' || c == '\032' ) { pending.clear(); matched = overflow = false; }
    else if ( pending.size() < OSC5522_MAX_FRAME + 2 ) { pending += c; }
    else { overflow = true; pending.back() = c; }
  }
  return out;
}

int Osc5522InputFilter::wait_time( uint64_t now ) const
{
  if ( pending.empty() ) { return INT_MAX; }
  const uint64_t deadline = updated + ( matched ? 10000 : 25 );
  return now >= deadline ? 0 : int( deadline - now );
}

Osc5522InputFilter::Output Osc5522InputFilter::expire( uint64_t now )
{
  Output out;
  if ( !wait_time( now ) ) {
    if ( !matched ) { out.user.swap( pending ); }
    pending.clear(); matched = overflow = false;
  }
  return out;
}
}
