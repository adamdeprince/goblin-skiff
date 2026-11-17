/*
    Goblin Skiff: notifications, ahead of everything else
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/network/noticechannel.h"

#include <algorithm>

using namespace Network;
using Bulk::Datagram;
using Bulk::PacketType;

void NoticeChannel::enqueue( const std::string& message )
{
  if ( message.empty() ) {
    return;
  }
  for ( size_t at = 0; at < message.size(); at += SEGMENT_MAX ) {
    Outgoing segment;
    segment.number = ++queued;
    const bool more = message.size() - at > SEGMENT_MAX;
    segment.bytes.assign( 1, more ? '\1' : '\0' );
    segment.bytes.append( message, at, SEGMENT_MAX );
    outgoing.push_back( std::move( segment ) );
  }
}

/* A datagram is tried again after about two round trips, and then ever more
   patiently, so that a path that has slowed is not flooded with repeats. */
uint64_t NoticeChannel::retry_delay( unsigned rtt_ms, unsigned tries )
{
  const uint64_t base = std::max<uint64_t>( 200, std::min<uint64_t>( 3000, 2 * uint64_t( rtt_ms ) + 50 ) );
  return std::min<uint64_t>( 10000, base << std::min( tries, 6u ) );
}

void NoticeChannel::acknowledged( Outgoing& segment, uint32_t offset, uint32_t length )
{
  /* The bytes may be those of one datagram, of several a smaller path made
     of it since, or of part of a larger one. What they do not cover of a
     datagram stays on its way. */
  const uint64_t end = uint64_t( offset ) + length;
  auto it = segment.flying.lower_bound( offset );
  if ( it != segment.flying.begin() ) {
    auto before = std::prev( it );
    if ( uint64_t( before->first ) + before->second.length > offset ) {
      it = before;
    }
  }
  while ( it != segment.flying.end() && it->first < end ) {
    const uint32_t start = it->first;
    const Chunk chunk = it->second;
    const uint64_t chunk_end = uint64_t( start ) + chunk.length;
    it = segment.flying.erase( it );
    flying--;
    if ( start < offset ) {
      Chunk left = chunk;
      left.length = offset - start;
      segment.flying[start] = left;
      flying++;
    }
    if ( chunk_end > end ) {
      Chunk right = chunk;
      right.length = static_cast<uint32_t>( chunk_end - end );
      it = segment.flying.emplace( static_cast<uint32_t>( end ), right ).first;
      flying++;
      ++it;
    }
  }
}

bool NoticeChannel::receive( const Datagram& packet )
{
  if ( packet.type == PacketType::NoticeAck ) {
    for ( auto it = outgoing.begin(); it != outgoing.end(); ++it ) {
      if ( it->number == packet.transfer_id ) {
        acknowledged( *it, packet.block_id, packet.symbol_id );
        if ( it->done() ) {
          outgoing.erase( it );
        }
        break;
      }
    }
    return true;
  }
  if ( packet.type != PacketType::Notice ) {
    return false;
  }
  const uint64_t number = packet.transfer_id;
  const uint32_t offset = packet.block_id, total = packet.symbol_id;
  const size_t length = packet.payload.size();
  if ( !number || !total || total > SEGMENT_MAX + 1 || !length || offset >= total || length > total - offset ) {
    return true; /* not a datagram this channel writes */
  }
  if ( number <= delivered ) {
    /* Its acknowledgment was lost: said again. */
    acks.push_back( { number, offset, static_cast<uint32_t>( length ) } );
    return true;
  }
  Incoming& segment = incoming[number];
  if ( segment.bytes.empty() ) {
    segment.bytes.assign( total, '\0' );
  } else if ( segment.bytes.size() != total ) {
    return true; /* not the segment the rest of it was */
  }
  /* The bytes it brings that were not here already. */
  uint32_t start = offset;
  const uint32_t end = offset + static_cast<uint32_t>( length );
  auto it = segment.have.upper_bound( start );
  if ( it != segment.have.begin() && std::prev( it )->second >= start ) {
    --it;
    start = it->first;
  }
  uint32_t merged_end = end;
  while ( it != segment.have.end() && it->first <= end ) {
    segment.count -= it->second - it->first;
    merged_end = std::max( merged_end, it->second );
    it = segment.have.erase( it );
  }
  segment.have[start] = merged_end;
  segment.count += merged_end - start;
  segment.bytes.replace( offset, length, packet.payload );
  acks.push_back( { number, offset, static_cast<uint32_t>( length ) } );

  /* Segments are taken in by number, so messages come out in order. */
  for ( auto first = incoming.find( delivered + 1 );
        first != incoming.end() && first->second.count == first->second.bytes.size();
        first = incoming.find( delivered + 1 ) ) {
    const bool more = first->second.bytes[0] != '\0';
    assembling.append( first->second.bytes, 1, std::string::npos );
    if ( !more ) {
      ready.push_back( std::move( assembling ) );
      assembling.clear();
    }
    incoming.erase( first );
    delivered++;
  }
  return true;
}

