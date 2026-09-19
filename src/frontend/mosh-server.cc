/*
    Modified for Goblin Skiff on 2026-09-19.
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

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

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "src/include/config.h"
#include "src/include/version.h"

#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <getopt.h>
#include <sstream>
#include <stdexcept>
#include <typeinfo>
#include <vector>

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <pwd.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#ifdef HAVE_UTEMPTER
#include <utempter.h>
#endif
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#ifdef HAVE_UTMPX_H
#include <utmpx.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#endif

#if FORKPTY_IN_LIBUTIL
#include <libutil.h>
#endif

#include "src/statesync/clipboard.h"
#include "src/statesync/completeterminal.h"
#include "src/statesync/user.h"
#include "src/util/fatal_assert.h"
#include "src/util/locale_utils.h"
#include "src/util/pty_compat.h"
#include "src/util/select.h"
#include "src/util/swrite.h"
#include "src/util/timestamp.h"

#ifndef _PATH_BSHELL
#define _PATH_BSHELL "/bin/sh"
#endif

#include "src/network/networktransport-impl.h"
#include "src/network/bulkcontrol.h"
#include "src/network/compressor.h"
#include "streamforward.h"
#include "directory.h"
#include "mimeclipboard.h"
#include "download.h"
#include "filetransfer.h"
#include "relay-server.h"

using ServerConnection = Network::Transport<Terminal::Complete, Network::UserStream>;

static void serve( int host_fd,
                   int pipe_fd,
                   Terminal::Complete& terminal,
                   ServerConnection& network,
                   StreamForwarder& forwarder,
                   Network::Bulk::ControlServer& bulk_control,
                   long network_timeout,
                   long network_signaled_timeout,
                   bool tmux_control, Crypto::Mode crypto_mode );

static int run_server( const char* desired_ip,
                       const char* desired_port,
                       const std::string& command_path,
                       char* command_argv[],
                       const int colors,
                       const std::string& client_term,
                       unsigned int verbose,
                       bool with_motd,
                       const std::vector<std::string>& remote_forwards,
                       bool agent_forwarding,
                       bool x11_forwarding,
                       unsigned int stream_delay_ms,
                       unsigned int stream_rate_bytes_per_second,
                       const std::string& state_zstd_dictionary,
                       bool unlink_state_zstd_dictionary,
                       bool compact_keepalive,
                       Crypto::Mode crypto_mode,
                       bool tmux_control,
                       const Terminal::ImageEncoding& image_encoding );

static void print_version( FILE* file )
{
  fputs( "goblin-skiff-server " GOBLIN_VERSION " (" PACKAGE_STRING ") [build " BUILD_VERSION "]\n"
         "Copyright 2012 Keith Winstein <mosh-devel@mit.edu>\n"
         "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n",
         file );
}

static void print_usage( FILE* stream, const char* argv0 )
{
  fprintf( stream,
           "Usage: %s new [-s] [-v] [-i LOCALADDR] [-p PORT[:PORT2]] [-c COLORS] [-l NAME=VALUE] [-A] [-X] [-R SPEC] [-t MS] [-b BPS] [--fips-crypto] [-- COMMAND...]\n",
           argv0 );
  fputs( "       goblin-skiff-server relay --help  (authenticated UDP jump relay)\n", stream );
  fputs( "       --lossy=QUALITY  WebP quality 0-100 (higher is better; default lossless)\n"
         "       --djvu-lossy     allow cjb2 symbol substitution for two-color images\n", stream );
}

static bool print_motd( const char* filename );
static void chdir_homedir( void );
static bool motd_hushed( void );
static void warn_unattached( const std::string& ignore_entry );

static unsigned int parse_uint_option( const char* name, const char* value )
{
  char* end = NULL;
  errno = 0;
  unsigned long parsed = strtoul( value, &end, 10 );
  if ( errno || *end || parsed > UINT_MAX ) {
    fprintf( stderr, "Bad %s (%s)\n", name, value );
    exit( 1 );
  }
  return parsed;
}

static unsigned int uint_from_env( const char* name, unsigned int fallback )
{
  const char* value = getenv( name );
  if ( !value || !*value ) {
    return fallback;
  }
  return parse_uint_option( name, value );
}

static bool bool_from_env( const char* name, bool fallback )
{
  const char* value = getenv( name );
  if ( !value || !*value ) {
    return fallback;
  }
  if ( 0 == strcmp( value, "1" ) || 0 == strcmp( value, "yes" ) || 0 == strcmp( value, "true" ) ) {
    return true;
  }
  if ( 0 == strcmp( value, "0" ) || 0 == strcmp( value, "no" ) || 0 == strcmp( value, "false" ) ) {
    return false;
  }
  fprintf( stderr, "Bad %s (%s)\n", name, value );
  exit( 1 );
}

static bool has_capability( const char* list, const std::string& capability )
{
  if ( !list ) {
    return false;
  }

  std::string normalized( list );
  for ( std::string::iterator it = normalized.begin(); it != normalized.end(); it++ ) {
    if ( *it == ',' ) {
      *it = ' ';
    }
  }

  std::istringstream capabilities( normalized );
  std::string candidate;
  while ( capabilities >> candidate ) {
    if ( candidate == capability ) {
      return true;
    }
  }
  return false;
}

static Terminal::ClientGeometry sanitize_client_geometry( const Terminal::ClientGeometry& geometry )
{
  static const uint32_t MAX_GEOMETRY_VALUE = 65535;
  Terminal::ClientGeometry result = geometry;

  if ( !result.has_grid() || result.columns > MAX_GEOMETRY_VALUE || result.rows > MAX_GEOMETRY_VALUE ) {
    return Terminal::ClientGeometry();
  }
  if ( !result.has_pixel_size() || result.width_px > MAX_GEOMETRY_VALUE
       || result.height_px > MAX_GEOMETRY_VALUE ) {
    result.width_px = 0;
    result.height_px = 0;
  }
  if ( !result.has_cell_size() || result.cell_width_px > MAX_GEOMETRY_VALUE
       || result.cell_height_px > MAX_GEOMETRY_VALUE ) {
    result.cell_width_px = result.width_px / result.columns;
    result.cell_height_px = result.height_px / result.rows;
  }

  return result;
}

/* Simple spinloop */
static void spin( void )
{
  static unsigned int spincount = 0;
  spincount++;

  if ( spincount > 10 ) {
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = 100000000; /* 0.1 sec */
    nanosleep( &req, NULL );
    freeze_timestamp();
  }
}

static std::string get_SSH_IP( void )
{
  const char* SSH_CONNECTION = getenv( "SSH_CONNECTION" );
  if ( !SSH_CONNECTION ) { /* Older sshds don't set this */
    fputs( "Warning: SSH_CONNECTION not found; binding to any interface.\n", stderr );
    return std::string( "" );
  }
  std::istringstream ss( SSH_CONNECTION );
  std::string dummy, local_interface_IP;
  ss >> dummy >> dummy >> local_interface_IP;
  if ( !ss ) {
    fputs( "Warning: Could not parse SSH_CONNECTION; binding to any interface.\n", stderr );
    return std::string( "" );
  }

  /* Strip IPv6 prefix. */
  const char IPv6_prefix[] = "::ffff:";

  if ( ( local_interface_IP.length() > strlen( IPv6_prefix ) )
       && ( 0 == strncasecmp( local_interface_IP.c_str(), IPv6_prefix, strlen( IPv6_prefix ) ) ) ) {
    return local_interface_IP.substr( strlen( IPv6_prefix ) );
  }

  return local_interface_IP;
}

