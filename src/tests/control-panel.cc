/* Distributed under the GNU GPL, version 3 or later. */
#include <chrono>
#include <algorithm>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "src/frontend/controlpanel.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"

namespace {
void require( bool value, const char* message )
{
  if ( !value ) {
    throw std::runtime_error( message );
  }
}
uint64_t now()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now().time_since_epoch() )
    .count();
}
Control::Message query( const std::string& path, uint64_t id )
{
  Control::Message request;
  request.set_kind( Control::Message::OPEN );
  request.set_request( id );
  request.set_path( path );
  return request;
}
Control::Message await_reply( Control::DirectoryWorker& worker )
{
  const uint64_t deadline = now() + 10000;
  Control::Message reply;
  while ( !worker.pop( reply ) ) {
    require( now() < deadline, "directory worker deadline" );
    worker.tick( now() );
    usleep( 1000 );
  }
  return reply;
}

void channel_test()
{
  const auto request = query( "/tmp", 17 );
  const std::string bytes = Control::frame( request );
  for ( size_t split = 0; split <= bytes.size(); split++ ) {
    Control::Channel receiver;
    Control::Message decoded;
    receiver.receive( bytes.substr( 0, split ) );
    if ( split != bytes.size() ) {
      require( !receiver.pop( decoded ), "partial frame" );
    }
    receiver.receive( bytes.substr( split ) );
    require( receiver.pop( decoded ) && decoded.SerializeAsString() == request.SerializeAsString(),
             "fragmented frame" );
    require( !receiver.pop( decoded ), "single delivery" );
  }
  Control::Channel sender, receiver;
  auto large = query( std::string( 3000, 'x' ), 18 );
  require( sender.queue( large ), "queue page" );
  Network::StreamEvent event;
  uint64_t clock = 100, last = 0, ack = 0;
  while ( sender.queued_bytes() ) {
    require( sender.take( clock, last, ack, event ), "next ACK permits next chunk" );
    require( event.data.size() <= 512 && event.stream_id == 0, "bounded interactive chunk" );
    require( !sender.take( clock + 1000, last + 2, ack, event ), "no more metadata while ACK is lost" );
    receiver.receive( event.data );
    last += 3; // Terminal updates continue independently of the channel.
    ack = last;
    clock += 25;
  }
  Control::Message decoded;
  require( receiver.pop( decoded ) && decoded.path() == large.path(), "complete ACK-paced message" );
  for ( int i = 0; i < 100; i++ ) {
    sender.queue( large );
  }
  require( sender.queued_bytes() <= 2 * Control::MAX_MESSAGE, "bounded disconnected queue" );
  bool rejected = false;
  try {
    std::string bad( 4, '\xff' );
    Control::unframe( bad, decoded );
  } catch ( const std::runtime_error& ) {
    rejected = true;
  }
  require( rejected, "oversized frame rejected before allocation" );

  // Exercise SST's cumulative repair and ACK-prefix subtraction with
  // adjacent side-channel chunks (which the serializer coalesces).
  Terminal::Complete blank( 80, 24 ), first( blank ), later( blank ), received( blank );
  first.push_back( Network::StreamEvent::data_event( 0, bytes.substr( 0, 7 ) ) );
  later = first;
  later.push_back( Network::StreamEvent::data_event( 0, bytes.substr( 7 ) ) );
  received.apply_string( later.diff_from( blank ) ); // first packet was lost
  Control::Channel repaired;
  for ( const auto& item : received.get_stream_events() ) {
    repaired.receive( item.data );
  }
  require( repaired.pop( decoded ) && decoded.request() == 17, "lost state recovered cumulatively" );
  require( received.diff_from( received ).empty(), "duplicate state has no extra stream bytes" );
  later.subtract( &first );
  require( later.get_stream_events().size() == 1, "ACK frees earlier chunks" );
}

