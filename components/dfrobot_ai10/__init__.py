import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome import automation

DEPENDENCIES = ["uart"]
AUTO_LOAD = []

CONF_ON_RECOGNIZED = "on_recognized"
CONF_ON_QR_SCANNED = "on_qr_scanned"  # NEU

dfrobot_ai10_ns = cg.esphome_ns.namespace("dfrobot_ai10")
DFRobotAI10Component = dfrobot_ai10_ns.class_(
    "DFRobotAI10Component", cg.Component, uart.UARTDevice
)

# Auth Trigger: Liefert (name, uid)
RecognizedTrigger = dfrobot_ai10_ns.class_(
    "RecognizedTrigger", automation.Trigger.template(cg.std_string, cg.uint16)
)

# NEU: QR Trigger: Liefert (data)
QRScannedTrigger = dfrobot_ai10_ns.class_(
    "QRScannedTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DFRobotAI10Component),
        
        # Trigger für Gesicht/Hand
        cv.Optional(CONF_ON_RECOGNIZED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RecognizedTrigger),
            }
        ),

        # NEU: Trigger für QR-Codes
        cv.Optional(CONF_ON_QR_SCANNED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(QRScannedTrigger),
            }
        ),
    }
).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    for conf in config.get(CONF_ON_RECOGNIZED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "name"), (cg.uint16, "uid")], conf
        )

    for conf in config.get(CONF_ON_QR_SCANNED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "qr_data")], conf
        )