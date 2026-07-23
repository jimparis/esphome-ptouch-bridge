import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import PtouchBridge, ptouch_bridge_ns

CONF_PTOUCH_BRIDGE_ID = "ptouch_bridge_id"
CONF_KIND = "kind"

KINDS = {
    "reconnect": 0,
    "disconnect": 1,
    "refresh_status": 2,
}

PtouchBridgeButton = ptouch_bridge_ns.class_("PtouchBridgeButton", button.Button)

CONFIG_SCHEMA = button.button_schema(PtouchBridgeButton).extend(
    {
        cv.Required(CONF_PTOUCH_BRIDGE_ID): cv.use_id(PtouchBridge),
        cv.Required(CONF_KIND): cv.enum(KINDS, lower=True, space="_"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_PTOUCH_BRIDGE_ID])
    await button.new_button(config, parent, config[CONF_KIND])

