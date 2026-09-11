/* Distributed under the GNU GPL, version 3 or later. */
#include "directory.h"
#include "config.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <signal.h>
#include <stdexcept>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#elif defined( __APPLE__ ) || defined( __FreeBSD__ ) || defined( __OpenBSD__ ) || defined( __NetBSD__ )
#include <sys/event.h>
#endif

namespace Control {
namespace {
ssize_t send_bytes( int fd, const std::string& bytes )
{
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  return send( fd, bytes.data(), bytes.size(), flags );
}

enum class Metadata { Present, Missing, Unavailable };

Metadata metadata( DIR* directory, Message::Entry& entry )
{
  struct stat st = {};
  entry.clear_size();
  // Never follow a link or open a device while inspecting a name.
  if ( fstatat( dirfd( directory ), entry.name().c_str(), &st, AT_SYMLINK_NOFOLLOW ) == 0 ) {
    entry.set_type( S_ISDIR( st.st_mode ) ? Message::Entry::DIRECTORY
                    : S_ISREG( st.st_mode ) ? Message::Entry::FILE
                    : S_ISLNK( st.st_mode ) ? Message::Entry::LINK : Message::Entry::UNKNOWN );
    if ( st.st_size >= 0 && !S_ISDIR( st.st_mode ) ) { entry.set_size( st.st_size ); }
    return Metadata::Present;
  }
  const bool missing = errno == ENOENT || errno == ENOTDIR;
  entry.set_type( Message::Entry::UNKNOWN );
  return missing ? Metadata::Missing : Metadata::Unavailable;
}

bool same_directory( const struct stat& a, const struct stat& b )
{
#ifdef __APPLE__
  const auto am = a.st_mtimespec, bm = b.st_mtimespec, ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
  const auto am = a.st_mtim, bm = b.st_mtim, ac = a.st_ctim, bc = b.st_ctim;
#endif
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec
         && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

bool alphabetical( const std::string& a, const std::string& b )
{
  // Stable, locale-independent ordering across hosts. Fold ASCII case and
  // use the original bytes to break ties; UTF-8 names retain byte ordering.
  for ( size_t i = 0; i < std::min( a.size(), b.size() ); ++i ) {
    const unsigned char ac = a[i], bc = b[i];
    const unsigned af = ac >= 'A' && ac <= 'Z' ? ac + 'a' - 'A' : ac;
    const unsigned bf = bc >= 'A' && bc <= 'Z' ? bc + 'a' - 'A' : bc;
    if ( af != bf ) { return af < bf; }
  }
  return a.size() != b.size() ? a.size() < b.size() : a < b;
}

// Timestamp resolution varies by kernel/filesystem. Observe membership events
// independently of mtime/ctime, with bounded, nonblocking work per request.
// This object exists only in the killable filesystem worker, never the session.
class DirectoryWatch
{
  int fd = -1;

public:
  DirectoryWatch() = default;
  DirectoryWatch( const DirectoryWatch& ) = delete;
  DirectoryWatch& operator=( const DirectoryWatch& ) = delete;
  ~DirectoryWatch() { reset(); }
  bool active() const { return fd >= 0; }
  void reset()
  {
    if ( fd >= 0 ) { close( fd ); fd = -1; }
  }
  void start( DIR* directory, const std::string& path )
  {
    reset();
    (void)directory; (void)path;
#ifdef __linux__
    fd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
    if ( fd >= 0 && inotify_add_watch( fd, path.c_str(), IN_ONLYDIR | IN_CREATE | IN_DELETE
                                      | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF ) < 0 ) {
      reset();
    }
#elif defined( __APPLE__ ) || defined( __FreeBSD__ ) || defined( __OpenBSD__ ) || defined( __NetBSD__ )
    fd = kqueue();
    if ( fd < 0 ) { return; }
    struct kevent event;
    EV_SET( &event, dirfd( directory ), EVFILT_VNODE, EV_ADD | EV_CLEAR,
            NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE, 0, nullptr );
    if ( fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0 || kevent( fd, &event, 1, nullptr, 0, nullptr ) < 0 ) { reset(); }
#endif
  }
  bool changed()
  {
    if ( fd < 0 ) { return false; }
#ifdef __linux__
    char events[4096];
    const ssize_t count = read( fd, events, sizeof events );
    // Any event, including overflow, unmount or an ignored watch, invalidates
    // the snapshot. Reopening/rearming drops the old queue before rescanning.
    if ( count > 0 ) { return true; }
    if ( count < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) ) { return false; }
#elif defined( __APPLE__ ) || defined( __FreeBSD__ ) || defined( __OpenBSD__ ) || defined( __NetBSD__ )
    struct kevent event;
    const struct timespec immediate = { 0, 0 };
    const int count = kevent( fd, nullptr, 0, &event, 1, &immediate );
    if ( count > 0 ) { return true; }
    if ( count == 0 || ( count < 0 && errno == EINTR ) ) { return false; }
#endif
    reset();
    return true;
  }
};

uint64_t monotonic_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now().time_since_epoch() ).count();
}

