"""H.264 recording from the ESP32-P4 camera to an SD card, as fragmented MP4."""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32p4_camera, microphone
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLK_PIN,
    CONF_DATA_PINS,
    CONF_DURATION,
    CONF_FREQUENCY,
    CONF_ID,
    CONF_MAX_DURATION,
    CONF_MICROPHONE,
    CONF_ON_START,
    CONF_TRIGGER_ID,
)

CODEOWNERS = ["@JohnMcLear"]
DEPENDENCIES = ["esp32p4_camera"]

CONF_CAMERA_ID = "camera_id"
CONF_SD_CARD = "sd_card"
CONF_CMD_PIN = "cmd_pin"
CONF_SLOT = "slot"
CONF_POWER_LDO_CHANNEL = "power_ldo_channel"
CONF_MOUNT_POINT = "mount_point"
CONF_FILE_PREFIX = "file_prefix"
CONF_BITRATE = "bitrate"
CONF_MIN_QP = "min_qp"
CONF_MAX_QP = "max_qp"
CONF_GOP = "gop"
CONF_ON_FINISHED = "on_finished"

esp32p4_recorder_ns = cg.esphome_ns.namespace("esp32p4_recorder")
ESP32P4Recorder = esp32p4_recorder_ns.class_("ESP32P4Recorder", cg.Component)
StartAction = esp32p4_recorder_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(ESP32P4Recorder)
)
StopAction = esp32p4_recorder_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(ESP32P4Recorder)
)
IsRecordingCondition = esp32p4_recorder_ns.class_(
    "IsRecordingCondition", automation.Condition, cg.Parented.template(ESP32P4Recorder)
)
StartTrigger = esp32p4_recorder_ns.class_(
    "StartTrigger", automation.Trigger.template(cg.std_string)
)
FinishedTrigger = esp32p4_recorder_ns.class_(
    "FinishedTrigger", automation.Trigger.template(cg.std_string)
)


def _validate_qp(config):
    if config[CONF_MIN_QP] > config[CONF_MAX_QP]:
        raise cv.Invalid(f"{CONF_MIN_QP} must not be above {CONF_MAX_QP}")
    return config


SD_CARD_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SLOT, default=0): cv.int_range(min=0, max=1),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DATA_PINS): cv.All(
            cv.ensure_list(pins.internal_gpio_pin_number),
            cv.Length(min=1, max=4),
        ),
        cv.Optional(CONF_FREQUENCY, default="20MHz"): cv.All(
            cv.frequency, cv.Range(min=400e3, max=40e6)
        ),
        cv.Optional(CONF_POWER_LDO_CHANNEL): cv.int_range(min=1, max=4),
    }
)


def _validate_data_pins(config):
    if len(config[CONF_SD_CARD][CONF_DATA_PINS]) not in (1, 4):
        raise cv.Invalid(f"{CONF_DATA_PINS} needs 1 or 4 pins")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ESP32P4Recorder),
            cv.GenerateID(CONF_CAMERA_ID): cv.use_id(esp32p4_camera.ESP32P4Camera),
            cv.Required(CONF_SD_CARD): SD_CARD_SCHEMA,
            # Optional audio track: AAC-LC, mono, at the microphone's sample rate.
            cv.Optional(CONF_MICROPHONE): microphone.microphone_source_schema(
                min_bits_per_sample=16, max_bits_per_sample=16
            ),
            cv.Optional(CONF_MOUNT_POINT, default="/sdcard"): cv.All(
                cv.string_strict, cv.Length(min=2, max=16)
            ),
            cv.Optional(CONF_FILE_PREFIX, default="rec"): cv.All(
                cv.string_strict, cv.Length(min=1, max=16)
            ),
            cv.Optional(CONF_BITRATE, default=4000000): cv.int_range(
                min=500000, max=20000000
            ),
            cv.Optional(CONF_MIN_QP, default=25): cv.int_range(min=1, max=51),
            cv.Optional(CONF_MAX_QP, default=45): cv.int_range(min=1, max=51),
            cv.Optional(CONF_GOP, default=30): cv.int_range(min=1, max=120),
            cv.Optional(CONF_MAX_DURATION, default="10min"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(max=cv.TimePeriod(hours=1)),
            ),
            cv.Optional(CONF_ON_START): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StartTrigger)}
            ),
            cv.Optional(CONF_ON_FINISHED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FinishedTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_qp,
    _validate_data_pins,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    camera = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(camera))
    # Registered before setup: the camera's capture task iterates its listeners without a lock.
    cg.add(camera.add_frame_listener(var))

    sd = config[CONF_SD_CARD]
    data = sd[CONF_DATA_PINS] + [-1] * (4 - len(sd[CONF_DATA_PINS]))
    cg.add(var.set_sd_pins(sd[CONF_CLK_PIN], sd[CONF_CMD_PIN], *data))
    cg.add(var.set_sd_slot(sd[CONF_SLOT]))
    cg.add(var.set_sd_frequency_khz(int(sd[CONF_FREQUENCY] / 1000)))
    if CONF_POWER_LDO_CHANNEL in sd:
        cg.add(var.set_sd_power_ldo_channel(sd[CONF_POWER_LDO_CHANNEL]))
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
    cg.add(var.set_file_prefix(config[CONF_FILE_PREFIX]))
    cg.add(var.set_bitrate(config[CONF_BITRATE]))
    cg.add(var.set_qp_range(config[CONF_MIN_QP], config[CONF_MAX_QP]))
    cg.add(var.set_gop(config[CONF_GOP]))
    cg.add(var.set_max_duration(config[CONF_MAX_DURATION].total_seconds))

    if mic_config := config.get(CONF_MICROPHONE):
        mic_source = await microphone.microphone_source_to_code(mic_config)
        cg.add(var.set_microphone_source(mic_source))
        cg.add_define("USE_ESP32P4_RECORDER_AUDIO")
        # 2.5.x: from 2.6 the library needs P4 silicon rev 3.0 or later.
        add_idf_component(name="espressif/esp_audio_codec", ref="2.5.0")

    for conf in config.get(CONF_ON_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "file")], conf)
    for conf in config.get(CONF_ON_FINISHED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "file")], conf)

    include_builtin_idf_component("sdmmc")
    include_builtin_idf_component("esp_driver_sdmmc")
    include_builtin_idf_component("fatfs")
    include_builtin_idf_component("wear_levelling")
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE", True)
    # Long file names, and FAT writes in 4 KB blocks.
    add_idf_sdkconfig_option("CONFIG_FATFS_LFN_HEAP", True)
    add_idf_sdkconfig_option("CONFIG_FATFS_VFS_FSTAT_BLKSIZE", 4096)


FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Optional(
            CONF_MICROPHONE
        ): microphone.final_validate_microphone_source_schema(
            "esp32p4_recorder", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


RECORDER_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(ESP32P4Recorder)}
)


@automation.register_action(
    "esp32p4_recorder.start",
    StartAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(ESP32P4Recorder),
            cv.Optional(CONF_DURATION, default="60s"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def start_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    duration = await cg.templatable(config[CONF_DURATION], args, cg.uint32)
    cg.add(var.set_duration(duration))
    return var


@automation.register_action(
    "esp32p4_recorder.stop", StopAction, RECORDER_ACTION_SCHEMA, synchronous=True
)
async def stop_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "esp32p4_recorder.is_recording", IsRecordingCondition, RECORDER_ACTION_SCHEMA
)
async def is_recording_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
