/* Distributed under the GNU GPL, version 3 or later. */
#ifndef OSC5522_HPP
#define OSC5522_HPP
#include <cstdint>
#include <string>
#include <vector>

namespace Terminal {
constexpr size_t OSC5522_MAX_FRAME = 16384;
constexpr size_t OSC5522_MAX_TRANSFER = 64 * 1024 * 1024;
constexpr size_t OSC5522_FAST_THRESHOLD = 64 * 1024;

struct MimeClipboardMessage {
  std::string body {}, type {}, status {}, id {}, mime {}, data {};
  bool write_end() const { return type == "wdata" && mime.empty() && data.empty(); }
  bool text() const { return mime == "text/plain" || mime.compare( 0, 11, "text/plain;" ) == 0; }
};
bool parse_osc5522( const std::string& body, MimeClipboardMessage& message );
std::string osc5522_error( const MimeClipboardMessage& request, const std::string& status );

// Local-terminal replies are protocol data, never keystrokes. Recognized
// malformed/oversized replies are discarded, not released into the shell.
class Osc5522InputFilter {
  std::string pending {};
  bool matched = false, overflow = false, paste = false;
  uint64_t updated = 0;
public:
  struct Output { std::string user {}; std::vector<std::string> messages {}; };
  Output consume( const std::string& bytes, uint64_t now );
  Output expire( uint64_t now );
  int wait_time( uint64_t now ) const;
};
}
#endif
