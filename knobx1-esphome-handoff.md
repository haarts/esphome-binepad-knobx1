# KnobX1 → ESPHome USB Host Component — Handoff Notes

## Goal
Build a custom ESPHome component that reads volume/mute events from a Binepad
KnobX1 (USB HID) plugged into an ESP32-S3's USB-OTG host port, and exposes
them as ESPHome binary_sensors so they can trigger Home Assistant automations
that call Music Assistant `media_player.volume_up` / `volume_down` / `mute`.

## Hardware
- Board: M5Stack AtomS3-Lite (ESP32-S3FN8, 8MB flash, WiFi)
- Wiring: KnobX1 → USB-C cable → Atom's USB-C port (host mode)
- Power: injected separately via the board's GND/PWR header pins (bottom
  breakout), NOT via the USB-C port — that port is dedicated to the knob.
- Framework: must use ESP-IDF (not Arduino) — USB Host support requires it.

## KnobX1 identity
- lsusb: `Bus 005 Device 003: ID 5831:4249 Binepad KnobX1`
- VID: `0x5831`, PID: `0x4249`
- iSerial: `vial:f64c2b3c` (QMK/VIAL-based firmware)

## USB descriptor (relevant interfaces)
Full `lsusb -v -d 5831:4249` output showed 5 interfaces total (keyboard boot,
VIAL raw HID, extrakeys, audio control, MIDI streaming). Only one matters here:

| Interface | Endpoint | Packet size | Purpose |
|---|---|---|---|
| 0 | 0x81 IN | 8 bytes | Boot Interface Keyboard (standard keycodes, NOT used here) |
| 1 | 0x82 IN / 0x03 OUT | 32 bytes | VIAL raw HID config channel (NOT used here) |
| **2** | **0x84 IN** | **32 bytes** | **Extrakeys interface — this carries Consumer Control reports (target)** |
| 3+4 | — | — | USB Audio Control + MIDI Streaming (not relevant) |

Report descriptor bytes were unavailable via `lsusb -v` (permissions) but the
report structure was captured empirically instead — see below.

## Captured report format (via `usbhid-dump -a 5:3 -e stream`)
3-byte reports, little-endian 16-bit usage code after a report-ID byte:

```
04 E9 00   → Volume Up  (usage 0x00E9), press
04 00 00   → release (any key)
04 EA 00   → Volume Down (usage 0x00EA), press
04 00 00   → release
04 E2 00   → Mute (usage 0x00E2), press
04 00 00   → release
```

- Byte 0: report ID, always `0x04`
- Bytes 1-2: HID Consumer Control usage code, little-endian (`E9 00` = 0x00E9)
- `usage == 0` means "key released" (no code held down)
- These are standard HID Usage Page 0x0C (Consumer) codes:
  - `0x00E9` = Volume Increment
  - `0x00EA` = Volume Decrement
  - `0x00E2` = Mute

This is why the KnobX1 works natively on Linux/Mac with zero drivers — fully
standard consumer-control usage codes, no vendor-specific parsing needed.

## Sketch component (drafted, needs verification against real usb_host API)

`components/usb_knob/__init__.py`:
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import usb_host
from esphome.const import CONF_ID

DEPENDENCIES = ["usb_host"]

usb_knob_ns = cg.esphome_ns.namespace("usb_knob")
UsbKnob = usb_knob_ns.class_("UsbKnob", usb_host.USBClient, cg.Component)

CONF_VID = "vid"
CONF_PID = "pid"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UsbKnob),
    cv.Optional(CONF_VID, default=0x5831): cv.hex_uint16_t,
    cv.Optional(CONF_PID, default=0x4249): cv.hex_uint16_t,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
```

`components/usb_knob/binary_sensor.py`:
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from . import UsbKnob

CONF_USB_KNOB_ID = "usb_knob_id"
CONF_KEY = "key"

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend({
    cv.GenerateID(CONF_USB_KNOB_ID): cv.use_id(UsbKnob),
    cv.Required(CONF_KEY): cv.hex_uint16_t,
})

async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_KNOB_ID])
    cg.add(parent.register_key_sensor(config[CONF_KEY], var))
```