void panel_test()
{
  Control::Panel panel( true );
  const std::string alt = "\033[57443;3:1u\033[57443;1:3u\033[57449;3:1u\033[57449;1:3u";
  require( panel.input( alt, 0 ) == alt, "standalone left/right Alt events reach remote exactly" );
  const std::string zero_keys = "\0330\033[48;3u\033[48;3:1u\033[48;3:2u\033[48;1:3u\033[48;5u";
  require( panel.input( zero_keys, 0 ) == zero_keys && !panel.active(), "Alt-0 and Ctrl-0 are no longer reserved" );
  require( panel.input( "abc\033[A\033OB", 0 ) == "abc\033[A\033OB", "normal keys preserved" );
  require( panel.input( "\033[200~paste\0360\033[201~", 0 ) == "\033[200~paste\0360\033[201~",
           "paste cannot open panel" );
  require( !panel.active(), "still hidden" );
  panel.toggle( 2 );
  require( panel.active(), "client command opens panel" );
  require( panel.input( alt, 2 ) == alt, "popup does not strand remote modifier keys" );
  const auto command = []( const std::string& key ) { return key == "\036" || key == "0" || key == "\033[54;6u"; };
  require( panel.input( "\0360", 2, command ) == "\0360", "visible popup passes local commands to client" );
  require( panel.input( "\033[54;", 2, command ).empty(), "split command key is buffered" );
  require( panel.input( "6u", 2, command ) == "\033[54;6u", "complete CSI-u command key reaches client" );
  require( panel.input( "\033[200~\0360\033[201~", 2, command ).empty(), "pasted commands never bypass visible popup" );
  const uint64_t id = panel.pane( 1 ).request;
  Control::Message reply;
  reply.set_kind( Control::Message::REPLY );
  reply.set_request( id );
  reply.set_path( "/remote" );
  reply.set_cursor( 1 );
  for ( const char* name : { "a", "b", "evil\033[2J" } ) {
    auto* entry = reply.add_entry();
    entry->set_name( name );
    entry->set_type( Control::Message::Entry::DIRECTORY );
  }
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  panel.input( "\t\033[B", 3 );
  require( panel.pane( 1 ).pages[0].selected == 1, "selection" );
  const auto saved = panel.pane( 1 ).pages[0].listing.SerializeAsString();
  panel.toggle( 4 );
  require( !panel.active(), "hide" );
  panel.toggle( 5 );
  require( panel.pane( 1 ).request == id && panel.pane( 1 ).pages[0].selected == 1
             && panel.pane( 1 ).pages[0].listing.SerializeAsString() == saved,
           "reopen preserves pane without requery" );

  Terminal::Framebuffer underlying( 80, 24 );
  underlying.get_mutable_cell( 0, 0 )->append( 'A' );
  auto composed = underlying;
  panel.paint( composed );
  require( composed.get_cell( 3, 2 )->debug_contents().find( "╭" ) != std::string::npos, "rounded Unicode window" );
  require( composed.get_cell( 0, 0 )->debug_contents() == underlying.get_cell( 0, 0 )->debug_contents(),
           "outside overlay stays live" );
  require( underlying.get_cell( 2, 2 )->empty() && !composed.get_cell( 2, 3 )->empty(),
           "overlay never mutates remote state" );
  for ( int row = 0; row < 24; row++ ) {
    for ( int col = 0; col < 80; col++ ) {
      require( composed.get_cell( row, col )->debug_contents().find( '\033' ) == std::string::npos,
               "filename cannot inject escapes" );
    }
  }
  panel.input( "\033[15;193:3~\033[15;5:1~", 6 );
  require( panel.pane( 1 ).error.empty(), "F5 release and Ctrl-F5 cannot start copies" );
  panel.input( "\033[15;193:1~", 6 );
  require( panel.pane( 1 ).error.find( "librsync" ) != std::string::npos, "enhanced Kitty F5 honors lock modifiers and checks capability" );
  panel.input( "\r", 6 );
  require( panel.pane( 1 ).request == id && panel.pane( 1 ).error.find( "librsync" ) != std::string::npos,
           "Enter offers transfer instead of opening a directory" );
  for ( const auto& tab : { "\t", "\033[Z", "\033[1;2Z", "\033[9;2u", "\033[1;194:1Z", "\033[9;194:1u" } ) {
    panel.input( tab, 6 ); panel.input( "\033[A", 6 );
    require( panel.pane( 1 ).pages[0].selected == 1, "Tab or Shift-Tab moves focus to local pane" );
    panel.input( tab, 6 ); panel.input( "\033[A", 6 );
    require( panel.pane( 1 ).pages[0].selected == 0, "Tab or Shift-Tab moves focus back to remote pane" );
    panel.input( "\033[B", 6 );
  }
  panel.input( "\033[1;194:3Z\033[9;194:3u\033[1;193:3A\033[1;5A", 6 );
  require( panel.pane( 1 ).pages[0].selected == 1, "modified arrows and navigation releases do nothing" );
  panel.input( "\033[1;193:1A\033[1;193:3A", 6 );
  require( panel.pane( 1 ).pages[0].selected == 0, "Kitty arrow press with lock modifiers moves once" );
  panel.input( "\033[1;193:2B", 6 );
  require( panel.pane( 1 ).pages[0].selected == 1, "Kitty arrow repeat moves selection" );
  panel.input( "\033[1;193:3C\033[1;5C", 6 );
  require( panel.pane( 1 ).request == id, "Right release and Ctrl-Right do not navigate" );
  panel.input( "\033[1;193:1C", 6 ); // Navigate to selected b.
  require( panel.pane( 1 ).request > id && panel.pane( 1 ).loading, "new navigation has a new id" );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  require( panel.pane( 1 ).loading && panel.pane( 1 ).path == "/remote", "late reply cannot replace navigation" );
  reply.set_request( panel.pane( 1 ).request );
  reply.set_path( "/remote/b" );
  panel.toggle( 7 );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  require( !panel.active() && panel.pane( 1 ).path == "/remote/b", "in-flight navigation completes while hidden" );
  panel.toggle( 8 );
  for ( const auto& parent : { "\177", "\010", "\033[D", "\033OD", "\033[1;193:1D", "\033[127;193u" } ) {
    const uint64_t previous = panel.pane( 1 ).request;
    panel.input( parent, 8 );
    require( panel.pane( 1 ).request > previous && panel.pane( 1 ).loading, "Left or Backspace requests parent directory" );
    reply.set_request( panel.pane( 1 ).request );
    panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  }
  // A regular file is deliberately inert under Right, not an error or copy.
  panel.input( "r", 8 ); reply.set_request( panel.pane( 1 ).request );
  reply.mutable_entry( 0 )->set_type( Control::Message::Entry::FILE );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  const uint64_t file_request = panel.pane( 1 ).request;
  panel.input( "\033[C\033OC\033[1;193:1C", 8 );
  require( panel.pane( 1 ).request == file_request && panel.pane( 1 ).error.empty(), "Right on a file does nothing" );
  panel.toggle( 9 );
  panel.input( "\033", 10 );
  require( panel.flush_input( 59 ).empty() && panel.flush_input( 60 ) == "\033", "ordinary Escape timeout" );
}

