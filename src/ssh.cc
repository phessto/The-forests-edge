/*
 *   SSH LISTENER
 *
 *   Lets players reach the normal login over SSH instead of telnet.  The
 *   SSH layer (libssh, non-blocking) lives entirely in this file.  Each
 *   SSH session that opens a shell channel gets a socketpair: one end is
 *   handed to the game as an ordinary link_data channel (so login, ban,
 *   who, idle and linkdead logic are exactly the telnet code paths), the
 *   other end is pumped to and from the SSH channel once per pulse.
 *
 *   SSH authentication is "none" for one configured user name; the real
 *   authentication is still the in-game account password, which is now
 *   encrypted in transit.
 */

#include <string>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>
#include "define.h"
#include "struct.h"


int ssh_port = 0;

static int          ssh_socket     = -1;
static ssh_bind     ssh_binder     = 0;
static ssh_event    ssh_events     = 0;
static std::string  ssh_user       = "play";
static int          ssh_max        = 32;
static int          ssh_grace      = 30;     // seconds to reach the game
static int          ssh_count      = 0;

static const size_t TO_GAME_MAX    = 16384;
static const size_t TO_CLIENT_MAX  = 16384;
static const size_t LINE_MAX       = 400;

// Algorithm lists: modern AEAD/ETM only.  Every entry exists in libssh
// 0.9 and 0.10, and in any OpenSSH client from the last decade.
static const char* const KEX_ALGOS =
  "curve25519-sha256,curve25519-sha256@libssh.org,"
  "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
  "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"
  "diffie-hellman-group14-sha256";
static const char* const CIPHER_ALGOS =
  "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,"
  "aes128-gcm@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr";
static const char* const MAC_ALGOS =
  "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
  "hmac-sha2-256,hmac-sha2-512";
static const char* const HOSTKEY_ALGOS =
  "ssh-ed25519,rsa-sha2-512,rsa-sha2-256";


enum { SSH_ST_KEX, SSH_ST_UP, SSH_ST_CLOSING, SSH_ST_DEAD };


struct ssh_conn
{
  ssh_conn*      next;
  ssh_session    session;
  ssh_channel    channel;
  struct ssh_server_callbacks_struct   scb;
  struct ssh_channel_callbacks_struct  ccb;
  int            state;
  int            fd;            // our end of the socketpair, -1 until shell
  time_t         deadline;
  in_addr        addr;
  char           ip [ INET_ADDRSTRLEN ];
  bool           in_event;
  bool           pty;
  bool           shell;
  bool           echo_off;      // game sent IAC WILL ECHO
  bool           last_cr;       // swallow LF/NUL after CR on input
  bool           last_nl;       // swallow CR after LF on output
  int            esc;           // input escape-sequence state
  int            iac;           // output telnet state
  unsigned char  iac_cmd;
  std::string    line;          // pty line being edited
  std::string    to_game;
  std::string    to_client;
};


static ssh_conn* conn_list = 0;


// The journal gets stdout; flush so lines show up when they happen.
static void sshlog( const char* fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));
static void sshlog( const char* fmt, ... )
{
  va_list args;
  va_start( args, fmt );
  vprintf( fmt, args );
  va_end( args );
  fflush( stdout );
}


/*
 *   HOST KEYS
 */


static bool load_or_make_key( const char* dir, const char* name,
			      enum ssh_keytypes_e type, int bits )
{
  char path [ 512 ];
  snprintf( path, sizeof( path ), "%s/%s", dir, name );

  ssh_key key = 0;
  struct stat st;

  if( stat( path, &st ) == 0 ) {
    if( ssh_pki_import_privkey_file( path, 0, 0, 0, &key ) != SSH_OK ) {
      sshlog( "SSH: Cannot read host key %s; fix or remove it.\n", path );
      return false;
    }
    if( ( st.st_mode & 077 ) != 0 ) {
      sshlog( "SSH: Host key %s was mode %03o, setting 0600.\n",
	      path, (unsigned) ( st.st_mode & 0777 ) );
      chmod( path, 0600 );
    }
  } else {
    sshlog( "SSH: Generating %s host key %s ...\n", name, path );
    if( ssh_pki_generate( type, bits, &key ) != SSH_OK ) {
      sshlog( "SSH: Key generation failed.\n" );
      return false;
    }
    const mode_t old = umask( 077 );
    const int rc = ssh_pki_export_privkey_file( key, 0, 0, 0, path );
    umask( old );
    if( rc != SSH_OK ) {
      sshlog( "SSH: Cannot write host key %s.\n", path );
      ssh_key_free( key );
      return false;
    }
    chmod( path, 0600 );
  }

