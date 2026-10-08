#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

struct IPAddress {
  uint8_t bytes[4];
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes{a, b, c, d} {}
  String toString() const { return "192.168.8.47"; }
};

struct FakeNetwork {
  uint32_t now = 0;
  uint32_t readTimeMs = 0;
  bool connected = false;
  bool allowConnect = true;
  unsigned connectAttempts = 0;
  size_t nextWriteLimit = std::numeric_limits<size_t>::max();
  int writeError = 0;
  std::deque<uint8_t> incoming;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> address;
  uint16_t port = 0;

  void inject(const std::vector<uint8_t> &bytes) {
    incoming.insert(incoming.end(), bytes.begin(), bytes.end());
  }
};

extern FakeNetwork network;

class WiFiClient {
 public:
  int fd() const { return network.connected ? 48 : -1; }
  IPAddress localIP() const { return IPAddress(192, 168, 8, 47); }
  uint16_t localPort() const { return 50000; }
  void setNoDelay(bool) {}
  void setTimeout(unsigned) {}
  int setSocketOption(int, char *, size_t) { return 0; }
  bool connect(IPAddress address, uint16_t port, int) {
    ++network.connectAttempts;
    network.address.assign(address.bytes, address.bytes + 4);
    network.port = port;
    return network.connected = network.allowConnect;
  }
  bool connected() const { return network.connected; }
  int available() const { return static_cast<int>(network.incoming.size()); }
  int read() {
    if (network.incoming.empty()) return -1;
    const uint8_t value = network.incoming.front();
    network.incoming.pop_front();
    network.now += network.readTimeMs;
    return value;
  }
  size_t write(const uint8_t *bytes, size_t length) {
    if (network.writeError) { errno = network.writeError; return 0; }
    if (!network.connected) return 0;
    const size_t written = std::min(length, network.nextWriteLimit);
    network.writes.emplace_back(bytes, bytes + written);
    network.nextWriteLimit = std::numeric_limits<size_t>::max();
    return written;
  }
  void stop() {
    network.connected = false;
    network.incoming.clear();
  }
};
