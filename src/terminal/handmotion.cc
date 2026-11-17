/*
    Goblin Skiff: the motion of a hand, smoothed and thinned before it crosses the link
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/handmotion.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "src/kalman/kalman.hpp"
#include "src/terminal/terminalframebuffer.h"

namespace Terminal {
namespace Motion {

namespace {
const int64_t COORD_MAX = 1073741823;
const int64_t T_MAX = int64_t( 1 ) << 50;
/* A contact whose last report is this much older than its next is a new motion. */
const int64_t GAP_MS = 100;
/* A report this far from where the hand was heading is a new motion too. */
const double JUMP_UNITS = 16 * 256;

bool is_tablet( const std::string& selector )
{
  return selector == Selector::TABLET || selector == "goblin-pointer";
}

int64_t integer_or( const Ext::Fields& f, const char* key, int64_t fallback )
{
  int64_t value = fallback;
  if ( Ext::integer( f, key, INT64_MIN / 4, INT64_MAX / 4, value ) != Ext::Value::Valid ) {
    return fallback;
  }
  return value;
}

/* The value of a field as a string, or "-" for none: what must be the same for
   two reports to merge. */
std::string same( const Ext::Fields& f, const char* key )
{
  const std::string* value = f.find( key );
  return value ? *value : std::string( "-" );
}

double field_or( const Ext::Fields& f, const char* key, bool& present )
{
  int64_t value = 0;
  present = Ext::integer( f, key, INT64_MIN / 4, INT64_MAX / 4, value ) == Ext::Value::Valid;
  return double( value );
}

/* A measurement of a report, with its tolerance and whether it wraps around. */
struct Measurement
{
  const char* key;
  double tolerance;
  double wrap; /* 0, or the period of an angle */
};

std::vector<Measurement> measurements( const Settings& s )
{
  const double f = s.fraction_tolerance;
  return { { "p", s.pressure_tolerance, 0 },    { "tx", s.tilt_tolerance, 0 },      { "ty", s.tilt_tolerance, 0 },
           { "twist", s.angle_tolerance, 36000 }, { "tangent", f * 131070, 0 },      { "z", f * 2000000, 0 },
           { "dist", f * 65535, 0 },            { "major", s.tolerance, 0 },         { "minor", s.tolerance, 0 },
           { "angle", s.angle_tolerance, 36000 }, { "rx", f * 65535, 0 },             { "ry", f * 65535, 0 },
           { "grip", f * 65535, 0 } };
}

double distance_to_segment( double px, double py, double ax, double ay, double bx, double by )
{
  const double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
  double u = 0;
  if ( length > 0 ) {
    u = std::max( 0.0, std::min( 1.0, ( ( px - ax ) * dx + ( py - ay ) * dy ) / length ) );
  }
  return std::hypot( px - ( ax + u * dx ), py - ( ay + u * dy ) );
}

double circular( double difference, double period )
{
  difference = std::fmod( difference, period );
  if ( difference > period / 2 ) {
    difference -= period;
  } else if ( difference < -period / 2 ) {
    difference += period;
  }
  return difference;
}

/* The merge of two relative fields that add up. */
void add( const Ext::Fields& earlier, Ext::Fields& later, const char* key, bool& any )
{
  int64_t before = 0, after = 0;
  if ( Ext::integer( earlier, key, -T_MAX, T_MAX, before ) == Ext::Value::Valid ) {
    Ext::integer( later, key, -T_MAX, T_MAX, after );
    later.set( key, before + after );
    any = true;
  }
}

/* The merge of two factors given in thousandths (a pinch) or ten-thousandths. */
void multiply( const Ext::Fields& earlier, Ext::Fields& later, const char* key, int64_t one, bool change )
{
  int64_t before = 0, after = 0;
  if ( Ext::integer( earlier, key, -T_MAX, T_MAX, before ) != Ext::Value::Valid
       || Ext::integer( later, key, -T_MAX, T_MAX, after ) != Ext::Value::Valid ) {
    return;
  }
  /* A gesture's scale is the change in magnification: 100 is a tenth larger. */
  const double a = change ? 1 + double( before ) / one : double( before ) / one;
  const double b = change ? 1 + double( after ) / one : double( after ) / one;
  const double product = change ? ( a * b - 1 ) * one : a * b * one;
  later.set( key, int64_t( std::llround( product ) ) );
}

template<std::size_t N>
kalman::Mat<N, N> process_noise( double density, double dt );

