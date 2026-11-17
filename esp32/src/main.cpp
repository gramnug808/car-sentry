// Car Sentry — ESP32 firmware (src/main.cpp)
// Standalone alarm / presence-sensor node for the user's OWN vehicle.
//   Sensor: HLK-LD2410 (default) or HLK-LD2450 (compile-time -DSENSOR_LD2450)
//   What it does: mmWave person detection near the vehicle, car-alarm mode
//   (grace period, siren + strobe, disarm code/button), wake the host LAPTOP
//   (Wake-on-LAN magic packet + MQTT WAKE + GPIO wake pulse), light/deep
//   sleep, serial + MQTT + HTTP protocol (ARM/DISARM/ALERT/WAKE/STATUS).
//   What it does NOT do: no wardriving, no SDR, no LPR, no signal intel,
//   no jamming/spoofing/intercepting. It only talks to the host via the
//   documented protocol (see README.md). Local network only, no cloud.
//
// Host = LAPTOP (Linux, DragonOS/Ubuntu) on car 12 V inverter / USB-C.
// Wake methods actually implemented here — see README "Waking the laptop":
//   1. Wake-on-LAN magic packet over WiFi (primary, implemented).
//   2. MQTT WAKE message (implemented — notifies an AWAKE host/broker only;
//      it cannot wake a sleeping laptop by itself).
//   3. GPIO wake pulse on WAKE_PIN (implemented — for an external wake
//      adapter, e.g. a USB-HID dongle or power-button relay; the ESP32
//      cannot wake a laptop over plain USB serial on its own).
//   USB HID keyboard wake is NOT implemented on this classic-ESP32 board
//   (no native USB); see README for that future option.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Update.h>
#include <esp_sleep.h>
#include "config.h"
#include "alarm_logic.h"
#include "protocol.h"

#define FW_VERSION "1.0.0"
#define FW_BUILD_DATE __DATE__ " " __TIME__

#if defined(SENSOR_LD2450)
#include "ld2450.h"
#define SENSOR_NAME "LD2450"
static Ld2450Parser radar;
#else
#include "ld2410.h"
#define SENSOR_NAME "LD2410"
static Ld2410Parser radar;
#endif

// ---- state kept across deep sleep (RTC memory survives, RAM does not) ----
RTC_DATA_ATTR bool rtcWasArmed = false;
RTC_DATA_ATTR uint32_t rtcBootCount = 0;

// ---- persistent config (NVS "carsentry") + WiFi/MQTT settings ----
static AlarmMachine alarmMachine;
static Preferences prefs;
static String staSsid, staPass, mqttHost, laptopMacStr, deviceId;
static uint16_t mqttPort = MQTT_PORT;
static uint8_t laptopMac[6] = {0};
static bool laptopMacSet = false;
static bool radarOutWired = true;  // set false via CONFIG if OUT is not wired

static void loadConfig() {
  prefs.begin("carsentry", false);
  alarmMachine.cfg.maxRangeCm = prefs.getUShort("range", DEFAULT_MAX_RANGE_CM);
  alarmMachine.cfg.motionSens = prefs.getUChar("motion", DEFAULT_MOTION_SENS);
  alarmMachine.cfg.staticSens = prefs.getUChar("static", DEFAULT_STATIC_SENS);
  alarmMachine.cfg.maxGate = prefs.getUChar("maxgate", DEFAULT_MAX_GATE);
  alarmMachine.cfg.noTargetSecs = prefs.getUShort("notarget", DEFAULT_NO_TARGET_SECS);
  alarmMachine.cfg.alarmDurationS = prefs.getUShort("alarmdur", DEFAULT_ALARM_DURATION_S);
  alarmMachine.cfg.graceS = prefs.getUShort("grace", DEFAULT_GRACE_S);
  alarmMachine.cfg.confirmMs = prefs.getUShort("confirm", DEFAULT_CONFIRM_MS);
  alarmMachine.cfg.deepSleep = prefs.getBool("deepsleep", DEFAULT_DEEP_SLEEP);
  alarmMachine.cfg.lightSleepCycleMs = prefs.getUInt("sleepcyc", DEFAULT_LIGHT_SLEEP_CYCLE_MS);
  String code = prefs.getString("code", DEFAULT_DISARM_CODE);
  strncpy(alarmMachine.cfg.disarmCode, code.c_str(), sizeof(alarmMachine.cfg.disarmCode) - 1);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  mqttHost = prefs.getString("mqtthost", "");
  mqttPort = prefs.getUShort("mqttport", MQTT_PORT);
  laptopMacStr = prefs.getString("laptopmac", "");
  laptopMacSet = protoParseMac(laptopMacStr.c_str(), laptopMac);
  radarOutWired = prefs.getBool("outwired", true);
  alarmConfigSanitize(alarmMachine.cfg);
}