int main( int argc, char* argv[] )
{
  /* For security, make sure we don't dump core */
  Crypto::disable_dumping_core();

  if ( argc >= 2 && !strcmp( argv[1], "relay" ) ) { return relay_server_main( argc - 1, argv + 1 ); }

  /* Detect edge case */
  fatal_assert( argc > 0 );

  const char* desired_ip = NULL;
  std::string desired_ip_str;
  const char* desired_port = NULL;
  std::string command_path;
  char** command_argv = NULL;
  int colors = 0;
  const char* client_term_env = getenv( "MOSH_CLIENT_TERM" );
  std::string client_term = client_term_env ? client_term_env : "";
  unsigned int verbose = 0; /* don't close stdin/stdout/stderr */
  std::vector<std::string> remote_forwards;
  bool agent_forwarding = false;
  bool x11_forwarding = false;
  unsigned int stream_delay_ms = uint_from_env( "MOSH_STREAM_DELAY", 75 );
  unsigned int stream_rate_bytes_per_second = uint_from_env( "MOSH_STREAM_BANDWIDTH", 0 );
  const char* state_zstd_dictionary_env = getenv( "MOSH_STATE_ZSTD_DICT" );
  std::string state_zstd_dictionary = state_zstd_dictionary_env ? state_zstd_dictionary_env : "";
  bool unlink_state_zstd_dictionary = bool_from_env( "MOSH_STATE_ZSTD_DICT_UNLINK", false );
  const char* client_capabilities = getenv( "MOSH_CLIENT_CAPS" );
  Terminal::ImageEncoding image_encoding;
  image_encoding.palette_djvu = has_capability( client_capabilities, "palette-djvu-v1" );
  bool compact_keepalive = has_capability( client_capabilities, "keepalive-v1" );
  bool tmux_control = has_capability( client_capabilities, "tmux-control-v1" );
  bool client_supports_fips_crypto = has_capability( client_capabilities, "fips-aes128-gcm-v1" );
  Crypto::Mode crypto_mode = Crypto::Mode::LegacyOCB;
  /* Will cause goblin-skiff-server not to correctly detach on old versions of sshd. */
  std::list<std::string> locale_vars;

  /* strip off command */
  for ( int i = 1; i < argc; i++ ) {
    if ( 0 == strcmp( argv[i], "--help" ) || 0 == strcmp( argv[i], "-h" ) ) {
      print_usage( stdout, argv[0] );
      exit( 0 );
    }
    if ( 0 == strcmp( argv[i], "--version" ) ) {
      print_version( stdout );
      exit( 0 );
    }
    if ( 0 == strcmp( argv[i], "--" ) ) { /* -- is mandatory */
      if ( i != argc - 1 ) {
        command_argv = argv + i + 1;
      }
      argc = i; /* rest of options before -- */
      break;
    }
  }

  /* Parse new command-line syntax */
  if ( ( argc >= 2 ) && ( strcmp( argv[1], "new" ) == 0 ) ) {
    /* new option syntax */
    int opt;
    static const struct option long_options[] = {
      { "fips-crypto", no_argument, NULL, 256 },
      { "lossy", required_argument, NULL, 257 },
      { "djvu-lossy", no_argument, NULL, 258 },
      { 0, 0, 0, 0 },
    };
    while ( ( opt = getopt_long( argc - 1, argv + 1, "@:i:p:c:svl:R:AXt:b:", long_options, NULL ) ) != -1 ) {
      switch ( opt ) {
          /*
           * This undocumented option does nothing but eat its argument.
           * Useful in scripting where you prepend something to a
           * goblin-skiff-server argv, and might end up with something like
           * "goblin-skiff-server new -v new -c 256", now you can say
           * "goblin-skiff-server new -v -@ new -c 256" to discard the second
           * "new".
           */
        case '@':
          break;
        case 'i':
          desired_ip = optarg;
          break;
        case 'p':
          desired_port = optarg;
          break;
        case 's':
          desired_ip = NULL;
          desired_ip_str = get_SSH_IP();
          if ( !desired_ip_str.empty() ) {
            desired_ip = desired_ip_str.c_str();
            fatal_assert( desired_ip );
          }
          break;
        case 'c':
          try {
            colors = myatoi( optarg );
          } catch ( const CryptoException& ) {
            fprintf( stderr, "%s: Bad number of colors (%s)\n", argv[0], optarg );
            print_usage( stderr, argv[0] );
            exit( 1 );
          }
          break;
        case 'v':
          verbose++;
          break;
        case 'l':
          locale_vars.push_back( std::string( optarg ) );
          break;
        case 'R':
          remote_forwards.push_back( std::string( optarg ) );
          break;
        case 'A':
          agent_forwarding = true;
          break;
        case 'X':
          x11_forwarding = true;
          break;
        case 't':
          stream_delay_ms = parse_uint_option( "-t", optarg );
          break;
        case 'b':
          stream_rate_bytes_per_second = parse_uint_option( "-b", optarg );
          if ( stream_rate_bytes_per_second == 0 ) {
            fputs( "-b stream bandwidth must be greater than zero\n", stderr );
            exit( 1 );
          }
          break;
        case 256:
          crypto_mode = Crypto::Mode::FipsAES128GCM;
          break;
        case 257:
          if ( !*optarg || strlen( optarg ) > 3 || strspn( optarg, "0123456789" ) != strlen( optarg )
               || parse_uint_option( "--lossy", optarg ) > 100 ) {
            fputs( "--lossy requires an integer quality from 0 to 100\n", stderr );
            return 1;
          }
          image_encoding.quality = parse_uint_option( "--lossy", optarg );
          break;
        case 258:
          image_encoding.djvu_lossy = true;
          break;
        case '?':
          print_usage( stderr, argv[0] );
          return 1;
        default:
          /* don't die on unknown options */
          print_usage( stderr, argv[0] );
          break;
      }
    }
  } else if ( argc == 1 ) {
    /* legacy argument parsing for older client wrapper script */
    /* do nothing */
  } else if ( argc == 2 ) {
    desired_ip = argv[1];
  } else if ( argc == 3 ) {
    desired_ip = argv[1];
    desired_port = argv[2];
  } else {
    print_usage( stderr, argv[0] );
    exit( 1 );
  }

  /* Sanity-check arguments */
  int dpl, dph;
  if ( desired_port && !Connection::parse_portrange( desired_port, dpl, dph ) ) {
    fprintf( stderr, "%s: Bad UDP port range (%s)\n", argv[0], desired_port );
    print_usage( stderr, argv[0] );
    exit( 1 );
  }

  if ( crypto_mode == Crypto::Mode::FipsAES128GCM && client_capabilities && !client_supports_fips_crypto ) {
    fputs( "--fips-crypto requested without a matching client capability advertisement\n", stderr );
    exit( 1 );
  }

  bool with_motd = false;

#ifdef HAVE_SYSLOG
  openlog( argv[0], LOG_PID | LOG_NDELAY, LOG_AUTH );
#endif

  /* Get shell */
  char* my_argv[2];
  std::string shell_name;
  if ( !command_argv ) {
    /* get shell name */
    const char* shell = getenv( "SHELL" );
    if ( shell == NULL ) {
      struct passwd* pw = getpwuid( getuid() );
      if ( pw == NULL ) {
        perror( "getpwuid" );
        exit( 1 );
      }
      shell = pw->pw_shell;
    }

    std::string shell_path( shell );
    if ( shell_path.empty() ) { /* empty shell means Bourne shell */
      shell_path = _PATH_BSHELL;
    }

    command_path = shell_path;

    size_t shell_slash( shell_path.rfind( '/' ) );
    if ( shell_slash == std::string::npos ) {
      shell_name = shell_path;
    } else {
      shell_name = shell_path.substr( shell_slash + 1 );
    }

    /* prepend '-' to make login shell */
    shell_name = '-' + shell_name;

    my_argv[0] = const_cast<char*>( shell_name.c_str() );
    my_argv[1] = NULL;
    command_argv = my_argv;

    with_motd = true;
  }

  if ( command_path.empty() ) {
    command_path = command_argv[0];
  }

  /* Adopt implementation locale */
  set_native_locale();
  if ( !is_utf8_locale() ) {
    /* save details for diagnostic */
    LocaleVar native_ctype = get_ctype();
    std::string native_charset( locale_charset() );

    /* apply locale-related environment variables from client */
    clear_locale_variables();
    for ( std::list<std::string>::const_iterator i = locale_vars.begin(); i != locale_vars.end(); i++ ) {
      char* env_string = strdup( i->c_str() );
      fatal_assert( env_string );
      if ( 0 != putenv( env_string ) ) {
        perror( "putenv" );
      }
    }

    /* check again */
    set_native_locale();
    if ( !is_utf8_locale() ) {
      LocaleVar client_ctype = get_ctype();
      std::string client_charset( locale_charset() );

      fprintf( stderr,
               "goblin-skiff-server needs a UTF-8 native locale to run.\n\n"
               "Unfortunately, the local environment (%s) specifies\n"
               "the character set \"%s\",\n\n"
               "The client-supplied environment (%s) specifies\n"
               "the character set \"%s\".\n\n",
               native_ctype.str().c_str(),
               native_charset.c_str(),
               client_ctype.str().c_str(),
               client_charset.c_str() );
      int unused __attribute( ( unused ) ) = system( "locale" );
      exit( 1 );
    }
  }

  try {
    return run_server( desired_ip,
                       desired_port,
                       command_path,
                       command_argv,
                       colors,
                       client_term,
                       verbose,
                       with_motd,
                       remote_forwards,
                       agent_forwarding,
                       x11_forwarding,
                       stream_delay_ms,
                       stream_rate_bytes_per_second,
                       state_zstd_dictionary,
                       unlink_state_zstd_dictionary,
                       compact_keepalive,
                       crypto_mode,
                       tmux_control,
                       image_encoding );
  } catch ( const Network::NetworkException& e ) {
    fprintf( stderr, "Network exception: %s\n", e.what() );
    return 1;
  } catch ( const Crypto::CryptoException& e ) {
    fprintf( stderr, "Crypto exception: %s\n", e.what() );
    return 1;
  } catch ( const std::exception& e ) {
    fprintf( stderr, "Error: %s\n", e.what() );
    return 1;
  }
}