template<>
kalman::Mat<3, 3> process_noise<3>( double q, double dt )
{
  const double dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt3 * dt, dt5 = dt4 * dt;
  return kalman::Mat<3, 3> { q * dt5 / 20, q * dt4 / 8, q * dt3 / 6, q * dt4 / 8, q * dt3 / 3,
                             q * dt2 / 2,  q * dt3 / 6, q * dt2 / 2, q * dt };
}

template<>
kalman::Mat<2, 2> process_noise<2>( double q, double dt )
{
  const double dt2 = dt * dt, dt3 = dt2 * dt;
  return kalman::Mat<2, 2> { q * dt3 / 3, q * dt2 / 2, q * dt2 / 2, q * dt };
}

/* One measured quantity of a contact: its filter, and the time of the last
   measurement it took. NX is 3 for a position (with velocity and
   acceleration) and 2 for pressure and tilt (with their rate of change). */
template<std::size_t NX>
struct Axis
{
  kalman::LinearKalmanFilter<NX, 1> filter;
  bool have = false;
  int64_t last_t = 0;
  double noise = 1, drift = 1;

  void configure( double s_noise, double s_drift )
  {
    noise = s_noise;
    drift = s_drift;
    filter.H( 0, 0 ) = 1;
    filter.R( 0, 0 ) = noise * noise;
  }
  void start( double z, int64_t t )
  {
    filter.x = kalman::Vec<NX>();
    filter.x[0] = z;
    filter.P = kalman::Mat<NX, NX>();
    filter.P( 0, 0 ) = noise * noise;
    filter.P( 1, 1 ) = NX == 3 ? 1e5 : 1e4; /* units per ms: a fast hand, squared */
    if ( NX == 3 ) {
      filter.P( 2, 2 ) = 1e3;
    }
    have = true;
    last_t = t;
  }
  /* Where the filter expects the next measurement, for the jump test. */
  double expected( int64_t t ) const
  {
    const double dt = double( t - last_t );
    double x = filter.x[0] + filter.x[1] * dt;
    if ( NX == 3 ) {
      x += filter.x[2] * dt * dt / 2;
    }
    return x;
  }

  /* The smoothed value of the first of a run of measurements, with the ones
     after it refining it, and the filter advanced through the first alone. */
  double smooth( const std::vector<std::pair<int64_t, double>>& zs )
  {
    const std::size_t m = zs.size();
    std::vector<kalman::Vec<NX>> x_pred( m ), x_filt( m ), x_smooth( m );
    std::vector<kalman::Mat<NX, NX>> P_pred( m ), P_filt( m ), P_smooth( m ), F( m );
    kalman::LinearKalmanFilter<NX, 1> f = filter;
    int64_t t = last_t;
    kalman::LinearKalmanFilter<NX, 1> after_first = f;
    for ( std::size_t i = 0; i < m; i++ ) {
      const double dt = double( std::max<int64_t>( 0, zs[i].first - t ) );
      f.Q = process_noise<NX>( drift, dt );
      f.predict_kinematic( dt );
      x_pred[i] = f.x;
      P_pred[i] = f.P;
      F[i] = f.F;
      (void)f.correct( kalman::Vec<1> { zs[i].second } );
      x_filt[i] = f.x;
      P_filt[i] = f.P;
      t = zs[i].first;
      if ( i == 0 ) {
        after_first = f;
      }
    }
    x_smooth[m - 1] = x_filt[m - 1];
    P_smooth[m - 1] = P_filt[m - 1];
    for ( std::size_t i = m - 1; i-- > 0; ) {
      if ( !kalman::rts_smooth_step( x_filt[i], P_filt[i], x_pred[i + 1], P_pred[i + 1], x_smooth[i + 1],
                                     P_smooth[i + 1], F[i + 1], x_smooth[i], P_smooth[i] ) ) {
        x_smooth[i] = x_filt[i];
        P_smooth[i] = P_filt[i];
      }
    }
    filter = after_first;
    last_t = zs[0].first;
    return x_smooth[0][0];
  }
};

int64_t clamp_round( double value, int64_t lo, int64_t hi )
{
  if ( !std::isfinite( value ) ) {
    return lo;
  }
  const double rounded = std::round( value );
  return rounded < double( lo ) ? lo : rounded > double( hi ) ? hi : int64_t( rounded );
}
}

