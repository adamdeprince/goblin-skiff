/* Distributed under the GNU GPL, version 3 or later. */
#include "config.h"
#include "filetransfer.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_FILE_SYNC
#include <librsync.h>
#include <openssl/evp.h>
#include <openssl/provider.h>
#endif

namespace Files {
namespace {
constexpr uint64_t MAX_FILE = uint64_t( 64 ) * 1024 * 1024 * 1024;
[[maybe_unused]] constexpr size_t MAX_SIGNATURE = 8 * 1024 * 1024;
constexpr size_t QUEUE_BYTES = 256 * 1024;

bool basename( const std::string& name )
{
  return !name.empty() && name != "." && name != ".." && name.size() <= 255
         && name.find_first_of( std::string( "/\0", 2 ) ) == std::string::npos;
}
bool absolute( const std::string& path )
{
  return !path.empty() && path.front() == '/' && path.size() <= 3072 && path.find( '\0' ) == std::string::npos;
}
[[maybe_unused]] bool relative( const std::string& path )
{
  if ( path.empty() || path.front() == '/' || path.size() > 3072 ) { return false; }
  size_t start = 0;
  for ( unsigned depth = 0; depth < 64; ++depth ) {
    const auto slash = path.find( '/', start );
    if ( !basename( path.substr( start, slash == std::string::npos ? slash : slash - start ) ) ) { return false; }
    if ( slash == std::string::npos ) { return true; }
    start = slash + 1;
  }
  return false;
}
bool decode( const std::string& bytes, Record& r )
{
  r.Clear();
  return bytes.size() <= MAX_RECORD && r.ParseFromString( bytes ) && r.job()
         && r.path().size() <= 3072 && r.data().size() <= CHUNK && r.sha256().size() <= 32
         && r.status().size() <= 512 && r.size() <= MAX_FILE;
}
std::string frame( const Record& r )
{
  const auto bytes = r.SerializeAsString();
  if ( bytes.size() > MAX_RECORD ) { throw std::runtime_error( "File record too large" ); }
  std::string out;
  for ( int shift = 24; shift >= 0; shift -= 8 ) { out += char( bytes.size() >> shift ); }
  return out + bytes;
}
bool unframe( std::string& bytes, Record& r )
{
  if ( bytes.size() < 4 ) { return false; }
  uint32_t n = 0;
  for ( unsigned i = 0; i < 4; ++i ) { n = ( n << 8 ) | uint8_t( bytes[i] ); }
  if ( n > MAX_RECORD ) { throw std::runtime_error( "File IPC record too large" ); }
  if ( bytes.size() < n + 4 ) { return false; }
  if ( !decode( bytes.substr( 4, n ), r ) ) { throw std::runtime_error( "Invalid file IPC record" ); }
  bytes.erase( 0, n + 4 );
  return true;
}
ssize_t send_bytes( int fd, const std::string& bytes )
{
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  return send( fd, bytes.data(), bytes.size(), flags );
}
Command outcome( uint64_t id, const std::string& text, bool done = false, bool failed = false )
{
  Command c;
  c.set_kind( Command::STATUS ); c.set_id( id ); c.set_status( text.substr( 0, 512 ) );
  c.set_done( done ); c.set_failed( failed );
  return c;
}

#ifdef HAVE_FILE_SYNC
using File = std::unique_ptr<FILE, decltype( &fclose )>;
struct FD {
  int value;
  explicit FD( int fd ) : value( fd ) { if ( fd < 0 ) { throw std::runtime_error( strerror( errno ) ); } }
  ~FD() { if ( value >= 0 ) { close( value ); } }
  FD( const FD& ) = delete;
  FD& operator=( const FD& ) = delete;
  FD( FD&& other ) noexcept : value( other.value ) { other.value = -1; }
};
void check( bool success, const char* operation )
{
  if ( !success ) { throw std::runtime_error( std::string( operation ) + ": " + strerror( errno ) ); }
}
File as_file( int fd )
{
  check( fd >= 0, "open file" );
  FILE* f = fdopen( fd, "rb+" );
  if ( !f ) { close( fd ); throw std::runtime_error( "Cannot open file stream" ); }
  return File( f, fclose );
}
File input_file( int dir, const std::string& name )
{
  const int fd = openat( dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK );
  check( fd >= 0, "open input" );
  struct stat st = {};
  if ( fstat( fd, &st ) < 0 || !S_ISREG( st.st_mode ) || st.st_size < 0 || uint64_t( st.st_size ) > MAX_FILE ) {
    close( fd ); throw std::runtime_error( "Input must be a regular file of at most 64 GiB" );
  }
  FILE* f = fdopen( fd, "rb" );
  if ( !f ) { close( fd ); throw std::runtime_error( "Cannot open input stream" ); }
  return File( f, fclose );
}
File temporary()
{
  FILE* f = tmpfile();
  check( f != nullptr, "temporary file" );
  return File( f, fclose );
}
void rewind_file( FILE* f ) { check( fseeko( f, 0, SEEK_SET ) == 0, "rewind" ); }
uint64_t file_size( FILE* f )
{
  check( fflush( f ) == 0, "flush" );
  struct stat st = {};
  check( fstat( fileno( f ), &st ) == 0 && st.st_size >= 0, "file size" );
  return st.st_size;
}
bool unchanged( const struct stat& a, const struct stat& b )
{
#ifdef __APPLE__
  const auto am = a.st_mtimespec, bm = b.st_mtimespec, ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
  const auto am = a.st_mtim, bm = b.st_mtim, ac = a.st_ctim, bc = b.st_ctim;
#endif
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mode == b.st_mode
         && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
FD directory_at( int parent, const std::string& path, bool create )
{
  FD current( dup( parent ) );
  size_t start = 0;
  while ( start < path.size() ) {
    const auto slash = path.find( '/', start );
    const auto name = path.substr( start, slash == std::string::npos ? slash : slash - start );
    if ( !basename( name ) ) { throw std::runtime_error( "Unsafe directory component" ); }
    if ( create && mkdirat( current.value, name.c_str(), 0777 ) < 0 && errno != EEXIST ) {
      throw std::runtime_error( std::string( "mkdir: " ) + strerror( errno ) );
    }
    FD next( openat( current.value, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW ) );
    std::swap( current.value, next.value );
    if ( slash == std::string::npos ) { break; }
    start = slash + 1;
  }
  return current;
}
FD absolute_directory( const std::string& path )
{
  if ( !absolute( path ) ) { throw std::runtime_error( "Invalid directory root" ); }
  FD root( open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC ) );
  return directory_at( root.value, path.substr( 1 ), false );
}

class Digest {
  OSSL_LIB_CTX* library = nullptr;
  OSSL_PROVIDER *base = nullptr, *fips = nullptr;
  EVP_MD* algorithm = nullptr;
  EVP_MD_CTX* context = nullptr;
public:
  explicit Digest( Crypto::Mode mode )
  {
    if ( mode == Crypto::Mode::FipsAES128GCM ) {
      library = OSSL_LIB_CTX_new();
      // Match the session's configured FIPS module and integrity data. A
      // failed provider/configuration never falls back to a default digest.
      if ( library && OSSL_LIB_CTX_load_config( library, nullptr ) == 1
           && EVP_set_default_properties( library, "fips=yes,provider=fips" ) == 1 ) {
        base = OSSL_PROVIDER_load( library, "base" ); fips = OSSL_PROVIDER_load( library, "fips" );
        if ( fips && OSSL_PROVIDER_self_test( fips ) != 1 ) { OSSL_PROVIDER_unload( fips ); fips = nullptr; }
      }
    }
    if ( mode != Crypto::Mode::FipsAES128GCM || ( library && base && fips ) ) {
      algorithm = EVP_MD_fetch( library, "SHA256", mode == Crypto::Mode::FipsAES128GCM ? "fips=yes,provider=fips" : nullptr );
      context = EVP_MD_CTX_new();
    }
  }
  ~Digest()
  {
    EVP_MD_CTX_free( context ); EVP_MD_free( algorithm );
    OSSL_PROVIDER_unload( fips ); OSSL_PROVIDER_unload( base ); OSSL_LIB_CTX_free( library );
  }
  std::string hash( FILE* file )
  {
    if ( !algorithm || !context || EVP_DigestInit_ex( context, algorithm, nullptr ) != 1 ) {
      throw std::runtime_error( "SHA256 provider unavailable" );
    }
    rewind_file( file );
    unsigned char bytes[CHUNK];
    while ( const size_t n = fread( bytes, 1, sizeof bytes, file ) ) {
      if ( EVP_DigestUpdate( context, bytes, n ) != 1 ) { throw std::runtime_error( "SHA256 update failed" ); }
    }
    check( !ferror( file ), "read for SHA256" );
    unsigned char result[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if ( EVP_DigestFinal_ex( context, result, &length ) != 1 || length != 32 ) {
      throw std::runtime_error( "SHA256 finish failed" );
    }
    return std::string( reinterpret_cast<char*>( result ), length );
  }
};

void rs_check( rs_result result )
{
  if ( result != RS_DONE ) { throw std::runtime_error( std::string( "librsync: " ) + rs_strerror( result ) ); }
}
void run_job( rs_job_t* raw, FILE* input, FILE* output, uint64_t limit )
{
  if ( !raw ) { throw std::runtime_error( "Cannot create librsync job" ); }
  std::unique_ptr<rs_job_t, decltype( &rs_job_free )> job( raw, rs_job_free );
  rewind_file( input );
  rs_buffers_t buffers = {};
  char in[CHUNK], out[CHUNK]; uint64_t written = 0;
  while ( true ) {
    if ( !buffers.avail_in && !buffers.eof_in ) {
      buffers.avail_in = fread( in, 1, sizeof in, input ); buffers.next_in = in;
      check( !ferror( input ), "read librsync input" ); buffers.eof_in = feof( input );
    }
    buffers.next_out = out; buffers.avail_out = sizeof out;
    const size_t before = buffers.avail_in;
    const rs_result result = rs_job_iter( job.get(), &buffers );
    const size_t n = sizeof out - buffers.avail_out;
    if ( n > limit - written ) { throw std::runtime_error( "librsync output exceeds size limit" ); }
    check( fwrite( out, 1, n, output ) == n, "write librsync output" ); written += n;
    if ( result == RS_DONE ) {
      if ( buffers.avail_in || fgetc( input ) != EOF || ferror( input ) ) {
        throw std::runtime_error( "Trailing librsync input" );
      }
      break;
    }
    if ( result != RS_BLOCKED && result != RS_RUNNING ) { rs_check( result ); }
    if ( buffers.eof_in && before == buffers.avail_in && !n ) {
      throw std::runtime_error( "Incomplete librsync input" );
    }
  }
  check( fflush( output ) == 0, "flush librsync output" );
}

// Only one publishing temporary file exists at a time in a worker. SIGTERM
// removes it without touching any completed file. Anonymous scratch files
// used for signatures/deltas disappear automatically when the worker exits.
volatile sig_atomic_t cleanup_fd = -1;
char cleanup_name[96] = {};
void terminate_worker( int )
{
  if ( cleanup_fd >= 0 ) { unlinkat( cleanup_fd, cleanup_name, 0 ); }
  _exit( 1 );
}
struct Publish {
  int parent;
  std::string name;
  File output;
  Publish( int directory, uint64_t serial )
    : parent( directory ), name( ".goblin-copy-" + std::to_string( getpid() ) + "-" + std::to_string( serial ) ),
      output( nullptr, fclose )
  {
    // Block termination until ownership of the O_EXCL staging name is known.
    // Never clean up somebody else's colliding file.
    if ( name.size() >= sizeof cleanup_name ) { throw std::runtime_error( "Temporary name too long" ); }
    sigset_t blocked, previous; sigemptyset( &blocked );
    for ( int signal : { SIGTERM, SIGINT, SIGHUP } ) { sigaddset( &blocked, signal ); }
    sigprocmask( SIG_BLOCK, &blocked, &previous );
    memcpy( cleanup_name, name.c_str(), name.size() + 1 );
    const int fd = openat( parent, name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600 );
    if ( fd >= 0 ) { cleanup_fd = parent; }
    const int saved_errno = errno;
    sigprocmask( SIG_SETMASK, &previous, nullptr ); errno = saved_errno;
    check( fd >= 0, "create destination temporary" );
    try { output = as_file( fd ); }
    catch ( ... ) { unlinkat( parent, name.c_str(), 0 ); cleanup_fd = -1; throw; }
  }
  ~Publish() { unlinkat( parent, name.c_str(), 0 ); cleanup_fd = -1; }
};

class Runner {
  int socket;
  Command command;
  Digest digest;
  std::string input;
  uint64_t sequence = 0, files = 0, bytes = 0, visited = 0;
  Record record( Record::Kind kind ) const
  {
    Record r; r.set_kind( kind ); r.set_job( command.id() ); r.set_serial( sequence ); return r;
  }
  void emit( const Record& r )
  {
    std::string out = frame( r );
    while ( !out.empty() ) {
      const ssize_t n = send_bytes( socket, out );
      if ( n < 0 && errno == EINTR ) { continue; }
      check( n > 0, "file worker send" ); out.erase( 0, n );
    }
  }
  Record receive()
  {
    Record r;
    while ( !unframe( input, r ) ) {
      char buffer[MAX_RECORD]; const ssize_t n = read( socket, buffer, sizeof buffer );
      if ( n < 0 && errno == EINTR ) { continue; }
      check( n > 0, "file worker receive" ); input.append( buffer, n );
    }
    if ( r.job() != command.id() || r.kind() == Record::PROGRESS ) { throw std::runtime_error( "Unexpected file record" ); }
    return r;
  }
  void expect( Record::Kind kind )
  {
    const auto r = receive();
    if ( r.kind() != kind || r.serial() != sequence ) { throw std::runtime_error( "Unexpected file acknowledgment" ); }
  }
  void notify( const std::string& state, bool done = false, bool failed = false )
  {
    auto r = record( Record::PROGRESS ); r.set_status( state.substr( 0, 512 ) );
    r.set_files( files ); r.set_bytes( bytes ); r.set_done( done ); r.set_failed( failed ); emit( r );
  }
  void stream( FILE* file, Record::Kind kind, bool delta = false )
  {
    rewind_file( file ); char buffer[CHUNK];
    while ( const size_t n = fread( buffer, 1, sizeof buffer, file ) ) {
      auto r = record( kind ); r.set_data( buffer, n ); r.set_delta( delta ); emit( r );
    }
    check( !ferror( file ), "read file payload" );
  }
  void send_file( int parent, const std::string& name, const std::string& path )
  {
    auto source = input_file( parent, name ); struct stat before = {}, after = {};
    check( fstat( fileno( source.get() ), &before ) == 0, "source stat" );
    notify( "Checking " + path );
    auto offer = record( Record::OFFER ); offer.set_path( path ); offer.set_size( before.st_size );
    offer.set_mode( before.st_mode & 0777 ); emit( offer );
    auto signature = temporary(); uint64_t signature_bytes = 0; bool use_delta = false;
    while ( true ) {
      const auto r = receive();
      if ( r.serial() != sequence ) { throw std::runtime_error( "Signature file mismatch" ); }
      if ( r.kind() == Record::SIGNATURE_END ) { use_delta = r.delta(); break; }
      if ( r.kind() != Record::BASIS_SIGNATURE || r.data().empty() || r.data().size() > MAX_SIGNATURE - signature_bytes ) {
        throw std::runtime_error( "Invalid or oversized signature" );
      }
      check( fwrite( r.data().data(), 1, r.data().size(), signature.get() ) == r.data().size(), "write signature" );
      signature_bytes += r.data().size();
    }
    auto delta = temporary();
    if ( use_delta ) {
      // Accept only our modern 32-byte BLAKE2 signature shape, bounding the
      // checksum table before handing an authenticated peer's data to librsync.
      rewind_file( signature.get() ); unsigned char header[12];
      check( fread( header, 1, sizeof header, signature.get() ) == sizeof header, "signature header" );
      uint32_t magic = 0, block = 0, strong = 0;
      for ( unsigned i = 0; i < 4; ++i ) {
        magic = ( magic << 8 ) | header[i]; block = ( block << 8 ) | header[i + 4]; strong = ( strong << 8 ) | header[i + 8];
      }
      if ( magic != RS_RK_BLAKE2_SIG_MAGIC || !block || strong != 32 || ( signature_bytes - 12 ) % 36 ) {
        throw std::runtime_error( "Unsupported librsync signature" );
      }
      rewind_file( signature.get() ); rs_signature_t* raw = nullptr;
      const auto result = rs_loadsig_file( signature.get(), &raw, nullptr );
      std::unique_ptr<rs_signature_t, decltype( &rs_free_sumset )> sums( raw, rs_free_sumset );
      rs_check( result ); rs_check( rs_build_hash_table( sums.get() ) );
      notify( "Computing delta: " + path );
      run_job( rs_delta_begin( sums.get() ), source.get(), delta.get(), uint64_t( before.st_size ) * 2 + 4096 );
      // Still use librsync to compare existing files, but do not send a delta
      // larger than the source. Both representations use the same FEC lane.
      use_delta = file_size( delta.get() ) < uint64_t( before.st_size );
    } else if ( signature_bytes ) { throw std::runtime_error( "Unexpected signature for a new file" ); }
    const auto hash = digest.hash( source.get() );
    check( fstat( fileno( source.get() ), &after ) == 0, "source recheck" );
    if ( !unchanged( before, after ) ) { throw std::runtime_error( "Source changed while computing transfer" ); }
    notify( std::string( use_delta ? "Sending delta: " : "Sending file: " ) + path );
    stream( use_delta ? delta.get() : source.get(), Record::DATA, use_delta );
    check( fstat( fileno( source.get() ), &after ) == 0, "source final recheck" );
    if ( !unchanged( before, after ) ) { throw std::runtime_error( "Source changed while sending" ); }
    auto end = record( Record::END ); end.set_delta( use_delta ); end.set_sha256( hash ); emit( end );
    expect( Record::SAVED ); ++files; bytes += before.st_size; notify( "Saved " + path );
  }
  void visit( int parent, const std::string& name, const std::string& path, unsigned depth )
  {
    if ( depth >= 64 || !relative( path ) || ++visited > 1000000 ) { throw std::runtime_error( "Directory transfer limit exceeded" ); }
    struct stat st = {}; check( fstatat( parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW ) == 0, "source stat" );
    ++sequence;
    if ( S_ISREG( st.st_mode ) ) { send_file( parent, name, path ); return; }
    if ( !S_ISDIR( st.st_mode ) ) { throw std::runtime_error( "Symlinks and special files are not copied: " + path ); }
    FD fd( openat( parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW ) );
    auto r = record( Record::DIRECTORY ); r.set_path( path ); emit( r ); expect( Record::SAVED );
    DIR* raw = fdopendir( dup( fd.value ) ); check( raw != nullptr, "read directory" );
    std::unique_ptr<DIR, decltype( &closedir )> directory( raw, closedir );
    // Stream the tree rather than building a whole-directory archive in RAM.
    while ( true ) {
      errno = 0; const auto* entry = readdir( directory.get() );
      if ( !entry ) { check( errno == 0, "read directory" ); break; }
      const std::string child = entry->d_name;
      if ( child != "." && child != ".." ) { visit( fd.value, child, path + "/" + child, depth + 1 ); }
    }
  }
  void receive_file( int root, const Record& offer )
  {
    const auto slash = offer.path().rfind( '/' );
    const auto parent_path = slash == std::string::npos ? "" : offer.path().substr( 0, slash );
    const auto name = offer.path().substr( slash == std::string::npos ? 0 : slash + 1 );
    FD parent = directory_at( root, parent_path, false );
    struct stat before = {}; bool exists = fstatat( parent.value, name.c_str(), &before, AT_SYMLINK_NOFOLLOW ) == 0;
    if ( !exists && errno != ENOENT ) { throw std::runtime_error( strerror( errno ) ); }
    File basis( nullptr, fclose );
    if ( exists ) {
      if ( !S_ISREG( before.st_mode ) ) { throw std::runtime_error( "Destination is not a regular file: " + offer.path() ); }
      basis = input_file( parent.value, name ); struct stat opened = {};
      check( fstat( fileno( basis.get() ), &opened ) == 0, "basis stat" );
      if ( !unchanged( before, opened ) ) { throw std::runtime_error( "Destination changed before signature" ); }
      notify( "Signing " + offer.path() );
      auto signature = temporary();
      size_t block = std::max<uint64_t>( 2048, ( uint64_t( before.st_size ) + ( MAX_SIGNATURE - 12 ) / 36 - 1 ) / ( ( MAX_SIGNATURE - 12 ) / 36 ) );
      size_t strong = 32; rs_magic_number magic = RS_RK_BLAKE2_SIG_MAGIC;
      rs_check( rs_sig_args( before.st_size, &magic, &block, &strong ) );
      run_job( rs_sig_begin( block, strong, magic ), basis.get(), signature.get(), MAX_SIGNATURE );
      stream( signature.get(), Record::BASIS_SIGNATURE );
    }
    auto signature_end = record( Record::SIGNATURE_END ); signature_end.set_delta( exists ); emit( signature_end );
    notify( "Receiving " + offer.path() );
    auto payload = temporary(); uint64_t received = 0; Record end;
    while ( true ) {
      const auto r = receive();
      if ( r.serial() != sequence ) { throw std::runtime_error( "File data sequence mismatch" ); }
      if ( r.kind() == Record::END ) { end = r; break; }
      if ( r.kind() != Record::DATA || r.data().empty() || r.data().size() > offer.size() * 2 + 4096 - received ) {
        throw std::runtime_error( "Invalid or oversized file data" );
      }
      check( fwrite( r.data().data(), 1, r.data().size(), payload.get() ) == r.data().size(), "write received data" );
      const uint64_t previous = received;
      received += r.data().size();
      if ( received / 65536 != previous / 65536 ) {
        notify( "Receiving " + offer.path() + ": " + std::to_string( received ) + " payload bytes" );
      }
    }
    auto patched = temporary(); FILE* result = payload.get();
    if ( end.delta() ) {
      if ( !basis ) { throw std::runtime_error( "Delta has no basis file" ); }
      notify( "Applying delta: " + offer.path() );
      run_job( rs_patch_begin( rs_file_copy_cb, basis.get() ), payload.get(), patched.get(), offer.size() );
      result = patched.get();
    }
    if ( end.sha256().size() != 32 || file_size( result ) != offer.size() || digest.hash( result ) != end.sha256() ) {
      throw std::runtime_error( "File integrity check failed; destination left unchanged" );
    }
    Publish destination( parent.value, sequence );
    rewind_file( result ); char buffer[CHUNK];
    while ( const size_t n = fread( buffer, 1, sizeof buffer, result ) ) {
      check( fwrite( buffer, 1, n, destination.output.get() ) == n, "write destination" );
    }
    check( !ferror( result ) && fflush( destination.output.get() ) == 0, "flush destination" );
    check( fchmod( fileno( destination.output.get() ), offer.mode() & 0777 ) == 0, "destination permissions" );
    check( fsync( fileno( destination.output.get() ) ) == 0, "sync destination" );
    if ( exists ) {
      struct stat current = {};
      check( fstatat( parent.value, name.c_str(), &current, AT_SYMLINK_NOFOLLOW ) == 0, "destination recheck" );
      if ( !unchanged( before, current ) ) { throw std::runtime_error( "Destination changed during transfer; not overwritten" ); }
      check( renameat( parent.value, destination.name.c_str(), parent.value, name.c_str() ) == 0, "publish synchronized file" );
    } else {
      check( linkat( parent.value, destination.name.c_str(), parent.value, name.c_str(), 0 ) == 0, "publish new file without overwrite" );
    }
    emit( record( Record::SAVED ) ); ++files; bytes += offer.size(); notify( "Saved " + offer.path() );
  }
public:
  Runner( int fd, const Command& request, Crypto::Mode mode ) : socket( fd ), command( request ), digest( mode ) {}
  void run()
  {
    try {
      if ( command.send() ) {
        const auto slash = command.path().rfind( '/' );
        const auto name = command.path().substr( slash + 1 );
        if ( name != command.name() ) { throw std::runtime_error( "Source name changed" ); }
        FD parent = absolute_directory( slash ? command.path().substr( 0, slash ) : "/" );
        visit( parent.value, name, name, 0 );
        emit( record( Record::FINISH ) ); expect( Record::FINISHED );
      } else {
        FD root = absolute_directory( command.path() );
        while ( true ) {
          const auto r = receive();
          if ( r.kind() == Record::FINISH && sequence && r.serial() == sequence ) { emit( record( Record::FINISHED ) ); break; }
          if ( r.serial() != sequence + 1 || !relative( r.path() ) || ++visited > 1000000
               || ( r.path() != command.name() && r.path().compare( 0, command.name().size() + 1, command.name() + "/" ) != 0 ) ) {
            throw std::runtime_error( "File path or sequence outside selected transfer" );
          }
          sequence = r.serial();
          if ( r.kind() == Record::DIRECTORY ) {
            directory_at( root.value, r.path(), true ); emit( record( Record::SAVED ) );
          } else if ( r.kind() == Record::OFFER && r.has_size() ) { receive_file( root.value, r ); }
          else { throw std::runtime_error( "Unexpected file offer" ); }
        }
      }
      notify( "Complete", true );
    } catch ( const std::exception& error ) { notify( error.what(), true, true ); }
  }
};

void worker( int fd, const Command& command, Crypto::Mode mode )
{
  const int limit = getdtablesize();
  for ( int i = 0; i < limit; ++i ) { if ( i != fd ) { close( i ); } }
  sigset_t mask; sigemptyset( &mask ); sigprocmask( SIG_SETMASK, &mask, nullptr );
  struct sigaction action = {}; sigemptyset( &action.sa_mask ); action.sa_handler = terminate_worker;
  for ( int signal : { SIGTERM, SIGINT, SIGHUP } ) { sigaction( signal, &action, nullptr ); }
  action.sa_handler = SIG_IGN; sigaction( SIGPIPE, &action, nullptr );
  try { Runner( fd, command, mode ).run(); } catch ( ... ) { _exit( 1 ); }
  _exit( 0 );
}
#endif
}

bool available()
{
#ifdef HAVE_FILE_SYNC
  return true;
#else
  return false;
#endif
}

bool valid_command( const Command& c )
{
  if ( !c.IsInitialized() || !c.id() || c.ByteSizeLong() > 4096 || c.status().size() > 512 ) { return false; }
  return c.kind() != Command::START || ( c.has_send() && absolute( c.path() ) && basename( c.name() ) );
}

Endpoint::~Endpoint()
{
  stop();
  if ( child > 0 ) { waitpid( child, nullptr, WNOHANG ); }
}

void Endpoint::stop()
{
  if ( socket >= 0 ) { close( socket ); socket = -1; }
  if ( child > 0 ) { kill( child, SIGTERM ); kill( child, SIGCONT ); }
  incoming.clear(); outgoing.clear();
}

bool Endpoint::spawn( uint64_t now )
{
#ifdef HAVE_FILE_SYNC
  int pair[2];
  if ( socketpair( AF_UNIX, SOCK_STREAM, 0, pair ) < 0 ) { return false; }
  bool usable = fcntl( pair[0], F_SETFL, O_NONBLOCK ) == 0;
  for ( const int fd : pair ) {
    usable = ( fcntl( fd, F_SETFD, FD_CLOEXEC ) == 0 ) && usable;
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    usable = ( setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes ) == 0 ) && usable;
#endif
  }
  if ( !usable ) { close( pair[0] ); close( pair[1] ); return false; }
  child = fork();
  if ( child == 0 ) { close( pair[0] ); worker( pair[1], current, mode ); }
  close( pair[1] );
  if ( child < 0 ) { close( pair[0] ); return false; }
  socket = pair[0];
  return true;
#else
  (void)mode;
  return false;
#endif
}

void Endpoint::remember( const Command& value )
{
  for ( auto& entry : history ) {
    if ( entry.id() == value.id() ) { entry = value; return; }
  }
  history.push_back( value );
  if ( history.size() > 16 ) { history.pop_front(); }
}

void Endpoint::fail( const std::string& text )
{
  if ( !current.id() ) { return; }
  stop(); completed = true;
  channel.discard_queued( []( const std::string& ) { return true; } );
  status.set_status( text.substr( 0, 512 ) ); status.set_done( true ); status.set_failed( true ); remember( status );
  if ( server ) { pending_status = status; status_pending = true; next_status = 0; }
  else {
    Command cancel; cancel.set_kind( Command::CANCEL ); cancel.set_id( current.id() );
    if ( controls.size() < 16 ) { controls.push_back( cancel ); }
  }
}

bool Endpoint::queue( bool upload, const std::string& source, const std::string& destination, const std::string& name )
{
  if ( server || !enabled || queued.size() >= 8 || serial == UINT64_MAX ) { return false; }
  Job job;
  job.local.set_kind( Command::START ); job.local.set_id( ++serial ); job.local.set_name( name );
  job.local.set_send( upload ); job.local.set_path( upload ? source : destination );
  job.remote = job.local; job.remote.set_send( !upload ); job.remote.set_path( upload ? destination : source );
  if ( !valid_command( job.local ) || !valid_command( job.remote ) ) { return false; }
  auto value = outcome( serial, "Queued: " + name ); value.set_name( name ); value.set_send( upload );
  remember( value ); queued.push_back( job ); return true;
}

void Endpoint::cancel()
{
  for ( const auto& job : queued ) {
    auto value = outcome( job.local.id(), "Canceled: " + job.local.name(), true, true );
    value.set_name( job.local.name() ); remember( value );
  }
  queued.clear();
  if ( current.id() && !completed ) { fail( "Canceled; completed files kept" ); }
}

void Endpoint::receive_control( const Command& command, uint64_t now )
{
  if ( !enabled || !valid_command( command ) ) { return; }
  if ( command.kind() == Command::START ) {
    // Only the local UI can authorize local writes. A server can never initiate
    // a transfer on its client, even inside an authenticated session.
    if ( !server || command.id() <= last_remote ) { return; }
    last_remote = command.id();
    if ( ( current.id() && !completed ) || !queued.empty() ) {
      if ( controls.size() < 16 ) { controls.push_back( outcome( command.id(), "File worker busy", true, true ) ); }
      return;
    }
    queued.push_back( { command, Command() } );
  } else if ( command.id() == current.id() ) {
    if ( command.kind() == Command::CANCEL ) { if ( !completed ) { fail( "Canceled by peer; completed files kept" ); } }
    else if ( !server && command.kind() == Command::STATUS ) {
      if ( command.failed() ) { fail( command.status() ); }
      else if ( command.status() == "ready" ) { peer_ready = true; }
      else if ( !completed && !command.done() ) {
        status.set_status( "Remote: " + command.status() ); remember( status );
      }
      // Local worker progress is authoritative; a peer's status never marks a
      // local file saved before its integrity check and atomic publication.
    }
  } else if ( command.kind() == Command::CANCEL && server ) {
    queued.erase( std::remove_if( queued.begin(), queued.end(), [&]( const Job& j ) { return j.local.id() == command.id(); } ), queued.end() );
  }
  (void)now;
}

void Endpoint::progress( const Record& r )
{
  status.set_status( r.status() ); status.set_files( r.files() ); status.set_bytes( r.bytes() );
  status.set_done( r.done() ); status.set_failed( r.failed() ); remember( status );
  if ( server ) { pending_status = status; status_pending = true; }
  if ( r.failed() ) { fail( r.status() ); }
  else if ( r.done() ) { completed = true; }
}

void Endpoint::count( const Record& r )
{
  if ( r.kind() == Record::BASIS_SIGNATURE ) { status.set_signatures( status.signatures() + r.data().size() ); }
  if ( r.kind() == Record::DATA ) {
    status.set_payload( status.payload() + r.data().size() );
    if ( r.delta() ) { status.set_deltas( status.deltas() + r.data().size() ); }
  }
  remember( status );
}

void Endpoint::tick( uint64_t now )
{
  if ( !enabled ) { return; }
  try {
    // Clear only after outgoing records are acknowledged. Cancel discards
    // unsent records, but in-flight sequence numbers must drain without holes.
    if ( completed && child < 0 && socket < 0 && channel.idle() && controls.empty() && !status_pending ) {
      current.Clear(); completed = false; peer_ready = false;
    }
    if ( !current.id() && child < 0 && !queued.empty() ) {
      const Job job = queued.front(); queued.pop_front(); current = job.local;
      status = outcome( current.id(), "Starting: " + current.name() );
      status.set_name( current.name() ); status.set_send( current.send() ); remember( status );
      peer_ready = server;
      if ( !spawn( now ) ) { fail( "Cannot start file worker" ); }
      else if ( server ) { controls.push_back( outcome( current.id(), "ready" ) ); }
      else { controls.push_back( job.remote ); }
    }

    if ( socket >= 0 && !outgoing.empty() ) {
      const ssize_t n = send_bytes( socket, outgoing );
      if ( n > 0 ) { outgoing.erase( 0, n ); }
      else if ( n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK ) {
        throw std::runtime_error( "File worker disconnected" );
      }
    }
    // Consume at most four records per turn. Taking a record releases its FEC
    // window; wait for IPC room before doing so, propagating disk backpressure.
    for ( unsigned i = 0; i < 4 && outgoing.empty(); ++i ) {
      std::string body;
      if ( !channel.take_output( body ) ) { break; }
      Record r;
      if ( !decode( body, r ) || r.kind() == Record::PROGRESS ) { throw std::runtime_error( "Invalid peer file record" ); }
      if ( !current.id() || r.job() != current.id() || completed || socket < 0 ) { continue; }
      count( r );
      outgoing = frame( r );
    }

    for ( unsigned i = 0; i < 4 && socket >= 0 && channel.pending_bytes() < QUEUE_BYTES; ++i ) {
      Record r;
      if ( !unframe( incoming, r ) ) {
        char buffer[MAX_RECORD]; const ssize_t n = read( socket, buffer, sizeof buffer );
        if ( n > 0 ) { incoming.append( buffer, n ); continue; }
        if ( n < 0 && ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) ) { break; }
        close( socket ); socket = -1;
        if ( !completed ) { throw std::runtime_error( "File worker exited without completion" ); }
        break;
      }
      if ( r.job() != current.id() ) { throw std::runtime_error( "File worker job mismatch" ); }
      if ( r.kind() == Record::PROGRESS ) { progress( r ); }
      else {
        count( r );
        if ( !channel.enqueue( { r.SerializeAsString() }, Clipboard::Priority::Background ) ) {
          throw std::runtime_error( "File FEC queue full" );
        }
      }
    }
  } catch ( const std::exception& error ) { fail( error.what() ); }

