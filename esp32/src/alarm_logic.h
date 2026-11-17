// Car Sentry — alarm + sleep state machines (src/alarm_logic.h)
// PURE logic: no Arduino / WiFi / hardware dependencies, so it compiles and
// is unit-tested on a host PC (see test/). main.cpp feeds it radar presence
// and time (millis) and drives the buzzer/strobe/wake outputs from its state.
// Defensive own-vehicle alarm only. No transmitting, jamming, spoofing,
// intercepting, or attack code anywhere in this file or this firmware.
#pragma once
#include <cstdint>
#include <cstring>

// ---- Tunable alarm configuration (mirrors include/config.h defaults) ----
struct AlarmConfig {
  uint16_t maxRangeCm = 300;        // trigger only within this distance
  uint8_t motionSens = 50;          // 0..100, passed to the radar module
  uint8_t staticSens = 50;          // 0..100
  uint8_t maxGate = 4;              // LD2410 gate (0.75 m each)
  uint16_t noTargetSecs = 2;
  uint16_t alarmDurationS = 30;     // siren length per trigger
  uint16_t graceS = 30;             // grace period after ARM, no alarm
  uint16_t confirmMs = 1000;        // presence must persist this long
  uint16_t maxAlarmCycles = 3;      // retrigger cap, then latch quiet (see below)
  char disarmCode[16] = "1234";     // CHANGE on first setup
  bool deepSleep = false;           // true: deep sleep + radar-OUT wake; else light sleep
  uint32_t lightSleepCycleMs = 2000;
  uint32_t scanMs = 500;
  uint16_t idleToSleepS = 120;      // disarmed + idle this long -> light sleep
};

enum class AlarmState : uint8_t {
  DISARMED = 0,  // no alarm; presence only reported as STATUS
  GRACE = 1,     // just armed: owner walk-away window, detections ignored
  ARMED = 2,     // watching; a confirmed in-range person triggers ALARMING
  ALARMING = 3,  // siren + strobe + WAKE firing
  LATCHED = 4,   // max alarm cycles hit: quiet, still armed, needs manual disarm
};

enum class SleepMode : uint8_t {
  AWAKE = 0,
  LIGHT_SLEEP = 1,  // light sleep between scan cycles (RAM kept, fast wake)
  DEEP_SLEEP = 2,   // deep sleep, wake on radar-OUT pin or timer (RAM lost)
};

struct AlarmEvent {
  bool alarmStarted = false;  // entering ALARMING this tick
  bool alarmEnded = false;    // leaving ALARMING this tick
  bool wakeNow = false;       // host wake must fire (WoL + MQTT + GPIO pulse)
  bool stateChanged = false;
};

// Clamp helper (host + firmware).
static inline int csClampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Validate + clamp a config in place. Returns false if the disarm code is
// empty (refused: an alarm with no disarm code is a lockout risk).
static inline bool alarmConfigSanitize(AlarmConfig& c) {
  c.maxRangeCm = (uint16_t)csClampInt(c.maxRangeCm, 50, 800);
  c.motionSens = (uint8_t)csClampInt(c.motionSens, 0, 100);
  c.staticSens = (uint8_t)csClampInt(c.staticSens, 0, 100);
  c.maxGate = (uint8_t)csClampInt(c.maxGate, 2, 8);
  c.alarmDurationS = (uint16_t)csClampInt(c.alarmDurationS, 5, 300);
  c.graceS = (uint16_t)csClampInt(c.graceS, 0, 300);
  c.confirmMs = (uint16_t)csClampInt(c.confirmMs, 0, 10000);
  c.maxAlarmCycles = (uint16_t)csClampInt(c.maxAlarmCycles, 1, 10);
  if (c.disarmCode[0] == '\0') return false;
  return true;
}

class AlarmMachine {
 public:
  AlarmConfig cfg;
  AlarmState state = AlarmState::DISARMED;
  uint32_t stateEnteredMs = 0;   // when the current state began
  uint32_t alarmEndsMs = 0;      // when the current siren burst ends
  uint32_t presenceSinceMs = 0;  // start of the current continuous presence
  bool presenceNow = false;
  uint16_t lastDistCm = 0;
  uint16_t alarmCycles = 0;      // siren bursts since arming
  uint32_t lastPresenceMs = 0;   // last time any presence was seen

  // ARM from DISARMED (or re-arm from LATCHED). Returns false if already
  // armed/alarming — arming twice must not reset the grace period.
  bool arm(uint32_t nowMs) {
    if (state == AlarmState::GRACE || state == AlarmState::ARMED ||
        state == AlarmState::ALARMING) return false;
    state = AlarmState::GRACE;
    stateEnteredMs = nowMs;
    alarmCycles = 0;
    presenceSinceMs = 0;
    return true;
  }

  // DISARM. The code must match, unless `byButton` (physical owner button)
  // or `force` (host explicitly authorised, e.g. after laptop login).
  // Wrong code while ALARMING does NOT stop the siren. Returns false on
  // wrong code or when already disarmed.
  bool disarm(const char* code, uint32_t nowMs, bool byButton = false,
              bool force = false) {
    if (state == AlarmState::DISARMED) return false;
    if (!byButton && !force) {
      if (code == nullptr || std::strcmp(code, cfg.disarmCode) != 0) return false;
    }
    state = AlarmState::DISARMED;
    stateEnteredMs = nowMs;
    alarmEndsMs = 0;
    presenceSinceMs = 0;
    alarmCycles = 0;
    return true;
  }

