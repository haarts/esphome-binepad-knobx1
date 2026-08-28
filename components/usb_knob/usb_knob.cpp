#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "usb_knob.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::usb_knob {

static const char *const TAG = "usb_knob";

// VIA raw HID protocol (VIAL firmware speaks it too).
static constexpr uint8_t VIA_GET_PROTOCOL_VERSION = 0x01;
static constexpr uint8_t VIA_CUSTOM_SET_VALUE = 0x07;
static constexpr uint8_t VIA_CUSTOM_GET_VALUE = 0x08;
// VIA >= 11 addresses lighting through a channel byte.
static constexpr uint8_t VIA_CHANNEL_RGBLIGHT = 0x02;
static constexpr uint8_t VIA_RGBLIGHT_BRIGHTNESS = 0x01;
static constexpr uint8_t VIA_RGBLIGHT_EFFECT = 0x02;
static constexpr uint8_t VIA_RGBLIGHT_COLOR = 0x04;
// VIA < 11 uses a flat value id with no channel byte.
static constexpr uint8_t VIA_V2_RGBLIGHT_BRIGHTNESS = 0x80;
static constexpr uint8_t VIA_V2_RGBLIGHT_EFFECT = 0x81;
static constexpr uint8_t VIA_V2_RGBLIGHT_COLOR = 0x83;
// rgblight mode 1 = static light; 0 would disable the underglow entirely.
static constexpr uint8_t RGBLIGHT_MODE_STATIC = 1;
static constexpr uint16_t VIA_PROTOCOL_CUSTOM_CHANNELS = 11;

void UsbKnob::on_connected() {
  const usb_config_desc_t *config_desc;
  auto err = usb_host_get_active_config_descriptor(this->device_handle_, &config_desc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_active_config_descriptor failed: %s", esp_err_to_name(err));
    this->status_set_error(LOG_STR("No config descriptor"));
    this->disconnect();
    return;
  }

  if (!this->find_endpoint_(config_desc, this->interface_, this->endpoint_, &this->packet_size_)) {
    this->status_set_error(LOG_STR("Endpoint not found"));
    this->disconnect();
    return;
  }

  err = usb_host_interface_claim(this->handle_, this->device_handle_, this->interface_, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "interface_claim(%u) failed: %s", this->interface_, esp_err_to_name(err));
    this->status_set_error(LOG_STR("Interface claim failed"));
    this->disconnect();
    return;
  }
  this->claimed_ = true;
  this->status_clear_error();
  ESP_LOGI(TAG, "Claimed interface %u, listening on endpoint 0x%02X (%u bytes)", this->interface_, this->endpoint_,
           this->packet_size_);

  this->input_started_.store(false);
  this->start_input_();

  if (this->lighting_)
    this->setup_lighting_();
}

bool UsbKnob::find_endpoint_(const usb_config_desc_t *config_desc, uint8_t interface, uint8_t endpoint,
                             uint16_t *packet_size) {
  int offset = 0;
  const auto *intf_desc = usb_parse_interface_descriptor(config_desc, interface, 0, &offset);
  if (intf_desc == nullptr) {
    ESP_LOGE(TAG, "Interface %u not found", interface);
    return false;
  }
  for (uint8_t i = 0; i != intf_desc->bNumEndpoints; i++) {
    int ep_offset = offset;
    const auto *ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &ep_offset);
    if (ep == nullptr)
      continue;
    ESP_LOGD(TAG, "Interface %u endpoint 0x%02X, attributes 0x%02X, mps %u", interface, ep->bEndpointAddress,
             ep->bmAttributes, ep->wMaxPacketSize);
    if (ep->bEndpointAddress == endpoint) {
      *packet_size = std::min<uint16_t>(ep->wMaxPacketSize, usb_host::USB_MAX_PACKET_SIZE);
      return true;
    }
  }
  ESP_LOGE(TAG, "Endpoint 0x%02X not found on interface %u", endpoint, interface);
  return false;
}