Settings Settings::from_environment()
{
  Settings s;
  if ( const char* lag = getenv( "GOBLIN_SKIFF_HAND_SMOOTHING" ) ) {
    char* end = nullptr;
    const unsigned long value = strtoul( lag, &end, 10 );
    if ( *lag && !*end ) {
      s.lag_ms = value;
    }
  }
  if ( const char* tolerance = getenv( "GOBLIN_SKIFF_HAND_TOLERANCE" ) ) {
    char* end = nullptr;
    const double value = strtod( tolerance, &end );
    if ( *tolerance && !*end && value >= 0 && std::isfinite( value ) ) {
      s.tolerance = value;
      s.pad_tolerance = value;
    }
  }
  if ( const char* keep = getenv( "GOBLIN_SKIFF_HAND_INTERVAL" ) ) {
    char* end = nullptr;
    const unsigned long value = strtoul( keep, &end, 10 );
    if ( *keep && !*end ) {
      s.keep_ms = value;
    }
  }
  if ( const char* exact = getenv( "GOBLIN_SKIFF_EXACT_MOUSE" ) ) {
    if ( !strcmp( exact, "1" ) || !strcmp( exact, "yes" ) || !strcmp( exact, "true" ) ) {
      s.exact = true;
      s.lag_ms = 0;
      s.keep_ms = 0;
    }
  }
  return s;
}

Merge merge_policy( const Ext::Frame& frame, std::string& contact, std::string& compatible )
{
  const Ext::Fields& f = frame.fields;
  const std::string* op = f.find( "op" );
  if ( !op || *op != "event" ) {
    return Merge::Never;
  }
  const int64_t phase = integer_or( f, "phase", -1 );
  if ( is_tablet( frame.selector ) ) {
    const int64_t kind = integer_or( f, "kind", 1 );
    int64_t device = 0, id = 0;
    if ( f.has( "seq" ) || f.has( "pred" ) || f.has( "est" ) || f.has( "ink" )
         || Ext::integer( f, "device", 0, INT64_MAX / 4, device ) != Ext::Value::Valid ) {
      return Merge::Never; /* a report a correction may name, or the start of ink */
    }
    if ( kind == 3 && frame.selector == "goblin-pointer" ) {
      /* A gesture of Skiff's own protocol: its relative fields add up. */
      if ( phase != 2 ) {
        return Merge::Never;
      }
      contact = frame.selector + ":3:" + std::to_string( device ) + ":" + same( f, "peer" );
      compatible = same( f, "mods" ) + ":" + same( f, "buttons" );
      return Merge::Always;
    }
    if ( ( kind != 1 && kind != 2 ) || ( phase != 0 && phase != 2 )
         || Ext::integer( f, "contact", 0, INT64_MAX / 4, id ) != Ext::Value::Valid ) {
      return Merge::Never;
    }
    contact = frame.selector + ":" + std::to_string( kind ) + ":" + std::to_string( device ) + ":" + std::to_string( id );
    compatible.clear();
    for ( const char* key : { "phase", "buttons", "mods", "tool", "valid", "src", "stage", "conf", "w", "h" } ) {
      compatible += same( f, key ) + ":";
    }
    /* A hand that is down is drawing or dragging, and is kept to the tolerances;
       one hovering is only pointing, and goes at the rate of the link. */
    return phase == 2 ? Merge::Bounded : Merge::Always;
  }
  if ( frame.selector == Selector::SCROLL ) {
    int64_t src = 0, dx = 0, dy = 0;
    if ( ( phase != 0 && phase != 2 && phase != 5 ) || Ext::integer( f, "src", 0, INT32_MAX, src ) != Ext::Value::Valid
         || Ext::integer( f, "dx", -T_MAX, T_MAX, dx ) != Ext::Value::Valid
         || Ext::integer( f, "dy", -T_MAX, T_MAX, dy ) != Ext::Value::Valid ) {
      return Merge::Never; /* a change of phase is never merged */
    }
    contact = "scroll:" + std::to_string( src );
    compatible = std::to_string( phase );
    return Merge::Always;
  }
  if ( frame.selector == Selector::GESTURE ) {
    int64_t kind = 0;
    if ( phase != 2 || Ext::integer( f, "kind", 1, INT32_MAX, kind ) != Ext::Value::Valid ) {
      return Merge::Never; /* only the updates of a gesture, never its begin or end */
    }
    contact = "gesture:" + std::to_string( kind );
    compatible = same( f, "fingers" );
    return Merge::Always;
  }
  return Merge::Never;
}

