#pragma once
#include "WiFiClient.h"
constexpr int WL_CONNECTED = 3;
struct FakeWifi {
  int status() const { return network.connected ? WL_CONNECTED : 6; }
  int RSSI() const { return -60; }
};
extern FakeWifi WiFi;
