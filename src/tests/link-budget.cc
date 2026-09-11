/* Distributed under the GNU GPL, version 3 or later. */
#include "src/network/linkbudget.h"
#include <iostream>
#include <queue>
#include <stdexcept>

static void require( bool yes, const char* what ) { if ( !yes ) { throw std::runtime_error( what ); } }

// Two independent serialization queues: actual wire bytes, asymmetric
// capacity, finite buffering, 600 ms one-way propagation, no real sleeps.
static void simulate( double upstream, double downstream, unsigned latency = 600, bool downgrade = false )
{
  Network::LinkBudget endpoints[2];
  for ( auto& endpoint : endpoints ) { endpoint.enable( true ); }
  struct Event {
    uint64_t time, seq; unsigned from; size_t bytes; std::string report;
    bool operator<( const Event& other ) const { return time > other.time; }
  };
  std::priority_queue<Event> events;
  double free_at[2] = { 0, 0 }, capacities[2] = { upstream, downstream };
  uint64_t sequences[2] = { 1, 1 }, delivered[2] = { 0, 0 }, dropped[2] = { 0, 0 };
  uint64_t ramp_at = 0, cut_at = 0;
  bool learned[2] = { false, false };
  const unsigned rtt = 2 * latency;
  const uint64_t duration = latency < 100 && !downgrade ? 30000 : 180000;
  const uint64_t count_from = duration * 2 / 3;
  for ( uint64_t now = 1; now <= duration; ++now ) {
    if ( downgrade && now == 30000 ) { capacities[0] = capacities[1] = 300; }
    while ( !events.empty() && events.top().time <= now ) {
      auto event = events.top(); events.pop();
      auto& destination = endpoints[1 - event.from];
      if ( event.report.empty() ) {
        destination.received_packet( event.seq, event.bytes, now, rtt );
        if ( now > count_from ) { delivered[event.from] += event.bytes; }
      } else { destination.receive_feedback( event.report, now, rtt ); }
    }
    for ( unsigned side = 0; side < 2; ++side ) {
      auto& endpoint = endpoints[side];
      learned[side] = learned[side] || endpoint.has_measurement();
      endpoint.tick( now, rtt );
      for ( unsigned burst = 0; burst < 32; ++burst ) {
        const bool feedback_ready = endpoint.feedback_wait( now ) == 0;
        if ( endpoint.wait_time( now, feedback_ready ) ) { break; }
        auto report = feedback_ready ? endpoint.feedback( now ) : std::string();
        const size_t bytes = report.empty() ? 400 : 88;
        const auto seq = sequences[side]++;
        endpoint.sent_packet( seq, bytes, now, !report.empty() );
        if ( free_at[side] > now + 500 ) { ++dropped[side]; continue; }
        free_at[side] = std::max( double( now ), free_at[side] ) + bytes * 1000.0 / capacities[side];
        events.push( { uint64_t( std::ceil( free_at[side] ) ) + latency, seq, side, bytes, report } );
      }
    }
    if ( !ramp_at && endpoints[0].budget() >= 1000000 ) { ramp_at = now; }
    if ( downgrade && now > 30000 && !cut_at && endpoints[0].budget() < 600 ) { cut_at = now; }
  }
  std::cout << "capacity " << upstream * .008 << "/" << downstream * .008 << " kbit/s; final budget "
            << endpoints[0].budget() * .008 << "/" << endpoints[1].budget() * .008
            << "; delivered final third " << delivered[0] * 8.0 / ( duration - count_from ) << "/" << delivered[1] * 8.0 / ( duration - count_from )
            << "; dropped " << dropped[0] << "/" << dropped[1] << "; 8 Mbit ramp ms " << ramp_at
            << "; slow-path cut ms " << ( cut_at ? cut_at - 30000 : 0 ) << '\n';
  for ( unsigned side = 0; side < 2; ++side ) {
    require( learned[side], "active path must learn a delivery estimate" );
    require( endpoints[side].budget() < capacities[side] * 2, "pacer must respond to congestion" );
    require( delivered[side] * 1000.0 / ( duration - count_from ) > std::min( capacities[side] * .35, 2000000.0 ),
             "adaptive rate must grow beyond the initial low-bandwidth budget" );
  }
  if ( !downgrade && upstream > downstream * 2 ) { require( endpoints[0].budget() > endpoints[1].budget() * 1.5, "directions must not share a bandwidth assumption" ); }
  if ( latency < 100 ) { require( ramp_at && ramp_at < 20000, "clean LAN must ramp to 8 Mbit/s within 20 seconds" ); }
  if ( downgrade ) { require( cut_at && cut_at < 45000, "obvious downgrade to 2.4 kbit/s must cut below 4.8 kbit/s within 15 seconds" ); }
}

