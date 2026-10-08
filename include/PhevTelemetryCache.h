#pragma once
#include "PhevProtocol.h"

// Only confirmed values are cached, independently of TCP availability.
// Values are RAM-only; after a reboot an initial refresh repopulates them.
class PhevTelemetryCache {
 public:
  static constexpr uint64_t LifetimeMs = 25ULL * 60 * 60 * 1000;
  void capture(const PhevState &s, uint64_t now, bool acceptBattery = true) {
    // Battery values may glitch immediately after climate register writes.
    // Command sessions never refresh the SOC or its age; explicit/daily reads do.
    if (acceptBattery && s.batteryValid) {
      value_.batteryPercent = s.batteryPercent; value_.parkingLights = s.parkingLights;
      stamp(0, now - static_cast<uint32_t>(static_cast<uint32_t>(now) - s.batteryUpdatedMs));
    }
    if (s.climateValid) { value_.climateOn = s.climateOn; value_.climateTerminated = s.climateTerminated; stamp(1, now); }
    if (s.climateSettingsValid) { value_.climateMode = s.climateMode; value_.climateDuration = s.climateDuration; stamp(2, now); }
    if (s.chargingValid) { value_.charging = s.charging; stamp(3, now); }
    if (s.plugValid) { value_.plugged = s.plugged; stamp(4, now); }
    if (s.chargeTimeValid) { value_.chargeRemaining = s.chargeRemaining; stamp(5, now); }
    if (s.doorsValid) {
      value_.doorsLocked = s.doorsLocked; value_.openMask = s.openMask; value_.headlights = s.headlights; stamp(6, now);
    }
    if (s.lightsValid) { value_.hazards = s.hazards; value_.interiorLights = s.interiorLights; stamp(7, now); }
    if (s.acOperatingValid) { value_.acOperating = s.acOperating; stamp(8, now); }
    if (s.batteryWarningValid) { value_.batteryWarning = s.batteryWarning; stamp(9, now); }
    if (s.registrationsValid) { value_.registrations = s.registrations; stamp(10, now); }
  }
  uint16_t mask(uint64_t now) const {
    uint16_t result = 0;
    for (unsigned i = 0; i < 11; ++i)
      if ((known_ & (1U << i)) && now >= stamps_[i] && now - stamps_[i] < LifetimeMs) result |= 1U << i;
    return result;
  }
  PhevState state(uint64_t now) const {
    PhevState s = value_; const uint16_t m = mask(now);
    s.online = false;
    s.batteryValid = m & 1; s.climateValid = m & 2; s.climateSettingsValid = m & 4;
    s.chargingValid = m & 8; s.plugValid = m & 16; s.chargeTimeValid = m & 32;
    s.doorsValid = m & 64; s.lightsValid = m & 128; s.acOperatingValid = m & 256;
    s.batteryWarningValid = m & 512; s.registrationsValid = m & 1024;
    return s;
  }
  uint64_t batteryAgeSeconds(uint64_t now) const { return (now - stamps_[0]) / 1000; }
 private:
  void stamp(unsigned bit, uint64_t now) { known_ |= 1U << bit; stamps_[bit] = now; }
  PhevState value_{};
  uint16_t known_ = 0;
  uint64_t stamps_[11]{};
};
