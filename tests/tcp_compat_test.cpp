#include "PhevTcpCompat.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#include <cassert>
#include <iostream>

pbuf packet(uint8_t flags = 0x18) {
  pbuf p; p.data.resize(46);
  p.data[0] = 0x45; p.data[3] = 46; p.data[9] = 6;
  p.data[12] = 192; p.data[13] = 168; p.data[14] = 8; p.data[15] = 46;
  p.data[20] = 0x1f; p.data[21] = 0x90; // source port 8080
  p.data[22] = 0x08; p.data[23] = 0xae; // local port 2222
  p.data[32] = 0x50; p.data[33] = flags;
  return p;
}
void rejected(pbuf p) {
  const auto original = p.data;
  const auto count = CompatFake::acks.size();
  assert(CompatFake::receive(nullptr, nullptr, &p, nullptr) == 0);
  assert(p.data == original);
  CompatFake::drain(); assert(CompatFake::acks.size() == count);
}
int main() {
  CompatFake::schedulingFails = true; beginPhevTcpCompat();
  CompatFake::schedulingFails = false; CompatFake::allocationFails = true;
  beginPhevTcpCompat(); CompatFake::drain(); assert(!CompatFake::receive);
  CompatFake::allocationFails = false;
  beginPhevTcpCompat(); CompatFake::drain(); assert(CompatFake::receive);
  const auto allocated = CompatFake::allocations;
  beginPhevTcpCompat(); CompatFake::drain(); assert(CompatFake::allocations == allocated);
  tcp_pcb pcb; tcp_active_pcbs = &pcb;
  auto p = packet(); auto original = p.data;
  assert(CompatFake::receive(nullptr, nullptr, &p, nullptr) == 0);
  assert(p.data == original && CompatFake::acks.empty());
  pcb.rcv_nxt = 106; // normal TCP processing occurs BEFORE our queued callback
  CompatFake::drain(); assert(CompatFake::acks.back() == 106 && pcb.rcv_nxt == 106);
  // Out-of-order packet: ACK the unchanged next byte, never advance over a gap.
  CompatFake::receive(nullptr, nullptr, &p, nullptr); CompatFake::drain();
  assert(CompatFake::acks.back() == 106 && pcb.rcv_nxt == 106);
  p = packet(); p.data[15] = 47; rejected(p);
  p = packet(); p.data[21] = 0x91; rejected(p);
  p = packet(); p.data[23] = 0xaf; rejected(p);
  rejected(packet(0x12)); rejected(packet(0x14));
  p = packet(); p.data.resize(40); p.data[3] = 40; rejected(p); // pure TCP ACK
  p = packet(); p.data.resize(39); rejected(p);
  p = packet(); p.data[32] = 0x40; rejected(p); // invalid header
  CompatFake::schedulingFails = true; rejected(packet()); CompatFake::schedulingFails = false;
  pcb.state = 0; rejected(packet());
  std::cout << "TCP compatibility: retry, singleton, post-input ordering, cumulative integrity, filters and failure fallback PASS\n";
}
