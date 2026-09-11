/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef STREAM_FORWARD_HPP
#define STREAM_FORWARD_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "src/network/network.h"
#include "src/statesync/stream.h"

class StreamForwarder
{
public:
  enum Side
  {
    ClientSide,
    ServerSide
  };

  StreamForwarder( Side side,
                   unsigned int stream_delay_ms,
                   unsigned int stream_rate_bytes_per_second,
                   Crypto::Mode crypto_mode = Crypto::Mode::LegacyOCB );
  ~StreamForwarder();
  void adapt_link_budget( double bytes_per_second );

  bool add_tcp_forward( const std::string& spec, std::string& error );
  bool add_dynamic_forward( const std::string& spec, std::string& error );
  bool enable_agent_forwarding( std::string& error );
  bool enable_x11_forwarding( std::string& error );
  bool listen( std::string& error );

  const std::string& agent_socket_path( void ) const { return agent_path; }
  const std::string& x11_display( void ) const { return x11_display_string; }
  const std::string& x11_authority_path( void ) const { return x11_auth_path; }

  std::vector<int> fds( void ) const;
  int wait_time( uint64_t now ) const;
  bool has_pending_network_data( void ) const;
  void process_readable_fd( int fd );
  void handle_remote_event( const Network::StreamEvent& event );

  template<class State>
  void flush( State& state, uint64_t now, unsigned int send_interval, size_t max_datagram_payload )
  {
    const size_t chunk_cap = stream_chunk_cap( send_interval, max_datagram_payload );
    refill_tokens( now, chunk_cap );
    try_write_local();

    if ( !pending_medium_control.empty() ) {
      state.push_back( pending_medium_control.front() );
      pending_medium_control.pop_front();
      return;
    }
    if ( !pending_low_control.empty() ) {
      state.push_back( pending_low_control.front() );
      pending_low_control.pop_front();
      return;
    }

    for ( int priority = Network::StreamPriorityMedium; priority >= Network::StreamPriorityLow; priority-- ) {
      for ( std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); it++ ) {
        Stream& stream = it->second;
        if ( stream.priority == static_cast<Network::StreamPriority>( priority ) && stream.to_network.empty()
             && stream.local_eof_pending ) {
          state.push_back( Network::StreamEvent::eof( stream.id, stream.priority ) );
          stream.local_eof_pending = false;
          stream.local_eof_sent = true;
          return;
        }
      }
    }

    for ( int priority = Network::StreamPriorityMedium; priority >= Network::StreamPriorityLow; priority-- ) {
      Network::StreamPriority stream_priority = static_cast<Network::StreamPriority>( priority );
      bool priority_has_network_data = false;
      size_t budget = static_cast<size_t>( stream_tokens_for( stream_priority ) );

      for ( std::map<uint64_t, Stream>::iterator it = streams.begin(); it != streams.end(); it++ ) {
        Stream& stream = it->second;
        if ( stream.priority != stream_priority || stream.to_network.empty() ) {
          continue;
        }
        priority_has_network_data = true;

        if ( ( stream.to_network.size() < chunk_cap )
             && ( now < stream.first_pending_byte + stream_delay_ms )
             && !stream.local_eof_pending ) {
          continue;
        }

        const size_t min_send = ( stream.to_network.size() >= MIN_STREAM_CHUNK && !stream.local_eof_pending )
                                  ? std::min( MIN_STREAM_CHUNK, chunk_cap )
                                  : 1;
        if ( budget < min_send ) {
          continue;
        }

        size_t chunk_len = stream.to_network.size();
        if ( chunk_len > chunk_cap ) {
          chunk_len = chunk_cap;
        }
        if ( chunk_len > budget ) {
          chunk_len = budget;
        }
        if ( chunk_len == 0 ) {
          continue;
        }

        std::string chunk( stream.to_network.begin(), stream.to_network.begin() + chunk_len );
        stream.to_network.erase( 0, chunk_len );
        if ( !stream.to_network.empty() ) {
          stream.first_pending_byte = now;
        }
        state.push_back( Network::StreamEvent::data_event( stream.id, chunk, stream.priority ) );
        stream_tokens_for( stream_priority ) -= chunk_len;
        budget -= chunk_len;

        if ( stream.to_network.empty() && stream.local_eof_pending ) {
          state.push_back( Network::StreamEvent::eof( stream.id, stream.priority ) );
          stream.local_eof_pending = false;
          stream.local_eof_sent = true;
        }
        return;
      }

      if ( priority_has_network_data ) {
        return;
      }
    }
  }

