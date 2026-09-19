/*
    Modified for Goblin Skiff on 2026-09-19.
 Distributed under the GNU GPL, version 3 or later. */
#ifndef MOSH_LINKBUDGET_H
#define MOSH_LINKBUDGET_H

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>

namespace Network {
// A conservative, per-direction delivery controller, not a capacity oracle.
// No padding probes, idle reports or symmetry assumption. The existing
// authenticated packet nonce supplies sequence numbers at no per-data cost.
class LinkBudget
{
  struct Sent { uint64_t seq, at, total_bytes; };
  std::deque<Sent> sent;
  bool enabled = false, receiving = false, report_pending = false;
  uint64_t highest = 0, mask = 0, report_due = 0, rx_first = 0, rx_last = 0, rx_bytes = 0;
  uint64_t feedback_at = 0, demand_at = 0, last_sent = 0, last_data_sent = 0, last_received = 0, last_cut = 0;
  uint64_t tx_epoch = 0, tx_bytes = 0, rx_epoch = 0, traffic_rx_bytes = 0;
  double rate = 240, peer_rate = 0, next_send = 0, base_rtt = 0, loss = 0;
  double upload = 0, download = 0;
  size_t largest_packet = 1280;
  bool measured = false, cautious = false;
  unsigned report_packets = 0;
  uint64_t last_growth = 0, latest_delivery_sent = 0;
  uint64_t total_sent_bytes = 0, latest_delivery_bytes = 0;
  uint64_t last_report_sent = 0;
  uint64_t report_ack = 0, report_mask = 0;
  bool have_report = false;
  unsigned feedback_packet_size = 128;
  bool radio = false;

  static void put( std::string& out, uint64_t n, unsigned bytes )
  { for ( int shift = int( bytes * 8 ) - 8; shift >= 0; shift -= 8 ) { out += char( n >> shift ); } }
  static uint64_t get( const std::string& in, size_t& at, unsigned bytes )
  { uint64_t n = 0; while ( bytes-- ) { n = ( n << 8 ) | uint8_t( in[at++] ); } return n; }
  void set_rate( double value, uint64_t now )
  {
    const double debt = std::max( 0.0, next_send - now ) * rate / 1000;
    rate = std::max( 64.0, std::min( radio ? 128.0 : 12500000.0, value ) );
    next_send = now + debt * 1000 / rate;
  }

public:
  void enable( bool value ) { enabled = value; }
  // A goTenna packet consumes airtime even when almost empty. Limit each
  // direction to at most one packet per 1.5 s, including ACKs and reports.
  // Congestion can reduce this further; idle/growth must preserve the cap.
  void enable_radio() { radio = true; rate = 128; largest_packet = 192; }
  void set_feedback_packet_size( unsigned bytes ) { feedback_packet_size = std::max( 128u, bytes ); }
  bool active() const { return enabled; }
  double budget() const { return rate; }
  double remote_budget() const { return peer_rate; }
  double loss_fraction() const { return loss; }
  bool has_measurement() const { return measured; }
  double upload_rate( uint64_t now ) const { return now - last_sent > 3000 ? 0 : upload; }
  double download_rate( uint64_t now ) const { return now - last_received > 3000 ? 0 : download; }

  // One millisecond of scheduling slack, capped at 4 KiB, avoids a one-
  // packet-per-millisecond ceiling on fast links. At satellite rates this
  // is less than one byte. Idle time never accumulates burst credit.
  // Foreground input/ACKs can additionally borrow at most 128 bytes.
  int wait_time( uint64_t now, bool foreground = false )
  {
    if ( !enabled ) { return 0; }
    if ( last_data_sent && now - last_data_sent > 30000 ) {
      set_rate( 240, now ); next_send = now; measured = cautious = false;
      sent.clear(); feedback_at = latest_delivery_sent = latest_delivery_bytes = 0; last_data_sent = now;
      base_rtt = loss = 0; last_cut = last_growth = 0;
    }
    // Bound an abrupt path downgrade: do not keep pouring background data
    // into a dead/slow path while its delivery feedback is still in flight.
    // Control packets can still get through and release the window.
    if ( !foreground && sent.size() >= 512 ) { demand_at = now; return 100; }
    const double slack = measured ? std::min( 1.0, 4096000 / rate ) : 0;
    const double due = next_send - ( radio ? 0 : slack + ( foreground ? 128000 / rate : 0 ) );
    if ( due <= now ) { return 0; }
    demand_at = now;
    return int( std::min( double( INT_MAX ), std::ceil( due - now ) ) );
  }

