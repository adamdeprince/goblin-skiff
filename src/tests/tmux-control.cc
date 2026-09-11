/* Copyright 2026. Distributed under the GNU GPL, version 3 or later. */
#include <clocale>
#include <iostream>
#include <stdexcept>

#include "src/statesync/completeterminal.h"
#include "src/statesync/tmux.h"
#include "src/statesync/user.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}

void test_parser()
{
  const std::string control = "\033P1000p%begin 1 2 0\r\n%output %0 h\303\251llo\\033\r\n%exit\r\n\033\\";
  const std::string all = "before" + control + "\033[31mafter";
  for ( size_t split = 0; split <= all.size(); split++ ) {
    Terminal::TmuxControlParser parser;
    auto first = parser.consume( all.substr( 0, split ) );
    auto second = parser.consume( all.substr( split ) );
    require( first.terminal + second.terminal == "before\033[31mafter", "normal output across every split" );
    require( first.control + second.control == control, "control output across every split" );
    require( !parser.active(), "exit marker leaves control mode" );
  }
  Terminal::TmuxControlParser parser;
  std::string received;
  for ( char c : control + control ) {
    auto output = parser.consume( std::string( 1, c ) );
    require( output.terminal.empty(), "one-byte chunks must not leak protocol into screen" );
    received += output.control;
  }
  require( received == control + control, "two consecutive sessions" );
  require( parser.consume( "\033P1000p" ).control == "\033P1000p" && parser.active(), "entry" );
  const std::string large( 1024 * 1024, 'x' );
  require( parser.consume( large ).control == large, "large output is not truncated or buffered" );
  require( parser.consume( "\033" ).control == "\033" && parser.active(), "split ST" );
  require( parser.consume( "\\" ).control == "\\" && !parser.active(), "complete split ST" );
  const std::string strings[] = { "\033]0;title\033P1000p\007normal",
                                  "\033_Gpayload\033P1000p\033\\normal",
                                  "\033Ptmux;\033\033P1000p\033\\normal",
                                  "\033P1001pignored\033\\normal",
                                  "\033P1000qignored\033\\normal",
                                  "\033\033[0mnormal" };
  for ( const std::string& string : strings ) {
    Terminal::TmuxControlParser plain;
    std::string passed;
    for ( char c : string ) {
      auto output = plain.consume( std::string( 1, c ) );
      require( output.control.empty() && !plain.active(), "other escape strings are not tmux" );
      passed += output.terminal;
    }
    require( passed == string, "other escape strings remain intact" );
  }
}

void test_reliable_output()
{
  Terminal::Complete empty( 80, 24 ), sender( empty ), receiver( empty );
  sender.append_tmux_output( "\033P1000p%begin 1 2 0\r\n" );
  const Terminal::Complete first( sender );
  receiver.apply_string( first.diff_from( empty ) );
  sender.append_tmux_output( "%output %0 first\\012second\r\n" );
  const Terminal::Complete lost( sender );
  sender.append_tmux_output( "%exit\r\n\033\\" );
  // A cumulative update repairs a lost intermediate state; the receive
  // frontier exposes only bytes not already delivered to the GUI.
  Terminal::Complete later( empty );
  later.apply_string( sender.diff_from( empty ) );
  receiver.apply_string( later.diff_from( receiver ) );
  require( receiver == sender, "cumulative state recovers lost control output" );
  require( receiver.diff_from( later ).empty(), "retransmission produces no duplicate bytes" );
  require( lost.get_tmux_output().size() < receiver.get_tmux_output().size(), "skipped state included" );
  sender.subtract( &first );
  receiver.subtract( &first );
  require( sender == receiver, "ACK subtraction preserves byte-stream prefix" );
  Terminal::Complete screen( 80, 24 );
  screen.act( "shell prompt" );
  const std::string pending = sender.get_tmux_output();
  sender.replace_terminal_state( screen );
  require( sender.get_tmux_output() == pending, "screen refresh preserves unacknowledged control bytes" );
  receiver.apply_string( sender.diff_from( receiver ) );
  // Framebuffer operator== uses shared row identity; independently rendered
  // rows are compared by cell content and cursor position instead.
  require( !receiver.compare( sender ) && receiver.get_tmux_output() == sender.get_tmux_output(),
           "screen and control state coexist" );
  sender.subtract( &sender );
  require( sender.get_tmux_output().empty(), "ACK releases output queue" );
}

void test_input()
{
  Network::UserStream sender, receiver, empty;
  std::string commands = "send-keys -H 1b 5b 41\n\033OA\036.";
  commands += '\0';
  sender.push_back( Terminal::TmuxBytes( commands ) );
  sender.push_back( Parser::Resize( 100, 35 ) );
  receiver.apply_string( sender.diff_from( empty ) );
  require( receiver == sender, "raw commands and resize round trip" );
  require( receiver.is_tmux_event( 0 ) && receiver.get_tmux_input( 0 ) == commands, "raw command payload" );
  require( receiver.tmux_input_size() == commands.size(), "input byte accounting" );
  sender.subtract( &receiver );
  require( sender.empty() && sender.tmux_input_size() == 0, "ACK releases input queue" );
}
}

int main()
{
  std::setlocale( LC_ALL, "" );
  try {
    test_parser();
    test_reliable_output();
    test_input();
    std::cout << "tmux control tests passed\n";
  } catch ( const std::exception& error ) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
