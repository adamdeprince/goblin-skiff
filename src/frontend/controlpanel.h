/* Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_CONTROLPANEL_H
#define MOSH_CONTROLPANEL_H

#include "directory.h"
#include "filetransfer.h"
#include "src/terminal/terminalframebuffer.h"
#include <functional>

namespace Control {
bool decode_key_event( const std::string& input, unsigned& code, unsigned& modifiers, unsigned& event );
int key_event_byte( unsigned code, unsigned modifiers );
// Lifetime is the connection, not the popup. Hiding is purely a rendering
// decision; it does not destroy workers, requests, paths or selections.
class Panel
{
public:
  struct Page
  {
    Message listing;
    size_t selected = 0;
  };
  struct Pane
  {
    std::string path = ".", error;
    std::deque<Page> pages;
    size_t page = 0, first_page = 0;
    // Index within the bounded cache, independent of the selected row.
    // Mutable so resizing the painted viewport can keep selection visible.
    mutable size_t scroll_top = 0;
    uint64_t request = 0;
    bool loading = false, opening = false, prepend = false, advance = false;
    bool watching = false;
    uint64_t watched_cursor = 0;
  };

private:
  Files::Endpoint* files = nullptr;
  struct DownloadOffer {
    uint64_t token = 0, bytes = 0;
    std::string name {}, directory {};
  } download;
  bool download_view = false;
  mutable bool download_reviewed = false;
  bool download_popup_pending = false, download_save_selected = false;
  mutable bool download_save_reviewed = false;
  uint64_t decided_download = 0;
  bool download_allowed = false;
  bool visible = false, initialized = false, remote_enabled, remote_live, remote_pending = false;
  mutable int viewport_rows = 12;
  std::string traffic = "Link: waiting for traffic";
  bool paste = false;
  std::string command_hint = "Ctrl-^ 0";
  unsigned focused = 0;
  uint64_t serial = 0, escape_started = 0;
  std::string escape;
  Pane panes[2];
  DirectoryWorker local;
  Channel channel;
  Message pending_request;
  void open( unsigned pane, const std::string& path, uint64_t now );
  void request( unsigned pane, Message message, uint64_t now );
  void accept( unsigned pane, const Message& message );
  void fetch( unsigned side, uint64_t cursor, bool backwards, bool advance, uint64_t now );
  void move( unsigned side, int amount, uint64_t now );
  void keep_visible( const Pane& pane ) const;
  void key( const std::string& key, uint64_t now );

public:
  explicit Panel( bool enabled, bool live = false ) : remote_enabled( enabled ), remote_live( live ) {}
  void set_files( Files::Endpoint* endpoint ) { files = endpoint; }
  void set_traffic( const std::string& text ) { traffic = text; }
  void toggle( uint64_t now );
  void set_command_hint( const std::string& hint ) { command_hint = hint; }
  bool active() const { return visible; }
  const Pane& pane( unsigned side ) const { return panes[side]; }
  void paint( Terminal::Framebuffer& framebuffer ) const;
  using CommandFilter = std::function<bool( const std::string& )>;
  std::string input( const std::string& bytes, uint64_t now, const CommandFilter& command = {} );
  std::string flush_input( uint64_t now, const CommandFilter& command = {} );
  void tick( uint64_t now );
  int fd() const { return local.fd(); }
  int wait_time( uint64_t now, uint64_t acked ) const;
  void receive( const Network::StreamEvent& event );
  void offer_download( uint64_t token, const std::string& name, uint64_t bytes, const std::string& directory, bool popup = false );
  bool take_download_decision( uint64_t& token, bool& allow );
  bool take( uint64_t now, uint64_t last, uint64_t acked, Network::StreamEvent& event )
  {
    return channel.take( now, last, acked, event );
  }
};
}
#endif