  unsigned char *hash = 0;
  size_t hlen = 0;
  ssh_key pub = 0;
  if( ssh_pki_export_privkey_to_pubkey( key, &pub ) == SSH_OK
      && ssh_get_publickey_hash( pub, SSH_PUBLICKEY_HASH_SHA256,
				 &hash, &hlen ) == 0 ) {
    char *fp = ssh_get_fingerprint_hash( SSH_PUBLICKEY_HASH_SHA256, hash, hlen );
    sshlog( "SSH: Host key %s %s\n", ssh_key_type_to_char( ssh_key_type( key ) ),
	    fp ? fp : "?" );
    ssh_string_free_char( fp );
    ssh_clean_pubkey_hash( &hash );
  }
  ssh_key_free( pub );
  ssh_key_free( key );

  if( ssh_bind_options_set( ssh_binder, SSH_BIND_OPTIONS_HOSTKEY, path ) < 0 ) {
    sshlog( "SSH: libssh rejected host key %s: %s\n", path,
	    ssh_get_error( ssh_binder ) );
    return false;
  }

  return true;
}


/*
 *   CONFIGURATION
 *
 *   Each setting comes from, in order of precedence: the command line,
 *   the environment, the default.  A bad value stops the boot (see
 *   check_ssh_config) with a message naming it; nothing falls back silently.
 */


static std::string  ssh_keydir     = "../ssh";


struct ssh_setting
{
  const char*  option;    // command line, takes a value
  const char*  env;
  const char*  what;
};


static const ssh_setting settings [] = {
  { "--ssh-port",          "TFE_SSH_PORT",          "port: 1-65535, or 0/off to disable" },
  { "--ssh-user",          "TFE_SSH_USER",          "user: 1-32 of A-Z a-z 0-9 . _ -, or *" },
  { "--ssh-max-sessions",  "TFE_SSH_MAX_SESSIONS",  "max sessions: 1-1024" },
  { "--ssh-hostkey-dir",   "TFE_SSH_HOSTKEY_DIR",   "host key directory: a path" },
  { "--ssh-login-timeout", "TFE_SSH_LOGIN_TIMEOUT", "login timeout: 5-600 seconds" },
};


static std::string  config_error;   // first bad setting, reported at boot


static void bad_setting( const char* source, const char* name,
			 const char* value, const char* what )
{
  if( !config_error.empty( ) )
    return;
  char tmp [ 512 ];
  snprintf( tmp, sizeof( tmp ), "Bad %s %s='%s' (expected %s).",
	    source, name, value, what );
  config_error = tmp;
}


static bool parse_int( const char* value, int low, int high, int& out )
{
  if( !*value )
    return false;
  char *end;
  errno = 0;
  const long n = strtol( value, &end, 10 );
  if( errno || *end || n < low || n > high )
    return false;
  out = (int) n;
  return true;
}


static void apply_setting( int i, const char* source, const char* name,
			   const char* value )
{
  const char *what = settings[i].what;
  int n;

  switch( i ) {
  case 0:
    if( !strcasecmp( value, "off" ) || !strcasecmp( value, "no" )
	|| !strcmp( value, "0" ) ) {
      ssh_port = 0;
    } else if( parse_int( value, 1, 65535, n ) ) {
      ssh_port = n;
    } else {
      bad_setting( source, name, value, what );
    }
    break;

  case 1: {
    const size_t len = strlen( value );
    bool ok = ( len >= 1 && len <= 32 );
    if( ok && strcmp( value, "*" ) ) {
      for( const char *c = value; *c; ++c )
	if( !isalnum( (unsigned char) *c ) && !strchr( "._-", *c ) )
	  ok = false;
    }
    if( !ok ) {
      bad_setting( source, name, value, what );
      return;
    }
    ssh_user = value;
    break;
  }

  case 2:
    if( !parse_int( value, 1, 1024, n ) ) {
      bad_setting( source, name, value, what );
      return;
    }
    ssh_max = n;
    break;

  case 3:
    if( !*value ) {
      bad_setting( source, name, value, what );
      return;
    }
    ssh_keydir = value;
    break;

  case 4:
    if( !parse_int( value, 5, 600, n ) ) {
      bad_setting( source, name, value, what );
      return;
    }
    ssh_grace = n;
    break;
  }
}


