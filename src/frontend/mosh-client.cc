/*
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
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include "src/crypto/crypto.h"
#include "src/network/compressor.h"
#include "src/util/fatal_assert.h"
#include "src/util/locale_utils.h"
#include "stmclient.h"

/* These need to be included last because of conflicting defines. */
/*
 * stmclient.h includes termios.h, and that will break termio/termios pull in on Solaris.
 * The solution is to include termio.h also.
 * But Mac OS X doesn't have termio.h, so this needs a guard.
 */
#ifdef HAVE_TERMIO_H
#include <termio.h>
#endif

#if defined HAVE_NCURSESW_CURSES_H
#include <ncursesw/curses.h>
#include <ncursesw/term.h>
#elif defined HAVE_NCURSESW_H
#include <ncursesw.h>
#include <term.h>
#elif defined HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#include <ncurses/term.h>
#elif defined HAVE_NCURSES_H
#include <ncurses.h>
#include <term.h>
#elif defined HAVE_CURSES_H
#include <curses.h>
#include <term.h>
#else
#error "SysV or X/Open-compatible Curses header file required"
#endif

static void print_version( FILE* file )
{
  fputs( "adam-mosh-client (" PACKAGE_STRING ") [build " BUILD_VERSION "]\n"
         "Copyright 2012 Keith Winstein <mosh-devel@mit.edu>\n"
         "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n"
         "This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n",
         file );
}

static void print_usage( FILE* file, const char* argv0 )
{
  print_version( file );
  fprintf( file,
           "\nUsage: %s [-# 'ARGS'] [-A] [-X] [-L SPEC] [-D SPEC] [--stream-delay=MS] [--stream-bandwidth=BPS] [--state-zstd-dict=FILE] IP PORT\n"
           "       %s -c\n",
           argv0,
           argv0 );
}

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

static std::string string_from_env( const char* name )
{
  const char* value = getenv( name );
  return value ? value : "";
}

static void print_colorcount( void )
{
  /* check colors */
  setupterm( (char*)0, 1, (int*)0 );

  char colors_name[] = "colors";
  int color_val = tigetnum( colors_name );
  if ( color_val == -2 ) {
    fprintf( stderr, "Invalid terminfo numeric capability: %s\n", colors_name );
  }

  printf( "%d\n", color_val );
}

