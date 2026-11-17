// HLK-LD2410 / LD2410B / LD2410C protocol — parser + config commands.
// Source: HLK "LD2410 Serial Communication Protocol" V1.02 / V1.07.
//   Report frame:  F4 F3 F2 F1 | len LE u16 | type | 0xAA | state | movDist u16
//                  | movEnergy u8 | statDist u16 | statEnergy u8 | detDist u16
//                  | [engineering: maxMovGate u8, maxStatGate u8,
//                     9 x (movEnergy u8, statEnergy u8), 0x55, 0x00]
//                  | F8 F7 F6 F5
//   type 0x02 = basic report (11 data bytes), 0x01 = engineering (33 data bytes)
//   state bit0 = moving target, bit1 = stationary target (0 none, 1 mov, 2 stat, 3 both)
//   distances in cm, energies 0..100, gates are 0.75 m each (0..8).
//   Config frame:  FD FC FB FA | len LE u16 | cmd LE u16 | value | 04 03 02 01
//   Enable config  cmd 0x00FF value {0x01,0x00}; end config cmd 0x00FE, no value.
//   Set max gates  cmd 0x0060 value = 3 x (word u16, value u32):
//     word 0x0000 max moving gate, 0x0001 max static gate, 0x0002 no-target seconds
//   Set sensitivity cmd 0x0064 value = 3 x (word u16, value u32):
//     word 0x0000 gate (0xFFFF = all gates), 0x0001 motion sens, 0x0002 static sens
#pragma once
#include <Arduino.h>
#include "config.h"

struct RadarReading {
  bool valid = false;
  bool moving = false;
  bool stationary = false;
  uint16_t movingDistCm = 0;
  uint8_t movingEnergy = 0;
  uint16_t staticDistCm = 0;
  uint8_t staticEnergy = 0;
  uint16_t detectionDistCm = 0;
  // Engineering-mode extras (0 when not in engineering mode).
  uint8_t maxMovingGate = 0;
  uint8_t maxStaticGate = 0;
  uint8_t gateMovingEnergy[9] = {0};
  uint8_t gateStaticEnergy[9] = {0};
  uint32_t lastFrameMs = 0;
};

class Ld2410Parser {
 public:
  RadarReading reading;

  // Feed one UART byte. Returns true when a complete report frame was parsed.
  bool feed(uint8_t b) {
    switch (state_) {
      case 0:  // waiting for F4
        if (b == 0xF4) state_ = 1;
        return false;
      case 1:
        state_ = (b == 0xF3) ? 2 : (b == 0xF4 ? 1 : 0);
        return false;
      case 2:
        state_ = (b == 0xF2) ? 3 : 0;
        return false;
      case 3:
        state_ = (b == 0xF1) ? 4 : 0;
        return false;
      case 4:  // length low
        len_ = b; state_ = 5; return false;
      case 5:  // length high
        len_ = len_ | (b << 8);
        if (len_ < 11 || len_ > 64) { reset(); return false; }
        idx_ = 0; state_ = 6; return false;
      case 6:  // data bytes
        buf_[idx_++] = b;
        if (idx_ >= len_) state_ = 7, foot_ = 0;
        return false;
      case 7:  // footer F8 F7 F6 F5
        static const uint8_t foot[4] = {0xF8, 0xF7, 0xF6, 0xF5};
        if (b == foot[foot_]) {
          foot_++;
          // Parse BEFORE reset: reset() clears len_, which parseFrame needs.
          if (foot_ == 4) { bool ok = parseFrame(); reset(); return ok; }
        } else { reset(); }
        return false;
    }
    return false;
  }

  // ---- Config commands (call between enableConfig() and endConfig()) ----
  static void sendCommand(HardwareSerial& s, uint16_t cmd, const uint8_t* value, uint16_t valueLen) {
    uint8_t hdr[4] = {0xFD, 0xFC, 0xFB, 0xFA};
    s.write(hdr, 4);
    uint16_t len = 2 + valueLen;
    s.write((uint8_t)(len & 0xFF)); s.write((uint8_t)(len >> 8));
    s.write((uint8_t)(cmd & 0xFF)); s.write((uint8_t)(cmd >> 8));
    if (valueLen) s.write(value, valueLen);
    uint8_t ftr[4] = {0x04, 0x03, 0x02, 0x01};
    s.write(ftr, 4);
  }
  static void enableConfig(HardwareSerial& s) {
    const uint8_t v[2] = {0x01, 0x00};
    sendCommand(s, 0x00FF, v, 2);
  }
  static void endConfig(HardwareSerial& s) { sendCommand(s, 0x00FE, nullptr, 0); }

  // value records are (word u16 LE, value u32 LE) triples.
  static void setMaxGates(HardwareSerial& s, uint8_t movingGate, uint8_t staticGate, uint16_t noTargetSecs) {
    uint8_t v[18];
    putRecord(v + 0, 0x0000, movingGate);
    putRecord(v + 6, 0x0001, staticGate);
    putRecord(v + 12, 0x0002, noTargetSecs);
    sendCommand(s, 0x0060, v, 18);
  }
  static void setSensitivityAll(HardwareSerial& s, uint8_t motion, uint8_t stationary) {
    uint8_t v[18];
    putRecord(v + 0, 0x0000, 0xFFFF);  // gate 0xFFFF = all gates
    putRecord(v + 6, 0x0001, motion);
    putRecord(v + 12, 0x0002, stationary);
    sendCommand(s, 0x0064, v, 18);
  }

 private:
  uint8_t state_ = 0, foot_ = 0;
  uint16_t len_ = 0, idx_ = 0;
  uint8_t buf_[64] = {0};

  void reset() { state_ = 0; foot_ = 0; len_ = 0; idx_ = 0; }
  static void putRecord(uint8_t* p, uint16_t word, uint32_t value) {
    p[0] = word & 0xFF; p[1] = word >> 8;
    p[2] = value & 0xFF; p[3] = (value >> 8) & 0xFF;
    p[4] = (value >> 16) & 0xFF; p[5] = (value >> 24) & 0xFF;
  }
  static uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }

  bool parseFrame() {
    if (len_ < 11) return false;
    uint8_t type = buf_[0];
    if (type != 0x01 && type != 0x02) return false;
    if (buf_[1] != 0xAA) return false;
    RadarReading r;
    r.valid = true;
    uint8_t st = buf_[2];
    r.moving = (st & 0x01) != 0;
    r.stationary = (st & 0x02) != 0;
    r.movingDistCm = u16(buf_ + 3);
    r.movingEnergy = buf_[5];
    r.staticDistCm = u16(buf_ + 6);
    r.staticEnergy = buf_[8];
    r.detectionDistCm = u16(buf_ + 9);
    if (type == 0x01 && len_ >= 33) {
      r.maxMovingGate = buf_[11];
      r.maxStaticGate = buf_[12];
      for (int g = 0; g < 9; g++) {
        r.gateMovingEnergy[g] = buf_[13 + g * 2];
        r.gateStaticEnergy[g] = buf_[14 + g * 2];
      }
    }
    r.lastFrameMs = millis();
    reading = r;
    return true;
  }
};
