#pragma once
#include "raw.h"
#include <utility>
namespace CompatFake {
inline std::vector<std::pair<void (*)(void *), void *>> work;
inline bool schedulingFails = false;
inline void drain() {
  while (!work.empty()) {
    auto item = work.front(); work.erase(work.begin()); item.first(item.second);
  }
}
}
inline err_t tcpip_callback(void (*fn)(void *), void *arg) {
  if (CompatFake::schedulingFails) return -1;
  CompatFake::work.emplace_back(fn, arg); return ERR_OK;
}
