import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import PtouchBridge

CONF_PTOUCH_BRIDGE_ID = "ptouch_bridge_id"
CONF_KIND = "kind"

KINDS = {
    "connection_phase": 0,
    "connection_detail": 1,
    "current_cartridge": 2,
    "last_cartridge": 3,
    "last_print_result": 4,
    "printer_address": 5,
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.Required(CONF_PTOUCH_BRIDGE_ID): cv.use_id(PtouchBridge),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_PTOUCH_BRIDGE_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(parent.set_text_sensor(config[CONF_KIND], var))

