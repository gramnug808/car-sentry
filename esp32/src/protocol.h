// Car Sentry — host protocol (src/protocol.h)
// PURE logic, no Arduino dependencies: JSON lines over serial, MQTT payloads
// and HTTP bodies all use these same message shapes.
//   Host -> ESP32 (one JSON object per line, newline terminated):
//     {"cmd":"ARM"}                          arm (grace period starts)
//     {"cmd":"DISARM","code":"1234"}         disarm (code required)
//     {"cmd":"STATUS"}                       request a STATUS reply
//     {"cmd":"CONFIG","max_range_cm":250,"alarm_duration_s":30,
//                  "grace_s":30,"motion_sensitivity":50,
//                  "static_sensitivity":50,"disarm_code":"1234",
//                  "deep_sleep":false}
//     {"cmd":"WAKE_TEST"}                    fire the wake outputs once (test)
//     {"cmd":"SLEEP"}                        allow sleep now (host is done)
//   ESP32 -> host:
//     {"type":"STATUS","state":"ARMED","presence":false,"dist_cm":0,
//      "sensor_ok":true,"uptime_s":123,"vbat_v":12.6,"grace_s":0,"alarm_s":0}
//     {"type":"ALERT","event":"person_detected","dist_cm":180,
//      "state":"ALARMING","uptime_s":123}
//     {"type":"WAKE","reason":"person_detected","method":"wol+mqtt+gpio"}
//     {"type":"ARMED","state":"GRACE","grace_s":30}
//     {"type":"DISARMED","state":"DISARMED"}
//     {"type":"ERROR","error":"bad disarm code"}
// MQTT (when WiFi/MQTT configured): same JSON on
//   carsentry/<id>/status  (STATUS, retained), carsentry/<id>/alert (ALERT),
//   carsentry/<id>/wake   (WAKE),  and commands arrive on
//   carsentry/<id>/command (same {"cmd":...} objects as serial).
// This file implements the small parser/builder used by main.cpp AND by the
// host unit tests. It deliberately does NOT use ArduinoJson (so host tests
// need no Arduino toolchain); main.cpp uses ArduinoJson only for HTTP.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum class HostCmd : uint8_t {
  UNKNOWN = 0,
  ARM = 1,
  DISARM = 2,
  STATUS = 3,
  CONFIG = 4,
  WAKE_TEST = 5,
  SLEEP = 6,
};

struct HostCommand {
  HostCmd cmd = HostCmd::UNKNOWN;
  char code[16] = {0};           // DISARM code / new disarm_code
  bool hasMaxRange = false;
  uint16_t maxRangeCm = 0;
  bool hasAlarmDuration = false;
  uint16_t alarmDurationS = 0;
  bool hasGrace = false;
  uint16_t graceS = 0;
  bool hasMotionSens = false;
  uint8_t motionSens = 0;
  bool hasStaticSens = false;
  uint8_t staticSens = 0;
  bool hasDeepSleep = false;
  bool deepSleep = false;
  bool hasCode = false;          // CONFIG disarm_code present
};

// --- tiny JSON helpers (flat objects only; enough for this protocol) ---
static inline bool protoFindString(const char* json, const char* key,
                                   char* out, size_t outSize) {
  // Looks for "key":"value". Returns false when absent.
  char pat[48];
  std::snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = std::strstr(json, pat);
  if (!p) return false;
  p = std::strchr(p + std::strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outSize) out[i++] = *p++;
  out[i] = '\0';
  return *p == '"';
}

