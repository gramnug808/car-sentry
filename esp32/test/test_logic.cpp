// Car Sentry — host unit tests (test/test_logic.cpp)
// Tests the PURE logic headers (alarm_logic.h, protocol.h) plus the LD2410
// parser (ld2410.h, via the Arduino stub) with plain g++ — no ESP32, no
// PlatformIO needed. Run: ./test/run_tests.sh  (or g++ directly, see below)
// Exit code 0 = all pass, 1 = any failure.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <Arduino.h>
uint32_t mockMillis = 0;

#include "../src/alarm_logic.h"
#include "../src/protocol.h"
#include "../src/ld2410.h"

static int passed = 0, failed = 0;
#define CHECK(cond, name) do { \
  if (cond) { passed++; printf("  PASS %s\n", name); } \
  else { failed++; printf("  FAIL %s\n", name); } \
} while (0)

// Feed a complete LD2410 basic report frame into a parser.
// state: 0 none, 1 moving, 2 static, 3 both. Distances/energies in cm / %.
static bool feedBasic(Ld2410Parser& p, uint8_t state, uint16_t movD, uint8_t movE,
                      uint16_t statD, uint8_t statE, uint16_t detD) {
  uint8_t f[] = {0xF4,0xF3,0xF2,0xF1, 0x0B,0x00, 0x02,0xAA, state,
                 (uint8_t)(movD&0xFF),(uint8_t)(movD>>8), movE,
                 (uint8_t)(statD&0xFF),(uint8_t)(statD>>8), statE,
                 (uint8_t)(detD&0xFF),(uint8_t)(detD>>8),
                 0xF8,0xF7,0xF6,0xF5};
  bool got = false;
  for (size_t i = 0; i < sizeof(f); i++) if (p.feed(f[i])) got = true;
  return got;
}

