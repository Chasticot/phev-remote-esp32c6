#include "PhevTcpCompat.h"
#include <atomic>
#include <cstdint>
#include <lwip/raw.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcp_priv.h>

namespace {
std::atomic<unsigned> installation{0}; // 0=retry, 1=scheduled, 2=installed
uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }

void quickAck(void *arg) {
  // Runs on the TCP/IP thread AFTER the observed packet's normal processing.
  // ACK exactly rcv_nxt, never advance it or accept an incomplete/corrupt stream.
  const uint16_t port = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg));
  for (tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
    if (pcb->local_port == port && pcb->remote_port == 8080 && pcb->state == ESTABLISHED) {
      tcp_send_empty_ack(pcb);
      return;
    }
  }
}

u8_t observe(void *, raw_pcb *, pbuf *p, const ip_addr_t *) {
  uint8_t header[80]{};
  const uint16_t n = pbuf_copy_partial(p, header, sizeof(header), 0);
  if (n < 40 || header[0] >> 4 != 4 || header[9] != 6 ||
      header[12] != 192 || header[13] != 168 || header[14] != 8 || header[15] != 46) return 0;
  const uint8_t ip = (header[0] & 15) * 4;
  if (ip < 20 || n < ip + 20 || be16(header + ip) != 8080) return 0;
  const uint16_t total = be16(header + 2);
  const uint8_t tcp = (header[ip + 12] >> 4) * 4;
  if (tcp < 20 || total <= ip + tcp || (header[ip + 13] & 0x06)) return 0;
  // A failed scheduling attempt leaves ordinary lwIP ACK/retransmission active.
  tcpip_callback(quickAck, reinterpret_cast<void *>(static_cast<uintptr_t>(be16(header + ip + 2))));
  return 0; // never consume, free, modify or inject application data
}

void install(void *) {
  raw_pcb *pcb = raw_new_ip_type(IPADDR_TYPE_V4, 6);
  if (!pcb) { installation.store(0); return; }
  raw_recv(pcb, observe, nullptr);
  installation.store(2);
}
}

void beginPhevTcpCompat() {
  unsigned expected = 0;
  if (!installation.compare_exchange_strong(expected, 1)) return;
  if (tcpip_callback(install, nullptr) != ERR_OK) installation.store(0);
}