  if ( child > 0 ) {
    const pid_t result = waitpid( child, nullptr, WNOHANG );
    if ( result == child || ( result < 0 && errno == ECHILD ) ) { child = -1; }
  }
  if ( server && status_pending && ( pending_status.done() || now >= next_status ) && controls.size() < 16 ) {
    controls.push_back( pending_status ); status_pending = false; next_status = now + 2000;
  }
}

bool Endpoint::take_control( Command& command )
{
  if ( controls.empty() ) { return false; }
  command = std::move( controls.front() ); controls.pop_front(); return true;
}

void Endpoint::flush_controls( Control::Channel& destination )
{
  if ( controls.empty() ) { return; }
  Control::Message message; message.set_kind( Control::Message::FILE_CONTROL );
  message.set_request( controls.front().id() ); *message.mutable_file() = controls.front();
  if ( destination.queue( message ) ) { controls.pop_front(); }
}

int Endpoint::fd() const
{
  return channel.pending_bytes() < QUEUE_BYTES ? socket : -1;
}

int Endpoint::wait_time( uint64_t now, bool background ) const
{
  if ( !enabled ) { return INT_MAX; }
  // A completed receive record may be waiting on a blocked filesystem worker.
  // Do not let the generic channel's ready-output hint busy-spin the session.
  int delay = channel.wait_time( now, background && peer_ready );
  if ( delay == 0 && !outgoing.empty() && !channel.has_interactive() ) { delay = 20; }
  if ( !incoming.empty() && channel.pending_bytes() < QUEUE_BYTES ) { delay = std::min( delay, 20 ); }
  if ( current.id() || !queued.empty() ) { delay = std::min( delay, outgoing.empty() ? 250 : 20 ); }
  return delay;
}

bool Endpoint::take_packet( Clipboard::Priority priority, uint64_t now, unsigned rtt, Network::Bulk::Datagram& packet )
{
  return enabled && ( priority == Clipboard::Priority::Interactive || peer_ready || completed )
         && channel.take_packet( priority, now, rtt, packet );
}
}
