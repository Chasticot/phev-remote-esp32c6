#pragma once
#include <WiFiClient.h>
#include <cassert>
constexpr int MSG_DONTWAIT = 0x40;
inline int send(int fd, const void *buffer, size_t length, int flags) {
  assert(flags == MSG_DONTWAIT); // production path must never block/retry
  assert(fd == 48 && network.connected);
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  const size_t written = std::min(length, network.nextWriteLimit);
  network.writes.emplace_back(bytes, bytes + written);
  network.nextWriteLimit = std::numeric_limits<size_t>::max();
  return static_cast<int>(written);
}
