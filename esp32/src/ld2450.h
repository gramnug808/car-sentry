// HLK-LD2450 multi-target radar protocol — parser only.
// Frame: AA FF 03 00 | 3 x (x i16 mm, y i16 mm, speed i16 cm/s, resolution u16)
//        | 55 CC   (30 bytes total)
// x / y / speed are 15-bit magnitudes with the top bit SET for a positive
// value (bit15 clear = negative). A target at (0,0) is "no target" in that slot.
// The LD2450 reports no per-target energy, so energy here is synthetic:
// nearest target gets energy scaled by 1/distance (100 at <0.5 m, fading to
// ~10 at 6 m), 0 when no target. It is labelled synthetic in /api/status.
// This file is only compiled when -DSENSOR_LD2450 is set (see platformio.ini).
#pragma once
#include <Arduino.h>
#include "config.h"

struct Ld2450Target {
  int16_t xMm = 0;      // +x = right of sensor, mm
  int16_t yMm = 0;      // +y = straight ahead, mm
  int16_t speedCmS = 0; // + = approaching, cm/s
  uint16_t resolutionMm = 0;
  bool present = false;
  uint16_t distCm() const {
    return (uint16_t)(sqrt((double)xMm * xMm + (double)yMm * yMm) / 10.0 + 0.5);
  }
};

struct Ld2450Reading {
  bool valid = false;
  Ld2450Target targets[3];
  uint32_t lastFrameMs = 0;
  int presentCount() const {
    int n = 0; for (int i = 0; i < 3; i++) if (targets[i].present) n++; return n;
  }
  bool anyMoving() const {
    for (int i = 0; i < 3; i++) if (targets[i].present && targets[i].speedCmS != 0) return true;
    return false;
  }
  // Nearest target of any kind; nullptr when none present.
  const Ld2450Target* nearest() const {
    const Ld2450Target* best = nullptr;
    for (int i = 0; i < 3; i++) {
      if (!targets[i].present) continue;
      if (!best || targets[i].distCm() < best->distCm()) best = &targets[i];
    }
    return best;
  }
};

class Ld2450Parser {
 public:
  Ld2450Reading reading;

  // Feed one UART byte. Returns true when a complete frame was parsed.
  bool feed(uint8_t b) {
    buf_[idx_++] = b;
    if (idx_ >= 30) {  // full frame buffered
      bool ok = parseFrame();
      idx_ = 0;
      return ok;
    }
    // Resync: if the first bytes stop looking like a header, slide the window.
    if (idx_ == 1 && buf_[0] != 0xAA) idx_ = 0;
    else if (idx_ == 2 && buf_[1] != 0xFF) { buf_[0] = buf_[1]; idx_ = 1; }
    else if (idx_ == 3 && buf_[2] != 0x03) idx_ = 0;
    else if (idx_ == 4 && buf_[3] != 0x00) idx_ = 0;
    return false;
  }

 private:
  uint8_t buf_[30] = {0};
  uint8_t idx_ = 0;

  static int16_t decode15(uint16_t raw) {
    int16_t mag = raw & 0x7FFF;
    return (raw & 0x8000) ? mag : (int16_t)-mag;
  }
  static uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }

  bool parseFrame() {
    if (buf_[0] != 0xAA || buf_[1] != 0xFF || buf_[2] != 0x03 || buf_[3] != 0x00) return false;
    if (buf_[28] != 0x55 || buf_[29] != 0xCC) return false;
    Ld2450Reading r;
    r.valid = true;
    for (int i = 0; i < 3; i++) {
      const uint8_t* p = buf_ + 4 + i * 8;
      Ld2450Target t;
      t.xMm = decode15(u16(p + 0));
      t.yMm = decode15(u16(p + 2));
      t.speedCmS = decode15(u16(p + 4));
      t.resolutionMm = u16(p + 6);
      t.present = (t.xMm != 0 || t.yMm != 0);
      r.targets[i] = t;
    }
    r.lastFrameMs = millis();
    reading = r;
    return true;
  }
};
