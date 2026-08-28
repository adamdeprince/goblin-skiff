/*
    Mosh: the mobile shell
    Copyright 2026

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
*/

#include "src/include/config.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/protobufs/clipboard.pb.h"
#include "src/protobufs/hostinput.pb.h"
#include "src/statesync/clipboard.h"
#include "src/statesync/completeterminal.h"
#include "src/terminal/osc52.h"
#include "src/terminal/terminaldisplay.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

void test_base64( void )
{
  const std::string hello = "hello";
  const std::string encoded = Terminal::base64_encode_osc52( hello );
  require( encoded == "aGVsbG8=", "hello base64 mismatch" );

  std::string decoded;
  require( Terminal::base64_decode_osc52( encoded, decoded ), "hello base64 decode failed" );
  require( decoded == hello, "hello base64 roundtrip failed" );

  require( Terminal::base64_decode_osc52( "aGVsbG8", decoded ), "unpadded decode failed" );
  require( decoded == hello, "unpadded decode mismatch" );

  require( Terminal::base64_decode_osc52( "aGVs\n bG8=", decoded ), "whitespace decode failed" );
  require( decoded == hello, "whitespace decode mismatch" );

  require( !Terminal::base64_decode_osc52( "????", decoded ), "invalid base64 accepted" );
  require( Terminal::base64_decode_osc52( "", decoded ), "empty base64 rejected" );
  require( decoded.empty(), "empty base64 should decode empty" );
}

void test_parse_variants( void )
{
  Terminal::ClipboardEvent ev;
  require( Terminal::parse_osc52_string( "52;c;aGVsbG8=", ev ), "parse 52;c; failed" );
  require( ev.op == Terminal::ClipboardSet, "c payload should be SET" );
  require( ev.includes_clipboard(), "c target missing" );
  require( ev.payload == "hello", "c payload decode mismatch" );

  require( Terminal::parse_osc52_string( "52;;aGVsbG8=", ev ), "empty Pc should parse" );
  require( ev.includes_clipboard(), "empty Pc should default to clipboard" );
  require( ev.payload == "hello", "empty Pc payload mismatch" );

  require( Terminal::parse_osc52_string( "52;cps;aGVsbG8=", ev ), "multi-target parse failed" );
  require( ev.has_target( Terminal::ClipboardClipboard ), "missing c" );
  require( ev.has_target( Terminal::ClipboardPrimary ), "missing p" );
  require( ev.has_target( Terminal::ClipboardSelect ), "missing s" );

  require( Terminal::parse_osc52_string( "52;c;?", ev ), "query parse failed" );
  require( ev.op == Terminal::ClipboardQuery, "query op mismatch" );
  require( ev.payload.empty(), "query should not decode a payload" );

  require( Terminal::parse_osc52_string( "52;c;", ev ), "clear parse failed" );
  require( ev.op == Terminal::ClipboardClear, "empty Pd should clear" );

  require( Terminal::parse_osc52_string( "52;c;not base64!", ev ), "invalid Pd should still be OSC 52" );
  require( ev.op == Terminal::ClipboardClear, "invalid Pd should clear" );

  require( !Terminal::parse_osc52_string( "0;title", ev ), "title OSC should not parse as 52" );
  require( !Terminal::parse_osc52_string( "52;c", ev ), "missing Pd separator should fail" );
  require( !Terminal::parse_osc52_string( "52;x;aGVsbG8=", ev ), "unknown Pc should fail" );
}

void test_encode_roundtrip( void )
{
  Terminal::ClipboardEvent ev;
  ev.op = Terminal::ClipboardSet;
  ev.targets = Terminal::ClipboardClipboard;
  ev.payload = "hello";
  ev.decode_ok = true;

  const std::string seq = Terminal::encode_osc52( ev, false );
  require( seq == "\033]52;c;aGVsbG8=\007", "encode SET mismatch" );

  Terminal::ClipboardEvent query;
  query.op = Terminal::ClipboardQuery;
  query.targets = Terminal::ClipboardClipboard;
  require( Terminal::encode_osc52( query, true ) == "\033]52;c;?\033\\", "encode query ST mismatch" );
}

