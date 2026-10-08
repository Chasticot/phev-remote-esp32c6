#include "ZclReport.h"
#include <cassert>
#include <iostream>
int main() {
  uint8_t bytes[10]{};
  const uint8_t yes = 1, invalid = 2;
  assert(encodePresentValueReport(bytes, sizeof(bytes), 7, 0x10, &yes) == 7);
  assert(bytes[0] == 0x18 && bytes[1] == 7 && bytes[2] == 10 && bytes[3] == 85 && bytes[4] == 0 && bytes[5] == 0x10 && bytes[6] == 1);
  const uint16_t mode = 3;
  assert(encodePresentValueReport(bytes, sizeof(bytes), 8, 0x21, &mode) == 8);
  assert(bytes[6] == 3 && bytes[7] == 0);
  const float battery = 94.0f;
  assert(encodePresentValueReport(bytes, sizeof(bytes), 9, 0x39, &battery) == 10);
  assert(bytes[6] == 0 && bytes[7] == 0 && bytes[8] == 0xbc && bytes[9] == 0x42);
  assert(!encodePresentValueReport(bytes, 6, 0, 0x39, &battery));
  assert(!encodePresentValueReport(bytes, 10, 0, 0x99, &battery));
  assert(!encodePresentValueReport(bytes, 10, 0, 0x10, &invalid));
  assert(!encodePresentValueReport(bytes, 10, 0, 0x10, nullptr));
  std::cout << "PASS ZCL: standard frame, bool/U16/float, invalid/truncated/null rejected\n";
}
