#include <Arduino.h>
#include <atomic>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Zigbee.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_coexist.h>
#include <esp_ieee802154.h>
#include "PhevTcpCompat.h"
#include <esp_timer.h>
#include <errno.h>
#include <ESPmDNS.h>

#include "PhevProtocol.h"
#include "RawTcpProbe.h"
#include "PhevIdentity.h"
#include "MaintenanceService.h"
#include "ZclReport.h"
#include "PhevDemandPolicy.h"
#include "PhevTelemetryCache.h"
#include "PhevSessionResult.h"
#include <aps/esp_zigbee_aps.h>

#ifndef ZIGBEE_MODE_ED
#error "Dans Outils, choisir Zigbee Mode > Zigbee ED (end device)"
#endif

#ifndef PHEV_WIFI_LEGACY_BGN
#define PHEV_WIFI_LEGACY_BGN 1
#endif

#ifndef PHEV_RAW_TCP_TEST
#define PHEV_RAW_TCP_TEST 0
#endif

#ifndef PHEV_PROTOCOL_WATCH_TEST
#define PHEV_PROTOCOL_WATCH_TEST 0
#endif

// ESP32-C6-DevKitC-1U v1.2. Poussoirs externes entre la broche et GND.
constexpr uint8_t PIN_ZIGBEE = 2;
constexpr uint8_t PIN_PORTAL = 3;
constexpr char FIRMWARE_VERSION[] = "0.1.0-beta.1";
constexpr bool SUSPEND_ZIGBEE_DURING_PHEV = false;
constexpr uint32_t WATCH_TEST_MS = 90000;
bool watchTestStopped = false;
uint32_t watchTestStartedMs = 0;
constexpr uint32_t PAIR_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t PORTAL_MS = 30UL * 60UL * 1000UL;

// Les endpoints et leurs clusters sont repris dans zigbee2mqtt/.
ZigbeeBinary epClimate(1), epCar(6), epCharging(7), epPlug(8), epLocked(10);
ZigbeeBinary epLights(11), epParking(12);
ZigbeeBinary epCommandAck(13), epCommandFailed(14), epWifi(15);
ZigbeeMultistate epMode(2), epDuration(3);
ZigbeeAnalog epBattery(4), epChargeTime(5), epOpenMask(9);
ZigbeeMultistate epMaintenance(16);
ZigbeeAnalog epRssi(17), epBatteryAge(19), epUptime(20), epBatteryWarning(22), epRegistrations(26);
ZigbeeBinary epBatteryValid(18), epClimateTerminated(21), epAcOperating(23), epHazards(24), epInterior(25);
ZigbeeAnalog epTelemetryMask(27);
ZigbeeAnalog epSessionResult(28);
PhevDemandPolicy demand;
PhevTelemetryCache telemetry;
PhevSessionResult sessionResult;
bool postClimateRefreshSent = false;
uint64_t uptimeMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
void finishDemand(bool success, const char *reason,
                  PhevSessionResult::Status failure = PhevSessionResult::Status::TransportFailed);

Preferences prefs;
WebServer web(80);
MaintenanceService maintenance(web, prefs);
PhevProtocol phev;
RawTcpProbe rawTcp;

String carSsid, carPassword, clonedMacText;
String portalSsid, portalPassword;
String scannedRemoteOptions;
String scannedRemoteJson = "[]";
String testError;
uint8_t clonedMac[6] = {};
uint8_t portalChannel = 1;
bool configured = false;
bool maintenanceBoot = false;
bool wifiOnlyBoot = false;
bool portalActive = false;
bool testMode = false;
bool scanInProgress = false;
bool scanPending = false;
bool testConnectPending = false;
int8_t scanError = 0;
uint32_t scanRequestedMs = 0, testConnectRequestedMs = 0;
bool staMacApplied = false;
bool carWifiBegun = false;
volatile uint16_t lastWifiDisconnectReason = 0;
volatile bool wifiGotIpEvent = false;
std::atomic<uint32_t> lastWifiDisconnectMs{0};
std::atomic<uint32_t> lastWifiGotIpMs{0};
bool zigbeeSeen = false;
bool pairingWindow = false;
bool zigbeeStarted = false;
bool zigbeeInitialized = false;
bool zigbeePausedForPortal = false;
bool zigbeePausedForDemand = false;
void reportState();
uint32_t portalOpenedMs = 0, pairingOpenedMs = 0;
uint32_t lastWifiAttemptMs = 0, lastReportMs = 0, lastLedMs = 0;
uint32_t normalStartedMs = 0;
uint32_t homeStartedMs = 0;
bool homeAddressReported = false;
uint8_t selectedMode = 2, selectedDuration = 10;
// C6/toolchain: use native aligned word atomics. A byte exchange(-1) was
// observed overwriting the adjacent selectedDuration/selectedMode bytes.
std::atomic<int32_t> requestedClimate{-1};
std::atomic<uint32_t> requestedMode{0xffff}, requestedDuration{0xffff};
std::atomic<uint32_t> requestedMaintenance{0xffff};
static_assert(sizeof(std::atomic<int32_t>) == 4 && alignof(std::atomic<int32_t>) >= 4,
              "Command mailbox needs native aligned 32-bit atomics");

bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}

bool rawTcpMode() { return PHEV_RAW_TCP_TEST && wifiOnlyBoot; }

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  const uint32_t eventMs = millis();
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectMs.store(eventMs, std::memory_order_relaxed);
    Serial.printf("[%lu ms] DIAG WIFI: deconnexion raison=%u RSSI_evenement=%d\n",
                  (unsigned long)eventMs, info.wifi_sta_disconnected.reason, info.wifi_sta_disconnected.rssi);
    // 36 (STA_LEAVING) est genere notamment par une deconnexion locale.
    // Il ne doit pas masquer le dernier vrai motif d'echec d'association.
    if (info.wifi_sta_disconnected.reason != WIFI_REASON_STA_LEAVING)
      lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    const auto &connected = info.wifi_sta_connected;
    Serial.printf("[%lu ms] DIAG WIFI: associe canal=%u authmode=%u AID=%u\n",
                  (unsigned long)eventMs, connected.channel, (unsigned int)connected.authmode, connected.aid);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    lastWifiGotIpMs.store(eventMs, std::memory_order_relaxed);
    wifiGotIpEvent = true;
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    Serial.printf("[%lu ms] DIAG WIFI: perte adresse IP\n", (unsigned long)eventMs);
  }
  else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *mac = info.wifi_ap_staconnected.mac;
    Serial.printf("Portail: telephone/client connecte, MAC=%02X:%02X:%02X:%02X:%02X:%02X (AID=%u)\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  info.wifi_ap_staconnected.aid);
  }
}

bool parseMac(String value, uint8_t out[6]) {
  value.trim();
  return parsePhevMac(value.c_str(), out);
}

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n' || c == '\r') out += ' ';
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void climateOutputChanged(bool value) { requestedClimate = value ? 1 : 0; }
void modeOutputChanged(uint16_t value) { requestedMode = value; }
void durationOutputChanged(uint16_t value) { requestedDuration = value; }
void maintenanceOutputChanged(uint16_t value) { requestedMaintenance = value; }