private:
  enum ListenerKind
  {
    TcpListener,
    DynamicListener,
    AgentListener,
    X11Listener
  };

  enum SocksState
  {
    NoSocks,
    SocksGreeting,
    SocksRequest,
    SocksEstablished
  };

  struct Listener
  {
    int fd;
    ListenerKind kind;
    std::string bind_host;
    uint32_t bind_port;
    std::string target_host;
    uint32_t target_port;
    std::string path;

    Listener()
      : fd( -1 ), kind( TcpListener ), bind_host(), bind_port( 0 ), target_host(), target_port( 0 ), path()
    {}
  };

  struct Stream
  {
    uint64_t id;
    int fd;
    SocksState socks_state;
    std::string socks_buffer;
    std::string to_network;
    std::string to_local;
    uint64_t first_pending_byte;
    bool local_eof_pending;
    bool local_eof_sent;
    bool remote_eof_seen;
    bool closed;
    Network::StreamPriority priority;
    bool x11;
    std::string x11_peer_auth_data;
    bool x11_auth_rewritten;
    std::string x11_setup_buffer;

    Stream()
      : id( 0 ), fd( -1 ), socks_state( NoSocks ), socks_buffer(), to_network(), to_local(),
        first_pending_byte( 0 ), local_eof_pending( false ), local_eof_sent( false ), remote_eof_seen( false ),
        closed( false ), priority( Network::StreamPriorityLow ), x11( false ), x11_peer_auth_data(),
        x11_auth_rewritten( false ), x11_setup_buffer()
    {}
  };

  static const size_t LOCAL_QUEUE_LIMIT = 65536;
  static const size_t STREAM_QUEUE_LIMIT = 65536;
  static const size_t MAX_STREAM_CHUNK = 768;
  static const size_t MIN_STREAM_CHUNK = 96;
  static const size_t STREAM_DATAGRAM_OVERHEAD_ALLOWANCE = 192;

  Side side;
  Crypto::Mode crypto_mode;
  uint64_t next_stream_id;
  unsigned int stream_delay_ms;
  unsigned int stream_rate_bytes_per_second;
  bool automatic_rate;
  double stream_tokens[2];
  uint64_t last_token_update;
  bool agent_requested;
  bool x11_requested;
  std::string agent_path;
  std::string agent_dir;
  std::string local_agent_path;
  std::string local_x11_display;
  std::string local_x11_auth_data;
  std::string x11_auth_data;
  std::string x11_auth_path;
  std::string x11_display_string;

  std::vector<Listener> requested_listeners;
  std::vector<Listener> listeners;
  std::map<uint64_t, Stream> streams;
  std::map<int, uint64_t> fd_to_stream;
  std::deque<Network::StreamEvent> pending_medium_control;
  std::deque<Network::StreamEvent> pending_low_control;

  bool parse_tcp_forward( const std::string& spec, Listener& listener, std::string& error ) const;
  bool parse_bind_port( const std::string& spec, Listener& listener, std::string& error ) const;
  bool open_tcp_listener( Listener& listener, std::string& error );
  bool open_agent_listener( Listener& listener, std::string& error );
  bool open_x11_listener( Listener& listener, std::string& error );
  void close_listener( Listener& listener );
  void close_stream( Stream& stream, bool notify_peer );
  void accept_connection( const Listener& listener );
  void read_stream( Stream& stream );
  void read_socks( Stream& stream );
  void handle_socks_request( Stream& stream );
  void open_local_stream( Stream& stream,
                          const std::string& host,
                          uint32_t port,
                          bool agent,
                          bool x11,
                          const std::string& x11_peer_auth_data,
                          Network::StreamPriority priority );
  void connect_remote_stream( const Network::StreamEvent& event );
  int connect_tcp( const std::string& host, uint32_t port );
  int connect_unix( const std::string& path );
  int connect_x11( void );
  void append_stream_data( Stream& stream, const std::string& data );
  bool rewrite_x11_setup( Stream& stream );
  void queue_control( const Network::StreamEvent& event );
  void try_write_local( void );
  void refill_tokens( uint64_t now, size_t chunk_cap );
  size_t stream_chunk_cap( unsigned int send_interval, size_t max_datagram_payload ) const;
  uint64_t allocate_stream_id( void );

  static size_t priority_index( Network::StreamPriority priority )
  {
    return priority == Network::StreamPriorityMedium ? 1 : 0;
  }

  double& stream_tokens_for( Network::StreamPriority priority ) { return stream_tokens[priority_index( priority )]; }

  double stream_tokens_for( Network::StreamPriority priority ) const
  {
    return stream_tokens[priority_index( priority )];
  }
};

#endif
