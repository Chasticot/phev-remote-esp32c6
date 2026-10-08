#include "PhevProtocol.h"

#include <WiFi.h>
#include <cerrno>
#ifdef ARDUINO_ARCH_ESP32
#include <lwip/sockets.h>
#endif

namespace {
constexpr uint32_t kReconnectMs = 10000;
// Cadence du client Go, validee avec la MAC Realtek sur ce PHEV 2020.
constexpr uint32_t kPingPollMs = 200;
constexpr uint32_t kPingSilenceMs = 500;
constexpr uint32_t kNoRxTimeoutMs = 30000;
constexpr uint32_t kStartTimeoutMs = 20000;
constexpr uint32_t kRefreshMs = 300000;

bool elapsed(uint32_t now, uint32_t previous, uint32_t interval) {
  return static_cast<uint32_t>(now - previous) >= interval;
}

bool isVehicleFrame(uint8_t type) {
  return type == 0x2e || type == 0x2f || type == 0x3f || type == 0x4e || type == 0x5e ||
         type == 0x6e || type == 0x6f || type == 0xbb || type == 0xcc;
}

bool decodeCandidate(const uint8_t *raw, size_t available, uint8_t xorValue,
                     uint8_t *decoded, uint8_t &total) {
  const uint8_t type = raw[0] ^ xorValue;
  const uint8_t ack = raw[2] ^ xorValue;
  if (!isVehicleFrame(type) || ack > 1) return false;
  if ((type == 0x4e || type == 0x5e || type == 0x6e) && ack != 0) return false;
  const uint16_t candidateLength = static_cast<uint8_t>(raw[1] ^ xorValue) + 2;
  if (candidateLength < 5 || candidateLength > 250) return false;
  total = candidateLength;
  if (available < total) return false;
  uint8_t checksum = 0;
  for (uint8_t i = 0; i < total; ++i) {
    decoded[i] = raw[i] ^ xorValue;
    if (i != total - 1) checksum += decoded[i];
  }
  return checksum == decoded[total - 1];
}
}  // namespace

String PhevProtocol::tcpFirstRxHex() const {
  return "masque (cle de session/VIN)";
}

void PhevProtocol::beginDemandSession() {
  disconnect("nouvelle session a la demande");
  demandManaged_ = true;
  demandAttempted_ = false;
  lastConnectAttemptMs_ = millis() - kReconnectMs;
  state_.commandAck = state_.commandFailed = false;
  state_.lastTelemetryMs = 0;
}