`components/usb_knob/usb_knob.h`:
```cpp
#pragma once
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/usb_host/usb_host.h"  // CONFIRM exact path/class name
#include <map>

namespace esphome {
namespace usb_knob {

class UsbKnob : public usb_host::USBClient, public Component {
 public:
  UsbKnob(uint16_t vid, uint16_t pid) : vid_(vid), pid_(pid) {}
  void setup() override;
  void register_key_sensor(uint16_t usage_code, binary_sensor::BinarySensor *sensor) {
    sensors_[usage_code] = sensor;
  }

 protected:
  void on_report(const uint8_t *data, size_t len);  // hook into interrupt IN transfer callback

  uint16_t vid_, pid_;
  std::map<uint16_t, binary_sensor::BinarySensor *> sensors_;
  uint16_t active_usage_{0};
};

}  // namespace usb_knob
}  // namespace esphome
```

`components/usb_knob/usb_knob.cpp` (parsing logic — derived directly from the
captured bytes above, should be correct as-is):
```cpp
#include "usb_knob.h"

namespace esphome {
namespace usb_knob {

static const uint8_t REPORT_ID = 0x04;

void UsbKnob::on_report(const uint8_t *data, size_t len) {
  if (len < 3 || data[0] != REPORT_ID) return;

  uint16_t usage = data[1] | (data[2] << 8);
  if (usage == active_usage_) return;

  if (active_usage_ != 0) {
    auto it = sensors_.find(active_usage_);
    if (it != sensors_.end()) it->second->publish_state(false);
  }
  if (usage != 0) {
    auto it = sensors_.find(usage);
    if (it != sensors_.end()) it->second->publish_state(true);
  }
  active_usage_ = usage;
}

}  // namespace usb_knob
}  // namespace esphome
```

Example YAML:
```yaml
esp32:
  board: esp32-s3-devkitc-1  # confirm exact board id for AtomS3-Lite
  variant: esp32s3
  framework:
    type: esp-idf
  sdkconfig_options:
    CONFIG_USB_HOST_ENABLE: y

usb_host:

usb_knob:
  id: knob1
  vid: 0x5831
  pid: 0x4249

binary_sensor:
  - platform: usb_knob
    usb_knob_id: knob1
    name: "Volume Up"
    key: 0x00E9
  - platform: usb_knob
    usb_knob_id: knob1
    name: "Volume Down"
    key: 0x00EA
  - platform: usb_knob
    usb_knob_id: knob1
    name: "Mute"
    key: 0x00E2
```

## Known gaps to resolve in Claude Code
1. **Verify the real `usb_host::USBClient` base class API** in the official
   ESPHome repo (`esphome/components/usb_host/usb_host.h` and `.cpp`) —
   confirm actual class/method names, how to claim interface 2 specifically,
   how to open endpoint 0x84, and how interrupt IN transfers/callbacks are
   wired up. The sketch above is structurally plausible but unverified
   against real API.
2. Reference implementation to compare against: NonaSuomy's fork,
   `https://github.com/NonaSuomy/esphome/tree/hidx-testing-002/esphome/components/usb_hidx`
   — has working USB HID host connection/transfer boilerplate, useful as a
   pattern even though we're not using it wholesale.
3. Confirm exact ESPHome `board:` identifier for M5Stack AtomS3-Lite.
4. Once binary_sensors fire correctly, wire `on_press`/`on_release` to
   `mqtt.publish` or a direct `homeassistant.service` call targeting the
   correct Music Assistant `media_player` entity for that room.

## End goal / broader project context
Scatter several of these knob+Atom units around the farmhouse, each
controlling volume/mute for the Music Assistant player in that room. Power
via 5V/GND header injection (not the USB port, not battery). Cost-conscious:
this ESP32-S3 route was chosen specifically over pricier SBC+PoE options
(NanoPi NEO3 Plus, NanoPi Zero2) as disproportionate for "just send volume
commands to Music Assistant."
