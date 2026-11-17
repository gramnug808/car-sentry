// Minimal Arduino.h stub for HOST unit tests only (test/Arduino.h).
// Lets ld2410.h / ld2450.h compile under g++ on this VM. NOT firmware code.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdio>

using uint8_t = std::uint8_t;
using uint16_t = std::uint16_t;
using uint32_t = std::uint32_t;
using int16_t = std::int16_t;

// Mock clock, driven by the tests.
extern uint32_t mockMillis;
inline uint32_t millis() { return mockMillis; }
inline void delay(uint32_t) {}
inline int constrain(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define SERIAL_8N1 0
#define HIGH 1
#define LOW 0

class HardwareSerial {
 public:
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t* buf, size_t n) { (void)buf; return n; }
  int available() { return 0; }
  int read() { return -1; }
};