static void testAlarm() {
  printf("[alarm state machine]\n");
  AlarmMachine m;  // defaults: range 300 cm, grace 30 s, confirm 1000 ms, alarm 30 s
  uint32_t t = 1000;

  // 1. Disarmed: presence never triggers.
  AlarmEvent ev = m.tick(true, 180, t);
  CHECK(m.state == AlarmState::DISARMED && !ev.alarmStarted, "disarmed: presence ignored");

  // 2. Arm -> GRACE; presence during grace does not trigger.
  CHECK(m.arm(t), "arm from disarmed succeeds");
  CHECK(m.state == AlarmState::GRACE, "arm -> GRACE");
  CHECK(!m.arm(t + 1000), "arming twice does not reset grace");
  ev = m.tick(true, 180, t + 5000);
  CHECK(m.state == AlarmState::GRACE && !ev.alarmStarted, "grace: detection ignored");
  CHECK(m.graceRemainingS(t + 5000) == 25, "grace remaining counts down (25 s)");

  // 3. After grace -> ARMED.
  ev = m.tick(false, 0, t + 30000);
  CHECK(m.state == AlarmState::ARMED && ev.stateChanged, "grace expiry -> ARMED");

  // 4. Brief presence (< confirm 1000 ms) does not trigger.
  ev = m.tick(true, 180, t + 31000);
  CHECK(!ev.alarmStarted, "armed: brief presence, no trigger yet");
  ev = m.tick(false, 0, t + 31500);
  CHECK(m.state == AlarmState::ARMED, "presence lost before confirm resets");

  // 5. Sustained in-range presence triggers; wake fires.
  m.tick(true, 180, t + 32000);
  ev = m.tick(true, 180, t + 33000);
  CHECK(m.state == AlarmState::ALARMING && ev.alarmStarted && ev.wakeNow,
        "sustained presence -> ALARMING + wake");
  CHECK(m.alarmRemainingS(t + 33000) == 30, "alarm remaining 30 s at trigger");
  CHECK(m.sirenOn(t + 33100) && !m.sirenOn(t + 33600),
        "siren pattern alternates every 500 ms");

  // 6. Wrong disarm code does not stop the siren; right code does.
  CHECK(!m.disarm("9999", t + 34000), "wrong disarm code rejected");
  CHECK(m.state == AlarmState::ALARMING, "wrong code: siren keeps sounding");
  CHECK(m.disarm("1234", t + 34000), "correct disarm code works");
  CHECK(m.state == AlarmState::DISARMED && !m.sirenOn(t + 34000),
        "disarmed: siren off");

  // 7. Out-of-range presence never triggers.
  m.arm(t + 40000);
  m.tick(false, 0, t + 70000);  // grace expires -> ARMED
  CHECK(m.state == AlarmState::ARMED, "re-arm -> ARMED after grace");
  m.tick(true, 500, t + 71000);
  ev = m.tick(true, 500, t + 73000);
  CHECK(m.state == AlarmState::ARMED && !ev.alarmStarted,
        "out-of-range (500 cm > 300 cm) never triggers");

  // 8. Unknown distance (0) is treated as in-range (radar may not report it).
  m.tick(true, 0, t + 74000);
  ev = m.tick(true, 0, t + 75000);
  CHECK(m.state == AlarmState::ALARMING, "dist 0 treated as in-range trigger");

  // 9. Alarm burst ends -> ARMED; retrigger works until max cycles -> LATCHED.
  m.cfg.maxAlarmCycles = 2;
  uint32_t base = t + 75000;
  m.tick(true, 180, base + 30000);  // burst 1 ends (30 s)
  CHECK(m.state == AlarmState::ARMED, "burst end -> ARMED (cycles left)");
  m.tick(true, 180, base + 31000);
  ev = m.tick(true, 180, base + 32000);  // burst 2 starts
  CHECK(ev.alarmStarted && m.alarmCycles == 2, "retrigger starts burst 2");
  m.tick(true, 180, base + 62000);  // burst 2 ends
  CHECK(m.state == AlarmState::LATCHED, "max cycles hit -> LATCHED (quiet)");
  ev = m.tick(true, 180, base + 64000);
  CHECK(m.state == AlarmState::LATCHED && !ev.alarmStarted,
        "latched: presence no longer sirens");
  CHECK(m.disarm("", base + 65000, true), "physical disarm button clears latch");
  CHECK(m.state == AlarmState::DISARMED, "button disarm -> DISARMED");

  // 10. Config sanitize.
  AlarmConfig c;
  c.maxRangeCm = 9999; c.motionSens = 200; c.alarmDurationS = 1; c.graceS = 999;
  CHECK(alarmConfigSanitize(c), "sanitize accepts clamped config");
  CHECK(c.maxRangeCm == 800 && c.motionSens == 100 && c.alarmDurationS == 5 &&
        c.graceS == 300, "sanitize clamps range/sens/duration/grace");
  c.disarmCode[0] = '\0';
  CHECK(!alarmConfigSanitize(c), "sanitize rejects empty disarm code");
}

