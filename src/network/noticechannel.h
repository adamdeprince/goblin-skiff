/*
    Goblin Skiff: notifications, ahead of everything else
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef NETWORK_NOTICECHANNEL_H
#define NETWORK_NOTICECHANNEL_H

#include <climits>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <string>

#include "src/network/bulkdatagram.h"

/* A desktop notification is for a person who is looking elsewhere, and says
   something happened: it is worth nothing behind a screen update that takes
   a slow link a minute to carry. So notifications do not ride the state the
   two ends synchronize. They cross in datagrams of their own on the same
   authenticated connection, which the sender writes before anything else it
   has to send, and the answers a terminal gives (a notification was clicked,
   or closed) come back the same way.

   The channel is reliable and ordered: every message arrives once, whole,
   and in the order it was queued, whatever is lost, repeated or reordered
   on the way. A message has no length limit. It crosses as segments, each a
   message of the wire with a number of its own, and a segment as as many
   datagrams as the path needs at the time, each acknowledged by the bytes
   it carried, so a path whose datagrams grow smaller mid-message (a relay
   hop, a radio) loses nothing.

   Datagram (see bulkdatagram.h):
     Notice     transfer_id  the number of the segment, from 1
                block_id     the offset of these bytes in the segment
                symbol_id    the length of the segment
                payload      the bytes
     NoticeAck  transfer_id, block_id as received; symbol_id the count of
                bytes received there
   The first byte of a segment is 0 when it ends a message and 1 when more
   of the message follows. */

namespace Network {

class NoticeChannel
{
public:
  /* The most a segment holds, so that its offsets and length fit the
     32-bit fields of a datagram with room to spare; a longer message is
     several segments. A receiver refuses a segment that claims more. */
  static const size_t SEGMENT_MAX = size_t( 1 ) << 20;
  /* How many datagrams may be on their way unacknowledged: flow control for
     a path nothing else paces, not a bound on what can be sent. */
  static const size_t WINDOW = 64;

  /* Queues a message for the peer. An empty message is nothing. */
  void enqueue( const std::string& message );
  /* A datagram from the peer. False when it is not this channel's. */
  bool receive( const Bulk::Datagram& packet );
  /* The next datagram to write, if one is due. max_datagram is what the path
     carries of an encoded datagram. Acknowledgments are always due; the
     messages themselves are held while the peer is not being heard from, so
     that a peer that has gone is not written to for nothing. */
  bool take_packet( uint64_t now, unsigned rtt_ms, size_t max_datagram, bool peer_active, Bulk::Datagram& packet );
  /* The next message from the peer, in the order it queued them. */
  bool take_message( std::string& message );
  /* Milliseconds until take_packet has something: 0 now, INT_MAX never. */
  int wait_time( uint64_t now, bool peer_active ) const;
  /* Nothing queued, nothing unacknowledged and nothing to acknowledge. */
  bool idle( void ) const { return outgoing.empty() && acks.empty(); }
  size_t unacknowledged( void ) const { return outgoing.size(); }

private:
  struct Chunk
  {
    uint32_t length = 0;
    uint64_t retry_at = 0;
    unsigned tries = 0;
  };
  struct Outgoing
  {
    uint64_t number = 0;
    std::string bytes;                 /* the flag byte and the segment's part of the message */
    size_t next = 0;                   /* the first byte never sent */
    std::map<uint32_t, Chunk> flying;  /* by offset: sent and not acknowledged */
    bool done( void ) const { return next == bytes.size() && flying.empty(); }
  };
  struct Incoming
  {
    std::string bytes;
    std::map<uint32_t, uint32_t> have; /* the ranges received: offset to end, disjoint */
    size_t count = 0;                  /* bytes received */
  };
  struct Ack
  {
    uint64_t number;
    uint32_t offset, length;
  };

  std::list<Outgoing> outgoing;
  uint64_t queued = 0; /* the number of the last segment queued */
  size_t flying = 0;   /* chunks on their way, over all segments */

  std::map<uint64_t, Incoming> incoming;
  uint64_t delivered = 0; /* the number of the last segment taken in */
  std::string assembling; /* the segments so far of a message that has more */
  std::deque<std::string> ready;
  std::deque<Ack> acks;

  void acknowledged( Outgoing& segment, uint32_t offset, uint32_t length );
  static uint64_t retry_delay( unsigned rtt_ms, unsigned tries );
};

}

#endif
