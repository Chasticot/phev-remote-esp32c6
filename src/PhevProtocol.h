#pragma once

#include <Arduino.h>
#include <WiFiClient.h>

// Implementation of the MY18/MY24 Wi-Fi remote protocol documented by
// buxtronix/phev2mqtt. Lecture/chauffage/arret verifies sur MY2020 avec le
// firmware source; this public configuration variant remains a beta.
struct PhevState {
  bool online = false;
  bool batteryValid = false;
  uint8_t batteryPercent = 0;
  uint32_t batteryUpdatedMs = 0;
  bool batteryWarningValid = false;
  uint8_t batteryWarning = 0; // code brut documente, pas une tension 12 V
  bool acOperatingValid = false, acOperating = false;
  bool lightsValid = false, hazards = false, interiorLights = false;
  bool registrationsValid = false;
  uint8_t registrations = 0; // pas de publication du VIN
  bool climateValid = false;
  bool climateOn = false;
  bool climateTerminated = false;
  bool climateSettingsValid = false;
  uint8_t climateMode = 0;       // 0 unknown, 1 cool, 2 heat, 3 windscreen
  uint8_t climateDuration = 0;   // minutes
  bool chargeValid = false;
  bool chargingValid = false;
  bool plugValid = false;
  bool chargeTimeValid = false;
  bool charging = false;
  bool plugged = false;
  uint16_t chargeRemaining = 0;
  bool doorsValid = false;
  bool doorsLocked = false;
  uint8_t openMask = 0;          // driver, passenger, rear R/L, boot, bonnet
  bool headlights = false;
  bool parkingLights = false;
  uint32_t lastVehicleMessageMs = 0;
  uint32_t lastTelemetryMs = 0; // register traffic, excluding keepalive pings
  bool commandAck = false;
  bool commandFailed = false;
};

class PhevProtocol {
 public:
  enum class Registration : uint8_t { Idle, WaitingVehicle, WaitingAck, Acknowledged, Full, Failed, Uncertain, Cancelled };
  // Explicit local maintenance only; never entered by tick(), boot or Zigbee.
  bool beginRegistrationSession();
  void endRegistrationSession();
  Registration registration() const { return registration_; }
  bool registrationActive() const { return registration_ == Registration::WaitingVehicle || registration_ == Registration::WaitingAck; }
  const PhevState &state() const { return state_; }
  bool tcpConnected() { return tcp_.connected(); }
  uint32_t tcpConnectCount() const { return tcpConnectCount_; }
  uint32_t tcpRxBytes() const { return tcpRxBytes_; }
  uint32_t tcpRxFrames() const { return tcpRxFrames_; }
  uint32_t tcpTxPings() const { return tcpTxPings_; }
  String tcpFirstRxHex() const;
  bool pingAcknowledged() const { return pingAcknowledged_; }
  uint8_t pingAckCount() const { return pingAckCount_; }
  bool commandPending() const { return pending_ && pendingRegister_ == 0x1b; }
  uint8_t txQueued() const { return txCount_; }
  void tick(bool wifiConnected);
  // Diagnostic: pings/ACK only, never a register-setting request.
  void setWatchOnly(bool enabled) { watchOnly_ = enabled; }
  void setReadRequests(bool enabled) { readRequestsAllowed_ = enabled; }
  bool requestClimate(uint8_t mode, uint8_t durationMinutes);
  bool requestRefresh();
  void disconnect(const char *reason = "demande locale / changement de mode");
  void beginDemandSession();
  bool demandTransportEnded() const { return demandManaged_ && demandAttempted_ && !tcpSessionOpen_; }
  void failClimate() { state_.commandAck = false; state_.commandFailed = true; }

 private:
  Registration registration_ = Registration::Idle;
  bool registrationSession_ = false, registrationVinSeen_ = false;
  bool registrationPreviousReadRequests_ = false;
  uint32_t registrationStartedMs_ = 0;
  WiFiClient tcp_;
  uint32_t tcpConnectCount_ = 0;
  uint32_t tcpRxBytes_ = 0;
  uint32_t tcpRxFrames_ = 0;
  uint32_t tcpTxPings_ = 0;
  uint32_t tcpSessionStartedMs_ = 0;
  uint8_t tcpFirstRx_[32] = {};
  uint8_t tcpFirstRxLength_ = 0;
  bool tcpSessionOpen_ = false;
  struct TxPacket {
    uint8_t bytes[250];
    uint8_t length = 0, offset = 0;
    uint32_t queuedAt = 0;
  };
  static constexpr uint8_t kTxCapacity = 48;
  TxPacket txQueue_[kTxCapacity];
  uint8_t txHead_ = 0, txCount_ = 0;
  bool pingAcknowledged_ = false;
  uint8_t pingAckCount_ = 0;
  PhevState state_;
  uint8_t rx_[512] = {};
  size_t rxLength_ = 0;
  uint8_t keyMap_[256] = {};
  bool keyReady_ = false;
  uint8_t sendIndex_ = 0;
  uint8_t receiveIndex_ = 0;
  uint8_t pingSeq_ = 10;
  uint32_t lastConnectAttemptMs_ = 0;
  uint32_t connectedAtMs_ = 0;
  uint32_t lastPingMs_ = 0;
  uint32_t lastUpdateRequestMs_ = 0;
  uint32_t pendingSinceMs_ = 0;
  bool started_ = false;
  bool watchOnly_ = false;
  bool readRequestsAllowed_ = false, initialRefreshSent_ = false;
  bool demandManaged_ = false, demandAttempted_ = false;
  uint32_t lastTcpRxMs_ = 0;
  uint32_t lastPingPollMs_ = 0;
  bool pending_ = false;
  uint8_t pendingRegister_ = 0;
  uint8_t pendingData_[16] = {};
  uint8_t pendingLength_ = 0;
  uint8_t retryCount_ = 0;

  bool sendFrame(uint8_t type, uint8_t ack, uint8_t reg,
                 const uint8_t *data, uint8_t length, int overrideXor = -1);
  void sendPing(bool requestedByVehicle = false);
  bool flushTx();
  void sendRegister(uint8_t reg, const uint8_t *data, uint8_t length);
  void processRx();
  void processFrame(const uint8_t *decoded, uint8_t totalLength,
                    const uint8_t *raw, uint8_t xorValue);
  void updateKey(const uint8_t *packet, uint8_t length);
  void updateRegister(uint8_t reg, const uint8_t *data, uint8_t length);
};
