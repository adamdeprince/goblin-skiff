// SPDX-License-Identifier: GPL-3.0-or-later
#include "relay.h"

#include <stdexcept>

using namespace Network::Relay;

namespace {
constexpr uint64_t DIRECTION = uint64_t( 1 ) << 63;
}

unsigned Network::Relay::overhead( Crypto::Mode mode, unsigned hops )
{
  if ( hops > MAX_HOPS ) { throw std::invalid_argument( "Too many UDP relay hops (maximum 4)" ); }
  return hops * ( mode == Crypto::Mode::FipsAES128GCM ? 36 : 24 );
}

Cipher::Cipher( const std::string& key, Crypto::Mode mode, bool server_side )
  : session( Crypto::Base64Key( key ), mode, server_side ? Crypto::Endpoint::Server : Crypto::Endpoint::Client ),
    server( server_side )
{}

std::string Cipher::seal( const std::string& plaintext )
{
  if ( plaintext.size() + 8 + session.added_bytes() > WIRE_MTU ) {
    throw Crypto::CryptoException( "UDP relay packet exceeds MTU" );
  }
  if ( next_send >= DIRECTION ) { throw Crypto::CryptoException( "UDP relay nonce exhausted", true ); }
  return session.encrypt( Crypto::Message( Crypto::Nonce( next_send++ | ( server ? DIRECTION : 0 ) ), plaintext ) );
}

std::string Cipher::open( const std::string& wire, bool& newest )
{
  newest = false;
  if ( wire.size() > WIRE_MTU ) { throw Crypto::CryptoException( "Oversized UDP relay packet" ); }
  const auto message = session.decrypt( wire );
  const uint64_t nonce = message.nonce.val(), seq = nonce & ~DIRECTION;
  if ( bool( nonce & DIRECTION ) == server || seq == 0 ) {
    throw Crypto::CryptoException( "Wrong UDP relay direction or sequence" );
  }
  if ( seq > highest_received ) {
    const uint64_t advance = seq - highest_received;
    if ( advance >= received.size() ) { received.reset(); }
    else { received <<= static_cast<size_t>( advance ); }
    highest_received = seq;
    newest = true;
  } else if ( highest_received - seq >= received.size() || received[highest_received - seq] ) {
    throw Crypto::CryptoException( "Replayed UDP relay packet" );
  }
  received.set( highest_received - seq );
  return message.text;
}

void Chain::configure( const std::string& keys, Crypto::Mode mode )
{
  if ( !hops.empty() ) { throw std::logic_error( "UDP relay keys may only be configured once" ); }
  std::vector<std::unique_ptr<Cipher>> configured;
  std::vector<std::string> seen;
  size_t start = 0;
  while ( start < keys.size() ) {
    const size_t end = keys.find( ',', start );
    const auto key = keys.substr( start, end == std::string::npos ? end : end - start );
    if ( key.size() != 22 || configured.size() == MAX_HOPS ) {
      throw std::invalid_argument( "Invalid UDP relay key list" );
    }
    for ( const auto& previous : seen ) {
      if ( key == previous ) { throw std::invalid_argument( "UDP relay hops require independent keys" ); }
    }
    seen.push_back( key );
    configured.emplace_back( new Cipher( key, mode, false ) );
    if ( end == std::string::npos ) { break; }
    start = end + 1;
    if ( start == keys.size() ) { throw std::invalid_argument( "Empty UDP relay key" ); }
  }
  if ( configured.empty() ) { throw std::invalid_argument( "Missing UDP relay keys" ); }
  hops.swap( configured );
}

std::string Chain::seal( std::string packet )
{
  for ( auto it = hops.rbegin(); it != hops.rend(); ++it ) { packet = ( *it )->seal( packet ); }
  return packet;
}

std::string Chain::open( std::string packet )
{
  bool newest = false;
  for ( auto& hop : hops ) { packet = hop->open( packet, newest ); }
  if ( !hops.empty() && packet.empty() ) { throw Crypto::CryptoException( "Empty UDP relay reply" ); }
  return packet;
}

std::string Chain::close_packet( unsigned hop )
{
  if ( hop >= hops.size() ) { throw std::out_of_range( "UDP relay hop" ); }
  std::string packet;
  for ( unsigned i = hop + 1; i; --i ) { packet = hops[i - 1]->seal( packet ); }
  return packet;
}
