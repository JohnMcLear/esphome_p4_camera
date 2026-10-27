"""ESP32-P4 MIPI-CSI camera for ESPHome.

Capture runs through Espressif's esp_video (CSI + ISP), the frame is decimated by 3
on the CPU and encoded by the P4's hardware JPEG engine. The result is an ordinary
ESPHome camera, so the native API (Home Assistant) and esp32_camera_web_server work
unchanged.
"""

from pathlib import Path

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
    only_on_variant,
)
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import CONF_I2C_ID, CONF_ID, CONF_RESET_PIN, CONF_SENSORS
from esphome.core.entity_helpers import setup_entity

CODEOWNERS = ["@JohnMcLear"]
DEPENDENCIES = ["esp32", "psram"]
AUTO_LOAD = ["camera"]

CONF_POWER_DOWN_PIN = "power_down_pin"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_MAX_FRAMERATE = "max_framerate"
CONF_IDLE_FRAMERATE = "idle_framerate"
CONF_IPA_JSON = "ipa_json"
CONF_INIT_LDO = "init_ldo"
CONF_ESP_VIDEO_VERSION = "esp_video_version"
CONF_STILL_JPEG_QUALITY = "still_jpeg_quality"
CONF_LOCKED = "locked"

esp32p4_camera_ns = cg.esphome_ns.namespace("esp32p4_camera")
ESP32P4Camera = esp32p4_camera_ns.class_("ESP32P4Camera", cg.Component, cg.EntityBase)
CaptureStillAction = esp32p4_camera_ns.class_(
    "CaptureStillAction", automation.Action, cg.Parented.template(ESP32P4Camera)
)
ExposureLockAction = esp32p4_camera_ns.class_(
    "ExposureLockAction", automation.Action, cg.Parented.template(ESP32P4Camera)
)

# Sensor name -> Kconfig stem. Each is pinned to its 1920x1080 30 fps mode, which is
# what the /3 decimation expects (640x360 out). Both have run on hardware in the ESP-IDF build
# this is ported from: OV5647 RAW10 and SC2336 RAW8.
SENSORS = {
    "sc2336": ("CAMERA_SC2336", "MIPI_RAW8_1920X1080_30FPS"),
    "ov5647": ("CAMERA_OV5647", "MIPI_RAW10_1920X1080_30FPS"),
}


def _validate_ipa(config):
    if CONF_IPA_JSON in config and len(config[CONF_SENSORS]) != 1:
        raise cv.Invalid(f"{CONF_IPA_JSON} needs exactly one entry in {CONF_SENSORS}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ESP32P4Camera),
            cv.GenerateID(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
            cv.Optional(CONF_SENSORS, default=["sc2336", "ov5647"]): cv.All(
                cv.ensure_list(cv.one_of(*SENSORS, lower=True)), cv.Length(min=1)
            ),
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_POWER_DOWN_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_INIT_LDO, default=True): cv.boolean,
            cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=1, max=100),
            cv.Optional(CONF_MAX_FRAMERATE, default="10 fps"): cv.All(
                cv.framerate, cv.Range(min=0, min_included=False, max=30)
            ),
            cv.Optional(CONF_IDLE_FRAMERATE, default="0.1 fps"): cv.All(
                cv.framerate, cv.Range(min=0, max=1)
            ),
            # Enables esp32p4_camera.capture_still: the next image is the full 1920x1080 frame.
            # Costs ~6 MB more PSRAM (full-frame UYVY buffer and two 1 MB JPEG slots).
            cv.Optional(CONF_STILL_JPEG_QUALITY): cv.int_range(min=1, max=100),
            cv.Optional(CONF_IPA_JSON): cv.file_,
            cv.Optional(CONF_ESP_VIDEO_VERSION, default="2.5.0"): cv.string,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32P4]),
    _validate_ipa,
)


async def to_code(config):
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)

    bus = await cg.get_variable(config[CONF_I2C_ID])
    cg.add(var.set_i2c_bus(bus))
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    if CONF_POWER_DOWN_PIN in config:
        cg.add(var.set_power_down_pin(config[CONF_POWER_DOWN_PIN]))
    cg.add(var.set_init_ldo(config[CONF_INIT_LDO]))
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    if CONF_STILL_JPEG_QUALITY in config:
        cg.add(var.set_still_jpeg_quality(config[CONF_STILL_JPEG_QUALITY]))
    cg.add(var.set_max_update_interval(int(1000 / config[CONF_MAX_FRAMERATE])))
    if config[CONF_IDLE_FRAMERATE] == 0:
        cg.add(var.set_idle_update_interval(0))
    else:
        cg.add(var.set_idle_update_interval(int(1000 / config[CONF_IDLE_FRAMERATE])))

    add_idf_component(name="espressif/esp_video", ref=config[CONF_ESP_VIDEO_VERSION])
    # Excluded by default from ESPHome 2026.10; esp_video needs the CSI driver.
    include_builtin_idf_component("esp_driver_cam")

    for name in config[CONF_SENSORS]:
        stem, mode = SENSORS[name]
        add_idf_sdkconfig_option(f"CONFIG_{stem}", True)
        add_idf_sdkconfig_option(
            f"CONFIG_{stem}_AUTO_DETECT_MIPI_INTERFACE_SENSOR", True
        )
        add_idf_sdkconfig_option(f"CONFIG_{stem}_{mode}", True)
        add_idf_sdkconfig_option(
            f"CONFIG_{stem}_MIPI_DEFAULT_FMT_{mode.removeprefix('MIPI_')}", True
        )
        if CONF_IPA_JSON in config:
            add_idf_sdkconfig_option(
                f"CONFIG_{stem}_DEFAULT_IPA_JSON_CONFIGURATION_FILE", False
            )
            add_idf_sdkconfig_option(
                f"CONFIG_{stem}_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE", True
            )
            add_idf_sdkconfig_option(
                f"CONFIG_{stem}_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH",
                str(Path(config[CONF_IPA_JSON]).resolve()),
            )

    # The SC2336 has no on-chip AWB: without the ISP pipeline controller the image is
    # heavily green. Its task stack goes to PSRAM to spare internal SRAM.
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER", True)
    add_idf_sdkconfig_option(
        "CONFIG_ISP_PIPELINE_CONTROLLER_TASK_STACK_USE_PSRAM", True
    )


@automation.register_action(
    "esp32p4_camera.capture_still",
    CaptureStillAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(ESP32P4Camera)}),
    synchronous=True,
)
async def capture_still_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "esp32p4_camera.set_exposure_lock",
    ExposureLockAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(ESP32P4Camera),
            cv.Required(CONF_LOCKED): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def exposure_lock_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    locked = await cg.templatable(config[CONF_LOCKED], args, bool)
    cg.add(var.set_locked(locked))
    return var
