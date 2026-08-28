#pragma once

// Mirrors the variant guard used by esphome/components/usb_host/usb_host.h
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/lock_free_queue.h"

#include <vector>

namespace esphome::usb_knob {

// One decoded HID consumer-control report. Produced in the USB task, consumed in the main loop.
struct KnobEvent {
  uint16_t usage;
  void release() {}  // required by EventPool; POD, nothing to clean up
};

static constexpr size_t KNOB_EVENT_QUEUE_SIZE = 16;

class UsbKnob : public usb_host::USBClient {
 public:
  UsbKnob(uint16_t vid, uint16_t pid) : USBClient(vid, pid) {}

  void loop() override;
  void dump_config() override;

  void set_interface(uint8_t interface) { this->interface_ = interface; }
  void set_endpoint(uint8_t endpoint) { this->endpoint_ = endpoint; }
  void set_report_id(uint8_t report_id) { this->report_id_ = report_id; }
  void set_raw_interface(uint8_t interface) { this->raw_interface_ = interface; }
  void set_raw_endpoint_in(uint8_t endpoint) { this->raw_endpoint_in_ = endpoint; }
  void set_raw_endpoint_out(uint8_t endpoint) { this->raw_endpoint_out_ = endpoint; }
  void set_lighting(bool lighting) { this->lighting_ = lighting; }

  /// Set the underglow colour. Hue/saturation/value are QMK's 0-255 scale
  /// (hue 0 = red, 85 = green, 170 = blue). Not written to the knob's EEPROM.
  void set_hsv(uint8_t hue, uint8_t saturation, uint8_t value);
  void register_key(uint16_t usage, binary_sensor::BinarySensor *sensor) {
    this->keys_.push_back(KeySensor{usage, sensor});
  }

 protected:
  struct KeySensor {
    uint16_t usage;
    binary_sensor::BinarySensor *sensor;
  };

  void on_connected() override;
  void on_disconnected() override;

  // Claims the VIAL raw HID interface and starts the response listener. Failure is
  // non-fatal: the knob still reports volume events without working lighting.
  // Looks up an endpoint on an interface, returning its capped max packet size in
  // packet_size, or false if either the interface or the endpoint is missing.
  bool find_endpoint_(const usb_config_desc_t *config_desc, uint8_t interface, uint8_t endpoint,
                      uint16_t *packet_size);
  void setup_lighting_();
  void start_raw_input_();
  void handle_raw_report_(const uint8_t *data, size_t len);
  bool send_via_(const uint8_t *data, size_t len);
  void send_rgblight_effect_(uint8_t effect);

  // Submits an interrupt IN transfer on the configured endpoint. Called from both threads.
  void start_input_();
  // Decodes a raw report; runs in the USB task, so it only queues the result.
  void handle_report_(const uint8_t *data, size_t len);
  // Publishes state changes; main loop only.
  void publish_usage_(uint16_t usage);

  std::vector<KeySensor> keys_;
  LockFreeQueue<KnobEvent, KNOB_EVENT_QUEUE_SIZE> event_queue_;
  // Pool is sized to queue capacity (SIZE-1) so allocate() fails before push() can.
  EventPool<KnobEvent, KNOB_EVENT_QUEUE_SIZE - 1> event_pool_;

  uint16_t packet_size_{0};
  uint16_t raw_packet_size_{0};
  uint16_t active_usage_{0};
  // VIA protocol version, read from the knob; decides the lighting packet layout.
  std::atomic<uint16_t> via_protocol_{0};
  uint8_t interface_{2};
  uint8_t endpoint_{0x84};
  uint8_t report_id_{0x04};
  uint8_t raw_interface_{1};
  uint8_t raw_endpoint_in_{0x82};
  uint8_t raw_endpoint_out_{0x03};
  bool lighting_{true};
  bool claimed_{false};
  bool raw_claimed_{false};
  std::atomic<bool> input_started_{false};
  std::atomic<bool> raw_input_started_{false};
};

}  // namespace esphome::usb_knob

#endif  // USE_ESP32_VARIANT_*