bool within_tolerances( const Settings& settings,
                        const Ext::Fields& last,
                        const std::vector<Ext::Fields>& dropped,
                        const Ext::Fields& held,
                        const Ext::Fields& next,
                        double position_tolerance )
{
  bool have_last_x, have_last_y, have_next_x, have_next_y;
  const double ax = field_or( last, "x", have_last_x ), ay = field_or( last, "y", have_last_y );
  const double bx = field_or( next, "x", have_next_x ), by = field_or( next, "y", have_next_y );
  if ( !have_last_x || !have_last_y || !have_next_x || !have_next_y ) {
    return false;
  }
  const double eps = position_tolerance >= 0 ? position_tolerance
                     : same( next, "src" ) == "1"  ? settings.pad_tolerance
                                                   : settings.tolerance;
  bool have_t0, have_t1;
  const double t0 = field_or( last, "t", have_t0 ), t1 = field_or( next, "t", have_t1 );
  const bool timed = have_t0 && have_t1 && t1 > t0;
  const std::vector<Measurement> fields = measurements( settings );
  const auto check = [&]( const Ext::Fields& d ) {
    bool have_x, have_y;
    const double px = field_or( d, "x", have_x ), py = field_or( d, "y", have_y );
    if ( !have_x || !have_y || distance_to_segment( px, py, ax, ay, bx, by ) > eps ) {
      return false;
    }
    bool have_t;
    const double td = field_or( d, "t", have_t );
    const double u = timed && have_t ? std::max( 0.0, std::min( 1.0, ( td - t0 ) / ( t1 - t0 ) ) ) : -1;
    for ( const Measurement& m : fields ) {
      bool have_d, have_a, have_b;
      const double vd = field_or( d, m.key, have_d ), va = field_or( last, m.key, have_a ), vb = field_or( next, m.key, have_b );
      if ( !have_d ) {
        continue;
      }
      if ( !have_a || !have_b ) {
        return false; /* a measurement that comes and goes is not interpolated */
      }
      double expected;
      if ( u >= 0 ) {
        expected = m.wrap ? va + u * circular( vb - va, m.wrap ) : va + u * ( vb - va );
      } else {
        /* Without times, the report must be near both ends. */
        const double da = m.wrap ? circular( vd - va, m.wrap ) : vd - va;
        const double db = m.wrap ? circular( vd - vb, m.wrap ) : vd - vb;
        if ( std::fabs( da ) > m.tolerance || std::fabs( db ) > m.tolerance ) {
          return false;
        }
        continue;
      }
      const double error = m.wrap ? circular( vd - expected, m.wrap ) : vd - expected;
      if ( std::fabs( error ) > m.tolerance ) {
        return false;
      }
    }
    return true;
  };
  for ( const Ext::Fields& d : dropped ) {
    if ( !check( d ) ) {
      return false;
    }
  }
  return check( held );
}

void merge_into( const Ext::Frame& earlier, Ext::Frame& later )
{
  if ( later.selector == Selector::SCROLL ) {
    bool any = false;
    add( earlier.fields, later.fields, "dx", any );
    add( earlier.fields, later.fields, "dy", any );
    return;
  }
  if ( later.selector == Selector::GESTURE ) {
    bool any = false;
    multiply( earlier.fields, later.fields, "scale", 1000, true );
    add( earlier.fields, later.fields, "angle", any );
    return;
  }
  if ( later.selector == "goblin-pointer" && integer_or( later.fields, "kind", 1 ) == 3 ) {
    bool any = false;
    add( earlier.fields, later.fields, "dx", any );
    add( earlier.fields, later.fields, "dy", any );
    add( earlier.fields, later.fields, "angle", any );
    multiply( earlier.fields, later.fields, "scale", 10000, false );
    return;
  }
  /* A tool's wheel: the rotation since the last report adds up, and the merged
     report measures a wheel if any of them did. */
  bool wheel = false;
  add( earlier.fields, later.fields, "wheel", wheel );
  add( earlier.fields, later.fields, "wclicks", wheel );
  int64_t valid = 0;
  if ( wheel && Ext::integer( later.fields, "valid", 0, INT32_MAX, valid ) != Ext::Value::Invalid ) {
    later.fields.set( "valid", valid | 64 );
  }
}

/* The smoother */

struct Smoother::Track
{
  Axis<3> x, y;
  Axis<2> p, tx, ty;
};

Smoother::Smoother( const Settings& s_settings ) : settings( s_settings ) {}