int main( int argc, char* argv[] )
{
  unsigned int verbose = 0;
  std::vector<std::string> local_forwards;
  std::vector<std::string> dynamic_forwards;
  bool agent_forwarding = false;
  bool x11_forwarding = false;
  unsigned int stream_delay_ms = uint_from_env( "MOSH_STREAM_DELAY", 75 );
  unsigned int stream_rate_bytes_per_second = uint_from_env( "MOSH_STREAM_BANDWIDTH", 2048 );
  std::string state_zstd_dictionary = string_from_env( "MOSH_STATE_ZSTD_DICT" );
  std::string state_sample_log = string_from_env( "MOSH_STATE_SAMPLE_LOG" );
  unsigned int state_sample_min_size = uint_from_env( "MOSH_STATE_SAMPLE_MIN_SIZE", 0 );
  /* For security, make sure we don't dump core */
  Crypto::disable_dumping_core();

  /* Detect edge case */
  fatal_assert( argc > 0 );

  /* Get arguments */
  for ( int i = 1; i < argc; i++ ) {
    if ( 0 == strcmp( argv[i], "--help" ) ) {
      print_usage( stdout, argv[0] );
      exit( 0 );
    }
    if ( 0 == strcmp( argv[i], "--version" ) ) {
      print_version( stdout );
      exit( 0 );
    }
  }

  int opt;
  static const struct option long_options[] = {
    { "stream-delay", required_argument, NULL, 256 },
    { "stream-bandwidth", required_argument, NULL, 257 },
    { "state-zstd-dict", required_argument, NULL, 258 },
    { "state-sample-log", required_argument, NULL, 259 },
    { "state-sample-min-size", required_argument, NULL, 260 },
    { 0, 0, 0, 0 },
  };
  while ( ( opt = getopt_long( argc, argv, "#:AcvXL:D:", long_options, NULL ) ) != -1 ) {
    switch ( opt ) {
      case '#':
        // Ignore the original arguments to mosh wrapper
        break;
      case 'A':
        agent_forwarding = true;
        break;
      case 'c':
        print_colorcount();
        exit( 0 );
        break;
      case 'L':
        local_forwards.push_back( optarg );
        break;
      case 'D':
        dynamic_forwards.push_back( optarg );
        break;
      case 'X':
        x11_forwarding = true;
        break;
      case 'v':
        verbose++;
        break;
      case 256:
        stream_delay_ms = parse_uint_option( "--stream-delay", optarg );
        break;
      case 257:
        stream_rate_bytes_per_second = parse_uint_option( "--stream-bandwidth", optarg );
        if ( stream_rate_bytes_per_second == 0 ) {
          fputs( "--stream-bandwidth must be greater than zero\n", stderr );
          exit( 1 );
        }
        break;
      case 258:
        state_zstd_dictionary = optarg;
        break;
      case 259:
        state_sample_log = optarg;
        break;
      case 260:
        state_sample_min_size = parse_uint_option( "--state-sample-min-size", optarg );
        break;
      default:
        print_usage( stderr, argv[0] );
        exit( 1 );
        break;
    }
  }

  char *ip, *desired_port;

  if ( argc - optind != 2 ) {
    print_usage( stderr, argv[0] );
    exit( 1 );
  }

  ip = argv[optind];
  desired_port = argv[optind + 1];

  /* Sanity-check arguments */
  if ( desired_port && ( strspn( desired_port, "0123456789" ) != strlen( desired_port ) ) ) {
    fprintf( stderr, "%s: Bad UDP port (%s)\n\n", argv[0], desired_port );
    print_usage( stderr, argv[0] );
    exit( 1 );
  }

  /* Read key from environment */
  char* env_key = getenv( "MOSH_KEY" );
  if ( env_key == NULL ) {
    fputs( "MOSH_KEY environment variable not found.\n", stderr );
    exit( 1 );
  }

  /* Read prediction preference */
  char* predict_mode = getenv( "MOSH_PREDICTION_DISPLAY" );
  /* can be NULL */

  /* Read prediction insertion preference */
  char* predict_overwrite = getenv( "MOSH_PREDICTION_OVERWRITE" );
  /* can be NULL */

  std::string key( env_key );

  if ( unsetenv( "MOSH_KEY" ) < 0 ) {
    perror( "unsetenv" );
    exit( 1 );
  }

  /* Adopt native locale */
  set_native_locale();

  bool success = false;
  try {
    if ( !state_zstd_dictionary.empty() ) {
      Network::get_compressor().set_zstd_dictionary_from_file( state_zstd_dictionary );
    }

    STMClient client( ip,
                      desired_port,
                      key.c_str(),
                      predict_mode,
                      verbose,
                      predict_overwrite,
                      local_forwards,
                      dynamic_forwards,
                      agent_forwarding,
                      x11_forwarding,
                      stream_delay_ms,
                      stream_rate_bytes_per_second,
                      state_sample_log,
                      state_sample_min_size );
    client.init();

    try {
      success = client.main();
    } catch ( ... ) {
      client.shutdown();
      throw;
    }

    client.shutdown();
  } catch ( const Network::NetworkException& e ) {
    fprintf( stderr, "Network exception: %s\r\n", e.what() );
    success = false;
  } catch ( const Crypto::CryptoException& e ) {
    fprintf( stderr, "Crypto exception: %s\r\n", e.what() );
    success = false;
  } catch ( const std::exception& e ) {
    fprintf( stderr, "Error: %s\r\n", e.what() );
    success = false;
  }

  printf( "[adam-mosh is exiting.]\n" );

  return !success;
}
