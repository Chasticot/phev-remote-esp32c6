#include "RawTcpProbe.h"

#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <string.h>
#include <unistd.h>

namespace {
constexpr uint32_t RETRY_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 1500;
constexpr uint32_t SESSION_LIMIT_MS = 60000;
constexpr uint32_t HEX_LIMIT = 64;
constexpr uint16_t CAR_PORT = 8080;
const char *errorName(int error) {
  switch (error) {
    case ECONNRESET: return "ECONNRESET";
    case ECONNREFUSED: return "ECONNREFUSED";
    case ECONNABORTED: return "ECONNABORTED";
    case ENOTCONN: return "ENOTCONN";
    case EPIPE: return "EPIPE";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EWOULDBLOCK: return "EWOULDBLOCK";
    case EINPROGRESS: return "EINPROGRESS";
    case EINTR: return "EINTR";
    default: return "autre";
  }
}
bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}
} // namespace

const char *RawTcpProbe::phase() const {
  switch (_phase) {
    case Phase::Connecting: return "connexion";
    case Phase::Connected: return "connecte";
    default: return "repos";
  }
}

void RawTcpProbe::disconnect(const char *cause) {
  if (_fd < 0) return;
  const uint32_t now = millis();
  Serial.printf("[%lu ms] RAW TCP: fermeture locale cause=%s fd=%d etat=%s age=%lu ms rx=%lu\n",
                (unsigned long)now, cause, _fd, phase(),
                (unsigned long)(now - _startedMs), (unsigned long)_rxBytes);
  const int oldFd = _fd;
  _fd = -1;
  _phase = Phase::Idle;
  // Ne pas remplacer la cause originale par le resultat du nettoyage.
  if (::close(oldFd) < 0) {
    const int error = errno;
    Serial.printf("[%lu ms] RAW TCP: close fd=%d errno=%d (%s), nettoyage uniquement\n",
                  (unsigned long)millis(), oldFd, error, errorName(error));
  }
}

