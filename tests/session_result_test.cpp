#include "PhevSessionResult.h"
#include "PhevDemandPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  using R = PhevSessionResult; using S = R::Status;
  R r; assert(r.word() == 0 && r.id() == 0 && !r.active());
  r.finish(S::CommandAcknowledged); assert(r.word() == 0); // no phantom success
  r.begin(false); assert(r.word() == 9 && r.id() == 1 && r.active());
  r.begin(false); assert(r.id() == 1); // defensive duplicate protection
  r.promoteClimate(); assert(r.word() == 10 && r.id() == 1 && r.active());
  r.finish(S::Never); assert(r.word() == 10); // invalid terminal ignored
  r.finish(S::CommandAcknowledged); assert(r.word() == 12 && !r.active());
  r.promoteClimate(); assert(r.word() == 12); // terminal survives idle
  r.finish(S::Timeout); assert(r.word() == 12);
  for (S status : {S::RefreshCompleted, S::CommandAcknowledged, S::Timeout,
                   S::TransportFailed, S::CommandFailed}) {
    r.begin(status == S::CommandAcknowledged || status == S::CommandFailed);
    const uint32_t id = r.id(); r.finish(status);
    assert(r.word() == ((id << 3) | static_cast<uint8_t>(status)) && !r.active());
    assert(static_cast<uint32_t>(static_cast<float>(r.word())) == r.word());
  }
  R wrap;
  for (uint32_t i = 1; i <= R::MaxId; ++i) {
    wrap.begin(false); assert(wrap.id() == i); wrap.finish(S::CommandFailed);
  }
  assert(wrap.word() == R::MaxWord);
  assert(static_cast<uint32_t>(static_cast<float>(wrap.word())) == R::MaxWord);
  wrap.begin(false); assert(wrap.id() == 1 && wrap.word() == 9);

  // Mirror normal firmware gating: refresh coalesces and promotion preserves
  // the same deadline/identity; no replay or schedule changes are introduced.
  PhevDemandPolicy demand; R coupled; demand.begin(0);
  demand.refresh(15000); coupled.begin(false);
  const bool alreadyActive = demand.active(); demand.refresh(50000);
  if (!alreadyActive) coupled.begin(false);
  assert(coupled.id() == 1 && demand.active());
  assert(demand.climate(50000, 2, 10)); coupled.promoteClimate();
  assert(coupled.word() == 10);
  assert(demand.observe(75000, false, false, 0, false, false, false) == PhevDemandPolicy::Action::Failure);
  coupled.finish(S::Timeout); demand.finish(); assert(coupled.word() == 13 && !coupled.active());
  assert(!demand.dailyDue(15000 + PhevDemandPolicy::DailyMs - 1));
  assert(demand.dailyDue(15000 + PhevDemandPolicy::DailyMs));
  std::cout << "PASS atomic session: exact24bit, identity/wrap, duplicate/promotion, terminal/ACK semantics, unchanged60s/daily\n";
}
