/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/statesync/stream.h"

#include "src/protobufs/stream.pb.h"

using namespace Network;

StreamEvent StreamEvent::open( uint64_t stream_id,
                               const std::string& host,
                               uint32_t port,
                               bool agent,
                               bool x11,
                               const std::string& x11_auth_data,
                               StreamPriority priority )
{
  StreamEvent event;
  event.type = StreamOpenType;
  event.stream_id = stream_id;
  event.host = host;
  event.port = port;
  event.agent = agent;
  event.x11 = x11;
  event.x11_auth_data = x11_auth_data;
  event.priority = priority;
  return event;
}

StreamEvent StreamEvent::data_event( uint64_t stream_id, const std::string& data, StreamPriority priority )
{
  StreamEvent event;
  event.type = StreamDataType;
  event.stream_id = stream_id;
  event.data = data;
  event.priority = priority;
  return event;
}

StreamEvent StreamEvent::eof( uint64_t stream_id, StreamPriority priority )
{
  StreamEvent event;
  event.type = StreamEOFType;
  event.stream_id = stream_id;
  event.priority = priority;
  return event;
}

StreamEvent StreamEvent::close( uint64_t stream_id, StreamPriority priority )
{
  StreamEvent event;
  event.type = StreamCloseType;
  event.stream_id = stream_id;
  event.priority = priority;
  return event;
}

void Network::stream_event_to_proto( StreamBuffers::StreamEvent* proto, const StreamEvent& event )
{
  proto->set_stream_id( event.stream_id );

  switch ( event.type ) {
    case StreamOpenType:
      proto->set_type( StreamBuffers::StreamEvent::OPEN );
      proto->set_host( event.host );
      proto->set_port( event.port );
      proto->set_agent( event.agent );
      proto->set_x11( event.x11 );
      proto->set_x11_auth_data( event.x11_auth_data );
      proto->set_priority( event.priority );
      break;
    case StreamDataType:
      proto->set_type( StreamBuffers::StreamEvent::DATA );
      proto->set_data( event.data );
      proto->set_priority( event.priority );
      break;
    case StreamEOFType:
      proto->set_type( StreamBuffers::StreamEvent::STREAM_EOF );
      proto->set_priority( event.priority );
      break;
    case StreamCloseType:
      proto->set_type( StreamBuffers::StreamEvent::CLOSE );
      proto->set_priority( event.priority );
      break;
  }
}

StreamEvent Network::stream_event_from_proto( const StreamBuffers::StreamEvent& proto )
{
  StreamPriority priority = proto.priority() ? StreamPriorityMedium : StreamPriorityLow;
  switch ( proto.type() ) {
    case StreamBuffers::StreamEvent::OPEN:
      return StreamEvent::open(
        proto.stream_id(), proto.host(), proto.port(), proto.agent(), proto.x11(), proto.x11_auth_data(), priority );
    case StreamBuffers::StreamEvent::DATA:
      return StreamEvent::data_event( proto.stream_id(), proto.data(), priority );
    case StreamBuffers::StreamEvent::STREAM_EOF:
      return StreamEvent::eof( proto.stream_id(), priority );
    case StreamBuffers::StreamEvent::CLOSE:
    default:
      return StreamEvent::close( proto.stream_id(), priority );
  }
}