void setupZigbeeEndpoints() {
  epClimate.addBinaryInput(); epClimate.addBinaryOutput();
  epClimate.setBinaryInputDescription("Climate actual");
  epClimate.setBinaryOutputDescription("Climate command");
  epClimate.onBinaryOutputChange(climateOutputChanged);

  epMode.addMultistateInput(); epMode.addMultistateOutput();
  epMode.setMultistateInputStates(4); epMode.setMultistateOutputStates(4);
  epMode.setMultistateInputDescription("Climate mode actual");
  epMode.setMultistateOutputDescription("Climate mode command");
  epMode.onMultistateOutputChange(modeOutputChanged);

  epDuration.addMultistateInput(); epDuration.addMultistateOutput();
  epDuration.setMultistateInputStates(4); epDuration.setMultistateOutputStates(4);
  epDuration.setMultistateInputDescription("Climate duration actual");
  epDuration.setMultistateOutputDescription("Climate duration command");
  epDuration.onMultistateOutputChange(durationOutputChanged);

  epBattery.addAnalogInput(); epBattery.setAnalogInputDescription("Drive battery percent");
  epChargeTime.addAnalogInput(); epChargeTime.setAnalogInputDescription("Charge remaining minutes");
  epOpenMask.addAnalogInput(); epOpenMask.setAnalogInputDescription("Open doors mask");
  epMaintenance.addMultistateInput(); epMaintenance.addMultistateOutput();
  epMaintenance.setMultistateInputStates(3); epMaintenance.setMultistateOutputStates(4);
  epMaintenance.setMultistateInputDescription("Gateway mode normal AP home");
  epMaintenance.setMultistateOutputDescription("Restart gateway mode");
  epMaintenance.onMultistateOutputChange(maintenanceOutputChanged);
  epRssi.addAnalogInput(); epRssi.setAnalogInputDescription("PHEV WiFi RSSI dBm");
  epBatteryAge.addAnalogInput(); epBatteryAge.setAnalogInputDescription("Drive battery age seconds");
  epUptime.addAnalogInput(); epUptime.setAnalogInputDescription("Gateway uptime seconds");
  epBatteryWarning.addAnalogInput(); epBatteryWarning.setAnalogInputDescription("Vehicle battery warning code");
  epRegistrations.addAnalogInput(); epRegistrations.setAnalogInputDescription("Registered remote devices");
  epBatteryValid.addBinaryInput(); epBatteryValid.setBinaryInputDescription("Drive battery valid");
  epClimateTerminated.addBinaryInput(); epClimateTerminated.setBinaryInputDescription("Preconditioning terminated");
  epAcOperating.addBinaryInput(); epAcOperating.setBinaryInputDescription("AC operating actual");
  epHazards.addBinaryInput(); epHazards.setBinaryInputDescription("Hazard lights");
  epInterior.addBinaryInput(); epInterior.setBinaryInputDescription("Interior lights");

  epCar.addBinaryInput(); epCar.setBinaryInputDescription("PHEV protocol online");
  epCharging.addBinaryInput(); epCharging.setBinaryInputDescription("Charging");
  epPlug.addBinaryInput(); epPlug.setBinaryInputDescription("Charge plug connected");
  epLocked.addBinaryInput(); epLocked.setBinaryInputDescription("Doors locked");
  epLights.addBinaryInput(); epLights.setBinaryInputDescription("Headlights");
  epParking.addBinaryInput(); epParking.setBinaryInputDescription("Parking lights");
  epCommandAck.addBinaryInput(); epCommandAck.setBinaryInputDescription("Climate command acknowledged");
  epCommandFailed.addBinaryInput(); epCommandFailed.setBinaryInputDescription("Climate command failed");
  epWifi.addBinaryInput(); epWifi.setBinaryInputDescription("PHEV WiFi associated");

  ZigbeeBinary *binary[] = {&epClimate, &epCar, &epCharging, &epPlug, &epLocked, &epLights, &epParking,
                            &epCommandAck, &epCommandFailed, &epWifi, &epBatteryValid,
                            &epClimateTerminated, &epAcOperating, &epHazards, &epInterior};
  for (auto *ep : binary) {
    ep->setManufacturerAndModel("PHEV-C6", "Outlander-PHEV-Remote");
    ep->setPowerSource(ZB_POWER_SOURCE_MAINS);
    Zigbee.addEndpoint(ep);
  }
  ZigbeeMultistate *multi[] = {&epMode, &epDuration, &epMaintenance};
  for (auto *ep : multi) {
    ep->setManufacturerAndModel("PHEV-C6", "Outlander-PHEV-Remote");
    ep->setPowerSource(ZB_POWER_SOURCE_MAINS);
    Zigbee.addEndpoint(ep);
  }
  ZigbeeAnalog *analog[] = {&epBattery, &epChargeTime, &epOpenMask, &epRssi,
                          &epBatteryAge, &epUptime, &epBatteryWarning, &epRegistrations};
  for (auto *ep : analog) {
    ep->setManufacturerAndModel("PHEV-C6", "Outlander-PHEV-Remote");
    ep->setPowerSource(ZB_POWER_SOURCE_MAINS);
    Zigbee.addEndpoint(ep);
  }
  epTelemetryMask.addAnalogInput(); epTelemetryMask.setAnalogInputDescription("Valid cached telemetry groups");
  epTelemetryMask.setManufacturerAndModel("PHEV-C6", "Outlander-PHEV-Remote");
  epTelemetryMask.setPowerSource(ZB_POWER_SOURCE_MAINS); Zigbee.addEndpoint(&epTelemetryMask);
  epSessionResult.addAnalogInput(); epSessionResult.setAnalogInputDescription("Atomic session ID and result");
  epSessionResult.setManufacturerAndModel("PHEV-C6", "Outlander-PHEV-Remote");
  epSessionResult.setPowerSource(ZB_POWER_SOURCE_MAINS); Zigbee.addEndpoint(&epSessionResult);
}

bool applyStaMac() {
  if (!configured) return false;
  carWifiBegun = false;
  WiFi.mode(WIFI_STA);
  esp_wifi_stop();
  const esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, clonedMac);
  esp_wifi_start();
  if (err != ESP_OK) {
    Serial.printf("Erreur MAC Wi-Fi STA: %s\n", esp_err_to_name(err));
    staMacApplied = false;
    return false;
  }
  uint8_t actualMac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, actualMac) != ESP_OK ||
      memcmp(actualMac, clonedMac, sizeof(clonedMac)) != 0) {
    Serial.println("Erreur: MAC STA differente de la MAC configuree");
    staMacApplied = false;
    return false;
  }
  staMacApplied = true;
  Serial.printf("MAC STA appliquee: %s\n", WiFi.macAddress().c_str());
  return true;
}

bool applyCarWifiProfile() {
#if PHEV_WIFI_LEGACY_BGN
  // Seulement l'interface cliente : ne pas modifier le PA du portail ni Zigbee.
  // Appliquer apres l'initialisation/changement de mode, avant la connexion.
  constexpr uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  const esp_err_t setResult = esp_wifi_set_protocol(WIFI_IF_STA, protocol);
  if (setResult != ESP_OK) {
    Serial.printf("[%lu ms] DIAG WIFI: echec profil STA b/g/n: %s; tentative voiture annulee\n",
                  (unsigned long)millis(), esp_err_to_name(setResult));
    return false;
  }
  uint8_t actualProtocol = 0;
  const esp_err_t getResult = esp_wifi_get_protocol(WIFI_IF_STA, &actualProtocol);
  if (getResult != ESP_OK || actualProtocol != protocol) {
    Serial.printf("[%lu ms] DIAG WIFI: profil STA non confirme: lecture=%s masque=0x%02X attendu=0x%02X; tentative voiture annulee\n",
                  (unsigned long)millis(), esp_err_to_name(getResult), actualProtocol, protocol);
    return false;
  }
  Serial.printf("[%lu ms] DIAG WIFI: profil STA 802.11b/g/n confirme, sans ax, masque=0x%02X\n",
                (unsigned long)millis(), actualProtocol);
#else
  Serial.printf("[%lu ms] DIAG WIFI: profil STA par defaut; test b/g/n desactive\n",
                (unsigned long)millis());
#endif
  return true;
}

void connectCarWifi() {
  if (!configured || (portalActive && !testMode)) return;
  if (!portalActive && !wifiOnlyBoot && !maintenance.active() && !demand.active()) return;
  if (WiFi.status() == WL_CONNECTED) return;
  lastWifiAttemptMs = millis();
  phev.disconnect();
  rawTcp.disconnect("reconfiguration Wi-Fi locale");
  if (!staMacApplied) {
    if (portalActive) { Serial.println("Test PHEV impossible: MAC STA non appliquee"); return; }
    if (!applyStaMac()) return;
  }
  if (portalActive) WiFi.mode(WIFI_AP_STA);
  if (!applyCarWifiProfile()) return;
  if (!carWifiBegun) {
    carWifiBegun = true;
    WiFi.begin(carSsid.c_str(), carPassword.c_str());
  } else if (!WiFi.reconnect()) {
    Serial.println("Wi-Fi PHEV: demande de reconnexion refusee par le pilote");
  }
  Serial.printf("Wi-Fi PHEV: tentative sur %s (statut=%d, dernier echec=%u)\n",
                carSsid.c_str(), (int)WiFi.status(), lastWifiDisconnectReason);
}

void openDemandTransport() {
  sessionResult.begin(demand.reason() == PhevDemandPolicy::Reason::Climate);
  phev.beginDemandSession();
  phev.setReadRequests(demand.reason() == PhevDemandPolicy::Reason::Refresh);
  postClimateRefreshSent = false;
  reportState();
  delay(400); // laisser partir le rapport de debut avant la courte pause radio
  if (SUSPEND_ZIGBEE_DURING_PHEV && zigbeeInitialized && zigbeeStarted && esp_zb_lock_acquire(pdMS_TO_TICKS(200))) {
    Zigbee.stop();
    zigbeeStarted = false;
    zigbeePausedForDemand = true;
    esp_zb_lock_release();
    const esp_err_t sleep = esp_ieee802154_sleep();
    Serial.printf("PHEV: pause Zigbee pour lecture Wi-Fi, radio=%s; appairage conserve\n", esp_err_to_name(sleep));
  }
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false);
  staMacApplied = carWifiBegun = false;
  connectCarWifi();
  const esp_err_t coex = esp_coex_wifi_i154_enable();
  Serial.printf("PHEV: session courte ouverte, limite 60 s, radio=%s\n", esp_err_to_name(coex));
  lastReportMs = millis() - 30000;
}

void finishDemand(bool success, const char *reason,
                  PhevSessionResult::Status failure) {
  if (!demand.active()) return;
  const bool climate = demand.reason() == PhevDemandPolicy::Reason::Climate;
  telemetry.capture(phev.state(), uptimeMs(), !climate);
  const bool acknowledged = climate && phev.state().commandAck;
  phev.disconnect(reason);
  if (climate && !success && !phev.state().commandAck) phev.failClimate();
  sessionResult.finish(acknowledged ? PhevSessionResult::Status::CommandAcknowledged : success ? (climate ? PhevSessionResult::Status::CommandAcknowledged
                                        : PhevSessionResult::Status::RefreshCompleted) : failure);
  demand.finish(); // discard unsent commands, never replay on a later connection
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false); WiFi.mode(WIFI_OFF);
  staMacApplied = carWifiBegun = false;
  if (zigbeePausedForDemand) {
    const esp_err_t receive = esp_ieee802154_receive();
    Zigbee.start();
    zigbeeStarted = true;
    zigbeePausedForDemand = false;
    Serial.printf("PHEV: reprise Zigbee apres lecture, radio=%s\n", esp_err_to_name(receive));
  }
  lastReportMs = millis() - 30000;
  Serial.printf("PHEV: session terminee (%s), TCP et Wi-Fi REMOTE coupes; Zigbee conserve\n", reason);
}