Smoother::~Smoother() = default;

Smoother::Track& Smoother::track( const std::string& key )
{
  std::unique_ptr<Track>& slot = tracks[key];
  if ( !slot ) {
    slot.reset( new Track() );
    slot->x.configure( settings.position_noise, settings.jerk );
    slot->y.configure( settings.position_noise, settings.jerk );
    slot->p.configure( settings.pressure_noise, settings.pressure_drift );
    slot->tx.configure( settings.tilt_noise, settings.tilt_drift );
    slot->ty.configure( settings.tilt_noise, settings.tilt_drift );
  }
  return *slot;
}

void Smoother::feed( const Report& report, uint64_t now, std::vector<ExtensionInput>& ready )
{
  Pending pending;
  pending.input = report.input;
  pending.ink = report.ink;
  pending.arrived = now;
  const std::string* op = nullptr;
  int64_t kind = 1, phase = -1, device = 0, contact = 0, x = 0, y = 0;
  const bool parsed = !report.input.payload.empty() && Ext::parse( report.input.payload, pending.frame );
  if ( parsed ) {
    op = pending.frame.fields.find( "op" );
    kind = integer_or( pending.frame.fields, "kind", 1 );
    phase = integer_or( pending.frame.fields, "phase", -1 );
  }
  const Ext::Fields& f = pending.frame.fields;
  const bool hand = parsed && op && *op == "event" && is_tablet( pending.frame.selector ) && ( kind == 1 || kind == 2 )
                    && Ext::integer( f, "device", 0, INT64_MAX / 4, device ) == Ext::Value::Valid;
  if ( !hand || !settings.lag_ms ) {
    /* Not a contact's motion, or no smoothing: it goes behind what is held. */
    release( now, ready );
    ready.push_back( report.input );
    return;
  }
  Ext::integer( f, "contact", 0, INT64_MAX / 4, contact );
  pending.key = pending.frame.selector + ":" + std::to_string( kind ) + ":" + std::to_string( device ) + ":"
                + std::to_string( contact );
  /* The down, the moves and the up are the stroke; a hover only points. */
  pending.measured = phase >= 1 && phase <= 3 && !f.has( "seq" ) && !f.has( "pred" ) && !f.has( "est" )
                     && Ext::integer( f, "x", -COORD_MAX, COORD_MAX, x ) == Ext::Value::Valid
                     && Ext::integer( f, "y", -COORD_MAX, COORD_MAX, y ) == Ext::Value::Valid;
  /* A finger's id is given to the next finger once it lifts; a tool stays
     itself across an up and a hover, and its filter follows it. */
  pending.ends = phase == 4 || phase == 5 || ( phase == 3 && kind == 2 );
  queue.push_back( std::move( pending ) );
  release( now, ready );
}

void Smoother::release( uint64_t now, std::vector<ExtensionInput>& ready )
{
  while ( !queue.empty() && now >= queue.front().arrived + settings.lag_ms ) {
    emit( queue.front(), ready );
    queue.pop_front();
  }
}

void Smoother::flush( std::vector<ExtensionInput>& ready )
{
  while ( !queue.empty() ) {
    emit( queue.front(), ready );
    queue.pop_front();
  }
}

int Smoother::wait_time( uint64_t now ) const
{
  if ( queue.empty() ) {
    return INT_MAX;
  }
  const uint64_t due = queue.front().arrived + settings.lag_ms;
  return now >= due ? 0 : int( std::min<uint64_t>( INT_MAX, due - now ) );
}

void Smoother::emit( Pending& front, std::vector<ExtensionInput>& ready )
{
  if ( front.measured ) {
    smooth( front );
  }
  if ( front.ends ) {
    tracks.erase( front.key );
  }
  ready.push_back( front.input );
}

