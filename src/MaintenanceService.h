#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include "MaintenancePolicy.h"

class MaintenanceService {
 public:
  MaintenanceService(WebServer &web, Preferences &prefs) : web_(web), prefs_(prefs) {}
  void begin();
  BootMode consumeBootMode();
  void setMode(BootMode mode);
  BootMode mode() const { return mode_; }
  bool active() const { return mode_ != BootMode::Normal; }
  bool homeMode() const { return mode_ == BootMode::HomeWifi; }
  bool updating() const { return uploadActive_; }
  bool rebootPending() const { return rebootQueued_; }
  void setBeforeUpdate(void (*callback)()) { beforeUpdate_ = callback; }
  bool homeConfigured() const { return homeSsid_.length() && homePassword_.length() >= 8; }
  const String &homeSsid() const { return homeSsid_; }
  const String &homePassword() const { return homePassword_; }
  const String &csrf() const { return csrf_; }
  const String &error() const { return error_; }
  void setError(const String &error) { error_ = error; }
  bool queueReboot(BootMode mode);
  void tick();
  void installRoutes();
  void decoratePage(String &page) const;
  String controlsHtml() const;
 private:
  WebServer &web_;
  Preferences &prefs_;
  BootMode mode_ = BootMode::Normal;
  String homeSsid_, homePassword_, otaPassword_, csrf_, error_;
  bool rebootQueued_ = false;
  uint32_t rebootAtMs_ = 0;
  bool uploadActive_ = false, uploadAuthorized_ = false, uploadOk_ = false;
  uint32_t uploadLastMs_ = 0;
  size_t expectedSize_ = 0, receivedSize_ = 0, prefixSize_ = 0;
  uint8_t prefix_[288] = {};
  bool prefixChecked_ = false;
  bool identityFound_ = false;
  uint8_t identityMatched_ = 0;
  void (*beforeUpdate_)() = nullptr;
  bool authenticate();
  bool tokenValid() const;
  void uploadChunk();
  void abortUpload(const char *error);
};