/* Reads the environment, then the command line (which wins), and removes
   the SSH options from argv so the game's own argument parsing is
   unchanged.  --no-ssh disables the listener whatever else is set. */
void configure_ssh( int& argc, char** argv )
{
  const int count = sizeof( settings )/sizeof( settings[0] );

  for( int i = 0; i < count; ++i ) {
    const char *env = getenv( settings[i].env );
    if( env && *env )   // set-but-empty counts as unset, as in tfe.env
      apply_setting( i, "environment variable", settings[i].env, env );
  }

  bool no_ssh = false;

  for( int a = 1; a < argc; ) {
    const char *arg = argv[a];
    int used = 0;

    if( !strcmp( arg, "--no-ssh" ) ) {
      no_ssh = true;
      used = 1;
    } else if( !strncmp( arg, "--ssh-", 6 ) ) {
      for( int i = 0; i < count && !used; ++i ) {
	const size_t len = strlen( settings[i].option );
	if( strncmp( arg, settings[i].option, len ) )
	  continue;
	if( arg[len] == '=' ) {
	  apply_setting( i, "option", settings[i].option, arg+len+1 );
	  used = 1;
	} else if( arg[len] == '\0' ) {
	  if( a+1 >= argc ) {
	    bad_setting( "option", settings[i].option, "", settings[i].what );
	    used = 1;
	  } else {
	    apply_setting( i, "option", settings[i].option, argv[a+1] );
	    used = 2;
	  }
	}
      }
      if( !used ) {
	if( config_error.empty( ) )
	  config_error = std::string( "Unknown option " ) + arg + ".";
	used = 1;
      }
    }

    if( !used ) {
      ++a;
      continue;
    }

    for( int j = a; j+used <= argc; ++j )
      argv[j] = argv[j+used];
    argc -= used;
  }

  if( no_ssh )
    ssh_port = 0;
}


/* Called once the bug log is open, so a bad setting shows in bugs.txt (and
   tfe-run's failed-boot excerpt) as well as on stderr (the journal). */
void check_ssh_config( )
{
  if( config_error.empty( ) )
    return;
  fflush( stdout );
  fprintf( stderr, "TFE: %s\n", config_error.c_str( ) );
  bug( -1, config_error.c_str( ) );
  bug( -1, "** MUD KILLED **" );
  exit( 1 );
}


/*
 *   STARTUP
 */


