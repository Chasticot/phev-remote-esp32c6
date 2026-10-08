#pragma once
#include <cstddef>
#include <cstdint>

enum class BootMode : uint8_t { Normal = 0, AccessPoint = 1, HomeWifi = 2 };
constexpr char PHEV_IMAGE_ID[] = "PHEV-REMOTE-C6-OTA-IMAGE-v1";
inline bool validBootMode(uint8_t value) { return value <= 2; }
inline bool scanPhevIdentity(const uint8_t *bytes, size_t length, uint8_t &matched) {
  if (matched >= sizeof(PHEV_IMAGE_ID) - 1) return true;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] == static_cast<uint8_t>(PHEV_IMAGE_ID[matched])) ++matched;
    else matched = bytes[i] == static_cast<uint8_t>(PHEV_IMAGE_ID[0]) ? 1 : 0;
    if (matched == sizeof(PHEV_IMAGE_ID) - 1) return true;
  }
  return false;
}
// Application image, not bootloader/factory image. ESP image header (24 bytes),
// first segment header (8), then esp_app_desc. Chip ID 13 is ESP32-C6.
inline bool validC6ApplicationPrefix(const uint8_t *bytes, size_t length) {
  return length >= 36 && bytes[0] == 0xe9 && bytes[12] == 13 && bytes[13] == 0 &&
         bytes[32] == 0x32 && bytes[33] == 0x54 && bytes[34] == 0xcd && bytes[35] == 0xab;
}
