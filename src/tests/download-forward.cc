/* Distributed under the GNU GPL, version 3 or later. */
#include "src/frontend/download.h"
#include "src/terminal/osc52.h"
#include <iostream>
#include <stdexcept>
#include <zlib.h>

using Download::Parent;
using Download::Record;
namespace {
void require( bool value, const char* message ) { if ( !value ) { throw std::runtime_error( message ); } }
std::string b64( const std::string& s ) { return Terminal::base64_encode_osc52( s ); }
std::string body( const std::string& s )
{ require( s.size() >= 4 && s.substr( 0, 2 ) == "\033]" && s.substr( s.size() - 2 ) == "\033\\", "whole OSC frame" ); return s.substr( 2, s.size() - 4 ); }
Download::Command goblin( const std::string& s )
{ Download::Command c; require( Download::parse( body( s ), c ), "valid Goblin command" ); return c; }
std::map<std::string, std::string> kitty( const std::string& s )
{
  auto bytes = body( s ); require( bytes.substr( 0, 5 ) == "5113;", "Kitty transfer namespace" );
  std::map<std::string, std::string> fields;
  for ( size_t at = 5; at < bytes.size(); ) {
    const size_t sep = bytes.find( ';', at ), stop = sep == std::string::npos ? bytes.size() : sep;
    const size_t eq = bytes.find( '=', at ); require( eq < stop, "Kitty field" );
    fields.emplace( bytes.substr( at, eq - at ), bytes.substr( eq + 1, stop - eq - 1 ) ); at = stop + 1;
  }
  return fields;
}
std::string kitty_reply( const std::string& id, const std::string& state, const std::string& extra = "" )
{ return "\033]5113;ac=status;id=" + id + ";st=" + b64( state ) + extra + "\033\\"; }
Record begin( uint64_t token, const std::string& name, uint64_t size )
{ Record r; r.set_kind( Record::BEGIN ); r.set_token( token ); r.set_name( name ); r.set_size( size ); return r; }
Record data( uint64_t token, const std::string& bytes )
{ Record r; r.set_kind( Record::DATA ); r.set_token( token ); r.set_data( bytes ); return r; }
Record end( uint64_t token, const std::string& bytes )
{ Record r; r.set_kind( Record::END ); r.set_token( token ); r.set_checksum( crc32( 0, reinterpret_cast<const Bytef*>( bytes.data() ), bytes.size() ) ); return r; }
void expect( Parent& parent, uint64_t token, const std::string& state )
{ Record r; require( parent.pop( r ) && r.token() == token && r.status() == state, "mapped parent status" ); }
uint32_t probe( Parent& parent )
{
  parent.enable(); parent.discover( 0 );
  const auto query = goblin( parent.take_output() ); require( query.op == "query", "Goblin probed first" );
  parent.discover( 1 ); require( parent.take_output().empty(), "probe once per connection" ); return query.id;
}
void goblin_forward( bool confirmation )
{
  Parent parent( Crypto::Mode::LegacyOCB );
  const auto query_id = probe( parent );
  const auto support = Download::status( query_id, "supported", ":max_chunk=1024:max_size=67108864" + std::string( confirmation ? ":approval=1" : "" ) );
  const auto pasted = "\033[200~" + support + "\033[201~";
  require( parent.filter( pasted, 1 ) == pasted && parent.selected() == Parent::Route::ProbeGoblin, "pasted support is not a terminal reply" );
  std::string user;
  for ( char c : "a" + support + "z" ) { user += parent.filter( std::string( 1, c ), 2 ); }
  require( user == "az" && parent.selected() == Parent::Route::Goblin && parent.take_output().empty(), "split Goblin probe reply selects Goblin without trying Kitty" );
  const std::string bytes = std::string( 4090, 'x' ) + std::string( "\0\xff\033]", 4 );
  require( parent.submit( begin( 55, "report.bin", bytes.size() ), 3 ), "start forwarding" );
  const auto start = goblin( parent.take_output() );
  require( start.op == "begin" && start.name == "report.bin" && start.reply && start.confirm == confirmation && start.id != query_id,
           "remapped ID and optional consent response" );
  if ( confirmation ) {
    Record r; require( !parent.pop( r ), "no upstream approval before parent's approval" );
    const auto approved = Download::status( start.id, "approved" );
    require( parent.filter( "\033[200~" + approved + "\033[201~", 4 ) == "\033[200~" + approved + "\033[201~"
               && !parent.pop( r ), "pasted approval cannot start payload" );
    parent.filter( approved, 5 );
  }
  expect( parent, 55, "approved" );
  require( parent.submit( data( 55, bytes ), 6 ) && parent.submit( end( 55, bytes ), 7 ), "forward file records" );
  std::string reconstructed;
  for ( unsigned n = 0; n < 4; n++ ) {
    auto c = goblin( parent.take_output() );
    require( c.op == "data" && c.id == start.id && c.data.size() <= 1024, "respects parent chunk limit" ); reconstructed += c.data;
  }
  require( reconstructed == bytes && goblin( parent.take_output() ).op == "end", "exact binary bytes then end" );
  Record r; require( !parent.pop( r ), "forwarding bytes is not saving" );
  const auto saved = Download::status( start.id, "saved", ":name=" + b64( "report.bin.1" ) );
  require( parent.filter( saved, 8 ).empty() && parent.pop( r ) && r.status() == "saved" && r.token() == 55 && r.name() == "report.bin.1",
           "actual parent completion and collision name propagated" );
  require( parent.filter( saved, 9 ).empty() && !parent.pop( r ), "duplicate completion absorbed once" );
  const auto unrelated = Download::status( query_id - 1, "saved", ":name=" + b64( "other" ) );
  require( parent.filter( unrelated, 10 ) == unrelated, "unrelated OSC response remains available to remote application" );
  parent.submit( begin( 56, "denied", 0 ), 11 ); auto denied = goblin( parent.take_output() );
  if ( !confirmation ) { expect( parent, 56, "approved" ); }
  parent.filter( Download::status( denied.id, "error", ":message=" + b64( "User declined" ) ), 12 );
  expect( parent, 56, "error" );
  require( goblin( parent.take_output() ).op == "cancel" && parent.selected() == Parent::Route::Goblin, "refusal cancels without downgrading" );
}

void kitty_forward()
{
  Parent parent( Crypto::Mode::LegacyOCB );
  const auto query = probe( parent ); parent.filter( Download::status( query, "unsupported" ), 1 );
  const auto detect = kitty( parent.take_output() );
  require( detect.at( "ac" ) == "send" && detect.at( "pw" ) == b64( "sha256:0" ) && detect.size() == 3,
           "Kitty probe cannot name a file, carry data, or authorize it" );
  parent.filter( kitty_reply( detect.at( "id" ), "EPERM:User refused the transfer" ), 2 );
  require( parent.selected() == Parent::Route::Kitty && kitty( parent.take_output() ).at( "ac" ) == "cancel", "probe detected and cleaned up" );
  const std::string bytes( "data\0\xff", 6 );
  parent.submit( begin( 10, "alpine.bin", bytes.size() ), 3 );
  auto send = kitty( parent.take_output() ); const std::string id = send.at( "id" );
  require( send.at( "ac" ) == "send" && send.size() == 2 && id != detect.at( "id" ), "real Kitty request has no authorization bypass" );
  Record r; require( !parent.pop( r ) && parent.take_output().empty(), "wait for Kitty user permission" );
  parent.filter( kitty_reply( id, "OK" ), 4 );
  auto file = kitty( parent.take_output() );
  require( file.at( "ac" ) == "file" && file.at( "n" ) == b64( "~/Downloads/alpine.bin" ) && file.at( "prm" ) == "384", "home belongs to parent, metadata private regular file" );
  parent.filter( kitty_reply( id, "STARTED", ";fid=f0;sz=99" ), 5 );
  file = kitty( parent.take_output() );
  require( file.at( "fid" ) == "f1" && file.at( "n" ) == b64( "~/Downloads/alpine.bin.1" ) && !parent.pop( r ), "known collision gets suffix before any bytes" );
  parent.filter( kitty_reply( id, "STARTED", ";fid=f1;sz=-1" ), 6 ); expect( parent, 10, "approved" );
  parent.submit( data( 10, bytes ), 7 ); parent.submit( end( 10, bytes ), 8 );
  auto payload = kitty( parent.take_output() ); auto ended = kitty( parent.take_output() );
  require( payload.at( "d" ) == b64( bytes ) && payload.at( "fid" ) == "f1" && ended.at( "ac" ) == "end_data", "Kitty inline binary data and end marker" );
  parent.filter( kitty_reply( id, "PROGRESS", ";fid=f1;sz=6" ), 9 ); require( !parent.pop( r ), "progress is not completion" );
  parent.filter( kitty_reply( id, "OK", ";fid=f1;sz=6" ), 10 );
  require( parent.pop( r ) && r.status() == "saved" && r.name() == "alpine.bin.1", "Kitty actual file completion" );
  require( kitty( parent.take_output() ).at( "ac" ) == "finish", "Kitty session is finished" );
  require( parent.filter( kitty_reply( id, "OK", ";fid=f1;sz=6" ), 11 ).empty() && !parent.pop( r ), "late Kitty replies never reach shell" );
  parent.submit( begin( 11, "denied", 0 ), 12 ); send = kitty( parent.take_output() );
  parent.filter( kitty_reply( send.at( "id" ), "EPERM:User refused the transfer" ), 13 ); expect( parent, 11, "error" );
  require( parent.selected() == Parent::Route::Kitty && kitty( parent.take_output() ).at( "ac" ) == "cancel", "real refusal never falls back to local save" );
}

void timeouts_and_validation()
{
  Parent parent( Crypto::Mode::LegacyOCB ); const auto id = probe( parent );
  parent.filter( "\033", 1 ); require( parent.expire_input( 25 ).empty() && parent.expire_input( 26 ) == "\033", "ordinary escape has a bounded wait" );
  parent.tick( 5000 ); auto k = kitty( parent.take_output() );
  parent.tick( 10000 ); require( parent.selected() == Parent::Route::Local && kitty( parent.take_output() ).at( "ac" ) == "cancel", "silent terminals select local permission fallback" );
  parent.filter( Download::status( id, "supported", ":approval=1" ) + kitty_reply( k.at( "id" ), "EPERM" ), 10001 );
  require( parent.selected() == Parent::Route::Local && parent.take_output().empty(), "late probes cannot switch a live route" );
  Parent p( Crypto::Mode::LegacyOCB ); auto probe_id = probe( p );
  p.filter( Download::status( probe_id, "supported", ":approval=1" ), 1 );
  p.submit( begin( 1, "timeout", 0 ), 2 ); auto started = goblin( p.take_output() );
  p.tick( 120003 ); expect( p, 1, "error" ); require( goblin( p.take_output() ).op == "cancel", "parent confirmation timeout cancels request" );
  p.filter( Download::status( started.id, "approved" ), 120004 ); Record r; require( !p.pop( r ), "late approval cannot revive expired job" );
  p.submit( begin( 2, "bad", 1 ), 120005 ); started = goblin( p.take_output() );
  p.filter( Download::status( started.id, "approved" ), 120006 ); expect( p, 2, "approved" );
  p.submit( data( 2, "x" ), 120007 ); p.submit( end( 2, "y" ), 120008 );
  expect( p, 2, "error" ); require( goblin( p.take_output() ).op == "cancel" && p.take_output().empty(), "checksum failure removes unsent payload, never forwards end" );
  p.submit( begin( 3, "pending", 0 ), 120009 ); started = goblin( p.take_output() );
  require( goblin( p.close() ).op == "cancel", "disconnect cancels pending parent transfer" );
}
}

int main()
{
  try { goblin_forward( true ); goblin_forward( false ); kitty_forward(); timeouts_and_validation(); }
  catch ( const std::exception& error ) { std::cerr << "download-forward: " << error.what() << '\n'; return 1; }
  std::cout << "Goblin/Kitty download handoff, consent, framing, collisions and timeout tests passed\n";
}