// Lighting lives behind the VIAL raw HID interface, which is separate from the
// consumer-control one. Every failure here is logged and swallowed: losing the
// LEDs must not take the volume knob down with it.
void UsbKnob::setup_lighting_() {
  const usb_config_desc_t *config_desc;
  if (usb_host_get_active_config_descriptor(this->device_handle_, &config_desc) != ESP_OK)
    return;

  uint16_t in_size = 0;
  uint16_t out_size = 0;
  if (!this->find_endpoint_(config_desc, this->raw_interface_, this->raw_endpoint_in_, &in_size) ||
      !this->find_endpoint_(config_desc, this->raw_interface_, this->raw_endpoint_out_, &out_size)) {
    ESP_LOGW(TAG, "Raw HID endpoints not found; lighting disabled");
    return;
  }
  this->raw_packet_size_ = std::min(in_size, out_size);

  auto err = usb_host_interface_claim(this->handle_, this->device_handle_, this->raw_interface_, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not claim raw HID interface %u: %s; lighting disabled", this->raw_interface_,
             esp_err_to_name(err));
    return;
  }
  this->raw_claimed_ = true;
  ESP_LOGI(TAG, "Claimed raw HID interface %u for lighting (%u bytes)", this->raw_interface_, this->raw_packet_size_);

  this->raw_input_started_.store(false);
  this->start_raw_input_();

  // The reply decides which lighting packet layout to use; see handle_raw_report_().
  const uint8_t query[] = {VIA_GET_PROTOCOL_VERSION};
  this->send_via_(query, sizeof(query));
}

void UsbKnob::start_raw_input_() {
  if (!this->raw_claimed_)
    return;
  auto started = false;
  if (!this->raw_input_started_.compare_exchange_strong(started, true))
    return;

  // CALLBACK CONTEXT: USB task.
  auto callback = [this](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGW(TAG, "Raw HID transfer failed, status=0x%X", status.error_code);
      this->raw_input_started_.store(false);
      return;
    }
    this->handle_raw_report_(status.data, status.data_len);
    this->raw_input_started_.store(false);
    this->start_raw_input_();
  };

  if (!this->transfer_in(this->raw_endpoint_in_, callback, this->raw_packet_size_)) {
    ESP_LOGE(TAG, "Raw HID IN transfer submission failed");
    this->raw_input_started_.store(false);
  }
}

// THREAD CONTEXT: USB task.
void UsbKnob::handle_raw_report_(const uint8_t *data, size_t len) {
  if (len < 1)
    return;
  ESP_LOGV(TAG, "VIA reply: %02X %02X %02X %02X (%u bytes)", data[0], len > 1 ? data[1] : 0,
           len > 2 ? data[2] : 0, len > 3 ? data[3] : 0, len);
  // Reply to query_effect(). VIA >= 11 answers [ 08, channel, value_id, value ];
  // VIAL (protocol 9) answers [ 08, value_id, value ].
  if (data[0] == VIA_CUSTOM_GET_VALUE) {
    int16_t effect = -1;
    if (len >= 4 && data[1] == VIA_CHANNEL_RGBLIGHT && data[2] == VIA_RGBLIGHT_EFFECT) {
      effect = data[3];
    } else if (len >= 3 && data[1] == VIA_V2_RGBLIGHT_EFFECT) {
      effect = data[2];
    }
    if (effect >= 0 && effect != this->last_reported_effect_) {
      this->last_reported_effect_ = effect;
      ESP_LOGI(TAG, "knob reports rgblight effect %d (0 = underglow disabled)", effect);
    }
    return;
  }
  if (data[0] == VIA_GET_PROTOCOL_VERSION && len >= 3) {
    // Big-endian, unlike everything else in this protocol.
    uint16_t version = (data[1] << 8) | data[2];
    this->via_protocol_.store(version);
    ESP_LOGI(TAG, "VIA protocol version %u", version);
    // The version decides the packet layout, so anything cached was sent in the
    // other format and has to be resent.
    this->last_effect_ = -1;
    this->last_hue_ = -1;
    this->last_sat_ = -1;
    this->last_val_ = -1;
    // Solid colour, otherwise an animation would immediately overwrite our hue.
    this->set_effect(RGBLIGHT_MODE_STATIC);
  }
}

// THREAD CONTEXT: main loop (set_hsv) and USB task (protocol reply).
bool UsbKnob::send_via_(const uint8_t *data, size_t len) {
  if (!this->raw_claimed_)
    return false;
  if (len > this->raw_packet_size_)
    return false;
  // Raw HID reports are fixed-size and unprefixed; pad the rest with zeroes.
  uint8_t packet[usb_host::USB_MAX_PACKET_SIZE] = {};
  memcpy(packet, data, len);
  return this->transfer_out(
      this->raw_endpoint_out_, [](const usb_host::TransferStatus &status) {}, packet, this->raw_packet_size_);
}

