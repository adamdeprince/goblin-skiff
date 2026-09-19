// Modified for Goblin Skiff on 2026-09-19.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/include/config.h"
#include "src/include/version.h"
#include "sessionversion.h"
#include "src/protobufs/transportinstruction.pb.h"

#include <algorithm>
#include <stdexcept>

namespace {
bool valid_text( const std::string& text, bool spaces )
{
  return !text.empty() && text.size() <= 128
         && std::all_of( text.begin(), text.end(), [spaces]( unsigned char c ) {
              return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' )
                     || c == '.' || c == '-' || c == '_' || c == '+' || ( spaces && c == ' ' );
            } );
}
}

namespace Network {
const SessionVersion& SessionVersion::local()
{
  static const SessionVersion version { GOBLIN_VERSION, BUILD_VERSION, GOBLIN_PROTOCOL_VERSION };
  return version;
}

std::string SessionVersion::bootstrap_line() const
{
  return "MOSH VERSION " + std::to_string( protocol ) + " " + release + " " + build;
}

void SessionVersion::advertise( TransportBuffers::Instruction& instruction ) const
{
  instruction.set_goblin_release( release );
  instruction.set_goblin_build( build );
  instruction.set_goblin_protocol( protocol );
}

bool SessionVersion::observe( const TransportBuffers::Instruction& instruction )
{
  if ( !instruction.has_goblin_release() && !instruction.has_goblin_build() && !instruction.has_goblin_protocol() ) {
    return false;
  }
  if ( !instruction.has_goblin_protocol() || instruction.goblin_protocol() == 0
       || !valid_text( instruction.goblin_release(), false ) || !valid_text( instruction.goblin_build(), true ) ) {
    throw std::runtime_error( "Invalid Goblin Skiff peer version metadata" );
  }
  if ( instruction.goblin_protocol() != GOBLIN_PROTOCOL_VERSION ) {
    throw std::runtime_error( "Incompatible Goblin Skiff protocols: local " + local().release + " uses "
                              + std::to_string( GOBLIN_PROTOCOL_VERSION ) + "; peer " + instruction.goblin_release()
                              + " uses " + std::to_string( instruction.goblin_protocol() ) );
  }
  if ( protocol ) {
    if ( release != instruction.goblin_release() || build != instruction.goblin_build()
         || protocol != instruction.goblin_protocol() ) {
      throw std::runtime_error( "Goblin Skiff peer changed version during a session" );
    }
    return false;
  }
  release = instruction.goblin_release();
  build = instruction.goblin_build();
  protocol = instruction.goblin_protocol();
  return true;
}
}