bool NoticeChannel::take_message( std::string& message )
{
  if ( ready.empty() ) {
    return false;
  }
  message = std::move( ready.front() );
  ready.pop_front();
  return true;
}

bool NoticeChannel::take_packet( uint64_t now, unsigned rtt_ms, size_t max_datagram, bool peer_active,
                                 Datagram& packet )
{
  if ( !acks.empty() ) {
    packet = Datagram();
    packet.type = PacketType::NoticeAck;
    packet.transfer_id = acks.front().number;
    packet.block_id = acks.front().offset;
    packet.symbol_id = acks.front().length;
    acks.pop_front();
    return true;
  }
  if ( !peer_active || max_datagram <= Bulk::DATAGRAM_HEADER_LEN ) {
    return false;
  }
  const size_t room = max_datagram - Bulk::DATAGRAM_HEADER_LEN;
  const auto write = [&]( Outgoing& segment, uint32_t offset, uint32_t length ) {
    packet = Datagram();
    packet.type = PacketType::Notice;
    packet.transfer_id = segment.number;
    packet.block_id = offset;
    packet.symbol_id = static_cast<uint32_t>( segment.bytes.size() );
    packet.payload.assign( segment.bytes, offset, length );
  };
  /* What was sent and has waited long enough is sent again, oldest first... */
  for ( Outgoing& segment : outgoing ) {
    for ( auto it = segment.flying.begin(); it != segment.flying.end(); ++it ) {
      if ( it->second.retry_at > now ) {
        continue;
      }
      Chunk& chunk = it->second;
      if ( chunk.length > room ) {
        /* The path carries less than it did: the rest is a datagram of its own, due at once. */
        Chunk rest = chunk;
        rest.length = chunk.length - static_cast<uint32_t>( room );
        rest.retry_at = now;
        chunk.length = static_cast<uint32_t>( room );
        segment.flying[it->first + chunk.length] = rest;
        flying++;
      }
      chunk.tries++;
      chunk.retry_at = now + retry_delay( rtt_ms, chunk.tries );
      write( segment, it->first, chunk.length );
      return true;
    }
  }
  /* ...and then what has never been sent, in order. */
  if ( flying >= WINDOW ) {
    return false;
  }
  for ( Outgoing& segment : outgoing ) {
    if ( segment.next == segment.bytes.size() ) {
      continue;
    }
    Chunk chunk;
    chunk.length = static_cast<uint32_t>( std::min( room, segment.bytes.size() - segment.next ) );
    chunk.retry_at = now + retry_delay( rtt_ms, 0 );
    const uint32_t offset = static_cast<uint32_t>( segment.next );
    segment.flying[offset] = chunk;
    flying++;
    segment.next += chunk.length;
    write( segment, offset, chunk.length );
    return true;
  }
  return false;
}

int NoticeChannel::wait_time( uint64_t now, bool peer_active ) const
{
  if ( !acks.empty() ) {
    return 0;
  }
  if ( !peer_active ) {
    return INT_MAX;
  }
  uint64_t due = UINT64_MAX;
  for ( const Outgoing& segment : outgoing ) {
    if ( segment.next < segment.bytes.size() && flying < WINDOW ) {
      return 0;
    }
    for ( const auto& chunk : segment.flying ) {
      due = std::min( due, chunk.second.retry_at );
    }
  }
  if ( due == UINT64_MAX ) {
    return INT_MAX;
  }
  return due <= now ? 0 : static_cast<int>( std::min<uint64_t>( INT_MAX, due - now ) );
}