  void sent_packet( uint64_t seq, size_t bytes, uint64_t now, bool feedback = false )
  {
    if ( !enabled ) { return; }
    last_sent = now;
    if ( !tx_epoch ) { tx_epoch = now; }
    tx_bytes += bytes;
    if ( now >= tx_epoch + 1000 ) { upload = tx_bytes * 1000.0 / std::max<uint64_t>( 1, now - tx_epoch ); tx_epoch = now; tx_bytes = 0; }
    next_send = std::max( double( now ), next_send ) + ( radio ? std::max<size_t>( 192, bytes ) : bytes ) * 1000.0 / rate;
    if ( feedback ) { return; }
    total_sent_bytes += bytes;
    last_data_sent = now;
    largest_packet = std::max( largest_packet, bytes );
    sent.push_back( { seq, now, total_sent_bytes } );
    while ( sent.size() > 512 ) { sent.pop_front(); }
    if ( !feedback_at ) { feedback_at = now; }
  }

  void received_packet( uint64_t seq, size_t bytes, uint64_t now, double rtt )
  {
    if ( !enabled ) { return; }
    if ( receiving && seq <= highest && ( highest - seq >= 64 || ( mask & ( uint64_t( 1 ) << ( highest - seq ) ) ) ) ) { return; }
    if ( !receiving || seq > highest ) {
      const uint64_t delta = receiving ? seq - highest : 64;
      mask = delta >= 64 ? 1 : ( mask << delta ) | 1;
      highest = seq;
    } else { mask |= uint64_t( 1 ) << ( highest - seq ); }
    receiving = true;
    last_received = now;
    if ( !rx_epoch ) { rx_epoch = now; }
    traffic_rx_bytes += bytes;
    if ( now >= rx_epoch + 1000 ) { download = traffic_rx_bytes * 1000.0 / std::max<uint64_t>( 1, now - rx_epoch ); rx_epoch = now; traffic_rx_bytes = 0; }
    if ( !report_pending ) {
      // Keep the preceding arrival as the start of the delivery interval,
      // but do not interpret an idle gap as a slow link.
      if ( !rx_last || now - rx_last > 10000 ) { rx_first = now; rx_bytes = 0; }
      else { rx_first = rx_last; rx_bytes = bytes; }
      report_due = now + uint64_t( std::max( 50.0, std::min( 3000.0, 2 * rtt ) ) );
      report_packets = 0;
    } else { rx_bytes += bytes; }
    rx_last = now;
    report_pending = true;
    // Request earlier feedback to keep the selective ACK window useful at
    // high packet rates. feedback_wait still limits its reverse-path share.
    if ( ++report_packets >= 16 ) { report_due = now; }
  }

  int feedback_wait( uint64_t now ) const
  {
    if ( !enabled || !report_pending ) { return INT_MAX; }
    // Fast forward traffic must not turn feedback into a flood on a slow
    // reverse path or starve keyboard/SST ACKs. Reserve at most a quarter
    // of this direction's budget, including any outer jump-relay envelopes.
    const uint64_t due = std::max( report_due, last_report_sent
      ? last_report_sent + uint64_t( std::ceil( 4000.0 * feedback_packet_size / rate ) ) : 0 );
    return now >= due ? 0 : int( std::min<uint64_t>( INT_MAX, due - now ) );
  }

  std::string feedback( uint64_t now )
  {
    if ( feedback_wait( now ) ) { return {}; }
    std::string out( "\0GL1", 4 );
    put( out, highest, 8 ); put( out, mask, 8 );
    put( out, std::min<uint64_t>( UINT32_MAX, rx_last - rx_first ), 4 );
    put( out, std::min<uint64_t>( UINT32_MAX, rx_bytes ), 4 ); put( out, uint32_t( rate ), 4 );
    report_pending = false;
    last_report_sent = now;
    return out;
  }

