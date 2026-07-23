import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import PtouchBridge

CONF_PTOUCH_BRIDGE_ID = "ptouch_bridge_id"
CONF_KIND = "kind"

KINDS = {
    "printer_connected": 0,
    "media_loaded": 1,
}

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.Required(CONF_PTOUCH_BRIDGE_ID): cv.use_id(PtouchBridge),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_PTOUCH_BRIDGE_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.set_binary_sensor(config[CONF_KIND], var))

