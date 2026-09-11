/* Distributed under the GNU GPL, version 3 or later. */
#ifndef TERMINAL_DOWNLOAD_H
#define TERMINAL_DOWNLOAD_H
#include <cstdint>
#include <string>

namespace Download {
constexpr size_t MAX_CHUNK = 4096, MAX_OSC = 8192;
constexpr uint64_t MAX_SIZE = 64 * 1024 * 1024;
constexpr unsigned MAX_TRANSFERS = 4;
const char PREFIX[] = "777;goblin-download;";

struct Command {
  uint32_t id = 0;
  uint64_t size = 0;
  bool reply = false, confirm = false;
  std::string op {}, name {}, data {};
};
// On malformed input, id/reply/op are retained when safely parseable, so an
// application that opted in can receive an error for its active transfer.
bool parse( const std::string& body, Command& command );
bool safe_basename( const std::string& name );
std::string status( uint32_t id, const std::string& state, const std::string& extra = "" );
}
#endif