static void testSleep() {
  printf("[sleep / wake state machine]\n");
  AlarmMachine m;
  uint32_t t = 100000;
  // Disarmed + never saw presence: awake (nothing to sleep for yet).
  CHECK(sleepDecision(m, t, true, false) == SleepMode::AWAKE,
        "disarmed, no presence history -> AWAKE");
  m.lastPresenceMs = t - 200000;  // idle 200 s
  CHECK(sleepDecision(m, t, true, false) == SleepMode::LIGHT_SLEEP,
        "disarmed + idle -> LIGHT_SLEEP");
  m.arm(t);
  CHECK(sleepDecision(m, t, true, false) == SleepMode::AWAKE,
        "GRACE -> AWAKE (owner may be at the car)");
  m.tick(false, 0, t + 30000);  // -> ARMED
  m.cfg.deepSleep = false;
  CHECK(sleepDecision(m, t + 31000, true, false) == SleepMode::LIGHT_SLEEP,
        "ARMED, deep sleep off -> LIGHT_SLEEP");
  m.cfg.deepSleep = true;
  CHECK(sleepDecision(m, t + 31000, true, false) == SleepMode::DEEP_SLEEP,
        "ARMED, deep sleep + OUT wired -> DEEP_SLEEP");
  CHECK(sleepDecision(m, t + 31000, false, false) == SleepMode::LIGHT_SLEEP,
        "ARMED, deep sleep but OUT not wired -> LIGHT_SLEEP (fallback)");
  m.tick(true, 180, t + 32000);
  m.tick(true, 180, t + 33000);  // -> ALARMING
  CHECK(sleepDecision(m, t + 33000, true, false) == SleepMode::AWAKE,
        "ALARMING -> AWAKE (siren must sound)");
  CHECK(sleepDecision(m, t + 33000, true, true) == SleepMode::DEEP_SLEEP,
        "battery cutoff -> DEEP_SLEEP regardless of state");
}

static void testProtocol() {
  printf("[protocol parsing / building]\n");
  HostCommand c;
  CHECK(protoParseCommand("{\"cmd\":\"ARM\"}", &c) && c.cmd == HostCmd::ARM,
        "parse ARM");
  CHECK(protoParseCommand("{\"cmd\":\"DISARM\",\"code\":\"1234\"}", &c) &&
        c.cmd == HostCmd::DISARM && std::strcmp(c.code, "1234") == 0,
        "parse DISARM + code");
  CHECK(protoParseCommand("{\"cmd\":\"STATUS\"}", &c) && c.cmd == HostCmd::STATUS,
        "parse STATUS");
  CHECK(protoParseCommand("{\"cmd\":\"WAKE_TEST\"}", &c) && c.cmd == HostCmd::WAKE_TEST,
        "parse WAKE_TEST");
  CHECK(protoParseCommand(
            "{\"cmd\":\"CONFIG\",\"max_range_cm\":250,\"alarm_duration_s\":45,"
            "\"grace_s\":15,\"motion_sensitivity\":60,\"static_sensitivity\":40,"
            "\"deep_sleep\":true,\"disarm_code\":\"9876\"}", &c) &&
        c.cmd == HostCmd::CONFIG && c.hasMaxRange && c.maxRangeCm == 250 &&
        c.alarmDurationS == 45 && c.graceS == 15 && c.motionSens == 60 &&
        c.staticSens == 40 && c.deepSleep && std::strcmp(c.code, "9876") == 0,
        "parse CONFIG (all fields)");
  CHECK(!protoParseCommand("{\"cmd\":\"EXPLODE\"}", &c), "unknown cmd rejected");
  CHECK(!protoParseCommand("not json at all", &c), "garbage rejected");
  CHECK(!protoParseCommand("{\"cmd\":\"DISARM\"}", &c) || c.cmd == HostCmd::DISARM,
        "DISARM without code parses (auth fails later, not here)");
  CHECK(!protoParseCommand("", &c), "empty line rejected");

  char buf[256];
  protoBuildStatus(buf, sizeof(buf), "ARMED", true, 180, true, 123, 12.6f, 0, 0);
  CHECK(std::strstr(buf, "\"state\":\"ARMED\"") && std::strstr(buf, "\"dist_cm\":180") &&
        std::strstr(buf, "\"vbat_v\":12.6"),
        "STATUS json shape");
  protoBuildAlert(buf, sizeof(buf), 180, "ALARMING", 123);
  CHECK(std::strstr(buf, "\"type\":\"ALERT\"") &&
        std::strstr(buf, "\"event\":\"person_detected\""),
        "ALERT json shape");
  protoBuildWake(buf, sizeof(buf), "person_detected");
  CHECK(std::strstr(buf, "\"type\":\"WAKE\"") &&
        std::strstr(buf, "\"method\":\"wol+mqtt+gpio\""),
        "WAKE json shape");
  protoBuildError(buf, sizeof(buf), "bad disarm code");
  CHECK(std::strstr(buf, "\"type\":\"ERROR\""), "ERROR json shape");

  // Wake-on-LAN magic packet.
  uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  uint8_t pkt[102];
  protoBuildWolPacket(mac, pkt);
  bool wolOk = true;
  for (int i = 0; i < 6; i++) wolOk &= (pkt[i] == 0xFF);
  for (int r = 0; r < 16; r++)
    for (int i = 0; i < 6; i++) wolOk &= (pkt[6 + r * 6 + i] == mac[i]);
  CHECK(wolOk, "WoL packet: 6xFF + MAC x16");
  uint8_t m2[6];
  CHECK(protoParseMac("AA:BB:CC:DD:EE:FF", m2) && m2[0] == 0xAA && m2[5] == 0xFF,
        "parse MAC (colons)");
  CHECK(protoParseMac("AA-BB-CC-DD-EE-FF", m2), "parse MAC (dashes)");
  CHECK(!protoParseMac("not-a-mac", m2), "bad MAC rejected");
  CHECK(!protoParseMac("AA:BB:CC", m2), "short MAC rejected");
}

