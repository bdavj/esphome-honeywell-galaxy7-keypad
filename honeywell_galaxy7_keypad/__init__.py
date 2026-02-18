import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor, binary_sensor, switch, sensor
from esphome.const import CONF_ID

from ..galaxybus import GalaxyBus

CONF_SCREEN_NUMBER = "screen_number"
CONF_DISPLAY_TEXT = "display_text"
CONF_BACKLIGHT_TIMEOUT = "backlight_timeout"
CONF_PROX_POLL = "prox_poll"

CONF_BUS_ID = "bus_id"
CONF_ADDRESS = "address"

CONF_CODE = "code_sensor"
CONF_RX_TEXT_SENSOR = "rx_text_sensor"
CONF_TAMPER = "tamper_sensor"
CONF_PAGE_SENSOR = "page_sensor"
CONF_BEEP_SWITCH = "beep_switch"
CONF_PANEL_ONLINE = "panel_online_sensor"

DEPENDENCIES = ["galaxybus"]
AUTO_LOAD = ["text_sensor", "binary_sensor", "switch", "sensor"]

ns = cg.esphome_ns.namespace("honeywell_galaxy7_keypad")
galaxybus_ns = cg.esphome_ns.namespace("galaxybus")

GalaxyBusClient = galaxybus_ns.class_("GalaxyBusClient")
BeepSwitch = ns.class_("HoneywellBeepSwitch", switch.Switch)
HoneywellGalaxy7Keypad = ns.class_(
    "HoneywellGalaxy7Keypad",
    cg.Component,
    GalaxyBusClient,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HoneywellGalaxy7Keypad),

            # Shared GalaxyBus instance
            cv.Required(CONF_BUS_ID): cv.use_id(GalaxyBus),

            # Either explicit RS485 addr or a logical screen number 1–4
            cv.Optional(CONF_ADDRESS): cv.int_range(min=0x00, max=0xFF),
            cv.Optional(CONF_SCREEN_NUMBER, default=1): cv.int_range(min=1, max=4),

            cv.Optional(CONF_DISPLAY_TEXT, default="ESP-HOME|Initializing"): cv.string,
            cv.Optional(
                CONF_BACKLIGHT_TIMEOUT,
                default="15s",
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PROX_POLL, default=False): cv.boolean,

            # Proper declarative entities
            cv.Optional(CONF_CODE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_RX_TEXT_SENSOR): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_TAMPER): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_BEEP_SWITCH): switch.switch_schema(BeepSwitch),
            cv.Optional(CONF_PANEL_ONLINE): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_PAGE_SENSOR): sensor.sensor_schema(
                unit_of_measurement="",
                icon="mdi:page-next",
                accuracy_decimals=0,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Bind to shared GalaxyBus
    bus = await cg.get_variable(config[CONF_BUS_ID])
    cg.add(var.set_bus(bus))
    cg.add(bus.register_client(var))

    # Address mapping: explicit address wins, else 0x10/0x20/0x30/0x40 by screen_number
    if CONF_ADDRESS in config:
        cg.add(var.set_device_id(config[CONF_ADDRESS]))
    else:
        screen_number = config[CONF_SCREEN_NUMBER]
        device_id = 0x10 + (screen_number - 1) * 0x10
        cg.add(var.set_device_id(device_id))

    cg.add(var.set_display_text(config[CONF_DISPLAY_TEXT]))
    cg.add(var.set_backlight_timeout(config[CONF_BACKLIGHT_TIMEOUT]))
    cg.add(var.enable_prox_polling(config[CONF_PROX_POLL]))

    # Code text sensor (publishes entered code on ENT)
    if CONF_CODE in config:
        code_sens = await text_sensor.new_text_sensor(config[CONF_CODE])
        cg.add(var.set_code_text_sensor(code_sens))

    # Raw RX text sensor (if you still want that debug stream / API)
    if CONF_RX_TEXT_SENSOR in config:
        rx_sens = await text_sensor.new_text_sensor(config[CONF_RX_TEXT_SENSOR])
        cg.add(var.set_rx_text_sensor(rx_sens))

    # Tamper binary_sensor
    if CONF_TAMPER in config:
        tamper_sens = await binary_sensor.new_binary_sensor(config[CONF_TAMPER])
        cg.add(var.set_tamper_binary_sensor(tamper_sens))

    # Page number
    if CONF_PAGE_SENSOR in config:
        page_sens = await sensor.new_sensor(config[CONF_PAGE_SENSOR])
        cg.add(var.set_page_sensor(page_sens))

    # Beep control switch
    if CONF_BEEP_SWITCH in config:
        beep_sw = cg.new_Pvariable(config[CONF_BEEP_SWITCH][CONF_ID], var)
        await switch.register_switch(beep_sw, config[CONF_BEEP_SWITCH])
        cg.add(var.set_beep_switch(beep_sw))

    # Panel-online state
    if CONF_PANEL_ONLINE in config:
        panel_sens = await binary_sensor.new_binary_sensor(config[CONF_PANEL_ONLINE])
        cg.add(var.set_panel_online_binary_sensor(panel_sens))