  // One tick. `present` = radar reports a person, `distCm` = its distance
  // (0 = unknown). Returns events the firmware must act on this tick.
  AlarmEvent tick(bool present, uint16_t distCm, uint32_t nowMs) {
    AlarmEvent ev;
    AlarmState before = state;
    bool inRange = present && (distCm == 0 || distCm <= cfg.maxRangeCm);
    if (present) lastPresenceMs = nowMs;
    presenceNow = present;
    lastDistCm = distCm;

    switch (state) {
      case AlarmState::DISARMED:
        presenceSinceMs = 0;
        break;
      case AlarmState::GRACE:
        // Owner walk-away window: detections are ignored, never trigger.
        presenceSinceMs = 0;
        if (nowMs - stateEnteredMs >= (uint32_t)cfg.graceS * 1000U)
          state = AlarmState::ARMED, stateEnteredMs = nowMs;
        break;
      case AlarmState::ARMED: {
        if (inRange) {
          if (presenceSinceMs == 0) presenceSinceMs = nowMs;
          if (nowMs - presenceSinceMs >= cfg.confirmMs) {
            state = AlarmState::ALARMING;
            stateEnteredMs = nowMs;
            alarmEndsMs = nowMs + (uint32_t)cfg.alarmDurationS * 1000U;
            alarmCycles++;
            presenceSinceMs = 0;
            ev.alarmStarted = true;
            ev.wakeNow = true;  // wake the laptop the moment we trigger
          }
        } else {
          presenceSinceMs = 0;
        }
        break;
      }
      case AlarmState::ALARMING:
        if (nowMs >= alarmEndsMs) {
          ev.alarmEnded = true;
          if (alarmCycles >= cfg.maxAlarmCycles) {
            // Cap reached: stay armed but go quiet (LATCHED) instead of
            // sirening all night and draining the car battery / crying wolf.
            state = AlarmState::LATCHED;
            stateEnteredMs = nowMs;
          } else {
            state = AlarmState::ARMED;
            stateEnteredMs = nowMs;
            presenceSinceMs = 0;  // fresh confirmation needed to retrigger
          }
        }
        break;
      case AlarmState::LATCHED:
        // Quiet but still armed: no new siren until disarm + re-arm.
        presenceSinceMs = 0;
        break;
    }
    ev.stateChanged = (state != before);
    return ev;
  }

  // Siren/strobe outputs: HIGH only while ALARMING, alternating 500 ms
  // on/off (pattern) so it is plainly an alarm, not a stuck output.
  bool sirenOn(uint32_t nowMs) const {
    if (state != AlarmState::ALARMING) return false;
    return ((nowMs - stateEnteredMs) / 500U) % 2U == 0U;
  }
  bool strobeOn(uint32_t nowMs) const { return sirenOn(nowMs); }

  const char* stateName() const {
    switch (state) {
      case AlarmState::DISARMED: return "DISARMED";
      case AlarmState::GRACE: return "GRACE";
      case AlarmState::ARMED: return "ARMED";
      case AlarmState::ALARMING: return "ALARMING";
      case AlarmState::LATCHED: return "LATCHED";
    }
    return "?";
  }
  uint32_t msInState(uint32_t nowMs) const { return nowMs - stateEnteredMs; }
  uint32_t graceRemainingS(uint32_t nowMs) const {
    if (state != AlarmState::GRACE) return 0;
    uint32_t el = (nowMs - stateEnteredMs) / 1000U;
    return el >= cfg.graceS ? 0 : cfg.graceS - el;
  }
  uint32_t alarmRemainingS(uint32_t nowMs) const {
    if (state != AlarmState::ALARMING) return 0;
    return nowMs >= alarmEndsMs ? 0 : (alarmEndsMs - nowMs + 999U) / 1000U;
  }
};

// ---- Sleep decision (pure logic; main.cpp performs the actual sleep) ----
// Rules:
//  * Never sleep while ALARMING (siren must sound) or in GRACE (owner may
//    still be at the car; keep watching).
//  * ARMED + deepSleep enabled + radar-OUT wired -> DEEP_SLEEP, wake on OUT.
//  * ARMED + deepSleep off -> LIGHT_SLEEP between scan cycles.
//  * DISARMED + idle past idleToSleepS -> LIGHT_SLEEP (scan rarely).
//  * LATCHED -> LIGHT_SLEEP (quiet, still watching for STATUS).
//  * Battery cutoff (voltage below cutoff) -> DEEP_SLEEP regardless, to
//    protect the car battery; the timer wake re-checks voltage.
static inline SleepMode sleepDecision(const AlarmMachine& m, uint32_t nowMs,
                                      bool radarOutWired, bool batteryCutoff) {
  if (batteryCutoff) return SleepMode::DEEP_SLEEP;
  switch (m.state) {
    case AlarmState::ALARMING:
    case AlarmState::GRACE:
      return SleepMode::AWAKE;
    case AlarmState::ARMED:
      if (m.cfg.deepSleep && radarOutWired) return SleepMode::DEEP_SLEEP;
      return SleepMode::LIGHT_SLEEP;
    case AlarmState::LATCHED:
      return SleepMode::LIGHT_SLEEP;
    case AlarmState::DISARMED:
    default:
      if (nowMs - m.lastPresenceMs >= (uint32_t)m.cfg.idleToSleepS * 1000U &&
          m.lastPresenceMs != 0)
        return SleepMode::LIGHT_SLEEP;
      return SleepMode::AWAKE;
  }
}
