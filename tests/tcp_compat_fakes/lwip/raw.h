#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
using u8_t = uint8_t;
using err_t = int;
constexpr int ERR_OK = 0, IPADDR_TYPE_V4 = 0;
struct ip_addr_t {};
struct pbuf { std::vector<uint8_t> data; };
struct raw_pcb {};
using raw_recv_fn = u8_t (*)(void *, raw_pcb *, pbuf *, const ip_addr_t *);
namespace CompatFake {
inline raw_pcb raw;
inline raw_recv_fn receive = nullptr;
inline bool allocationFails = false;
inline unsigned allocations = 0;
}
inline raw_pcb *raw_new_ip_type(u8_t, u8_t) {
  ++CompatFake::allocations;
  return CompatFake::allocationFails ? nullptr : &CompatFake::raw;
}
inline void raw_recv(raw_pcb *, raw_recv_fn fn, void *) { CompatFake::receive = fn; }
inline uint16_t pbuf_copy_partial(pbuf *p, void *to, uint16_t size, uint16_t offset) {
  if (offset >= p->data.size()) return 0;
  size = std::min<size_t>(size, p->data.size() - offset);
  std::memcpy(to, p->data.data() + offset, size);
  return size;
}