void PhevProtocol::disconnect(const char *reason) {
  // fd() est une lecture locale. Ne pas sonder SO_ERROR ici : cette lecture
  // effacerait une erreur que NetworkClient doit encore traiter/journaliser.
  if (tcpSessionOpen_ || tcp_.fd() >= 0) {
    const uint32_t now = millis();
    Serial.printf("[%lu ms] DIAG TCP: nettoyage local cause=%s fd=%d session=%lu age=%lu ms wifi=%d RSSI=%d rx=%lu trames=%lu pings=%lu protocole=%d heap=%lu\n",
                  (unsigned long)now, reason, tcp_.fd(), (unsigned long)tcpConnectCount_,
                  (unsigned long)(tcpSessionOpen_ ? now - tcpSessionStartedMs_ : 0),
                  (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                  (unsigned long)tcpRxBytes_, (unsigned long)tcpRxFrames_,
                  (unsigned long)tcpTxPings_, started_, (unsigned long)ESP.getFreeHeap());
  }
  if (pending_ && pendingRegister_ == 0x1b) state_.commandFailed = true;
  tcp_.stop();
  tcpSessionOpen_ = false;
  txHead_ = txCount_ = 0; // Never carry bytes or a climate request into a new session.
  initialRefreshSent_ = false;
  rxLength_ = 0;
  keyReady_ = false;
  sendIndex_ = receiveIndex_ = 0;
  started_ = false;
  pending_ = false;
  state_.online = false;
  state_.batteryValid = false;
  state_.batteryWarningValid = state_.acOperatingValid = state_.lightsValid = state_.registrationsValid = false;
  state_.climateValid = false;
  state_.climateSettingsValid = false;
  state_.climateMode = state_.climateDuration = 0;
  state_.chargeValid = false;
  state_.chargingValid = state_.plugValid = state_.chargeTimeValid = false;
  state_.doorsValid = false;
}

void PhevProtocol::updateKey(const uint8_t *packet, uint8_t length) {
  // Une initialisation courte signifie aucune cle, y compris apres une
  // session chiffree. Ne pas reutiliser une ancienne cle dans ce cas.
  keyReady_ = false;
  sendIndex_ = receiveIndex_ = 0;
  if (length < 12) return;
  uint8_t key = (packet[4] & 8) >> 3;
  key |= (packet[5] & 8) >> 2;
  key |= (packet[6] & 8) >> 1;
  key |= packet[7] & 8;
  key |= (packet[8] & 8) << 1;
  key |= (packet[9] & 8) << 2;
  key |= (packet[10] & 8) << 3;
  key |= (packet[11] & 8) << 4;
  for (int i = 0; i < 256; ++i) keyMap_[i] = i;
  uint8_t index = 0;
  for (int i = 0; i < 256; ++i) {
    index = static_cast<uint8_t>(index + keyMap_[i] + key);
    uint8_t temp = keyMap_[i];
    keyMap_[i] = keyMap_[index];
    keyMap_[index] = temp;
  }
  sendIndex_ = receiveIndex_ = 0;
  keyReady_ = true;
}

bool PhevProtocol::sendFrame(uint8_t type, uint8_t ack, uint8_t reg,
                             const uint8_t *data, uint8_t length, int overrideXor) {
  // Independent final gate: no climate, enrollment, refresh or other writes
  // can leave this diagnostic firmware, even through a future caller.
  const bool safeRead = readRequestsAllowed_ && type == 0xf6 && ack == 0 && reg == 0x06 && length == 1 && data && data[0] == 3;
  if (watchOnly_ && !(safeRead || type == 0xf3 ||
      ((type == 0xf6 || type == 0xe5 || type == 0xe4 || type == 0xe6) && ack == 1))) {
    Serial.println("WATCH: emission de commande bloquee");
    return false;
  }
  if (!tcp_.connected() || length > 240) return false;
  if (txCount_ == kTxCapacity) {
    disconnect("file TCP locale pleine");
    return false;
  }
  uint8_t bytes[250];
  const uint8_t total = length + 5;
  bytes[0] = type;
  bytes[1] = length + 3;
  bytes[2] = ack;
  bytes[3] = reg;
  uint8_t checksum = bytes[0] + bytes[1] + bytes[2] + bytes[3];
  for (uint8_t i = 0; i < length; ++i) {
    bytes[4 + i] = data[i];
    checksum += data[i];
  }
  bytes[total - 1] = checksum;

  uint8_t xorValue = 0;
  if (overrideXor >= 0) {
    xorValue = static_cast<uint8_t>(overrideXor);
  } else if (type != 0xe5 && type != 0xe4 && type != 0xe6 && keyReady_) {
    xorValue = keyMap_[sendIndex_];
    if (type == 0xf6) ++sendIndex_;
  }
  for (uint8_t i = 0; i < total; ++i) bytes[i] ^= xorValue;
  TxPacket &packet = txQueue_[(txHead_ + txCount_) % kTxCapacity];
  memcpy(packet.bytes, bytes, total);
  packet.length = total;
  packet.offset = 0;
  packet.queuedAt = millis();
  ++txCount_;
  const bool sent = flushTx();
  if (watchOnly_ && (type != 0xf3 || tcpTxPings_ < 3)) {
    Serial.printf("[%lu ms] WATCH TX: type=%02X ack=%u reg/seq=%02X longueur=%u resultat=%s\n",
                  (unsigned long)millis(), type, ack, reg, total, sent ? "accepte" : "echec");
  }
  return sent;
}

bool PhevProtocol::flushTx() {
  // A transient full socket is not a broken stream. Keep exactly the unsent
  // suffix, in order, with the XOR encoded only once. No blocking wait and no
  // re-encoding/replay after a reconnect. Bound each packet's lifetime to 1.5 s.
  for (uint8_t budget = 0; txCount_ && budget < 8; ++budget) {
    TxPacket &packet = txQueue_[txHead_];
    if (elapsed(millis(), packet.queuedAt, 1500)) {
      disconnect("timeout local file TCP 1500 ms");
      return false;
    }
    errno = 0;
    // tools/NetworkClientBounded.cpp preserves the successful Arduino send
    // path but bounds each write to 25 ms, instead of ten 1-second retries.
    const int result = static_cast<int>(tcp_.write(packet.bytes + packet.offset,
                                                  packet.length - packet.offset));
    const int error = errno;
    if (result <= 0) {
      if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR || (result == 0 && error == 0)) return true;
      Serial.printf("DIAG TX: erreur fatale errno=%d, file=%u\n", error, txCount_);
      disconnect("erreur socket emission");
      return false;
    }
    packet.offset += static_cast<uint8_t>(result);
    if (packet.offset < packet.length) return true;
    txHead_ = (txHead_ + 1) % kTxCapacity;
    --txCount_;
  }
  return true;
}

