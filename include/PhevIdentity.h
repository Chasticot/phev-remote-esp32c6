#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstring>

// No vehicle identity in the source or public image. The user supplies an
// already registered client identity through the AP; it remains in local NVS.
inline bool parsePhevMac(const char *value, uint8_t out[6]) {
  if (!value || std::strlen(value) != 17) return false;
  uint8_t parsed[6]{};
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  uint8_t any = 0;
  for (uint8_t i = 0; i < 6; ++i) {
    const uint8_t p = i * 3;
    if (i && value[p - 1] != ':') return false;
    const int a = digit(value[p]), b = digit(value[p + 1]);
    if (a < 0 || b < 0) return false;
    parsed[i] = static_cast<uint8_t>((a << 4) | b);
    any |= parsed[i];
  }
  if (!any || (parsed[0] & 1)) return false;
  std::memcpy(out, parsed, sizeof(parsed));
  return true;
}
