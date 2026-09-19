#!/usr/bin/env perl

#   Mosh: the mobile shell
#   Copyright 2012 Keith Winstein
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#   In addition, as a special exception, the copyright holders give
#   permission to link the code of portions of this program with the
#   OpenSSL library under certain conditions as described in each
#   individual source file, and distribute linked combinations including
#   the two.
#
#   You must obey the GNU General Public License in all respects for all
#   of the code used other than OpenSSL. If you modify file(s) with this
#   exception, you may extend this exception to your version of the
#   file(s), but you are not obligated to do so. If you do not wish to do
#   so, delete this exception statement from your version. If you delete
#   this exception statement from all source files in the program, then
#   also delete it here.

use 5.8.8;

use warnings;
use strict;
use Getopt::Long;
use IO::Socket;
use IPC::Open3;
use Text::ParseWords;
use Socket qw(IPPROTO_TCP);
use Errno qw(EINTR);
use POSIX qw(_exit);
use Symbol qw(gensym);

Getopt::Long::Configure( 'no_ignore_case' );

BEGIN {
  my @gai_reqs = qw( getaddrinfo getnameinfo AI_CANONNAME AI_NUMERICHOST NI_NUMERICHOST );
  eval { Socket->import( @gai_reqs ); 1; }
    || (eval { require Socket::GetAddrInfo; 1; }
        && (eval { Socket::GetAddrInfo->import( ':newapi', @gai_reqs ); 1; }
            || eval { Socket::GetAddrInfo->import( '0.22', @gai_reqs ); 1; }))
    || die "$0 error: requires Perl 5.14 or Socket::GetAddrInfo.\n";
}

my $have_ipv6 = eval {
  require IO::Socket::IP;
  IO::Socket::IP->import('-register');
  1;
} || eval {
  require IO::Socket::INET6;
  1;
};

$|=1;

my $client = 'goblin-skiff-client';
my $server = 'goblin-skiff-server';

my $predict = undef;

my $overwrite = 0;

my $bind_ip = undef;

my $use_remote_ip = 'proxy';

my $family = 'prefer-inet';
my $port_request = undef;

my @ssh = ('ssh');
my $socks5_proxy;
my $proxy_report = 1; # internal ProxyCommand: report the destination, not the proxy
my $jump = undef;
my @jumps;
my @jump_ssh;
my $jump_server = 'goblin-skiff-server';
my $jump_port = undef;
my $jump_idle_timeout = 86400;
my @local_forwards;
my @remote_forwards;
my @dynamic_forwards;
my $agent_forwarding = 0;
my $x11_forwarding = 0;
my $fips_crypto = 0;
my $stream_delay = undef;
my $stream_bandwidth = undef;
my $state_zstd_dict = undef;
my $state_sample_log = undef;
my $state_sample_min_size = undef;
my $remote_state_zstd_dict = undef;
my $uploaded_state_zstd_dict = 0;
my $client_term = $ENV{ 'TERM' };
my $tmux_control = 0;
my $mascot = $ENV{ 'GOBLIN_SKIFF_MASCOT' } // 'auto';
my $no_kitty = 0;
my $no_sixel = 0;
my $lossy_quality = undef;
my $djvu_lossy = 0;
my $clipboard_fast_threshold = $ENV{ 'GOBLIN_SKIFF_CLIPBOARD_FAST_THRESHOLD' } // 65536;
my $no_downloads = 0;
my $download_directory = undef;
my $server_directory = 0;
my $server_files = 0;
my $server_link_budget = 0;

# Keep the session on the primary screen by default so scroll operations
# become part of the local terminal emulator's native scrollback.
my $term_init = 0;

my $localhost = undef;

my $ssh_pty = 1;

my $help = undef;
my $version = undef;

my @cmdline = @ARGV;