static inline bool protoFindInt(const char* json, const char* key, long* out) {
  char pat[48];
  std::snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = std::strstr(json, pat);
  if (!p) return false;
  p = std::strchr(p + std::strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') return false;  // quoted: not a number
  char* end = nullptr;
  long v = std::strtol(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

static inline bool protoFindBool(const char* json, const char* key, bool* out) {
  char pat[48];
  std::snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = std::strstr(json, pat);
  if (!p) return false;
  p = std::strchr(p + std::strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (std::strncmp(p, "true", 4) == 0) { *out = true; return true; }
  if (std::strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

// Parse one host command line. Returns false for garbage/unknown cmd
// (caller should reply ERROR, not act).
static inline bool protoParseCommand(const char* line, HostCommand* out) {
  if (!line || !out) return false;
  *out = HostCommand();
  char cmdStr[16] = {0};
  if (!protoFindString(line, "cmd", cmdStr, sizeof(cmdStr))) return false;
  long v = 0;
  bool b = false;
  if (std::strcmp(cmdStr, "ARM") == 0) out->cmd = HostCmd::ARM;
  else if (std::strcmp(cmdStr, "DISARM") == 0) {
    out->cmd = HostCmd::DISARM;
    out->hasCode = protoFindString(line, "code", out->code, sizeof(out->code));
  } else if (std::strcmp(cmdStr, "STATUS") == 0) out->cmd = HostCmd::STATUS;
  else if (std::strcmp(cmdStr, "WAKE_TEST") == 0) out->cmd = HostCmd::WAKE_TEST;
  else if (std::strcmp(cmdStr, "SLEEP") == 0) out->cmd = HostCmd::SLEEP;
  else if (std::strcmp(cmdStr, "CONFIG") == 0) {
    out->cmd = HostCmd::CONFIG;
    if (protoFindInt(line, "max_range_cm", &v)) { out->hasMaxRange = true; out->maxRangeCm = (uint16_t)v; }
    if (protoFindInt(line, "alarm_duration_s", &v)) { out->hasAlarmDuration = true; out->alarmDurationS = (uint16_t)v; }
    if (protoFindInt(line, "grace_s", &v)) { out->hasGrace = true; out->graceS = (uint16_t)v; }
    if (protoFindInt(line, "motion_sensitivity", &v)) { out->hasMotionSens = true; out->motionSens = (uint8_t)v; }
    if (protoFindInt(line, "static_sensitivity", &v)) { out->hasStaticSens = true; out->staticSens = (uint8_t)v; }
    if (protoFindBool(line, "deep_sleep", &b)) { out->hasDeepSleep = true; out->deepSleep = b; }
    if (protoFindString(line, "disarm_code", out->code, sizeof(out->code))) out->hasCode = true;
  } else return false;
  return true;
}

// --- Outbound message builders (snprintf, fixed shapes) ---
static inline int protoBuildStatus(char* out, size_t n, const char* state,
                                   bool presence, uint16_t distCm,
                                   bool sensorOk, uint32_t uptimeS,
                                   float vbatV, uint32_t graceS,
                                   uint32_t alarmS) {
  return std::snprintf(out, n,
    "{\"type\":\"STATUS\",\"state\":\"%s\",\"presence\":%s,\"dist_cm\":%u,"
    "\"sensor_ok\":%s,\"uptime_s\":%lu,\"vbat_v\":%.1f,\"grace_s\":%lu,\"alarm_s\":%lu}",
    state, presence ? "true" : "false", distCm,
    sensorOk ? "true" : "false", (unsigned long)uptimeS, (double)vbatV,
    (unsigned long)graceS, (unsigned long)alarmS);
}

static inline int protoBuildAlert(char* out, size_t n, uint16_t distCm,
                                  const char* state, uint32_t uptimeS) {
  return std::snprintf(out, n,
    "{\"type\":\"ALERT\",\"event\":\"person_detected\",\"dist_cm\":%u,"
    "\"state\":\"%s\",\"uptime_s\":%lu}",
    distCm, state, (unsigned long)uptimeS);
}

static inline int protoBuildWake(char* out, size_t n, const char* reason) {
  return std::snprintf(out, n,
    "{\"type\":\"WAKE\",\"reason\":\"%s\",\"method\":\"wol+mqtt+gpio\"}", reason);
}

static inline int protoBuildSimple(char* out, size_t n, const char* type,
                                   const char* state) {
  return std::snprintf(out, n, "{\"type\":\"%s\",\"state\":\"%s\"}", type, state);
}

static inline int protoBuildError(char* out, size_t n, const char* error) {
  return std::snprintf(out, n, "{\"type\":\"ERROR\",\"error\":\"%s\"}", error);
}

// Wake-on-LAN magic packet: 6 x 0xFF followed by the target MAC 16 times.
// Pure function, host-testable. `mac` = 6 bytes, `out` needs 102 bytes.
static inline void protoBuildWolPacket(const uint8_t mac[6], uint8_t out[102]) {
  for (int i = 0; i < 6; i++) out[i] = 0xFF;
  for (int r = 0; r < 16; r++)
    for (int i = 0; i < 6; i++) out[6 + r * 6 + i] = mac[i];
}

// Parse a MAC string "AA:BB:CC:DD:EE:FF" (colons or dashes). False if malformed.
static inline bool protoParseMac(const char* s, uint8_t mac[6]) {
  if (!s) return false;
  unsigned int b[6];
  if (std::sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
      std::sscanf(s, "%2x-%2x-%2x-%2x-%2x-%2x",
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return true;
  }
  return false;
}
