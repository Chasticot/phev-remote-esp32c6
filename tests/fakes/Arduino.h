#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using String = std::string;
uint32_t millis();

struct FakeSerial {
  template <typename... Args>
  void printf(const char *, Args...) {}
  void println(const char *) {}
};

extern FakeSerial Serial;
struct FakeEsp { uint32_t getFreeHeap() const { return 280000; } };
extern FakeEsp ESP;