static int run_server( const char* desired_ip,
                       const char* desired_port,
                       const std::string& command_path,
                       char* command_argv[],
                       const int colors,
                       const std::string& client_term,
                       unsigned int verbose,
                       bool with_motd,
                       const std::vector<std::string>& remote_forwards,
                       bool agent_forwarding,
                       bool x11_forwarding,
                       unsigned int stream_delay_ms,
                       unsigned int stream_rate_bytes_per_second,
                       const std::string& state_zstd_dictionary,
                       bool unlink_state_zstd_dictionary,
                       bool compact_keepalive,
                       Crypto::Mode crypto_mode,
                       bool tmux_control,
                       const Terminal::ImageEncoding& image_encoding )
{
  Crypto::ensure_mode_available( crypto_mode );
  if ( image_encoding.djvu_lossy && !image_encoding.palette_djvu ) {
    throw std::runtime_error( "--djvu-lossy requires client capability palette-djvu-v1" );
  }

  if ( !state_zstd_dictionary.empty() ) {
    Network::get_compressor().set_zstd_dictionary_from_file( state_zstd_dictionary );
    if ( unlink_state_zstd_dictionary && unlink( state_zstd_dictionary.c_str() ) < 0 ) {
      fprintf( stderr, "Warning: could not remove uploaded zstd dictionary %s: %s\n",
               state_zstd_dictionary.c_str(),
               strerror( errno ) );
    }
  }

  /* get network idle timeout */
  long network_timeout = 0;
  char* timeout_envar = getenv( "MOSH_SERVER_NETWORK_TMOUT" );
  if ( timeout_envar && *timeout_envar ) {
    errno = 0;
    char* endptr;
    network_timeout = strtol( timeout_envar, &endptr, 10 );
    if ( *endptr != '\0' || ( network_timeout == 0 && errno == EINVAL ) ) {
      fputs( "MOSH_SERVER_NETWORK_TMOUT not a valid integer, ignoring\n", stderr );
    } else if ( network_timeout < 0 ) {
      fputs( "MOSH_SERVER_NETWORK_TMOUT is negative, ignoring\n", stderr );
      network_timeout = 0;
    }
  }
  /* get network signaled idle timeout */
  long network_signaled_timeout = 0;
  char* signal_envar = getenv( "MOSH_SERVER_SIGNAL_TMOUT" );
  if ( signal_envar && *signal_envar ) {
    errno = 0;
    char* endptr;
    network_signaled_timeout = strtol( signal_envar, &endptr, 10 );
    if ( *endptr != '\0' || ( network_signaled_timeout == 0 && errno == EINVAL ) ) {
      fputs( "MOSH_SERVER_SIGNAL_TMOUT not a valid integer, ignoring\n", stderr );
    } else if ( network_signaled_timeout < 0 ) {
      fputs( "MOSH_SERVER_SIGNAL_TMOUT is negative, ignoring\n", stderr );
      network_signaled_timeout = 0;
    }
  }
  /* get initial window size */
  struct winsize window_size;
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 || window_size.ws_col == 0 || window_size.ws_row == 0 ) {
    /* Fill in sensible defaults. */
    /* They will be overwritten by client on first connection. */
    memset( &window_size, 0, sizeof( window_size ) );
    window_size.ws_col = 80;
    window_size.ws_row = 24;
  }

  /* open parser and terminal */
  Terminal::Complete terminal( window_size.ws_col, window_size.ws_row );
  terminal.set_image_encoding( image_encoding );

  /* open network */
  Network::UserStream blank;
  using NetworkPointer = std::shared_ptr<ServerConnection>;
  NetworkPointer network(
    new ServerConnection( terminal, blank, desired_ip, desired_port, compact_keepalive, crypto_mode ) );
  const unsigned relay_hops = uint_from_env( "MOSH_RELAY_HOPS", 0 );
  network->set_relay_hops( relay_hops );
  unsetenv( "MOSH_RELAY_HOPS" ); // routing metadata is not an application environment setting
  const bool link_budget = compact_keepalive && has_capability( getenv( "MOSH_CLIENT_CAPS" ), "link-budget-v1" );
  network->enable_link_budget( link_budget );

  StreamForwarder forwarder(
    StreamForwarder::ServerSide, stream_delay_ms, stream_rate_bytes_per_second, crypto_mode );
  std::string forward_error;
  for ( std::vector<std::string>::const_iterator it = remote_forwards.begin(); it != remote_forwards.end(); it++ ) {
    if ( !forwarder.add_tcp_forward( *it, forward_error ) ) {
      fprintf( stderr, "Bad -R forwarding spec: %s\n", forward_error.c_str() );
      exit( 1 );
    }
  }
  if ( agent_forwarding && !forwarder.enable_agent_forwarding( forward_error ) ) {
    fprintf( stderr, "Cannot enable agent forwarding: %s\n", forward_error.c_str() );
    exit( 1 );
  }
  if ( x11_forwarding && !forwarder.enable_x11_forwarding( forward_error ) ) {
    fprintf( stderr, "Cannot enable X11 forwarding: %s\n", forward_error.c_str() );
    exit( 1 );
  }
  if ( !forwarder.listen( forward_error ) ) {
    fprintf( stderr, "Cannot set up stream forwarding: %s\n", forward_error.c_str() );
    exit( 1 );
  }

  network->set_verbose( verbose );
  Select::set_verbose( verbose );

  /*
   * If goblin-skiff-server is run on a pty, then typeahead may echo and break goblin-skiff's
   * detection of the MOSH CONNECT message.  Print it on a new line to bodge
   * around that.
   */
  if ( isatty( STDIN_FILENO ) ) {
    puts( "\r\n" );
  }
  if ( compact_keepalive ) {
    puts( "MOSH CAPS keepalive-v1" );
  }
  puts( Network::SessionVersion::local().bootstrap_line().c_str() );
  if ( relay_hops ) { printf( "MOSH RELAY-MTU 1 %u\n", relay_hops ); }
  if ( has_capability( getenv( "MOSH_CLIENT_CAPS" ), "sixel-state-v1" ) ) {
    puts( "MOSH GRAPHICS sixel-state-v1" );
  }
  if ( image_encoding.palette_djvu ) { puts( "MOSH IMAGE palette-djvu-v1" ); }
  else if ( image_encoding.quality >= 0 ) { puts( "MOSH IMAGE webp" ); }
  if ( has_capability( getenv( "MOSH_CLIENT_CAPS" ), "osc5522-v1" ) ) { puts( "MOSH CLIPBOARD osc5522-v1" ); }
  if ( has_capability( getenv( "MOSH_CLIENT_CAPS" ), "goblin-download-v2" ) ) { puts( "MOSH DOWNLOADS goblin-download-v2" ); }
  if ( tmux_control ) {
    puts( "MOSH TMUX control-v1" );
  }
  if ( link_budget ) { puts( "MOSH LINK budget-v1" ); }
  if ( has_capability( getenv( "MOSH_CLIENT_CAPS" ), "directory-v2" ) ) {
    puts( "MOSH DIRECTORY directory-v2" );
  } else if ( has_capability( getenv( "MOSH_CLIENT_CAPS" ), "directory-v1" ) ) {
    puts( "MOSH DIRECTORY directory-v1" );
  }
  if ( !tmux_control && Files::available() && has_capability( getenv( "MOSH_CLIENT_CAPS" ), "file-sync-v1" ) ) {
    puts( "MOSH FILES file-sync-v1" );
  }
  if ( crypto_mode == Crypto::Mode::FipsAES128GCM ) {
    printf( "MOSH CRYPTO %s\n", Crypto::mode_name( crypto_mode ) );
  }
  printf( "MOSH CONNECT %s %s\n", network->port().c_str(), network->get_key().c_str() );

  /* don't let signals kill us */
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  fatal_assert( 0 == sigfillset( &sa.sa_mask ) );
  fatal_assert( 0 == sigaction( SIGHUP, &sa, NULL ) );
  fatal_assert( 0 == sigaction( SIGPIPE, &sa, NULL ) );

  /* detach from terminal */
  fflush( NULL );
  pid_t the_pid = fork();
  if ( the_pid < 0 ) {
    perror( "fork" );
  } else if ( the_pid > 0 ) {
    fputs( "\ngoblin-skiff-server " GOBLIN_VERSION " (" PACKAGE_STRING ") [build " BUILD_VERSION "]\n"
           "Copyright 2012 Keith Winstein <mosh-devel@mit.edu>\n"
           "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
           "This is free software: you are free to change and redistribute it.\n"
           "There is NO WARRANTY, to the extent permitted by law.\n\n",
           stderr );

    fprintf( stderr, "[goblin-skiff-server detached, pid = %d]\n", static_cast<int>( the_pid ) );
#ifndef HAVE_IUTF8
    fputs( "\nWarning: termios IUTF8 flag not defined.\n"
           "Character-erase of multibyte character sequence\n"
           "probably does not work properly on this platform.\n",
           stderr );
#endif /* HAVE_IUTF8 */

    fflush( NULL );
    if ( isatty( STDOUT_FILENO ) ) {
      tcdrain( STDOUT_FILENO );
    }
    if ( isatty( STDERR_FILENO ) ) {
      tcdrain( STDERR_FILENO );
    }
    exit( 0 );
  }

  int master;

  /* close file descriptors */
  if ( verbose == 0 ) {
    /* Necessary to properly detach on old versions of sshd (e.g. RHEL/CentOS 5.0). */
    int nullfd;

    nullfd = open( "/dev/null", O_RDWR );
    if ( nullfd == -1 ) {
      perror( "open" );
      exit( 1 );
    }

    if ( dup2( nullfd, STDIN_FILENO ) < 0 || dup2( nullfd, STDOUT_FILENO ) < 0
         || dup2( nullfd, STDERR_FILENO ) < 0 ) {
      perror( "dup2" );
      exit( 1 );
    }

    if ( close( nullfd ) < 0 ) {
      perror( "close" );
      exit( 1 );
    }
  }

  Network::Bulk::ControlServer bulk_control( "server" );

  char utmp_entry[64] = { 0 };
  snprintf( utmp_entry, 64, "goblin-skiff [%ld]", static_cast<long int>( getpid() ) );

  /* Fork child process */
  int pipes[2];
  int success = pipe( pipes );
  if ( success == -1 ) {
    perror( "pipe" );
    exit( 1 );
  }
  pid_t child = forkpty( &master, NULL, NULL, &window_size );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */
    if ( close( pipes[1] ) < 0 ) {
      perror( "child write pipe close" );
      exit( 1 );
    }

    /* reenable signals */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    fatal_assert( 0 == sigfillset( &sa.sa_mask ) );
    fatal_assert( 0 == sigaction( SIGHUP, &sa, NULL ) );
    fatal_assert( 0 == sigaction( SIGPIPE, &sa, NULL ) );