void tickDemand() {
  if (maintenance.active() || maintenance.rebootPending() || portalActive || wifiOnlyBoot || !configured) return;
  const uint64_t clock = uptimeMs();
  if (demand.dailyDue(clock)) { demand.refresh(clock); openDemandTransport(); }
  if (!demand.active()) return;
  // Check deadline BEFORE and AFTER network code (NetworkClient has bounded blocking calls).
  if (demand.observe(clock, false, false, 0, false, false, false) == PhevDemandPolicy::Action::Failure) {
    finishDemand(false, "limite locale 60 s", PhevSessionResult::Status::Timeout); return;
  }
  phev.tick(WiFi.status() == WL_CONNECTED);
  telemetry.capture(phev.state(), uptimeMs(), demand.reason() != PhevDemandPolicy::Reason::Climate);
  if (phev.demandTransportEnded()) { finishDemand(false, "transport termine, pas de reconnexion automatique"); return; }
  const PhevState &s = phev.state();
  const auto action = demand.observe(uptimeMs(), s.online, s.batteryValid, s.lastTelemetryMs,
                                    phev.commandPending(), s.commandAck, s.commandFailed);
  if (action == PhevDemandPolicy::Action::SendClimate) {
    if (!phev.requestClimate(demand.mode(), demand.duration())) {
      finishDemand(false, "commande refusee", PhevSessionResult::Status::CommandFailed); return;
    }
    Serial.println("PHEV: commande clim explicite envoyee une fois; attente ACK et retour voiture");
  } else if (action == PhevDemandPolicy::Action::Success) {
    finishDemand(true, "acquisition terminee"); return;
  } else if (action == PhevDemandPolicy::Action::Failure) {
    finishDemand(false, "echec ou expiration session", s.commandFailed ?
                 PhevSessionResult::Status::CommandFailed : PhevSessionResult::Status::Timeout); return;
  }
  if (demand.sent() && s.commandAck && !postClimateRefreshSent) {
    postClimateRefreshSent = true;
    phev.requestRefresh(); // one readback in this session, never repeat the climate command
  }
}

void startOrResumeZigbee() {
  if (portalActive || wifiOnlyBoot) return;
  if (!zigbeeInitialized) {
    zigbeeInitialized = true;
    if (!zigbeeSeen) { pairingWindow = true; pairingOpenedMs = millis(); }
    const esp_err_t coex = esp_coex_wifi_i154_enable();
    Serial.printf("Radio: coexistence Wi-Fi/802.15.4=%s\n", esp_err_to_name(coex));
    zigbeeStarted = Zigbee.begin(ZIGBEE_END_DEVICE);
    if (zigbeeStarted) {
      const esp_err_t activeCoex = esp_coex_wifi_i154_enable();
      const auto radioConfig = esp_ieee802154_get_coex_config();
      Serial.printf("Radio: arbitrage apres initialisation=%s; priorites RX=%u TX=%u planifie=%u\n",
        esp_err_to_name(activeCoex), radioConfig.idle, radioConfig.txrx, radioConfig.txrx_at);
    }
    Serial.printf("Zigbee initialise: %s; MAC STA actuelle=%s\n", zigbeeStarted ? "oui" : "non", WiFi.macAddress().c_str());
    if (zigbeeStarted && esp_zb_lock_acquire(portMAX_DELAY)) {
      // Uniquement les rapports automatiques de CET ESP; ni association,
      // ni liaisons, ni configuration des autres appareils ne sont effacees.
      // Les valeurs sont publiees manuellement via APS par reportState().
      esp_zb_zcl_reset_all_reporting_info();
      esp_zb_lock_release();
      Serial.println("Zigbee: rapports automatiques locaux reinitialises; publication APS manuelle");
    }
  } else if (zigbeePausedForPortal) {
    Zigbee.start();
    zigbeeStarted = true;
    zigbeePausedForPortal = false;
    if (pairingWindow) pairingOpenedMs = millis();
    Serial.println("Zigbee: repris apres fermeture du portail");
  }
}

void stopPortal(bool resumeNormal = true);

void beginPairing(bool resetNetwork) {
  if (maintenance.active()) {
    Serial.println("Maintenance: revenir au mode normal avant tout appairage Zigbee");
    return;
  }
  if (wifiOnlyBoot) {
    Serial.println("Test Wi-Fi seul: GPIO3 pour le portail, RESET pour le mode normal");
    return;
  }
  if (portalActive) stopPortal(false);
  zigbeeSeen = false;
  prefs.putBool("zbseen", false);
  if (!zigbeeInitialized || zigbeePausedForPortal) startOrResumeZigbee();
  if (resetNetwork) {
    Zigbee.factoryReset(true); // ne touche pas au SSID/MAC PHEV
    return;
  }
  if (!zigbeeStarted) { Zigbee.start(); zigbeeStarted = true; }
  pairingWindow = true;
  pairingOpenedMs = millis();
  Serial.println("Zigbee: fenetre d'appairage 5 minutes");
}

void cacheScanResults(int n) {
  scannedRemoteOptions = "";
  scannedRemoteJson = "[";
  bool first = true;
  uint8_t selectedChannel = 0;
  uint8_t fallbackChannel = 0;
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.indexOf("REMOTE") >= 0) {
      const uint8_t channel = WiFi.channel(i);
      scannedRemoteOptions += "<option value='" + htmlEscape(ssid) + "'>" +
                              htmlEscape(ssid) + " (" + WiFi.RSSI(i) + " dBm)</option>";
      if (!first) scannedRemoteJson += ',';
      scannedRemoteJson += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" +
                           String(WiFi.RSSI(i)) + ",\"channel\":" + String(channel) + "}";
      first = false;
      if (ssid == carSsid) selectedChannel = channel;
      if (!fallbackChannel && channel >= 1 && channel <= 13) fallbackChannel = channel;
    }
  }
  if (selectedChannel >= 1 && selectedChannel <= 13) portalChannel = selectedChannel;
  else if (fallbackChannel) portalChannel = fallbackChannel;
  scannedRemoteJson += ']';
  WiFi.scanDelete();
}

void scanRemoteBeforePortal() {
  portalChannel = 1;
  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks(false, true, false, 120);
  if (n < 0) Serial.printf("Scan REMOTE indisponible: %d\n", n);
  cacheScanResults(n > 0 ? n : 0);
}

void startPortal() {
  if (demand.active()) finishDemand(false, "passage maintenance AP");
  maintenanceBoot = true; // meme sans configuration : retour normal par redemarrage
  maintenance.setMode(BootMode::AccessPoint);
  if (portalActive) { portalOpenedMs = millis(); return; }
  if (zigbeeInitialized && zigbeeStarted) {
    Zigbee.stop();
    zigbeeStarted = false;
    zigbeePausedForPortal = true;
  }
  phev.disconnect();
  WiFi.disconnect(false);
  scanRemoteBeforePortal();
  // Le portail de configuration demarre sans MAC usurpee, comme au tout
  // premier demarrage, sur un canal fixe compatible avec les telephones.
  staMacApplied = false;
  carWifiBegun = false;
  WiFi.mode(WIFI_AP);
  testMode = false;
  testError = "";
  if (WiFi.softAP(portalSsid.c_str(), portalPassword.c_str(), 1, false, 2)) {
    portalActive = true;
    portalOpenedMs = millis();
    Serial.printf("Portail ouvert canal 1: %s / %s / http://192.168.4.1\n", portalSsid.c_str(), portalPassword.c_str());
  } else {
    Serial.println("Erreur: demarrage du point d'acces impossible");
    WiFi.mode(WIFI_OFF);
    maintenance.queueReboot(BootMode::Normal);
  }
}

void stopPortal(bool resumeNormal) {
  if (!portalActive) return;
  testMode = false;
  scanInProgress = false;
  scanPending = false;
  testConnectPending = false;
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  WiFi.mode(WIFI_STA);
  Serial.println("Portail ferme");
  if (maintenanceBoot && resumeNormal) {
    Serial.println("Redemarrage en mode normal");
    maintenance.queueReboot(BootMode::Normal);
    return;
  }
  maintenance.setMode(BootMode::Normal);
  WiFi.mode(WIFI_OFF);
  startOrResumeZigbee();
}

bool prepareTestPortal() {
  if (staMacApplied) return WiFi.mode(WIFI_AP_STA);

  // Ce redemarrage unique du PA se fait seulement apres clic explicite sur
  // "Connecter a la voiture", jamais pendant la connexion initiale au portail.
  WiFi.softAPdisconnect(true);
  portalActive = false;
  WiFi.mode(WIFI_STA);
  if (applyStaMac()) {
    WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAP(portalSsid.c_str(), portalPassword.c_str(), portalChannel, false, 2)) {
      portalActive = true;
      portalOpenedMs = millis();
      Serial.printf("Portail de test ouvert canal %u; reconnecter le telephone\n", portalChannel);
      return true;
    }
  }

  testError = "Impossible de preparer le Wi-Fi de test; portail de configuration restaure";
  Serial.println(testError);
  staMacApplied = false;
  carWifiBegun = false;
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(portalSsid.c_str(), portalPassword.c_str(), 1, false, 2)) {
    portalActive = true;
    portalOpenedMs = millis();
    return false;
  }
  Serial.println("Portail indisponible; redemarrage normal");
  ESP.restart();
  return false;
}