static void saveConfig() {
  prefs.putUShort("range", alarmMachine.cfg.maxRangeCm);
  prefs.putUChar("motion", alarmMachine.cfg.motionSens);
  prefs.putUChar("static", alarmMachine.cfg.staticSens);
  prefs.putUChar("maxgate", alarmMachine.cfg.maxGate);
  prefs.putUShort("notarget", alarmMachine.cfg.noTargetSecs);
  prefs.putUShort("alarmdur", alarmMachine.cfg.alarmDurationS);
  prefs.putUShort("grace", alarmMachine.cfg.graceS);
  prefs.putUShort("confirm", alarmMachine.cfg.confirmMs);
  prefs.putBool("deepsleep", alarmMachine.cfg.deepSleep);
  prefs.putUInt("sleepcyc", alarmMachine.cfg.lightSleepCycleMs);
  prefs.putString("code", alarmMachine.cfg.disarmCode);
  prefs.putString("mqtthost", mqttHost);
  prefs.putUShort("mqttport", mqttPort);
  prefs.putString("laptopmac", laptopMacStr);
  prefs.putBool("outwired", radarOutWired);
}

// ---- live presence ----
struct Presence { bool present = false; uint16_t distCm = 0; bool sensorOk = false; };
static Presence currentPresence() {
  Presence p;
#if defined(SENSOR_LD2450)
  const Ld2450Reading& r = radar.reading;
  p.sensorOk = r.valid && (millis() - r.lastFrameMs < SENSOR_TIMEOUT_MS);
  if (!r.valid) return p;
  const Ld2450Target* near = r.nearest();
  if (near) { p.present = true; p.distCm = near->distCm(); }
#else
  const RadarReading& r = radar.reading;
  p.sensorOk = r.valid && (millis() - r.lastFrameMs < SENSOR_TIMEOUT_MS);
  if (!r.valid) return p;
  p.present = r.moving || r.stationary;
  p.distCm = r.detectionDistCm;
#endif
  return p;
}

// ---- battery / car voltage ----
static float readVbat() {
  int raw = analogRead(VIN_SENSE_PIN);  // 12-bit ADC, ~3.3 V full scale @ 11 dB
  return (raw / 4095.0f) * 3.3f * VIN_DIVIDER_RATIO;
}
static float vbatV = 12.6f;
static uint32_t belowCutoffSinceMs = 0;
static bool batteryCutoff = false;
static void checkBattery(uint32_t nowMs) {
  vbatV = readVbat();
  if (vbatV < VIN_CUTOFF_V) {
    if (belowCutoffSinceMs == 0) belowCutoffSinceMs = nowMs;
    if (nowMs - belowCutoffSinceMs >= (uint32_t)VIN_CUTOFF_HOLD_S * 1000U)
      batteryCutoff = true;
  } else if (vbatV > VIN_RECOVER_V) {
    belowCutoffSinceMs = 0;
    batteryCutoff = false;
  }
  if (batteryCutoff && alarmMachine.state == AlarmState::ALARMING) {
    // Stop sirening on a dying battery; stay armed in memory, go quiet.
    alarmMachine.disarm(alarmMachine.cfg.disarmCode, nowMs, false, true);
    Serial.println("[power] battery cutoff: alarm stopped, going to sleep");
  }
}

// ---- outputs ----
static uint32_t wakePulseUntilMs = 0;
static void driveOutputs(uint32_t nowMs) {
  bool on = alarmMachine.sirenOn(nowMs) && !batteryCutoff;
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  digitalWrite(STROBE_PIN, on ? HIGH : LOW);
  digitalWrite(WAKE_PIN, (nowMs < wakePulseUntilMs) ? HIGH : LOW);
}

// ---- comms: serial + MQTT + HTTP share these send helpers ----
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static WebServer http(HTTP_PORT);
static DNSServer dns;
static String apSsid;

static String topic(const char* leaf) {
  return String("carsentry/") + deviceId + "/" + leaf;
}