void UsbKnob::set_effect(uint8_t effect) {
  if (!this->raw_claimed_) {
    ESP_LOGD(TAG, "set_effect ignored, lighting not available");
    return;
  }
  if (effect == this->last_effect_)
    return;
  // The firmware drops colour writes while the underglow is disabled, and
  // restores its own last colour when re-enabled. Either way our cache no
  // longer describes what the knob is showing, so force the next set_hsv().
  if (effect == 0 || this->last_effect_ == 0) {
    this->last_hue_ = -1;
    this->last_sat_ = -1;
    this->last_val_ = -1;
  }
  ESP_LOGV(TAG, "set_effect(%u)", effect);
  this->send_rgblight_effect_(effect);
  this->last_effect_ = effect;
}

void UsbKnob::query_effect() {
  if (!this->raw_claimed_)
    return;
  if (this->via_protocol_.load() >= VIA_PROTOCOL_CUSTOM_CHANNELS) {
    const uint8_t packet[] = {VIA_CUSTOM_GET_VALUE, VIA_CHANNEL_RGBLIGHT, VIA_RGBLIGHT_EFFECT};
    this->send_via_(packet, sizeof(packet));
  } else {
    const uint8_t packet[] = {VIA_CUSTOM_GET_VALUE, VIA_V2_RGBLIGHT_EFFECT};
    this->send_via_(packet, sizeof(packet));
  }
}

void UsbKnob::send_rgblight_effect_(uint8_t effect) {
  if (this->via_protocol_.load() >= VIA_PROTOCOL_CUSTOM_CHANNELS) {
    const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_CHANNEL_RGBLIGHT, VIA_RGBLIGHT_EFFECT, effect};
    this->send_via_(packet, sizeof(packet));
  } else {
    const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_V2_RGBLIGHT_EFFECT, effect};
    this->send_via_(packet, sizeof(packet));
  }
}

void UsbKnob::set_hsv(uint8_t hue, uint8_t saturation, uint8_t value) {
  if (!this->raw_claimed_) {
    ESP_LOGD(TAG, "set_hsv ignored, lighting not available");
    return;
  }
  // rgblight_sethsv_noeeprom() returns early while the underglow is disabled,
  // so sending now would be dropped by the firmware and wrongly cached here.
  if (this->last_effect_ == 0) {
    ESP_LOGV(TAG, "set_hsv ignored, underglow is off");
    return;
  }
  bool color_changed = hue != this->last_hue_ || saturation != this->last_sat_;
  bool value_changed = value != this->last_val_;
  if (!color_changed && !value_changed)
    return;
  ESP_LOGV(TAG, "set_hsv(%u, %u, %u)", hue, saturation, value);

  bool modern = this->via_protocol_.load() >= VIA_PROTOCOL_CUSTOM_CHANNELS;
  if (color_changed) {
    if (modern) {
      const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_CHANNEL_RGBLIGHT, VIA_RGBLIGHT_COLOR, hue, saturation};
      this->send_via_(packet, sizeof(packet));
    } else {
      const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_V2_RGBLIGHT_COLOR, hue, saturation};
      this->send_via_(packet, sizeof(packet));
    }
    this->last_hue_ = hue;
    this->last_sat_ = saturation;
  }
  if (value_changed) {
    if (modern) {
      const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_CHANNEL_RGBLIGHT, VIA_RGBLIGHT_BRIGHTNESS, value};
      this->send_via_(packet, sizeof(packet));
    } else {
      const uint8_t packet[] = {VIA_CUSTOM_SET_VALUE, VIA_V2_RGBLIGHT_BRIGHTNESS, value};
      this->send_via_(packet, sizeof(packet));
    }
    this->last_val_ = value;
  }
}

