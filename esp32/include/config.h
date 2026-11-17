// Car Sentry — firmware configuration (include/config.h)
// Standalone alarm/sensor node. Sensor default: HLK-LD2410 24 GHz mmWave
// human-presence radar, UART, 256000 baud. LD2450 is a compile-time option
// (-DSENSOR_LD2450). Only ONE sensor define may be active.
// No wardriving / SDR / LPR / signal-intel code lives in this firmware.
#pragma once

// ---- Radar UART ----
// LD2410 TX -> ESP32 GPIO16 (RX2); LD2410 RX -> ESP32 GPIO17 (TX2).
// LD2410 needs 5 V on VCC — power it from ESP32 VIN (5 V), NOT 3V3.
#ifndef RADAR_UART_NUM
#define RADAR_UART_NUM 2
#endif
#ifndef RADAR_TX_PIN
#define RADAR_TX_PIN 17  // ESP32 TX -> LD2410 RX (OT)
#endif
#ifndef RADAR_RX_PIN
#define RADAR_RX_PIN 16  // ESP32 RX <- LD2410 TX (TX)
#endif
#ifndef RADAR_BAUD
#define RADAR_BAUD 256000
#endif
// LD2410 OUT pin (goes HIGH on presence) -> ESP32 GPIO33 (RTC pin).
// Used ONLY as the deep-sleep wake interrupt. Leave unconnected if you do
// not use deep sleep; timer-wake scan cycles are the fallback.
#define RADAR_OUT_PIN 33

// ---- Alarm outputs (all 3V3 logic — drive siren/relay via MOSFET/relay) ----
#define BUZZER_PIN 26   // buzzer/siren driver input (HIGH = sounding)
#define STROBE_PIN 27   // flash / trigger output (HIGH = on, e.g. relay, LED strobe)
#define WAKE_PIN 25     // host-wake pulse output (500 ms HIGH on alarm, see README)
// ---- Disarm button ----
// Momentary push-button between GPIO32 and GND (INPUT_PULLUP, active LOW).
// A press while ARMING or ALARMING disarms. Hold 2 s also disarms from ARMED.
#define DISARM_BUTTON_PIN 32

// ---- Car power sense ----
// 12 V car rail -> voltage divider -> GPIO34 (ADC1, input-only).
// Divider 100 k (top) / 27 k (bottom): 15.0 V in -> ~3.19 V at the pin.
// ADC reading is scaled back in code (see VIN_DIVIDER_RATIO).
#define VIN_SENSE_PIN 34
#define VIN_DIVIDER_RATIO 4.7037f  // (100k + 27k) / 27k
#define VIN_CUTOFF_V 11.5f         // below this, stop alarming, deep-sleep to save battery
#define VIN_RECOVER_V 12.0f        // re-enable above this
#define VIN_CUTOFF_HOLD_S 60       // must stay below cutoff this long before cutoff trips

// ---- WiFi ----
#define AP_SSID_PREFIX "CarSentry-"
#define AP_PASSWORD ""  // open AP on purpose: setup only
#define MDNS_NAME "carsentry"  // reachable as carsentry.local

// ---- Servers ----
#define HTTP_PORT 80
#define MQTT_PORT 1883
// MQTT topics are carsentry/<device-id>/... ; device-id = last 4 MAC hex chars.

// ---- Alarm defaults (stored in NVS, changeable via serial / HTTP / MQTT) ----
// Near-vehicle range: person must be within this distance to trigger.
#define DEFAULT_MAX_RANGE_CM 300    // 3.0 m; tune 200-300 cm for near-vehicle
#define DEFAULT_MOTION_SENS 50      // 0..100 (LD2410 gate sensitivity)
#define DEFAULT_STATIC_SENS 50      // 0..100
#define DEFAULT_MAX_GATE 4          // LD2410 gates are 0.75 m each: gate 4 = 3.0 m
#define DEFAULT_NO_TARGET_SECS 2
#define DEFAULT_ALARM_DURATION_S 30  // how long the siren sounds per trigger
#define DEFAULT_GRACE_S 30            // disarm grace period after arming
#define DEFAULT_CONFIRM_MS 1000       // presence must persist this long to trigger
#define DEFAULT_DISARM_CODE "1234"    // CHANGE THIS on first setup (serial/HTTP)
#define DEFAULT_DEEP_SLEEP false      // off by default: light-sleep scan cycles instead
#define DEFAULT_LIGHT_SLEEP_CYCLE_MS 2000  // armed idle: sleep 2 s, scan 0.5 s
#define DEFAULT_SCAN_MS 500

// How long without a radar frame before the sensor is reported "not OK".
#define SENSOR_TIMEOUT_MS 2000
// MQTT reconnect interval.
#define MQTT_RETRY_MS 5000
