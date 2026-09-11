/* Distributed under the GNU GPL, version 3 or later. */
#ifndef GOBLIN_FILETRANSFER_H
#define GOBLIN_FILETRANSFER_H

#include "directory.h"
#include "mimeclipboard.h"
#include "src/crypto/crypto.h"
#include <deque>

namespace Files {
using Command = ControlBuffers::FileControl;
using Record = ControlBuffers::FileRecord;
constexpr size_t CHUNK = 8192;
constexpr size_t MAX_RECORD = CHUNK + 4096;
bool available();
bool valid_command( const Command& command );

// One bounded, killable filesystem worker per endpoint. The parent never
// scans, hashes, patches or opens a transfer path. Bytes in both directions
// (including librsync signatures) stay outside screen-state synchronization.
class Endpoint {
  struct Job { Command local, remote; };
  bool server, enabled, peer_ready = false, completed = false;
  Crypto::Mode mode;
  uint64_t serial = 0, last_remote = 0;
  uint64_t next_status = 0;
  pid_t child = -1;
  int socket = -1;
  Command current, status, pending_status;
  bool status_pending = false;
  std::deque<Job> queued;
  std::deque<Command> controls;
  std::deque<Command> history;
  std::string incoming, outgoing;
  bool spawn( uint64_t now );
  void stop();
  void fail( const std::string& text );
  void remember( const Command& value );
  void progress( const Record& record );
  void count( const Record& record );

public:
  Clipboard::Channel channel { Clipboard::Channel::Records::Files };
  Endpoint( bool server_side, bool negotiated, Crypto::Mode crypto_mode )
    : server( server_side ), enabled( negotiated && available() ), mode( crypto_mode ) {}
  ~Endpoint();
  Endpoint( const Endpoint& ) = delete;
  Endpoint& operator=( const Endpoint& ) = delete;
  bool supported() const { return enabled; }
  bool queue( bool upload, const std::string& source, const std::string& destination,
              const std::string& name );
  void cancel();
  void receive_control( const Command& command, uint64_t now );
  void tick( uint64_t now );
  bool take_control( Command& command );
  void flush_controls( Control::Channel& destination );
  int fd() const;
  int wait_time( uint64_t now, bool background_allowed ) const;
  bool take_packet( Clipboard::Priority priority, uint64_t now, unsigned rtt,
                    Network::Bulk::Datagram& datagram );
  const std::deque<Command>& jobs() const { return history; }
  size_t queue_size() const { return queued.size(); }
  pid_t pid() const { return child; }
};
}
#endif