void viewport_test()
{
  Control::Panel panel( true );
  panel.toggle( now() ); panel.input( "\t", now() );
  const auto window = [&]( unsigned number ) {
    Control::Message reply;
    reply.set_kind( Control::Message::REPLY ); reply.set_request( panel.pane( 1 ).request );
    reply.set_path( "/remote" ); reply.set_revision( 1 );
    reply.set_start_cursor( number ); reply.set_cursor( number + 1 );
    if ( number ) { reply.set_previous_cursor( number - 1 ); }
    for ( unsigned i = 0; i < 64; ++i ) {
      auto* entry = reply.add_entry();
      entry->set_name( std::to_string( 10000 + number * 64 + i ) );
      entry->set_type( Control::Message::Entry::FILE );
    }
    panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  };
  const auto rendered = [&]( size_t top, int selected_row, int height = 24 ) {
    Terminal::Framebuffer fb( 80, height );
    panel.paint( fb );
    require( panel.pane( 1 ).scroll_top == top, "viewport keeps its independent scroll offset" );
    Terminal::Renditions selection( 4 ); selection.set_background_color( 117 );
    for ( int row = 0; row < height - 14; ++row ) {
      require( ( fb.get_cell( 6 + row, 41 )->get_renditions().get_background_rendition()
                 == selection.get_background_rendition() ) == ( row == selected_row ),
               "highlight moves within viewport, not pinned to its bottom" );
    }
    const size_t absolute_top = panel.pane( 1 ).first_page * 64 + top;
    const auto name = std::to_string( 10000 + absolute_top );
    for ( size_t i = 0; i < name.size(); ++i ) {
      require( fb.get_cell( 6, 43 + i )->debug_contents().find( "'" + name.substr( i, 1 ) + "'" ) != std::string::npos,
               "visible first filename matches scroll offset across cached windows" );
    }
  };
  window( 0 ); rendered( 0, 0 );
  panel.input( std::string( 15, 'j' ), now() ); rendered( 6, 9 );
  panel.input( "k", now() ); rendered( 6, 8 );
  panel.input( std::string( 8, 'k' ), now() ); rendered( 6, 0 );
  panel.input( "k", now() ); rendered( 5, 0 );
  panel.input( "j", now() ); rendered( 5, 1 );
  panel.toggle( now() ); panel.toggle( now() ); rendered( 5, 1 );
  panel.input( "\tjj\033[Z", now() ); rendered( 5, 1 );
  panel.input( "\033[6;193:1~", now() ); rendered( 7, 9 );
  panel.input( "\033[5;193:1~", now() ); rendered( 6, 0 );
  panel.input( std::string( 9, 'j' ), now() ); rendered( 6, 9 );
  rendered( 6, 9, 30 ); // A taller viewport does not pull the list upward.
  rendered( 10, 5, 20 ); // A smaller viewport keeps selection visible.
  panel.input( "k", now() ); rendered( 10, 4, 20 );
  rendered( 10, 4 );

  // Crossing an invisible transport window, evicting old windows and then
  // fetching them in reverse must preserve the same viewport behavior.
  panel.input( std::string( 50, 'j' ), now() );
  require( panel.pane( 1 ).loading, "next window requested" );
  panel.input( "k", now() ); rendered( 54, 8 );
  window( 1 ); rendered( 54, 8 ); // Delayed data must not undo the reversal.
  panel.input( "jj", now() ); rendered( 55, 9 );
  panel.input( "k", now() ); rendered( 55, 8 );
  panel.input( "j", now() ); rendered( 55, 9 );
  for ( unsigned number = 2; number <= 8; ++number ) {
    panel.input( std::string( 64, 'j' ), now() );
    require( panel.pane( 1 ).loading, "continuation requested while scrolling down" );
    window( number );
  }
  require( panel.pane( 1 ).pages.size() == 8 && panel.pane( 1 ).first_page == 1, "oldest window evicted" );
  rendered( 439, 9 );
  panel.input( "k", now() ); rendered( 439, 8 );
  panel.input( std::string( 448, 'k' ), now() );
  require( panel.pane( 1 ).loading, "evicted previous window requested" );
  panel.input( "j", now() ); rendered( 0, 1 );
  window( 0 ); rendered( 64, 1 );
  panel.input( "kk", now() ); rendered( 63, 0 );
  panel.input( "j", now() ); rendered( 63, 1 );
  panel.input( "\177", now() ); window( 0 ); rendered( 0, 0 );

  for ( unsigned number = 1; number <= 7; ++number ) {
    panel.input( std::string( 64, 'j' ), now() ); window( number );
  }
  panel.input( std::string( 64, 'j' ), now() );
  panel.input( std::string( 511, 'k' ), now() );
  window( 8 ); rendered( 0, 0 );
  require( panel.pane( 1 ).pages.size() == 8 && panel.pane( 1 ).first_page == 0,
           "late forward prefetch does not evict selected first window" );
  panel.input( std::string( 512, 'j' ), now() ); window( 8 );
  panel.input( std::string( 449, 'k' ), now() );
  panel.input( std::string( 511, 'j' ), now() );
  window( 0 ); rendered( 502, 9 );
  require( panel.pane( 1 ).pages.size() == 8 && panel.pane( 1 ).first_page == 1,
           "late reverse prefetch does not evict selected last window" );
}