static void sendStatusSerial() {
  Presence p = currentPresence();
  char buf[256];
  protoBuildStatus(buf, sizeof(buf), alarmMachine.stateName(), p.present, p.distCm,
                   p.sensorOk, millis() / 1000, vbatV,
                   alarmMachine.graceRemainingS(millis()), alarmMachine.alarmRemainingS(millis()));
  Serial.println(buf);
}
static void publishStatus() {
  if (!mqtt.connected()) return;
  Presence p = currentPresence();
  char buf[256];
  protoBuildStatus(buf, sizeof(buf), alarmMachine.stateName(), p.present, p.distCm,
                   p.sensorOk, millis() / 1000, vbatV,
                   alarmMachine.graceRemainingS(millis()), alarmMachine.alarmRemainingS(millis()));
  mqtt.publish(topic("status").c_str(), buf, true);
}
static void publishAlert(uint16_t distCm) {
  char buf[192];
  protoBuildAlert(buf, sizeof(buf), distCm, alarmMachine.stateName(), millis() / 1000);
  Serial.println(buf);
  if (mqtt.connected()) mqtt.publish(topic("alert").c_str(), buf, false);
}

// ---- Wake the laptop (see README for what each method can/can't do) ----
static bool sendWol() {
  if (!laptopMacSet) {
    Serial.println("[wake] WoL skipped: no laptop MAC configured (set laptop_mac)");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wake] WoL skipped: WiFi STA not connected (WoL needs the laptop's LAN)");
    return false;
  }
  uint8_t pkt[102];
  protoBuildWolPacket(laptopMac, pkt);
  WiFiUDP udp;
  IPAddress bcast = WiFi.broadcastIP();
  udp.beginPacket(bcast, 9);
  udp.write(pkt, sizeof(pkt));
  bool ok = udp.endPacket() == 1;
  Serial.printf("[wake] WoL magic packet -> %s : %s\n",
                bcast.toString().c_str(), ok ? "sent" : "FAILED");
  return ok;
}

static void fireWake(const char* reason) {
  // 1) GPIO pulse (500 ms) — for an external wake adapter, if fitted.
  wakePulseUntilMs = millis() + 500;
  digitalWrite(WAKE_PIN, HIGH);
  // 2) Wake-on-LAN magic packet to the laptop (primary laptop wake).
  bool wol = sendWol();
  // 3) MQTT WAKE message (notifies an awake host/broker to start camera+radios).
  char buf[160];
  protoBuildWake(buf, sizeof(buf), reason);
  Serial.println(buf);
  if (mqtt.connected()) mqtt.publish(topic("wake").c_str(), buf, false);
  Serial.printf("[wake] fired reason=%s wol=%s mqtt=%s gpio=pulse 500ms\n",
                reason, wol ? "sent" : "no", mqtt.connected() ? "sent" : "no");
}

// ---- command handling (shared by serial, MQTT, HTTP) ----
// Forward decl: config apply lives below (needs the radar parsers).
static void applySensorConfig();

static void applyConfigCommand(const HostCommand& c, bool* changed) {
  *changed = false;
  if (c.hasMaxRange) { alarmMachine.cfg.maxRangeCm = c.maxRangeCm; *changed = true; }
  if (c.hasAlarmDuration) { alarmMachine.cfg.alarmDurationS = c.alarmDurationS; *changed = true; }
  if (c.hasGrace) { alarmMachine.cfg.graceS = c.graceS; *changed = true; }
  if (c.hasMotionSens) { alarmMachine.cfg.motionSens = c.motionSens; *changed = true; }
  if (c.hasStaticSens) { alarmMachine.cfg.staticSens = c.staticSens; *changed = true; }
  if (c.hasDeepSleep) { alarmMachine.cfg.deepSleep = c.deepSleep; *changed = true; }
  if (c.hasCode) {
    strncpy(alarmMachine.cfg.disarmCode, c.code, sizeof(alarmMachine.cfg.disarmCode) - 1);
    alarmMachine.cfg.disarmCode[sizeof(alarmMachine.cfg.disarmCode) - 1] = '\0';
    *changed = true;
  }
  if (*changed && alarmConfigSanitize(alarmMachine.cfg)) {
    saveConfig();
    applySensorConfig();
  }
}

// Forward decl (used by applyConfigCommand).
static void applySensorConfig();