bool same_names( DIR* directory, const std::vector<std::string>& names )
{
  // No per-file stat calls, second name cache or wire traffic. Remote changes
  // on filesystems such as NFS need this fallback even when a watch exists.
  rewinddir( directory );
  size_t count = 0;
  while ( true ) {
    errno = 0;
    const auto* entry = readdir( directory );
    if ( !entry ) { return errno == 0 && count == names.size(); }
    const std::string name = entry->d_name;
    if ( name == "." || name == ".." ) { continue; }
    if ( ++count > names.size() || !std::binary_search( names.begin(), names.end(), name, alphabetical ) ) { return false; }
  }
}

void worker( int fd, bool native_notifications )
{
  // Do not retain a PTY, a network socket, or another helper's descriptors.
  const int max_fd = getdtablesize();
  for ( int i = 0; i < max_fd; i++ ) {
    if ( i != fd ) {
      close( i );
    }
  }
  sigset_t mask;
  sigemptyset( &mask );
  sigprocmask( SIG_SETMASK, &mask, nullptr );
  for ( int signal : { SIGINT, SIGTERM, SIGHUP, SIGALRM } ) {
    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    sigemptyset( &action.sa_mask );
    sigaction( signal, &action, nullptr );
  }
  DIR* directory = nullptr;
  DirectoryWatch changes;
  constexpr uint64_t NAME_RECHECK_MS = 30000;
  uint64_t next_name_check = 0;
  std::string path, incoming;
  struct Cursor { size_t position; uint64_t previous; };
  std::map<uint64_t, Cursor> cursors;
  std::map<uint64_t, Message> windows;
  std::vector<std::string> names;
  size_t position = 0;
  struct stat observed = {};
  uint64_t revision = 0;
  uint64_t next_cursor = 1;
  while ( true ) {
    Message request;
    while ( !unframe( incoming, request ) ) {
      char bytes[1024];
      const ssize_t n = read( fd, bytes, sizeof bytes );
      if ( n < 0 && errno == EINTR ) {
        continue;
      }
      if ( n <= 0 ) {
        _exit( 0 );
      }
      incoming.append( bytes, n );
    }
    Message reply;
    reply.set_kind( Message::REPLY );
    reply.set_request( request.request() );
    const bool check = request.kind() == Message::CHECK;
    bool restart = request.kind() == Message::OPEN;
    struct stat current = {};
    if ( !restart && directory && request.live() ) {
      // Membership changes invalidate the sorted snapshot. Rebuild it here,
      // in the killable worker, never in the network/UI thread or on the wire.
      if ( stat( path.c_str(), &current ) < 0 || !same_directory( observed, current )
           || ( request.has_revision() && request.revision() != revision ) || changes.changed() ) {
        restart = true;
      } else if ( ( check && !changes.active() ) || monotonic_ms() >= next_name_check ) {
        restart = !same_names( directory, names );
        next_name_check = monotonic_ms() + NAME_RECHECK_MS;
      }
      if ( restart ) { reply.set_reset( true ); }
    }
    if ( check && !restart ) {
      reply.set_path( path );
      reply.set_revision( revision );
      reply.set_start_cursor( request.cursor() );
      reply.set_update( true );
      const auto found = windows.find( request.cursor() );
      if ( found != windows.end() ) {
        reply.set_delta( true );
        for ( auto& entry : *found->second.mutable_entry() ) {
          const auto before = entry.SerializeAsString();
          if ( metadata( directory, entry ) == Metadata::Missing ) {
            // An unstamped/unnotified deletion is a membership change, not an
            // UNKNOWN file. Discard any partial delta and rebuild the names.
            restart = true;
            reply.clear_entry(); reply.clear_delta(); reply.set_reset( true );
            break;
          }
          if ( entry.SerializeAsString() != before ) { *reply.add_entry() = entry; }
        }
        if ( !restart && reply.entry_size() == 0 ) { reply.set_unchanged( true ); }
      } else { reply.set_error( "Directory window expired; r refreshes" ); }
    }
    if ( !check || restart ) {
      if ( restart ) {
        changes.reset();
        if ( directory ) {
          closedir( directory );
          directory = nullptr;
        }
        cursors.clear();
        windows.clear();
        names.clear();
        position = 0;
        revision = std::max( revision + 1, request.request() );
        const std::string supplied = request.path().empty() ? "." : request.path();
        char* resolved = realpath( supplied.c_str(), nullptr );
        if ( resolved ) {
          path = resolved;
          free( resolved );
          directory = opendir( path.c_str() );
          if ( directory ) {
            // Arm before the scan: changes racing with enumeration must not
            // be lost even when the final directory timestamps are identical.
            if ( native_notifications ) { changes.start( directory, path ); }
            fstat( dirfd( directory ), &observed );
            size_t bytes = 0;
            while ( true ) {
              errno = 0;
              const auto* entry = readdir( directory );
              if ( !entry ) {
                if ( errno ) { reply.set_error( strerror( errno ) ); }
                break;
              }
              const std::string name = entry->d_name;
              if ( name == "." || name == ".." ) { continue; }
              bytes += name.size() + sizeof( std::string ) + 32;
              if ( names.size() >= 1000000 || bytes > 64 * 1024 * 1024 ) {
                reply.set_error( "Directory exceeds the sorted-name cache limit" );
                names.clear();
                break;
              }
              names.push_back( name );
            }
            if ( reply.error().empty() ) { std::sort( names.begin(), names.end(), alphabetical ); }
            else { names.clear(); changes.reset(); closedir( directory ); directory = nullptr; }
            next_name_check = monotonic_ms() + NAME_RECHECK_MS;
          }
        }
        if ( !directory && reply.error().empty() ) {
          reply.set_error( strerror( errno ) );
        }
      } else {
        auto cursor = cursors.find( request.cursor() );
        if ( !directory || ( request.cursor() && cursor == cursors.end() ) || request.path() != path ) {
          reply.set_error( "Directory cursor expired; press r to refresh" );
        } else {
          if ( request.cursor() ) {
            position = cursor->second.position;
            reply.set_previous_cursor( cursor->second.previous );
          } else { position = 0; }
        }
      }
      reply.set_path( path );
      reply.set_start_cursor( restart ? 0 : request.cursor() );
      reply.set_revision( revision );
      if ( check ) { reply.set_update( true ); }
      size_t name_bytes = path.size() + 128;
      while ( reply.error().empty() && reply.entry_size() < int( PAGE_ENTRIES ) ) {
        if ( position == names.size() ) { reply.set_eof( true ); break; }
        const std::string& name = names[position];
        if ( name_bytes + name.size() + 32 > PAGE_BYTES ) {
          break;
        }
        auto* item = reply.add_entry();
        item->set_name( name );
        // Sorting reads names only; stat calls remain bounded to this window.
        metadata( directory, *item );
        name_bytes += name.size() + 32;
        ++position;
      }
      if ( directory && reply.error().empty() && !reply.eof() ) {
        if ( reply.entry_size() == 0 ) {
          reply.set_error( "Path too long for a directory page" );
        } else {
          cursors[next_cursor] = { position, reply.start_cursor() };
          reply.set_cursor( next_cursor++ );
          // Bound cursor memory independently of directory size.
          if ( cursors.size() > 8192 ) {
            cursors.erase( cursors.begin() );
          }
        }
      }
      if ( request.live() && reply.error().empty() ) {
        windows[reply.start_cursor()] = reply;
        while ( windows.size() > 8 ) {
          auto evict = windows.begin();
          if ( evict->first == reply.start_cursor() ) { ++evict; }
          windows.erase( evict );
        }
      }
    }
    std::string output = frame( reply );
    while ( !output.empty() ) {
      const ssize_t n = send_bytes( fd, output );
      if ( n < 0 && errno == EINTR ) {
        continue;
      }
      if ( n <= 0 ) {
        _exit( 0 );
      }
      output.erase( 0, n );
    }
  }
}
}

