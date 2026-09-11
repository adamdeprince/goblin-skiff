/* Distributed under the GNU GPL, version 3 or later. */
#include "download.h"
#include "osc52.h"
#include <map>

namespace Download {
namespace {
bool integer( const std::string& s, uint64_t limit, uint64_t& value )
{
  value = 0;
  if ( s.empty() ) { return false; }
  for ( char c : s ) {
    if ( c < '0' || c > '9' || value > ( limit - ( c - '0' ) ) / 10 ) { return false; }
    value = value * 10 + c - '0';
  }
  return value <= limit;
}
}

bool safe_basename( const std::string& name )
{
  // Leave room for a collision suffix on filesystems with NAME_MAX=255.
  if ( name.empty() || name.size() > 240 || name == "." || name == ".." ) { return false; }
  for ( size_t at = 0; at < name.size(); ) {
    uint32_t c = uint8_t( name[at++] ), minimum = 0;
    unsigned continuation = 0;
    if ( c >= 0xc2 && c <= 0xdf ) { continuation = 1; c &= 31; minimum = 0x80; }
    else if ( c >= 0xe0 && c <= 0xef ) { continuation = 2; c &= 15; minimum = 0x800; }
    else if ( c >= 0xf0 && c <= 0xf4 ) { continuation = 3; c &= 7; minimum = 0x10000; }
    else if ( c >= 0x80 ) { return false; }
    if ( at + continuation > name.size() ) { return false; }
    while ( continuation-- ) {
      const uint8_t next = name[at++];
      if ( ( next & 0xc0 ) != 0x80 ) { return false; }
      c = ( c << 6 ) | ( next & 63 );
    }
    if ( c < minimum || c > 0x10ffff || ( c >= 0xd800 && c <= 0xdfff ) || c < 32
         || ( c >= 127 && c <= 159 ) || c == '/' || c == '\\' ) { return false; }
    // Reject bidi/line controls that can disguise a downloaded filename.
    if ( c == 0x061c || c == 0x200e || c == 0x200f || ( c >= 0x2028 && c <= 0x202e )
         || ( c >= 0x2066 && c <= 0x2069 ) ) { return false; }
  }
  return true;
}

bool parse( const std::string& body, Command& command )
{
  command = Command();
  if ( body.compare( 0, sizeof PREFIX - 1, PREFIX ) || body.size() > MAX_OSC ) { return false; }
  for ( unsigned char c : body ) { if ( c < 32 || c > 126 ) { return false; } }
  const size_t payload = body.find( ';', sizeof PREFIX - 1 );
  const size_t end = payload == std::string::npos ? body.size() : payload;
  if ( end > 2048 ) { return false; }
  std::map<std::string, std::string> meta;
  for ( size_t at = sizeof PREFIX - 1; at < end; ) {
    const size_t colon = body.find( ':', at ), stop = colon < end ? colon : end;
    const size_t equal = body.find( '=', at );
    if ( equal >= stop || equal == at || !meta.emplace( body.substr( at, equal - at ), body.substr( equal + 1, stop - equal - 1 ) ).second ) { return false; }
    at = stop + 1;
  }
  uint64_t id = 0;
  if ( !integer( meta["id"], UINT32_MAX, id ) || !id ) { return false; }
  command.id = id; command.op = meta["op"]; command.reply = meta["reply"] == "1";
  command.confirm = meta["confirm"] == "1";
  if ( meta["v"] != "1" || ( meta.count( "reply" ) && !meta["reply"].empty() && meta["reply"] != "0" && meta["reply"] != "1" ) ) { return false; }
  if ( !meta["confirm"].empty() && meta["confirm"] != "0" && meta["confirm"] != "1" ) { return false; }
  if ( command.op == "begin" ) {
    return payload == std::string::npos && integer( meta["size"], MAX_SIZE, command.size )
           && Terminal::base64_decode_osc52( meta["name"], command.name ) && command.name.size() <= 220 && safe_basename( command.name );
  }
  if ( command.op == "data" ) {
    return payload != std::string::npos && Terminal::base64_decode_osc52( body.substr( payload + 1 ), command.data )
           && !command.data.empty() && command.data.size() <= MAX_CHUNK;
  }
  return payload == std::string::npos && ( command.op == "end" || command.op == "cancel" || command.op == "query" );
}

std::string status( uint32_t id, const std::string& state, const std::string& extra )
{
  return "\033]777;goblin-download;v=1:op=status:id=" + std::to_string( id ) + ":status=" + state + extra + "\033\\";
}
}