static void handleCommand(const HostCommand& c, bool fromSerial) {
  char buf[256];
  uint32_t nowMs = millis();
  switch (c.cmd) {
    case HostCmd::ARM: {
      bool ok = alarmMachine.arm(nowMs);
      rtcWasArmed = (alarmMachine.state != AlarmState::DISARMED);
      protoBuildSimple(buf, sizeof(buf), ok ? "ARMED" : "ERROR",
                       ok ? alarmMachine.stateName() : "already armed");
      if (fromSerial) Serial.println(buf);
      Serial.printf("[alarm] ARM %s -> %s\n", ok ? "ok" : "ignored", alarmMachine.stateName());
      break;
    }
    case HostCmd::DISARM: {
      bool ok = alarmMachine.disarm(c.hasCode ? c.code : "", nowMs);
      rtcWasArmed = false;
      if (ok) {
        protoBuildSimple(buf, sizeof(buf), "DISARMED", "DISARMED");
        if (fromSerial) Serial.println(buf);
      } else if (fromSerial) {
        protoBuildError(buf, sizeof(buf), "bad disarm code");
        Serial.println(buf);
      }
      Serial.printf("[alarm] DISARM %s -> %s\n", ok ? "ok" : "BAD CODE", alarmMachine.stateName());
      break;
    }
    case HostCmd::STATUS:
      if (fromSerial) sendStatusSerial();
      break;
    case HostCmd::CONFIG: {
      bool changed = false;
      applyConfigCommand(c, &changed);
      if (fromSerial) sendStatusSerial();
      Serial.printf("[config] updated=%s range=%ucm alarm=%us grace=%us deep=%s\n",
                    changed ? "yes" : "no", alarmMachine.cfg.maxRangeCm,
                    alarmMachine.cfg.alarmDurationS, alarmMachine.cfg.graceS,
                    alarmMachine.cfg.deepSleep ? "on" : "off");
      break;
    }
    case HostCmd::WAKE_TEST:
      fireWake("wake_test");
      break;
    case HostCmd::SLEEP:
      Serial.println("[sleep] host says sleep is OK");
      break;
    default:
      if (fromSerial) { protoBuildError(buf, sizeof(buf), "unknown command"); Serial.println(buf); }
      break;
  }
  publishStatus();
}

// ---- radar config apply (LD2410 only) ----
static void applySensorConfig() {
#if defined(SENSOR_LD2450)
  Serial.println("[radar] LD2450: module defaults kept (no UART config in this build)");
#else
  Serial.println("[radar] applying config to LD2410...");
  delay(200);
  while (Serial2.available()) Serial2.read();
  Ld2410Parser::enableConfig(Serial2);
  delay(100);
  Ld2410Parser::setMaxGates(Serial2, alarmMachine.cfg.maxGate, alarmMachine.cfg.maxGate,
                            alarmMachine.cfg.noTargetSecs);
  delay(100);
  Ld2410Parser::setSensitivityAll(Serial2, alarmMachine.cfg.motionSens, alarmMachine.cfg.staticSens);
  delay(100);
  Ld2410Parser::endConfig(Serial2);
  delay(100);
  while (Serial2.available()) Serial2.read();
  Serial.println("[radar] config applied");
#endif
}

// ---- MQTT ----
static void mqttCallback(char* topicStr, byte* payload, unsigned int len) {
  char line[512];
  size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
  memcpy(line, payload, n);
  line[n] = '\0';
  HostCommand c;
  if (protoParseCommand(line, &c)) handleCommand(c, false);
  else Serial.printf("[mqtt] ignored bad command on %s\n", topicStr);
}

