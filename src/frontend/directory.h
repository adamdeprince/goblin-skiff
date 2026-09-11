/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_DIRECTORY_H
#define MOSH_DIRECTORY_H

#include <cstdint>
#include <deque>
#include <string>
#include <sys/types.h>

#include "src/protobufs/control.pb.h"
#include "src/statesync/stream.h"

namespace Control {
using Message = ControlBuffers::Directory;
constexpr size_t MAX_MESSAGE = 8192;
constexpr size_t PAGE_BYTES = 4096;
constexpr size_t PAGE_ENTRIES = 64;
constexpr uint64_t STREAM_ID = 0;

std::string frame( const Message& message );
// Empty input means incomplete, oversized/malformed input throws.
bool unframe( std::string& input, Message& message );

// A bounded, ACK-clocked side channel within SST. At most one 512-byte
// chunk can be unacknowledged; keyboard/screen updates never wait for it.
class Channel
{
  std::string incoming, outgoing;
  uint64_t barrier = 0, next_send = 0;

public:
  bool queue( const Message& message );
  void receive( const std::string& data );
  bool pop( Message& message ) { return unframe( incoming, message ); }
  int wait_time( uint64_t now, uint64_t acked ) const;
  bool take( uint64_t now, uint64_t last, uint64_t acked, Network::StreamEvent& event );
  size_t queued_bytes() const { return outgoing.size(); }
};

// One killable filesystem worker per endpoint. No filesystem lookup,
// blocking IPC, or blocking waitpid is permitted on the session thread.
// A stuck, uninterruptible child must be reaped before a replacement starts.
class DirectoryWorker
{
  pid_t child = -1;
  int socket = -1;
  bool busy = false, restarting = false, pending = false;
  bool watching = false, watch_pending = false;
  bool native_notifications;
  uint64_t started = 0, next_check = 0;
  Message current, wanted, watch_request;
  std::string incoming, outgoing;
  std::deque<Message> replies;
  bool spawn();
  void stop();
  void fail( const std::string& error );

public:
  // Polling-only mode also exercises the fallback used without native events.
  explicit DirectoryWorker( bool use_native_notifications = true ) : native_notifications( use_native_notifications ) {}
  ~DirectoryWorker();
  DirectoryWorker( const DirectoryWorker& ) = delete;
  DirectoryWorker& operator=( const DirectoryWorker& ) = delete;
  void request( const Message& message, uint64_t now );
  void tick( uint64_t now );
  int fd() const { return socket; }
  int wait_time() const;
  bool pop( Message& message );
  // Read-only diagnostic, also allows deterministic stopped-worker tests.
  pid_t pid() const { return child; }
};
}
#endif