void Smoother::smooth( Pending& front )
{
  /* The run of measurements of this contact: this one and those behind it in
     the queue, up to the end of its motion. */
  std::vector<const Pending*> run { &front };
  for ( auto it = queue.begin(); it != queue.end(); ++it ) {
    if ( &*it == &front ) {
      for ( ++it; it != queue.end(); ++it ) {
        if ( it->key != front.key ) {
          continue;
        }
        if ( it->measured ) {
          run.push_back( &*it );
        }
        if ( it->ends ) {
          break;
        }
      }
      break;
    }
  }
  Track& tr = track( front.key );
  const auto value = []( const Pending* p, const char* key ) { return integer_or( p->frame.fields, key, 0 ); };
  const int64_t t0 = value( &front, "t" );
  int64_t x = value( &front, "x" ), y = value( &front, "y" );
  /* A new motion: nothing before it, a pause, or a jump from where the hand was heading. */
  const bool fresh = !tr.x.have || t0 < tr.x.last_t || t0 - tr.x.last_t > GAP_MS
                     || std::hypot( double( x ) - tr.x.expected( t0 ), double( y ) - tr.y.expected( t0 ) ) > JUMP_UNITS;
  if ( fresh ) {
    tr.x.start( double( x ), t0 );
    tr.y.start( double( y ), t0 );
    tr.p.have = tr.tx.have = tr.ty.have = false;
  }
  std::vector<std::pair<int64_t, double>> xs, ys, ps, txs, tys;
  int64_t previous = t0 - 1;
  for ( const Pending* p : run ) {
    const int64_t t = std::max( value( p, "t" ), previous + ( p == &front ? 0 : 1 ) ); /* time only goes forward */
    previous = t;
    xs.emplace_back( t, double( value( p, "x" ) ) );
    ys.emplace_back( t, double( value( p, "y" ) ) );
    const int64_t valid = value( p, "valid" );
    if ( ( valid & 1 ) && p->frame.fields.has( "p" ) ) {
      ps.emplace_back( t, double( value( p, "p" ) ) );
    }
    if ( ( valid & 2 ) && p->frame.fields.has( "tx" ) && p->frame.fields.has( "ty" ) ) {
      txs.emplace_back( t, double( value( p, "tx" ) ) );
      tys.emplace_back( t, double( value( p, "ty" ) ) );
    }
  }
  Ext::Fields& f = front.frame.fields;
  x = clamp_round( tr.x.smooth( xs ), -COORD_MAX, COORD_MAX );
  y = clamp_round( tr.y.smooth( ys ), -COORD_MAX, COORD_MAX );
  f.set( "x", x );
  f.set( "y", y );
  int64_t p = value( &front, "p" );
  if ( !ps.empty() && ps.front().first == xs.front().first ) {
    if ( !tr.p.have || tr.p.last_t > t0 || t0 - tr.p.last_t > GAP_MS ) {
      tr.p.start( ps.front().second, ps.front().first );
    }
    p = clamp_round( tr.p.smooth( ps ), 0, 65535 );
    f.set( "p", p );
  }
  if ( !txs.empty() && txs.front().first == xs.front().first ) {
    if ( !tr.tx.have || tr.tx.last_t > t0 || t0 - tr.tx.last_t > GAP_MS ) {
      tr.tx.start( txs.front().second, txs.front().first );
      tr.ty.start( tys.front().second, tys.front().first );
    }
    f.set( "tx", clamp_round( tr.tx.smooth( txs ), -9000, 9000 ) );
    f.set( "ty", clamp_round( tr.ty.smooth( tys ), -9000, 9000 ) );
  }
  front.input.payload = "777;" + front.frame.selector + ";" + f.text();
  if ( front.ink.present ) {
    front.input.ink_records = front.ink.render( x, y, p );
    front.input.ink = front.ink;
  }
}

/* The collector */