static uint32_t lastMqttTryMs = 0;
static void mqttMaintain(uint32_t nowMs) {
  if (mqttHost.length() == 0 || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (nowMs - lastMqttTryMs < MQTT_RETRY_MS) return;
  lastMqttTryMs = nowMs;
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(mqttCallback);
  String cid = "carsentry-" + deviceId;
  if (mqtt.connect(cid.c_str())) {
    Serial.printf("[mqtt] connected to %s:%u\n", mqttHost.c_str(), mqttPort);
    mqtt.subscribe(topic("command").c_str());
    publishStatus();
  } else {
    Serial.printf("[mqtt] connect failed, rc=%d\n", mqtt.state());
  }
}

// ---- HTTP ----
static void handleStatusHttp() {
  Presence p = currentPresence();
  JsonDocument doc;
  doc["state"] = alarmMachine.stateName();
  doc["presence"] = p.present;
  doc["dist_cm"] = p.distCm;
  doc["sensor"] = SENSOR_NAME;
  doc["sensor_ok"] = p.sensorOk;
  doc["vbat_v"] = vbatV;
  doc["battery_cutoff"] = batteryCutoff;
  doc["grace_s"] = alarmMachine.graceRemainingS(millis());
  doc["alarm_s"] = alarmMachine.alarmRemainingS(millis());
  doc["alarm_cycles"] = alarmMachine.alarmCycles;
  doc["uptime_s"] = millis() / 1000;
  doc["wake_methods"] = "wol+mqtt+gpio";
  doc["laptop_mac_configured"] = laptopMacSet;
  String out;
  serializeJson(doc, out);
  http.send(200, "application/json", out);
}

static void handleConfigHttp(bool isPost) {
  if (!isPost) {
    JsonDocument doc;
    doc["max_range_cm"] = alarmMachine.cfg.maxRangeCm;
    doc["motion_sensitivity"] = alarmMachine.cfg.motionSens;
    doc["static_sensitivity"] = alarmMachine.cfg.staticSens;
    doc["max_distance_gate"] = alarmMachine.cfg.maxGate;
    doc["no_target_seconds"] = alarmMachine.cfg.noTargetSecs;
    doc["alarm_duration_s"] = alarmMachine.cfg.alarmDurationS;
    doc["grace_s"] = alarmMachine.cfg.graceS;
    doc["confirm_ms"] = alarmMachine.cfg.confirmMs;
    doc["deep_sleep"] = alarmMachine.cfg.deepSleep;
    doc["radar_out_wired"] = radarOutWired;
    doc["mqtt_host"] = mqttHost;
    doc["mqtt_port"] = mqttPort;
    doc["laptop_mac"] = laptopMacStr;
    doc["sensor"] = SENSOR_NAME;
    String out;
    serializeJson(doc, out);
    http.send(200, "application/json", out);
    return;
  }
  if (!http.hasArg("plain")) { http.send(400, "application/json", "{\"error\":\"empty body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, http.arg("plain"))) {
    http.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  // Build a JSON line the shared parser understands, so HTTP config uses
  // exactly the same validation as serial/MQTT.
  String line = "{\"cmd\":\"CONFIG\"";
  if (doc["max_range_cm"].is<int>()) { line += ",\"max_range_cm\":"; line += doc["max_range_cm"].as<int>(); }
  if (doc["alarm_duration_s"].is<int>()) { line += ",\"alarm_duration_s\":"; line += doc["alarm_duration_s"].as<int>(); }
  if (doc["grace_s"].is<int>()) { line += ",\"grace_s\":"; line += doc["grace_s"].as<int>(); }
  if (doc["motion_sensitivity"].is<int>()) { line += ",\"motion_sensitivity\":"; line += doc["motion_sensitivity"].as<int>(); }
  if (doc["static_sensitivity"].is<int>()) { line += ",\"static_sensitivity\":"; line += doc["static_sensitivity"].as<int>(); }
  if (doc["deep_sleep"].is<bool>()) { line += ",\"deep_sleep\":"; line += doc["deep_sleep"].as<bool>() ? "true" : "false"; }
  if (doc["disarm_code"].is<const char*>()) { line += ",\"disarm_code\":\""; line += doc["disarm_code"].as<const char*>(); line += "\""; }
  line += "}";
  HostCommand c;
  if (protoParseCommand(line.c_str(), &c)) {
    bool changed = false;
    applyConfigCommand(c, &changed);
  }
  if (doc["mqtt_host"].is<const char*>()) { mqttHost = doc["mqtt_host"].as<const char*>(); saveConfig(); }
  if (doc["mqtt_port"].is<int>()) { mqttPort = doc["mqtt_port"].as<int>(); saveConfig(); }
  if (doc["laptop_mac"].is<const char*>()) {
    laptopMacStr = doc["laptop_mac"].as<const char*>();
    laptopMacSet = protoParseMac(laptopMacStr.c_str(), laptopMac);
    saveConfig();
  }
  if (doc["radar_out_wired"].is<bool>()) { radarOutWired = doc["radar_out_wired"].as<bool>(); saveConfig(); }
  handleConfigHttp(false);
}

static void handleArmHttp() {
  HostCommand c;
  c.cmd = HostCmd::ARM;
  handleCommand(c, false);
  handleStatusHttp();
}
static void handleDisarmHttp() {
  if (!http.hasArg("plain")) { http.send(400, "application/json", "{\"error\":\"empty body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, http.arg("plain"))) {
    http.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  HostCommand c;
  c.cmd = HostCmd::DISARM;
  const char* code = doc["code"].is<const char*>() ? doc["code"].as<const char*>() : "";
  strncpy(c.code, code, sizeof(c.code) - 1);
  c.hasCode = true;
  bool ok = alarmMachine.disarm(c.code, millis());
  rtcWasArmed = false;
  if (!ok) { http.send(403, "application/json", "{\"error\":\"bad disarm code\"}"); return; }
  handleStatusHttp();
}
static void handleWakeTestHttp() {
  fireWake("wake_test");
  http.send(200, "application/json", "{\"status\":\"wake fired\"}");
}
static void handleVersionHttp() {
  JsonDocument doc;
  doc["firmware_version"] = FW_VERSION;
  doc["build_date"] = FW_BUILD_DATE;
  doc["sensor"] = SENSOR_NAME;
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["boot_count"] = rtcBootCount;
  String out;
  serializeJson(doc, out);
  http.send(200, "application/json", out);
}

// OTA (same honest limits as the mmWave build: a failed upload aborts
// before commit; power loss mid-write after commit may need a USB re-flash).
static bool otaActive = false, otaOk = false;
static String otaError = "";
static uint32_t otaReceived = 0, otaRebootAt = 0;
static void handleOtaUpload() {
  HTTPUpload& up = http.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaActive = true; otaOk = false; otaError = ""; otaReceived = 0;
    if (!up.filename.endsWith(".bin")) { otaError = "file must be a .bin firmware image"; otaActive = false; return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaError = String("Update.begin failed: ") + Update.errorString();
      otaActive = false;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError.length()) return;
    if (otaReceived == 0 && up.buf[0] != 0xE9) {
      otaError = "not an ESP32 firmware image (bad magic byte)";
      Update.abort(); otaActive = false; return;
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaError = String("write failed: ") + Update.errorString();
      Update.abort(); otaActive = false; return;
    }
    otaReceived += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    otaActive = false;
    if (otaError.length()) return;
    if (Update.end(true)) { otaOk = true; otaRebootAt = millis() + 1000; }
    else otaError = String("finalize failed: ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaActive = false;
    if (!otaError.length()) otaError = "upload interrupted";
  }
}
static void handleOtaPost() {
  JsonDocument doc;
  doc["status"] = otaOk ? "ok" : "error";
  doc["message"] = otaOk ? "firmware written, rebooting"
                         : (otaError.length() ? otaError : "upload failed");
  doc["received_bytes"] = otaReceived;
  String out;
  serializeJson(doc, out);
  http.send(otaOk ? 200 : 400, "application/json", out);
}

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Car Sentry</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}h1{font-size:20px}.card{background:#1e1e1e;border-radius:10px;padding:14px;margin:10px 0}#state{font-size:28px;font-weight:bold}input,button{font-size:16px;padding:8px;margin:4px 0;width:100%;box-sizing:border-box}button{background:#d9822b;border:0;border-radius:6px;color:#111;font-weight:bold}.danger{background:#c0392b;color:#fff}</style></head>
<body><h1>Car Sentry</h1>
<div class="card"><div id="state">—</div><div id="detail">connecting…</div><div id="vbat"></div></div>
<div class="card"><h3>Alarm</h3><button onclick="arm()">ARM (30 s grace)</button><input id="code" type="password" placeholder="Disarm code"><button class="danger" onclick="disarm()">DISARM</button><button onclick="wake()">WAKE TEST (laptop)</button></div>
<div class="card"><h3>WiFi setup</h3><form method="POST" action="/wifi"><input name="ssid" placeholder="WiFi SSID (same network as laptop)"><input name="pass" type="password" placeholder="WiFi password"><button>Save &amp; connect</button></form></div>
<div class="card"><h3>Settings</h3><form id="cfg">Max range cm <input name="max_range_cm" type="number" value="300">Alarm seconds <input name="alarm_duration_s" type="number" value="30">Grace seconds <input name="grace_s" type="number" value="30">Motion sens <input name="motion_sensitivity" type="number" value="50">Static sens <input name="static_sensitivity" type="number" value="50">Laptop MAC (WoL) <input name="laptop_mac" placeholder="AA:BB:CC:DD:EE:FF">MQTT host <input name="mqtt_host" placeholder="e.g. 192.168.1.10"><button type="submit">Apply</button></form><p>Set the laptop MAC for Wake-on-LAN, and the MQTT host if the laptop runs a broker. Change the disarm code over serial: {"cmd":"CONFIG","disarm_code":"NEWCODE"}</p></div>
<script>
async function arm(){await fetch('/api/arm',{method:'POST'});load();}
async function disarm(){await fetch('/api/disarm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:document.getElementById('code').value})});load();}
async function wake(){await fetch('/api/wake-test',{method:'POST'});alert('wake fired — check the laptop');}
async function load(){let d=await (await fetch('/api/status')).json();document.getElementById('state').textContent=d.state;
document.getElementById('detail').textContent='presence '+d.presence+' · '+d.dist_cm+' cm · sensor '+(d.sensor_ok?'OK':'NO FRAME')+' · grace '+d.grace_s+'s · alarm '+d.alarm_s+'s';
document.getElementById('vbat').textContent='car battery '+d.vbat_v+' V'+(d.battery_cutoff?' · CUTOFF — sleeping to save battery':'');}
document.getElementById('cfg').onsubmit=async e=>{e.preventDefault();let o={};new FormData(e.target).forEach((v,k)=>{if(v!=='')o[k]=isNaN(+v)?v:+v;});
await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});alert('saved');load();};
load();setInterval(load,2000);
</script></body></html>)HTML";

static void handleRoot() { http.send_P(200, "text/html", INDEX_HTML); }
static void handleWifiPost() {
  String ssid = http.arg("ssid");
  String pass = http.arg("pass");
  if (ssid.length() == 0) { http.send(400, "text/plain", "SSID required"); return; }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  staSsid = ssid; staPass = pass;
  http.send(200, "text/html", "<p>Saved. Connecting — the node will join your WiFi shortly.</p>");
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// ---- serial input ----
static String serialLine;
static void handleSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (serialLine.length() > 0) {
        HostCommand c;
        if (protoParseCommand(serialLine.c_str(), &c)) handleCommand(c, true);
        else {
          char buf[128];
          protoBuildError(buf, sizeof(buf), "unknown command");
          Serial.println(buf);
        }
        serialLine = "";
      }
    } else if (serialLine.length() < 512) {
      serialLine += ch;
    }
  }
}

// ---- disarm button ----
static uint32_t btnDownSinceMs = 0;
static bool btnWasDown = false;
static void handleButton(uint32_t nowMs) {
  bool down = digitalRead(DISARM_BUTTON_PIN) == LOW;
  if (down && !btnWasDown) btnDownSinceMs = nowMs;  // press start
  if (!down && btnWasDown) {
    // Release: short press disarms from GRACE/ALARMING; long hold (2 s)
    // disarms from ARMED too (owner standing at the car).
    bool longHold = (nowMs - btnDownSinceMs >= 2000);
    if (alarmMachine.state == AlarmState::GRACE || alarmMachine.state == AlarmState::ALARMING ||
        (alarmMachine.state == AlarmState::ARMED && longHold) ||
        alarmMachine.state == AlarmState::LATCHED) {
      if (alarmMachine.disarm("", nowMs, true)) {
        rtcWasArmed = false;
        Serial.println("[alarm] DISARM by button");
        publishStatus();
      }
    }
  }
  btnWasDown = down;
}

// ---- sleep ----
static void enterLightSleep(uint32_t ms) {
  Serial.printf("[sleep] light sleep %lu ms (scan cycle)\n", (unsigned long)ms);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_light_sleep_start();
}

static void enterDeepSleep() {
  Serial.println("[sleep] deep sleep — wake on radar OUT (GPIO33 HIGH) or 10 min timer");
  Serial.flush();
  if (radarOutWired) esp_sleep_enable_ext0_wakeup((gpio_num_t)RADAR_OUT_PIN, 1);
  esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  rtcBootCount++;
  Serial.printf("\n[boot] Car Sentry %s sensor=%s boot#%lu\n", FW_VERSION,
                SENSOR_NAME, (unsigned long)rtcBootCount);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(STROBE_PIN, OUTPUT);
  pinMode(WAKE_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(STROBE_PIN, LOW);
  digitalWrite(WAKE_PIN, LOW);
  pinMode(DISARM_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RADAR_OUT_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  loadConfig();

  // Restore armed state after a deep-sleep wake: the owner armed it, the
  // node slept, the radar OUT pin woke it — skip grace, go straight to ARMED.
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (rtcWasArmed && cause == ESP_SLEEP_WAKEUP_EXT0) {
    alarmMachine.state = AlarmState::ARMED;
    alarmMachine.stateEnteredMs = millis();
    Serial.println("[boot] woken by radar OUT while armed — straight to ARMED");
  } else if (rtcWasArmed && cause == ESP_SLEEP_WAKEUP_TIMER) {
    alarmMachine.state = AlarmState::ARMED;
    alarmMachine.stateEnteredMs = millis();
    Serial.println("[boot] timer wake while armed — back to ARMED (voltage re-check)");
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  deviceId = String(suffix);

  Serial2.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  delay(1000);
  applySensorConfig();

  WiFi.mode(WIFI_AP_STA);
  apSsid = String(AP_SSID_PREFIX) + suffix;
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  Serial.printf("[wifi] AP %s @ %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  if (staSsid.length() > 0) {
    Serial.printf("[wifi] connecting STA %s ...\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addServiceTxt("http", "tcp", "sensor", SENSOR_NAME);
    Serial.println("[mdns] carsentry.local");
  }
  dns.start(53, "*", WiFi.softAPIP());

  http.on("/", HTTP_GET, handleRoot);
  http.on("/api/status", HTTP_GET, handleStatusHttp);
  http.on("/api/config", HTTP_GET, []() { handleConfigHttp(false); });
  http.on("/api/config", HTTP_POST, []() { handleConfigHttp(true); });
  http.on("/api/arm", HTTP_POST, handleArmHttp);
  http.on("/api/disarm", HTTP_POST, handleDisarmHttp);
  http.on("/api/wake-test", HTTP_POST, handleWakeTestHttp);
  http.on("/api/version", HTTP_GET, handleVersionHttp);
  http.on("/api/ota/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["state"] = otaActive ? "uploading" : (otaOk ? "ok" : (otaError.length() ? "error" : "idle"));
    doc["received_bytes"] = otaReceived;
    doc["error"] = otaError;
    String out; serializeJson(doc, out);
    http.send(200, "application/json", out);
  });
  http.on("/api/ota", HTTP_POST, handleOtaPost, handleOtaUpload);
  http.on("/wifi", HTTP_POST, handleWifiPost);
  http.on("/generate_204", HTTP_GET, []() { http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/"); http.send(302, "text/plain", ""); });
  http.onNotFound([]() {
    if (http.uri().startsWith("/api/")) { http.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
    http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    http.send(302, "text/plain", "");
  });
  http.begin();
  Serial.printf("[http] :%d  state=%s vbat=%.1f V\n", HTTP_PORT, alarmMachine.stateName(), readVbat());
  Serial.println("[ready] serial JSON: {\"cmd\":\"ARM\"} {\"cmd\":\"DISARM\",\"code\":\"1234\"} {\"cmd\":\"STATUS\"} {\"cmd\":\"WAKE_TEST\"}");
}

void loop() {
  uint32_t nowMs = millis();
  if (otaRebootAt && nowMs >= otaRebootAt) {
    Serial.println("[ota] rebooting now");
    delay(100);
    ESP.restart();
  }
  // Radar frames.
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    if (radar.feed(b)) {
      Presence p = currentPresence();
      Serial.printf("[radar] present=%d dist=%d cm sensor_ok=%d\n",
                    p.present ? 1 : 0, p.distCm, p.sensorOk ? 1 : 0);
    }
  }
  handleSerial();
  handleButton(nowMs);
  mqttMaintain(nowMs);
  http.handleClient();
  dns.processNextRequest();

  // Battery check every 10 s.
  static uint32_t lastBattMs = 0;
  if (nowMs - lastBattMs >= 10000) {
    lastBattMs = nowMs;
    checkBattery(nowMs);
    if (batteryCutoff)
      Serial.printf("[power] CUTOFF latched: vbat=%.1f V — sleeping to save battery\n", vbatV);
  }

  // Alarm tick: presence -> (maybe) alarm + wake.
  Presence p = currentPresence();
  AlarmEvent ev = alarmMachine.tick(p.present, p.distCm, nowMs);
  if (ev.alarmStarted) {
    Serial.printf("[alarm] TRIGGER dist=%d cm cycles=%u — firing wake\n",
                  p.distCm, alarmMachine.alarmCycles);
    publishAlert(p.distCm);
    fireWake("person_detected");
  }
  if (ev.alarmEnded) {
    Serial.printf("[alarm] siren burst ended -> %s\n", alarmMachine.stateName());
    publishStatus();
  }
  if (ev.stateChanged) {
    Serial.printf("[alarm] state -> %s\n", alarmMachine.stateName());
    rtcWasArmed = (alarmMachine.state != AlarmState::DISARMED);
    publishStatus();
  }
  driveOutputs(nowMs);

  // Periodic STATUS publish (MQTT) every 5 s.
  static uint32_t lastPubMs = 0;
  if (nowMs - lastPubMs >= 5000) {
    lastPubMs = nowMs;
    publishStatus();
  }

  // Sleep: only when nothing needs the CPU right now. Never while
  // ALARMING/GRACE, never during OTA, never while the wake pulse is active.
  if (!otaActive && nowMs >= wakePulseUntilMs) {
    SleepMode sm = sleepDecision(alarmMachine, nowMs, radarOutWired, batteryCutoff);
    if (sm == SleepMode::DEEP_SLEEP) {
      rtcWasArmed = (alarmMachine.state != AlarmState::DISARMED);
      enterDeepSleep();  // does not return (reboots on wake)
    } else if (sm == SleepMode::LIGHT_SLEEP) {
      enterLightSleep(alarmMachine.cfg.lightSleepCycleMs);
    }
  }
  delay(20);  // yield; keeps the loop at ~50 Hz when awake
}