void open_ssh( int game_port )
{
  if( ssh_port <= 0 ) {
    sshlog( "SSH: Disabled (set TFE_SSH_PORT or --ssh-port to enable).\n" );
    return;
  }

  if( ssh_port == game_port || ssh_port == game_port+1 ) {
    char tmp [ 100 ];
    snprintf( tmp, sizeof( tmp ), "SSH port %d clashes with the game ports %d/%d.",
	      ssh_port, game_port, game_port+1 );
    fprintf( stderr, "TFE: %s\n", tmp );
    panic( "Open_Ssh: %s", tmp );
  }

  const char *dir = ssh_keydir.c_str( );

  struct stat st;
  if( mkdir( dir, 0700 ) == 0 ) {
    sshlog( "SSH: Created host key directory %s.\n", dir );
  } else if( stat( dir, &st ) == 0 && S_ISDIR( st.st_mode )
	     && ( st.st_mode & 077 ) && st.st_uid == geteuid( ) ) {
    sshlog( "SSH: Host key directory %s was mode %03o, setting 0700.\n",
	    dir, (unsigned) ( st.st_mode & 0777 ) );
    chmod( dir, 0700 );
  }

  if( ssh_init( ) != SSH_OK )
    panic( "Open_Ssh: ssh_init failed." );

  ssh_binder = ssh_bind_new( );
  if( !ssh_binder )
    panic( "Open_Ssh: ssh_bind_new failed." );

  if( !load_or_make_key( dir, "ssh_host_ed25519_key", SSH_KEYTYPE_ED25519, 0 )
      || !load_or_make_key( dir, "ssh_host_rsa_key", SSH_KEYTYPE_RSA, 3072 ) )
    panic( "Open_Ssh: Host keys unavailable in %s.", dir );

  // libssh applies the system-wide server config (/etc/libssh/
  // libssh_server.config, the crypto policy on Fedora/RHEL) at the first
  // accept, on top of anything set before.  Load it now so the lists below
  // are what actually gets offered.
  if( ssh_bind_options_parse_config( ssh_binder, 0 ) != 0 )
    sshlog( "SSH: Could not read the system libssh server config; ignoring it.\n" );

  struct { enum ssh_bind_options_e opt; const char *val; } opts [] = {
    { SSH_BIND_OPTIONS_KEY_EXCHANGE,       KEX_ALGOS },
    { SSH_BIND_OPTIONS_CIPHERS_C_S,        CIPHER_ALGOS },
    { SSH_BIND_OPTIONS_CIPHERS_S_C,        CIPHER_ALGOS },
    { SSH_BIND_OPTIONS_HMAC_C_S,           MAC_ALGOS },
    { SSH_BIND_OPTIONS_HMAC_S_C,           MAC_ALGOS },
    { SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS, HOSTKEY_ALGOS },
    { SSH_BIND_OPTIONS_BANNER,             "TFE" },
  };
  for( size_t i = 0; i < sizeof( opts )/sizeof( opts[0] ); ++i )
    if( ssh_bind_options_set( ssh_binder, opts[i].opt, opts[i].val ) < 0 )
      panic( "Open_Ssh: Bad libssh option %s: %s.",
	     opts[i].val, ssh_get_error( ssh_binder ) );

  ssh_events = ssh_event_new( );
  if( !ssh_events )
    panic( "Open_Ssh: ssh_event_new failed." );

  ssh_socket = open_port( ssh_port );
  fcntl( ssh_socket, F_SETFL, O_NDELAY );

  sshlog( "SSH: Listening on port %d for user '%s' (max %d sessions).\n",
	  ssh_port, ssh_user.c_str( ), ssh_max );
  fflush( stdout );
}


/*
 *   CALLBACKS
 */


static int auth_none_cb( ssh_session, const char* user, void* data )
{
  ssh_conn *conn = (ssh_conn*) data;

  if( ssh_user == "*" || ssh_user == user )
    return SSH_AUTH_SUCCESS;

  sshlog( "SSH: %s tried user '%s'; only '%s' is accepted.\n",
	  conn->ip, user, ssh_user.c_str( ) );
  return SSH_AUTH_DENIED;
}


static int auth_password_cb( ssh_session, const char*, const char*, void* )
{
  return SSH_AUTH_DENIED;
}


static int auth_pubkey_cb( ssh_session, const char*, struct ssh_key_struct*,
			   char, void* )
{
  return SSH_AUTH_DENIED;
}


static int pty_cb( ssh_session, ssh_channel, const char*, int, int, int, int,
		   void* data )
{
  ssh_conn *conn = (ssh_conn*) data;
  if( conn->shell )
    return -1;
  conn->pty = true;
  return 0;
}


static int resize_cb( ssh_session, ssh_channel, int, int, int, int, void* )
{
  return 0;
}


static void start_game( ssh_conn* );


static int shell_cb( ssh_session, ssh_channel, void* data )
{
  ssh_conn *conn = (ssh_conn*) data;
  if( conn->shell )
    return -1;
  conn->shell = true;
  start_game( conn );
  return conn->fd >= 0 ? 0 : -1;
}


static int deny_exec_cb( ssh_session, ssh_channel, const char*, void* )
{
  return -1;
}


static int deny_env_cb( ssh_session, ssh_channel, const char*, const char*,
			void* )
{
  return -1;
}


static void close_cb( ssh_session, ssh_channel, void* data )
{
  ssh_conn *conn = (ssh_conn*) data;
  conn->state = SSH_ST_DEAD;
}


static void echo( ssh_conn* conn, const char* text, size_t len )
{
  if( !conn->echo_off )
    conn->to_client.append( text, len );
}