void continuous_test()
{
  Control::Panel panel( true, true );
  panel.toggle( now() );
  Control::Message reply;
  reply.set_kind( Control::Message::REPLY ); reply.set_request( panel.pane( 1 ).request );
  reply.set_path( "/remote" ); reply.set_cursor( 1 ); reply.set_start_cursor( 0 ); reply.set_revision( 7 );
  for ( int i = 0; i < 64; ++i ) { auto* e = reply.add_entry(); e->set_name( "file-" + std::to_string( i ) ); e->set_size( i ); }
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  panel.input( "\t" + std::string( 64, 'j' ), now() );
  require( panel.pane( 1 ).loading && panel.pane( 1 ).pages[0].selected == 63, "arrow scroll requests continuation without clearing list" );
  reply.set_request( panel.pane( 1 ).request ); reply.set_start_cursor( 1 ); reply.set_previous_cursor( 0 );
  reply.set_eof( true ); reply.clear_entry(); reply.add_entry()->set_name( "last-file" );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  require( panel.pane( 1 ).page == 1 && panel.pane( 1 ).pages.size() == 2, "selection crosses invisible chunk boundary" );
  panel.input( "k", now() );
  require( panel.pane( 1 ).page == 0 && panel.pane( 1 ).pages[0].selected == 63, "up scroll crosses back continuously" );
  reply.set_update( true ); reply.set_delta( true ); reply.set_start_cursor( 0 ); reply.clear_entry();
  auto* changed = reply.add_entry(); changed->set_name( "file-63" ); changed->set_size( 32768 );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  require( panel.pane( 1 ).pages[0].listing.entry( 63 ).size() == 32768 && panel.pane( 1 ).pages[0].selected == 63,
           "live size delta preserves scrolling selection" );
  reply.set_delta( false ); reply.set_reset( true ); reply.set_revision( 8 );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  require( panel.pane( 1 ).pages.size() == 1 && panel.pane( 1 ).pages[0].selected == 0, "new directory generation discards stale cursors and preserves selected name" );
}