void PhevProtocol::sendPing(bool requestedByVehicle) {
  const uint8_t zero = 0;
  // La reponse au 2F utilise toujours 0A dans le client de reference;
  // elle ne consomme pas le compteur des pings periodiques.
  const uint8_t sequence = requestedByVehicle ? 0x0a : pingSeq_;
  if (sendFrame(0xf3, 0, sequence, &zero, 1)) {
    ++tcpTxPings_;
    lastPingMs_ = millis();
    if (!requestedByVehicle) pingSeq_ = pingSeq_ >= 0x63 ? 0 : pingSeq_ + 1;
  }
}

void PhevProtocol::sendRegister(uint8_t reg, const uint8_t *data, uint8_t length) {
  pending_ = sendFrame(0xf6, 0, reg, data, length);
  if (pending_) {
    pendingRegister_ = reg;
    pendingLength_ = length;
    memcpy(pendingData_, data, length);
    pendingSinceMs_ = millis();
    retryCount_ = 0;
  }
}

bool PhevProtocol::requestClimate(uint8_t mode, uint8_t durationMinutes) {
  if (watchOnly_) return false;
  if (!state_.online || pending_ || mode > 3 ||
      (durationMinutes != 10 && durationMinutes != 20 && durationMinutes != 30)) {
    state_.commandAck = false;
    state_.commandFailed = true;
    return false; // ne pas conserver une commande pour une reconnexion ulterieure
  }
  const uint8_t duration = mode ? (durationMinutes / 10) - 1 : 0;
  const uint8_t data[4] = {static_cast<uint8_t>(mode ? 2 : 1), mode, duration, 0};
  state_.commandAck = state_.commandFailed = false;
  sendRegister(0x1b, data, sizeof(data));
  if (!pending_) state_.commandFailed = true;
  return pending_;
}

bool PhevProtocol::requestRefresh() {
  if (watchOnly_ && !readRequestsAllowed_) return false;
  if (!state_.online || pending_) return false;
  const uint8_t refresh = 3;
  sendRegister(0x06, &refresh, 1);
  if (pending_) lastUpdateRequestMs_ = millis();
  return pending_;
}

