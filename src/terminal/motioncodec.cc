/*
    Goblin Skiff: the reports of a hand as binary records on the wire
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/terminal/motioncodec.h"

#include <cstring>
#include <limits>

namespace Terminal {
namespace Motion {

namespace {

/* The records. */
const uint64_t LITERAL = 0, SHAPE = 1, INK = 2, REPORT = 3;
/* A literal report's fields. */
const uint64_t HAS_PAYLOAD = 1, HAS_INK = 2, HAS_REPLY = 4, HAS_CLOCK = 8;
/* The low bits of a coded report's head; the track is above them. */
const uint64_t REPORT_CLOCK = 1, REPORT_INK = 2;
const unsigned REPORT_TRACK_SHIFT = 2;

/* Whether `text` is an integer written the one plain way, so that writing the
   integer back gives the text. */
bool canonical_integer( const std::string& text, int64_t& value )
{
  if ( text.empty() || text.size() > 20 ) {
    return false;
  }
  size_t i = 0;
  const bool negative = text[0] == '-';
  if ( negative ) {
    i = 1;
  }
  if ( i >= text.size() ) {
    return false;
  }
  uint64_t magnitude = 0;
  for ( ; i < text.size(); i++ ) {
    const char c = text[i];
    if ( c < '0' || c > '9' ) {
      return false;
    }
    const unsigned digit = unsigned( c - '0' );
    if ( magnitude > ( std::numeric_limits<uint64_t>::max() - digit ) / 10 ) {
      return false;
    }
    magnitude = magnitude * 10 + digit;
  }
  if ( negative ) {
    if ( magnitude > uint64_t( std::numeric_limits<int64_t>::max() ) + 1 ) {
      return false;
    }
    value = int64_t( 0 - magnitude ); /* two's complement: 2^63 becomes the minimum */
  } else {
    if ( magnitude > uint64_t( std::numeric_limits<int64_t>::max() ) ) {
      return false;
    }
    value = int64_t( magnitude );
  }
  return std::to_string( value ) == text;
}

void put_string( std::string& out, const std::string& s )
{
  put_unsigned( out, s.size() );
  out += s;
}

bool get_bytes( const std::string& in, size_t& pos, uint64_t length, std::string& s )
{
  if ( length > in.size() - pos ) {
    return false;
  }
  s.assign( in, pos, size_t( length ) );
  pos += size_t( length );
  return true;
}

bool get_string( const std::string& in, size_t& pos, std::string& s )
{
  uint64_t length;
  return get_unsigned( in, pos, length ) && get_bytes( in, pos, length, s );
}

void put_double( std::string& out, double d )
{
  uint64_t bits;
  static_assert( sizeof bits == sizeof d, "a double is 64 bits" );
  memcpy( &bits, &d, sizeof bits );
  for ( int i = 0; i < 8; i++ ) {
    out += char( ( bits >> ( 8 * i ) ) & 0xff );
  }
}

bool get_double( const std::string& in, size_t& pos, double& d )
{
  if ( in.size() - pos < 8 ) {
    return false;
  }
  uint64_t bits = 0;
  for ( int i = 0; i < 8; i++ ) {
    bits |= uint64_t( static_cast<unsigned char>( in[pos + i] ) ) << ( 8 * i );
  }
  pos += 8;
  memcpy( &d, &bits, sizeof d );
  return true;
}

/* The checked steps of a prediction. */
bool mul( int64_t a, int64_t b, int64_t& out )
{
  return !__builtin_mul_overflow( a, b, &out );
}
bool add( int64_t a, int64_t b, int64_t& out )
{
  return !__builtin_add_overflow( a, b, &out );
}
bool sub( int64_t a, int64_t b, int64_t& out )
{
  return !__builtin_sub_overflow( a, b, &out );
}
/* n / d to the nearest integer, halves away from zero. */
bool round_div( int64_t n, int64_t d, int64_t& out )
{
  if ( d <= 0 ) {
    return false;
  }
  int64_t q = n / d;
  const int64_t r = n % d;
  int64_t twice;
  if ( !mul( r, 2, twice ) ) {
    return false;
  }
  if ( twice >= d ) {
    if ( !add( q, 1, q ) ) {
      return false;
    }
  } else if ( twice <= -d ) {
    if ( !sub( q, 1, q ) ) {
      return false;
    }
  }
  out = q;
  return true;
}

