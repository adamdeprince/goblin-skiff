/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/include/config.h"

#include "src/protobufs/clipboard.pb.h"
#include "src/statesync/clipboard.h"

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

using namespace Terminal;

namespace {
#ifdef HAVE_ZSTD
std::string zstd_compress_payload( const std::string& input )
{
  const size_t bound = ZSTD_compressBound( input.size() );
  std::string compressed( bound, '\0' );
  const size_t written
    = ZSTD_compress( &compressed[0], compressed.size(), input.data(), input.size(), CLIPBOARD_ZSTD_LEVEL );
  if ( ZSTD_isError( written ) ) {
    return std::string();
  }
  compressed.resize( written );
  return compressed;
}

bool zstd_decompress_payload( const std::string& input, std::string& output )
{
  const unsigned long long declared = ZSTD_getFrameContentSize( input.data(), input.size() );
  if ( declared == ZSTD_CONTENTSIZE_ERROR ) {
    return false;
  }

  size_t guess = 0;
  if ( declared == ZSTD_CONTENTSIZE_UNKNOWN ) {
    guess = input.size() * 8;
    if ( guess < 4096 ) {
      guess = 4096;
    }
    if ( guess > OSC52_MAX_OSC_CHARS ) {
      guess = OSC52_MAX_OSC_CHARS;
    }
  } else if ( declared > OSC52_MAX_OSC_CHARS ) {
    return false;
  } else {
    guess = static_cast<size_t>( declared );
  }

  output.assign( guess, '\0' );
  const size_t written = ZSTD_decompress( &output[0], output.size(), input.data(), input.size() );
  if ( ZSTD_isError( written ) ) {
    return false;
  }
  output.resize( written );
  return true;
}
#endif

ClipboardBuffers::ClipboardEvent_Op proto_op( ClipboardOp op )
{
  switch ( op ) {
    case ClipboardQuery:
      return ClipboardBuffers::ClipboardEvent::QUERY;
    case ClipboardClear:
      return ClipboardBuffers::ClipboardEvent::CLEAR;
    case ClipboardSet:
    default:
      return ClipboardBuffers::ClipboardEvent::SET;
  }
}

ClipboardOp native_op( ClipboardBuffers::ClipboardEvent_Op op )
{
  switch ( op ) {
    case ClipboardBuffers::ClipboardEvent::QUERY:
      return ClipboardQuery;
    case ClipboardBuffers::ClipboardEvent::CLEAR:
      return ClipboardClear;
    case ClipboardBuffers::ClipboardEvent::SET:
    default:
      return ClipboardSet;
  }
}
}

bool Terminal::clipboard_should_transmit( const ClipboardEvent& event )
{
  if ( event.truncated ) {
    return false;
  }
  if ( event.op == ClipboardSet ) {
    return event.decode_ok;
  }
  return true;
}

void Terminal::clipboard_event_to_proto( ClipboardBuffers::ClipboardEvent* proto, const ClipboardEvent& event )
{
  proto->set_op( proto_op( event.op ) );
  proto->set_targets( event.targets );
  proto->set_truncated( event.truncated );

  if ( event.op != ClipboardSet || event.payload.empty() ) {
    return;
  }

#ifdef HAVE_ZSTD
  const std::string compressed = zstd_compress_payload( event.payload );
  if ( !compressed.empty() && compressed.size() < event.payload.size() ) {
    proto->set_payload( compressed );
    proto->set_zstd( true );
    return;
  }
#endif

  proto->set_payload( event.payload );
  proto->set_zstd( false );
}

ClipboardEvent Terminal::clipboard_event_from_proto( const ClipboardBuffers::ClipboardEvent& proto )
{
  ClipboardEvent event;
  event.op = proto.has_op() ? native_op( proto.op() ) : ClipboardSet;
  event.targets = proto.has_targets() ? proto.targets() : static_cast<unsigned>( ClipboardClipboard );
  if ( event.targets == ClipboardNone ) {
    event.targets = ClipboardClipboard;
  }
  event.truncated = proto.truncated();

  if ( proto.has_payload() && !proto.payload().empty() ) {
    if ( proto.zstd() ) {
#ifdef HAVE_ZSTD
      if ( zstd_decompress_payload( proto.payload(), event.payload ) ) {
        event.decode_ok = true;
      }
#else
      event.decode_ok = false;
#endif
    } else {
      event.payload = proto.payload();
      event.decode_ok = true;
    }
  }

  if ( event.op == ClipboardSet && event.decode_ok ) {
    event.raw_pd = base64_encode_osc52( event.payload );
  }

  return event;
}
