/* Distributed under the GNU GPL, version 3 or later. */
#ifndef GOBLIN_DOWNLOAD_FORWARD_H
#define GOBLIN_DOWNLOAD_FORWARD_H
#include <deque>
#include <map>
#include <string>
#include "src/crypto/crypto.h"
#include "src/protobufs/download.pb.h"

namespace Download {
// One connection-scoped handoff to the terminal enclosing this client. The
// terminal, not an intermediate Mosh process, owns final permission/storage.
class Parent {
public:
  enum class Route { Local, Unknown, ProbeGoblin, ProbeKitty, Goblin, Kitty };
private:
  using Record = DownloadBuffers::Record;
  enum class Stage { GoblinApproval, KittyApproval, KittyFile, Data, End };
  struct Job {
    Record begin;
    uint32_t id = 0, checksum = 0;
    uint64_t received = 0, updated = 0;
    Stage stage = Stage::Data;
    std::string name {};
    unsigned collision = 0;
  };
  struct Output { uint64_t token; std::string bytes; bool cleanup; };
  Crypto::Mode mode;
  Route route = Route::Local;
  bool confirmation = false;
  uint64_t deadline = 0;
  uint32_t first_id = 0, next_id = 0;
  size_t goblin_chunk = 4096;
  uint64_t goblin_limit = 64 * 1024 * 1024;
  std::string nonce {};
  std::map<uint64_t, Job> jobs {};
  std::deque<Output> output {};
  size_t output_bytes = 0;
  std::deque<Record> replies {};
  std::string input {};
  bool input_matched = false, input_overflow = false, paste = false;
  uint64_t input_updated = 0;
  void emit( uint64_t token, const std::string& bytes, bool cleanup = false );
  void response( uint64_t token, const std::string& status, const std::string& error = "", const std::string& name = "" );
  void cancel( uint64_t token, const std::string& reason );
  void probe_kitty( uint64_t now );
  bool handle( const std::string& sequence, uint64_t now );
  void kitty_file( uint64_t token, Job& job );
  std::string kitty_id( uint32_t id ) const;
  std::string goblin( uint32_t id, const std::string& meta ) const;
  std::string kitty( uint32_t id, const std::string& meta ) const;
public:
  explicit Parent( Crypto::Mode crypto_mode ) : mode( crypto_mode ) {}
  void enable() { if ( route == Route::Local && !first_id ) { route = Route::Unknown; } }
  Route selected() const { return route; }
  void discover( uint64_t now );
  bool forwarding() const { return route == Route::Goblin || route == Route::Kitty; }
  bool ready() const;
  bool submit( const Record& record, uint64_t now );
  bool pop( Record& record );
  void tick( uint64_t now );
  std::string filter( const std::string& bytes, uint64_t now );
  std::string expire_input( uint64_t now );
  int wait_time( uint64_t now ) const;
  std::string take_output();
  std::string close();
};
}
#endif
