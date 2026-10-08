#include "PhevProtocol.h"
#include <WiFi.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

FakeNetwork network;
FakeSerial Serial;
FakeEsp ESP;
FakeWifi WiFi;
uint32_t millis() { return network.now; }
using Bytes = std::vector<uint8_t>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while(false)
Bytes frame(uint8_t type, uint8_t ack, uint8_t reg, Bytes data = {}, uint8_t key = 0) {
  Bytes b = {type, static_cast<uint8_t>(data.size()+3), ack, reg};
  b.insert(b.end(), data.begin(), data.end());
  uint8_t sum = 0; for (auto v : b) sum += v; b.push_back(sum);
  for (auto &v : b) v ^= key;
  return b;
}
const Bytes initA5 = {0x5e,0x0b,0,1,8,0,8,0,0,8,0,8,0x8a};
const Bytes ping0A = {0xf3,4,0,0x0a,0,1};
struct Session {
  PhevProtocol p;
  explicit Session(bool watch = false, uint32_t at = 10000) {
    network = FakeNetwork{}; p.setWatchOnly(watch); tick(at);
    CHECK(p.tcpConnected()); CHECK(network.writes.size() == 1);
    CHECK(network.writes[0] == ping0A);
  }
  void tick(uint32_t t) { network.now=t; p.tick(true); }
  void rx(const Bytes &b) { network.inject(b); p.tick(true); }
  void init() { rx(initA5); CHECK(p.state().online); }
};
int main() {
  const std::vector<std::pair<const char*,std::function<void()>>> tests = {
    {"demand session starts immediately and cannot reconnect until a NEW request", [] {
      network = FakeNetwork{}; network.now=500;
      PhevProtocol p; p.beginDemandSession(); p.tick(true);
      CHECK(network.connectAttempts==1 && p.tcpConnected());
      network.connected=false; network.now=600; p.tick(true);
      CHECK(p.demandTransportEnded()); network.now=60500; p.tick(true);
      CHECK(network.connectAttempts==1);
      p.beginDemandSession(); p.tick(true); CHECK(network.connectAttempts==2);
      CHECK(!p.commandPending() && !p.state().commandAck && !p.state().commandFailed);
    }},
    {"demand partial climate write never replays in a later refresh session", [] {
      network = FakeNetwork{}; network.now=500;
      PhevProtocol p; p.beginDemandSession(); p.tick(true); network.inject(initA5); p.tick(true);
      network.nextWriteLimit=3; CHECK(p.requestClimate(2,10));
      p.disconnect("test local fermeture"); CHECK(p.demandTransportEnded());
      network.now=15000; p.tick(true); CHECK(network.connectAttempts==1);
      p.beginDemandSession(); p.tick(true); CHECK(network.connectAttempts==2);
      CHECK(network.writes.back()==ping0A); CHECK(!p.commandPending());
    }},
    {"new telemetry remains invalid until a full register arrives", [] {
      Session s; s.init(); s.rx(frame(0x6f,0,0x02,{0,0},0xa5));
      CHECK(!s.p.state().batteryWarningValid);
      Session t; t.init(); t.rx(frame(0x6f,0,0x23,{0,0},0xa5)); CHECK(!t.p.state().lightsValid);
    }},
    {"battery timestamp and new telemetry invalidate on disconnect", [] {
      Session s; s.init(); s.rx(frame(0x6f,0,0x1d,{94,0,1,0},0xa5));
      CHECK(s.p.state().batteryValid); CHECK(s.p.state().batteryUpdatedMs==network.now);
      s.p.tick(false); CHECK(!s.p.state().batteryValid);
      Session t; t.init(); t.rx(frame(0x6f,0,0x02,{0,0,7,0},0xa5));
      CHECK(t.p.state().batteryWarningValid); CHECK(t.p.state().batteryWarning==7);
      t.p.tick(false); CHECK(!t.p.state().batteryWarningValid);
    }},
    {"registered devices, AC and lights decode actual registers only", [] {
      Session s; s.init(); Bytes registration(20,0); registration[19]=2;
      s.rx(frame(0x6f,0,0x15,registration,0xa5));
      CHECK(s.p.state().registrationsValid); CHECK(s.p.state().registrations==2);
      s.p.tick(false); CHECK(!s.p.state().registrationsValid);
      Session t; t.init(); t.rx(frame(0x6f,0,0x1a,{0,1},0xa5));
      CHECK(t.p.state().acOperatingValid); CHECK(t.p.state().acOperating);
      t.p.tick(false); CHECK(!t.p.state().acOperatingValid);
      Session u; u.init(); u.rx(frame(0x6f,0,0x23,{0,0,0,1,1},0xa5));
      CHECK(u.p.state().lightsValid); CHECK(u.p.state().hazards); CHECK(u.p.state().interiorLights);
      u.p.tick(false); CHECK(!u.p.state().lightsValid);
    }},
    {"immediate ping normal and read-only", [] { Session a; CHECK(a.p.tcpTxPings()==1); Session b(true); CHECK(b.p.tcpTxPings()==1); }},
    {"500ms silence / 200ms polling", [] {
      Session s; s.tick(10200); s.tick(10400); CHECK(network.writes.size()==1);
      s.tick(10600); CHECK(network.writes.back()==frame(0xf3,0,11,{0}));
      s.tick(10800); CHECK(network.writes.size()==3);
      CHECK(network.writes.back()==frame(0xf3,0,12,{0}));
    }},
    {"ping ACK is not protocol initialization", [] {
      Session s; s.rx({0x3f,4,1,10,0,0x4e});
      CHECK(s.p.pingAcknowledged()); CHECK(!s.p.state().online);
      CHECK(!s.p.requestClimate(2,10)); CHECK(s.p.state().commandFailed);
      s.tick(30000); CHECK(!s.p.tcpConnected());
    }},
    {"2F fixed 0A does not consume periodic sequence", [] {
      Session s; s.rx(frame(0x2f,0,1,{0})); CHECK(network.writes.back()==ping0A);
      s.tick(10600); CHECK(network.writes.back()==frame(0xf3,0,11,{0}));
    }},
    {"fragmented init and coalesced rolling XOR telemetry", [] {
      Session s; for(size_t i=0;i+1<initA5.size();++i) s.rx({initA5[i]});
      CHECK(!s.p.state().online); s.rx({initA5.back()}); CHECK(s.p.state().online);
      auto b=frame(0x6f,0,0x1d,{94,0,1,0},0xa5);
      auto c=frame(0x6f,0,0x10,{2},0x19); b.insert(b.end(),c.begin(),c.end()); s.rx(b);
      CHECK(s.p.state().batteryPercent==94); CHECK(s.p.state().climateOn);
      CHECK(network.writes.back()==frame(0xf6,1,0x10,{0},0x19));
    }},
    {"climate request ACK never fabricates actual climate state", [] {
      Session s; s.init(); CHECK(!s.p.state().climateValid);
      CHECK(s.p.requestClimate(2,20)); CHECK(network.writes.back()==frame(0xf6,0,0x1b,{2,2,1,0},0xa5));
      s.rx(frame(0x6f,1,0x1b,{0},0xa5)); CHECK(s.p.state().commandAck);
      CHECK(!s.p.state().climateValid); CHECK(!s.p.commandPending());
    }},
    {"stop / cool / heat / windscreen exact payloads", [] {
      for(uint8_t mode=0;mode<=3;++mode) for(uint8_t mins: {10,20,30}) {
        Session s; s.init(); CHECK(s.p.requestClimate(mode,mins));
        CHECK(network.writes.back()==frame(0xf6,0,0x1b,{uint8_t(mode?2:1),mode,uint8_t(mode?mins/10-1:0),0},0xa5));
      }
    }},
    {"invalid commands cannot transmit", [] {
      Session s; s.init(); auto n=network.writes.size();
      CHECK(!s.p.requestClimate(4,10)); CHECK(!s.p.requestClimate(2,5)); CHECK(network.writes.size()==n);
    }},
    {"read-only gates refresh and climate", [] {
      Session s(true); s.init(); auto n=network.writes.size();
      CHECK(!s.p.requestClimate(2,10)); CHECK(!s.p.requestRefresh()); CHECK(network.writes.size()==n);
    }},
    {"explicit read-only refresh exception cannot send climate", [] {
      Session s(true); s.p.setReadRequests(true); s.init(); auto n=network.writes.size();
      CHECK(!s.p.requestClimate(2,10)); CHECK(network.writes.size()==n);
      CHECK(s.p.requestRefresh()); CHECK(network.writes.back()==frame(0xf6,0,0x06,{3},0xa5));
      CHECK(!s.p.state().batteryValid && !s.p.state().climateOn);
    }},
    {"one initial refresh after handshake without reconnect retry", [] {
      Session s(true); s.p.setReadRequests(true); s.init(); s.tick(10999);
      const auto expected=frame(0xf6,0,0x06,{3},0xa5);
      CHECK(std::count(network.writes.begin(),network.writes.end(),expected)==0);
      s.tick(11000); CHECK(std::count(network.writes.begin(),network.writes.end(),expected)==1);
      s.tick(21001); s.tick(21500);
      CHECK(std::count(network.writes.begin(),network.writes.end(),expected)==1);
    }},
    {"pending command expires after 10s", [] {
      Session s; s.init(); CHECK(s.p.requestClimate(1,10)); s.tick(20000);
      CHECK(s.p.state().commandFailed); CHECK(!s.p.commandPending());
    }},
    {"partial write continues exact suffix in same session", [] {
      Session s; s.init(); const auto expected=frame(0xf6,0,0x1b,{2,2,0,0},0xa5);
      network.nextWriteLimit=3; CHECK(s.p.requestClimate(2,10)); CHECK(s.p.txQueued()==1);
      CHECK(network.writes.back()==Bytes(expected.begin(),expected.begin()+3));
      s.tick(10010); CHECK(s.p.txQueued()==0); CHECK(s.p.tcpConnected());
      CHECK(network.writes.back()==Bytes(expected.begin()+3,expected.end()));
    }},
    {"partial init ACK is retained and completed in order", [] {
      Session s; const auto n=network.writes.size(); network.nextWriteLimit=3; s.rx(initA5);
      CHECK(s.p.state().online); CHECK(s.p.tcpConnected()); CHECK(s.p.txQueued()==0);
      const auto expected=frame(0xe5,1,1,{0});
      CHECK(network.writes[n]==Bytes(expected.begin(),expected.begin()+3));
      CHECK(network.writes[n+1]==Bytes(expected.begin()+3,expected.end()));
    }},
    {"EWOULDBLOCK preserves ACK order and XOR sequence", [] {
      Session s(true); s.init(); network.writeError=EWOULDBLOCK;
      s.rx(frame(0x6f,0,0x1d,{94,0,1,0},0xa5));
      s.rx(frame(0x6f,0,0x10,{1},0x19));
      CHECK(s.p.txQueued()==2 && s.p.tcpConnected());
      auto n=network.writes.size(); network.writeError=0; s.tick(10020);
      CHECK(s.p.txQueued()==0 && network.writes.size()==n+2);
      CHECK(network.writes[n]==frame(0xf6,1,0x1d,{0},0xa5));
      CHECK(network.writes[n+1]==frame(0xf6,1,0x10,{0},0x19));
    }},
    {"blocked transmit expires and cannot replay command", [] {
      Session s; s.init(); network.writeError=EWOULDBLOCK;
      CHECK(s.p.requestClimate(2,10)); s.tick(11499); CHECK(s.p.tcpConnected());
      s.tick(11500); CHECK(!s.p.tcpConnected() && s.p.state().commandFailed);
      CHECK(s.p.txQueued()==0); network.writeError=0; s.tick(20000);
      CHECK(network.writes.back()==ping0A && !s.p.commandPending());
    }},
    {"fatal transmit error does not reconnect/replay in demand session", [] {
      network=FakeNetwork{}; network.now=500; PhevProtocol p; p.beginDemandSession(); p.tick(true);
      network.inject(initA5); p.tick(true); network.writeError=EPIPE;
      CHECK(!p.requestClimate(2,10)); CHECK(p.demandTransportEnded());
      CHECK(p.state().commandFailed && p.txQueued()==0); network.writeError=0;
      network.now=60500; p.tick(true); CHECK(network.connectAttempts==1);
    }},
    {"full TX queue terminates safely without retaining requests", [] {
      Session s(true); s.init(); network.writeError=EWOULDBLOCK;
      for (unsigned i=0;i<49 && s.p.tcpConnected();++i) s.rx(frame(0x6f,0,0x1d,{94,0,0,0}));
      CHECK(!s.p.tcpConnected() && s.p.txQueued()==0);
    }},
    {"reconnect discards fragments and resets sequence", [] {
      Session s; s.rx(Bytes(initA5.begin(),initA5.begin()+7)); network.connected=false;
      s.tick(10050); s.tick(19999); CHECK(network.connectAttempts==1); s.tick(20000);
      CHECK(network.connectAttempts==2); CHECK(s.p.tcpRxBytes()==0); CHECK(s.p.tcpTxPings()==1);
      CHECK(network.writes.back()==ping0A); s.init();
    }},
    {"disconnect invalidates every telemetry group", [] {
      Session s; s.init(); s.rx(frame(0x6f,0,0x1c,{0x12},0xa5));
      CHECK(s.p.state().climateSettingsValid); CHECK(s.p.requestClimate(2,10)); s.p.tick(false);
      CHECK(!s.p.state().online); CHECK(!s.p.state().climateSettingsValid);
      CHECK(s.p.state().climateMode==0); CHECK(s.p.state().commandFailed);
    }},
    {"charge plug cannot invent charging / remaining time", [] {
      Session s; s.init(); s.rx(frame(0x6f,0,0x1e,{0,1},0xa5));
      CHECK(s.p.state().plugValid); CHECK(s.p.state().plugged); CHECK(!s.p.state().chargingValid);
      s.rx(frame(0x6f,0,0x1f,{1,0,0xff},0x19)); CHECK(s.p.state().chargingValid);
      CHECK(!s.p.state().chargeTimeValid);
    }},
    {"30s valid-message silence timeout", [] { Session s; s.init(); s.tick(39999); CHECK(s.p.state().online); s.tick(40000); CHECK(!s.p.state().online); }},
    {"receive traffic postpones ping", [] { Session s; s.init(); s.tick(10400); s.rx(frame(0x3f,1,10,{0},0xa5)); s.tick(10800); CHECK(s.p.tcpTxPings()==1); s.tick(11000); CHECK(s.p.tcpTxPings()==2); }},
    {"millis wrap and advancing read clock", [] { Session s(false,0xfffff000U); network.readTimeMs=1; s.init(); CHECK(s.p.state().online); s.tick(0xfffff000U+30014U); CHECK(!s.p.state().online); }},
    {"short clear initialization replaces old XOR key", [] { Session s; s.init(); s.rx(frame(0x6e,0,1,{0})); CHECK(s.p.requestClimate(2,10)); CHECK(network.writes.back()==frame(0xf6,0,0x1b,{2,2,0,0})); }},
    {"expected XOR wins over false init collision", [] {
      const uint8_t keys[]={0,0x23,3,0x2b,9,0xb,0x41,0xe5,0x20,0x24,0x86,0x62,0x3b,0x22,0xad,0x99,0xd6,0xc8,0x40,0xa1,0xbf,0x3e,6,0x19,0x38,0xea,0x31,0xf6,0x45,0x85,0xcb,0xc2,0xa,0x2a,0xe4,0xc6,0xc3,0xf5,0xec,0x5b,0xce,0x17,0xeb,0x1b,0x8a,0x12,0x8f,0xfa,0xf4,0x4c,0x7b,0xd9,0x84,0xf9,0x48,0x7f,0x5e,0x97,0x21,0x3c,0xf8,0x55,0xb1,0xd2,0x8e,0x53,0x6e,0x8c,0x29,0x87,0xc4};
      Session s; s.rx(frame(0x5e,0,1,Bytes(8,0)));
      for(size_t i=0;i<71;++i) s.rx(frame(0x6f,0,0x10,{1},keys[i]));
      CHECK(s.p.requestClimate(2,10)); const Bytes b={0x81,0xea,0xef,0xf5,0xee,0x61,0xf3,0x9b,0x9c,0x81,0xcc,0x9c,0x9c,0x9c,0x7f};
      s.rx(Bytes(b.begin(),b.begin()+5)); CHECK(s.p.commandPending()); s.rx(Bytes(b.begin()+5,b.end()));
      CHECK(s.p.state().commandAck); CHECK(s.p.state().batteryPercent==80); CHECK(s.p.tcpRxFrames()==74);
    }},
  };
  unsigned failed=0; for(auto &t:tests) { try { t.second(); std::cout<<"PASS "<<t.first<<'\n'; } catch(const std::exception &e) { ++failed; std::cerr<<"FAIL "<<t.first<<": "<<e.what()<<'\n'; } }
  std::cout<<tests.size()-failed<<'/'<<tests.size()<<" passed\n"; return failed?1:0;
}