my $usage =
qq{Usage: $0 [options] [--] [user@]host [command...]
        --client=PATH        goblin-skiff client on local machine
                                (default: "goblin-skiff-client")
        --server=COMMAND     goblin-skiff server on remote machine
                                (default: "goblin-skiff-server")

        --predict=adaptive      local echo for slower links [default]
-a      --predict=always        use local echo even on fast links
-n      --predict=never         never use local echo
        --predict=experimental  aggressively echo even when incorrect

-o      --predict-overwrite     prediction overwrites instead of inserting

-4      --family=inet        use IPv4 only
-6      --family=inet6       use IPv6 only
        --family=auto        autodetect network type for single-family hosts only
        --family=all         try all network types
        --family=prefer-inet use all network types, but try IPv4 first [default]
        --family=prefer-inet6 use all network types, but try IPv6 first
-p PORT[:PORT2]
        --port=PORT[:PORT2]  server-side UDP port or range
                                (No effect on server-side SSH port)
-L [BIND:]PORT:HOST:HOSTPORT
                            forward a local TCP port to the remote side
-R [BIND:]PORT:HOST:HOSTPORT
                            forward a remote TCP port to the local side
-D [BIND:]PORT             open a local SOCKS5 dynamic forward
-J [USER@]HOST[:PORT][,...]
        --jump=HOSTS       relay the Skiff UDP session through 1-4 jump hosts
                                (also discovers OpenSSH ProxyJump configuration)
        --jump-server=COMMAND   server command on each jump host
                                (default: "goblin-skiff-server")
        --jump-port=PORT[:PORT2] UDP listener range on each jump (default: 60001:60999)
        --jump-idle-timeout=SEC  relay lease without authenticated client traffic
                                (default: 86400; range: 1800..604800)
-A                         forward the local SSH authentication agent
-X                         forward X11 connections
        --fips-crypto      require the OpenSSL FIPS provider and use AES-128-GCM
                                for the UDP session
        --tmux-control     pass tmux -CC through to the local terminal
        --mascot=FORMAT    startup goblin: auto, kitty, sixel, ascii, none
        --no-mascot        disable the local startup goblin
        --no-kitty         disable local Kitty graphics and detection
        --lossy=QUALITY    lossy WebP images, quality 0-100 (higher is better)
        --djvu-lossy       allow cjb2 symbol substitution for two-color images
        --no-sixel         disable local sixel graphics and detection
        --clipboard-fast-threshold=BYTES
                            unsolicited remote blobs above this use bulk FEC
                                (default: 65536; text and local uploads stay fast)
        --no-downloads     disable Goblin inline file downloads
        --download-directory=DIR
                            local destination (default: ~/Downloads)
        --stream-delay=MS   coalesce forwarded stream bytes before sending
                                (default: 75)
        --stream-bandwidth=BPS
                            cap forwarded stream payload bytes per second
                                (default: adaptive; 2048 with older peers)
        --state-zstd-dict=FILE
                            use a compiled dictionary for negotiated zstd-22 state updates;
                                the wrapper uploads the compressed file to the server
        --state-sample-log=FILE
                            write received uncompressed state samples to FILE
                                for dictionary training
        --state-sample-min-size=BYTES
                            only log state samples at least this large
                                (default: 0)
        --bind-server={ssh|any|IP}  ask the server to reply from an IP address
                                       (default: "ssh")

        --ssh=COMMAND        ssh command to run when setting up session
                                (example: "ssh -p 2222")
                                (default: "ssh")
        --socks5-proxy=HOST:PORT
                             use SOCKS5 for SSH and UDP (including the first jump);
                             resolve destination names at the proxy, no direct fallback
                             (IPv6 proxy: [ADDRESS]:PORT; no proxy authentication)

        --no-ssh-pty         do not allocate a pseudo tty on ssh connection

        --native-scroll      use the local terminal's native scrollback [default]
        --alternate-screen   isolate the session in the alternate screen
        --no-init            compatibility alias for --native-scroll

        --local              run goblin-skiff-server locally without using ssh

        --experimental-remote-ip=(local|remote|proxy)  select the method for
                             discovering the remote IP address to use for Skiff
                             (default: "proxy")

        --help               this message
        --version            version and copyright information

Please report bugs to https://github.com/adamdeprince/goblin-skiff/issues.
Goblin Skiff home page: https://skiff.goblinreactor.com\n};

my $version_message = 'goblin-skiff @GOBLIN_VERSION@ (@PACKAGE_STRING@) [build @VERSION@]' . qq{
Copyright 2012 Keith Winstein <mosh-devel\@mit.edu>
License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.\n};

sub predict_check {
  my ( $predict, $env_set ) = @_;

  if ( not exists { adaptive => 0, always => 0,
		    never => 0, experimental => 0 }->{ $predict } ) {
    my $explanation = $env_set ? " (MOSH_PREDICTION_DISPLAY in environment)" : "";
    print STDERR qq{$0: Unknown mode \"$predict\"$explanation.\n\n};

    die $usage;
  }
}

GetOptions( 'client=s' => \$client,
	    'server=s' => \$server,
	    'predict=s' => \$predict,
	    'predict-overwrite|o!' => \$overwrite,
	    'port=s' => \$port_request,
	    'a' => sub { $predict = 'always' },
	    'n' => sub { $predict = 'never' },
	    'family=s' => \$family,
	    '4' => sub { $family = 'inet' },
	    '6' => sub { $family = 'inet6' },
	    'p=s' => \$port_request,
	    'L=s@' => \@local_forwards,
	    'R=s@' => \@remote_forwards,
	    'D=s@' => \@dynamic_forwards,
	    'jump|J=s' => \$jump,
	    'jump-server=s' => \$jump_server,
	    'jump-port=s' => \$jump_port,
	    'jump-idle-timeout=s' => \$jump_idle_timeout,
	    'A' => \$agent_forwarding,
	    'X' => \$x11_forwarding,
	    'fips-crypto' => \$fips_crypto,
	    'tmux-control' => \$tmux_control,
	    'mascot=s' => \$mascot,
	    'no-mascot' => sub { $mascot = 'none'; },
	    'no-kitty' => \$no_kitty,
	    'no-sixel' => \$no_sixel,
	    'lossy=s' => \$lossy_quality,
	    'djvu-lossy' => \$djvu_lossy,
	    'clipboard-fast-threshold=s' => \$clipboard_fast_threshold,
	    'no-downloads' => \$no_downloads,
	    'download-directory=s' => \$download_directory,
	    'stream-delay=i' => \$stream_delay,
	    'stream-bandwidth=i' => \$stream_bandwidth,
	    'state-zstd-dict=s' => \$state_zstd_dict,
	    'state-sample-log=s' => \$state_sample_log,
	    'state-sample-min-size=i' => \$state_sample_min_size,
	    'ssh=s' => sub { @ssh = shellwords($_[1]); },
	    'socks5-proxy=s' => \$socks5_proxy,
	    'ssh-pty!' => \$ssh_pty,
	    'native-scroll' => sub { $term_init = 0; },
	    'alternate-screen' => sub { $term_init = 1; },
	    'init!' => \$term_init,
	    'local' => \$localhost,
	    'help' => \$help,
	    'version' => \$version,
	    'fake-proxy!' => \my $fake_proxy,
	    'proxy-report!' => \$proxy_report,
	    'bind-server=s' => \$bind_ip,
	    'experimental-remote-ip=s' => \$use_remote_ip) or die $usage;

if ( defined $help ) {
    print $usage;
    exit;
}
if ( defined $version ) {
    print $version_message;
    exit;
}

if ( defined $socks5_proxy ) {
  die "$0: --socks5-proxy requires HOST:PORT or [IPv6]:PORT (no URL or credentials).\n"
    unless $socks5_proxy =~ /\A(?:[A-Za-z0-9_.-]+|\[[A-Fa-f0-9:]+\]):([0-9]{1,5})\z/
      && $1 > 0 && $1 <= 65535;
  die "$0: --socks5-proxy cannot be used with --local.\n" if $localhost;
}

if ( defined $predict ) {
  predict_check( $predict, 0 );
} elsif ( defined $ENV{ 'MOSH_PREDICTION_DISPLAY' } ) {
  $predict = $ENV{ 'MOSH_PREDICTION_DISPLAY' };
  predict_check( $predict, 1 );
} else {
  $predict = 'adaptive';
  predict_check( $predict, 0 );
}

die "$0: --mascot must be auto, kitty, sixel, ascii, or none.\n"
  unless $mascot =~ /\A(?:auto|kitty|sixel|ascii|none)\z/;
die "$0: --lossy requires an integer quality from 0 to 100.\n"
  if defined $lossy_quality && ( $lossy_quality !~ /\A[0-9]{1,3}\z/ || $lossy_quality > 100 );

if ( not grep { $_ eq $use_remote_ip } qw { local remote proxy } ) {
  die "Unknown parameter $use_remote_ip";
}

$family = lc( $family );
# Handle IPv4-only Perl installs.
if (!$have_ipv6) {
  # Report failure if IPv6 needed and not available.
  if (defined($family) && $family eq "inet6") {
    die "$0: IPv6 sockets not available in this Perl install\n";
  }
  # Force IPv4.
  $family = "inet";
}
if ( $overwrite ) {
    $ENV{ "MOSH_PREDICTION_OVERWRITE" } = "yes";
}

if ( defined $stream_delay and $stream_delay < 0 ) {
  die "$0: --stream-delay must be non-negative.\n";
}

if ( defined $stream_bandwidth and $stream_bandwidth <= 0 ) {
  die "$0: --stream-bandwidth must be greater than zero.\n";
}
die "$0: --clipboard-fast-threshold must be an integer between 0 and 67108864.\n"
  unless $clipboard_fast_threshold =~ /\A[0-9]+\z/ && $clipboard_fast_threshold <= 67108864;

if ( defined $state_sample_min_size and $state_sample_min_size < 0 ) {
  die "$0: --state-sample-min-size must be non-negative.\n";
}

if ( defined $state_zstd_dict and not -r $state_zstd_dict ) {
  die "$0: --state-zstd-dict file is not readable: $state_zstd_dict\n";
}

if ( $agent_forwarding and not defined $ENV{ 'SSH_AUTH_SOCK' } ) {
  die "$0: -A requested but SSH_AUTH_SOCK is not set.\n";
}

if ( $x11_forwarding and not defined $ENV{ 'DISPLAY' } ) {
  die "$0: -X requested but DISPLAY is not set.\n";
}

if ( defined $port_request ) {
  if ( $port_request =~ m{^(\d+)(:(\d+))?$} ) {
    my ( $low, $clause, $high ) = ( $1, $2, $3 );
    # good port or port-range
    if ( $low < 0 or $low > 65535 ) {
      die "$0: Server-side (low) port ($low) must be within valid range [0..65535].\n";
    }
    if ( defined $high ) {
      if ( $high <= 0 or $high > 65535 ) {
	die "$0: Server-side high port ($high) must be within valid range [1..65535].\n";
      }
      if ( $low == 0 ) {
	die "$0: Server-side port ranges may not be used with starting port 0 ($port_request).\n";
      }
      if ( $low > $high ) {
	die "$0: Server-side port range ($port_request): low port greater than high port.\n";
      }
    }
  } else {
    die "$0: Server-side port or range ($port_request) is not valid.\n";
  }
}

delete $ENV{ 'MOSH_PREDICTION_DISPLAY' };
delete $ENV{ 'GOBLIN_SKIFF_COMPACT_KEEPALIVE' };
delete $ENV{ 'GOBLIN_SKIFF_LINK_BUDGET' };
delete $ENV{ 'MOSH_NO_TERM_INIT' };
delete $ENV{ 'GOBLIN_SKIFF_RELAY_KEYS' };
delete $ENV{ 'MOSH_RELAY_HOPS' };

my $userhost;
my @command;
my @bind_arguments;

if ( ! defined $fake_proxy ) {
  if ( scalar @ARGV < 1 ) {
    die $usage;
  }
  $userhost = shift;
  @command = @ARGV;
  if ( not defined $bind_ip or $bind_ip =~ m{^ssh$}i ) {
    if ( not defined $localhost ) {
      push @bind_arguments, '-s';
    } else {
      push @bind_arguments, ('-i', "$userhost");
    }
  } elsif ( $bind_ip =~ m{^any$}i ) {
    # do nothing
  } else {
    push @bind_arguments, ('-i', "$bind_ip");
  }
} else {
  my ( $host, $port ) = @ARGV;

  my ( $connect_host, $connect_port ) = ( $host, $port );
  if ( defined $socks5_proxy ) {
    ( $connect_host, $connect_port ) = $socks5_proxy =~ /\A(.+):([0-9]+)\z/;
    $connect_host =~ s/\A\[(.*)\]\z/$1/;
  }
  # The proxy's address family is independent of the tailnet destination's.
  # In particular, an IPv4-only kernel can carry an IPv6 target via SOCKS5.
  my @res = resolvename( $connect_host, $connect_port, defined $socks5_proxy ? 'prefer-inet' : $family );

  # Now try and connect to something.
  my $err;
  my $sock;
  my $addr_string;
  my $service;
  for my $ai ( @res ) {
    ( $err, $addr_string, $service ) = getnameinfo( $ai->{addr}, NI_NUMERICHOST );
    next if $err;
    if ( $sock = IO::Socket->new( Domain => $ai->{family},
				  Family => $ai->{family},
				  PeerHost => $addr_string,
				  PeerPort => $connect_port,
				  Proto => 'tcp',
				  Timeout => 30 )) {
      last;
    } else {
      $err = $@;
    }
  }
  die "$0: Could not connect to ${host}, last tried ${addr_string}: ${err}\n" if !$sock;
  if ( defined $socks5_proxy ) {
    socks_connect( $sock, $host, $port );
    $addr_string = $host;
  }
  print STDERR 'MOSH IP ', $addr_string, "\n" if $proxy_report;

  # Act like netcat
  binmode($sock);
  binmode(STDIN);
  binmode(STDOUT);

  sub cat {
    my ( $from, $to ) = @_;
    while ( my $n = $from->sysread( my $buf, 4096 ) ) {
      next if ( $n == -1 && $! == EINTR );
      $n >= 0 or last;
      my $offset = 0;
      while ( $offset < length $buf ) {
        my $written = syswrite( $to, $buf, length( $buf ) - $offset, $offset );
        next if !defined( $written ) && $! == EINTR;
        return if !defined( $written ) || !$written;
        $offset += $written;
      }
    }
  }

  defined( my $pid = fork ) or die "$0: fork: $!\n";
  if ( $pid == 0 ) {
    close STDIN;
    cat $sock, \*STDOUT; $sock->shutdown( 0 );
    _exit 0;
  }
  $SIG{ 'HUP' } = 'IGNORE';
  close STDOUT;
  cat \*STDIN, $sock; $sock->shutdown( 1 );
  close STDIN;
  waitpid $pid, 0;
  exit;
}

# Let OpenSSH evaluate Host/Match rules, including -J inside --ssh. Do not
# install our legacy ProxyCommand when SSH selected a jump route.
die "$0: --jump cannot be used with --local.\n" if defined $jump && $localhost;
die "$0: --jump-idle-timeout must be 1800..604800 seconds.\n"
  unless $jump_idle_timeout =~ /\A[0-9]+\z/ && $jump_idle_timeout >= 1800 && $jump_idle_timeout <= 604800;
if ( defined $jump_port ) {
  die "$0: invalid --jump-port.\n"
    unless $jump_port =~ /\A([0-9]+)(?::([0-9]+))?\z/
      && $1 <= 65535 && ( !defined $2 || ( $1 > 0 && $2 >= $1 && $2 <= 65535 ) );
}
if ( !$localhost ) {
  push @ssh, ( '-J', $jump ) if defined $jump;
  my %configuration = ssh_configuration( @ssh, '-G', '-T', $userhost );
  $jump = $configuration{proxyjump} if defined $configuration{proxyjump};
  if ( defined $jump && lc( $jump ) ne 'none' ) {
    @jumps = split /,/, $jump, -1;
    die "$0: UDP jump routes support one to four hosts.\n" unless @jumps >= 1 && @jumps <= 4;
    parse_jump( $_ ) for @jumps;
    $use_remote_ip = 'remote'; # target names may resolve only behind the jump
    # Like OpenSSH -J, use per-host SSH configuration for jump credentials and
    # ports, not destination-specific -l/-p/-i options. Carry -F to the hops.
    @jump_ssh = ( $ssh[0] );
    for ( my $i = 1; $i < @ssh; ++$i ) {
      if ( $ssh[$i] eq '-F' ) {
        die "$0: missing -F argument in --ssh.\n" if ++$i >= @ssh;
        push @jump_ssh, ( '-F', $ssh[$i] );
      } elsif ( $ssh[$i] =~ /\A-F(.+)/ ) { push @jump_ssh, $ssh[$i]; }
    }
    # OpenSSH also permits the first jump alias to have its own ProxyJump.
    # Expand that prefix so the UDP path follows the complete SSH route.
    my %expanded;
    while ( 1 ) {
      die "$0: cyclic ProxyJump configuration.\n" if $expanded{$jumps[0]}++;
      my ( $first_host, $first_port ) = parse_jump( $jumps[0] );
      my @query = ( @jump_ssh, '-G', '-T' );
      push @query, ( '-p', $first_port ) if defined $first_port;
      my %first = ssh_configuration( @query, $first_host );
      last unless defined $first{proxyjump} && lc( $first{proxyjump} ) ne 'none';
      my @prefix = split /,/, $first{proxyjump}, -1;
      parse_jump( $_ ) for @prefix;
      unshift @jumps, @prefix;
      die "$0: UDP jump routes support at most four hosts, including nested ProxyJump settings.\n" if @jumps > 4;
    }
  }
}

# The local proxy knows the tailnet names. Never resolve the destination in
# the tablet's kernel DNS, or confuse SSH_CONNECTION's loopback address with
# the actual peer. Jump destinations still use their SSH-facing address.
$use_remote_ip = 'proxy' if defined $socks5_proxy && !@jumps;

# Count colors and, in FIPS mode, fail before starting anything remotely if
# the local client cannot initialize its configured provider.
my @colorcount_args = $fips_crypto ? ( '--fips-crypto', '-c' ) : ( '-c' );
unshift @colorcount_args, '--tmux-control' if $tmux_control;
unshift @colorcount_args, '--udp-relay' if @jumps;
unshift @colorcount_args, "--socks5-proxy=$socks5_proxy" if defined $socks5_proxy;
open COLORCOUNT, '-|', $client, @colorcount_args or die "Can't count colors: $!\n";
my $colors = "";
{
  local $/ = undef;
  $colors = <COLORCOUNT>;
}
close COLORCOUNT or die;

# A separately selected older client can still receive WebP. Probe without
# exposing its unknown-option diagnostics, and advertise only confirmed codecs.
my $client_palette_djvu = 0;
my $codec_pid = open( my $codec_output, '-|' );
die "Can't query client image codecs: $!\n" unless defined $codec_pid;
if ( !$codec_pid ) {
  open STDERR, '>', '/dev/null' or die "Can't redirect codec probe: $!\n";
  exec {$client} $client, '--image-codecs';
  die "Can't query client image codecs: $!\n";
}
{
  local $/ = undef;
  my $codecs = <$codec_output> // '';
  $client_palette_djvu = close( $codec_output ) && $codecs =~ /(?:\A|\s)palette-djvu-v1(?:\s|\z)/;
}
die "$0: --djvu-lossy requires a client supporting palette-djvu-v1; update goblin-skiff-client.\n"
  if $djvu_lossy && !$client_palette_djvu;

# Query the actual selected binary, which may differ from this wrapper's build.
# Older clients do not implement this query and remain explicitly unversioned.
my @client_version;
my $version_pid = open( my $version_output, '-|' );
die "Can't query client version: $!\n" unless defined $version_pid;
if ( !$version_pid ) {
  open STDERR, '>', '/dev/null' or die "Can't redirect version probe: $!\n";
  exec {$client} $client, '--connection-version';
  die "Can't query client version: $!\n";
}
{
  local $/ = undef;
  my $identity = <$version_output> // '';
  if ( close( $version_output ) ) { @client_version = parse_connection_version( $identity ); }
}

chomp $colors;

if ( (not defined $colors)
    or $colors !~ m{^[0-9]+$}
    or $colors < 0 ) {
  $colors = 0;
}

$ENV{ 'MOSH_CLIENT_PID' } = $$; # We don't support this, but it's useful for test and debug.

# If we are using a locally-resolved address, we have to get it before we fork,
# so both parent and child get it.
my $ip;
if ( $use_remote_ip eq 'local' ) {
  # "parse" the host from what the user gave us
  my ($user, $host) = $userhost =~ /^((?:.*@)?)(.*)$/;
  # get list of addresses
  my @res = resolvename( $host, 22, $family );
  # Use only the first address as the Mosh IP
  my $hostaddr = $res[0];
  if ( !defined $hostaddr ) {
    die( "could not find address for $host" );
  }
  my ( $err, $addr_string, $service ) = getnameinfo( $hostaddr->{addr}, NI_NUMERICHOST );
  if ( $err ) {
    die( "could not use address for $host" );
  }
  $ip = $addr_string;
  $userhost = "$user$ip";
}

if ( defined $state_zstd_dict ) {
  if ( defined $localhost ) {
    $remote_state_zstd_dict = $state_zstd_dict;
  } else {
    $remote_state_zstd_dict = upload_state_dictionary( $state_zstd_dict, $userhost, $family, $use_remote_ip );
    $uploaded_state_zstd_dict = 1;
  }
}

my $pid = open(my $pipe, "-|");
die "$0: fork: $!\n" unless ( defined $pid );
if ( $pid == 0 ) { # child
  open(STDERR, ">&STDOUT") or die;

  my @sshopts = ( '-n' );
  if ($ssh_pty) {
      push @sshopts, '-tt';
  }

  my $ssh_connection = "";
  if ( $use_remote_ip eq 'remote' ) {
    # Ask the server for its IP.  The user's shell may not be
    # Posix-compatible so invoke sh explicitly.
    $ssh_connection = "sh -c " .
      shell_quote ( '[ -n "$SSH_CONNECTION" ] && printf "\nMOSH SSH_CONNECTION %s\n" "$SSH_CONNECTION"' ) .
      " ; ";
    # Only with 'remote', we may need to tell SSH which protocol to use.
    if ( $family eq 'inet' ) {
      push @sshopts, '-4';
    } elsif ( $family eq 'inet6' ) {
      push @sshopts, '-6';
    }
  }
  my @server = ( 'new' );
  push @server, "--lossy=$lossy_quality" if defined $lossy_quality;
  push @server, '--djvu-lossy' if $djvu_lossy;

  push @server, '--fips-crypto' if $fips_crypto;

  push @server, ( '-c', $colors );

  push @server, @bind_arguments;

  if ( defined $port_request ) {
    push @server, ( '-p', $port_request );
  }

  for ( @remote_forwards ) {
    push @server, ( '-R', $_ );
  }

  if ( $agent_forwarding ) {
    push @server, '-A';
  }

  if ( $x11_forwarding ) {
    push @server, '-X';
  }

  if ( defined $stream_delay ) {
    push @server, ( '-t', $stream_delay );
  }

  if ( defined $stream_bandwidth ) {
    push @server, ( '-b', $stream_bandwidth );
  }

  for ( &locale_vars ) {
    push @server, ( '-l', $_ );
  }

  if ( scalar @command > 0 ) {
    push @server, '--', @command;
  }

  if ( defined( $localhost )) {
    delete $ENV{ 'SSH_CONNECTION' };
    chdir; # $HOME
    print "MOSH IP ${userhost}\n";
    exec( server_command_string( $server, @server ) );
    die "Cannot exec $server: $!\n";
  }
  if ( $use_remote_ip eq 'proxy' && !defined $socks5_proxy ) {
    # Non-standard shells and broken shrc files cause the ssh
    # proxy to break mysteriously.
    $ENV{ 'SHELL' } = '/bin/sh';
    my $quoted_proxy_command = shell_quote( $0, "--family=$family" );
    push @sshopts, ( '-S', 'none', '-o', "ProxyCommand=$quoted_proxy_command --fake-proxy -- %h %p" );
  }
  push @sshopts, '-S', 'none' if defined $socks5_proxy; # after any explicit --ssh -S
  my @exec_argv = ( @ssh, @sshopts, $userhost, '--', $ssh_connection . server_command_string( $server, @server ) );
  if ( defined $socks5_proxy ) {
    $ENV{ 'SHELL' } = '/bin/sh';
    # First occurrence wins in OpenSSH: deliberately replace ProxyCommand/-J
    # only when the user explicitly selects our proxy, preserving all other
    # per-host authentication, HostName, host-key and port configuration.
    splice @exec_argv, 1, 0, '-S', 'none', '-o', 'ControlMaster=no',
      '-o', 'ProxyCommand=' . socks_proxy_command( scalar @jumps, !@jumps );
  }
  exec @exec_argv;
  die "Cannot exec ssh: $!\n";
} else { # parent
  my ( $sship, $port, $key, $server_crypto );
  my @server_version;
  my $compact_keepalive = 0;
  my $server_tmux_control = 0;
  my $server_sixel_state = 0;
  my $server_image_encoding = 0;
  my $server_clipboard = 0;
  my $server_downloads = 0;
  my $server_relay_hops = 0;
  my $bad_udp_port_warning = 0;
  LINE: while ( <$pipe> ) {
    chomp;
    if ( m{^MOSH IP } ) {
      if ( defined $ip ) {
	die "$0 error: detected attempt to redefine MOSH IP.\n";
      }
      ( $ip ) = m{^MOSH IP (\S+)\s*$} or die "Bad MOSH IP string: $_\n";
    } elsif ( m{^MOSH SSH_CONNECTION } ) {
      my @words = split;
      if ( scalar @words == 6 ) {
	$sship = $words[4];
      } else {
	die "Bad MOSH SSH_CONNECTION string: $_\n";
      }
    } elsif ( m{^MOSH VERSION } ) {
      die "Duplicate MOSH VERSION message.\n" if @server_version;
      @server_version = parse_connection_version( $_ );
    } elsif ( m{^MOSH CAPS } ) {
      if ( m{^MOSH CAPS keepalive-v1\s*$} ) {
	$compact_keepalive = 1;
      } else {
	die "Bad MOSH CAPS string: $_\n";
      }
    } elsif ( m{^MOSH IMAGE } ) {
      die "Bad MOSH IMAGE string: $_\n" unless m{^MOSH IMAGE (?:webp|palette-djvu-v1)\s*$};
      $server_image_encoding = 1;
    } elsif ( m{^MOSH GRAPHICS } ) {
      die "Bad MOSH GRAPHICS string: $_\n" unless m{^MOSH GRAPHICS sixel-state-v1\s*$};
      $server_sixel_state = 1;
    } elsif ( m{^MOSH CLIPBOARD } ) {
      die "Bad MOSH CLIPBOARD string: $_\n" unless m{^MOSH CLIPBOARD osc5522-v1\s*$};
      $server_clipboard = 1;
    } elsif ( m{^MOSH DOWNLOADS } ) {
      die "Bad MOSH DOWNLOADS string: $_\n" unless m{^MOSH DOWNLOADS goblin-download-v2\s*$};
      $server_downloads = 1;
    } elsif ( m{^MOSH RELAY-MTU } ) {
      die "Bad MOSH RELAY-MTU string.\n" unless m{^MOSH RELAY-MTU 1 ([1-4])\s*$};
      $server_relay_hops = $1;
    } elsif ( m{^MOSH LINK } ) {
      die "Bad MOSH LINK string: $_\n" unless m{^MOSH LINK budget-v1\s*$};
      $server_link_budget = 1;
    } elsif ( m{^MOSH DIRECTORY } ) {
      die "Bad MOSH DIRECTORY string: $_\n" unless m{^MOSH DIRECTORY directory-v([12])\s*$};
      $server_directory = $1;
    } elsif ( m{^MOSH FILES } ) {
      die "Bad MOSH FILES string: $_\n" unless m{^MOSH FILES file-sync-v1\s*$};
      $server_files = 1;
    } elsif ( m{^MOSH TMUX } ) {
      die "Bad MOSH TMUX string: $_\n" unless m{^MOSH TMUX control-v1\s*$};
      $server_tmux_control = 1;
    } elsif ( m{^MOSH CRYPTO } ) {
      ( $server_crypto ) = m{^MOSH CRYPTO (\S+)\s*$} or die "Bad MOSH CRYPTO string: $_\n";
      if ( $server_crypto ne 'aes128-gcm-v1' ) {
	die "Unsupported MOSH CRYPTO suite: $server_crypto\n";
      }
    } elsif ( m{^MOSH CONNECT } ) {
      if ( ( $port, $key ) = m{^MOSH CONNECT (\d+?) ([A-Za-z0-9/+]{22})\s*$} ) {
	last LINE;
      } else {
	die "Bad MOSH CONNECT string: $_\n";
      }
    } else {
      if ( defined $port_request and $port_request =~ m{:} and m{Bad UDP port} ) {
	$bad_udp_port_warning = 1;
      }
      print "$_\n";
    }
  }
  close $pipe;
  waitpid $pid, 0;

  if ( not defined $ip ) {
    if ( defined $sship ) {
      warn "$0: Using remote IP address ${sship} from \$SSH_CONNECTION for hostname ${userhost}\n";
      $ip = $sship;
    } else {
      die "$0: Did not find remote IP address (is SSH ProxyCommand disabled?).\n";
    }
  }

  if ( not defined $key or not defined $port ) {
    if ( $bad_udp_port_warning ) {
      die "$0: Server does not support UDP port range option.\n";
    }
    die "$0: Did not find goblin-skiff server startup message. (Have you installed goblin-skiff on your server?)\n";
  }

  if ( $fips_crypto and ( not defined $server_crypto or $server_crypto ne 'aes128-gcm-v1' ) ) {
    die "$0: remote server did not confirm the requested FIPS crypto suite.\n";
  }
  if ( ( defined $lossy_quality || $djvu_lossy ) && !$server_image_encoding ) {
    die "$0: remote server does not support the requested image encoding options; update goblin-skiff-server.\n";
  }
  if ( not $fips_crypto and defined $server_crypto ) {
    die "$0: remote server selected a crypto suite that was not requested.\n";
  }

  if ( $server_relay_hops != scalar @jumps ) {
    die "$0: destination did not confirm the UDP relay MTU budget; update goblin-skiff-server.\n";
  }
  if ( @client_version && @server_version && $client_version[0] != $server_version[0] ) {
    die "$0: Incompatible Goblin Skiff protocols: client $client_version[1] uses $client_version[0]; " .
      "server $server_version[1] uses $server_version[0].\n";
  }
  my $client_release = @client_version ? $client_version[1] : 'unversioned';
  my $server_release = @server_version ? $server_version[1] : 'unversioned';
  my $protocol = @client_version && @server_version ? $client_version[0] : 'unversioned peer';
  print STDERR "[goblin-skiff client $client_release; server $server_release; protocol $protocol]\n";
  if ( @jumps ) {
    # An explicit bind address supersedes the SSH interface on a multihomed
    # destination. Relays resolve no destination names and cannot be retargeted.
    $ip = $bind_ip if defined $bind_ip && $bind_ip !~ /\A(?:ssh|any)\z/i;
    my @keys;
    for ( my $hop = $#jumps; $hop >= 0; --$hop ) {
      my ( $relay_ip, $relay_port, $relay_key ) = start_udp_relay( $hop, $ip, $port );
      ( $ip, $port ) = ( $relay_ip, $relay_port );
      unshift @keys, $relay_key;
    }
    $ENV{ 'GOBLIN_SKIFF_RELAY_KEYS' } = join ',', @keys;
    warn "$0: Skiff UDP route: " . join( ' -> ', @jumps, $userhost ) . "\n";
  }

  # Now start real goblin-skiff client
  if ( $tmux_control != $server_tmux_control ) {
    die "$0: remote server did not negotiate the requested tmux control mode.\n";
  }
  $ENV{ 'MOSH_KEY' } = $key;
  $ENV{ 'GOBLIN_SKIFF_MASCOT' } = $mascot;
  $ENV{ 'GOBLIN_SKIFF_DIRECTORY' } = $server_directory;
  $ENV{ 'GOBLIN_SKIFF_FILES' } = $server_files;
  $ENV{ 'GOBLIN_SKIFF_LINK_BUDGET' } = $server_link_budget;
  $ENV{ 'GOBLIN_SKIFF_SIXEL_STATE' } = $server_sixel_state ? '1' : '0';
  $ENV{ 'GOBLIN_SKIFF_CLIPBOARD' } = $server_clipboard ? '1' : '0';
  $ENV{ 'GOBLIN_SKIFF_CLIPBOARD_FAST_THRESHOLD' } = $clipboard_fast_threshold;
  $ENV{ 'GOBLIN_SKIFF_DOWNLOADS' } = $server_downloads && !$no_downloads ? '1' : '0';
  $ENV{ 'GOBLIN_SKIFF_DOWNLOAD_DIR' } = $download_directory if defined $download_directory;
  $ENV{ 'MOSH_PREDICTION_DISPLAY' } = $predict;
  $ENV{ 'MOSH_NO_TERM_INIT' } = '1' if !$term_init;
  $ENV{ 'MOSH_STREAM_DELAY' } = $stream_delay if defined $stream_delay;
  $ENV{ 'MOSH_STREAM_BANDWIDTH' } = $stream_bandwidth if defined $stream_bandwidth;
  $ENV{ 'MOSH_STATE_ZSTD_DICT' } = $state_zstd_dict if defined $state_zstd_dict;
  $ENV{ 'GOBLIN_SKIFF_STATE_SAMPLE_LOG' } = $state_sample_log if defined $state_sample_log;
  $ENV{ 'GOBLIN_SKIFF_STATE_SAMPLE_MIN_SIZE' } = $state_sample_min_size if defined $state_sample_min_size;
  $ENV{ 'GOBLIN_SKIFF_COMPACT_KEEPALIVE' } = '1' if $compact_keepalive;
  my @client_forwarding;
  for ( @local_forwards ) {
    push @client_forwarding, ( '-L', $_ );
  }
  for ( @dynamic_forwards ) {
    push @client_forwarding, ( '-D', $_ );
  }
  if ( $agent_forwarding ) {
    push @client_forwarding, '-A';
  }
  if ( $x11_forwarding ) {
    push @client_forwarding, '-X';
  }
  my @client_crypto = $fips_crypto ? ( '--fips-crypto' ) : ();
  push @client_crypto, '--tmux-control' if $tmux_control;
  push @client_crypto, '--no-kitty' if $no_kitty;
  push @client_crypto, '--no-sixel' if $no_sixel;
  push @client_crypto, '--udp-relay' if @jumps;
  push @client_crypto, "--socks5-proxy=$socks5_proxy" if defined $socks5_proxy;
  exec {$client} ("$client", "-# @cmdline |", @client_crypto, @client_forwarding, $ip, $port);
}

sub shell_quote { join ' ', map {(my $a = $_) =~ s/'/'\\''/g; "'$a'"} @_ }

sub parse_connection_version {
  my ( $line ) = @_;
  my @fields = $line =~ /\AMOSH VERSION ([1-9][0-9]{0,9}) ([A-Za-z0-9._+-]{1,128}) ([A-Za-z0-9._+ -]{1,128})\s*\z/;
  die "Invalid MOSH VERSION message.\n" unless @fields && $fields[0] <= 4294967295;
  return @fields;
}

sub shell_assign {
  my ( $name, $value ) = @_;
  return $name . "=" . shell_quote( $value );
}

sub socks_read {
  my ( $sock, $length ) = @_;
  my $bytes = '';
  while ( length( $bytes ) < $length ) {
    my $count = sysread( $sock, my $part, $length - length( $bytes ) );
    next if !defined( $count ) && $! == EINTR;
    die "$0: SOCKS5 proxy closed during handshake.\n" unless defined( $count ) && $count > 0;
    $bytes .= $part;
  }
  return $bytes;
}

sub socks_write {
  my ( $sock, $bytes ) = @_;
  my $offset = 0;
  while ( $offset < length $bytes ) {
    my $count = syswrite( $sock, $bytes, length( $bytes ) - $offset, $offset );
    next if !defined( $count ) && $! == EINTR;
    die "$0: SOCKS5 handshake write failed.\n" unless defined( $count ) && $count > 0;
    $offset += $count;
  }
}

sub socks_connect {
  my ( $sock, $host, $port ) = @_;
  die "$0: invalid SOCKS5 destination.\n" unless defined $host && defined $port
    && $host =~ /\A[A-Za-z0-9_.:\[\]-]{1,255}\z/ && $port =~ /\A[0-9]{1,5}\z/ && $port > 0 && $port <= 65535;
  $host =~ s/\A\[(.*)\]\z/$1/;
  my $ipv4 = eval { Socket::inet_pton( Socket::AF_INET(), $host ) };
  my $ipv6 = eval { Socket::inet_pton( Socket::AF_INET6(), $host ) };
  my $address = defined( $ipv4 ) ? "\1$ipv4" : defined( $ipv6 ) ? "\4$ipv6" : pack( 'CC', 3, length $host ) . $host;
  local $SIG{ALRM} = sub { die "$0: SOCKS5 SSH handshake timed out.\n"; };
  alarm 30;
  socks_write( $sock, "\5\1\0" );
  die "$0: SOCKS5 proxy must support no-authentication mode.\n" unless socks_read( $sock, 2 ) eq "\5\0";
  socks_write( $sock, "\5\1\0" . $address . pack( 'n', $port ) );
  my ( $version, $reply, $reserved, $type ) = unpack( 'CCCC', socks_read( $sock, 4 ) );
  die "$0: SOCKS5 CONNECT failed (reply $reply).\n" unless $version == 5 && $reply == 0 && $reserved == 0;
  my $length = $type == 1 ? 4 : $type == 4 ? 16 : $type == 3 ? unpack( 'C', socks_read( $sock, 1 ) ) : 0;
  die "$0: SOCKS5 proxy sent an invalid bound address.\n" unless $length;
  socks_read( $sock, $length + 2 ); # CONNECT BND.ADDR is not the destination!
  alarm 0;
}

sub socks_proxy_command {
  my ( $hops, $report ) = @_;
  my $command = shell_quote( $0, "--socks5-proxy=$socks5_proxy", "--family=$family",
                              '--fake-proxy', $report && !$hops ? '--proxy-report' : '--no-proxy-report', '--', '%h', '%p' );
  for ( my $hop = 0; $hop < $hops; ++$hop ) {
    my ( $host, $port ) = parse_jump( $jumps[$hop] );
    # Each surrounding OpenSSH expands percent tokens once. Inner helpers
    # must see their own hop's %h/%p, not the final destination's address.
    $command =~ s/%/%%/g;
    my @args = ( $jump_ssh[0], '-o', "ProxyCommand=$command", '-S', 'none',
                 '-o', 'ControlMaster=no', '-o', 'ClearAllForwardings=yes', @jump_ssh[1 .. $#jump_ssh] );
    push @args, '-p', $port if defined $port;
    push @args, '-W', '[%h]:%p', $host;
    $command = shell_quote( @args );
  }
  return $command;
}

sub ssh_configuration {
  my @args = @_;
  open my $config, '-|', @args or die "$0: cannot inspect SSH configuration: $!\n";
  my %values;
  while ( <$config> ) {
    if ( /\A(hostname|proxyjump)\s+(.+?)\s*\z/i ) { $values{lc $1} = $2; }
  }
  close $config or die "$0: SSH configuration lookup failed (--ssh must support OpenSSH -G).\n";
  die "$0: SSH did not return its configuration (--ssh must support OpenSSH -G).\n" unless $values{hostname};
  return %values;
}

sub parse_jump {
  my ( $spec ) = @_;
  my ( $user, $ipv6, $host, $port ) = $spec =~
    /\A(?:([A-Za-z0-9_.-]+)\@)?(?:\[([A-Fa-f0-9:.%a-zA-Z_-]+)\]|([A-Za-z0-9_][A-Za-z0-9_.-]*))(?::([0-9]+))?\z/;
  die "$0: invalid jump host '$spec'; use [user\@]host[:port] (bracket IPv6).\n"
    unless defined $host || defined $ipv6;
  die "$0: invalid jump SSH port.\n" if defined $port && ( $port < 1 || $port > 65535 );
  return ( ( defined $user ? "$user\@" : '' ) . ( defined $ipv6 ? $ipv6 : $host ), $port );
}

sub start_udp_relay {
  my ( $hop, $target_ip, $target_port ) = @_;
  my ( $host, $ssh_port ) = parse_jump( $jumps[$hop] );
  my @args = ( @jump_ssh, '-n', '-T', '-S', 'none', '-o', 'ControlMaster=no', '-o', 'ClearAllForwardings=yes' );
  push @args, ( '-p', $ssh_port ) if defined $ssh_port;
  if ( $hop ) {
    push @args, ( '-J', join( ',', @jumps[0 .. $hop - 1] ) );
    push @args, '-4' if $family eq 'inet';
    push @args, '-6' if $family eq 'inet6';
  } elsif ( !defined $socks5_proxy ) {
    # Capture the address actually used by SSH, honoring HostName and address
    # family selection. The SSH-facing interface may be private behind NAT.
    my $proxy = shell_quote( $0, "--family=$family" );
    push @args, ( '-o', "ProxyCommand=$proxy --fake-proxy -- %h %p" );
  }
  my @relay = ( 'relay', "--idle-timeout=$jump_idle_timeout" );
  push @relay, '--fips-crypto' if $fips_crypto;
  push @relay, "--port=$jump_port" if defined $jump_port;
  push @relay, '--', $target_ip, $target_port;
  push @args, $host, '--', $jump_server . ' ' . shell_quote( @relay );
  if ( defined $socks5_proxy ) {
    splice @args, 1, 0, '-o', 'ProxyCommand=' . socks_proxy_command( $hop, !$hop );
  }
  my $pid = open( my $relay, '-|' );
  die "$0: relay fork: $!\n" unless defined $pid;
  if ( !$pid ) {
    open STDERR, '>&STDOUT' or die;
    $ENV{SHELL} = '/bin/sh' if !$hop || defined $socks5_proxy;
    exec @args;
    die "$0: cannot start jump SSH: $!\n";
  }
  my ( $external_ip, $ip, $port, $key );
  my $suite = $fips_crypto ? 'aes128-gcm-v1' : 'ocb-aes128';
  while ( <$relay> ) {
    if ( /\AMOSH IP (\S+)\s*\z/ ) {
      die "$0: duplicate jump IP.\n" if defined $external_ip;
      $external_ip = $1;
    } elsif ( /\AMOSH RELAY / ) {
      die "$0: jump $jumps[$hop] returned an invalid relay handshake.\n"
        if defined $key || !/\AMOSH RELAY 1 \Q$suite\E (\S+) ([0-9]+) ([A-Za-z0-9\/+]{22})\s*\z/;
      ( $ip, $port, $key ) = ( $1, $2, $3 );
      die "$0: invalid jump UDP port.\n" if $port < 1 || $port > 65535;
    } else { print; }
  }
  close $relay or die "$0: jump $jumps[$hop] failed to start its UDP relay.\n";
  die "$0: jump $jumps[$hop] needs an updated goblin-skiff-server with UDP relay support.\n" unless defined $key;
  die "$0: could not discover the client-facing jump IP.\n" if !$hop && !defined $external_ip;
  return ( $hop ? $ip : $external_ip, $port, $key );
}

sub upload_state_dictionary {
  my ( $path, $userhost, $family, $use_remote_ip ) = @_;

  open my $dict_fh, '<:raw', $path or die "$0: cannot open $path: $!\n";
  local $/ = undef;
  my $dictionary = <$dict_fh>;
  close $dict_fh or die "$0: cannot close $path: $!\n";

  my @upload_ssh = ( @ssh, '-T' );
  if ( $use_remote_ip eq 'remote' ) {
    if ( $family eq 'inet' ) {
      push @upload_ssh, '-4';
    } elsif ( $family eq 'inet6' ) {
      push @upload_ssh, '-6';
    }
  } elsif ( $use_remote_ip eq 'proxy' && !defined $socks5_proxy ) {
    my $quoted_proxy_command = shell_quote( $0, "--family=$family" );
    push @upload_ssh, ( '-S', 'none', '-o', "ProxyCommand=$quoted_proxy_command --fake-proxy -- %h %p" );
  }

  if ( defined $socks5_proxy ) {
    push @upload_ssh, '-S', 'none';
    splice @upload_ssh, 1, 0, '-S', 'none', '-o', 'ControlMaster=no',
      '-o', 'ProxyCommand=' . socks_proxy_command( scalar @jumps, 0 );
  }

  my $script = 'tmp=$(mktemp "${TMPDIR:-/tmp}/goblin-skiff-zstd-dict.XXXXXX") || exit 1; '
    . 'chmod 600 "$tmp" || exit 1; '
    . 'cat > "$tmp" || exit 1; '
    . 'printf "MOSH DICT %s\n" "$tmp"';

  my $remote_path;
  {
    local $ENV{ 'SHELL' } = $ENV{ 'SHELL' };
    $ENV{ 'SHELL' } = '/bin/sh' if $use_remote_ip eq 'proxy' || defined $socks5_proxy;

    my $errfh = gensym;
    my $pid = open3( my $in, my $out, $errfh, @upload_ssh, $userhost, '--', 'sh -c ' . shell_quote( $script ) );
    binmode( $in );
    print {$in} $dictionary;
    close $in or die "$0: failed writing zstd dictionary to ssh: $!\n";

    local $/ = undef;
    my $stdout = <$out>;
    my $stderr = <$errfh>;
    $stdout = "" if not defined $stdout;
    $stderr = "" if not defined $stderr;
    waitpid $pid, 0;
    if ( $? != 0 ) {
      die "$0: failed to upload zstd dictionary over ssh" . ( length $stderr ? ": $stderr" : "\n" );
    }

    ( $remote_path ) = $stdout =~ m{^MOSH DICT (.+)\s*$}m;
    die "$0: bad zstd dictionary upload response: $stdout\n" if not defined $remote_path;
  }
  return $remote_path;
}

sub server_command_string {
  my ( $server, @server_args ) = @_;
  my $command = server_environment_prefix() . "$server " . shell_quote( @server_args );
  if ( $uploaded_state_zstd_dict ) {
    my $cleanup = "rc=\$?; rm -f " . shell_quote( $remote_state_zstd_dict ) . "; exit \$rc";
    return "sh -c " . shell_quote( "$command; $cleanup" );
  }
  return $command;
}

sub server_environment_prefix {
  my @assignments;
  my @client_capabilities = ( "keepalive-v1", "directory-v1", "directory-v2", "file-sync-v1", "link-budget-v1", "sixel-state-v1", "osc5522-v1" );
  push @client_capabilities, "palette-djvu-v1" if $client_palette_djvu;
  push @client_capabilities, "goblin-download-v2" unless $no_downloads;
  push @client_capabilities, "fips-aes128-gcm-v1" if $fips_crypto;
  push @client_capabilities, "tmux-control-v1" if $tmux_control;
  push @client_capabilities, "udp-relay-v1" if @jumps;
  push @assignments, shell_assign( "MOSH_RELAY_HOPS", scalar @jumps ) if @jumps;
  push @assignments, shell_assign( "MOSH_CLIENT_CAPS", join( ',', @client_capabilities ) );
  push @assignments, shell_assign( "MOSH_CLIENT_TERM", $client_term ) if defined $client_term and length $client_term;
  push @assignments, shell_assign( "MOSH_STREAM_DELAY", $stream_delay ) if defined $stream_delay;
  push @assignments, shell_assign( "MOSH_STREAM_BANDWIDTH", $stream_bandwidth ) if defined $stream_bandwidth;
  push @assignments, shell_assign( "MOSH_STATE_ZSTD_DICT", $remote_state_zstd_dict ) if defined $remote_state_zstd_dict;
  push @assignments, shell_assign( "MOSH_STATE_ZSTD_DICT_UNLINK", 1 ) if $uploaded_state_zstd_dict;
  return "" if not @assignments;
  return join( ' ', @assignments ) . " ";
}

sub locale_vars {
  my @names = qw[LANG LANGUAGE LC_CTYPE LC_NUMERIC LC_TIME LC_COLLATE LC_MONETARY LC_MESSAGES LC_PAPER LC_NAME LC_ADDRESS LC_TELEPHONE LC_MEASUREMENT LC_IDENTIFICATION LC_ALL];

  my @assignments;

  for ( @names ) {
    if ( defined $ENV{ $_ } ) {
      push @assignments, $_ . q{=} . $ENV{ $_ };
    }
  }

  return @assignments;
}

sub resolvename {
  my ( $host, $port, $family ) = @_;
  my $err;
  my @res;
  my $af;

  # If the user selected a specific family, parse it.
  if ( defined( $family ) && ( $family eq 'inet' || $family eq 'inet6' )) {
      # Choose an address family, or cause pain.
      my $afstr = 'AF_' . uc( $family );
      $af = eval { IO::Socket->$afstr } or die "$0: Invalid family $family\n";
  }

  # First try the address as a numeric.
  my %hints = ( flags => AI_NUMERICHOST,
		socktype => SOCK_STREAM,
		protocol => IPPROTO_TCP );
  if ( defined( $af )) {
    $hints{family} = $af;
  }
  ( $err, @res ) = getaddrinfo( $host, $port, \%hints );
  if ( $err ) {
    # Get canonical name for this host.
    $hints{flags} = AI_CANONNAME;
    ( $err, @res ) = getaddrinfo( $host, $port, \%hints );
    die "$0: could not get canonical name for $host: ${err}\n" if $err;
    # Then get final resolution of the canonical name.
    delete $hints{flags};
    my @newres;
    ( $err, @newres ) = getaddrinfo( $res[0]{canonname}, $port, \%hints );
    die "$0: could not resolve canonical name ${res[0]{canonname}} for ${host}: ${err}\n" if $err;
    @res = @newres;
  }

  if ( defined( $af )) {
    # If v4 or v6 was specified, reduce the host list.
    @res = grep {$_->{family} == $af} @res;
  } elsif ( $family =~ /^prefer-/ ) {
    # If prefer-* was specified, reorder the host list to put that family first.
    my $prefer_afstr = 'AF_' . uc( ($family =~ /prefer-(.*)/)[0] );
    my $prefer_af = eval { IO::Socket->$prefer_afstr } or die "$0: Invalid preferred family $family\n";
    @res = (grep({$_->{family} == $prefer_af} @res), grep({$_->{family} != $prefer_af} @res));
  } elsif ( $family ne 'all' ) {
    # If v4/v6/all were not specified, verify that this host only has one address family available.
    for my $ai ( @res ) {
      if ( !defined( $af )) {
	$af = $ai->{family};
      } else {
	die "$0: host has both IPv4 and IPv6 addresses, use --family to specify behavior\n"
	    if $af != $ai->{family};
      }
    }
  }
  return @res;
}