namespace {
/* An xterm mouse report at the start of text in the encoding in use: its
   length, the code of its button (bit 32 is motion) and its position. */
bool mouse_report( const std::string& text, size_t at, int encoding, size_t& length, int64_t& code, int64_t& x, int64_t& y )
{
  const auto number = [&]( size_t& p, int64_t& out, char end ) {
    const size_t start = p;
    out = 0;
    while ( p < text.size() && text[p] >= '0' && text[p] <= '9' && p - start < 10 ) {
      out = out * 10 + ( text[p++] - '0' );
    }
    if ( p == start || p >= text.size() || text[p] != end ) {
      return false;
    }
    p++;
    return true;
  };
  if ( text.compare( at, 2, "\033[" ) != 0 ) {
    return false;
  }
  size_t p = at + 2;
  x = y = 0;
  if ( encoding == DrawState::MOUSE_ENCODING_SGR || encoding == DrawState::MOUSE_ENCODING_SGR_PIXELS ) {
    if ( p >= text.size() || text[p++] != '<' || !number( p, code, ';' ) || !number( p, x, ';' )
         || !number( p, y, 'M' ) ) {
      return false; /* a release ends in m, and is no motion */
    }
  } else if ( encoding == DrawState::MOUSE_ENCODING_URXVT ) {
    if ( !number( p, code, ';' ) || !number( p, x, ';' ) || !number( p, y, 'M' ) || code < 32 ) {
      return false;
    }
    code -= 32;
  } else {
    /* ESC [ M and three values, each a byte or, in mode 1005, a character. */
    if ( p >= text.size() || text[p++] != 'M' ) {
      return false;
    }
    for ( unsigned i = 0; i < 3; i++ ) {
      if ( p >= text.size() ) {
        return false;
      }
      const unsigned char c = text[p++];
      int64_t value = c;
      if ( encoding == DrawState::MOUSE_ENCODING_UTF8 && c >= 0x80 ) {
        if ( ( c & 0xe0 ) != 0xc0 || p >= text.size() ) {
          return false;
        }
        value = ( ( c & 0x1f ) << 6 ) | ( static_cast<unsigned char>( text[p++] ) & 0x3f );
      }
      if ( i == 0 ) {
        if ( value < 32 ) {
          return false;
        }
        code = value - 32;
      } else if ( i == 1 ) {
        x = value - 32;
      } else {
        y = value - 32;
      }
    }
  }
  length = p - at;
  return true;
}

/* Motion alone: no wheel and no extra button. */
bool mouse_motion( int64_t code )
{
  return ( code & 32 ) && !( code & ( 64 | 128 ) );
}

const char* const POINTER_PREFIX = "\033]777;goblin-pointer;";
}

void PointerCollector::split_motion( const std::string& bytes, int encoding,
                                     std::vector<std::pair<bool, std::string>>& pieces )
{
  const auto plain = [&]( const std::string& text ) {
    if ( !pieces.empty() && !pieces.back().first ) {
      pieces.back().second += text;
    } else {
      pieces.emplace_back( false, text );
    }
  };
  for ( size_t i = 0; i < bytes.size(); ) {
    size_t length = 0;
    int64_t code = 0, x = 0, y = 0;
    if ( encoding >= 0 && !paste && mouse_report( bytes, i, encoding, length, code, x, y ) && mouse_motion( code ) ) {
      pieces.emplace_back( true, bytes.substr( i, length ) );
      i += length;
      paste_marker.clear();
      continue;
    }
    /* The delimiters of a paste may span reads. */
    paste_marker += bytes[i];
    if ( paste_marker.size() > 6 ) {
      paste_marker.erase( 0, 1 );
    }
    if ( paste_marker == "\033[200~" ) {
      paste = true;
    } else if ( paste_marker == "\033[201~" ) {
      paste = false;
    }
    plain( std::string( 1, bytes[i] ) );
    i++;
  }
}

void PointerCollector::emit( Held& entry, std::vector<Item>& ready )
{
  Track& track = tracks[entry.contact];
  track.have_kept = true;
  track.kept_at = entry.arrived;
  if ( entry.policy == Merge::Bounded ) {
    track.have_last = true;
    track.last = entry.frame.fields;
    track.dropped.clear();
  }
  ready.push_back( std::move( entry.item ) );
}

void PointerCollector::hold( Held entry, uint64_t now, uint64_t link_interval, bool backlog, std::vector<Item>& ready )
{
  entry.arrived = now;
  if ( settings.exact ) {
    /* As it came, behind whatever came before it. */
    flush( ready );
    emit( entry, ready );
    return;
  }
  /* Within half of keep_ms of a time. */
  const auto near = [&]( uint64_t then ) { return now >= then && 2 * ( now - then ) <= settings.keep_ms; };
  for ( size_t i = 0; i < held.size(); i++ ) {
    if ( held[i].contact != entry.contact ) {
      continue;
    }
    /* A report of the contact that cannot merge with what is held of it is a
       change of state, or a bend of the stroke: what came first goes first. */
    bool merge = held[i].compatible == entry.compatible && entry.policy != Merge::Never && settings.keep_ms > 0;
    /* The pace of the hand: a report merged away stays within half of keep_ms
       of one kept, the one before it or the one it merges into, so a moving
       hand is reported at least every keep_ms. What is held that the one kept
       before does not cover, this one must. */
    if ( merge && held[i].uncovered ) {
      merge = near( held[i].since );
    }
    if ( merge && entry.policy == Merge::Bounded ) {
      const Track& track = tracks[entry.contact];
      merge = track.have_last
              && within_tolerances( settings, track.last, track.dropped, held[i].frame.fields, entry.frame.fields,
                                    entry.tolerance );
    }
    if ( !merge ) {
      flush( ready );
      break;
    }
    entry.uncovered = held[i].uncovered;
    entry.since = held[i].since;
    merge_into( held[i].frame, entry.frame );
    if ( entry.item.bytes ) {
      if ( entry.item.direct ) {
        entry.item.raw = POINTER_PREFIX + entry.frame.fields.text() + "\033\\";
      }
      /* an xterm report merged is the last report as it came */
    } else {
      entry.item.input.payload = "777;" + entry.frame.selector + ";" + entry.frame.fields.text();
    }
    if ( entry.policy == Merge::Bounded ) {
      tracks[entry.contact].dropped.push_back( held[i].frame.fields );
    }
    /* It takes its place at the end: what is sent stays in the order of the
       last report of each. */
    held.erase( held.begin() + i );
    break;
  }
  if ( !entry.uncovered ) {
    /* Whether the report last kept of the contact, which may have just gone, covers this one. */
    const auto track = tracks.find( entry.contact );
    if ( track == tracks.end() || !track->second.have_kept || !near( track->second.kept_at ) ) {
      entry.uncovered = true;
      entry.since = now;
    }
  }
  held.push_back( std::move( entry ) );
  release( now, link_interval, backlog, ready );
}