#ifdef HAVE_SYSLOG
    closelog();
#endif

    /* close server-related file descriptors */
    network.reset();

    /* set IUTF8 if available */
#ifdef HAVE_IUTF8
    struct termios child_termios;
    if ( tcgetattr( STDIN_FILENO, &child_termios ) < 0 ) {
      perror( "tcgetattr" );
      exit( 1 );
    }

    child_termios.c_iflag |= IUTF8;

    if ( tcsetattr( STDIN_FILENO, TCSANOW, &child_termios ) < 0 ) {
      perror( "tcsetattr" );
      exit( 1 );
    }
#endif /* HAVE_IUTF8 */

    /* set TERM */
    const char default_term[] = "xterm";
    const char color_term[] = "xterm-256color";

    const char* term = client_term.empty() ? ( ( colors == 256 ) ? color_term : default_term ) : client_term.c_str();
    if ( setenv( "TERM", term, true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }
    if ( unsetenv( "MOSH_CLIENT_TERM" ) < 0 ) {
      perror( "unsetenv" );
      exit( 1 );
    }
    if ( unsetenv( "MOSH_CLIENT_CAPS" ) < 0 ) {
      perror( "unsetenv" );
      exit( 1 );
    }

    /* ask ncurses to send UTF-8 instead of ISO 2022 for line-drawing chars */
    if ( setenv( "NCURSES_NO_UTF8_ACS", "1", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    if ( !forwarder.agent_socket_path().empty() ) {
      if ( setenv( "SSH_AUTH_SOCK", forwarder.agent_socket_path().c_str(), true ) < 0 ) {
        perror( "setenv" );
        exit( 1 );
      }
    }

    if ( !forwarder.x11_display().empty() ) {
      if ( setenv( "DISPLAY", forwarder.x11_display().c_str(), true ) < 0 ) {
        perror( "setenv" );
        exit( 1 );
      }
    }

    if ( !forwarder.x11_authority_path().empty() ) {
      if ( setenv( "XAUTHORITY", forwarder.x11_authority_path().c_str(), true ) < 0 ) {
        perror( "setenv" );
        exit( 1 );
      }
    }

    if ( setenv( "GOBLIN_SKIFF_CP_SOCK", bulk_control.socket_path().c_str(), true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* clear STY environment variable so GNU screen regards us as top level */
    if ( unsetenv( "STY" ) < 0 ) {
      perror( "unsetenv" );
      exit( 1 );
    }

    chdir_homedir();

    if ( with_motd && ( !motd_hushed() ) ) {
      // On illumos motd is printed by /etc/profile.
#ifndef __sun
      // For Ubuntu, try and print one of {,/var}/run/motd.dynamic.
      // This file is only updated when pam_motd is run, but when
      // goblin-skiff-server is run in the usual way with ssh via the script,
      // this always happens.
      // XXX Hackish knowledge of Ubuntu PAM configuration.
      // But this seems less awful than build-time detection with autoconf.
      if ( !print_motd( "/run/motd.dynamic" ) ) {
        print_motd( "/var/run/motd.dynamic" );
      }
      // Always print traditional /etc/motd.
      print_motd( "/etc/motd" );
#endif
      warn_unattached( utmp_entry );
    }

    /* Wait for parent to release us. */
    char linebuf[81];
    // -1, errno == EINTR -- retry
    // otherwise, give up
    while ( read( pipes[0], linebuf, sizeof( linebuf ) ) == -1 ) {
      if ( errno != EINTR ) {
        err( 1, "parent signal" );
      }
    }

    Crypto::reenable_dumping_core();

    if ( execvp( command_path.c_str(), command_argv ) < 0 ) {
      warn( "execvp: %s", command_path.c_str() );
      sleep( 3 );
      exit( 1 );
    }

    if ( close( pipes[0] ) < 0 ) {
      perror( "child read pipe close" );
      exit( 1 );
    }
  } else {
    /* parent */
    if ( close( pipes[0] ) < 0 ) {
      perror( "parent read pipe close" );
      exit( 1 );
    }

    /* Drop unnecessary privileges */
#ifdef HAVE_PLEDGE
    /* OpenBSD pledge() syscall */
    if ( pledge( image_encoding.palette_djvu ? "stdio inet unix tty rpath wpath cpath proc exec"
                                             : "stdio inet unix tty rpath proc", NULL ) ) {
      perror( "pledge() failed" );
      exit( 1 );
    }
#endif

#ifdef HAVE_UTEMPTER
    /* make utmp entry */
    utempter_add_record( master, utmp_entry );
#endif

    try {
      serve( master, pipes[1], terminal, *network, forwarder, bulk_control, network_timeout, network_signaled_timeout,
             tmux_control, crypto_mode );
    } catch ( const Network::NetworkException& e ) {
      fprintf( stderr, "Network exception: %s\n", e.what() );
    } catch ( const Crypto::CryptoException& e ) {
      fprintf( stderr, "Crypto exception: %s\n", e.what() );
    }

#ifdef HAVE_UTEMPTER
    utempter_remove_record( master );
#endif

    if ( close( master ) < 0 ) {
      perror( "close" );
      exit( 1 );
    }
  }

  fputs( "\n[goblin-skiff-server is exiting.]\n", stdout );

  return 0;
}

static void serve( int host_fd,
                   int pipe_fd,
                   Terminal::Complete& terminal,
                   ServerConnection& network,
                   StreamForwarder& forwarder,
                   Network::Bulk::ControlServer& bulk_control,
                   long network_timeout,
                   long network_signaled_timeout,
                   bool tmux_control, Crypto::Mode crypto_mode )
{
  /* scale timeouts */
  const uint64_t network_timeout_ms = static_cast<uint64_t>( network_timeout ) * 1000;
  const uint64_t network_signaled_timeout_ms = static_cast<uint64_t>( network_signaled_timeout ) * 1000;
  /* prepare to poll for events */
  Select& sel = Select::get_instance();
  sel.add_signal( SIGTERM );
  sel.add_signal( SIGINT );
  sel.add_signal( SIGUSR1 );

  uint64_t last_remote_num = network.get_remote_state_num();
  Terminal::ClientGeometry client_geometry;
  Clipboard::Endpoint mime_clipboard( true );
  Download::Sender downloads;
  bool downloads_enabled = false;
  unsigned bulk_turn = 0;
  bool mime_enabled = false;
  const bool mime_negotiated = has_capability( getenv( "MOSH_CLIENT_CAPS" ), "osc5522-v1" );
  const bool sixel_state = has_capability( getenv( "MOSH_CLIENT_CAPS" ), "sixel-state-v1" );
  bool graphics_ready = !sixel_state;
  uint64_t graphics_deadline = 0;
  terminal.set_text_sizing_enabled( sixel_state );
  Terminal::TmuxControlParser tmux_parser;
  const bool directory_live = has_capability( getenv( "MOSH_CLIENT_CAPS" ), "directory-v2" );
  const bool directory_enabled = directory_live || has_capability( getenv( "MOSH_CLIENT_CAPS" ), "directory-v1" );
  Control::DirectoryWorker directory;
  Control::Channel control_channel;
  Files::Endpoint files( true, !tmux_control && has_capability( getenv( "MOSH_CLIENT_CAPS" ), "file-sync-v1" ), crypto_mode );
  Control::Message directory_reply;
  bool directory_reply_pending = false;

#ifdef HAVE_UTEMPTER
  bool connected_utmp = false;
#endif
#if defined( HAVE_SYSLOG ) || defined( HAVE_UTEMPTER )
  bool force_connection_change_evt = false;
  Addr saved_addr;
  socklen_t saved_addr_len = 0;
#endif

#ifdef HAVE_SYSLOG
  struct passwd* pw = getpwuid( getuid() );
  if ( pw == NULL ) {
    throw NetworkException( std::string( "serve: getpwuid: " ) + strerror( errno ), 0 );
  }
  syslog( LOG_INFO, "user %s session begin", pw->pw_name );
#endif

  bool child_released = false;
  uint64_t idle_cleanup_deadline = 0;

  while ( true ) {
    try {
      const uint64_t timeout_if_no_client
        = has_capability( getenv( "MOSH_CLIENT_CAPS" ), "udp-relay-v1" )
            ? uint64_t( Network::Relay::STARTUP_TIMEOUT ) * 1000 : 60000;
      int timeout = INT_MAX;
      uint64_t now = Network::timestamp();
      if ( idle_cleanup_deadline ) {
        if ( now >= idle_cleanup_deadline ) { break; }
        timeout = std::min( timeout, int( idle_cleanup_deadline - now ) );
      }
      forwarder.adapt_link_budget( network.link_budget().active() ? network.link_budget().budget() : 0 );
      files.channel.set_external_pacing( network.link_budget().active() );
      mime_clipboard.expire( now );
      downloads.tick( now );
      files.tick( now ); files.flush_controls( control_channel );
      if ( !child_released && network.get_remote_state_num() && !graphics_ready ) {
        // A new wrapper can be used with --client pointing at an older
        // binary. Do not strand login if that binary lacks attachment caps.
        if ( !graphics_deadline ) { graphics_deadline = now + 5000; }
        if ( now >= graphics_deadline ) {
          terminal.set_text_sizing_enabled( false );
          if ( close( pipe_fd ) < 0 ) { err( 1, "child release" ); }
          child_released = true;
        } else { timeout = std::min( timeout, int( graphics_deadline - now ) ); }
      }
      if ( !directory_reply_pending && !control_channel.queued_bytes() ) {
        directory.tick( now );
        if ( directory.pop( directory_reply ) ) { directory_reply_pending = true; }
      }
      if ( directory_reply_pending && control_channel.queue( directory_reply ) ) { directory_reply_pending = false; }
      const bool reliable_data_pending = network.has_unsent_data();

      timeout = std::min( timeout, network.wait_time() );
      timeout = std::min( timeout, std::max( network.bulk_wait_time(), files.wait_time( now, !forwarder.has_pending_network_data() ) ) );
      timeout = std::min( timeout, std::max( network.bulk_wait_time(), downloads.wait_time( now, !network.has_unsent_data() && !forwarder.has_pending_network_data() ) ) );
      if ( mime_enabled ) { timeout = std::min( timeout, std::max( network.bulk_wait_time(), mime_clipboard.channel.wait_time( now, !network.has_unsent_data() && !forwarder.has_pending_network_data() ) ) ); }
      timeout = std::min( timeout, terminal.wait_time( now ) );
      timeout = std::min( timeout, directory.wait_time() );
      if ( !network.shutdown_in_progress() ) {
        timeout = std::min( timeout, control_channel.wait_time( now, network.get_sent_state_acked() ) );
      }
      if ( !reliable_data_pending ) {
        timeout = std::min( timeout, forwarder.wait_time( now ) );
      }
      if ( ( !network.get_remote_state_num() ) || network.shutdown_in_progress() ) {
        timeout = std::min( timeout, 5000 );
      }
      /*
       * The server goes completely asleep if it has no remote peer.
       * We may want to wake up sooner.
       */
      if ( network_timeout_ms ) {
        int64_t network_sleep = network_timeout_ms - ( now - network.get_latest_remote_state().timestamp );
        if ( network_sleep < 0 ) {
          network_sleep = 0;
        } else if ( network_sleep > INT_MAX ) {
          /* 24 days might be too soon.  That's OK. */
          network_sleep = INT_MAX;
        }
        timeout = std::min( timeout, static_cast<int>( network_sleep ) );
      }
      if ( bulk_control.has_outgoing() && !reliable_data_pending && !forwarder.has_pending_network_data()
           && timeout > 20 ) {
        timeout = std::min( timeout, std::max( 20, network.bulk_wait_time() ) );
      }

      /* poll for events */
      sel.clear_fds();
      std::vector<int> fd_list( network.fds() );
      assert( fd_list.size() == 1 ); /* servers don't hop */
      int network_fd = fd_list.back();
      sel.add_fd( network_fd );
      std::vector<int> forward_fds( forwarder.fds() );
      for ( std::vector<int>::const_iterator it = forward_fds.begin(); it != forward_fds.end(); it++ ) {
        sel.add_fd( *it );
      }
      std::vector<int> bulk_fds( bulk_control.fds() );
      for ( std::vector<int>::const_iterator it = bulk_fds.begin(); it != bulk_fds.end(); it++ ) {
        sel.add_fd( *it );
      }
      const bool monitor_host = !network.shutdown_in_progress()
                                && network.get_current_state().get_tmux_output().size() < Terminal::TMUX_QUEUE_LIMIT;
      if ( monitor_host ) {
        sel.add_fd( host_fd );
      }
      if ( directory.fd() >= 0 && !directory_reply_pending && !control_channel.queued_bytes() ) { sel.add_fd( directory.fd() ); }
      if ( files.fd() >= 0 ) { sel.add_fd( files.fd() ); }

      int active_fds = sel.select( timeout );
      if ( active_fds < 0 ) {
        perror( "select" );
        break;
      }

      now = Network::timestamp();
      uint64_t time_since_remote_state = now - network.get_latest_remote_state().timestamp;
      std::string terminal_to_host;

      if ( sel.read( network_fd ) ) {
        /* packet received from the network */
        network.recv();

        Network::Bulk::Datagram bulk;
        while ( network.pop_bulk( bulk ) ) {
          if ( bulk.type == Network::Bulk::PacketType::FileSymbol || bulk.type == Network::Bulk::PacketType::FileAck ) {
            if ( files.supported() ) { files.channel.receive( bulk ); }
            continue;
          }
          if ( bulk.type == Network::Bulk::PacketType::DownloadSymbol || bulk.type == Network::Bulk::PacketType::DownloadAck ) {
            if ( downloads_enabled ) { downloads.channel.receive( bulk ); }
            continue;
          }
          if ( !mime_clipboard.channel.receive( bulk ) ) { bulk_control.broadcast( bulk ); }
        }

        /* is new user input available for the terminal? */
        if ( network.get_remote_state_num() != last_remote_num ) {
          last_remote_num = network.get_remote_state_num();

          Network::UserStream us;
          us.apply_string( network.get_remote_diff() );
          /* apply userstream to terminal */
          for ( size_t i = 0; i < us.size(); i++ ) {
            if ( us.is_graphics_event( i ) ) {
              const auto& caps = us.get_graphics_event( i );
              downloads_enabled = caps.downloads && !tmux_control && has_capability( getenv( "MOSH_CLIENT_CAPS" ), "goblin-download-v2" );
              downloads.set_enabled( downloads_enabled );
              mime_enabled = mime_negotiated && caps.clipboard && !tmux_control;
              terminal.set_mime_clipboard_enabled( mime_enabled );
              mime_clipboard.set_threshold( caps.clipboard_fast_threshold );
              terminal.set_sixel_enabled( sixel_state && ( caps.sixel || caps.kitty ) );
              terminal.set_keyboard_enabled( sixel_state && caps.keyboard );
              terminal.set_text_sizing_enabled( sixel_state );
              graphics_ready = true;
              continue;
            }
            if ( us.is_tmux_event( i ) ) {
              if ( tmux_control ) {
                terminal_to_host += us.get_tmux_input( i );
              }
              continue;
            }
            if ( us.is_client_geometry_event( i ) ) {
              client_geometry = sanitize_client_geometry( us.get_client_geometry_event( i ) );
              continue;
            }
            if ( us.is_stream_event( i ) ) {
              const auto& event = us.get_stream_event( i );
              if ( event.stream_id == Control::STREAM_ID ) {
                if ( directory_enabled && event.type == Network::StreamDataType ) {
                  control_channel.receive( event.data );
                  Control::Message request;
                  while ( control_channel.pop( request ) ) {
                    if ( request.kind() == Control::Message::FILE_CONTROL && request.has_file() ) {
                      files.receive_control( request.file(), now ); continue;
                    }
                    if ( request.kind() == Control::Message::OPEN || request.kind() == Control::Message::PAGE
                         || ( directory_live && request.kind() == Control::Message::WATCH ) ) {
                      if ( request.kind() != Control::Message::WATCH ) { directory_reply_pending = false; }
                      if ( !directory_live ) { request.set_live( false ); }
                      directory.request( request, now );
                    }
                  }
                }
              } else { forwarder.handle_remote_event( event ); }
              continue;
            }
            if ( us.is_clipboard_event( i ) ) {
              const Terminal::ClipboardEvent& ev = us.get_clipboard_event( i );
              if ( ev.op == Terminal::ClipboardSet || ev.op == Terminal::ClipboardClear ) {
                /* Reply to a host OSC 52 query, or a client-initiated set.
                   Write to the PTY; do not re-parse into the emulator or we
                   would echo the payload back to the client. */
                terminal_to_host += Terminal::encode_osc52( ev );
              }
              continue;
            }
            const Parser::Action& action = us.get_action( i );
            if ( typeid( action ) == typeid( Parser::Resize ) ) {
              /* apply only the last consecutive Resize action */
              if ( i < us.size() - 1 ) {
                const Parser::Action& next = us.get_action( i + 1 );
                if ( typeid( next ) == typeid( Parser::Resize ) ) {
                  continue;
                }
              }
              /* tell child process of resize */
              const Parser::Resize& res = static_cast<const Parser::Resize&>( action );
              struct winsize window_size;
              if ( ioctl( host_fd, TIOCGWINSZ, &window_size ) < 0 ) {
                perror( "ioctl TIOCGWINSZ" );
                network.start_shutdown();
              }
              window_size.ws_col = res.width;
              window_size.ws_row = res.height;
              if ( client_geometry.columns == res.width && client_geometry.rows == res.height ) {
                window_size.ws_xpixel = client_geometry.width_px;
                window_size.ws_ypixel = client_geometry.height_px;
              } else {
                window_size.ws_xpixel = 0;
                window_size.ws_ypixel = 0;
                client_geometry = Terminal::ClientGeometry();
              }
              if ( ioctl( host_fd, TIOCSWINSZ, &window_size ) < 0 ) {
                perror( "ioctl TIOCSWINSZ" );
                network.start_shutdown();
              }
            }
            terminal_to_host += terminal.act( action );
          }

          if ( !us.empty() ) {
            /* register input frame number for future echo ack */
            terminal.register_input_frame( last_remote_num, now );
          }

          /* update client with new state of terminal */
          if ( !network.shutdown_in_progress() ) {
            network.get_current_state().replace_terminal_state( terminal );
          }
#if defined( HAVE_SYSLOG ) || defined( HAVE_UTEMPTER )
#ifdef HAVE_UTEMPTER
          if ( !connected_utmp ) {
            force_connection_change_evt = true;
          } else {
            force_connection_change_evt = false;
          }
#else
          force_connection_change_evt = false;
#endif

          /**
           * - HAVE_UTEMPTER - update utmp entry if we have become "connected"
           * - HAVE_SYSLOG - log connection information to syslog
           **/
          if ( ( force_connection_change_evt ) || saved_addr_len != network.get_remote_addr_len()
               || memcmp( &saved_addr, &network.get_remote_addr(), saved_addr_len ) != 0 ) {

            saved_addr = network.get_remote_addr();
            saved_addr_len = network.get_remote_addr_len();

            char host[NI_MAXHOST];
            int errcode
              = getnameinfo( &saved_addr.sa, saved_addr_len, host, sizeof( host ), NULL, 0, NI_NUMERICHOST );
            if ( errcode != 0 ) {
              throw NetworkException( std::string( "serve: getnameinfo: " ) + gai_strerror( errcode ), 0 );
            }

#ifdef HAVE_UTEMPTER
            utempter_remove_record( host_fd );
            char tmp[64 + NI_MAXHOST];
            snprintf( tmp, 64 + NI_MAXHOST, "%s via goblin-skiff [%ld]", host, static_cast<long int>( getpid() ) );
            utempter_add_record( host_fd, tmp );

            connected_utmp = true;
#endif

#ifdef HAVE_SYSLOG
            syslog( LOG_INFO, "user %s connected from host: %s", pw->pw_name, host );
#endif
          }
#endif

          /* Tell child to start login session. */
          if ( !child_released && graphics_ready ) {
            if ( close( pipe_fd ) < 0 ) {
              err( 1, "child release" );
            }
            child_released = true;
          }
        }
      }

      for ( std::vector<int>::const_iterator it = forward_fds.begin(); it != forward_fds.end(); it++ ) {
        if ( sel.read( *it ) ) {
          forwarder.process_readable_fd( *it );
        }
      }

      for ( std::vector<int>::const_iterator it = bulk_fds.begin(); it != bulk_fds.end(); it++ ) {
        if ( sel.read( *it ) ) {
          bulk_control.process_readable_fd( *it );
        }
      }

      if ( monitor_host && !network.shutdown_in_progress() && sel.read( host_fd ) ) {
        /* input from the host needs to be fed to the terminal */
        const int buf_size = 16384;
        char buf[buf_size];

        /* fill buffer if possible */
        ssize_t bytes_read = read( host_fd, buf, buf_size );

        /* If the pty slave is closed, reading from the master can fail with
           EIO (see #264).  So we treat errors on read() like EOF. */
        if ( bytes_read <= 0 ) {
          network.start_shutdown();
        } else {
          if ( tmux_control ) {
            const Terminal::TmuxControlParser::Output output = tmux_parser.consume( std::string( buf, bytes_read ) );
            terminal_to_host += terminal.act( output.terminal, &client_geometry );
            network.get_current_state().append_tmux_output( output.control );
            if ( !output.control.empty() ) {
              network.request_immediate_send();
            }
          } else {
            terminal_to_host += terminal.act( std::string( buf, bytes_read ), &client_geometry );
          }

          /* update client with new state of terminal */
          for ( const auto& body : terminal.take_parser_mime_clipboard_events() ) { mime_clipboard.submit( body, now ); }
          for ( const auto& body : terminal.take_parser_download_events() ) { downloads.submit( body, now ); }
          network.get_current_state().replace_terminal_state( terminal );
        }
      }

      /* One-shot OSC 52 events from the host PTY. These ride the
         reliable hostbytes/keystroke channel once, not framebuffer state. */
      std::vector<Terminal::ClipboardEvent> clipboard_events = terminal.take_parser_clipboard_events();
      if ( !network.shutdown_in_progress() ) {
        for ( size_t i = 0; i < clipboard_events.size(); i++ ) {
          if ( Terminal::clipboard_should_transmit( clipboard_events[i] ) ) {
            network.get_current_state().push_back( clipboard_events[i] );
          }
        }
      }

      /* write user input and terminal writeback to the host */
      std::string mime_body;
      terminal_to_host += downloads.take_replies();
      if ( mime_enabled && mime_clipboard.take_output( mime_body ) ) { terminal_to_host += "\033]" + mime_body + "\033\\"; }
      if ( swrite( host_fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
        network.start_shutdown();
      }

      bool idle_shutdown = false;
      if ( network_timeout_ms && network_timeout_ms <= time_since_remote_state ) {
        idle_shutdown = true;
        fprintf( stderr,
                 "Network idle for %llu seconds.\n",
                 static_cast<unsigned long long>( time_since_remote_state / 1000 ) );
      }
      if ( sel.signal( SIGUSR1 )
           && ( !network_signaled_timeout_ms || network_signaled_timeout_ms <= time_since_remote_state ) ) {
        idle_shutdown = true;
        // An administrator explicitly reaping a disconnected session must
        // not wait for 16 retries at a shrinking link budget. Allow a short
        // paced goodbye, then reap it. Ordinary logout retains its full grace.
        idle_cleanup_deadline = now + 2000;
        fprintf( stderr,
                 "Network idle for %llu seconds when SIGUSR1 received\n",
                 static_cast<unsigned long long>( time_since_remote_state / 1000 ) );
      }

      if ( sel.any_signal() || idle_shutdown ) {
        /* shutdown signal */
        if ( network.has_remote_addr() && ( !network.shutdown_in_progress() ) ) {
          network.start_shutdown();
        } else {
          break;
        }
      }

      /* quit if our shutdown has been acknowledged */
      if ( network.shutdown_in_progress() && network.shutdown_acknowledged() ) {
        break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network.shutdown_in_progress() && network.shutdown_ack_timed_out() ) {
        break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network.counterparty_shutdown_ack_sent() ) {
        break;
      }

#ifdef HAVE_UTEMPTER
      /* update utmp if has been more than 30 seconds since heard from client */
      if ( connected_utmp && time_since_remote_state > 30000 ) {
        utempter_remove_record( host_fd );

        char tmp[64];
        snprintf( tmp, 64, "goblin-skiff [%ld]", static_cast<long int>( getpid() ) );
        utempter_add_record( host_fd, tmp );

        connected_utmp = false;
      }
#endif

      if ( terminal.set_echo_ack( now ) && !network.shutdown_in_progress() ) {
        /* update client with new echo ack */
        network.get_current_state().replace_terminal_state( terminal );
      }

      if ( !network.get_remote_state_num() && time_since_remote_state >= timeout_if_no_client ) {
        fprintf( stderr,
                 "No connection within %llu seconds.\n",
                 static_cast<unsigned long long>( timeout_if_no_client / 1000 ) );
        break;
      }

      if ( !network.shutdown_in_progress() ) {
        Network::StreamEvent event;
        if ( control_channel.take( now, network.get_sent_state_last(), network.get_sent_state_acked(), event ) ) {
          network.get_current_state().push_back( event );
          network.request_immediate_send();
        }
      }
      if ( !network.shutdown_in_progress() && !network.has_unsent_data() && !mime_clipboard.channel.has_interactive() ) {
        forwarder.flush( network.get_current_state(), now, network.send_interval(), network.max_datagram_payload() );
      }
      const int interactive_wait_before_tick = network.wait_time();
      network.tick();

      Network::Bulk::Datagram bulk;
      const bool sent_clipboard = mime_enabled && !network.shutdown_in_progress() && !network.bulk_wait_time()
        && mime_clipboard.channel.take_packet( Clipboard::Priority::Interactive, now, network.get_SRTT(), bulk );
      if ( sent_clipboard ) { network.send_bulk( bulk ); }
      const bool sent_download = !sent_clipboard && downloads_enabled && !network.shutdown_in_progress() && !network.bulk_wait_time()
        && downloads.channel.take_packet( Clipboard::Priority::Interactive, now, network.get_SRTT(), bulk );
      if ( sent_download ) { network.send_bulk( bulk ); }
      const bool sent_files = !sent_clipboard && !sent_download && !network.shutdown_in_progress() && !network.bulk_wait_time()
        && files.take_packet( Clipboard::Priority::Interactive, now, network.get_SRTT(), bulk );
      if ( sent_files ) { network.send_bulk( bulk ); }
      // Pending screen changes are not necessarily due yet. Let paced bulk
      // use the interval before the next frame instead of starving while
      // an application keeps its screen dirty. Due foreground work ran first.
      if ( interactive_wait_before_tick > 0 && network.wait_time() > 0
           && !sent_clipboard && !sent_download && !sent_files && !network.shutdown_in_progress()
           && !forwarder.has_pending_network_data() && !network.bulk_wait_time() ) {
        if ( mime_enabled && mime_clipboard.channel.take_packet( Clipboard::Priority::Background, now, network.get_SRTT(), bulk ) ) { network.send_bulk( bulk ); }
        else {
          // Share idle bulk opportunities with goblin-skiffcp. Neither class
          // can consume an opportunity reserved for terminal/socket traffic.
          bool sent = false;
          const unsigned first = bulk_turn++ % 3;
          for ( unsigned i = 0; i < 3 && !sent; ++i ) {
            switch ( ( first + i ) % 3 ) {
              case 0: sent = bulk_control.pop_outgoing( bulk ); break;
              case 1: sent = downloads_enabled && downloads.channel.take_packet( Clipboard::Priority::Background, now, network.get_SRTT(), bulk ); break;
              case 2: sent = files.take_packet( Clipboard::Priority::Background, now, network.get_SRTT(), bulk ); break;
            }
          }
          if ( sent ) { network.send_bulk( bulk ); }
        }
      }
    } catch ( const Network::NetworkException& e ) {
      fprintf( stderr, "%s\n", e.what() );
      spin();
    } catch ( const Crypto::CryptoException& e ) {
      if ( e.fatal ) {
        throw;
      } else {
        fprintf( stderr, "Crypto exception: %s\n", e.what() );
      }
    }
  }
#ifdef HAVE_SYSLOG
  syslog( LOG_INFO, "user %s session end", pw->pw_name );
#endif
}

/* Print the motd from a given file, if available */
static bool print_motd( const char* filename )
{
  FILE* motd = fopen( filename, "r" );
  if ( !motd ) {
    return false;
  }

  const int BUFSIZE = 256;

  char buffer[BUFSIZE];
  while ( 1 ) {
    size_t bytes_read = fread( buffer, 1, BUFSIZE, motd );
    if ( bytes_read == 0 ) {
      break; /* don't report error */
    }
    size_t bytes_written = fwrite( buffer, 1, bytes_read, stdout );
    if ( bytes_written == 0 ) {
      break;
    }
  }

  fclose( motd );
  return true;
}

static void chdir_homedir( void )
{
  const char* home = getenv( "HOME" );
  if ( home == NULL ) {
    struct passwd* pw = getpwuid( getuid() );
    if ( pw == NULL ) {
      perror( "getpwuid" );
      return; /* non-fatal */
    }
    home = pw->pw_dir;
  }

  if ( chdir( home ) < 0 ) {
    perror( "chdir" );
  }

  if ( setenv( "PWD", home, 1 ) < 0 ) {
    perror( "setenv" );
  }
}

static bool motd_hushed( void )
{
  /* must be in home directory already */
  struct stat buf;
  return 0 == lstat( ".hushlogin", &buf );
}

#ifdef HAVE_UTMPX_H
static bool device_exists( const char* ut_line )
{
  std::string device_name = std::string( "/dev/" ) + std::string( ut_line );
  struct stat buf;
  return 0 == lstat( device_name.c_str(), &buf );
}
#endif

static void warn_unattached( const std::string& ignore_entry )
{
#ifdef HAVE_UTMPX_H
  /* get username */
  const struct passwd* pw = getpwuid( getuid() );
  if ( pw == NULL ) {
    perror( "getpwuid" );
    /* non-fatal */
    return;
  }

  const std::string username( pw->pw_name );

  /* look for unattached sessions */
  std::vector<std::string> unattached_mosh_servers;

  while ( struct utmpx* entry = getutxent() ) {
    if ( ( entry->ut_type == USER_PROCESS ) && ( username == std::string( entry->ut_user ) ) ) {
      /* does line show unattached goblin-skiff session */
      std::string text( entry->ut_host );
      if ( ( text.size() >= sizeof( "goblin-skiff " ) )
           && ( text.compare( 0, sizeof( "goblin-skiff " ) - 1, "goblin-skiff " ) == 0 ) && ( text[text.size() - 1] == ']' )
           && ( text != ignore_entry ) && device_exists( entry->ut_line ) ) {
        unattached_mosh_servers.push_back( text );
      }
    }
  }

  /* print out warning if necessary */
  if ( unattached_mosh_servers.empty() ) {
    return;
  } else if ( unattached_mosh_servers.size() == 1 ) {
    printf( "\033[37;44mMosh: You have a detached Mosh session on this server (%s).\033[m\n\n",
            unattached_mosh_servers.front().c_str() );
  } else {
    std::string pid_string;

    for ( std::vector<std::string>::const_iterator it = unattached_mosh_servers.begin();
          it != unattached_mosh_servers.end();
          it++ ) {
      pid_string += "        - " + *it + "\n";
    }

    printf( "\033[37;44mMosh: You have %d detached Mosh sessions on this server, with PIDs:\n%s\033[m\n",
            (int)unattached_mosh_servers.size(),
            pid_string.c_str() );
  }
#endif /* HAVE_UTMPX_H */
}