static void testLd2410() {
  printf("[LD2410 parser]\n");
  Ld2410Parser p;
  mockMillis = 5000;
  CHECK(feedBasic(p, 1, 180, 75, 0, 0, 180), "basic moving frame parses");
  CHECK(p.reading.valid && p.reading.moving && !p.reading.stationary &&
        p.reading.movingDistCm == 180 && p.reading.movingEnergy == 75,
        "moving frame fields");
  CHECK(feedBasic(p, 3, 200, 60, 120, 40, 120), "both-targets frame parses");
  CHECK(p.reading.moving && p.reading.stationary &&
        p.reading.staticDistCm == 120 && p.reading.staticEnergy == 40,
        "both-targets fields");
  CHECK(feedBasic(p, 0, 0, 0, 0, 0, 0), "no-target frame parses");
  CHECK(!p.reading.moving && !p.reading.stationary, "no-target state");

  // Garbage bytes before a good frame: parser resyncs.
  Ld2410Parser q;
  uint8_t garbage[] = {0x00, 0xFF, 0xF4, 0x11, 0x22};
  for (uint8_t b : garbage) q.feed(b);
  CHECK(feedBasic(q, 2, 0, 0, 90, 55, 90), "resync through garbage");
  CHECK(q.reading.stationary && q.reading.staticDistCm == 90,
        "frame after garbage is correct");

  // Bad footer: frame rejected, previous reading kept.
  Ld2410Parser r;
  feedBasic(r, 1, 180, 75, 0, 0, 180);
  uint8_t badFoot[] = {0xF4,0xF3,0xF2,0xF1, 0x0B,0x00, 0x02,0xAA, 1,
                       0xB4,0x00, 75, 0,0, 0, 0xB4,0x00,
                       0x00,0x00,0x00,0x00};  // wrong footer
  bool got = false;
  for (uint8_t b : badFoot) if (r.feed(b)) got = true;
  CHECK(!got && r.reading.movingDistCm == 180, "bad footer rejected");

  // Bad report type (0x03): rejected.
  Ld2410Parser s;
  uint8_t badType[] = {0xF4,0xF3,0xF2,0xF1, 0x0B,0x00, 0x03,0xAA, 1,
                       0xB4,0x00, 75, 0,0, 0, 0xB4,0x00,
                       0xF8,0xF7,0xF6,0xF5};
  got = false;
  for (uint8_t b : badType) if (s.feed(b)) got = true;
  CHECK(!got && !s.reading.valid, "bad report type rejected");
}

int main() {
  printf("Car Sentry host tests\n");
  testAlarm();
  testSleep();
  testProtocol();
  testLd2410();
  printf("\n%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
