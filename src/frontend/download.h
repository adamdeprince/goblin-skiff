/* Distributed under the GNU GPL, version 3 or later. */
#ifndef FRONTEND_DOWNLOAD_H
#define FRONTEND_DOWNLOAD_H
#include <memory>
#include <sys/types.h>
#include "mimeclipboard.h"
#include "src/protobufs/download.pb.h"
#include "src/terminal/download.h"
#include "downloadforward.h"

namespace Download {
using Record = DownloadBuffers::Record;
bool decode_record( const std::string& bytes, Record& record );
std::string encode_record( const Record& record );

class Sender {
  struct Job {
    uint64_t token = 0, size = 0, received = 0, updated = 0;
    uint32_t checksum = 0;
    bool reply = false, confirm = false, ended = false, approved = false;
    std::deque<std::string> waiting {};
  };
  bool enabled = false;
  uint64_t next_token = 0, reserved = 0;
  size_t waiting_bytes = 0;
  std::map<uint32_t, Job> jobs {};
  std::deque<std::string> replies {};
  void reply( uint32_t id, const std::string& state, const std::string& extra = "" );
  void cancel( uint32_t id, const std::string& reason );
public:
  Clipboard::Channel channel { true };
  void set_enabled( bool value ) { enabled = value; }
  void submit( const std::string& body, uint64_t now );
  void tick( uint64_t now );
  std::string take_replies();
  int wait_time( uint64_t now, bool background_allowed ) const;
};

// Runs exclusively in the filesystem worker (also tested independently).
// All paths supplied by the remote are single names under a pinned directory
// descriptor. No filesystem paths are interpreted by the network loop.
class Sink {
  struct File {
    int fd = -1;
    std::string temporary {}, name {};
    uint64_t size = 0, written = 0;
    uint32_t checksum = 0;
  };
  std::string directory;
  Crypto::Mode mode;
  int root = -1;
  uint64_t reserved = 0, last_token = 0;
  std::map<uint64_t, File> files {};
  std::function<bool()> can_publish;
  void remove( std::map<uint64_t, File>::iterator file );
public:
  explicit Sink( const std::string& path, Crypto::Mode crypto_mode = Crypto::Mode::LegacyOCB,
                 std::function<bool()> publish_allowed = {} )
    : directory( path ), mode( crypto_mode ), can_publish( std::move( publish_allowed ) ) {}
  ~Sink();
  Sink( const Sink& ) = delete;
  Sink& operator=( const Sink& ) = delete;
  Record handle( const Record& record );
};

class Writer {
  std::string directory;
  Crypto::Mode mode;
  pid_t child = -1;
  int socket = -1;
  bool busy = false;
  uint64_t token = 0, started = 0;
  std::string incoming {}, outgoing {};
  std::deque<Record> replies {};
  bool spawn();
  void fail( const std::string& message );
public:
  explicit Writer( const std::string& path, Crypto::Mode crypto_mode ) : directory( path ), mode( crypto_mode ) {}
  ~Writer();
  Writer( const Writer& ) = delete;
  Writer& operator=( const Writer& ) = delete;
  bool ready() const { return !busy && ( socket >= 0 || child < 0 ); }
  bool submit( const Record& record, uint64_t now );
  void tick( uint64_t now );
  bool pop( Record& record );
  int fd() const { return socket; }
  int wait_time() const;
  pid_t pid() const { return child; }
};

class Receiver {
  struct Offer {
    Record begin;
    uint64_t created = 0, local_token = 0;
    bool approved = false, starting = false, active = false, forwarded = false;
  };
  Writer writer;
  Parent parent;
  std::string destination;
  std::deque<Record> pending_status {};
  std::map<uint64_t, Offer> offers {};
  uint64_t last_token = 0, reserved = 0, next_local_token = 0;
  void erase( uint64_t token );
public:
  Clipboard::Channel channel { true };
  explicit Receiver( const std::string& directory, Crypto::Mode crypto_mode )
    : writer( directory, crypto_mode ), parent( crypto_mode ), destination( directory ) {}
  void enable_forwarding( uint64_t now ) { parent.enable(); parent.discover( now ); }
  std::string filter_input( const std::string& bytes, uint64_t now ) { return parent.filter( bytes, now ); }
  std::string expire_input( uint64_t now ) { return parent.expire_input( now ); }
  std::string take_terminal_output() { return parent.take_output(); }
  std::string close_forwarding() { return parent.close(); }
  void tick( uint64_t now );
  bool pending( Record& request ) const;
  void decide( uint64_t token, bool allow );
  const std::string& directory() const { return destination; }
  int fd() const { return writer.fd(); }
  int wait_time( uint64_t now, int pacing_wait = 0 ) const;
};
std::string downloads_directory();
}
#endif