void live_worker_test()
{
  char path[] = "/tmp/goblin-live-test.XXXXXX";
  require( mkdtemp( path ), "live temporary directory" );
  const auto first = std::string( path ) + "/first", second = std::string( path ) + "/Alpha";
  int fd = open( first.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600 );
  require( fd >= 0, "live test file" );
  {
    Control::DirectoryWorker worker;
    auto request = query( path, 1 ); request.set_live( true );
    worker.request( request, now() );
    auto initial = await_reply( worker );
    require( initial.entry_size() == 1 && initial.entry( 0 ).has_size() && initial.entry( 0 ).size() == 0, "initial metadata" );
    const auto check = [&]() {
      auto watch = query( path, 1 ); watch.set_kind( Control::Message::WATCH ); watch.set_live( true );
      watch.set_cursor( 0 ); worker.request( watch, now() ); worker.tick( now() );
    };
    check();
    for ( int i = 0; i < 100; ++i ) { worker.tick( now() ); usleep( 1000 ); }
    Control::Message unchanged;
    require( !worker.pop( unchanged ), "unchanged watch has zero wire traffic" );
    require( ftruncate( fd, 12345 ) == 0, "change file size" );
    check();
    auto resized = await_reply( worker );
    require( resized.delta() && resized.entry_size() == 1 && resized.entry( 0 ).size() == 12345, "size change emits only metadata delta" );
    int extra = open( second.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600 );
    require( extra >= 0, "new entry" ); close( extra );
    check(); auto added = await_reply( worker );
    require( added.reset() && added.entry_size() == 2 && added.revision() > initial.revision(), "addition invalidates directory generation" );
    require( added.entry( 0 ).name() == "Alpha" && added.entry( 1 ).name() == "first", "membership change rebuilds alphabetical order" );
    require( unlink( second.c_str() ) == 0, "remove entry" );
    check(); auto removed = await_reply( worker );
    require( removed.reset() && removed.entry_size() == 1, "removal updates listing" );
    auto pause = query( path, 1 ); pause.set_kind( Control::Message::WATCH ); pause.set_live( false );
    worker.request( pause, now() );
    require( worker.wait_time() == INT_MAX, "hidden watcher sleeps" );
  }
  close( fd ); unlink( first.c_str() ); rmdir( path );
}

void parent_sort_test()
{
  char path[] = "/tmp/goblin-parent-test.XXXXXX";
  require( mkdtemp( path ), "parent-sort temporary directory" );
  const auto child = std::string( path ) + "/middle";
  require( mkdir( child.c_str(), 0700 ) == 0, "parent-sort child directory" );
  const std::vector<std::string> expected = { "Alpha", "beta", "bravo", "middle", "Zulu" };
  // Nonalphabetical creation order, on case-sensitive or insensitive hosts.
  for ( const auto& name : { "Zulu", "bravo", "beta", "Alpha" } ) {
    const int fd = open( ( std::string( path ) + "/" + name ).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600 );
    require( fd >= 0, "parent-sort test file" ); close( fd );
  }
  {
    Control::DirectoryWorker worker;
    uint64_t serial = 0;
    // The same worker implementation serves local and remote requests,
    // with or without live-directory negotiation.
    for ( bool live : { false, true } ) {
      for ( const auto& location : { std::string( path ), child, child + "/.." } ) {
        auto request = query( location, ++serial ); request.set_live( live );
        worker.request( request, now() );
        const auto reply = await_reply( worker );
        require( reply.error().empty() && reply.eof(), "navigation listing completed" );
        if ( location == child ) { require( reply.entry_size() == 0, "empty child" ); continue; }
        require( reply.entry_size() == int( expected.size() ), "all parent entries returned" );
        for ( size_t i = 0; i < expected.size(); ++i ) {
          require( reply.entry( i ).name() == expected[i], "parent navigation rebuilds alphabetical listing" );
        }
      }
    }
  }
  for ( const auto& name : expected ) {
    if ( name != "middle" ) { require( unlink( ( std::string( path ) + "/" + name ).c_str() ) == 0, "clean parent-sort file" ); }
  }
  require( rmdir( child.c_str() ) == 0 && rmdir( path ) == 0, "clean parent-sort directories" );
}

