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

## Status

Verified on hardware (AtomS3-Lite + KnobX1, ESPHome 2026.8.1, ESP-IDF 5.5.5):

- Volume up / down / mute all report correctly, so ESP-IDF enumerates the same
  interface 2 / endpoint 0x84 that Linux did, and the knob needs no HID
  SET_PROTOCOL or SET_IDLE request to report on the extrakeys interface.
- The VIA lighting commands work against Binepad's shipped VIAL firmware; the
  underglow tracks the player volume.

If a future firmware revision moves things around, `on_connected()` logs every
endpoint it finds on the claimed interfaces at DEBUG, and `interface:`,
`endpoint:`, `raw_interface:`, `raw_endpoint_in:` and `raw_endpoint_out:` are
all overridable in YAML.
