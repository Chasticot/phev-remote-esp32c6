#pragma once
#include "lwip/raw.h"
constexpr int ESTABLISHED = 4;
struct tcp_pcb { tcp_pcb *next = nullptr; uint16_t local_port = 2222, remote_port = 8080; int state = ESTABLISHED; uint32_t rcv_nxt = 100; };
inline tcp_pcb *tcp_active_pcbs = nullptr;
namespace CompatFake { inline std::vector<uint32_t> acks; }
inline err_t tcp_send_empty_ack(tcp_pcb *pcb) { CompatFake::acks.push_back(pcb->rcv_nxt); return ERR_OK; }