void rapid_membership_test( bool native_notifications )
{
  char path[] = "/tmp/goblin-membership-test.XXXXXX";
  require( mkdtemp( path ), "rapid membership temporary directory" );
  const auto first = std::string( path ) + "/Alpha", second = std::string( path ) + "/Zulu";
  {
    Control::DirectoryWorker worker( native_notifications );
    auto request = query( path, 1 ); request.set_live( true );
    worker.request( request, now() );
    auto listing = await_reply( worker );
    require( listing.error().empty() && listing.eof() && listing.entry_size() == 0, "initial empty membership" );
    const auto check = [&]() {
      auto watch = query( path, 1 ); watch.set_kind( Control::Message::WATCH ); watch.set_live( true );
      watch.set_cursor( 0 ); watch.set_revision( listing.revision() );
      worker.request( watch, now() ); worker.tick( now() );
    };
    const auto expect = [&]( const char* name ) {
      const uint64_t previous = listing.revision();
      check(); listing = await_reply( worker );
      require( listing.error().empty() && listing.reset() && !listing.delta() && listing.eof()
               && listing.revision() > previous, "rapid membership invalidates the generation" );
      require( listing.entry_size() == ( name ? 1 : 0 ), "rapid membership exact entry count" );
      if ( name ) {
        require( listing.entry( 0 ).name() == name && listing.entry( 0 ).type() == Control::Message::Entry::FILE,
                 "rapid membership contains the current filename" );
      }
    };
    // Deliberately no sleeps to advance the filesystem clock between changes.
    // This reproduces the 100 Hz kernel race and also tests eventless polling.
    for ( unsigned repeat = 0; repeat < 24; ++repeat ) {
      const int fd = open( first.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600 );
      require( fd >= 0, "rapid create" ); close( fd );
      expect( "Alpha" );
      require( rename( first.c_str(), second.c_str() ) == 0, "rapid rename" );
      expect( "Zulu" );
      require( unlink( second.c_str() ) == 0, "rapid unlink" );
      expect( nullptr );
    }
    check();
    for ( unsigned repeat = 0; repeat < 100; ++repeat ) { worker.tick( now() ); usleep( 1000 ); }
    Control::Message unchanged;
    require( !worker.pop( unchanged ), "unchanged native/polled membership generates no wire traffic" );
  }
  require( rmdir( path ) == 0, "clean rapid membership directory" );
}

void enter_transfer_test()
{
  if ( !Files::available() ) { return; }
  Files::Endpoint files( false, true, Crypto::Mode::LegacyOCB );
  Control::Panel panel( true );
  panel.toggle( now() );
  Control::Message reply;
  reply.set_kind( Control::Message::REPLY ); reply.set_request( panel.pane( 1 ).request );
  reply.set_path( "/remote" ); reply.set_eof( true );
  auto* entry = reply.add_entry(); entry->set_name( "report.txt" ); entry->set_type( Control::Message::Entry::FILE );
  panel.receive( Network::StreamEvent::data_event( 0, Control::frame( reply ) ) );
  const uint64_t deadline = now() + 10000;
  while ( panel.pane( 0 ).loading ) {
    require( now() < deadline, "local pane load for transfer test" ); panel.tick( now() ); usleep( 1000 );
  }
  require( panel.pane( 0 ).error.empty(), "local destination available" );
  // No tick after queueing: this tests authorization without writing files.
  panel.set_files( &files ); panel.input( "\t", now() );
  Terminal::Framebuffer fb( 100, 30 );
  panel.paint( fb );
  panel.input( "\033[200~\r\033[201~\033[13;1:3u\033[13;1:2u", now() );
  require( files.queue_size() == 0, "paste, release and held Enter cannot start a transfer" );
  panel.input( "\r", now() );
  require( files.queue_size() == 1 && files.jobs().back().name() == "report.txt" && !files.jobs().back().send(),
           "one Enter queues transfer immediately" );
  panel.paint( fb );
  require( panel.active() && fb.get_cell( 3, 2 )->debug_contents().find( "╭" ) != std::string::npos,
           "queueing keeps the two-pane browser visible" );
  panel.input( "\033[13;193:1u\033[13;193:2u\033[13;193:3u", now() );
  require( files.queue_size() == 2, "Kitty Enter press queues once without a confirmation screen" );
  panel.input( "\033[15;193:1~\033[15;193:2~\033[15;193:3~", now() );
  require( files.queue_size() == 3, "F5 also queues directly, ignoring repeat and release" );
  panel.input( "x", now() );
  require( files.queue_size() == 0, "browser cancellation is still available immediately" );
}