void RawTcpProbe::fail(const char *operation, int error) {
  _lastError = error;
  _lastCause = operation;
  Serial.printf("[%lu ms] RAW TCP: echec operation=%s fd=%d errno=%d (%s: %s) age=%lu ms rx=%lu Wi-Fi=%d RSSI=%d\n",
                (unsigned long)millis(), operation, _fd, error, errorName(error), strerror(error),
                (unsigned long)(millis() - _startedMs), (unsigned long)_rxBytes,
                (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  disconnect("consequence erreur native ci-dessus");
}

void RawTcpProbe::markConnected() {
  _phase = Phase::Connected;
  _connectedMs = millis();
  ++_sessions;
  sockaddr_in local = {};
  socklen_t length = sizeof(local);
  if (::getsockname(_fd, reinterpret_cast<sockaddr *>(&local), &length) < 0) {
    const int error = errno;
    Serial.printf("RAW TCP: getsockname errno=%d (%s), diagnostic adresse seulement\n", error, errorName(error));
  }
  Serial.printf("[%lu ms] RAW TCP: connecte session=%lu fd=%d duree=%lu ms local=%s:%u distant=192.168.8.46:8080; aucun envoi applicatif\n",
                (unsigned long)_connectedMs, (unsigned long)_sessions, _fd,
                (unsigned long)(_connectedMs - _startedMs),
                WiFi.localIP().toString().c_str(), (unsigned int)ntohs(local.sin_port));
}

void RawTcpProbe::start() {
  _lastAttemptMs = _startedMs = millis();
  ++_attempts;
  _rxBytes = _shownBytes = 0;
  // Le dernier resultat terminal reste visible jusqu'au prochain resultat.
  Serial.printf("[%lu ms] RAW TCP: tentative=%lu vers 192.168.8.46:8080 timeout=%lu ms RSSI=%d heap=%lu\n",
                (unsigned long)_startedMs, (unsigned long)_attempts,
                (unsigned long)CONNECT_TIMEOUT_MS, WiFi.RSSI(), (unsigned long)ESP.getFreeHeap());
  _fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (_fd < 0) { const int error = errno; fail("socket", error); return; }
  const int flags = ::fcntl(_fd, F_GETFL, 0);
  if (flags < 0) { const int error = errno; fail("fcntl GETFL", error); return; }
  if (::fcntl(_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    const int error = errno; fail("fcntl NONBLOCK", error); return;
  }
  sockaddr_in remote = {};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(CAR_PORT);
  remote.sin_addr.s_addr = inet_addr("192.168.8.46");
  const int result = ::connect(_fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote));
  const int error = result < 0 ? errno : 0; // Sauvegarder avant tout autre appel.
  if (result == 0) { markConnected(); return; }
  if (error != EINPROGRESS) { fail("connect", error); return; }
  _phase = Phase::Connecting;
  Serial.printf("[%lu ms] RAW TCP: connect en cours fd=%d errno=%d (%s)\n",
                (unsigned long)millis(), _fd, error, errorName(error));
}

void RawTcpProbe::tick(bool wifiReady) {
  if (!wifiReady) {
    if (_fd >= 0) {
      _lastCause = "Wi-Fi indisponible";
      _lastError = 0;
      disconnect(_lastCause);
    }
    return;
  }
  if (_phase == Phase::Idle) {
    if (elapsed(millis(), _lastAttemptMs, RETRY_MS)) start();
    if (_phase != Phase::Connected) return;
  }
  if (_phase == Phase::Connecting) {
    fd_set writable, exceptional;
    FD_ZERO(&writable); FD_ZERO(&exceptional);
    FD_SET(_fd, &writable); FD_SET(_fd, &exceptional);
    timeval timeout = {}; // Aucun blocage dans loop().
    const int result = ::select(_fd + 1, nullptr, &writable, &exceptional, &timeout);
    const int error = result < 0 ? errno : 0;
    if (result < 0) {
      if (error != EINTR) fail("select connexion", error);
      return;
    }
    if (result > 0) {
      int socketError = 0;
      socklen_t length = sizeof(socketError);
      // SO_ERROR uniquement pour terminer connect(), jamais sur la session RX.
      if (::getsockopt(_fd, SOL_SOCKET, SO_ERROR, &socketError, &length) < 0) {
        const int nativeError = errno; fail("SO_ERROR connexion", nativeError); return;
      }
      if (socketError != 0) { fail("connect SO_ERROR", socketError); return; }
      markConnected();
    } else {
      if (elapsed(millis(), _startedMs, CONNECT_TIMEOUT_MS)) {
        _lastCause = "timeout local connexion";
        _lastError = 0;
        disconnect(_lastCause); // Ne pas inventer une erreur ETIMEDOUT native.
      }
      return;
    }
  }
  // recv() est le seul observateur de l'etat de reception de cette socket.
  // Pas de MSG_PEEK, NetworkClient::connected() ni SO_ERROR qui consomme l'erreur.
  for (unsigned int readIndex = 0; readIndex < 4; ++readIndex) {
    uint8_t bytes[256];
    const int result = ::recv(_fd, bytes, sizeof(bytes), 0);
    const int error = result < 0 ? errno : 0;
    if (result > 0) {
      _rxBytes += static_cast<uint32_t>(result);
      const uint32_t count = min(static_cast<uint32_t>(result), HEX_LIMIT - _shownBytes);
      char hex[HEX_LIMIT * 2 + 1];
      for (uint32_t i = 0; i < count; ++i) snprintf(hex + i * 2, 3, "%02X", bytes[i]);
      hex[count * 2] = '\0';
      // Volume de journaux borne : les 64 premiers octets de chaque session.
      if (count) Serial.printf("[%lu ms] RAW TCP: recv=%d age=%lu ms total=%lu premiers_octets=%s\n",
                              (unsigned long)millis(), result,
                              (unsigned long)(millis() - _connectedMs), (unsigned long)_rxBytes, hex);
      _shownBytes += count;
      continue;
    }
    if (result == 0) {
      ++_eofCount;
      _lastCause = "recv=0 EOF";
      _lastError = 0;
      Serial.printf("[%lu ms] RAW TCP: recv=0 EOF fd=%d session=%lu age=%lu ms rx=%lu; fin de flux indiquee par la pile TCP\n",
                    (unsigned long)millis(), _fd, (unsigned long)_sessions,
                    (unsigned long)(millis() - _connectedMs), (unsigned long)_rxBytes);
      disconnect("consequence EOF ci-dessus");
      return;
    }
    if (error == EWOULDBLOCK || error == EAGAIN || error == EINTR) break;
    fail("recv", error);
    return;
  }
  if (elapsed(millis(), _connectedMs, SESSION_LIMIT_MS)) {
    _lastCause = "limite locale session 60 s";
    _lastError = 0;
    disconnect(_lastCause);
  }
}
