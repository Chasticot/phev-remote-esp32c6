#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// ZCL global Report Attributes, server -> client, default response disabled.
// Only standard presentValue types used by this firmware are accepted.
inline size_t encodePresentValueReport(uint8_t *out, size_t capacity, uint8_t sequence,
                                      uint8_t type, const void *value) {
  const size_t width = type == 0x10 ? 1 : type == 0x21 ? 2 : type == 0x39 ? 4 : 0;
  if (!out || !value || !width || capacity < 6 + width) return 0;
  out[0] = 0x18; out[1] = sequence; out[2] = 0x0a;
  out[3] = 0x55; out[4] = 0; out[5] = type;
  uint32_t bits = 0;
  std::memcpy(&bits, value, width);
  if (type == 0x10 && bits > 1) return 0;
  for (size_t i = 0; i < width; ++i) out[6 + i] = static_cast<uint8_t>(bits >> (8 * i));
  return 6 + width;
}
