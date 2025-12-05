import esphome.codegen as cg
from esphome.components import text_sensor, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID

CONF_SCREEN_NUMBER = "screen_number"
CONF_DISPLAY_TEXT = "display_text"
CONF_BACKLIGHT_TIMEOUT = "backlight_timeout"

# Honeywell Galaxy 7 Keypad - bdavj

DEPENDENCIES = ["uart"]

galaxy_ns = cg.esphome_ns.namespace("honeywell_galaxy7_keypad")
HoneywellGalaxy7Keypad = galaxy_ns.class_(
    "HoneywellGalaxy7Keypad", uart.UARTDevice, cg.Component
)

CONF_RS485_RX_ID = "rs485_rx_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HoneywellGalaxy7Keypad),
            cv.Optional(CONF_RS485_RX_ID): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_SCREEN_NUMBER, default=2): cv.int_range(min=1, max=4),
            cv.Optional(
                CONF_DISPLAY_TEXT, default="ESP-HOME|Initializing"
            ): cv.string,
            cv.Optional(CONF_BACKLIGHT_TIMEOUT, default="15s"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "honeywell_galaxy7_keypad", require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    rx = config.get(CONF_RS485_RX_ID)
    if rx is not None:
        sens = await cg.get_variable(rx)
        cg.add(var.set_rx_text_sensor(sens))

    # Map screen 1-4 to Honeywell device IDs 0x10/0x20/0x30/0x40
    screen_number = config[CONF_SCREEN_NUMBER]
    device_id = 0x10 + (screen_number - 1) * 0x10
    cg.add(var.set_device_id(device_id))

    cg.add(var.set_display_text(config[CONF_DISPLAY_TEXT]))

    cg.add(var.set_backlight_timeout(config[CONF_BACKLIGHT_TIMEOUT]))