void download_consent_test()
{
  Control::Panel panel( true );
  Terminal::Framebuffer display( 80, 24 );
  uint64_t token = 0; bool allow = false;
  panel.offer_download( 1, "report.pdf", 2048, "/local/Downloads" );
  require( panel.input( "y", 0 ) == "y" && !panel.take_download_decision( token, allow ), "ordinary terminal input never approves" );
  panel.toggle( 1 ); panel.input( "y", 1 );
  require( panel.active() && panel.fd() < 0 && !panel.take_download_decision( token, allow ), "approval requires rendered review and starts no filesystem worker" );
  panel.paint( display );
  panel.input( "\033[200~y\r\0360\033[201~", 2 );
  require( panel.active() && !panel.take_download_decision( token, allow ), "pasted confirmation cannot approve or dismiss" );
  panel.input( "\033[121;1:3u", 3 );
  require( !panel.take_download_decision( token, allow ), "key release cannot approve" );
  panel.input( "y\r", 4 );
  require( panel.active() && !panel.take_download_decision( token, allow ), "queued select and confirm cannot approve without repaint" );
  panel.paint( display ); panel.input( "\r", 4 );
  require( !panel.active() && panel.take_download_decision( token, allow ) && token == 1 && allow, "reviewed deliberate confirmation approves exactly this file" );
  require( !panel.take_download_decision( token, allow ), "decision is consumed once" );
  panel.offer_download( 2, "second.pdf", 4096, "/local/Downloads" );
  panel.input( "yy", 5 );
  require( !panel.take_download_decision( token, allow ), "held key cannot approve next file" );
  panel.toggle( 6 ); panel.paint( display ); panel.input( "\r", 7 );
  require( panel.take_download_decision( token, allow ) && token == 2 && !allow, "Enter defaults to decline" );
  panel.offer_download( 3, std::string( 220, 'x' ), 0, "/local/Downloads" );
  panel.toggle( 8 );
  Terminal::Framebuffer small( 40, 12 ); panel.paint( small ); panel.input( "y", 9 );
  require( !panel.take_download_decision( token, allow ), "truncated filename cannot be approved" );
  panel.paint( display ); panel.input( "y", 10 );
  panel.paint( display ); panel.input( "\r", 10 );
  require( panel.take_download_decision( token, allow ) && token == 3 && allow, "full filename after resize can be approved" );
  panel.offer_download( 4, "expired", 0, "/local/Downloads" );
  panel.toggle( 11 ); panel.paint( display );
  panel.offer_download( 5, "replacement", 0, "/local/Downloads" ); panel.input( "y", 12 );
  require( !panel.take_download_decision( token, allow ), "replacement must be rendered before consent" );
  panel.offer_download( 0, "", 0, "" ); panel.input( "y", 13 );
  require( !panel.take_download_decision( token, allow ), "canceled prompt cannot be approved" );
}

void download_popup_test()
{
  Control::Panel panel( true );
  Terminal::Framebuffer display( 80, 24 );
  uint64_t token = 0; bool allow = false;
  panel.offer_download( 1, "automatic.pdf", 2048, "/local/Downloads", true );
  require( !panel.active() && panel.wait_time( 0, 0 ) == 0, "offer wakes idle session for automatic popup" );
  panel.tick( 0 );
  require( panel.active() && panel.fd() < 0 && panel.wait_time( 0, 0 ) > 0, "popup opens once without a worker or busy loop" );
  panel.paint( display ); panel.input( "yyy\r", 1 );
  require( panel.active() && !panel.take_download_decision( token, allow ), "held key and queued Enter cannot save" );
  panel.paint( display ); panel.input( "\r", 2 );
  require( panel.take_download_decision( token, allow ) && token == 1 && allow, "automatic popup confirms one rendered selection" );
  panel.offer_download( 2, "next.pdf", 4096, "/local/Downloads", true );
  panel.tick( 3 ); panel.input( "y", 3 );
  require( !panel.take_download_decision( token, allow ), "typeahead cannot select an unrendered replacement" );
  panel.paint( display ); panel.input( "\r", 4 );
  require( panel.take_download_decision( token, allow ) && token == 2 && !allow, "new popup defaults to decline" );

  panel.offer_download( 3, "pending.pdf", 0, "/local/Downloads", true );
  panel.tick( 5 ); panel.paint( display ); panel.input( "y", 6 ); panel.paint( display );
  panel.toggle( 7 );
  panel.offer_download( 3, "pending.pdf", 0, "/local/Downloads", true ); panel.tick( 8 );
  require( !panel.active() && panel.input( "terminal input", 8 ) == "terminal input", "same pending offer stays hidden with live terminal input" );
  panel.toggle( 9 ); panel.paint( display ); panel.input( "\r", 10 );
  require( panel.take_download_decision( token, allow ) && token == 3 && !allow, "reopen restores offer without stale Save selection" );

  require( panel.input( "\033[200~start", 11 ) == "\033[200~start", "hidden paste starts normally" );
  panel.offer_download( 4, "after-paste.pdf", 0, "/local/Downloads", true ); panel.tick( 12 );
  require( !panel.active() && panel.wait_time( 12, 0 ) > 0, "automatic popup waits for paste without spinning" );
  require( panel.input( "y\r\0360\033[201~", 13 ) == "y\r\0360\033[201~", "paste and end marker reach underlying application intact" );
  require( panel.wait_time( 13, 0 ) == 0, "paste completion wakes pending popup" );
  panel.tick( 13 ); panel.paint( display );
  require( panel.active() && !panel.take_download_decision( token, allow ), "paste cannot approve automatic popup" );
  panel.offer_download( 0, "", 0, "" );
  require( !panel.active(), "withdrawn offer closes popup" );

  require( panel.input( "\033[", 14 ).empty(), "split key is buffered" );
  panel.offer_download( 5, "after-key.pdf", 0, "/local/Downloads", true ); panel.tick( 15 );
  require( !panel.active() && panel.wait_time( 15, 0 ) > 0, "automatic popup waits for split key" );
  require( panel.input( "A", 16 ) == "\033[A", "split key reaches underlying application intact" );
  panel.tick( 16 );
  require( panel.active() && panel.fd() < 0, "split key completion permits popup without filesystem work" );
  panel.offer_download( 0, "", 0, "" );

  panel.offer_download( 6, "manual-first.pdf", 0, "/local/Downloads", true );
  panel.toggle( 17 ); panel.toggle( 17 ); panel.tick( 18 );
  require( !panel.active(), "manually reviewed and hidden offer does not open automatically again" );
  panel.offer_download( 0, "", 0, "" );
}

