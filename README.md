# KnobX1 → ESPHome USB host component

Reads HID consumer-control events from a **Binepad KnobX1** plugged into an
**M5Stack AtomS3-Lite** running as a USB host, exposes them as ESPHome
`binary_sensor`s, and drives the knob's underglow back over the same USB link
to reflect what the player is doing.

See `knobx1-esphome-handoff.md` for the hardware investigation (USB descriptor
dump, captured report bytes).

## Controls

The knob has two inputs, rotation and push. Rotation maps straight through;
the push is decoded into three gestures by `on_multi_click`.

| Gesture | Action | Fires |
|---|---|---|
| Rotate CW / CCW | `media_player.volume_up` / `volume_down` | immediately |
| Tap | `media_player.media_play_pause` | after `double_push_window` |
| Double tap | `media_player.media_next_track` | on the second release |
| Hold >= `long_push_time` | `media_player.volume_mute` (toggle) | while still held |

The tap is necessarily delayed: until the double-push window lapses, a first
push is indistinguishable from the start of a double push. The hold fires
during the press rather than on release, so it gets immediate feedback.

Mute is a real toggle, not a set: an internal `homeassistant` binary_sensor
mirrors the player's `is_volume_muted` attribute back to the device, and the
action sends its opposite.

Timings, all substitutions:

| Substitution | Default | Meaning |
|---|---|---|
| `double_push_window` | `0.35s` | max gap between two pushes; also the tap delay |
| `short_push_max` | `0.55s` | longest push still counted as a tap |
| `long_push_time` | `0.6s` | shortest push counted as a hold |

The gap between `short_push_max` and `long_push_time` is deliberate: without it
a borderline push could match both patterns and fire two actions. The cost is a
narrow dead zone where a push does nothing.

## Underglow

The two WS2812s under the knob follow the player:

| Player state | Underglow |
|---|---|
| Not `playing` (paused, idle, off, unavailable) | dim white, `idle_brightness` |
| `playing`, muted | breathing at the volume hue |
| `playing` | static, coloured by volume |

While playing, hue runs **blue at volume 0 -> green at 0.5 -> red at 1.0**
(`hue = 170 * (1 - volume)` on QMK's 0-255 scale). Brightness is
`floor + (255 - floor) * sqrt(|cos(2*pi*volume)|)`, which peaks on each of the
three pure colours and dips through the blends. The square root flattens the
dip so the transitions stay bright rather than passing through near-darkness.

Breathing is driven from the ESP on a 50 ms interval, *not* by the knob's own
animation, which always restarts from fully dark and so drops abruptly when it
starts. On entering mute the phase is seeded from the brightness already
showing, `phase = acos(1 - 2 * b0)`, putting it on the rising half of the curve
so there is no visible jump.

| Substitution | Default | Meaning |
|---|---|---|
| `led_min_brightness` | `110` | floor for the volume gradient, out of 255 |
| `idle_brightness` | `100` | dim white shown when not playing |
| `breathe_period` | `4.0` | seconds per full breath |
| `breathe_min` / `breathe_max` | `20` / `255` | brightness the breath swings between |

Repainting is idempotent: `set_hsv()` and `set_effect()` drop updates identical
to the last one sent, so the 5 s repaint that recovers state after the knob is
unplugged and reattached costs no USB traffic while nothing changes.

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

## Lighting

The KnobX1 has two WS2812 underglow LEDs. Per QMK's
`keyboards/binepad/knobx1/keyboard.json` they are **`rgblight`** (not
`rgb_matrix`), `led_count: 2`, driver ws2812 on pin C13.

They are driven over the **VIAL raw HID interface** (interface 1, EP 0x03 OUT /
0x82 IN), using VIA's lighting commands:

| Command | VIA >= 11 | VIA < 11 (VIAL) |
|---|---|---|
| set colour | `07 02 04 <hue> <sat>` | `07 83 <hue> <sat>` |
| set brightness | `07 02 01 <val>` | `07 80 <val>` |
| set effect | `07 02 02 <effect>` | `07 81 <effect>` |
| read effect | `08 02 02` | `08 81` |

On connect the component sends `01` (get protocol version) and picks the layout
from the reply. The knob speaks **VIA protocol 9** — VIAL pins
`VIA_PROTOCOL_VERSION` to `0x0009` — so the legacy flat-value-id layout is the
one actually in use here; the VIA >= 11 channel layout is kept for other
firmware.

Nothing is written to the knob's EEPROM: VIA only persists on an explicit
`id_custom_save` (`0x09`), which is never sent.

`id(knob1).set_hsv(hue, sat, val)` and `id(knob1).set_effect(effect)` are
callable from any lambda. Hue/sat/val are QMK's 0-255 scale (hue 0 = red,
85 = green, 170 = blue); saturation 0 is white. rgblight effects: 0 turns the
underglow off, 1 is static light, 2-5 are breathing from slowest to fastest.
The device config uses static light throughout and animates brightness itself,
for the reason given under **Underglow** above. `id(knob1).query_effect()` reads
the current effect back for debugging and logs it when it changes; 0 means the
firmware considers the underglow disabled.

Hue/saturation and brightness are cached separately and sent as the separate
commands they are, so animating brightness alone costs one 32-byte report per
step rather than two.

Effect 0 is not used by the device config: it does make the firmware report the
underglow as off, but on this unit the LEDs stayed visibly lit, so idle shows
dim white instead.

The four layer indicators (IND1-IND4, GPIO A3-A6) are **not** reachable over
USB, and stock firmware leaves IND1 permanently lit: `x1_layer_led()` tests
`lyr >= 0` on a `uint8_t`. Turning it off needs a custom vial-qmk build
overriding that weak function.

Claiming the raw interface is best-effort: if it fails, lighting is skipped and
a warning is logged, but volume reporting keeps working. `lighting: false`
disables it.

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
