/* Reliable tmux -CC passthrough. See tmux's Control Mode protocol.
   Copyright 2026. Distributed under the GNU GPL, version 3 or later. */

#ifndef MOSH_TMUX_HPP
#define MOSH_TMUX_HPP

#include <string>

namespace Terminal {

static const size_t TMUX_QUEUE_LIMIT = 65536;

struct TmuxBytes
{
  std::string data;
  explicit TmuxBytes( const std::string& bytes = std::string() ) : data( bytes ) {}
  bool operator==( const TmuxBytes& other ) const { return data == other.data; }
};

/* Split PTY output without interpreting the control protocol. Hold at most
   the seven-byte entry marker; a control session itself may be unlimited.
   Other control strings (OSC, APC, DCS passthrough) are left alone. */
class TmuxControlParser
{
private:
  enum State
  {
    Normal,
    Escape,
    Candidate,
    String,
    StringEscape,
    Control,
    ControlEscape
  };
  State state;
  bool osc;
  std::string pending;

public:
  struct Output
  {
    std::string terminal;
    std::string control;
  };

  TmuxControlParser() : state( Normal ), osc( false ), pending() {}
  bool active() const { return state == Control || state == ControlEscape; }
  Output consume( const std::string& bytes );
};

}
#endif
