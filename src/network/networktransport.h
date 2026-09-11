/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#ifndef NETWORK_TRANSPORT_HPP
#define NETWORK_TRANSPORT_HPP

#include <algorithm>
#include <csignal>
#include <ctime>
#include <deque>
#include <list>
#include <string>
#include <vector>

#include "src/network/bulkdatagram.h"
#include "src/network/network.h"
#include "src/network/statesamples.h"
#include "src/network/transportsender.h"
#include "transportfragment.h"

namespace Network {
template<class MyState, class RemoteState>
class Transport
{
private:
  /* the underlying, encrypted network connection */
  Connection connection;

  /* sender side */
  TransportSender<MyState> sender;

  /* helper methods for recv() */
  void process_throwaway_until( uint64_t throwaway_num );

  /* simple receiver */
  std::list<TimestampedState<RemoteState>> received_states;
  std::deque<Bulk::Datagram> bulk_datagrams;
  uint64_t receiver_quench_timer;
  RemoteState last_receiver_state; /* the state we were in when user last queried state */
  FragmentAssembly fragments;
  StateSampleWriter state_sample_writer;
  unsigned int verbose;

public:
  Transport( MyState& initial_state,
             RemoteState& initial_remote,
             const char* desired_ip,
             const char* desired_port,
             bool compact_keepalive = false,
             Crypto::Mode crypto_mode = Crypto::Mode::LegacyOCB );
  Transport( MyState& initial_state,
             RemoteState& initial_remote,
             const char* key_str,
             const char* ip,
             const char* port,
             bool compact_keepalive = false,
             Crypto::Mode crypto_mode = Crypto::Mode::LegacyOCB );

  /* Send data or an ack if necessary. */
  void tick( void )
  {
    connection.tick();
    sender.tick();
  }

  /* Returns the number of ms to wait until next possible event. */
  int wait_time( void ) {
    return std::min( std::min( sender.wait_time(), connection.keepalive_wait_time() ),
                     std::max( connection.feedback_wait_time(), connection.pacing_wait_time( true ) ) );
  }
  void enable_link_budget( bool enabled ) { connection.enable_link_budget( enabled ); }
  void set_relay_hops( unsigned hops ) { connection.set_relay_hops( hops ); }
  void set_relay_keys( const std::string& keys ) { connection.set_relay_keys( keys ); }
  const LinkBudget& link_budget() const { return connection.link_budget(); }
  int bulk_wait_time() { return connection.pacing_wait_time(); }
  bool has_unsent_data( void ) const { return sender.has_unsent_data(); }
  size_t max_datagram_payload( void ) const;

  /* Blocks waiting for a packet. */
  void recv( void );

  void send_bulk( const Bulk::Datagram& datagram );
  bool pop_bulk( Bulk::Datagram& datagram );

  /* Find diff between last receiver state and current remote state, then rationalize states. */
  std::string get_remote_diff( void );

  /* Shut down other side of connection. */
  /* Illegal to change current_state after this. */
  void start_shutdown( void ) { sender.start_shutdown(); }
  bool shutdown_in_progress( void ) const { return sender.get_shutdown_in_progress(); }
  bool shutdown_acknowledged( void ) const { return sender.get_shutdown_acknowledged(); }
  bool shutdown_ack_timed_out( void ) const { return sender.shutdown_ack_timed_out(); }
  bool has_remote_addr( void ) const { return connection.get_has_remote_addr(); }

  /* Other side has requested shutdown and we have sent one ACK */
  bool counterparty_shutdown_ack_sent( void ) const { return sender.get_counterparty_shutdown_acknowledged(); }

  std::string port( void ) const { return connection.port(); }
  std::string get_key( void ) const { return connection.get_key(); }

  MyState& get_current_state( void ) { return sender.get_current_state(); }
  void set_current_state( const MyState& x ) { sender.set_current_state( x ); }

  uint64_t get_remote_state_num( void ) const { return received_states.back().num; }

  const TimestampedState<RemoteState>& get_latest_remote_state( void ) const { return received_states.back(); }

  const std::vector<int> fds( void ) const { return connection.fds(); }
  int get_MTU( void ) const { return connection.get_MTU(); }
  double get_SRTT( void ) const { return connection.get_SRTT(); }

  void set_verbose( unsigned int s_verbose )
  {
    sender.set_verbose( s_verbose );
    verbose = s_verbose;
  }

  void set_state_sample_log( const std::string& path, size_t min_size )
  {
    state_sample_writer.open( path, min_size );
  }

  void set_send_delay( int new_delay ) { sender.set_send_delay( new_delay ); }
  void request_immediate_send( void ) { sender.request_immediate_send(); }

  uint64_t get_sent_state_acked_timestamp( void ) const { return sender.get_sent_state_acked_timestamp(); }
  uint64_t get_last_roundtrip_success( void ) const { return connection.get_last_roundtrip_success(); }
  unsigned int get_keepalive_interval( void ) const { return connection.get_keepalive_interval(); }
  uint64_t get_sent_state_acked( void ) const { return sender.get_sent_state_acked(); }
  uint64_t get_sent_state_last( void ) const { return sender.get_sent_state_last(); }

  unsigned int send_interval( void ) const { return sender.send_interval(); }

  const Addr& get_remote_addr( void ) const { return connection.get_remote_addr(); }
  socklen_t get_remote_addr_len( void ) const { return connection.get_remote_addr_len(); }

  std::string& get_send_error( void ) { return connection.get_send_error(); }
};
}

#endif