void UsbKnob::on_disconnected() {
  // Release anything still held down so Home Assistant doesn't see a stuck press.
  this->publish_usage_(0);

  if (this->claimed_) {
    usb_host_endpoint_halt(this->device_handle_, this->endpoint_);
    usb_host_endpoint_flush(this->device_handle_, this->endpoint_);
    auto err = usb_host_interface_release(this->handle_, this->device_handle_, this->interface_);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "interface_release(%u) failed: %s", this->interface_, esp_err_to_name(err));
    this->claimed_ = false;
  }
  if (this->raw_claimed_) {
    usb_host_endpoint_halt(this->device_handle_, this->raw_endpoint_in_);
    usb_host_endpoint_flush(this->device_handle_, this->raw_endpoint_in_);
    usb_host_endpoint_halt(this->device_handle_, this->raw_endpoint_out_);
    usb_host_endpoint_flush(this->device_handle_, this->raw_endpoint_out_);
    auto err = usb_host_interface_release(this->handle_, this->device_handle_, this->raw_interface_);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "interface_release(%u) failed: %s", this->raw_interface_, esp_err_to_name(err));
    this->raw_claimed_ = false;
  }
  this->via_protocol_.store(0);
  this->last_effect_ = -1;
  this->last_hue_ = -1;
  this->last_sat_ = -1;
  this->last_val_ = -1;
  this->last_reported_effect_ = -1;
  this->raw_input_started_.store(false);
  this->input_started_.store(false);
  this->packet_size_ = 0;
  // Resets the transfer request pool.
  USBClient::on_disconnected();
}

// THREAD CONTEXT: main loop (from on_connected) and USB task (transfer callback restart).
void UsbKnob::start_input_() {
  if (!this->claimed_)
    return;
  auto started = false;
  if (!this->input_started_.compare_exchange_strong(started, true))
    return;

  // CALLBACK CONTEXT: USB task.
  auto callback = [this](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGW(TAG, "Input transfer failed, status=0x%X", status.error_code);
      this->input_started_.store(false);
      return;
    }
    this->handle_report_(status.data, status.data_len);
    this->input_started_.store(false);
    this->start_input_();
  };

  if (!this->transfer_in(this->endpoint_, callback, this->packet_size_)) {
    ESP_LOGE(TAG, "IN transfer submission failed for ep 0x%02X", this->endpoint_);
    this->input_started_.store(false);
  }
}

// THREAD CONTEXT: USB task. Must stay fast and non-blocking.
void UsbKnob::handle_report_(const uint8_t *data, size_t len) {
  const uint8_t *usage_bytes;
  if (this->report_id_ != 0) {
    if (len < 3 || data[0] != this->report_id_)
      return;
    usage_bytes = data + 1;
  } else {
    if (len < 2)
      return;
    usage_bytes = data;
  }
  // Consumer control usage code, little-endian. 0 means "nothing held down".
  uint16_t usage = usage_bytes[0] | (usage_bytes[1] << 8);

  KnobEvent *event = this->event_pool_.allocate();
  if (event == nullptr) {
    this->event_queue_.increment_dropped_count();
    return;
  }
  event->usage = usage;
  this->event_queue_.push(event);
  this->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
}

void UsbKnob::loop() {
  bool had_work = this->process_usb_events_();

  KnobEvent *event;
  while ((event = this->event_queue_.pop()) != nullptr) {
    had_work = true;
    this->publish_usage_(event->usage);
    this->event_pool_.release(event);
  }

  uint16_t dropped = this->event_queue_.get_and_reset_dropped_count();
  if (dropped > 0)
    ESP_LOGW(TAG, "Dropped %u knob events due to queue overflow", dropped);

  if (!had_work)
    this->disable_loop();
}

// THREAD CONTEXT: main loop only.
void UsbKnob::publish_usage_(uint16_t usage) {
  if (usage == this->active_usage_)
    return;
  ESP_LOGV(TAG, "Usage 0x%04X -> 0x%04X", this->active_usage_, usage);
  for (const auto &key : this->keys_) {
    if (key.usage == this->active_usage_)
      key.sensor->publish_state(false);
  }
  for (const auto &key : this->keys_) {
    if (key.usage == usage)
      key.sensor->publish_state(true);
  }
  this->active_usage_ = usage;
}

void UsbKnob::dump_config() {
  USBClient::dump_config();
  ESP_LOGCONFIG(TAG,
                "  Interface: %u\n"
                "  Endpoint: 0x%02X\n"
                "  Report ID: 0x%02X\n"
                "  Lighting: %s",
                this->interface_, this->endpoint_, this->report_id_, YESNO(this->lighting_));
  for (const auto &key : this->keys_) {
    ESP_LOGCONFIG(TAG, "  Key 0x%04X:", key.usage);
    LOG_BINARY_SENSOR("    ", "Binary Sensor", key.sensor);
  }
}

}  // namespace esphome::usb_knob

#endif  // USE_ESP32_VARIANT_*
