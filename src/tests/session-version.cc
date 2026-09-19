// Modified for Goblin Skiff on 2026-09-19.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/include/config.h"
#include "src/network/networktransport-impl.h"
#include "src/util/timestamp.h"

#include <cstdio>
#include <poll.h>
#include <stdexcept>

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) { throw std::runtime_error( message ); }
}

template<class F> void rejects( F operation )
{
  bool rejected = false;
  try { operation(); } catch ( const std::runtime_error& ) { rejected = true; }
  require( rejected, "invalid version accepted" );
}

void metadata()
{
  TransportBuffers::Instruction inst;
  Network::SessionVersion peer;
  require( !peer.observe( inst ) && peer.protocol == 0, "legacy peer was labelled as baseline" );
  Network::SessionVersion::local().advertise( inst );
  // Future and older releases are compatible if their wire protocol matches.
  inst.set_goblin_release( "1.4.0-goblin20270101.1" );
  require( peer.observe( inst ), "compatible newer release rejected" );
  require( peer.release == inst.goblin_release() && peer.build == inst.goblin_build(), "peer identity lost" );
  require( !peer.observe( inst ), "duplicate identity changed session" );
  require( !peer.observe( TransportBuffers::Instruction() ) && peer.protocol == 1, "omitted identity erased peer" );
  inst.set_goblin_build( "changed-build" );
  rejects( [&] { peer.observe( inst ); } );

  Network::SessionVersion::local().advertise( inst );
  for ( unsigned protocol : { 0U, 2U, 0xffffffffU } ) {
    inst.set_goblin_protocol( protocol );
    rejects( [&] { Network::SessionVersion().observe( inst ); } );
  }
  Network::SessionVersion::local().advertise( inst );
  inst.clear_goblin_build();
  rejects( [&] { Network::SessionVersion().observe( inst ); } );
  Network::SessionVersion::local().advertise( inst );
  inst.clear_goblin_protocol();
  rejects( [&] { Network::SessionVersion().observe( inst ); } );
  for ( const std::string& release : { std::string(), std::string( 129, 'x' ), std::string( "bad\033[2J" ) } ) {
    Network::SessionVersion::local().advertise( inst );
    inst.set_goblin_release( release );
    rejects( [&] { Network::SessionVersion().observe( inst ); } );
  }

  // Metadata changes must allocate a new fragment ID even for the same state.
  Network::SessionVersion::local().advertise( inst );
  Network::Fragmenter fragmenter;
  const auto before = fragmenter.make_fragments( inst, 1024 );
  inst.clear_goblin_build(); inst.clear_goblin_release(); inst.clear_goblin_protocol();
  const auto after = fragmenter.make_fragments( inst, 1024 );
  require( before.front().id != after.front().id, "changed metadata reused fragment identity" );
}

// Small snapshot state lets this test inspect the actual encrypted transport.
struct State
{
  std::string value {};
  bool operator==( const State& other ) const { return value == other.value; }
  bool compare( const State& other ) const { return !( *this == other ); }
  std::string diff_from( const State& other ) const { return *this == other ? "" : value; }
  std::string init_diff() const { return value; }
  void apply_string( const std::string& diff ) { if ( !diff.empty() ) { value = diff; } }
  void subtract( const State* ) {}
  void reset_input() {}
};

void readable( int fd )
{
  struct pollfd entry { fd, POLLIN, 0 };
  require( poll( &entry, 1, 2000 ) == 1 && ( entry.revents & POLLIN ), "timed out waiting for loopback packet" );
  freeze_timestamp();
}

TransportBuffers::Instruction receive( Network::Connection& connection )
{
  Network::FragmentAssembly assembly;
  for ( unsigned i = 0; i < 100; ++i ) {
    readable( connection.fds().front() );
    Network::Fragment fragment( connection.recv() );
    if ( assembly.add_fragment( fragment ) ) { return assembly.get_assembly(); }
  }
  throw std::runtime_error( "too many test fragments" );
}