  bool receive_feedback( const std::string& in, uint64_t now, double rtt )
  {
    if ( !enabled || in.size() != 32 || in.compare( 0, 4, std::string( "\0GL1", 4 ) ) ) { return false; }
    size_t at = 4;
    const uint64_t ack = get( in, at, 8 ), bits = get( in, at, 8 ), elapsed = get( in, at, 4 ), bytes = get( in, at, 4 );
    const auto advertised = get( in, at, 4 );
    if ( advertised < 64 || advertised > 12500000 ) { return true; }
    peer_rate = advertised;
    if ( sent.empty() || ack > sent.back().seq ) { return true; }
    const bool fresh = !have_report || ack > report_ack || ( ack == report_ack && ( bits & ~report_mask ) );
    if ( fresh ) { report_mask = ack == report_ack ? report_mask | bits : bits; report_ack = ack; have_report = true; }
    double recent_send_rate = 0;
    if ( fresh && sent.size() > 1 && sent.back().at > sent.front().at ) {
      recent_send_rate = ( sent.back().total_bytes - sent.front().total_bytes ) * 1000.0
                        / ( sent.back().at - sent.front().at );
    }
    unsigned delivered = 0, missing = 0;
    uint64_t newest_bytes = latest_delivery_bytes, newest_sent = latest_delivery_sent;
    for ( auto item = sent.begin(); item != sent.end(); ) {
      if ( item->seq > ack ) { break; }
      const auto distance = ack - item->seq;
      if ( distance >= 64 ) {
        // Outside a selective report's bitmap is unknown, not lost. Fast
        // forward traffic can outrun feedback on an asymmetric reverse link.
        item = sent.erase( item );
      } else if ( bits & ( uint64_t( 1 ) << distance ) ) {
        if ( item->total_bytes > newest_bytes ) { newest_bytes = item->total_bytes; newest_sent = item->at; }
        ++delivered; item = sent.erase( item );
      } else if ( distance >= 3 && now - item->at > std::max( 1500.0, 2 * rtt ) ) {
        ++missing; item = sent.erase( item );
      } else { ++item; }
    }
    if ( !fresh ) { return true; } // Replayed/stale reports cannot change the send budget.
    feedback_at = now;
    base_rtt = base_rtt ? std::min( base_rtt, rtt ) : rtt;
    const double sample_loss = delivered + missing ? double( missing ) / ( missing + delivered ) : 0;
    loss = .75 * loss + .25 * sample_loss;
    const double delivery = elapsed ? bytes * 1000.0 / elapsed : 0;
    // Compare the same byte population at both ends: include lost data,
    // but not feedback, which does not solicit reports. Including feedback
    // here would misclassify sparse ACK traffic as a slow physical link.
    const double send_rate = latest_delivery_sent && newest_bytes > latest_delivery_bytes
      ? ( newest_bytes - latest_delivery_bytes ) * 1000.0 / std::max<uint64_t>( 1, newest_sent - latest_delivery_sent ) : recent_send_rate;
    latest_delivery_sent = newest_sent; latest_delivery_bytes = newest_bytes;
    const bool demand = demand_at && now >= demand_at && now - demand_at < std::max( 5000.0, 4 * rtt );
    const bool queued = rtt > base_rtt + std::max( 150.0, base_rtt * .5 );
    // A sustained arrival rate far below the actual departure rate is a
    // slow-path signal even before a large router queue starts dropping.
    // Application/CPU-limited traffic must not be mistaken for congestion.
    const bool serialized = demand && elapsed >= std::max( 250.0, rtt ) && delivery > 0
      && delivery < rate * .65 && send_rate > delivery * 1.5
      && ( ( send_rate >= rate * .75 && bytes >= 1024 ) || ( send_rate > delivery * 4 && bytes >= 128 ) )
      && now - last_growth >= std::max( 500.0, 2 * rtt );
    // Sparse ACK/control traffic is application-limited: a lost small
    // packet is not evidence that the entire unused budget was too large.
    // Random loss makes growth cautious; cutting also needs a substantial
    // delivery shortfall while the sender is using most of its budget.
    const bool loss_congestion = missing && demand && elapsed >= 250 && bytes >= 128
      && send_rate >= rate * .75 && delivery < send_rate * .75;
    if ( missing ) { cautious = true; }
    if ( loss_congestion || queued || serialized ) {
      cautious = true;
      if ( now - last_cut >= std::max( 250.0, rtt ) ) {
        // Loss alone should not collapse an unmeasured path below the safe
        // startup budget. Going lower needs a meaningful delivery sample,
        // not one missing tiny ACK. A proven slower path can still go to 64.
        double target = std::min( rate, std::max( 240.0, rate * .8 ) );
        if ( serialized ) { target = std::min( rate * .5, delivery * .9 ); }
        set_rate( target, now ); last_cut = now;
      }
    } else if ( demand && delivery >= rate * .5
                && now - last_growth >= std::max( cautious ? 200.0 : 100.0, rtt )
                && now - last_cut >= std::max( 250.0, rtt ) ) {
      set_rate( std::max( rate, std::min( rate * ( cautious ? 1.15 : 2.0 ), delivery * 2.0 ) ), now );
      last_growth = now;
    }
    measured = measured || elapsed > 0;
    return true;
  }

  void tick( uint64_t now, double rtt )
  {
    // On a 2.4 kbit/s path one full datagram alone takes several seconds.
    // Include serialization and report coalescing, not just propagation RTT.
    if ( enabled && !sent.empty() && feedback_at
         && now - feedback_at > std::max( 10000.0, 4 * rtt + 6000 + 2 * largest_packet * 1000 / rate )
         && now - last_cut >= 5000 ) {
      set_rate( rate * .5, now ); last_cut = now; measured = false;
      if ( sent.size() >= 512 ) { sent.pop_front(); } // One bounded recovery opportunity.
    }
  }
};
}
#endif
