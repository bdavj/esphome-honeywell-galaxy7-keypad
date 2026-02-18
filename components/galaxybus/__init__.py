import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_UART_ID
from esphome import pins

CONF_REPLY_TIMEOUT = "reply_timeout"
CONF_INTER_FRAME_GAP = "inter_frame_gap"
CONF_MAX_QUEUE_DEPTH = "max_queue_depth"
CONF_MAX_QUEUE_DEPTH = "max_queue_depth"

DEPENDENCIES = ["uart"]

galaxybus_ns = cg.esphome_ns.namespace("galaxybus")
GalaxyBus = galaxybus_ns.class_("GalaxyBus", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GalaxyBus),
            cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_REPLY_TIMEOUT, default="100ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_INTER_FRAME_GAP, default="10ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_QUEUE_DEPTH, default=8): cv.positive_int,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_reply_timeout_ms(config[CONF_REPLY_TIMEOUT]))
    cg.add(var.set_inter_frame_gap_ms(config[CONF_INTER_FRAME_GAP]))
    cg.add(var.set_max_queue_depth(config[CONF_MAX_QUEUE_DEPTH]))