String statusJson() {
  const PhevState s = (testMode || wifiOnlyBoot) ? phev.state() : telemetry.state(uptimeMs());
  String result = "{\"configured\":" + String(configured ? "true" : "false");
  result += ",\"firmware\":\"" + String(FIRMWARE_VERSION) + "\"";
  result += ",\"zigbee\":" + String(!zigbeePausedForPortal && Zigbee.connected() ? "true" : "false");
  result += ",\"pairing\":" + String(pairingWindow ? "true" : "false");
  result += ",\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  result += ",\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  result += ",\"wifi_status\":" + String((int)WiFi.status());
  result += ",\"wifi_disconnect_reason\":" + String(lastWifiDisconnectReason);
  result += ",\"wifi_ip\":\"" + jsonEscape(WiFi.localIP().toString()) + "\"";
  result += ",\"wifi_gateway\":\"" + jsonEscape(WiFi.gatewayIP().toString()) + "\"";
  result += ",\"sta_mac\":\"" + jsonEscape(WiFi.macAddress()) + "\"";
  result += ",\"tcp\":" + String(phev.tcpConnected() ? "true" : "false");
  result += ",\"tcp_connect_count\":" + String(phev.tcpConnectCount());
  result += ",\"tcp_rx_bytes\":" + String(phev.tcpRxBytes());
  result += ",\"tcp_rx_frames\":" + String(phev.tcpRxFrames());
  result += ",\"tcp_tx_pings\":" + String(phev.tcpTxPings());
  result += ",\"tcp_first_rx_hex\":\"" + phev.tcpFirstRxHex() + "\"";
  result += ",\"ping_ack\":" + String(phev.pingAcknowledged() ? "true" : "false");
  result += ",\"ping_ack_count\":" + String(phev.pingAckCount());
  result += ",\"before_portal\":{\"valid\":" + String(prefs.getBool("snapvalid", false) ? "true" : "false");
  result += ",\"wifi_status\":" + String(prefs.getInt("snapws", -1));
  result += ",\"wifi_disconnect_reason\":" + String(prefs.getInt("snapwr", 0));
  result += ",\"wifi_ip\":\"" + jsonEscape(prefs.getString("snapip", "")) + "\"";
  result += ",\"wifi_gateway\":\"" + jsonEscape(prefs.getString("snapgw", "")) + "\"";
  result += ",\"sta_mac\":\"" + jsonEscape(prefs.getString("snapmac", "")) + "\"";
  result += ",\"tcp\":" + String(prefs.getBool("snaptcp", false) ? "true" : "false");
  result += ",\"phev\":" + String(prefs.getBool("snapphev", false) ? "true" : "false");
  result += ",\"uptime_seconds\":" + String(prefs.getUInt("snapage", 0)) + "}";
  result += ",\"test_mode\":" + String(testMode ? "true" : "false");
  result += ",\"test_error\":\"" + jsonEscape(testError) + "\"";
  result += ",\"phev\":" + String(phev.state().online ? "true" : "false");
  result += ",\"session_active\":" + String(demand.active() ? "true" : "false");
  result += ",\"telemetry_valid_mask\":" + String(telemetry.mask(uptimeMs()));
  result += ",\"battery_age_seconds\":" + String(s.batteryValid ? static_cast<long>(telemetry.batteryAgeSeconds(uptimeMs())) : -1);
  result += ",\"battery\":" + String(s.batteryValid ? s.batteryPercent : -1);
  result += ",\"climate\":" + String(s.climateValid ? (s.climateOn ? 1 : 0) : -1);
  result += ",\"climate_mode\":" + String(s.climateMode);
  result += ",\"climate_duration\":" + String(s.climateDuration);
  result += ",\"charging\":" + String(s.chargingValid ? (s.charging ? 1 : 0) : -1);
  result += ",\"plugged\":" + String(s.plugValid ? (s.plugged ? 1 : 0) : -1);
  result += ",\"charge_remaining\":" + String(s.chargeTimeValid ? s.chargeRemaining : -1);
  result += ",\"doors_locked\":" + String(s.doorsValid ? (s.doorsLocked ? 1 : 0) : -1);
  result += ",\"open_doors_mask\":" + String(s.doorsValid ? s.openMask : -1);
  result += ",\"headlights\":" + String(s.doorsValid ? (s.headlights ? 1 : 0) : -1);
  result += ",\"parking_lights\":" + String(s.batteryValid ? (s.parkingLights ? 1 : 0) : -1);
  result += ",\"command_pending\":" + String(phev.commandPending() ? "true" : "false");
  result += ",\"command_ack\":" + String(phev.state().commandAck ? "true" : "false");
  result += ",\"command_failed\":" + String(phev.state().commandFailed ? "true" : "false");
  result += ",\"uptime_seconds\":" + String(millis() / 1000);
  result += ",\"reset_reason\":" + String((int)esp_reset_reason());
  result += ",\"portal\":" + String(portalActive ? "true" : "false");
  result += ",\"maintenance_boot\":" + String(maintenanceBoot ? "true" : "false");
  result += ",\"gateway_mode\":\"" + String(maintenance.homeMode() ? "wifi-seb" : portalActive ? "ap" : "normal") + "\"";
  result += ",\"home_configured\":" + String(maintenance.homeConfigured() ? "true" : "false");
  result += ",\"ota_updating\":" + String(maintenance.updating() ? "true" : "false");
  result += ",\"battery_valid\":" + String(s.online && s.batteryValid ? "true" : "false");
  result += ",\"battery_age\":" + String(s.batteryValid ? static_cast<int>((millis() - s.batteryUpdatedMs) / 1000) : -1);
  result += ",\"portal_clients\":" + String(portalActive ? WiFi.softAPgetStationNum() : 0);
  return result + "}";
}

void showPage() {
  String page = F("<!doctype html><html lang='fr'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>PHEV C6</title><style>body{font:16px system-ui;max-width:620px;margin:24px auto;padding:0 14px}"
                  "input,select,button{font:inherit;box-sizing:border-box;width:100%;padding:9px;margin:6px 0 16px}"
                  "fieldset{border:1px solid #bbb;margin:18px 0}small{color:#555}</style><h1>PHEV C6</h1>");
  page += "<p>Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "connecte" : "non connecte") +
          " · Zigbee: " + String(!zigbeePausedForPortal && Zigbee.connected() ? "connecte" : "en pause/non connecte") +
          " · Protocole voiture: " + String(phev.state().online ? "connecte" : "non connecte") + "</p>";
  if (prefs.getBool("snapvalid", false)) {
    page += "<fieldset><legend>Diagnostic avant ouverture du portail</legend><p>Wi-Fi statut " +
            String(prefs.getInt("snapws", -1)) + ", raison deconnexion " +
            String(prefs.getInt("snapwr", 0)) + ", IP " +
            htmlEscape(prefs.getString("snapip", "")) + ", passerelle " +
            htmlEscape(prefs.getString("snapgw", "")) + ".</p><p>MAC STA " +
            htmlEscape(prefs.getString("snapmac", "")) + ", TCP " +
            String(prefs.getBool("snaptcp", false) ? "oui" : "non") + ", protocole " +
            String(prefs.getBool("snapphev", false) ? "oui" : "non") +
            ". <a href='/status'>Diagnostic complet</a></p></fieldset>";
  }
  page += F("<fieldset><legend>Configuration voiture</legend><form method='post' action='/save'>"
            "<label>Reseau REMOTE (recherche ou saisie)</label><select id='net' onchange='if(this.value)document.getElementById(\"ssid\").value=this.value'>"
            "<option value=''>Choisir un reseau scanne</option>");
  page += scannedRemoteOptions;
  page += "</select><button type='button' id='scanButton' onclick='runScan()'>Scanner les Wi-Fi maintenant</button>"
          "<small id='scanStatus'>Le scan peut ralentir brievement le point d'acces.</small>"
          "<input id='ssid' name='ssid' maxlength='32' required value='" + htmlEscape(carSsid) + "'>";
  page += F("<label>Mot de passe Wi-Fi voiture</label><input name='pass' type='password' maxlength='63' placeholder='laisser vide pour conserver'>");
  page += "<label>MAC cliente deja enregistree dans votre voiture</label><input name='mac' maxlength='17' required value='" + htmlEscape(clonedMacText) + "'><small>Votre propre identite cliente uniquement. Deconnecter l'appareil original; une seule passerelle active.</small>";
  page += F("<small>Ne pas utiliser le telephone et l'ESP simultanement sur la voiture avec la meme MAC.</small>"
            "<button>Enregistrer et redemarrer</button></form></fieldset>"
            "<p><a href='/test'>Page de test PHEV sans Zigbee</a></p>"
            "<fieldset><legend>Zigbee</legend><p>Association conservee. Pour un nouvel appairage : revenir au mode normal, puis utiliser GPIO2.</p></fieldset>"
            "<p><small>Diagnostic JSON : <a href='/status'>/status</a>. Le portail s'arrete apres 30 minutes.</small></p>"
            "<script>async function runScan(){let b=document.getElementById('scanButton'),s=document.getElementById('scanStatus');"
            "b.disabled=true;s.textContent='Scan en cours...';try{let r=await fetch('/scan',{method:'POST'});"
            "if(!r.ok)throw Error(await r.text());await pollScan();}catch(e){s.textContent=e.message;b.disabled=false;}}"
            "async function pollScan(){let b=document.getElementById('scanButton'),s=document.getElementById('scanStatus');"
            "try{let j=await(await fetch('/scan')).json();if(j.busy){setTimeout(pollScan,600);return;}"
            "if(j.error){s.textContent='Echec du scan ('+j.error+').';}else{let select=document.getElementById('net');"
            "select.replaceChildren(new Option('Choisir un reseau scanne',''));"
            "for(let w of j.networks)select.add(new Option(w.ssid+' ('+w.rssi+' dBm)',w.ssid));"
            "s.textContent=j.networks.length+' reseau(x) REMOTE trouve(s).';}}catch(e){s.textContent=e.message;}b.disabled=false;}"
            "</script></html>");
  page.replace("</html>", maintenance.controlsHtml() + "</html>");
  maintenance.decoratePage(page);
  web.send(200, "text/html; charset=utf-8", page);
}

