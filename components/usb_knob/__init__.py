import esphome.codegen as cg
from esphome.components.usb_host import USBClient, register_usb_client, usb_device_schema
import esphome.config_validation as cv
from esphome.types import ConfigType

DEPENDENCIES = ["usb_host"]
CODEOWNERS = ["@harm"]

usb_knob_ns = cg.esphome_ns.namespace("usb_knob")
UsbKnob = usb_knob_ns.class_("UsbKnob", USBClient)

CONF_INTERFACE = "interface"
CONF_ENDPOINT = "endpoint"
CONF_REPORT_ID = "report_id"
CONF_LIGHTING = "lighting"
CONF_RAW_INTERFACE = "raw_interface"
CONF_RAW_ENDPOINT_IN = "raw_endpoint_in"
CONF_RAW_ENDPOINT_OUT = "raw_endpoint_out"

# Binepad KnobX1
DEFAULT_VID = 0x5831
DEFAULT_PID = 0x4249
# Interface 2 = "extrakeys" (consumer control), endpoint 0x84 IN, reports prefixed 0x04.
DEFAULT_INTERFACE = 2
DEFAULT_ENDPOINT = 0x84
DEFAULT_REPORT_ID = 0x04
# Interface 1 = VIAL raw HID, which is where the rgblight (2x WS2812) controls live.
DEFAULT_RAW_INTERFACE = 1
DEFAULT_RAW_ENDPOINT_IN = 0x82
DEFAULT_RAW_ENDPOINT_OUT = 0x03

CONFIG_SCHEMA = usb_device_schema(UsbKnob, DEFAULT_VID, DEFAULT_PID).extend(
    {
        cv.Optional(CONF_INTERFACE, default=DEFAULT_INTERFACE): cv.uint8_t,
        cv.Optional(CONF_ENDPOINT, default=DEFAULT_ENDPOINT): cv.hex_uint8_t,
        # 0 means "device sends no report ID prefix"
        cv.Optional(CONF_REPORT_ID, default=DEFAULT_REPORT_ID): cv.hex_uint8_t,
        cv.Optional(CONF_LIGHTING, default=True): cv.boolean,
        cv.Optional(CONF_RAW_INTERFACE, default=DEFAULT_RAW_INTERFACE): cv.uint8_t,
        cv.Optional(CONF_RAW_ENDPOINT_IN, default=DEFAULT_RAW_ENDPOINT_IN): cv.hex_uint8_t,
        cv.Optional(CONF_RAW_ENDPOINT_OUT, default=DEFAULT_RAW_ENDPOINT_OUT): cv.hex_uint8_t,
    }
)


async def to_code(config: ConfigType) -> None:
    var = await register_usb_client(config)
    cg.add(var.set_interface(config[CONF_INTERFACE]))
    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_report_id(config[CONF_REPORT_ID]))
    cg.add(var.set_lighting(config[CONF_LIGHTING]))
    cg.add(var.set_raw_interface(config[CONF_RAW_INTERFACE]))
    cg.add(var.set_raw_endpoint_in(config[CONF_RAW_ENDPOINT_IN]))
    cg.add(var.set_raw_endpoint_out(config[CONF_RAW_ENDPOINT_OUT]))
