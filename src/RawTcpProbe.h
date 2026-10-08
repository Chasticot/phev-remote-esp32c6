#pragma once

#include <Arduino.h>

// Diagnostic transport uniquement : aucune emission de donnees applicatives.
// Une seule socket, jamais partagee avec PhevProtocol / NetworkClient.
class RawTcpProbe {
 public:
  void tick(bool wifiReady);
  void disconnect(const char *cause);
  bool connected() const { return _phase == Phase::Connected; }
  const char *phase() const;
  int fd() const { return _fd; }
  uint32_t attempts() const { return _attempts; }
  uint32_t sessions() const { return _sessions; }
  uint32_t rxBytes() const { return _rxBytes; }
  uint32_t eofCount() const { return _eofCount; }
  int lastError() const { return _lastError; }
  const char *lastCause() const { return _lastCause; }

 private:
  enum class Phase { Idle, Connecting, Connected };
  Phase _phase = Phase::Idle;
  int _fd = -1;
  uint32_t _lastAttemptMs = 0, _startedMs = 0, _connectedMs = 0;
  uint32_t _attempts = 0, _sessions = 0, _rxBytes = 0, _eofCount = 0;
  uint32_t _shownBytes = 0;
  int _lastError = 0;
  const char *_lastCause = "aucun";
  void start();
  void markConnected();
  void fail(const char *operation, int error);
};
