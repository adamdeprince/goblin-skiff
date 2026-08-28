/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef STREAM_HPP
#define STREAM_HPP

#include <cstdint>
#include <string>

namespace StreamBuffers {
class StreamEvent;
}

namespace Network {
enum StreamEventType
{
  StreamOpenType = 0,
  StreamDataType = 1,
  StreamEOFType = 2,
  StreamCloseType = 3
};

enum StreamPriority
{
  StreamPriorityLow = 0,
  StreamPriorityMedium = 1
};

class StreamEvent
{
public:
  StreamEventType type;
  uint64_t stream_id;
  std::string host;
  uint32_t port;
  bool agent;
  bool x11;
  std::string x11_auth_data;
  StreamPriority priority;
  std::string data;

  StreamEvent()
    : type( StreamCloseType ), stream_id( 0 ), host(), port( 0 ), agent( false ), x11( false ),
      x11_auth_data(), priority( StreamPriorityLow ), data()
  {}

  static StreamEvent open( uint64_t stream_id,
                           const std::string& host,
                           uint32_t port,
                           bool agent = false,
                           bool x11 = false,
                           const std::string& x11_auth_data = std::string(),
                           StreamPriority priority = StreamPriorityLow );
  static StreamEvent data_event( uint64_t stream_id,
                                 const std::string& data,
                                 StreamPriority priority = StreamPriorityLow );
  static StreamEvent eof( uint64_t stream_id, StreamPriority priority = StreamPriorityLow );
  static StreamEvent close( uint64_t stream_id, StreamPriority priority = StreamPriorityLow );

  bool operator==( const StreamEvent& x ) const
  {
    return ( type == x.type ) && ( stream_id == x.stream_id ) && ( host == x.host ) && ( port == x.port )
           && ( agent == x.agent ) && ( x11 == x.x11 ) && ( x11_auth_data == x.x11_auth_data )
           && ( priority == x.priority ) && ( data == x.data );
  }
};

void stream_event_to_proto( StreamBuffers::StreamEvent* proto, const StreamEvent& event );
StreamEvent stream_event_from_proto( const StreamBuffers::StreamEvent& proto );
}

#endif
