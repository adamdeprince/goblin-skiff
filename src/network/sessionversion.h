// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MOSH_SESSION_VERSION_H
#define MOSH_SESSION_VERSION_H

#include <cstdint>
#include <string>

namespace TransportBuffers {
class Instruction;
}

namespace Network {
// Keep this at 1 for compatible releases. Negotiate optional features separately.
constexpr uint32_t GOBLIN_PROTOCOL_VERSION = 1;

struct SessionVersion
{
  std::string release {};
  std::string build {};
  uint32_t protocol { 0 }; // Unversioned, before the compatibility baseline.

  static const SessionVersion& local();
  std::string bootstrap_line() const;
  void advertise( TransportBuffers::Instruction& instruction ) const;
  // Called only after authentication, before applying or acknowledging state.
  // Missing fields preserve an already learned identity. Returns true once.
  bool observe( const TransportBuffers::Instruction& instruction );
};
}

#endif
