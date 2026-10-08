#include "PhevIdentity.h"
#include <cassert>
#include <iostream>
int main() {
  uint8_t value[6]{};
  assert(parsePhevMac("02:11:22:33:44:55", value));
  assert(value[0] == 2 && value[5] == 0x55);
  assert(parsePhevMac("a2:b3:c4:d5:e6:f7", value));
  const uint8_t before = value[0];
  for (auto bad : {"", "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF",
       "03:11:22:33:44:55", "02-11-22-33-44-55", "02:11:22:33:44:5Z",
       "02:11:22:33:44", "02:11:22:33:44:55:66"}) {
    assert(!parsePhevMac(bad, value));
    assert(value[0] == before); // invalid input must not partially update identity
  }
  assert(!parsePhevMac(nullptr, value));
  std::cout << "PASS configurable identity: valid unicast, malformed/zero/multicast rejected\n";
}