static int data_cb( ssh_session, ssh_channel, void* buf, uint32_t len,
		    int is_stderr, void* data )
{
  ssh_conn *conn = (ssh_conn*) data;
  const unsigned char *in = (const unsigned char*) buf;

  if( is_stderr || conn->fd < 0 )
    return len;

  uint32_t i;
  for( i = 0; i < len; ++i ) {
    if( conn->to_game.size( ) >= TO_GAME_MAX
	|| conn->to_client.size( ) >= TO_CLIENT_MAX )
      break;   // leave the rest in libssh until the game catches up

    const unsigned char c = in[i];

    if( conn->last_cr ) {
      conn->last_cr = false;
      if( c == '\n' || c == '\0' )
	continue;
    }

    if( !conn->pty ) {
      // Line-mode client (ssh -T, scripts): the client already edited the
      // line.  Normalise line ends, drop anything the game would not keep.
      if( c == '\r' ) {
	conn->last_cr = true;
	conn->to_game += "\r\n";
      } else if( c == '\n' ) {
	conn->to_game += "\r\n";
      } else if( c >= 0x20 && c < 0x7f ) {
	conn->to_game += (char) c;
      }
      continue;
    }

    // Raw pty: do the line editing and echo a telnet client would.
    if( conn->esc == 1 ) {
      conn->esc = ( c == '[' || c == 'O' ) ? 2 : 0;
      continue;
    }
    if( conn->esc == 2 ) {
      if( c >= 0x40 && c <= 0x7e )
	conn->esc = 0;
      continue;
    }

    switch( c ) {
    case 0x1b:
      conn->esc = 1;
      break;

    case '\r':
    case '\n':
      if( c == '\r' )
	conn->last_cr = true;
      echo( conn, "\r\n", 2 );   // like telnet: not echoed while echo is off
      conn->to_game += conn->line;
      conn->to_game += "\r\n";
      conn->line.clear( );
      break;

    case 0x08:
    case 0x7f:
      if( !conn->line.empty( ) ) {
	conn->line.erase( conn->line.size( )-1 );
	echo( conn, "\b \b", 3 );
      }
      break;

    case 0x15:   // ^U
    case 0x03:   // ^C
      while( !conn->line.empty( ) ) {
	conn->line.erase( conn->line.size( )-1 );
	echo( conn, "\b \b", 3 );
      }
      break;

    default:
      if( c >= 0x20 && c < 0x7f && conn->line.size( ) < LINE_MAX ) {
	conn->line += (char) c;
	const char ch = c;
	echo( conn, &ch, 1 );
      }
      break;
    }
  }

  return i;
}


static ssh_channel channel_open_cb( ssh_session session, void* data )
{
  ssh_conn *conn = (ssh_conn*) data;

  if( conn->channel )
    return 0;   // one game channel per connection

  conn->channel = ssh_channel_new( session );
  if( !conn->channel )
    return 0;

  memset( &conn->ccb, 0, sizeof( conn->ccb ) );
  conn->ccb.userdata                              = conn;
  conn->ccb.channel_data_function                 = data_cb;
  conn->ccb.channel_close_function                = close_cb;
  conn->ccb.channel_pty_request_function          = pty_cb;
  conn->ccb.channel_pty_window_change_function    = resize_cb;
  conn->ccb.channel_shell_request_function        = shell_cb;
  conn->ccb.channel_exec_request_function         = deny_exec_cb;
  conn->ccb.channel_subsystem_request_function    = deny_exec_cb;
  conn->ccb.channel_env_request_function          = deny_env_cb;
  ssh_callbacks_init( &conn->ccb );
  ssh_set_channel_callbacks( conn->channel, &conn->ccb );

  return conn->channel;
}


/*
 *   GAME SIDE
 */


static void start_game( ssh_conn* conn )
{
  int sv [ 2 ];

  if( socketpair( AF_UNIX, SOCK_STREAM, 0, sv ) < 0 ) {
    bug( "Start_Game: socketpair failed." );
    return;
  }

  fcntl( sv[0], F_SETFL, O_NDELAY );
  fcntl( sv[1], F_SETFL, O_NDELAY );
  fcntl( sv[0], F_SETFD, FD_CLOEXEC );
  fcntl( sv[1], F_SETFD, FD_CLOEXEC );

  conn->fd = sv[1];

  // From here on this is an ordinary link: host lookup, ban check,
  // greeting, login, idle and linkdead handling are the telnet code.
  link_data *link = new link_data;
  link->channel   = sv[0];
  link->connected = CON_INTRO;
  write_host( link, conn->addr );
}