void showTestPage() {
  String page = R"HTML(<!doctype html><html lang="fr"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Test PHEV C6</title>
<style>body{font:16px system-ui;max-width:680px;margin:24px auto;padding:0 14px}
button,select{font:inherit;padding:9px;margin:6px 4px 10px 0}button{cursor:pointer}
fieldset{border:1px solid #bbb;margin:16px 0}pre{white-space:pre-wrap;background:#eee;padding:12px}
small{color:#555}</style><h1>Test direct PHEV</h1><p><a href="/">← Configuration</a></p>
<p>Ce test utilise le Wi-Fi de la voiture sans Zigbee. Au premier clic sur « Connecter »,
le portail se relance une fois avec la MAC cliente du PHEV, puis passe en mode AP+STA.
Reconnectez le téléphone à PHEV-C6 et rechargez cette page après quelques secondes.</p>
<p><button onclick="send('/test/connect')">Connecter à la voiture</button>
<button onclick="send('/test/disconnect')">Déconnecter la voiture</button>
<button onclick="send('/test/refresh')">Rafraîchir les états</button></p>
<fieldset><legend>Isoler la connexion voiture</legend>
<p>Ce test redémarre le module avec uniquement le Wi-Fi de la voiture.
Le portail se ferme et Zigbee reste arrêté. Lire le résultat dans le moniteur série USB à 115200 bauds.
GPIO3 rouvre le portail ; un reset revient au fonctionnement normal.</p>
<form method="post" action="/test/solo"><button>Test Wi-Fi seul (console USB)</button></form></fieldset>
<fieldset><legend>Préclimatisation</legend>
<label>Mode <select id="mode"><option value="1">Froid</option><option value="2" selected>Chaud</option>
<option value="3">Dégivrage pare-brise</option></select></label>
<label>Durée <select id="duration"><option value="10">10 min</option><option value="20">20 min</option>
<option value="30">30 min</option></select></label><br>
<button onclick="climate('on')">Démarrer</button><button onclick="climate('off')">Arrêter</button>
<p><small>Une commande acceptée n'est pas forcément exécutée : vérifier l'acquittement puis l'état réellement renvoyé par la voiture.</small></p>
</fieldset><p id="message"></p><pre id="state">Chargement...</pre>
<script>
const text=(id,value)=>document.getElementById(id).textContent=value;
async function send(path,body){try{let r=await fetch(path,{method:'POST',body});
let msg=await r.text();text('message',(r.ok?'✓ ':'Erreur : ')+msg);await status();}
catch(e){text('message','Connexion au portail perdue : reconnecter le téléphone au réseau PHEV-C6. '+e.message);}}
async function climate(action){let p=new URLSearchParams({action,mode:document.getElementById('mode').value,
duration:document.getElementById('duration').value});await send('/test/climate',p);}
async function status(){try{let s=await(await fetch('/status',{cache:'no-store'})).json();
let yn=v=>v<0?'inconnu':(v?'oui':'non');
let modes=['inconnu','froid','chaud','dégivrage'];
text('state',[
'Portail : '+(s.portal?'ouvert':'fermé')+' | Mode test : '+(s.test_mode?'actif':'inactif'),
'Erreur test : '+(s.test_error||'aucune'),
'Wi-Fi voiture : '+(s.wifi?'connecté ('+s.wifi_rssi+' dBm)':'déconnecté'),
'TCP : '+(s.tcp?'connecté':'déconnecté')+' | connexions : '+s.tcp_connect_count+
' | octets reçus : '+s.tcp_rx_bytes+' | début : '+(s.tcp_first_rx_hex||'aucun'),
'Trames reçues : '+s.tcp_rx_frames+' | Pings envoyés : '+s.tcp_tx_pings+' | confirmés : '+s.ping_ack_count,
'Protocole PHEV : '+(s.phev?'connecté':'déconnecté'),
'Climatisation : '+yn(s.climate)+' | mode : '+(modes[s.climate_mode]||'inconnu')+' | durée : '+(s.climate_duration||'?')+' min',
'Dernière commande : '+(s.command_pending?'en attente':s.command_failed?'échec':s.command_ack?'acquittée':'aucune'),
'Batterie traction : '+(s.battery<0?'inconnue':s.battery+' %'),
'Prise branchée : '+yn(s.plugged)+' | Charge en cours : '+yn(s.charging)+
' | Temps restant : '+(s.charge_remaining<0?'?':s.charge_remaining)+' min',
'Portes verrouillées : '+yn(s.doors_locked)+' | Masque portes ouvertes : '+s.open_doors_mask,
'Phares : '+yn(s.headlights)+' | Veilleuses : '+yn(s.parking_lights),
'Uptime : '+s.uptime_seconds+' s | Reset : '+s.reset_reason
].join('\n'));}catch(e){text('state','Portail inaccessible. Reconnecter le téléphone au réseau PHEV-C6.');}}
status();setInterval(status,2000);
</script></html>)HTML";
  maintenance.decoratePage(page);
  web.send(200, "text/html; charset=utf-8", page);
}

void setupWeb() {
  web.on("/", HTTP_GET, showPage);
  web.on("/test", HTTP_GET, showTestPage);
  web.on("/status", HTTP_GET, [] { web.send(200, "application/json", statusJson()); });
  web.on("/scan", HTTP_GET, [] {
    String body = "{\"busy\":" + String(scanPending || scanInProgress ? "true" : "false") +
                  ",\"error\":" + String(scanError) + ",\"networks\":" + scannedRemoteJson + "}";
    web.send(200, "application/json", body);
  });
  web.on("/scan", HTTP_POST, [] {
    if (!portalActive) { web.send(409, "text/plain", "Portail non actif"); return; }
    if (testMode || testConnectPending) { web.send(409, "text/plain", "Deconnecter d'abord la voiture"); return; }
    if (scanPending || scanInProgress) { web.send(200, "text/plain", "Scan deja en cours"); return; }
    scanError = 0;
    scanPending = true;
    scanRequestedMs = millis();
    web.send(202, "text/plain", "Scan demarre");
  });
  web.on("/test/connect", HTTP_POST, [] {
    if (!portalActive || !configured) { web.send(409, "text/plain", "Configurer d'abord le Wi-Fi et la MAC du vehicule"); return; }
    if (scanPending || scanInProgress) { web.send(409, "text/plain", "Attendre la fin du scan Wi-Fi"); return; }
    testConnectPending = true;
    testConnectRequestedMs = millis();
    web.send(202, "text/plain", "Preparation de la MAC et connexion voiture. Le portail va se relancer une fois : reconnecter le telephone.");
  });
  web.on("/test/solo", HTTP_POST, [] {
    if (!portalActive || !configured) {
      web.send(409, "text/plain", "Configurer d'abord le Wi-Fi et la MAC du vehicule"); return;
    }
    if (prefs.putBool("soloboot", true) == 0) {
      web.send(500, "text/plain", "Impossible de memoriser le mode de test"); return;
    }
    web.send(200, "text/plain; charset=utf-8",
             "Redemarrage en test Wi-Fi seul. Le portail se ferme. Lire la console USB a 115200 bauds pendant deux minutes. GPIO3 pour revenir au portail, RESET pour le mode normal.");
    delay(350);
    ESP.restart();
  });
  web.on("/test/disconnect", HTTP_POST, [] {
    testMode = false;
    testConnectPending = false;
    phev.disconnect();
    WiFi.disconnect(false);
    web.send(200, "text/plain", "Voiture deconnectee; portail maintenu.");
  });
  web.on("/test/refresh", HTTP_POST, [] {
    if (!testMode || !phev.requestRefresh()) { web.send(409, "text/plain", "Protocole voiture non connecte ou commande deja en cours"); return; }
    web.send(202, "text/plain", "Actualisation demandee a la voiture.");
  });
  web.on("/test/climate", HTTP_POST, [] {
    if (!testMode || !phev.state().online) { web.send(409, "text/plain", "Connecter d'abord le protocole voiture"); return; }
    const String action = web.arg("action");
    const int mode = web.arg("mode").toInt();
    const int duration = web.arg("duration").toInt();
    if ((action != "on" && action != "off") || mode < 1 || mode > 3 ||
        (duration != 10 && duration != 20 && duration != 30)) {
      web.send(400, "text/plain", "Mode ou duree invalide"); return;
    }
    if (!phev.requestClimate(action == "on" ? mode : 0, duration)) {
      web.send(409, "text/plain", "Commande refusee ou deja en cours"); return;
    }
    web.send(202, "text/plain", "Commande envoyee; attendre l'acquittement et l'etat du vehicule.");
  });
  web.on("/save", HTTP_POST, [] {
    if (!portalActive || maintenance.homeMode()) {
      web.send(403, "text/plain", "Configurer l'identite PHEV uniquement depuis le point d'acces local."); return;
    }
    String ssid = web.arg("ssid"), pass = web.arg("pass"), mac = web.arg("mac");
    mac.trim();
    uint8_t parsed[6];
    if (ssid.length() == 0 || ssid.length() > 32 || ssid.indexOf("REMOTE") < 0 ||
        pass.length() > 63 || !parseMac(mac, parsed)) {
      web.send(400, "text/plain; charset=utf-8", "SSID REMOTE ou MAC invalide.");
      return;
    }
    if (pass.isEmpty()) pass = carPassword;
    if (pass.length() < 8) {
      web.send(400, "text/plain; charset=utf-8", "Mot de passe Wi-Fi trop court.");
      return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("mac", mac);
    web.send(200, "text/plain; charset=utf-8", "Configuration enregistree. Redemarrage...");
    delay(250);
    ESP.restart();
  });
  web.on("/pair", HTTP_POST, [] {
    if (maintenance.active()) { web.send(409, "text/plain", "Revenir au mode normal pour Zigbee; GPIO2 permet ensuite l'appairage"); return; }
    if (zigbeeSeen || Zigbee.connected()) {
      web.send(200, "text/plain; charset=utf-8", "Le portail se ferme. Association Zigbee effacee puis redemarrage...");
      delay(250);
      beginPairing(true);
    } else {
      web.send(200, "text/plain; charset=utf-8", "Le portail se ferme. Appairage Zigbee pendant 5 minutes.");
      delay(250);
      beginPairing(false);
    }
  });
  maintenance.setBeforeUpdate([] {
    testMode = false; testConnectPending = scanPending = false;
    requestedClimate = -1;
    phev.disconnect("debut OTA : aucune commande voiture");
    rawTcp.disconnect("debut OTA");
  });
  maintenance.installRoutes();
  web.begin();
}

bool reportPresentValue(ZigbeeEP &ep, uint16_t cluster) {
  // Rapports ZCL standards via APS : pas de table de rapports automatiques.
  static uint8_t payloads[29][10]{}; // rester valide apres retour de l'API APS
  static uint8_t sequence = 0;
  if (zigbeePausedForDemand || !Zigbee.connected() || ep.getEndpoint() > 28) return false;
  if (!esp_zb_lock_acquire(portMAX_DELAY)) return false;
  const esp_zb_zcl_attr_t *attribute = esp_zb_zcl_get_attribute(ep.getEndpoint(), cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 0x0055);
  uint8_t *payload = payloads[ep.getEndpoint()];
  const size_t length = attribute ? encodePresentValueReport(payload, 10, sequence++, attribute->type, attribute->data_p) : 0;
  if (!length) {
    esp_zb_lock_release(); return false;
  }
  esp_zb_apsde_data_req_t request{};
  request.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  request.dst_addr.addr_short = 0x0000; request.dst_endpoint = 1;
  request.src_endpoint = ep.getEndpoint(); request.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  request.cluster_id = cluster; request.asdu = payload; request.asdu_length = length;
  request.tx_options = ESP_ZB_APSDE_TX_OPT_ACK_TX;
  request.radius = 30;
  const esp_err_t result = esp_zb_aps_data_request(&request);
  esp_zb_lock_release();
  return result == ESP_OK;
}

void reportState() {
  if (zigbeePausedForDemand || !Zigbee.connected()) return;
  const uint64_t clock = uptimeMs();
  telemetry.capture(phev.state(), clock, demand.reason() != PhevDemandPolicy::Reason::Climate);
  const PhevState s = telemetry.state(clock);
  epSessionResult.setAnalogInput(sessionResult.word()); reportPresentValue(epSessionResult, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
  epTelemetryMask.setAnalogInput(telemetry.mask(clock)); reportPresentValue(epTelemetryMask, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
  epWifi.setBinaryInput(demand.active() && WiFi.status() == WL_CONNECTED); reportPresentValue(epWifi, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  epCar.setBinaryInput(phev.state().online); reportPresentValue(epCar, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  epCommandAck.setBinaryInput(phev.state().commandAck); reportPresentValue(epCommandAck, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  epCommandFailed.setBinaryInput(phev.state().commandFailed); reportPresentValue(epCommandFailed, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  epBatteryValid.setBinaryInput(s.batteryValid); reportPresentValue(epBatteryValid, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  epMaintenance.setMultistateInput(1); reportPresentValue(epMaintenance, ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT);
  epUptime.setAnalogInput(clock / 1000); reportPresentValue(epUptime, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
  if (demand.active() && WiFi.status() == WL_CONNECTED) { epRssi.setAnalogInput(WiFi.RSSI()); reportPresentValue(epRssi, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT); }
  if (s.climateValid) { epClimate.setBinaryInput(s.climateOn); reportPresentValue(epClimate, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
  if (s.climateSettingsValid) { epMode.setMultistateInput(s.climateMode); reportPresentValue(epMode, ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT); }
  if (s.climateSettingsValid) {
    epDuration.setMultistateInput(s.climateDuration / 10); reportPresentValue(epDuration, ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT);
  }
  if (s.batteryValid) {
    epBattery.setAnalogInput(s.batteryPercent);
    const bool sent = reportPresentValue(epBattery, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
    epBatteryAge.setAnalogInput(telemetry.batteryAgeSeconds(clock)); reportPresentValue(epBatteryAge, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
    Serial.printf("Zigbee: batterie traction=%u %% rapport=%s\n", s.batteryPercent, sent ? "emis" : "echec");
  }
  if (s.climateValid) { epClimateTerminated.setBinaryInput(s.climateTerminated); reportPresentValue(epClimateTerminated, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
  if (s.acOperatingValid) { epAcOperating.setBinaryInput(s.acOperating); reportPresentValue(epAcOperating, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
  if (s.lightsValid) {
    epHazards.setBinaryInput(s.hazards); reportPresentValue(epHazards, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
    epInterior.setBinaryInput(s.interiorLights); reportPresentValue(epInterior, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  }
  if (s.batteryWarningValid) { epBatteryWarning.setAnalogInput(s.batteryWarning); reportPresentValue(epBatteryWarning, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT); }
  if (s.registrationsValid) { epRegistrations.setAnalogInput(s.registrations); reportPresentValue(epRegistrations, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT); }
  if (s.chargingValid) { epCharging.setBinaryInput(s.charging); reportPresentValue(epCharging, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
  if (s.plugValid) { epPlug.setBinaryInput(s.plugged); reportPresentValue(epPlug, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
  if (s.chargeTimeValid) { epChargeTime.setAnalogInput(s.chargeRemaining); reportPresentValue(epChargeTime, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT); }
  if (s.doorsValid) {
    epLocked.setBinaryInput(s.doorsLocked); reportPresentValue(epLocked, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
    epOpenMask.setAnalogInput(s.openMask); reportPresentValue(epOpenMask, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT);
    epLights.setBinaryInput(s.headlights); reportPresentValue(epLights, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT);
  }
  if (s.batteryValid) { epParking.setBinaryInput(s.parkingLights); reportPresentValue(epParking, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT); }
}

void handleZigbeeCommands() {
  const uint16_t gatewayMode = requestedMaintenance.exchange(0xffff);
  if (gatewayMode >= 1 && gatewayMode <= 3) {
    requestedClimate = -1; // une demande de maintenance a priorite sur la clim
    if (maintenance.queueReboot(static_cast<BootMode>(gatewayMode - 1))) {
      finishDemand(false, "commande maintenance");
      phev.disconnect("commande Zigbee maintenance/redemarrage");
      reportState();
    }
    return;
  }
  if (maintenance.rebootPending()) { requestedClimate = -1; return; }
  if (gatewayMode == 4) {
    const bool active = demand.active();
    if (demand.refresh(uptimeMs()) && !active) openDemandTransport();
    Serial.println("PHEV: actualisation demandee, session courte (pas de connexion permanente)");
    return;
  }
  const uint16_t mode = requestedMode.exchange(0xffff);
  const uint16_t duration = requestedDuration.exchange(0xffff);
  if (mode != 0xffff) {
    const uint16_t value = mode;
    if (value >= 1 && value <= 3) selectedMode = value;
    Serial.printf("Zigbee: mode recu=%u, selection=%u\n", value, selectedMode);
  }
  if (duration != 0xffff) {
    const uint16_t value = duration;
    if (value >= 1 && value <= 3) selectedDuration = value * 10;
    Serial.printf("Zigbee: duree recue=%u, selection=%u min\n", value, selectedDuration);
  }
  const int8_t command = requestedClimate.exchange(-1);
  if (command >= 0) {
    const bool active = demand.active();
    Serial.printf("Zigbee: commande clim=%d mode=%u duree=%u session_active=%d raison=%d\n",
                  command, selectedMode, selectedDuration, active, static_cast<int>(demand.reason()));
    if (!demand.climate(uptimeMs(), command ? selectedMode : 0, selectedDuration))
      Serial.println("PHEV: commande clim refusee (une commande est deja en cours)");
    else {
      if (!active) openDemandTransport();
      else sessionResult.promoteClimate(); // same session/deadline, never increment for a promotion
      Serial.println("PHEV: commande clim en attente de connexion, valide seulement dans cette session 60 s");
    }
    lastReportMs = millis() - 30000; // transmettre rapidement ACK/echec; pas d'etat suppose
  }
}

void handleButtons() {
  static bool oldZ = false, oldP = false;
  static uint32_t zDownMs = 0;
  const bool z = digitalRead(PIN_ZIGBEE) == LOW;
  const bool p = digitalRead(PIN_PORTAL) == LOW;
  if (z && !oldZ) zDownMs = millis();
  if (!z && oldZ && elapsed(millis(), zDownMs, 50)) {
    if (elapsed(millis(), zDownMs, 3000) && (zigbeeSeen || Zigbee.connected())) beginPairing(true);
    else if (!zigbeeSeen && !Zigbee.connected()) beginPairing(false);
    else Serial.println("Zigbee: maintenir GPIO2 3 s pour effacer l'ancien reseau");
  }
  if (p && !oldP) {
    if (configured && !portalActive) {
      // Redemarrer en maintenance avant d'initialiser Zigbee ou le Wi-Fi PHEV.
      // C'est le meme etat radio que lors de la premiere configuration.
      prefs.putBool("snapvalid", true);
      prefs.putInt("snapws", (int)WiFi.status());
      prefs.putInt("snapwr", lastWifiDisconnectReason);
      prefs.putString("snapip", WiFi.localIP().toString());
      prefs.putString("snapgw", WiFi.gatewayIP().toString());
      prefs.putString("snapmac", WiFi.macAddress());
      prefs.putBool("snaptcp", rawTcpMode() ? rawTcp.connected() : phev.tcpConnected());
      prefs.putBool("snapphev", phev.state().online);
      prefs.putUInt("snapage", millis() / 1000);
      prefs.putBool("portalboot", true);
      Serial.println("GPIO3: redemarrage en mode portail");
      delay(100);
      ESP.restart();
    } else startPortal();
  }
  oldZ = z; oldP = p;
}

void updateLed() {
  const uint32_t now = millis();
  if (!elapsed(now, lastLedMs, 80)) return;
  lastLedMs = now;
  uint8_t r = 0, g = 0, b = 0;
  if (maintenance.active()) b = (now / 500) % 2 ? 0 : 30;
  else if (!configured) b = 30;
  else if (wifiOnlyBoot) {
    // En diagnostic brut, ne jamais annoncer le protocole PHEV en ligne.
    g = 20; r = 30;
    if (rawTcpMode() && rawTcp.connected()) b = 20;
    else if (!rawTcpMode() && phev.state().online) r = 0;
  }
  else if (pairingWindow) r = (now / 130) % 2 ? 0 : 30;
  else if (!Zigbee.connected()) r = (now / 600) % 2 ? 0 : 30;
  else if (!phev.state().online) { r = 30; g = 12; }
  else g = 30;
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}

void setup() {
  Serial.begin(115200);
  // Attente bornee : ne jamais bloquer le fonctionnement sans ordinateur.
  const uint32_t consoleStart = millis();
  while (!Serial && !elapsed(millis(), consoleStart, 1500)) delay(10);
  Serial.setDebugOutput(true); // NetworkClient log_d/log_e sur l'USB natif.
  Serial.printf("PHEV-C6 firmware %s\n", FIRMWARE_VERSION);
  Serial.printf("Demarrage, raison reset=%d\n", (int)esp_reset_reason());
  Serial.printf("DIAG TCP: codes libc ECONNRESET=%d ECONNREFUSED=%d ECONNABORTED=%d ENOTCONN=%d EPIPE=%d ETIMEDOUT=%d EWOULDBLOCK=%d\n",
                ECONNRESET, ECONNREFUSED, ECONNABORTED, ENOTCONN, EPIPE, ETIMEDOUT, EWOULDBLOCK);
  pinMode(PIN_ZIGBEE, INPUT_PULLUP);
  pinMode(PIN_PORTAL, INPUT_PULLUP);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 30);
  prefs.begin("phev", false);
  maintenance.begin();
  const BootMode requestedBootMode = maintenance.consumeBootMode();
  carSsid = prefs.getString("ssid", "");
  carPassword = prefs.getString("pass", "");
  clonedMacText = prefs.getString("mac", ""); // preserve existing NVS identity
  configured = carSsid.length() && carPassword.length() && parseMac(clonedMacText, clonedMac);
  Serial.println("PHEV: identite cliente configuree localement; une seule passerelle active. Aucune commande clim au demarrage.");
  zigbeeSeen = prefs.getBool("zbseen", false);
  const bool legacyPortal = prefs.getBool("portalboot", false);
  if (legacyPortal) prefs.putBool("portalboot", false);
  maintenanceBoot = legacyPortal || requestedBootMode != BootMode::Normal;
  wifiOnlyBoot = prefs.getBool("soloboot", false);
  if (wifiOnlyBoot) prefs.remove("soloboot"); // un seul demarrage, retour normal au reset
  if (PHEV_PROTOCOL_WATCH_TEST) {
    // Dedicated diagnostic build: never start Zigbee or control endpoints.
    wifiOnlyBoot = true;
    maintenanceBoot = false;
    phev.setWatchOnly(true);
    watchTestStartedMs = millis();
    Serial.println("WATCH TEST: Wi-Fi STA seul force; pings/ACK seulement; ecritures de registres bloquees; arret Wi-Fi apres 90 s. RESET relance ce test.");
  }

  const uint32_t id = static_cast<uint32_t>(ESP.getEfuseMac());
  char suffix[9]; snprintf(suffix, sizeof(suffix), "%08lX", (unsigned long)id);
  portalSsid = String("PHEV-C6-") + suffix;
  portalPassword = String("Phev") + suffix; // affiche aussi sur le moniteur serie
  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.setSleep(false); // diagnostic: profil de la lecture Wi-Fi seule reussie
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // les relances sont pilotees par connectCarWifi()
  if (PHEV_PROTOCOL_WATCH_TEST && !configured) {
    Serial.println("WATCH TEST: configuration absente, test bloque; aucun Zigbee ni commande.");
    watchTestStopped = true;
    WiFi.mode(WIFI_OFF);
    return;
  }
  if (wifiOnlyBoot && configured && !maintenanceBoot) {
    maintenanceBoot = false;
    testMode = true;
    if (PHEV_PROTOCOL_WATCH_TEST) Serial.println("TEST PHEV: Wi-Fi STA seul, point d'acces et Zigbee arretes. Boutons ignores durant ce test; RESET: nouvel essai borne.");
    else Serial.println("TEST PHEV: Wi-Fi STA seul, point d'acces et Zigbee arretes. GPIO3: portail; RESET: normal.");
    if (rawTcpMode()) Serial.println("TEST TCP BRUT: socket native non bloquante; WiFiClient/PhevProtocol inactifs; aucun ping, ACK Mitsubishi ou commande. Limite locale 60 s.");
    connectCarWifi();
    return;
  }
  wifiOnlyBoot = false; // configuration manquante: ouvrir le portail habituel
  setupWeb();
  if (requestedBootMode == BootMode::HomeWifi && maintenance.homeConfigured()) {
    maintenance.setMode(BootMode::HomeWifi);
    homeStartedMs = portalOpenedMs = millis();
    WiFi.mode(WIFI_STA); // MAC native en maintenance, identite cliente reservee a REMOTE
    WiFi.begin(maintenance.homeSsid().c_str(), maintenance.homePassword().c_str());
    Serial.printf("Maintenance Wi-Fi maison: connexion a %s; Zigbee/PHEV arretes, repli AP apres 30 s\n", maintenance.homeSsid().c_str());
    return;
  }
  if (maintenanceBoot || !configured) {
    if (requestedBootMode == BootMode::HomeWifi) maintenance.setError("Wi-Fi maison non configure : renseigner la cle dans ce portail");
    startPortal();
    return; // ni pile Zigbee ni client PHEV en maintenance
  }
  Zigbee.setRxOnWhenIdle(true);
  Zigbee.setTimeout(5000);
  // 27 clusters d'entree : la table APS par defaut n'offre que 16 liaisons.
  // Avant Zigbee.begin/esp_zb_init ; conserver toutes les liaisons existantes.
  const esp_err_t bindingCapacity = esp_zb_aps_src_binding_table_size_set(64);
  const esp_err_t bindingDestCapacity = esp_zb_aps_dst_binding_table_size_set(64);
  Serial.printf("Zigbee: tables 64 liaisons source=%s destination=%s\n",
                esp_err_to_name(bindingCapacity), esp_err_to_name(bindingDestCapacity));
  setupZigbeeEndpoints();
  {
    normalStartedMs = millis();
    WiFi.mode(WIFI_OFF);
    startOrResumeZigbee(); // toujours disponible pour les demandes; aucune connexion PHEV ici
    demand.begin(uptimeMs());
    Serial.println("PHEV: a la demande; premiere lecture apres 15 s, puis 24 h; session 60 s maximum, Wi-Fi coupe au repos");
  }
}

void loop() {
  if (PHEV_PROTOCOL_WATCH_TEST) {
    if (watchTestStopped) { delay(20); return; }
    if (elapsed(millis(), watchTestStartedMs, WATCH_TEST_MS)) {
      phev.disconnect("fin locale du test lecture 90 s");
      rawTcp.disconnect("fin locale du test lecture 90 s");
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false); // conserver SSID, mot de passe et NVS.
      WiFi.mode(WIFI_OFF);
      watchTestStopped = true;
      Serial.println("WATCH TEST TERMINE: limite locale 90 s; TCP et Wi-Fi arretes, aucune relance avant RESET.");
      return;
    }
  }
  maintenance.tick();
  if (!wifiOnlyBoot && maintenance.active()) web.handleClient();
  if (!PHEV_PROTOCOL_WATCH_TEST && !zigbeePausedForDemand) handleButtons();
  if (maintenance.homeMode()) {
    if (WiFi.status() == WL_CONNECTED && !homeAddressReported) {
      homeAddressReported = true;
      MDNS.begin("phev-c6"); MDNS.addService("http", "tcp", 80);
      Serial.printf("Maintenance Wi-Fi maison: http://%s / http://phev-c6.local ; compte admin, code configure via AP\n", WiFi.localIP().toString().c_str());
    }
    if (WiFi.status() != WL_CONNECTED && elapsed(millis(), homeStartedMs, 30000) && !maintenance.updating()) {
      MDNS.end(); WiFi.disconnect(false, false);
      maintenance.setError("Wi-Fi maison inaccessible : repli sur le point d'acces");
      startPortal();
    }
    if (elapsed(millis(), portalOpenedMs, PORTAL_MS) && !maintenance.updating()) maintenance.queueReboot(BootMode::Normal);
    delay(10); return; // isolation stricte maison/PHEV : jamais de TCP vers la voiture ici
  }
  if (wifiGotIpEvent) {
    wifiGotIpEvent = false;
    beginPhevTcpCompat();
    Serial.printf("[%lu ms] Wi-Fi PHEV: IP=%s passerelle=%s RSSI=%d dBm (traite a %lu ms)\n",
                  (unsigned long)lastWifiGotIpMs.load(std::memory_order_relaxed),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.RSSI(), (unsigned long)millis());
  }
  const uint32_t now = millis();
  if (portalActive && elapsed(now, portalOpenedMs, PORTAL_MS) && !maintenance.updating()) stopPortal();
  if (portalActive && scanPending && elapsed(now, scanRequestedMs, 350)) {
    scanPending = false;
    WiFi.mode(WIFI_AP_STA); // pas de redemarrage du pilote ni du SoftAP
    const int n = WiFi.scanNetworks(true, true, false, 120);
    if (n == WIFI_SCAN_RUNNING) scanInProgress = true;
    else if (n >= 0) cacheScanResults(n);
    else scanError = n;
  }
  if (portalActive && scanInProgress) {
    const int n = WiFi.scanComplete();
    if (n >= 0) { cacheScanResults(n); scanInProgress = false; }
    else if (n != WIFI_SCAN_RUNNING) { scanError = n; scanInProgress = false; WiFi.scanDelete(); }
  }
  if (portalActive && testConnectPending && elapsed(now, testConnectRequestedMs, 350)) {
    testConnectPending = false;
    uint8_t actualMac[6] = {};
    if (!prepareTestPortal()) {
      testMode = false;
    } else if (esp_wifi_get_mac(WIFI_IF_STA, actualMac) != ESP_OK ||
        memcmp(actualMac, clonedMac, sizeof(clonedMac)) != 0) {
      testMode = false;
      testError = "MAC cliente perdue lors du passage AP+STA";
      Serial.println(testError);
    } else {
      testError = "";
      testMode = true;
      connectCarWifi();
    }
  }
  if (!wifiOnlyBoot && !portalActive && pairingWindow && Zigbee.connected()) pairingWindow = false;
  if (!wifiOnlyBoot && !portalActive && Zigbee.connected() && !zigbeeSeen) { zigbeeSeen = true; prefs.putBool("zbseen", true); }
  if (pairingWindow && !portalActive && elapsed(now, pairingOpenedMs, PAIR_MS)) {
    pairingWindow = false;
    if (!Zigbee.connected()) { Zigbee.stop(); zigbeeStarted = false; }
  }
  // Arduino peut relancer une premiere fois meme avec autoReconnect=false.
  // Laisser cette tentative se terminer au lieu de rappeler reconnect()
  // aussitot qu'une session ancienne perd son association.
  const uint32_t disconnectedAt = lastWifiDisconnectMs.load(std::memory_order_relaxed);
  const uint32_t wifiRetryNow = millis(); // apres la lecture de l'horodatage asynchrone
  if ((wifiOnlyBoot || (portalActive && testMode)) && configured && !maintenance.rebootPending() && WiFi.status() != WL_CONNECTED &&
      elapsed(wifiRetryNow, lastWifiAttemptMs, 30000) &&
      elapsed(wifiRetryNow, disconnectedAt, 30000)) connectCarWifi();
  const bool carWifiReady = configured && !maintenance.rebootPending() && (!portalActive || testMode) && WiFi.status() == WL_CONNECTED;
  if (wifiOnlyBoot || (portalActive && testMode)) {
    if (rawTcpMode()) rawTcp.tick(carWifiReady);
    else phev.tick(carWifiReady); // diagnostics manuels seulement, jamais en mode normal
  } else if (!portalActive && !maintenance.active()) {
    handleZigbeeCommands();
    tickDemand();
  }
  static uint16_t lastStatus = 0xffff;
  static int lastBattery = -1;
  static uint32_t lastSessionWord = 0xffffffffU;
  const PhevState state = telemetry.state(uptimeMs());
  const uint16_t status = (phev.state().online ? 1 : 0) | (phev.state().commandAck ? 2 : 0) |
                         (phev.state().commandFailed ? 4 : 0) | (WiFi.status() == WL_CONNECTED ? 8 : 0) |
                         (state.batteryValid ? 16 : 0) | (state.climateValid ? 32 : 0) |
                         (state.doorsValid ? 64 : 0) | (state.lightsValid ? 128 : 0);
  const int batteryNow = state.batteryValid ? state.batteryPercent : -1;
  static uint32_t lastNormalLogMs = 0;
  if (!wifiOnlyBoot && !portalActive && elapsed(now, lastNormalLogMs, 10000)) {
    lastNormalLogMs = now;
    Serial.printf("PHEV ZIGBEE: wifi=%d RSSI=%d Zigbee=%d TCP=%d protocole=%d batterie=%d rx=%lu pings=%lu ACK=%u commande=%d/%d heap=%lu\n",
      (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
        zigbeeStarted && Zigbee.connected(), phev.tcpConnected(), phev.state().online,
      state.batteryValid ? (int)state.batteryPercent : -1,
      (unsigned long)phev.tcpRxFrames(), (unsigned long)phev.tcpTxPings(),
      phev.pingAckCount(), phev.state().commandAck, phev.state().commandFailed, (unsigned long)ESP.getFreeHeap());
  }
  if (!wifiOnlyBoot && !portalActive && !zigbeePausedForDemand && Zigbee.connected() &&
      (elapsed(now, lastReportMs, 30000) ||
        ((status != lastStatus || batteryNow != lastBattery || sessionResult.word() != lastSessionWord) && elapsed(now, lastReportMs, 500)))) {
    reportState(); lastReportMs = millis(); lastStatus = status; lastBattery = batteryNow;
    lastSessionWord = sessionResult.word();
  }
  if (wifiOnlyBoot && elapsed(millis(), lastReportMs, 10000)) {
    lastReportMs = millis();
    if (rawTcpMode()) {
      Serial.printf("[%lu ms] TEST TCP BRUT: Wi-Fi=%d RSSI=%d canal=%d etat=%s fd=%d tentatives=%lu sessions=%lu rx=%lu EOF=%lu dernier_errno=%d dernier_resultat=%s heap=%lu\n",
                    (unsigned long)millis(), (int)WiFi.status(),
                    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, WiFi.channel(),
                    rawTcp.phase(), rawTcp.fd(), (unsigned long)rawTcp.attempts(),
                    (unsigned long)rawTcp.sessions(), (unsigned long)rawTcp.rxBytes(),
                    (unsigned long)rawTcp.eofCount(), rawTcp.lastError(), rawTcp.lastCause(),
                    (unsigned long)ESP.getFreeHeap());
    } else {
    Serial.printf("[%lu ms] TEST PHEV: Wi-Fi=%d RSSI=%d canal=%d TCP=%d protocole=%d pings=%lu reponses=%u trames=%lu heap=%lu\n",
                  (unsigned long)millis(),
                  (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                  WiFi.channel(), phev.tcpConnected(), phev.state().online,
                  (unsigned long)phev.tcpTxPings(), phev.pingAckCount(),
                  (unsigned long)phev.tcpRxFrames(), (unsigned long)ESP.getFreeHeap());
    }
  }
  updateLed();
  delay(10);
}