void PhevProtocol::updateRegister(uint8_t reg, const uint8_t *data, uint8_t length) {
  switch (reg) {
    case 0x02:
      if (length == 4) { state_.batteryWarningValid = true; state_.batteryWarning = data[2]; }
      break;
    case 0x15:
      if (length == 20) { state_.registrationsValid = true; state_.registrations = data[19]; }
      break;
    case 0x1a:
      if (length >= 2) { state_.acOperatingValid = true; state_.acOperating = data[1] == 1; }
      break;
    case 0x10:  // preconditioning state
      if (length >= 1) {
        state_.climateValid = true;
        state_.climateOn = data[0] == 2;
        state_.climateTerminated = data[0] == 3;
      }
      break;
    case 0x1c:  // climate mode and duration
      if (length == 1) {
        state_.climateMode = data[0] & 0x0f;
        state_.climateDuration = 10 * (((data[0] >> 4) & 0x0f) + 1);
        state_.climateSettingsValid = state_.climateMode >= 1 && state_.climateMode <= 3 &&
                                      state_.climateDuration <= 30;
      }
      break;
    case 0x1d:  // drive battery and parking lights
      if (length == 4) {
        state_.batteryValid = data[0] <= 100;
        state_.batteryPercent = data[0];
        state_.batteryUpdatedMs = millis();
        state_.parkingLights = data[2] == 1;
      }
      break;
    case 0x1e:  // charge plug
      if (length == 2) {
        state_.chargeValid = true;
        state_.plugValid = true;
        state_.plugged = data[1] == 1 || data[0] > 0;
      }
      break;
    case 0x1f:  // charging and remaining minutes
      if (length == 3) {
        state_.chargeValid = true;
        state_.chargingValid = true;
        state_.chargeTimeValid = data[2] != 0xff;
        state_.charging = data[0] == 1;
        state_.chargeRemaining = data[2] == 0xff ? 0 : (uint16_t(data[2]) << 8) | data[1];
      }
      break;
    case 0x23:
      if (length == 5) {
        state_.lightsValid = true;
        state_.interiorLights = (data[4] & 3) == 1;
        state_.hazards = (data[3] & 3) == 1;
      }
      break;
    case 0x24:  // doors and headlights
      if (length == 10) {
        state_.doorsValid = true;
        state_.doorsLocked = data[0] == 1;
        state_.openMask = 0;
        for (uint8_t i = 0; i < 6; ++i) {
          if (data[3 + i] == 1) state_.openMask |= 1U << i;
        }
        state_.headlights = data[9] == 1;
      }
      break;
  }
}

void PhevProtocol::processFrame(const uint8_t *frame, uint8_t total,
                                 const uint8_t *raw, uint8_t xorValue) {
  (void)raw;
  (void)xorValue;
  state_.lastVehicleMessageMs = millis();
  const uint8_t type = frame[0];
  const uint8_t ack = frame[2];
  const uint8_t reg = frame[3];
  const uint8_t *data = frame + 4;
  const uint8_t dataLength = total - 5;

  if (watchOnly_ && type != 0x3f) {
    Serial.printf("[%lu ms] WATCH RX: type=%02X ack=%u reg=%02X longueur=%u\n",
                  (unsigned long)millis(), type, ack, reg, total);
    if (keyReady_ && type != 0x5e && type != 0x4e && type != 0x6e)
      Serial.printf("WATCH: compteurs TX=%u RX=%u, encodage RX attendu=%d\n", sendIndex_, receiveIndex_, xorValue == keyMap_[receiveIndex_]);
    if (type == 0xbb && dataLength >= 1 && keyReady_) {
      int expectedIndex = -1;
      for (int i=0;i<256;++i) if (keyMap_[i] == data[0]) expectedIndex=i;
      Serial.printf("WATCH: BB position TX attendue=%d, actuelle=%u (cle masquee)\n", expectedIndex, sendIndex_);
    }
    if (type == 0x6f && ack == 0 && reg == 0x15 && dataLength == 20)
      Serial.printf("WATCH: appareils inscrits=%u (VIN masque)\n", data[19]);
    if (type == 0x6f && ack == 0 && reg == 0x1d && dataLength == 4)
      Serial.printf("WATCH: niveau batterie decode=%u (a comparer a l'application)\n", data[0]);
  }

  if (type == 0x5e || type == 0x4e || type == 0x6e) {
    if (watchOnly_) Serial.printf("WATCH: initialisation en clair=%d, longueur=%u\n", xorValue == 0, total);
    updateKey(frame, total);
    const uint8_t zero = 0;
    const uint8_t response = type == 0x5e ? 0xe5 : type == 0x4e ? 0xe4 : 0xe6;
    if (!sendFrame(response, 1, 1, &zero, 1)) {
      Serial.println("PHEV: echec envoi acquittement initialisation");
      disconnect("echec ecriture ACK initialisation");
      return;
    }
    started_ = true;
    state_.online = true;
    lastUpdateRequestMs_ = millis(); // l'initialisation fournit deja les registres
    Serial.printf("PHEV: protocole demarre (type %02X)\n", type);
  } else if (type == 0x3f && ack == 1) {
    if (pingAckCount_ < 255) ++pingAckCount_;
    if (!pingAcknowledged_) {
      pingAcknowledged_ = true;
      Serial.printf("PHEV: ping confirme (sequence %02X), attente initialisation 5E/4E/6E\n", reg);
    }
  } else if (type == 0x2f) {
    Serial.println("PHEV: demande de presence 2F, reponse ping 0A");
    sendPing(true);
  } else if (type == 0x6f) {
    if (keyReady_) ++receiveIndex_;
    if (ack == 0) {
      updateRegister(reg, data, dataLength);
      state_.lastTelemetryMs = millis();
      const uint8_t zero = 0;
      sendFrame(0xf6, 1, reg, &zero, 1);
    } else if (ack == 1 && pending_ && reg == pendingRegister_) {
      pending_ = false;
      if (reg == 0x1b) {
        state_.commandAck = true;
        state_.commandFailed = false;
        Serial.println("PHEV: ACK commande clim recu; verifier le retour climate_on du vehicule");
      }
    }
  } else if (type == 0xbb && pending_ && dataLength >= 1) {
    if (++retryCount_ <= 2) {
      if (!sendFrame(0xf6, 0, pendingRegister_, pendingData_, pendingLength_, data[0])) return;
      pendingSinceMs_ = millis();
    } else {
      pending_ = false;
      if (pendingRegister_ == 0x1b) state_.commandFailed = true;
    }
  }
}