static void process_game_output( ssh_conn* conn, const unsigned char* buf,
				 ssize_t len )
{
  for( ssize_t i = 0; i < len; ++i ) {
    const unsigned char c = buf[i];

    switch( conn->iac ) {
    case 1:   // after IAC
      if( c >= 251 && c <= 254 ) {     // WILL WONT DO DONT
	conn->iac_cmd = c;
	conn->iac = 2;
      } else if( c == 250 ) {          // SB
	conn->iac = 3;
      } else {
	conn->iac = 0;
      }
      continue;

    case 2:   // option byte
      if( c == 1 ) {                   // TELOPT_ECHO
	if( conn->iac_cmd == 251 )
	  conn->echo_off = true;
	else if( conn->iac_cmd == 252 )
	  conn->echo_off = false;
      }
      conn->iac = 0;
      continue;

    case 3:   // subnegotiation
      if( c == 255 )
	conn->iac = 4;
      continue;

    case 4:
      conn->iac = ( c == 240 ) ? 0 : 3;
      continue;
    }

    if( c == 255 ) {
      conn->iac = 1;
      continue;
    }

    if( c == '\n' ) {
      conn->to_client += conn->pty ? "\r\n" : "\n";
      conn->last_nl = true;
      continue;
    }

    if( c == '\r' ) {
      // The game ends lines with "\n\r"; that CR was already sent.
      if( conn->pty && !conn->last_nl )
	conn->to_client += '\r';
      conn->last_nl = false;
      continue;
    }

    conn->last_nl = false;
    conn->to_client += (char) c;
  }
}


/*
 *   PER-PULSE PUMP
 */


static void accept_ssh( )
{
  for( int n = 0; n < 8; ++n ) {
    sockaddr_in sin;
    socklen_t len = sizeof( sin );
    const int fd = accept( ssh_socket, (sockaddr*) &sin, &len );

    if( fd < 0 )
      return;

    char ip [ INET_ADDRSTRLEN ];
    inet_ntop( AF_INET, &sin.sin_addr, ip, sizeof( ip ) );

    char tmp [ TWO_LINES ];

    if( is_banned( ip ) ) {
      snprintf( tmp, TWO_LINES, "SSH connection from banned site %s refused.", ip );
      info( LEVEL_DEMIGOD, empty_string, 0, tmp, IFLAG_LOGINS, 3 );
      close( fd );
      continue;
    }

    if( ssh_count >= ssh_max ) {
      snprintf( tmp, TWO_LINES, "SSH connection from %s refused: %d sessions open.",
		ip, ssh_count );
      info( LEVEL_DEMIGOD, empty_string, 0, tmp, IFLAG_LOGINS, 3 );
      sshlog( "%s\n", tmp );
      close( fd );
      continue;
    }

    fcntl( fd, F_SETFL, O_NDELAY );
    fcntl( fd, F_SETFD, FD_CLOEXEC );

    ssh_session session = ssh_new( );
    if( !session ) {
      close( fd );
      return;
    }

    ssh_conn *conn = new ssh_conn;
    conn->session  = session;
    conn->channel  = 0;
    conn->state    = SSH_ST_KEX;
    conn->fd       = -1;
    conn->deadline = current_time + ssh_grace;
    conn->addr     = sin.sin_addr;
    strcpy( conn->ip, ip );
    conn->in_event = false;
    conn->pty      = false;
    conn->shell    = false;
    conn->echo_off = false;
    conn->last_cr  = false;
    conn->last_nl  = false;
    conn->esc      = 0;
    conn->iac      = 0;
    conn->iac_cmd  = 0;

    memset( &conn->scb, 0, sizeof( conn->scb ) );
    conn->scb.userdata                                = conn;
    conn->scb.auth_none_function                      = auth_none_cb;
    conn->scb.auth_password_function                  = auth_password_cb;
    conn->scb.auth_pubkey_function                    = auth_pubkey_cb;
    conn->scb.channel_open_request_session_function   = channel_open_cb;
    ssh_callbacks_init( &conn->scb );
    ssh_set_server_callbacks( session, &conn->scb );

    conn->next = conn_list;
    conn_list  = conn;
    ++ssh_count;

    if( ssh_bind_accept_fd( ssh_binder, session, fd ) != SSH_OK ) {
      sshlog( "SSH: accept from %s failed: %s\n", ip, ssh_get_error( ssh_binder ) );
      close( fd );
      conn->state = SSH_ST_DEAD;
      continue;
    }

    ssh_set_blocking( session, 0 );
  }
}


