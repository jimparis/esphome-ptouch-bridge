import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

CODEOWNERS = ["@jimparis"]
DEPENDENCIES = ["esp32", "wifi"]
AUTO_LOAD = ["binary_sensor", "button", "sensor", "text_sensor"]

CONF_PRINTER_ADDRESS = "printer_address"
CONF_HTTP_TOKEN = "http_token"

ptouch_bridge_ns = cg.esphome_ns.namespace("ptouch_bridge")
PtouchBridge = ptouch_bridge_ns.class_("PtouchBridge", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PtouchBridge),
        cv.Required(CONF_PRINTER_ADDRESS): cv.mac_address,
        cv.Optional(CONF_HTTP_TOKEN, default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_printer_address(str(config[CONF_PRINTER_ADDRESS])))
    cg.add(var.set_http_token(config[CONF_HTTP_TOKEN]))

    for component in ("bt", "esp_http_server", "json"):
        esp32.include_builtin_idf_component(component)

    for option, value in {
        "CONFIG_BT_ENABLED": True,
        "CONFIG_BT_BLUEDROID_ENABLED": True,
        "CONFIG_BT_CLASSIC_ENABLED": True,
        "CONFIG_BT_SPP_ENABLED": True,
        "CONFIG_BT_BLE_ENABLED": False,
        "CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY": True,
        "CONFIG_BTDM_CTRL_MODE_BTDM": False,
        "CONFIG_BTDM_CTRL_MODE_BLE_ONLY": False,
        "CONFIG_ESP_COEX_SW_COEXIST_ENABLE": True,
    }.items():
        esp32.add_idf_sdkconfig_option(option, value)