void PhevProtocol::processRx() {
  while (tcp_.available() && rxLength_ < sizeof(rx_)) {
    const int received = tcp_.read();
    if (received < 0) {
      Serial.printf("[%lu ms] DIAG TCP: lecture sans octet valide; voir erreur NetworkClient precedente\n",
                    (unsigned long)millis());
      break; // Ne pas compter -1 comme un faux octet FF.
    }
    const uint8_t byte = static_cast<uint8_t>(received);
    lastTcpRxMs_ = millis();
    rx_[rxLength_++] = byte;
    ++tcpRxBytes_;
    if (tcpFirstRxLength_ < sizeof(tcpFirstRx_))
      tcpFirstRx_[tcpFirstRxLength_++] = byte;
  }
  // Un tampon plein peut contenir plusieurs trames valides. Les decoder
  // avant de lire la suite TCP, en conservant la derniere trame incomplete.

  uint8_t decoded[250];
  while (rxLength_ >= 5) {
    // Un ACK chiffre peut aussi passer le checksum avec le XOR voisin,
    // en empruntant un octet a la trame suivante. La cle attendue est
    // prioritaire; conserver le fragment tant qu'il est incomplet.
    uint8_t expectedTotal = 0;
    uint8_t usedXor = keyReady_ ? keyMap_[receiveIndex_] : 0;
    bool valid = keyReady_ &&
                 decodeCandidate(rx_, rxLength_, usedXor, decoded, expectedTotal);
    // Une nouvelle initialisation arrive en clair meme si une ancienne cle
    // existe. La reconnaitre avant d'attendre une longueur issue de cette cle.
    if (!valid && rx_[2] == 0 &&
        (rx_[0] == 0x4e || rx_[0] == 0x5e || rx_[0] == 0x6e)) {
      uint8_t initTotal = 0;
      if (decodeCandidate(rx_, rxLength_, 0, decoded, initTotal)) {
        valid = true;
        usedXor = 0;
        expectedTotal = initTotal;
      }
    }
    if (!valid && expectedTotal > rxLength_) break;
    const uint8_t xor0 = rx_[2];
    const uint8_t xor1 = xor0 ^ 1;
    uint8_t total0 = 0, total1 = 0;
    bool valid0 = false;
    bool valid1 = false;
    uint8_t total = expectedTotal;
    if (!valid) {
      valid0 = decodeCandidate(rx_, rxLength_, xor0, decoded, total0);
      if (valid0) {
        usedXor = xor0;
        total = total0;
      }
    }
    if (!valid && !valid0) {
      valid1 = decodeCandidate(rx_, rxLength_, xor1, decoded, total1);
      if (valid1) {
        usedXor = xor1;
        total = total1;
      }
    }
    if (!valid && !valid0 && !valid1) {
      if ((total0 >= 5 && total0 <= 250 && rxLength_ < total0) ||
          (total1 >= 5 && total1 <= 250 && rxLength_ < total1)) break;
      memmove(rx_, rx_ + 1, --rxLength_);
      continue;
    }
    ++tcpRxFrames_;
    processFrame(decoded, total, rx_, usedXor);
    // Un echec d'ecriture peut fermer la session et vider le tampon.
    if (!tcpSessionOpen_) return;
    rxLength_ -= total;
    memmove(rx_, rx_ + total, rxLength_);
  }
}

