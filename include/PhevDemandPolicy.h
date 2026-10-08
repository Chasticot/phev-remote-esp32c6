#pragma once
#include <cstdint>

// Monotonic uptime, no Internet/NTP dependency while REMOTE is disconnected.
class PhevDemandPolicy {
 public:
  static constexpr uint64_t DailyMs = 24ULL * 60 * 60 * 1000;
  static constexpr uint64_t LimitMs = 60000;
  enum class Reason { None, Refresh, Climate };
  enum class Action { Wait, SendClimate, Success, Failure };
  void begin(uint64_t now) { nextDaily_ = now + 15000; }
  bool dailyDue(uint64_t now) const { return !active_ && now >= nextDaily_; }
  bool refresh(uint64_t now) {
    if (active_) return true; // coalesce requests; never extend the deadline
    start(now, Reason::Refresh); return true;
  }
  bool climate(uint64_t now, uint8_t mode, uint8_t duration) {
    if (mode > 3 || (duration != 10 && duration != 20 && duration != 30)) return false;
    if (active_ && reason_ == Reason::Climate) return false;
    if (!active_) start(now, Reason::Climate);
    reason_ = Reason::Climate; mode_ = mode; duration_ = duration; return true;
  }
  Action observe(uint64_t now, bool online, bool battery, uint32_t telemetryMs,
                 bool pending, bool ack, bool failed) {
    if (!active_) return Action::Wait;
    if (now - started_ >= LimitMs) return Action::Failure;
    if (!online) return Action::Wait;
    if (!seenOnline_) { onlineAt_ = now; seenOnline_ = true; }
    const bool quiet = telemetryMs && static_cast<uint32_t>(now) - telemetryMs >= 2000;
    if (reason_ == Reason::Climate) {
      if (!sent_) {
        if ((battery && quiet) || now - onlineAt_ >= 10000) {
          sent_ = true; return Action::SendClimate; // consume BEFORE any write
        }
        return Action::Wait;
      }
      if (failed && !ack) return Action::Failure;
      if (ack && !seenAck_) { ackAt_ = now; seenAck_ = true; }
      if (seenAck_ && !pending && quiet && now - ackAt_ >= 5000) return Action::Success;
    } else if (battery && quiet && now - onlineAt_ >= 10000) return Action::Success;
    return Action::Wait;
  }
  void finish() { active_ = false; reason_ = Reason::None; sent_ = false; }
  bool active() const { return active_; }
  Reason reason() const { return reason_; }
  uint8_t mode() const { return mode_; }
  uint8_t duration() const { return duration_; }
  bool sent() const { return sent_; }
 private:
  void start(uint64_t now, Reason reason) {
    active_ = true; reason_ = reason; started_ = now; nextDaily_ = now + DailyMs;
    seenOnline_ = seenAck_ = sent_ = false;
  }
  bool active_ = false, seenOnline_ = false, seenAck_ = false, sent_ = false;
  Reason reason_ = Reason::None;
  uint8_t mode_ = 0, duration_ = 10;
  uint64_t started_ = 0, onlineAt_ = 0, ackAt_ = 0, nextDaily_ = 0;
};