std::string frame( const Message& message )
{
  std::string bytes;
  if ( !message.SerializeToString( &bytes ) || bytes.size() > MAX_MESSAGE ) {
    throw std::runtime_error( "Oversized control message" );
  }
  const uint32_t size = bytes.size();
  std::string result;
  for ( int shift = 24; shift >= 0; shift -= 8 ) {
    result += char( size >> shift );
  }
  return result + bytes;
}

bool unframe( std::string& input, Message& message )
{
  if ( input.size() < 4 ) {
    return false;
  }
  uint32_t size = 0;
  for ( unsigned i = 0; i < 4; i++ ) {
    size = ( size << 8 ) | uint8_t( input[i] );
  }
  if ( size > MAX_MESSAGE ) {
    throw std::runtime_error( "Oversized control frame" );
  }
  if ( input.size() < size + 4 ) {
    return false;
  }
  if ( !message.ParseFromArray( input.data() + 4, size ) ) {
    throw std::runtime_error( "Malformed control frame" );
  }
  input.erase( 0, size + 4 );
  return true;
}

bool Channel::queue( const Message& message )
{
  const std::string bytes = frame( message );
  if ( outgoing.size() + bytes.size() > 2 * MAX_MESSAGE ) {
    return false;
  }
  outgoing += bytes;
  return true;
}