void PointerCollector::collect( const ExtensionInput& input, uint64_t now, uint64_t link_interval, bool backlog,
                                std::vector<Item>& ready )
{
  Held entry;
  entry.item.input = input;
  if ( !Ext::parse( input.payload, entry.frame )
       || ( entry.policy = merge_policy( entry.frame, entry.contact, entry.compatible ) ) == Merge::Never ) {
    flush( ready );
    ready.push_back( entry.item );
    return;
  }
  hold( std::move( entry ), now, link_interval, backlog, ready );
}

void PointerCollector::collect_motion( const std::string& raw, int encoding, uint64_t now, uint64_t link_interval,
                                       bool backlog, std::vector<Item>& ready )
{
  size_t length = 0;
  int64_t code = 0, x = 0, y = 0;
  Held entry;
  entry.item.bytes = true;
  entry.item.raw = raw;
  entry.contact = "xterm";
  if ( !mouse_report( raw, 0, encoding, length, code, x, y ) ) {
    flush( ready );
    ready.push_back( entry.item );
    return;
  }
  entry.compatible = std::to_string( code );
  entry.frame.selector = "xterm";
  entry.frame.fields.set( "x", x * 256 );
  entry.frame.fields.set( "y", y * 256 );
  /* A mouse with a button held is dragging or drawing, and its path is kept to
     the stroke: within a pixel in pixels, within half a cell in cells. One with
     no button is pointing, and goes at the pace of the hand: a position at
     least every keep_ms, the last of them each time. */
  if ( ( code & 3 ) != 3 ) {
    entry.policy = Merge::Bounded;
    entry.tolerance = encoding == DrawState::MOUSE_ENCODING_SGR_PIXELS ? std::max( 256.0, settings.tolerance ) : 128;
  } else {
    entry.policy = Merge::Always;
  }
  hold( std::move( entry ), now, link_interval, backlog, ready );
}

void PointerCollector::collect_pointer( const std::string& frame, uint64_t now, uint64_t link_interval, bool backlog,
                                        std::vector<Item>& ready )
{
  Held entry;
  entry.item.bytes = entry.item.direct = true;
  entry.item.raw = frame;
  if ( !Ext::parse( Ext::frame_payload( frame ), entry.frame )
       || ( entry.policy = merge_policy( entry.frame, entry.contact, entry.compatible ) ) == Merge::Never ) {
    flush( ready );
    ready.push_back( entry.item );
    return;
  }
  hold( std::move( entry ), now, link_interval, backlog, ready );
}

void PointerCollector::release( uint64_t now, uint64_t link_interval, bool backlog, std::vector<Item>& ready )
{
  if ( held.empty() || backlog || now < next_release ) {
    return;
  }
  next_release = now + link_interval;
  flush( ready );
}

void PointerCollector::flush( std::vector<Item>& ready )
{
  for ( Held& entry : held ) {
    emit( entry, ready );
  }
  held.clear();
}

int PointerCollector::wait_time( uint64_t now, bool backlog ) const
{
  /* With a backlog the link wakes the client when it has taken it. */
  if ( held.empty() || backlog ) {
    return INT_MAX;
  }
  return now >= next_release ? 0 : int( next_release - now );
}

}
}