/* The contact a report belongs to, as far as prediction goes. */
std::string track_key( const Ext::Frame& frame )
{
  std::string key = frame.selector;
  for ( const char* name : { "kind", "device", "contact", "src" } ) {
    key += '\0';
    if ( const std::string* value = frame.fields.find( name ) ) {
      key += *value;
    }
  }
  return key;
}

int time_index_of( const std::vector<std::string>& keys, const std::vector<bool>& numeric )
{
  for ( size_t i = 0; i < keys.size(); i++ ) {
    if ( numeric[i] && keys[i] == "t" ) {
      return int( i );
    }
  }
  return -1;
}

/* What the reports so far say the clock of a report should read: the clock of
   the last, moved on by as much as the report's own time has. */
int64_t clock_prediction( const RecordContext& context, bool has_time, int64_t when )
{
  if ( !context.have_clock ) {
    return 0;
  }
  int64_t prediction = context.clock;
  if ( context.have_time && has_time ) {
    prediction = int64_t( uint64_t( prediction ) + ( uint64_t( when ) - uint64_t( context.time ) ) );
  }
  return prediction;
}

/* The ink of a track, as a report decoded against it finds it. */
bool ink_of( const RecordTrack& track, ExtensionInput& report )
{
  if ( !track.has_ink ) {
    return false;
  }
  report.ink_image = track.ink_image;
  report.ink = track.ink;
  return true;
}

}

void put_unsigned( std::string& out, uint64_t value )
{
  while ( value >= 0x80 ) {
    out += char( ( value & 0x7f ) | 0x80 );
    value >>= 7;
  }
  out += char( value );
}

void put_signed( std::string& out, int64_t value )
{
  put_unsigned( out, ( uint64_t( value ) << 1 ) ^ uint64_t( value >> 63 ) );
}

bool get_unsigned( const std::string& in, size_t& pos, uint64_t& value )
{
  value = 0;
  for ( unsigned shift = 0; shift < 64; shift += 7 ) {
    if ( pos >= in.size() ) {
      return false;
    }
    const unsigned char byte = static_cast<unsigned char>( in[pos++] );
    if ( shift == 63 && byte > 1 ) {
      return false;
    }
    value |= uint64_t( byte & 0x7f ) << shift;
    if ( !( byte & 0x80 ) ) {
      return true;
    }
  }
  return false;
}

bool get_signed( const std::string& in, size_t& pos, int64_t& value )
{
  uint64_t raw;
  if ( !get_unsigned( in, pos, raw ) ) {
    return false;
  }
  value = int64_t( raw >> 1 ) ^ -int64_t( raw & 1 );
  return true;
}

void Series::push( int64_t time, int64_t value )
{
  if ( count == 3 ) {
    t[0] = t[1];
    v[0] = v[1];
    t[1] = t[2];
    v[1] = v[2];
    count = 2;
  }
  t[count] = time;
  v[count] = value;
  count++;
}

int64_t Series::predict( int64_t time, int order ) const
{
  if ( count == 0 ) {
    return 0;
  }
  const int64_t last_t = t[count - 1], last_v = v[count - 1];
  int64_t dt;
  if ( count == 1 || time <= last_t || !sub( time, last_t, dt ) ) {
    return last_v;
  }
  if ( order >= 2 && count == 3 ) {
    /* The quadratic through the three, in Newton's form, over the common
       denominator A B C of its divided differences. */
    int64_t A, B, C, dv1, dv0, s, n1, n2, m1, m2, N, D, q;
    if ( sub( t[1], t[0], A ) && sub( t[2], t[1], B ) && sub( t[2], t[0], C ) && A > 0 && B > 0 && sub( v[2], v[1], dv1 )
         && sub( v[1], v[0], dv0 ) && add( dt, B, s ) && mul( dv1, dt, n1 ) && mul( n1, A, n1 ) && mul( n1, C, n1 )
         && mul( dv1, A, m1 ) && mul( dv0, B, m2 ) && sub( m1, m2, m1 ) && mul( m1, dt, n2 ) && mul( n2, s, n2 )
         && add( n1, n2, N ) && mul( A, B, D ) && mul( D, C, D ) && round_div( N, D, q ) && add( last_v, q, q ) ) {
      return q;
    }
  }
  int64_t dT, dV, n, q;
  if ( sub( t[count - 1], t[count - 2], dT ) && dT > 0 && sub( v[count - 1], v[count - 2], dV ) && mul( dV, dt, n )
       && round_div( n, dT, q ) && add( last_v, q, q ) ) {
    return q;
  }
  return last_v;
}