void Channel::receive( const std::string& data )
{
  if ( incoming.size() + data.size() > 4 * MAX_MESSAGE ) {
    throw std::runtime_error( "Control receive queue exceeded" );
  }
  incoming += data;
}

int Channel::wait_time( uint64_t now, uint64_t acked ) const
{
  if ( outgoing.empty() || acked < barrier ) {
    return INT_MAX;
  }
  return now >= next_send ? 0 : int( next_send - now );
}

bool Channel::take( uint64_t now, uint64_t last, uint64_t acked, Network::StreamEvent& event )
{
  if ( wait_time( now, acked ) != 0 ) {
    return false;
  }
  const size_t size = std::min<size_t>( 512, outgoing.size() );
  event = Network::StreamEvent::data_event( STREAM_ID, outgoing.substr( 0, size ), Network::StreamPriorityMedium );
  outgoing.erase( 0, size );
  barrier = last + 1;
  next_send = now + 25;
  return true;
}

DirectoryWorker::~DirectoryWorker()
{
  stop();
  if ( child > 0 ) {
    waitpid( child, nullptr, WNOHANG );
  }
}

void DirectoryWorker::stop()
{
  if ( child > 0 ) {
    kill( child, SIGKILL );
  }
  if ( socket >= 0 ) {
    close( socket );
    socket = -1;
  }
  restarting = child > 0;
  busy = false;
  incoming.clear();
  outgoing.clear();
}

