# KnobX1 → ESPHome USB host component

Reads HID consumer-control events (volume up / down / mute) from a **Binepad
KnobX1** plugged into an **M5Stack AtomS3-Lite** running as a USB host, and
exposes them as ESPHome `binary_sensor`s.

See `knobx1-esphome-handoff.md` for the hardware investigation (USB descriptor
dump, captured report bytes).

## Layout

```
components/usb_knob/
  __init__.py        # usb_knob: platform config (vid/pid/interface/endpoint/report_id)
  binary_sensor.py   # binary_sensor platform, key: name or raw usage code
  usb_knob.h/.cpp    # USBClient subclass
knobx1.yaml          # example device config
```

## Config

```yaml
usb_host:

usb_knob:
  id: knob1          # vid/pid/interface/endpoint/report_id default to KnobX1 values

binary_sensor:
  - platform: usb_knob
    usb_knob_id: knob1
    key: volume_up   # or volume_down, mute, play_pause, stop, next_track,
                     # previous_track, or a raw usage code like 0x00E9
    name: Volume Up
```

`report_id: 0x00` means "device sends no report ID prefix" — the first two
bytes are then taken as the usage code.

## How it works against the real `usb_host` API

Facts confirmed against `esphome/components/usb_host` on `dev` (2026.8):

- `usb_host::USBClient` **is already a `Component`** — subclass it alone, do not
  also inherit `Component`. Its constructor takes `(vid, pid)` and it does the
  device open, VID/PID match, and descriptor fetch for you.
- Python side: `usb_device_schema(cls, vid, pid)` builds the schema and
  `register_usb_client(config)` constructs the variable — both exported from
  `esphome.components.usb_host`.
- Override `on_connected()` to claim the interface. The base class does **not**
  claim anything; call `usb_host_interface_claim(this->handle_,
  this->device_handle_, intf, 0)` yourself (both handles are `protected`).
- Interrupt IN data comes from `transfer_in(ep_address, callback, length)`.
  It is one-shot: the callback must resubmit to keep receiving.
- **The transfer callback runs in the USB task, not the main loop.** Publishing
  a `binary_sensor` state there is not safe, so `handle_report_()` only decodes
  and pushes onto a `LockFreeQueue`; `loop()` drains it and publishes. This is
  the pattern `usb_uart` uses, and the main correction to the original sketch.
- `loop()` should call `process_usb_events_()` (not `USBClient::loop()`) and
  combine its result with its own work check before `disable_loop()`.
- `usb_host:` handles the sdkconfig itself (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`,
  and the IDF 6.0+ `espressif/usb` managed component). No manual
  `sdkconfig_options` are needed — the handoff's `CONFIG_USB_HOST_ENABLE: y` is
  not a real option and should be dropped.
- Board id: `m5stack-atoms3` (variant `esp32s3`). There is no separate
  AtomS3-Lite entry in ESPHome's board list; the Lite is the same ESP32-S3FN8
  module and pinout without the LCD.

Verified: `esphome config` passes and `esphome compile` builds clean (ESPHome
2026.8.1, ESP-IDF 5.5.5) with no warnings from `usb_knob`.

## Lighting

The KnobX1 has two WS2812 underglow LEDs. Per QMK's `keyboards/binepad/knobx1/keyboard.json`
they are **`rgblight`** (not `rgb_matrix`), `led_count: 2`, driver ws2812 on pin C13.
The four layer indicators are separate single-colour GPIO LEDs (A3-A6) and are not
reachable this way.

They are driven over the **VIAL raw HID interface** (interface 1, EP 0x03 OUT /
0x82 IN), using VIA's lighting commands:

| Command | VIA >= 11 | VIA < 11 |
|---|---|---|
| set colour | `07 02 04 <hue> <sat>` | `07 83 <hue> <sat>` |
| set brightness | `07 02 01 <val>` | `07 80 <val>` |
| set effect | `07 02 02 <effect>` | `07 81 <effect>` |

On connect the component sends `01` (get protocol version), picks the layout from
the reply, and sets effect 1 (static light) so an animation can't overwrite the hue.
Nothing is written to the knob's EEPROM — VIA only persists on an explicit
`id_custom_save` (`0x09`), which is never sent.

`id(knob1).set_hsv(hue, sat, val)` is callable from any lambda; hue/sat/val are
QMK's 0-255 scale (hue 0 = red, 85 = green, 170 = blue).

Claiming the raw interface is best-effort: if it fails, lighting is skipped and a
warning is logged, but volume reporting keeps working. `lighting: false` disables it.

## Still to verify on hardware

- That interface 2 / endpoint 0x84 is what the ESP-IDF host enumerates (the
  numbering came from Linux). `on_connected()` logs every endpoint on the
  claimed interface at DEBUG, so `logger: level: DEBUG` will show the truth;
  override `interface:` / `endpoint:` in YAML if they differ.
- Whether the knob needs a HID SET_PROTOCOL / SET_IDLE request. Neither
  ESPHome's `usb_host` nor NonaSuomy's `usb_hidx` sends one, and interface 2 is
  not a boot interface, so it should report by default.