void session( bool legacy )
{
  freeze_timestamp();
  Network::Connection server( "127.0.0.1", "0" );
  State blank;
  Network::Transport<State, State> client( blank, blank, server.get_key().c_str(), "127.0.0.1", server.port().c_str() );
  client.get_current_state().value = "first";
  client.request_immediate_send();
  client.tick();
  auto first = receive( server );
  require( first.goblin_release() == Network::SessionVersion::local().release, "sender omitted initial identity" );

  // Lose the first instruction/ACK. The next state must still advertise.
  client.get_current_state().value = "second";
  client.request_immediate_send();
  client.tick();
  auto second = receive( server );
  require( second.has_goblin_release() && second.new_num() > first.new_num(), "lost startup identity not repeated" );

  Network::Fragmenter fragmenter;
  auto send = [&]( const TransportBuffers::Instruction& instruction ) {
    for ( auto& fragment : fragmenter.make_fragments( instruction, 1024 ) ) { server.send( fragment.tostring() ); }
    readable( client.fds().front() );
    client.recv();
  };
  TransportBuffers::Instruction reply;
  reply.set_protocol_version( Network::MOSH_PROTOCOL_VERSION );
  reply.set_old_num( 0 ); reply.set_new_num( 1 ); reply.set_ack_num( second.new_num() );
  reply.set_diff( "reply" );
  if ( !legacy ) {
    Network::SessionVersion::local().advertise( reply );
    reply.set_goblin_release( "1.4.0-goblin20270101.1" );
  }
  send( reply );
  require( client.get_latest_remote_state().state.value == "reply", "compatible state not applied" );
  require( client.get_peer_version().protocol == ( legacy ? 0U : 1U ), "transport did not retain identity" );

  client.get_current_state().value = "third";
  client.request_immediate_send(); client.tick();
  auto third = receive( server );
  require( !third.has_goblin_release() && !third.has_goblin_build() && !third.has_goblin_protocol(),
           "acknowledged identity still costs bandwidth" );

  reply.clear_goblin_release(); reply.clear_goblin_build(); reply.clear_goblin_protocol();
  reply.set_old_num( 1 ); reply.set_new_num( 2 ); reply.set_ack_num( third.new_num() );
  reply.set_diff( "later" );
  send( reply );
  require( client.get_peer_version().protocol == ( legacy ? 0U : 1U ), "later packet erased identity" );
  require( client.get_latest_remote_state().state.value == "later", "later state not applied" );

  reply.set_old_num( 2 ); reply.set_new_num( 3 ); reply.set_diff( "must not apply" );
  Network::SessionVersion::local().advertise( reply );
  reply.set_goblin_protocol( 2 );
  rejects( [&] { send( reply ); } );
  require( client.get_remote_state_num() == 2 && client.get_latest_remote_state().state.value == "later",
           "incompatible state was applied" );
}

void radio_settings()
{
  Network::Connection connection( "127.0.0.1", "0", true );
  require( connection.get_MTU() > 128 && connection.timeout() == 1000, "normal defaults changed" );
  connection.enable_link_budget( true ); connection.enable_radio_mode();
  bool refused_relay = false;
  try { connection.set_relay_hops( 4 ); } catch ( const std::invalid_argument& ) { refused_relay = true; }
  require( refused_relay, "UDP jump overhead must not exhaust the radio MTU" );
  require( connection.get_MTU() == 128, "radio datagram requires link fragmentation" );
  require( connection.timeout() == 18000 && connection.active_retry_timeout() > connection.timeout(),
           "radio retry expires before a response can arrive" );
  TransportBuffers::Instruction inst;
  inst.set_diff( std::string( 2000, 'x' ) );
  Network::SessionVersion::local().advertise( inst );
  Network::Fragmenter fragmenter;
  Network::FragmentAssembly assembly;
  bool complete = false;
  for ( auto f : fragmenter.make_fragments( inst, connection.get_MTU() - connection.packet_overhead() ) ) {
    require( f.tostring().size() + connection.packet_overhead() <= 128, "encrypted radio datagram exceeds budget" );
    complete = assembly.add_fragment( f );
  }
  require( complete && assembly.get_assembly().diff() == inst.diff(), "small radio fragments lost state" );
}
}

int main()
{
  try {
    metadata();
    radio_settings();
    session( false );
    session( true );
  } catch ( const std::exception& error ) {
    fprintf( stderr, "%s\n", error.what() );
    return 1;
  }
  return 0;
}