static void drop_conn( ssh_conn* conn )
{
  if( conn->fd >= 0 ) {
    close( conn->fd );   // game sees EOF: save and linkdead as for telnet
    conn->fd = -1;
  }

  if( conn->in_event )
    ssh_event_remove_session( ssh_events, conn->session );

  if( conn->channel )
    ssh_channel_free( conn->channel );

  ssh_disconnect( conn->session );
  ssh_free( conn->session );

  --ssh_count;
  delete conn;
}


static void pump_conn( ssh_conn* conn )
{
  if( conn->state == SSH_ST_KEX ) {
    const int rc = ssh_handle_key_exchange( conn->session );
    if( rc == SSH_AGAIN ) {
      if( current_time > conn->deadline )
	conn->state = SSH_ST_DEAD;
      return;
    }
    if( rc != SSH_OK ) {
      conn->state = SSH_ST_DEAD;
      return;
    }
    // Only "none" (for the game user) ever succeeds.  "publickey" is also
    // advertised because libssh cannot send an auth failure with an empty
    // method list; with it a wrong user name gets a prompt "Permission
    // denied (publickey)" instead of a hang.
    ssh_set_auth_methods( conn->session,
			  SSH_AUTH_METHOD_NONE | SSH_AUTH_METHOD_PUBLICKEY );
    ssh_event_add_session( ssh_events, conn->session );
    conn->in_event = true;
    conn->state = SSH_ST_UP;
    return;
  }

  if( conn->state == SSH_ST_DEAD )
    return;

  if( !ssh_is_connected( conn->session )
      || ( ssh_get_status( conn->session ) & ( SSH_CLOSED | SSH_CLOSED_ERROR ) ) ) {
    conn->state = SSH_ST_DEAD;
    return;
  }

  if( conn->fd < 0 ) {
    // Not in the game yet: hold to the handshake/auth deadline.
    if( current_time > conn->deadline )
      conn->state = SSH_ST_DEAD;
    return;
  }

  // Game -> client, only while the client keeps up.
  while( conn->state != SSH_ST_CLOSING
	 && conn->to_client.size( ) < TO_CLIENT_MAX ) {
    unsigned char buf [ 4096 ];
    const ssize_t n = read( conn->fd, buf, sizeof( buf ) );
    if( n > 0 ) {
      process_game_output( conn, buf, n );
      continue;
    }
    if( n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ) ) {
      // The game closed the link (quit, ban, idle): finish and hang up.
      conn->state = SSH_ST_CLOSING;
      conn->deadline = current_time + 5;
    }
    break;
  }

  if( !conn->to_client.empty( ) ) {
    const uint32_t win = ssh_channel_window_size( conn->channel );
    size_t n = conn->to_client.size( );
    if( n > win )
      n = win;
    if( n > 0 ) {
      const int w = ssh_channel_write( conn->channel, conn->to_client.data( ), n );
      if( w == SSH_ERROR ) {
	conn->state = SSH_ST_DEAD;
	return;
      }
      if( w > 0 )
	conn->to_client.erase( 0, w );
    }
  }

  // Client -> game.
  if( !conn->to_game.empty( ) && conn->state != SSH_ST_CLOSING ) {
    const ssize_t w = write( conn->fd, conn->to_game.data( ), conn->to_game.size( ) );
    if( w > 0 )
      conn->to_game.erase( 0, w );
  }

  if( conn->state == SSH_ST_CLOSING ) {
    if( conn->to_client.empty( ) && ssh_channel_is_open( conn->channel ) ) {
      ssh_channel_request_send_exit_status( conn->channel, 0 );
      ssh_channel_send_eof( conn->channel );
      ssh_channel_close( conn->channel );
    }
    // DEAD once the client answers our close (close_cb), which it only
    // does after it has received everything before it.
    if( current_time > conn->deadline )
      conn->state = SSH_ST_DEAD;
  }
}


void update_ssh( )
{
  if( ssh_socket < 0 )
    return;

  accept_ssh( );

  if( !conn_list )
    return;

  // Run libssh's packet handling (and our callbacks) without blocking.
  ssh_event_dopoll( ssh_events, 0 );

  for( ssh_conn **pp = &conn_list; *pp; ) {
    ssh_conn *conn = *pp;
    pump_conn( conn );
    if( conn->state == SSH_ST_DEAD ) {
      *pp = conn->next;
      drop_conn( conn );
    } else {
      pp = &conn->next;
    }
  }

  // Push what we just queued instead of waiting for the next pulse.
  ssh_event_dopoll( ssh_events, 0 );
}
