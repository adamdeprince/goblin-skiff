// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MOSH_SOCKS5_H
#define MOSH_SOCKS5_H

#include <cstdint>
#include <string>
#include <vector>
#include <sys/socket.h>

namespace Network {

// A SOCKS5 UDP association is a local transport for already encrypted Mosh
// datagrams, not a stream tunnel. Never queue datagrams while disconnected.
class Socks5UDP
{
  struct Endpoint {
    sockaddr_storage addr {};
    socklen_t length = 0;
  };
  enum class State { Backoff, Connecting, Greeting, Associate, Ready };
  std::vector<Endpoint> proxies {};
  size_t proxy_index = 0;
  std::string destination {}, output {}, input {}, last_error {};
  size_t written = 0;
  int control = -1, datagram = -1;
  State state = State::Backoff;
  uint64_t deadline = 0, retry_at = 0, generation = 0;
  unsigned retry_delay = 250;

  void close_sockets();
  void failed( const std::string& error, uint64_t now );
  void start( uint64_t now );
  void associate();

public:
  Socks5UDP( const std::string& proxy, const std::string& host, const std::string& port );
  ~Socks5UDP();
  Socks5UDP( const Socks5UDP& ) = delete;
  Socks5UDP& operator=( const Socks5UDP& ) = delete;

  // Validation only: the -c wrapper preflight must not contact the network.
  static void validate_proxy( const std::string& proxy );
  static std::string address( const std::string& host, const std::string& port );
  // Returns an address header's length, zero if incomplete; rejects bad ATYP.
  static size_t address_length( const std::string& bytes, size_t offset );
  void tick( uint64_t now );
  void reconnect( uint64_t now );
  int wait_time( uint64_t now ) const;
  std::vector<int> fds() const;
  bool ready() const { return state == State::Ready; }
  uint64_t epoch() const { return generation; }
  const std::string& error() const { return last_error; }
  ssize_t send( const std::string& ciphertext );
  std::string receive(); // EAGAIN when no complete, acceptable datagram exists
};

}
#endif
