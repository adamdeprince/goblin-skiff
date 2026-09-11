/* Distributed under the GNU GPL, version 3 or later. */
#include "mimeclipboard.h"
#include "src/fec/reedsolomon.h"
#include <algorithm>
#include <climits>
#include <zstd.h>

namespace Clipboard {
using Network::Bulk::Datagram;
using Network::Bulk::PacketType;
namespace {
constexpr unsigned SYMBOL_SIZE = 384, WINDOW = 8;
constexpr size_t QUEUE_LIMIT = 96 * 1024 * 1024;
std::string pack( const std::string& body, bool force_zstd )
{
  std::string compressed( ZSTD_compressBound( body.size() ), '\0' );
  const size_t n = ZSTD_compress( &compressed[0], compressed.size(), body.data(), body.size(), 22 );
  if ( !ZSTD_isError( n ) && ( force_zstd || n < body.size() ) ) { compressed.resize( n ); return "\1" + compressed; }
  return std::string( 1, '\0' ) + body;
}
bool unpack( const std::string& data, std::string& body, bool downloads )
{
  if ( data.empty() ) { return false; }
  if ( data[0] == 0 && !downloads ) { body = data.substr( 1 ); }
  else if ( data[0] == 1 ) {
    body.resize( Terminal::OSC5522_MAX_FRAME );
    const size_t n = ZSTD_decompress( &body[0], body.size(), data.data() + 1, data.size() - 1 );
    if ( ZSTD_isError( n ) ) { body.clear(); return false; }
    body.resize( n );
  } else { return false; }
  Terminal::MimeClipboardMessage message;
  return downloads ? !body.empty() : Terminal::parse_osc5522( body, message );
}
}

bool Channel::enqueue( const std::vector<std::string>& bodies, Priority priority )
{
  size_t bytes = 0;
  for ( const auto& body : bodies ) {
    if ( body.empty() || body.size() > Terminal::OSC5522_MAX_FRAME || body.size() > QUEUE_LIMIT - bytes ) { return false; }
    bytes += body.size();
  }
  if ( bytes > QUEUE_LIMIT - queued_bytes ) { return false; }
  auto& lane = lanes[size_t( priority )];
  for ( const auto& body : bodies ) { lane.queued.push_back( body ); }
  queued_bytes += bytes;
  return true;
}

bool Channel::receive( const Datagram& p )
{
  const auto symbol_type = files ? PacketType::FileSymbol : downloads ? PacketType::DownloadSymbol : PacketType::ClipboardSymbol;
  const auto ack_type = files ? PacketType::FileAck : downloads ? PacketType::DownloadAck : PacketType::ClipboardAck;
  if ( p.type != symbol_type && p.type != ack_type ) { return false; }
  // Reserved packet types must never be broadcast to file-transfer clients.
  if ( p.transfer_id < 1 || p.transfer_id > 2 || !p.block_id ) { return true; }
  auto& lane = lanes[p.transfer_id - 1];
  if ( p.type == ack_type ) {
    const auto it = lane.sending.find( p.block_id );
    if ( p.payload.empty() && p.symbol_id == 0 && it != lane.sending.end() ) {
      queued_bytes -= it->second.body.size(); lane.sending.erase( it );
    }
    return true;
  }
  if ( p.block_id <= lane.delivered ) {
    // Re-ACK recently delivered records when an ACK was lost; bound memory
    // even if a peer replays arbitrarily old encrypted application packets.
    if ( lane.delivered - p.block_id < WINDOW ) { lane.acknowledgments.insert( p.block_id ); }
    return true;
  }
  if ( uint64_t( p.block_id ) > uint64_t( lane.delivered ) + WINDOW || p.payload.size() != SYMBOL_SIZE + 2 ) { return true; }
  const unsigned n = ( uint8_t( p.payload[0] ) << 8 ) | uint8_t( p.payload[1] );
  if ( !n || n > Terminal::OSC5522_MAX_FRAME + 1 ) { return true; }
  const unsigned k = ( n + SYMBOL_SIZE - 1 ) / SYMBOL_SIZE;
  if ( p.symbol_id >= k + ( k + 7 ) / 8 ) { return true; }
  auto& receive = lane.receiving[p.block_id];
  if ( !receive.body.empty() ) { return true; }
  if ( receive.symbols.empty() ) {
    receive.metadata.codec = FEC::CodecKind::ReedSolomon;
    receive.metadata.original_size = n; receive.metadata.symbol_size = SYMBOL_SIZE; receive.metadata.source_symbols = k;
  }
  if ( receive.metadata.original_size != n ) { return true; }
  for ( const auto& s : receive.symbols ) { if ( s.id == p.symbol_id ) { return true; } }
  receive.symbols.emplace_back( p.symbol_id, p.payload.substr( 2 ) );
  if ( receive.symbols.size() >= k ) {
    auto codec = FEC::make_reed_solomon_codec();
    std::string encoded;
    std::string body;
    if ( codec->decode( receive.metadata, receive.symbols, encoded ) && unpack( encoded, body, downloads ) ) {
      receive.body.swap( body ); receive.symbols.clear();
    }
  }
  return true;
}

bool Channel::take_packet( Priority priority, uint64_t now, unsigned rtt, Datagram& packet )
{
  // ACKs always use the interactive scheduler, even for background data.
  if ( priority == Priority::Interactive ) {
    for ( size_t i = 0; i < lanes.size(); i++ ) {
      auto& acks = lanes[i].acknowledgments;
      if ( !acks.empty() ) {
        packet = Datagram(); packet.type = files ? PacketType::FileAck : downloads ? PacketType::DownloadAck : PacketType::ClipboardAck; packet.transfer_id = i + 1;
        packet.block_id = *acks.begin(); acks.erase( acks.begin() ); return true;
      }
    }
  }
  auto& lane = lanes[size_t( priority )];
  if ( now < lane.next_send ) { return false; }
  if ( !lane.queued.empty() && lane.sending.size() < WINDOW
       && ( lane.sending.empty() || uint64_t( lane.sent ) < uint64_t( lane.sending.begin()->first ) + WINDOW - 1 ) ) {
    Sending send;
    send.body = std::move( lane.queued.front() ); lane.queued.pop_front();
    const std::string encoded = pack( send.body, downloads );
    const unsigned k = ( encoded.size() + SYMBOL_SIZE - 1 ) / SYMBOL_SIZE;
    auto codec = FEC::make_reed_solomon_codec();
    send.block = codec->encode( encoded, SYMBOL_SIZE, ( k + 7 ) / 8 );
    lane.sending.emplace( ++lane.sent, std::move( send ) );
  }
  for ( auto& entry : lane.sending ) {
    auto& send = entry.second;
    if ( send.next >= send.block.symbols.size() ) {
      if ( now < send.retry ) { continue; }
      send.next = 0;
    }
    const auto& symbol = send.block.symbols[send.next++];
    packet = Datagram(); packet.type = files ? PacketType::FileSymbol : downloads ? PacketType::DownloadSymbol : PacketType::ClipboardSymbol; packet.transfer_id = size_t( priority ) + 1;
    packet.block_id = entry.first; packet.symbol_id = symbol.id;
    const unsigned n = send.block.metadata.original_size;
    packet.payload.push_back( char( n >> 8 ) ); packet.payload.push_back( char( n & 255 ) ); packet.payload += symbol.payload;
    send.retry = now + std::max( 300U, std::min( 10000U, 2 * rtt + 100 ) );
    // A negotiated connection-wide wire-byte pacer replaces the legacy
    // fixed delay. Keeping both would cap files near 150 kbit/s on a LAN.
    // The frontend still gives keyboard/screen/socket work first priority.
    lane.next_send = now + ( externally_paced ? 0 : priority == Priority::Interactive ? 4 : 20 );
    return true;
  }
  return false;
}

bool Channel::take_output( std::string& body )
{
  for ( auto& lane : lanes ) {
    const auto it = lane.receiving.find( lane.delivered + 1 );
    if ( it == lane.receiving.end() || it->second.body.empty() ) { continue; }
    body = std::move( it->second.body ); lane.receiving.erase( it );
    lane.acknowledgments.insert( ++lane.delivered );
    return true;
  }
  return false;
}

bool Channel::has_interactive() const
{
  return !lanes[0].queued.empty() || !lanes[0].sending.empty()
         || !lanes[0].acknowledgments.empty() || !lanes[1].acknowledgments.empty();
}

bool Channel::idle() const { return !queued_bytes && !has_interactive(); }

void Channel::discard_queued( const std::function<bool( const std::string& )>& discard )
{
  for ( auto& lane : lanes ) {
    for ( auto it = lane.queued.begin(); it != lane.queued.end(); ) {
      if ( discard( *it ) ) { queued_bytes -= it->size(); it = lane.queued.erase( it ); }
      else { ++it; }
    }
  }
}

int Channel::wait_time( uint64_t now, bool background ) const
{
  uint64_t deadline = UINT64_MAX;
  for ( size_t i = 0; i < lanes.size(); i++ ) {
    const auto& lane = lanes[i];
    if ( !lane.acknowledgments.empty() ) { return 0; }
    const auto received = lane.receiving.find( lane.delivered + 1 );
    if ( received != lane.receiving.end() && !received->second.body.empty() ) { return 0; }
    if ( i && !background ) { continue; }
    uint64_t ready = UINT64_MAX;
    if ( !lane.queued.empty() && lane.sending.size() < WINDOW
         && ( lane.sending.empty() || uint64_t( lane.sent ) < uint64_t( lane.sending.begin()->first ) + WINDOW - 1 ) ) { ready = now; }
    for ( const auto& send : lane.sending ) {
      ready = std::min( ready, send.second.next < send.second.block.symbols.size() ? now : send.second.retry );
    }
    deadline = std::min( deadline, std::max( lane.next_send, ready ) );
  }
  return deadline <= now ? 0 : int( std::min( uint64_t( INT_MAX ), deadline - now ) );
}

void Endpoint::error( const Terminal::MimeClipboardMessage& message, const std::string& status )
{
  if ( local_errors.size() < 16 ) { local_errors.push_back( Terminal::osc5522_error( message, status ) ); }
}

void Endpoint::submit( const std::string& body, uint64_t now )
{
  Terminal::MimeClipboardMessage m;
  if ( !Terminal::parse_osc5522( body, m ) ) { return; }
  if ( !server ) {
    // Every local-to-remote clipboard packet, including arbitrary MIME blobs,
    // is interactive. No size test, MIME test, or background fallback here.
    if ( !channel.enqueue( { body }, Priority::Interactive ) ) { error( m, "EIO" ); }
    return;
  }
  if ( m.type == "write" && m.status.empty() ) {
    if ( writing || awaiting_write ) { error( m, "EBUSY" ); return; }
    writing = true; plain_text = true; write_bytes = buffered_bytes = 0;
    request = m; buffered.clear(); last_write = now;
  }
  if ( ( m.type == "write" && m.status.empty() ) || m.type == "wdata" || m.type == "walias" ) {
    if ( !writing ) { return; }
    if ( !m.id.empty() && m.id != request.id ) { return; }
    if ( m.data.size() > Terminal::OSC5522_MAX_TRANSFER - write_bytes
         || body.size() > QUEUE_LIMIT - buffered_bytes ) {
      error( request, "EIO" ); writing = false; buffered.clear(); return;
    }
    write_bytes += m.data.size(); buffered_bytes += body.size(); last_write = now;
    if ( m.type == "wdata" && !m.write_end() && !m.text() ) { plain_text = false; }
    buffered.push_back( body );
    if ( m.write_end() ) {
      const Priority priority = plain_text || write_bytes <= threshold ? Priority::Interactive : Priority::Background;
      if ( channel.enqueue( buffered, priority ) ) { awaiting_write = true; }
      else { error( request, "EIO" ); }
      buffered.clear(); writing = false;
    }
    return;
  }
  // Requests, replies, and permission/error metadata are interactive. OSC 52
  // terminal text continues to use its existing reliable fast path as well.
  if ( !channel.enqueue( { body }, Priority::Interactive ) ) { error( m, "EBUSY" ); }
}

bool Endpoint::take_output( std::string& body )
{
  if ( !local_errors.empty() ) { body = std::move( local_errors.front() ); local_errors.pop_front(); return true; }
  if ( !channel.take_output( body ) ) { return false; }
  Terminal::MimeClipboardMessage m;
  if ( server && Terminal::parse_osc5522( body, m ) && m.type == "write" && !m.status.empty() && m.id == request.id ) { awaiting_write = false; }
  return true;
}

void Endpoint::expire( uint64_t now )
{
  if ( writing && now - last_write >= 120000 ) { error( request, "EIO" ); writing = false; buffered.clear(); }
}
}