void worker_test( size_t count )
{
  char path[] = "/tmp/goblin-directory-test.XXXXXX";
  require( mkdtemp( path ) != nullptr, "temporary directory" );
  std::vector<std::string> files;
  for ( size_t i = 0; i < count; i++ ) {
    const auto name = std::string( path ) + "/file-" + std::to_string( i );
    const int fd = open( name.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600 );
    require( fd >= 0, "test file" );
    close( fd );
    files.push_back( name );
  }
  {
    Control::DirectoryWorker worker;
    const uint64_t began = now();
    auto initial = query( path, 1 ); initial.set_live( true );
    worker.request( initial, began );
    auto reply = await_reply( worker );
    require( reply.error().empty() && !reply.eof(), "first bounded alphabetical window" );
    require( reply.entry_size() <= 64 && Control::frame( reply ).size() <= Control::PAGE_BYTES,
             "both page bounds" );
    std::cout << count << " directory entries; first " << reply.entry_size() << " in " << now() - began << " ms, "
              << Control::frame( reply ).size() << " framed bytes\n";
    auto next = query( reply.path(), 2 );
    next.set_kind( Control::Message::PAGE );
    next.set_live( true );
    next.set_cursor( reply.cursor() );
    worker.request( next, now() );
    auto second = await_reply( worker );
    require( second.error().empty() && second.request() == 2, "continuation page" );
    std::vector<std::string> expected;
    for ( size_t i = 0; i < count; ++i ) { expected.push_back( "file-" + std::to_string( i ) ); }
    std::sort( expected.begin(), expected.end() );
    size_t at = 0;
    for ( const auto& entry : reply.entry() ) { require( entry.name() == expected.at( at++ ), "first window alphabetical" ); }
    for ( const auto& entry : second.entry() ) { require( entry.name() == expected.at( at++ ), "alphabetical across window boundary" ); }
    std::set<std::string> names;
    for ( const auto& entry : reply.entry() ) {
      names.insert( entry.name() );
    }
    for ( const auto& entry : second.entry() ) {
      require( names.insert( entry.name() ).second, "no duplicate entries across pages" );
    }

    // A stopped process is a deterministic stalled-filesystem surrogate.
    const pid_t old_pid = worker.pid();
    require( kill( old_pid, SIGSTOP ) == 0, "stop worker" );
    worker.request( next, now() );
    const uint64_t start = now();
    for ( int i = 0; i < 1000; i++ ) {
      worker.tick( now() );
    }
    require( now() - start < 1000, "session loop must not wait for stalled worker" );
    require( worker.pid() == old_pid, "no extra worker while NAS query is pending" );
    // Timeout cannot synchronously wait for the helper.
    worker.tick( now() + 30001 );
    Control::Message timeout;
    require( worker.pop( timeout ) && !timeout.error().empty(), "bounded query deadline" );
    worker.request( query( path, 3 ), now() );
    reply = await_reply( worker );
    require( reply.request() == 3 && reply.error().empty(), "replacement starts after old child is reaped" );
  }
  for ( const auto& file : files ) {
    require( unlink( file.c_str() ) == 0, "clean test file" );
  }
  require( rmdir( path ) == 0, "clean test directory" );
}
}

int main( int argc, char** argv )
{
  if ( !std::setlocale( LC_CTYPE, "en_US.UTF-8" ) && !std::setlocale( LC_CTYPE, "C.UTF-8" ) ) { return 77; }
  try {
    channel_test();
    panel_test();
    continuous_test();
    viewport_test();
    live_worker_test();
    rapid_membership_test( true );
    rapid_membership_test( false );
    parent_sort_test();
    enter_transfer_test();
    download_consent_test();
    download_popup_test();
    worker_test( argc == 2 && std::string( argv[1] ) == "--stress" ? 100000 : 1024 );
    std::cout << "control panel tests passed\n";
  } catch ( const std::exception& error ) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