int prediction_order( const std::string& key )
{
  return key == "x" || key == "y" ? 2 : 1;
}

bool RecordContext::prepare( const ExtensionInput& input, Prepared& p ) const
{
  if ( input.payload.empty() || !input.service_reply.empty() || !Ext::parse( input.payload, p.frame )
       || "777;" + p.frame.selector + ";" + p.frame.fields.text() != input.payload
       || input.ink.present != ( input.ink_image != 0 ) ) { /* ink a record can render has a map and an image */
    return false;
  }
  const Ext::Fields& fields = p.frame.fields;
  const size_t n = fields.items.size();
  p.values.assign( n, 0 );
  p.texts.assign( n, std::string() );
  p.numeric.assign( n, false );
  p.residual.assign( n, 0 );
  p.changed.assign( n, false );
  std::vector<std::string> keys( n );
  for ( size_t i = 0; i < n; i++ ) {
    keys[i] = fields.items[i].first;
    p.texts[i] = fields.items[i].second;
    p.numeric[i] = canonical_integer( p.texts[i], p.values[i] );
  }

  /* The shape */
  p.shape = shapes.size();
  p.new_shape = true;
  for ( size_t s = 0; s < shapes.size() && p.new_shape; s++ ) {
    const RecordShape& shape = shapes[s];
    if ( shape.selector != p.frame.selector || shape.keys != keys || shape.numeric != p.numeric ) {
      continue;
    }
    p.shape = s;
    p.new_shape = false;
  }
  const int time_index = p.new_shape ? time_index_of( keys, p.numeric ) : shapes[p.shape].time_index;
  p.time_index = time_index;
  p.has_time = time_index >= 0;

  /* The track */
  const auto found = track_index.find( track_key( p.frame ) );
  p.track = found == track_index.end() ? tracks.size() : found->second;
  const RecordTrack* track = p.track < tracks.size() ? &tracks[p.track] : nullptr;
  const uint64_t samples = track ? track->samples : 0;

  /* The ink */
  p.has_ink = input.ink.present;
  p.new_ink = p.has_ink
              && ( !track || !track->has_ink || track->ink_image != input.ink_image || !( track->ink == input.ink ) );

  /* The predictions */
  p.when = p.has_time ? p.values[time_index] : int64_t( samples );
  for ( size_t i = 0; i < n; i++ ) {
    if ( p.numeric[i] ) {
      int64_t prediction = 0;
      if ( track ) {
        const auto series = track->series.find( keys[i] );
        if ( series != track->series.end() ) {
          prediction = series->second.predict( int( i ) == time_index ? int64_t( samples ) : p.when,
                                               prediction_order( keys[i] ) );
        }
      }
      p.residual[i] = uint64_t( p.values[i] ) - uint64_t( prediction );
      p.changed[i] = p.residual[i] != 0;
    } else {
      const auto previous = track ? track->text.find( keys[i] ) : std::map<std::string, std::string>::const_iterator();
      p.changed[i] = !track || previous == track->text.end() || previous->second != p.texts[i];
    }
  }
  p.has_clock = input.clock >= 0;
  p.clock_residual
    = p.has_clock ? int64_t( uint64_t( input.clock ) - uint64_t( clock_prediction( *this, p.has_time, p.when ) ) ) : 0;
  return true;
}

