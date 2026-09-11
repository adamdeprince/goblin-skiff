/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MIMECLIPBOARD_HPP
#define MIMECLIPBOARD_HPP
#include <array>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include "src/fec/fec.h"
#include "src/network/bulkdatagram.h"
#include "src/terminal/osc5522.h"

namespace Clipboard {
enum class Priority { Interactive = 0, Background = 1 };

// Two independently flow-controlled, ordered FEC lanes on the existing
// authenticated UDP connection. Neither lane enters the SST screen history.
class Channel {
  struct Sending {
    std::string body {};
    FEC::EncodedBlock block {};
    size_t next = 0;
    uint64_t retry = 0;
  };
  struct Receiving {
    FEC::BlockMetadata metadata {};
    std::vector<FEC::Symbol> symbols {};
    std::string body {};
  };
  struct Lane {
    uint32_t sent = 0, delivered = 0;
    std::deque<std::string> queued {};
    std::map<uint32_t, Sending> sending {};
    std::map<uint32_t, Receiving> receiving {};
    std::set<uint32_t> acknowledgments {};
    uint64_t next_send = 0;
  };
  std::array<Lane, 2> lanes {};
  size_t queued_bytes = 0;
  bool downloads = false;
  bool files = false;
  bool externally_paced = false;
public:
  enum class Records { Files };
  explicit Channel( bool download_records = false ) : downloads( download_records ) {}
  explicit Channel( Records ) : downloads( true ), files( true ) {}
  void set_external_pacing( bool enabled ) { externally_paced = enabled; }
  bool enqueue( const std::vector<std::string>& bodies, Priority priority );
  bool receive( const Network::Bulk::Datagram& packet );
  bool take_packet( Priority priority, uint64_t now, unsigned rtt_ms, Network::Bulk::Datagram& packet );
  bool take_output( std::string& body );
  int wait_time( uint64_t now, bool background_allowed ) const;
  bool has_interactive() const;
  bool idle() const;
  size_t pending_bytes() const { return queued_bytes; }
  void discard_queued( const std::function<bool( const std::string& )>& discard );
};

// Only remote volunteered writes are classified by size. Local replies and
// pasted data never go through the write accumulator or background scheduler.
class Endpoint {
  bool server;
  size_t threshold;
  bool writing = false, awaiting_write = false, plain_text = true;
  size_t write_bytes = 0, buffered_bytes = 0;
  uint64_t last_write = 0;
  Terminal::MimeClipboardMessage request {};
  std::vector<std::string> buffered {};
  std::deque<std::string> local_errors {};
  void error( const Terminal::MimeClipboardMessage& message, const std::string& status );
public:
  Channel channel {};
  explicit Endpoint( bool server_side, size_t cutoff = Terminal::OSC5522_FAST_THRESHOLD ) : server( server_side ), threshold( cutoff ) {}
  void set_threshold( size_t cutoff ) { threshold = std::min( cutoff, Terminal::OSC5522_MAX_TRANSFER ); }
  void submit( const std::string& body, uint64_t now );
  bool take_output( std::string& body );
  void expire( uint64_t now );
};
}
#endif