bool DirectoryWorker::spawn()
{
  int pair[2];
  if ( socketpair( AF_UNIX, SOCK_STREAM, 0, pair ) < 0 ) {
    return false;
  }
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  setsockopt( pair[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled );
  setsockopt( pair[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled );
#endif
  child = fork();
  if ( child == 0 ) {
    try {
      worker( pair[1], native_notifications );
    } catch ( ... ) {
      _exit( 1 );
    }
    _exit( 0 );
  }
  close( pair[1] );
  if ( child < 0 ) {
    close( pair[0] );
    return false;
  }
  socket = pair[0];
  if ( fcntl( socket, F_SETFL, O_NONBLOCK ) < 0 || fcntl( socket, F_SETFD, FD_CLOEXEC ) < 0 ) {
    stop();
    return false;
  }
  return true;
}

void DirectoryWorker::fail( const std::string& error )
{
  Message reply;
  reply.set_kind( Message::REPLY );
  reply.set_request( pending ? wanted.request() : current.request() );
  reply.set_update( !pending && current.kind() == Message::CHECK );
  reply.set_error( error );
  replies.clear();
  replies.push_back( reply );
  watching = false;
  stop();
}

void DirectoryWorker::request( const Message& message, uint64_t now )
{
  if ( message.kind() == Message::WATCH ) {
    if ( message.path().size() > 3072 || message.path().find( '\0' ) != std::string::npos ) { return; }
    watching = message.live();
    watch_request = message;
    watch_pending = watching;
    return;
  }
  if ( message.kind() != Message::OPEN && message.kind() != Message::PAGE ) {
    return;
  }
  if ( message.path().size() > 3072 || message.path().find( '\0' ) != std::string::npos ) {
    current = message; pending = false;
    fail( "Invalid or oversized directory path" );
    return;
  }
  if ( message.kind() == Message::PAGE && busy ) {
    // A short metadata check must not destroy the cursor needed by a scroll.
    // Navigation to a different directory (OPEN) still cancels immediately.
    wanted = message; pending = true; watch_pending = false;
    return;
  }
  current = message;
  watch_pending = false;
  watching = message.live();
  replies.clear();
  // Navigation cancels old work, but never spawns another child until that
  // child is reaped. Repeated navigation coalesces to just the newest request.
  if ( busy || message.kind() == Message::OPEN ) {
    stop();
  }
  wanted = message;
  pending = true;
  tick( now );
}

void DirectoryWorker::tick( uint64_t now )
{
  if ( child > 0 ) {
    const pid_t result = waitpid( child, nullptr, WNOHANG );
    if ( result == child || ( result < 0 && errno == ECHILD ) ) {
      child = -1;
      if ( busy && !pending ) {
        fail( "Directory worker exited" );
      }
      stop();
      restarting = false;
    }
  }
  if ( restarting ) {
    return;
  }
  if ( watch_pending && !busy && !pending && replies.empty() ) {
    current = watch_request; next_check = now; watch_pending = false;
  }
  if ( watching && !busy && !pending && replies.empty() && socket >= 0 && now >= next_check ) {
    wanted = current;
    wanted.set_kind( Message::CHECK );
    wanted.set_live( true );
    pending = true;
  }
  if ( pending && !busy ) {
    pending = false;
    current = wanted;
    if ( socket < 0 && !spawn() ) {
      fail( "Cannot start directory worker" );
      return;
    }
    outgoing = frame( current );
    incoming.clear();
    busy = true;
    started = now;
  }
  if ( !busy ) {
    return;
  }
  if ( now - started >= 30000 ) {
    fail( "Directory query timed out; press r to retry" );
    return;
  }
  if ( !outgoing.empty() ) {
    const ssize_t n = send_bytes( socket, outgoing );
    if ( n > 0 ) {
      outgoing.erase( 0, n );
    } else if ( n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) {
      fail( "Directory worker write failed" );
      return;
    }
  }
  char buffer[8192];
  const ssize_t n = read( socket, buffer, sizeof buffer );
  if ( n > 0 ) {
    incoming.append( buffer, n );
    Message reply;
    if ( unframe( incoming, reply ) ) {
      busy = false;
      replies.clear();
      if ( reply.request() == current.request() ) {
        if ( reply.error().empty() ) {
          current.set_path( reply.path() );
          current.set_cursor( reply.start_cursor() );
          current.set_revision( reply.revision() );
        } else { watching = false; }
        next_check = now + 3000;
        if ( !reply.unchanged() ) { replies.push_back( reply ); }
      }
    }
  } else if ( n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) ) {
    fail( "Directory worker disconnected" );
  }
}

int DirectoryWorker::wait_time() const
{
  if ( restarting || pending || !outgoing.empty() ) {
    return 25;
  }
  return busy ? 250 : watching ? 1000 : INT_MAX;
}

bool DirectoryWorker::pop( Message& message )
{
  if ( replies.empty() ) {
    return false;
  }
  message = replies.front();
  replies.pop_front();
  return true;
}
}