void RecordContext::commit( const ExtensionInput& input, const Prepared& p )
{
  if ( p.new_shape ) {
    RecordShape shape;
    shape.selector = p.frame.selector;
    for ( const auto& item : p.frame.fields.items ) {
      shape.keys.push_back( item.first );
    }
    shape.numeric = p.numeric;
    shape.time_index = time_index_of( shape.keys, shape.numeric );
    shapes.push_back( shape );
  }
  if ( p.track == tracks.size() ) {
    tracks.emplace_back();
  }
  track_index.emplace( track_key( p.frame ), p.track );
  RecordTrack& track = tracks[p.track];
  const RecordShape& shape = shapes[p.shape];
  if ( p.has_ink && p.new_ink ) {
    track.has_ink = true;
    track.ink_image = input.ink_image;
    track.ink = input.ink;
  }
  for ( size_t i = 0; i < shape.keys.size(); i++ ) {
    if ( shape.numeric[i] ) {
      track.series[shape.keys[i]].push( int( i ) == shape.time_index ? int64_t( track.samples ) : p.when, p.values[i] );
    } else {
      track.text[shape.keys[i]] = p.texts[i];
    }
  }
  track.samples++;
  if ( p.has_clock ) {
    have_clock = true;
    clock = input.clock;
  }
  if ( p.has_time ) {
    have_time = true;
    time = p.when;
  }
}

void RecordContext::note( const ExtensionInput& input )
{
  Prepared p;
  if ( prepare( input, p ) ) {
    commit( input, p );
  }
}

void RecordEncoder::literal( const ExtensionInput& input )
{
  put_unsigned( out, LITERAL );
  put_unsigned( out,
                ( input.payload.empty() ? 0 : HAS_PAYLOAD ) | ( input.ink_image ? HAS_INK : 0 )
                  | ( input.service_reply.empty() ? 0 : HAS_REPLY ) | ( input.clock >= 0 ? HAS_CLOCK : 0 ) );
  if ( !input.payload.empty() ) {
    put_string( out, input.payload );
  }
  if ( input.ink_image ) {
    put_unsigned( out, input.ink_image );
    put_string( out, input.ink_records );
  }
  if ( !input.service_reply.empty() ) {
    put_string( out, input.service_reply );
  }
  if ( input.clock >= 0 ) {
    put_signed( out, input.clock );
  }
}

void RecordEncoder::add( const ExtensionInput& input )
{
  added++;
  RecordContext::Prepared p;
  if ( !context.prepare( input, p ) ) {
    literal( input );
    return;
  }
  const Ext::Fields& fields = p.frame.fields;
  const size_t n = fields.items.size();
  if ( p.new_shape ) {
    put_unsigned( out, SHAPE );
    put_string( out, p.frame.selector );
    put_unsigned( out, n );
    for ( size_t i = 0; i < n; i++ ) {
      put_unsigned( out, ( uint64_t( fields.items[i].first.size() ) << 1 ) | ( p.numeric[i] ? 1 : 0 ) );
      out += fields.items[i].first;
    }
  }
  if ( p.has_ink && p.new_ink ) {
    const InkMap& map = input.ink;
    put_unsigned( out, INK );
    put_unsigned( out, p.track );
    put_unsigned( out, input.ink_image );
    put_unsigned( out, map.record == 'k' ? 1 : 2 );
    put_unsigned( out, map.element );
    put_unsigned( out, map.group );
    put_string( out, map.fill );
    put_unsigned( out, map.pressure ? 1 : 0 );
    for ( double d : { map.a, map.b, map.c, map.d, map.e, map.f, map.w0, map.w1, map.exponent } ) {
      put_double( out, d );
    }
  }
  const int t = p.time_index;
  put_unsigned( out, REPORT );
  put_unsigned( out,
                ( uint64_t( p.track ) << REPORT_TRACK_SHIFT ) | ( p.has_clock ? REPORT_CLOCK : 0 )
                  | ( p.has_ink ? REPORT_INK : 0 ) );
  put_unsigned( out, p.shape );
  for ( size_t byte = 0; byte < ( n + 7 ) / 8; byte++ ) {
    unsigned char mask = 0;
    for ( size_t bit = 0; bit < 8 && byte * 8 + bit < n; bit++ ) {
      if ( p.changed[byte * 8 + bit] ) {
        mask |= static_cast<unsigned char>( 1u << bit );
      }
    }
    out += char( mask );
  }
  if ( p.has_clock ) {
    put_signed( out, p.clock_residual );
  }
  if ( t >= 0 && p.changed[t] ) {
    put_signed( out, int64_t( p.residual[t] ) );
  }
  for ( size_t i = 0; i < n; i++ ) {
    if ( int( i ) == t || !p.changed[i] ) {
      continue;
    }
    if ( p.numeric[i] ) {
      put_signed( out, int64_t( p.residual[i] ) );
    } else {
      put_string( out, p.texts[i] );
    }
  }
  context.commit( input, p );
}

