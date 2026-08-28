import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import UsbKnob, usb_knob_ns

DEPENDENCIES = ["usb_knob"]

CONF_USB_KNOB_ID = "usb_knob_id"
CONF_KEY = "key"

# HID Usage Page 0x0C (Consumer) codes, as captured from the KnobX1.
CONSUMER_CODES = {
    "volume_up": 0x00E9,
    "volume_down": 0x00EA,
    "mute": 0x00E2,
    "play_pause": 0x00CD,
    "stop": 0x00B7,
    "next_track": 0x00B5,
    "previous_track": 0x00B6,
}


def validate_key(value):
    if isinstance(value, str):
        key = value.lower().replace(" ", "_")
        if key in CONSUMER_CODES:
            return CONSUMER_CODES[key]
        raise cv.Invalid(
            f"Unknown key name '{value}'; use one of {', '.join(sorted(CONSUMER_CODES))} "
            f"or a raw usage code such as 0x00E9"
        )
    return cv.hex_uint16_t(value)


CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_USB_KNOB_ID): cv.use_id(UsbKnob),
        cv.Required(CONF_KEY): validate_key,
    }
)


async def to_code(config: ConfigType) -> None:
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_USB_KNOB_ID])
    cg.add(parent.register_key(config[CONF_KEY], var))