void test_emulator_events( void )
{
  Terminal::Complete term( 80, 24 );
  term.act( std::string( "\033]52;c;aGVsbG8=\007" ) );
  std::vector<Terminal::ClipboardEvent> events = term.take_parser_clipboard_events();
  require( events.size() == 1, "expected one SET event" );
  require( events[0].op == Terminal::ClipboardSet, "emulator SET op" );
  require( events[0].payload == "hello", "emulator SET payload" );
  require( term.get_fb().get_clipboard().empty(), "SET must not enter framebuffer state" );

  Terminal::Complete query_term( 80, 24 );
  query_term.act( std::string( "\033]52;c;?\007" ) );
  events = query_term.take_parser_clipboard_events();
  require( events.size() == 1, "expected one QUERY event" );
  require( events[0].op == Terminal::ClipboardQuery, "emulator QUERY op" );
  require( query_term.get_fb().get_clipboard().empty(), "query must not store '?' in framebuffer" );

  Terminal::Complete tmux_term( 80, 24 );
  tmux_term.act( std::string( "\033]52;;aGVsbG8=\033\\" ) );
  events = tmux_term.take_parser_clipboard_events();
  require( events.size() == 1, "expected tmux-style SET event" );
  require( events[0].includes_clipboard(), "tmux empty Pc should be clipboard" );
  require( events[0].payload == "hello", "tmux payload" );
  require( tmux_term.get_fb().get_clipboard().empty(), "tmux SET must not enter framebuffer state" );

  Terminal::Complete primary_term( 80, 24 );
  primary_term.act( std::string( "\033]52;p;aGVsbG8=\007" ) );
  events = primary_term.take_parser_clipboard_events();
  require( events.size() == 1, "expected PRIMARY SET event" );
  require( events[0].has_target( Terminal::ClipboardPrimary ), "primary target" );
  require( primary_term.get_fb().get_clipboard().empty(), "primary-only SET should not use framebuffer" );

  Terminal::Complete clear_term( 80, 24 );
  clear_term.act( std::string( "\033]52;c;aGVsbG8=\007\033]52;c;\007" ) );
  events = clear_term.take_parser_clipboard_events();
  require( events.size() == 2, "expected SET then CLEAR" );
  require( events[1].op == Terminal::ClipboardClear, "second event should clear" );
  require( clear_term.get_fb().get_clipboard().empty(), "clear should leave framebuffer empty" );
}

void test_display_does_not_retransmit( void )
{
  Terminal::Complete before( 80, 24 );
  Terminal::Complete after( 80, 24 );
  after.act( std::string( "\033]52;c;aGVsbG8=\007" ) );
  after.push_back( after.take_parser_clipboard_events().front() );

  Terminal::Display display( false );
  const std::string diff = display.new_frame( true, before.get_fb(), after.get_fb() );
  require( diff.find( "\033]52;" ) == std::string::npos,
           "display diffs must not carry clipboard payloads" );
}

