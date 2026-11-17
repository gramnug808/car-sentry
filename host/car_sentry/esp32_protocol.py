"""Host side of the ESP32 protocol — serial + MQTT/HTTP.

This package contains NO ESP32 firmware. The ESP32 is a separate
standalone alarm/sensor node (see ../esp32/). The host only speaks
to it over this simple text protocol.

Wire format (serial line, \\n-terminated, or MQTT payload):
    ARM
    DISARM
    STATUS
    STATUS {"armed":true,"battery":82,"motion":false}
    ALERT MOTION <free-text detail>
    ALERT PERSON <free-text detail>
    ALERT TAMPER <free-text detail>
    WAKE <reason>            e.g. WAKE MOTION
    PING / PONG

MQTT topics (broker configurable, default localhost:1883):
    car-sentry/esp32/alert    host subscribes  (ALERT/WAKE as above, or JSON)
    car-sentry/esp32/status   host subscribes  (STATUS ...)
    car-sentry/host/cmd       host publishes   (ARM / DISARM / STATUS)
    car-sentry/host/wake      host publishes   (WAKE ack — informational)

JSON payloads are also accepted: {"type":"ALERT","alert":"MOTION",
"detail":"..."} / {"type":"WAKE","reason":"MOTION"} /
{"type":"STATUS","armed":true,...}. The text form is canonical.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field

TOPIC_ALERT = "car-sentry/esp32/alert"
TOPIC_STATUS = "car-sentry/esp32/status"
TOPIC_CMD = "car-sentry/host/cmd"

VALID_ALERTS = {"MOTION", "PERSON", "TAMPER", "SOUND", "VIBRATION"}


@dataclass
class Esp32Message:
    kind: str                    # ARM|DISARM|STATUS|ALERT|WAKE|PING|PONG|UNKNOWN
    alert: str = ""              # for ALERT: MOTION/PERSON/...
    detail: str = ""
    reason: str = ""             # for WAKE
    data: dict = field(default_factory=dict)
    raw: str = ""

    def as_dict(self):
        return {"kind": self.kind, "alert": self.alert, "detail": self.detail,
                "reason": self.reason, "data": self.data, "raw": self.raw}


def build_arm() -> str:
    return "ARM"


def build_disarm() -> str:
    return "DISARM"


def build_status_request() -> str:
    return "STATUS"


def parse_message(line: str) -> Esp32Message | None:
    """Parse one protocol line (text or JSON). None if blank."""
    raw = (line or "").strip()
    if not raw:
        return None
    # JSON form
    if raw.startswith("{"):
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            return Esp32Message("UNKNOWN", raw=raw)
        kind = str(obj.get("type", "")).upper()
        if kind == "ALERT":
            return Esp32Message("ALERT", alert=str(obj.get("alert", "")).upper(),
                                detail=str(obj.get("detail", "")), data=obj, raw=raw)
        if kind == "WAKE":
            return Esp32Message("WAKE", reason=str(obj.get("reason", "")), data=obj, raw=raw)
        if kind == "STATUS":
            return Esp32Message("STATUS", data=obj, raw=raw)
        if kind in ("ARM", "DISARM", "PING", "PONG"):
            return Esp32Message(kind, data=obj, raw=raw)
        return Esp32Message("UNKNOWN", data=obj, raw=raw)
    # Text form
    parts = raw.split(None, 2)
    verb = parts[0].upper()
    if verb in ("ARM", "DISARM", "PING", "PONG"):
        return Esp32Message(verb, raw=raw)
    if verb == "STATUS":
        data = {}
        if len(parts) > 1:
            try:
                data = json.loads(" ".join(parts[1:]))
            except json.JSONDecodeError:
                data = {"detail": " ".join(parts[1:])}
        return Esp32Message("STATUS", data=data, raw=raw)
    if verb == "ALERT" and len(parts) >= 2:
        return Esp32Message("ALERT", alert=parts[1].upper(),
                            detail=parts[2] if len(parts) > 2 else "", raw=raw)
    if verb == "WAKE":
        return Esp32Message("WAKE", reason=parts[1] if len(parts) > 1 else "", raw=raw)
    return Esp32Message("UNKNOWN", raw=raw)


def parse_mqtt(topic: str, payload: str) -> Esp32Message | None:
    """MQTT wrapper: topic selects the expected kind, payload is parsed as a message."""
    msg = parse_message(payload)
    if msg is None:
        return None
    if topic == TOPIC_ALERT and msg.kind not in ("ALERT", "WAKE"):
        # A bare alert payload like "MOTION front" on the alert topic.
        parts = payload.strip().split(None, 1)
        if parts and parts[0].upper() in VALID_ALERTS:
            return Esp32Message("ALERT", alert=parts[0].upper(),
                                detail=parts[1] if len(parts) > 1 else "", raw=payload)
    return msg


def serial_device_present(device: str = "/dev/ttyUSB0") -> tuple[bool, str]:
    import os
    if os.path.exists(device):
        return True, f"{device} present"
    import glob
    cands = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if cands:
        return True, f"{device} not present, but found: {', '.join(cands)}"
    return False, "no /dev/ttyUSB* or /dev/ttyACM* — ESP32 not connected over USB serial"