bool decode_records( const std::string& in, RecordContext& context, std::vector<ExtensionInput>& out )
{
  size_t pos = 0;
  while ( pos < in.size() ) {
    uint64_t type;
    if ( !get_unsigned( in, pos, type ) ) {
      return false;
    }
    if ( type == LITERAL ) {
      uint64_t flags;
      if ( !get_unsigned( in, pos, flags ) ) {
        return false;
      }
      ExtensionInput report;
      if ( ( flags & HAS_PAYLOAD ) && !get_string( in, pos, report.payload ) ) {
        return false;
      }
      if ( flags & HAS_INK ) {
        uint64_t image;
        if ( !get_unsigned( in, pos, image ) || image > std::numeric_limits<uint32_t>::max()
             || !get_string( in, pos, report.ink_records ) ) {
          return false;
        }
        report.ink_image = uint32_t( image );
      }
      if ( ( flags & HAS_REPLY ) && !get_string( in, pos, report.service_reply ) ) {
        return false;
      }
      if ( ( flags & HAS_CLOCK ) && !get_signed( in, pos, report.clock ) ) {
        return false;
      }
      out.push_back( report );
    } else if ( type == SHAPE ) {
      RecordShape shape;
      uint64_t n;
      if ( !get_string( in, pos, shape.selector ) || !get_unsigned( in, pos, n ) || n > in.size() - pos ) {
        return false;
      }
      for ( uint64_t i = 0; i < n; i++ ) {
        uint64_t head;
        std::string name;
        if ( !get_unsigned( in, pos, head ) || !get_bytes( in, pos, head >> 1, name ) ) {
          return false;
        }
        shape.keys.push_back( name );
        shape.numeric.push_back( head & 1 );
      }
      shape.time_index = time_index_of( shape.keys, shape.numeric );
      context.shapes.push_back( shape );
    } else if ( type == INK ) {
      uint64_t track_i, image, record, element, group, pressure;
      std::string fill;
      if ( !get_unsigned( in, pos, track_i ) || track_i > context.tracks.size() || !get_unsigned( in, pos, image )
           || image > std::numeric_limits<uint32_t>::max() || !image || !get_unsigned( in, pos, record ) || record < 1
           || record > 2 || !get_unsigned( in, pos, element ) || element > std::numeric_limits<uint32_t>::max()
           || !get_unsigned( in, pos, group ) || group > std::numeric_limits<uint32_t>::max() || !get_string( in, pos, fill )
           || !get_unsigned( in, pos, pressure ) || pressure > 1 ) {
        return false;
      }
      if ( track_i == context.tracks.size() ) {
        context.tracks.emplace_back();
      }
      RecordTrack& track = context.tracks[track_i];
      track.has_ink = true;
      track.ink_image = uint32_t( image );
      track.ink = InkMap();
      track.ink.present = true;
      track.ink.record = record == 1 ? 'k' : 'a';
      track.ink.element = uint32_t( element );
      track.ink.group = uint32_t( group );
      track.ink.fill = fill;
      track.ink.pressure = pressure == 1;
      for ( double* d : { &track.ink.a, &track.ink.b, &track.ink.c, &track.ink.d, &track.ink.e, &track.ink.f, &track.ink.w0,
                          &track.ink.w1, &track.ink.exponent } ) {
        if ( !get_double( in, pos, *d ) ) {
          return false;
        }
      }
    } else if ( type == REPORT ) {
      uint64_t head, shape_index;
      if ( !get_unsigned( in, pos, head ) || !get_unsigned( in, pos, shape_index ) || shape_index >= context.shapes.size() ) {
        return false;
      }
      const uint64_t track_i = head >> REPORT_TRACK_SHIFT;
      if ( track_i > context.tracks.size() ) {
        return false;
      }
      if ( track_i == context.tracks.size() ) {
        context.tracks.emplace_back();
      }
      RecordTrack& track = context.tracks[track_i];
      const RecordShape& shape = context.shapes[shape_index];
      const size_t n = shape.keys.size();
      const size_t mask_bytes = ( n + 7 ) / 8;
      if ( in.size() - pos < mask_bytes ) {
        return false;
      }
      RecordContext::Prepared p;
      p.shape = shape_index;
      p.track = track_i;
      p.numeric = shape.numeric;
      p.changed.assign( n, false );
      p.values.assign( n, 0 );
      p.texts.assign( n, std::string() );
      for ( size_t i = 0; i < n; i++ ) {
        p.changed[i] = ( static_cast<unsigned char>( in[pos + i / 8] ) >> ( i % 8 ) ) & 1;
      }
      pos += mask_bytes;
      p.has_clock = head & REPORT_CLOCK;
      p.has_ink = head & REPORT_INK;
      if ( p.has_clock && !get_signed( in, pos, p.clock_residual ) ) {
        return false;
      }
      p.time_index = shape.time_index;
      p.has_time = shape.time_index >= 0;
      p.when = int64_t( track.samples );
      if ( p.has_time ) {
        int64_t residual = 0;
        if ( p.changed[shape.time_index] && !get_signed( in, pos, residual ) ) {
          return false;
        }
        const int64_t prediction = track.series[shape.keys[shape.time_index]].predict( int64_t( track.samples ), 1 );
        p.when = p.values[shape.time_index] = int64_t( uint64_t( prediction ) + uint64_t( residual ) );
      }
      for ( size_t i = 0; i < n; i++ ) {
        if ( int( i ) == shape.time_index ) {
          continue;
        }
        if ( shape.numeric[i] ) {
          int64_t residual = 0;
          if ( p.changed[i] && !get_signed( in, pos, residual ) ) {
            return false;
          }
          const int64_t prediction = track.series[shape.keys[i]].predict( p.when, prediction_order( shape.keys[i] ) );
          p.values[i] = int64_t( uint64_t( prediction ) + uint64_t( residual ) );
        } else if ( p.changed[i] ) {
          if ( !get_string( in, pos, p.texts[i] ) ) {
            return false;
          }
        } else {
          const auto previous = track.text.find( shape.keys[i] );
          if ( previous == track.text.end() ) {
            return false;
          }
          p.texts[i] = previous->second;
        }
      }
      p.frame.selector = shape.selector;
      for ( size_t i = 0; i < n; i++ ) {
        p.frame.fields.items.emplace_back( shape.keys[i], shape.numeric[i] ? std::to_string( p.values[i] ) : p.texts[i] );
      }
      ExtensionInput report( "777;" + shape.selector + ";" + p.frame.fields.text() );
      if ( p.has_clock ) {
        report.clock
          = int64_t( uint64_t( clock_prediction( context, p.has_time, p.when ) ) + uint64_t( p.clock_residual ) );
      }
      if ( p.has_ink ) {
        if ( !ink_of( track, report ) ) {
          return false;
        }
        report.ink_records = report.ink.render( integer_or_zero( p.frame.fields, "x" ), integer_or_zero( p.frame.fields, "y" ),
                                                integer_or_zero( p.frame.fields, "p" ) );
      }
      context.commit( report, p );
      out.push_back( report );
    } else {
      return false;
    }
  }
  return true;
}

}
}
