#include "PhevDemandPolicy.h"
#include "PhevTelemetryCache.h"
#include <WiFi.h>
#include <cassert>
#include <iostream>
FakeNetwork network; FakeSerial Serial; FakeEsp ESP; FakeWifi WiFi;
uint32_t millis() { return network.now; }
int main() {
  using P = PhevDemandPolicy;
  P p; p.begin(1000); assert(!p.dailyDue(15999)); assert(p.dailyDue(16000));
  p.refresh(16000); assert(p.active()); assert(!p.dailyDue(20000));
  assert(p.observe(20000, true, true, 19999, false, false, false) == P::Action::Wait);
  assert(p.observe(30000, true, true, 27999, false, false, false) == P::Action::Success);
  p.finish(); assert(!p.active()); assert(!p.dailyDue(16000 + P::DailyMs - 1));
  assert(p.dailyDue(16000 + P::DailyMs));
  p.refresh(100000); p.refresh(150000); // duplicate request must not extend 60 s
  assert(p.observe(160000, false, false, 0, false, false, false) == P::Action::Failure);
  p.finish(); p.refresh(200000);
  assert(p.climate(200001, 2, 10)); assert(!p.climate(200002, 0, 10));
  assert(p.observe(201000, true, true, 198000, false, false, false) == P::Action::SendClimate);
  assert(p.sent());
  assert(p.observe(201001, true, true, 198000, false, false, false) == P::Action::Wait);
  assert(p.observe(202000, true, true, 198000, false, true, false) == P::Action::Wait);
  assert(p.observe(207000, true, true, 204000, false, true, false) == P::Action::Success);
  p.finish(); assert(!p.sent());
  assert(!p.climate(300000, 4, 10)); assert(!p.active());
  assert(!p.climate(300000, 2, 5));
  p.climate(400000, 0, 10); // stop also opens a one-shot session
  assert(p.observe(401000, true, true, 398000, false, false, false) == P::Action::SendClimate);
  assert(p.observe(401001, true, true, 398000, false, false, true) == P::Action::Failure);
  p.finish(); p.refresh(500000);
  assert(p.reason() == P::Reason::Refresh && !p.sent()); // no climate replay next cycle
  p.finish();
  const uint64_t wrap = 0x100000000ULL + 5000;
  p.begin(wrap); p.refresh(wrap);
  assert(p.observe(wrap + 2000, true, true, 5000, false, false, false) == P::Action::Wait);
  assert(p.observe(wrap + 12000, true, true, 5000, false, false, false) == P::Action::Success);

  PhevTelemetryCache cache; PhevState s;
  assert(cache.mask(1000) == 0);
  s.batteryValid = true; s.batteryPercent = 94; s.batteryUpdatedMs = 1000;
  cache.capture(s, 1000); assert(cache.mask(1000) == 1);
  s.batteryPercent = 5; s.batteryUpdatedMs = 1500;
  s.climateValid = true; s.climateOn = true;
  cache.capture(s, 1500, false);
  assert(cache.state(1500).batteryPercent == 94 && cache.batteryAgeSeconds(2000) == 1);
  assert(cache.state(1500).climateOn); // HVAC readback remains accepted
  // Keep the remainder of this test focused on independent group expiry.
  cache = PhevTelemetryCache{};
  s = PhevState{}; s.batteryValid = true; s.batteryPercent = 94; s.batteryUpdatedMs = 1000;
  cache.capture(s, 1000);
  s = PhevState{}; cache.capture(s, 2000);
  assert(cache.state(2000).batteryValid && cache.state(2000).batteryPercent == 94);
  assert(!cache.state(2000).online); assert(cache.batteryAgeSeconds(2000) == 1);
  s.doorsValid = true; s.doorsLocked = true; cache.capture(s, 10000);
  assert(cache.mask(10000) == (1 | 64));
  assert(cache.mask(1000 + PhevTelemetryCache::LifetimeMs) == 64); // groups expire independently
  assert(cache.mask(10000 + PhevTelemetryCache::LifetimeMs) == 0);
  PhevTelemetryCache wrapped; s = PhevState{}; s.batteryValid = true; s.batteryUpdatedMs = 4000;
  wrapped.capture(s, wrap); assert(wrapped.batteryAgeSeconds(wrap) == 1);
  std::cout << "PASS demand/cache: boot/daily, coalescing, bounded expiry, climate once/no replay, ack, wrap64, known/unknown/expired groups\n";
}
