#pragma once
#include <cstdint>

// One exactly representable IEEE-754 float/ZCL analog presentValue carries
// session identity and result atomically. HA must never combine an old ACK
// with a new request. This records protocol acknowledgement, NOT actual HVAC.
class PhevSessionResult {
 public:
  enum class Status : uint8_t {
    Never = 0, Refreshing = 1, ClimatePending = 2, RefreshCompleted = 3,
    CommandAcknowledged = 4, Timeout = 5, TransportFailed = 6, CommandFailed = 7,
  };
  static constexpr uint32_t MaxId = (1U << 21) - 1;
  static constexpr uint32_t MaxWord = (1U << 24) - 1;

  void begin(bool climate) {
    if (active()) return; // defensive: duplicate requests cannot become a new session
    id_ = id_ == MaxId ? 1 : id_ + 1; // zero means no session since boot
    status_ = climate ? Status::ClimatePending : Status::Refreshing;
  }
  void promoteClimate() {
    if (active()) status_ = Status::ClimatePending;
  }
  void finish(Status status) {
    const auto code = static_cast<uint8_t>(status);
    if (active() && code >= 3 && code <= 7) status_ = status;
  }
  bool active() const { return status_ == Status::Refreshing || status_ == Status::ClimatePending; }
  uint32_t id() const { return id_; }
  Status status() const { return status_; }
  uint32_t word() const { return (id_ << 3) | static_cast<uint8_t>(status_); }

 private:
  uint32_t id_ = 0;
  Status status_ = Status::Never;
};
