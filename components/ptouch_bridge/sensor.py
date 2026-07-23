import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor

from . import PtouchBridge

CONF_PTOUCH_BRIDGE_ID = "ptouch_bridge_id"
CONF_KIND = "kind"

KINDS = {
    "labels_printed": 0,
    "tape_used": 1,
    "failed_prints": 2,
    "bluetooth_connections": 3,
    "connection_attempts": 4,
    "bluetooth_recoveries": 5,
}

CONFIG_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required(CONF_PTOUCH_BRIDGE_ID): cv.use_id(PtouchBridge),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_PTOUCH_BRIDGE_ID])
    var = await sensor.new_sensor(config)
    cg.add(parent.set_numeric_sensor(config[CONF_KIND], var))