void test_clipboard_instruction_roundtrip( void )
{
  Terminal::Complete src( 80, 24 );
  src.act( std::string( "\033]52;c;aGVsbG8=\007" ) );
  std::vector<Terminal::ClipboardEvent> parsed = src.take_parser_clipboard_events();
  require( parsed.size() == 1, "parser produced SET" );
  src.push_back( parsed[0] );

  Terminal::Complete empty( 80, 24 );
  const std::string wire = src.diff_from( empty );
  HostBuffers::HostMessage msg;
  require( msg.ParseFromString( wire ), "host message parse" );
  bool saw_clipboard = false;
  for ( int i = 0; i < msg.instruction_size(); i++ ) {
    if ( msg.instruction( i ).HasExtension( HostBuffers::clipboard ) ) {
      saw_clipboard = true;
      const ClipboardBuffers::ClipboardEvent& proto
        = msg.instruction( i ).GetExtension( HostBuffers::clipboard );
      require( proto.op() == ClipboardBuffers::ClipboardEvent::SET, "wire op" );
#ifdef HAVE_ZSTD
      /* "hello" is too small to win against a zstd frame; a larger
         payload should be compressed. */
#endif
    }
  }
  require( saw_clipboard, "diff should contain a clipboard instruction" );

  Terminal::Complete dst( 80, 24 );
  dst.apply_string( wire );
  require( dst.get_clipboard_events().size() == 1, "apply_string should keep one event" );
  require( dst.get_clipboard_events().front().payload == "hello", "decoded payload" );

  const std::string same = src.diff_from( dst );
  HostBuffers::HostMessage empty_msg;
  require( empty_msg.ParseFromString( same ), "empty diff parse" );
  for ( int i = 0; i < empty_msg.instruction_size(); i++ ) {
    require( !empty_msg.instruction( i ).HasExtension( HostBuffers::clipboard ),
             "acked clipboard must not be resent" );
  }

  std::string bulky( 4096, 'A' );
  for ( size_t i = 0; i < bulky.size(); i += 7 ) {
    bulky[i] = 'B';
  }
  Terminal::ClipboardEvent large;
  large.op = Terminal::ClipboardSet;
  large.targets = Terminal::ClipboardClipboard;
  large.payload = bulky;
  large.decode_ok = true;

  ClipboardBuffers::ClipboardEvent proto;
  Terminal::clipboard_event_to_proto( &proto, large );
#ifdef HAVE_ZSTD
  require( proto.zstd(), "large clipboard payload should use zstd-22" );
  require( proto.payload().size() < bulky.size(), "zstd-22 should shrink repetitive text" );
#endif
  Terminal::ClipboardEvent roundtrip = Terminal::clipboard_event_from_proto( proto );
  require( roundtrip.payload == bulky, "zstd-22 clipboard roundtrip" );
}

void test_input_filter( void )
{
  Terminal::Osc52InputFilter filter;
  const std::string input = "ab\033]52;c;aGVsbG8=\007cd\033[A";
  Terminal::Osc52InputFilter::Output out = filter.consume( input.data(), input.size() );
  require( out.user_bytes == "abcd\033[A", "filter should pass non-OSC52 bytes" );
  require( out.events.size() == 1, "filter should extract one OSC 52" );
  require( out.events[0].payload == "hello", "filter payload" );
  require( !filter.in_sequence(), "filter should return to ground" );

  Terminal::Osc52InputFilter split;
  const std::string prefix = "x\033]52;c;aG";
  out = split.consume( prefix.data(), prefix.size() );
  require( out.user_bytes == "x", "prefix user bytes" );
  require( out.events.empty(), "incomplete sequence is not an event" );
  require( split.in_sequence(), "incomplete sequence stays buffered" );

  const std::string rest = "VsbG8=\007y";
  out = split.consume( rest.data(), rest.size() );
  require( out.user_bytes == "y", "suffix user bytes" );
  require( out.events.size() == 1, "split sequence should complete" );
  require( out.events[0].payload == "hello", "split payload" );

  Terminal::Osc52InputFilter title;
  const std::string not52 = "\033]0;title\007z";
  out = title.consume( not52.data(), not52.size() );
  require( out.events.empty(), "OSC 0 is not clipboard" );
  require( out.user_bytes == not52, "OSC 0 should pass through as user bytes" );
}
}

int main( void )
{
  try {
    test_base64();
    test_parse_variants();
    test_encode_roundtrip();
    test_emulator_events();
    test_display_does_not_retransmit();
    test_clipboard_instruction_roundtrip();
    test_input_filter();
  } catch ( const std::exception& e ) {
    std::cerr << "osc52-parse: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