// Foreground screen/ACK traffic has priority, as in the frontend loops. A
// lossy but unconstrained path must still discover room for background FEC;
// sparse feedback must not trap it permanently at the startup budget.
static void lossy_foreground()
{
  Network::LinkBudget endpoints[2];
  for ( auto& endpoint : endpoints ) { endpoint.enable( true ); }
  struct Event {
    uint64_t time, seq; unsigned from; size_t bytes; bool bulk; std::string report;
    bool operator<( const Event& other ) const { return time > other.time; }
  };
  std::priority_queue<Event> events;
  uint64_t sequences[2] = { 1, 1 }, last_screen[2] = { 0, 0 }, background[2] = { 0, 0 };
  unsigned packets = 0;
  for ( uint64_t now = 1; now <= 60000; ++now ) {
    while ( !events.empty() && events.top().time <= now ) {
      const auto event = events.top(); events.pop();
      auto& destination = endpoints[1 - event.from];
      if ( event.report.empty() ) {
        destination.received_packet( event.seq, event.bytes, now, 160 );
        if ( event.bulk && now > 30000 ) { background[event.from] += event.bytes; }
      } else { destination.receive_feedback( event.report, now, 160 ); }
    }
    for ( unsigned side = 0; side < 2; ++side ) {
      auto& endpoint = endpoints[side]; endpoint.tick( now, 160 );
      for ( unsigned burst = 0; burst < 8; ++burst ) {
        const bool reporting = endpoint.feedback_wait( now ) == 0;
        const bool screen = now - last_screen[side] >= 100;
        if ( endpoint.wait_time( now, reporting || screen ) ) { break; }
        const auto report = reporting ? endpoint.feedback( now ) : std::string();
        const bool bulk = report.empty() && !screen;
        const size_t bytes = !report.empty() ? 88 : screen ? 100 : 440;
        if ( screen && report.empty() ) { last_screen[side] = now; }
        const auto seq = sequences[side]++;
        endpoint.sent_packet( seq, bytes, now, !report.empty() );
        if ( ++packets % 7 == 0 ) { continue; }
        const uint64_t delay = packets % 5 == 0 ? 230 : 40;
        events.push( { now + delay, seq, side, bytes, bulk, report } );
      }
    }
  }
  for ( unsigned side = 0; side < 2; ++side ) {
    std::cout << "lossy foreground side " << side << ": budget " << endpoints[side].budget()
              << " B/s; background delivered final half " << background[side] << '\n';
    require( background[side] > 16000, "lossy foreground traffic must leave progress for background FEC on a fast path" );
  }
}

int main()
{
  try {
    Network::LinkBudget sender, receiver;
    require( sender.wait_time( 1 ) == 0 && sender.feedback_wait( 1 ) == INT_MAX, "old peers retain legacy wire behavior" );
    sender.enable( true ); receiver.enable( true );
    require( sender.budget() == 240 && sender.feedback( 10000 ).empty(), "conservative start without idle probes" );
    sender.sent_packet( 1, 240, 1000 );
    require( sender.wait_time( 1001 ) >= 999, "wire-byte pacing" );
    require( sender.wait_time( 1001, true ) < sender.wait_time( 1001 ), "bounded foreground borrowing" );
    receiver.received_packet( 1, 240, 1600, 1200 );
    receiver.received_packet( 1, 240, 1700, 1200 ); // Duplicate: no extra throughput credit.
    require( receiver.feedback( 3999 ).empty(), "coalesced feedback" );
    auto report = receiver.feedback( 4000 );
    require( report.size() == 32 && receiver.feedback( 5000 ).empty(), "one small report, no ACK-of-ACK loop" );
    sender.receive_feedback( report, 4600, 1200 );
    const auto rate = sender.budget();
    for ( int i = 0; i < 100; ++i ) { sender.receive_feedback( report, 4700 + i, 1200 ); }
    require( sender.budget() == rate, "replayed feedback cannot inflate send budget" );
    require( sender.wait_time( 40000 ) == 0 && !sender.has_measurement(), "reconnect/idle discards stale estimate and pacing debt" );
    Network::LinkBudget fast_reports;
    fast_reports.enable( true ); fast_reports.received_packet( 1, 400, 1000, 4 );
    require( fast_reports.feedback( 1050 ).size() == 32, "low-latency feedback starts promptly" );
    for ( unsigned i = 2; i <= 33; ++i ) { fast_reports.received_packet( i, 400, 1051, 4 ); }
    require( fast_reports.feedback_wait( 1051 ) > 2000 && fast_reports.feedback( 1051 ).empty(),
             "fast forward traffic cannot flood an unmeasured slow reverse path with feedback" );
    Network::LinkBudget history_sender, history_receiver;
    history_sender.enable( true ); history_receiver.enable( true );
    for ( unsigned i = 1; i <= 96; ++i ) {
      history_sender.sent_packet( i, 400, 1000 + i );
      history_receiver.received_packet( i, 400, 2000 + i, 500 );
    }
    history_sender.receive_feedback( history_receiver.feedback( 4000 ), 5000, 500 );
    require( history_sender.loss_fraction() == 0, "history older than the 64-packet bitmap is unknown, not lost" );
    require( history_sender.budget() == 240, "non-pacing-limited traffic cannot inflate the budget" );
    Network::LinkBudget sparse_sender, sparse_receiver;
    sparse_sender.enable( true ); sparse_receiver.enable( true );
    sparse_sender.sent_packet( 1, 60, 1000 ); sparse_sender.sent_packet( 2, 128, 1100, true );
    sparse_receiver.received_packet( 1, 60, 1100, 100 );
    sparse_sender.receive_feedback( sparse_receiver.feedback( 1300 ), 1400, 100 );
    sparse_sender.sent_packet( 3, 60, 2000 ); sparse_sender.wait_time( 2001 );
    sparse_sender.sent_packet( 4, 128, 2100, true );
    sparse_receiver.received_packet( 3, 60, 2100, 100 );
    sparse_sender.receive_feedback( sparse_receiver.feedback( 4000 ), 4100, 100 );
    require( sparse_sender.budget() == 240,
             "feedback overhead is not evidence of a serialization bottleneck in sparse data" );
    simulate( 11000, 2750 );
    simulate( 300, 300 );
    simulate( 1250000000, 1250000000, 2 );
    simulate( 12500000, 12500000, 2, true );
    lossy_foreground();
    std::cout << "link budget tests passed\n";
  } catch ( const std::exception& error ) { std::cerr << error.what() << '\n'; return 1; }
}
