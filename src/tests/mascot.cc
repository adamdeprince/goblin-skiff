/* Distributed under the GNU GPL, version 3 or later. */
#include <iostream>
#include <stdexcept>

#include "src/frontend/mascot.h"
#include "src/terminal/kittygraphics.h"

namespace {
void require( bool condition, const char* message )
{
  if ( !condition ) {
    throw std::runtime_error( message );
  }
}
void tests()
{
  const auto asset = Mascot::asset();
  uint32_t width = 0, height = 0;
  require( asset.size() < 20000 && asset.substr( 12, 4 ) == "VP8 ", "small lossy WebP" );
  require( Terminal::kitty_webp_dimensions( asset, width, height ) && width == 512 && height == 512,
           "retina-sized square source" );
  const auto ascii = Mascot::render( Mascot::Format::Ascii, {} );
  size_t position = 0;
  for ( int row = 0; row < 16; row++ ) {
    const size_t end = ascii.find( "\r\n", position );
    require( end == position + 32, "ASCII 32 columns" );
    position = end + 2;
  }
  require( position == ascii.size(), "ASCII 16 rows" );
  const auto kitty = Mascot::render( Mascot::Format::Kitty, {} );
  require( kitty.find( "c=16,r=8" ) != std::string::npos && kitty.find( "s=512,v=512" ) != std::string::npos,
           "Kitty dimensions" );
  require( kitty.find( ",i=" ) == std::string::npos, "mascot must not replace images from earlier sessions" );
  const auto sixel = Mascot::render( Mascot::Format::Sixel, {} );
  require( sixel.find( "\033P0;0;0q" ) != std::string::npos && sixel.find( "\033\\" ) != std::string::npos,
           "sixel envelope" );
  for ( const auto& graphics : { kitty, sixel } ) {
    require( graphics.find( "\033[H" ) == std::string::npos && graphics.find( "\033[2J" ) == std::string::npos
               && graphics.find( "\0337" ) != std::string::npos && graphics.find( "\0338" ) != std::string::npos,
             "graphics reserve inline space and restore relative cursor, never home or erase" );
  }
  require( Mascot::render( Mascot::Format::None, {} ).empty(), "opt out" );
  const std::string pointer_reply = "\033]777;goblin-pointer;v=1:op=cap:id=17349521:caps=7:"
                                    "w=1280:h=768:cw=16:ch=32\033\\";
  const std::string legacy_replies = "\033_Gi=4294967294;OK\033\\\033[?62;4c\033[6;32;16t\033[?0u\033[?5522;2$y"
                                     "\033[20;1R\033[20;3R\033[20;5R";
  const std::string replies = legacy_replies + pointer_reply;
  for ( size_t split = 0; split <= replies.size(); split++ ) {
    Mascot::Splash splash;
    const auto queries = splash.start( {}, true );
    require( queries.find( "\033]777;goblin-pointer;v=1:op=query:id=17349521\033\\" ) != std::string::npos,
             "local pointer capability query" );
    require( splash.paint( 0 ).empty(), "no ASCII flash before capability replies" );
    auto first = splash.filter( "before" + replies.substr( 0, split ), 0 );
    auto second = splash.filter( replies.substr( split ) + "after", 1 );
    require( first + second == "beforeafter", "capability replies never reach remote" );
    require( splash.selected() == Mascot::Format::Kitty, "Kitty preferred over sixel" );
    require( splash.probe_ready( 1 ) && splash.supports_keyboard() && splash.supports_text_sizing() == 2
               && splash.supports_clipboard() && splash.supports_pointer(),
             "all local capabilities discovered across arbitrary byte splits" );
    const auto banner = splash.paint( 1 );
    require( banner.find( "\033_G" ) != std::string::npos && banner.find( "\033[2J" ) == std::string::npos,
             "draw native banner once without clearing" );
    require( banner.substr( banner.size() - 5 ) == "\033[?6n" && !splash.cursor_ready( 1 ),
             "query physical cursor only after the complete banner" );
    require( splash.filter( "\033[?15;1R", 2 ).empty() && splash.cursor_ready( 2 ) && splash.start_row() == 14,
             "post-banner cursor position stays local and is zero-based" );
    require( splash.paint( 100 ).empty(), "never redraw a printed banner" );
    require( splash.dismiss().empty() && !splash.active(), "dismiss leaves banner in scrollback" );
  }
  Mascot::Splash no_pointer;
  no_pointer.start( {}, true, 100 );
  require( no_pointer.filter( legacy_replies, 101 ).empty(), "legacy terminal replies stay local" );
  require( !no_pointer.probe_ready( 349 ) && no_pointer.probe_ready( 350 ) && !no_pointer.supports_pointer(),
           "unsupported pointer probe completes at its bounded timeout" );
  require( !no_pointer.paint( 350 ).empty(), "missing pointer reply does not prevent mascot rendering" );
  require( no_pointer.filter( pointer_reply, 351 ).empty() && no_pointer.paint( 352 ).empty(),
           "late pointer reply stays local without repainting history" );

  const std::string no_pointer_reply = "\033]777;goblin-pointer;v=1:op=cap:id=17349521:caps=0:"
                                       "w=1280:h=768:cw=16:ch=32\a";
  Mascot::Splash declined_pointer;
  declined_pointer.start( {}, true );
  require( declined_pointer.filter( legacy_replies + no_pointer_reply, 1 ).empty()
             && declined_pointer.probe_ready( 1 ) && !declined_pointer.supports_pointer(),
           "negative BEL-terminated pointer reply completes the probe without enabling support" );
  Mascot::Splash splash;
  splash.start( {}, true );
  require( splash.filter( "\033", 0 ).empty(), "partial local reply" );
  require( splash.flush( 49 ).empty() && splash.flush( 50 ) == "\033", "ordinary Escape not swallowed" );
  require( splash.filter( "\033[Ahello\0330", 100 ) == "\033[Ahello\0330", "ordinary keys unchanged" );
  require( !splash.paint( 250 ).empty(), "bounded probe timeout produces ASCII without a reply" );
  require( splash.filter( replies, 251 ).empty() && splash.paint( 252 ).empty(),
           "late replies are consumed without repainting history" );
  Mascot::Splash sixel_splash;
  sixel_splash.start( {}, true );
  require( sixel_splash.filter( "\033[?62;4c", 0 ).empty() && sixel_splash.selected() == Mascot::Format::Sixel,
           "sixel capability" );
  /* The G key of the wide gamut protocol: asked with a pixel, after the
     Kitty query and before DA1, which bounds a terminal's silence. */
  Mascot::Splash gamut;
  const auto gamut_queries = gamut.start( {}, true );
  const size_t gamut_query = gamut_queries.find( "\033_Gi=4294967292,s=1,v=1,a=q,t=d,f=24,G=1;AAAA\033\\" );
  require( gamut_query != std::string::npos && gamut_query < gamut_queries.find( "\033[c" ), "gamut query before DA1" );
  require( !gamut.image_gamut_known() && !gamut.supports_image_gamut(), "not known before an answer" );
  const std::string gamut_replies = "\033_Gi=4294967294;OK\033\\\033_Gi=4294967292;OK\033\\\033[?62;4c";
  for ( size_t split = 0; split <= gamut_replies.size(); split++ ) {
    Mascot::Splash with_gamut;
    with_gamut.start( {}, true );
    require( with_gamut.filter( "a" + gamut_replies.substr( 0, split ), 0 )
                 + with_gamut.filter( gamut_replies.substr( split ) + "b", 1 )
               == "ab",
             "the answer stays local" );
    require( with_gamut.image_gamut_known() && with_gamut.supports_image_gamut() && with_gamut.supports_sixel(),
             "a terminal that takes G" );
  }
  Mascot::Splash without_gamut;
  without_gamut.start( {}, true );
  require( without_gamut.filter( "\033_Gi=4294967294;OK\033\\\033[?62c", 0 ).empty()
             && without_gamut.image_gamut_known() && !without_gamut.supports_image_gamut(),
           "DA1 with no answer before it: no G" );
  Mascot::Splash refused_gamut;
  refused_gamut.start( {}, true );
  require( refused_gamut.filter( "\033_Gi=4294967292;EINVAL:bad key\033\\\033[?62c", 0 ).empty()
             && refused_gamut.image_gamut_known() && !refused_gamut.supports_image_gamut(),
           "an error: no G" );
  Mascot::Splash dumb;
  require( dumb.start( {}, false ).empty() && !dumb.active(), "noninteractive no probes" );
  require( dumb.cursor_ready( 0 ) && dumb.start_row() == -1, "no cursor wait without a tty" );

  const std::string cursor_reply = "\033[?15;1R";
  for ( size_t split = 0; split <= cursor_reply.size(); split++ ) {
    Mascot::Splash no_image( Mascot::Format::None );
    const auto queries = no_image.start( {}, true );
    require( queries.substr( queries.size() - 5 ) == "\033[?6n", "no-mascot query follows all local probes" );
    require( !no_image.cursor_ready( 249 ) && no_image.cursor_ready( 250 ) && no_image.start_row() == -1,
             "missing cursor report falls back after a bounded wait" );
    const auto before = no_image.filter( "before" + cursor_reply.substr( 0, split ), 251 );
    const auto after = no_image.filter( cursor_reply.substr( split ) + "after", 252 );
    require( before + after == "beforeafter" && no_image.start_row() == 14,
             "fragmented and late cursor reports never become remote input" );
    no_image.invalidate_cursor();
    require( no_image.start_row() == -1, "resize/resume invalidates the startup coordinates" );
  }
  Mascot::Splash resized( Mascot::Format::None );
  resized.start( {}, true );
  resized.invalidate_cursor();
  require( resized.filter( cursor_reply, 1 ).empty() && resized.start_row() == -1,
           "a reply to a pre-resize query cannot restore stale coordinates" );

  for ( bool allow_kitty : { false, true } ) {
    for ( bool allow_sixel : { false, true } ) {
      Mascot::Splash limited( Mascot::Format::Auto, allow_kitty, allow_sixel );
      const auto queries = limited.start( {}, true );
      require( ( queries.find( "\033_G" ) != std::string::npos ) == allow_kitty, "Kitty probe override" );
      require( ( queries.find( "\033[c" ) != std::string::npos ) == ( allow_sixel || allow_kitty ),
               "DA bounds the sixel and gamut probes" );
      require( ( queries.find( "\033[16t" ) != std::string::npos ) == ( allow_sixel || allow_kitty ), "graphics size probe override" );
      std::string allowed_replies;
      if ( allow_kitty ) {
        allowed_replies += "\033_Gi=4294967294;OK\033\\";
      }
      if ( allow_sixel ) {
        allowed_replies += "\033[?62;4c";
      }
      if ( allow_sixel || allow_kitty ) { allowed_replies += "\033[6;32;16t"; }
      require( limited.filter( allowed_replies, 1 ).empty(), "allowed replies consumed" );
      const auto expected = allow_kitty   ? Mascot::Format::Kitty
                            : allow_sixel ? Mascot::Format::Sixel
                                          : Mascot::Format::Ascii;
      require( limited.selected() == expected && !limited.paint( 1 ).empty(), "capability override selection" );
      require( limited.dismiss().empty(), "override banner remains in history" );
    }
  }
  for ( const auto format : { Mascot::Format::Kitty, Mascot::Format::Sixel } ) {
    Mascot::Splash disabled( format, false, false );
    const auto queries = disabled.start( {}, true );
    const auto banner = disabled.paint();
    require( queries.find( "\033_G" ) == std::string::npos && queries.find( "\033[c" ) == std::string::npos
               && disabled.selected() == Mascot::Format::Ascii
               && banner.find( "\033_G" ) == std::string::npos && banner.find( "\033P" ) == std::string::npos,
             "disabled protocols override forced mascot formats" );
  }
}
}

int main()
{
  try {
    tests();
    std::cout << "mascot tests passed\n";
  } catch ( const std::exception& error ) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