void PhevProtocol::tick(bool wifiConnected) {
  if (!wifiConnected) {
    if (tcpSessionOpen_ || tcp_.connected() || state_.online || rxLength_) {
      Serial.printf("[%lu ms] PHEV: session interrompue, Wi-Fi deconnecte (%lu octets, %lu trames, %lu pings envoyes)\n",
                    (unsigned long)millis(),
                    (unsigned long)tcpRxBytes_, (unsigned long)tcpRxFrames_,
                    (unsigned long)tcpTxPings_);
      disconnect("perte Wi-Fi constatee par loop");
    }
    return;
  }
  // Lire les derniers octets meme si le pair a deja ferme son cote TCP.
  if (tcp_.available()) processRx();
  if (!tcp_.connected()) {
    const uint32_t now = millis();
    if (tcpSessionOpen_) {
      Serial.printf("[%lu ms] PHEV: TCP ferme apres %lu ms, %lu octets recus, %lu trames, %lu pings envoyes, %u confirmes, debut=%s\n",
                    (unsigned long)now,
                    (unsigned long)(now - tcpSessionStartedMs_),
                    (unsigned long)tcpRxBytes_, (unsigned long)tcpRxFrames_,
                    (unsigned long)tcpTxPings_, pingAckCount_, tcpFirstRxHex().c_str());
    }
    // Nettoyer aussi les sessions fermees AVANT l'initialisation : leurs
    // fragments TCP ne doivent pas contaminer la connexion suivante.
    if (tcpSessionOpen_ || state_.online || started_ || rxLength_)
      disconnect("transport perdu; voir erreur NetworkClient precedente");
    if (demandManaged_ && demandAttempted_) return; // one TCP attempt; no replay/retry loop
    if (!elapsed(now, lastConnectAttemptMs_, kReconnectMs)) return;
    demandAttempted_ = true;
    lastConnectAttemptMs_ = now;
    tcp_.setTimeout(250);
    const uint32_t attemptStarted = millis();
    Serial.printf("[%lu ms] DIAG TCP: tentative vers 192.168.8.46:8080 timeout=1500 ms wifi=%d RSSI=%d heap=%lu\n",
                  (unsigned long)attemptStarted, (int)WiFi.status(), WiFi.RSSI(),
                  (unsigned long)ESP.getFreeHeap());
    if (tcp_.connect(IPAddress(192, 168, 8, 46), 8080, 1500)) {
      ++tcpConnectCount_;
      tcpRxBytes_ = 0;
      tcpRxFrames_ = 0;
      tcpTxPings_ = 0;
      tcpFirstRxLength_ = 0;
      pingAcknowledged_ = false;
      pingAckCount_ = 0;
      tcpSessionStartedMs_ = millis();
      tcpSessionOpen_ = true;
      connectedAtMs_ = tcpSessionStartedMs_;
      state_.lastVehicleMessageMs = tcpSessionStartedMs_;
      pingSeq_ = 10;
      lastPingMs_ = tcpSessionStartedMs_;
      // Controlled variant: first ping immediately, before session logs.
      // Subsequent polls retain the Go 200 ms / 500 ms silence cadence.
      lastTcpRxMs_ = tcpSessionStartedMs_;
      lastPingPollMs_ = tcpSessionStartedMs_;
      tcp_.setNoDelay(true);
      sendPing(); // identique en diagnostic et en fonctionnement Zigbee
      if (!tcpSessionOpen_) return;
      Serial.printf("[%lu ms] DIAG TCP: session=%lu connect_duree=%lu ms fd=%d local=%s:%u distant=192.168.8.46:8080\n",
                    (unsigned long)tcpSessionStartedMs_, (unsigned long)tcpConnectCount_,
                    (unsigned long)(tcpSessionStartedMs_ - attemptStarted), tcp_.fd(),
                    tcp_.localIP().toString().c_str(), tcp_.localPort());
      if (watchOnly_) Serial.println("WATCH: TCP connecte, premier ping immediat; pings apres 500 ms de silence RX, poll 200 ms; aucune commande");
      else Serial.println("PHEV: TCP connecte, premier ping immediat; poll 200 ms, silence RX 500 ms; attente initialisation");
    } else {
      Serial.printf("[%lu ms] PHEV: echec TCP vers 192.168.8.46:8080 duree=%lu ms wifi=%d; voir erreur NetworkClient precedente\n",
                    (unsigned long)millis(), (unsigned long)(millis() - attemptStarted), (int)WiFi.status());
    }
    return;
  }
  if (!flushTx()) return;
  processRx();
  if (!tcpSessionOpen_) return; // un ACK incomplet peut avoir ferme la session
  // processRx() actualise les horodatages et peut envoyer un ping en reponse
  // au vehicule. Echantillonner APRES : une heure anterieure produit un
  // debordement non signe, donc un faux timeout ou un ping supplementaire.
  const uint32_t now = millis();
  const uint32_t startTimeout = kStartTimeoutMs;
  if (!started_ && elapsed(now, connectedAtMs_, startTimeout)) {
    Serial.printf("PHEV: initialisation absente apres %lu s (%lu octets, %lu trames, %lu pings, %u confirmes); verifier MAC enregistree et telephone deconnecte\n",
                  (unsigned long)(startTimeout / 1000), (unsigned long)tcpRxBytes_, (unsigned long)tcpRxFrames_,
                  (unsigned long)tcpTxPings_, pingAckCount_);
    disconnect("timeout local initialisation");
    return;
  }
  if (started_ && elapsed(now, state_.lastVehicleMessageMs, kNoRxTimeoutMs)) {
    Serial.println("PHEV: silence du vehicule, reconnexion TCP");
    disconnect("timeout local silence protocole");
    return;
  }
  if (pending_ && elapsed(now, pendingSinceMs_, 10000)) {
    pending_ = false;
    if (pendingRegister_ == 0x1b) state_.commandFailed = true;
  }
  if (readRequestsAllowed_ && started_ && !initialRefreshSent_ && !pending_ &&
      elapsed(now, lastUpdateRequestMs_, 1000)) {
    initialRefreshSent_ = true;
    Serial.println("PHEV: demande unique de lecture complete registre 06, sans commande clim");
    requestRefresh();
  }
  if (elapsed(now, lastPingPollMs_, kPingPollMs)) {
    lastPingPollMs_ = now;
    if (elapsed(now, lastTcpRxMs_, kPingSilenceMs)) sendPing();
  }
  if (!demandManaged_ && !watchOnly_ && started_ && !pending_ && elapsed(now, lastUpdateRequestMs_, kRefreshMs)) {
    const uint8_t refresh = 3;
    sendRegister(0x06, &refresh, 1);
    lastUpdateRequestMs_ = now;
  }
}
