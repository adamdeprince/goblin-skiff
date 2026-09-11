// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MOSH_UDP_RELAY_H
#define MOSH_UDP_RELAY_H

#include <bitset>
#include <memory>
#include <string>
#include <vector>

#include "src/crypto/crypto.h"

namespace Network {
namespace Relay {

constexpr unsigned MAX_HOPS = 4;
// Use the IPv6 budget even when one end uses IPv4: intermediate hops may not.
constexpr unsigned WIRE_MTU = 1216;
constexpr unsigned STARTUP_TIMEOUT = 600; // seconds, including multi-hop SSH authentication
constexpr unsigned IDLE_TIMEOUT = 86400; // authenticated client silence; no extra heartbeats

unsigned overhead( Crypto::Mode mode, unsigned hops );

// One independently keyed hop. Empty plaintext means close, not a keepalive.
// A normal Mosh keepalive still contains the inner encrypted Mosh packet.
class Cipher
{
  Crypto::Session session;
  bool server;
  uint64_t next_send = 1, highest_received = 0;
  std::bitset<1024> received {};

public:
  Cipher( const std::string& key, Crypto::Mode mode, bool server_side );
  Cipher( const Cipher& ) = delete;
  Cipher& operator=( const Cipher& ) = delete;
  std::string seal( const std::string& plaintext );
  std::string open( const std::string& wire, bool& newest );
};

// Keys are ordered from the client-facing hop to the destination-facing hop.
class Chain
{
  std::vector<std::unique_ptr<Cipher>> hops {};

public:
  void configure( const std::string& keys, Crypto::Mode mode );
  unsigned size() const { return hops.size(); }
  std::string seal( std::string packet );
  std::string open( std::string packet );
  std::string close_packet( unsigned hop );
};

} // namespace Relay
} // namespace Network
#endif
