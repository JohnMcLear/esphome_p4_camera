# ESP32-P4 camera and recorder for ESPHome

Two ESPHome external components for the ESP32-P4 MIPI-CSI camera port:

- **`esp32p4_camera`**: an ordinary ESPHome `camera`, so the Home Assistant camera entity and
  `esp32_camera_web_server` (MJPEG stream and snapshot) work unchanged.
- **`esp32p4_recorder`**: 1080p H.264 recordings from the same camera to an SD card, as fragmented
  MP4, with an optional AAC track from an ESPHome microphone. A clip cut short by a power loss still
  plays up to its last fragment.

```
sensor 1920x1080 -> CSI + ISP (esp_video) -> YUV420 --+-- capture task: H.264 (hardware) -> PSRAM ring -> writer task -> fMP4 -> SD
                                                      '-- JPEG task: decimate /3 -> 640x360 -> hardware JPEG -> API / web
```

The capture task never waits for the JPEG path, and drains to the newest frame if a listener makes it
fall behind. JPEG frames are only made when someone asked for one.

## Status

| Item | State |
|---|---|
| Compiles, ESPHome 2026.7, ESP-IDF 5.5.4, esp_video 2.5.0 | yes, 25 Oct 2026, and in CI on every push |
| ESPHome lint: clang-format, clang-tidy, ruff, ci-custom | clean, 25 Oct 2026 |
| fMP4 muxer | host test passes (`tests/host/run.sh`), output byte-identical to the ESP-IDF original |
| Runs on hardware | **unproven**, not flashed yet |
| Pipeline itself | ported from an ESP-IDF build that has run on the same dev kit; no public logs yet |

## Camera

```yaml
i2c:
  id: bus_a
  sda: GPIO7
  scl: GPIO8

esp32p4_camera:
  id: door_cam
  name: Door camera
  i2c_id: bus_a              # the sensor's SCCB bus, shared with other I2C parts if need be
  sensors: [sc2336]          # sc2336 and/or ov5647, auto-detected at boot
  ipa_json: ipa/sc2336_yondoor.json   # optional ISP tuning, needs exactly one sensor
  jpeg_quality: 40
  max_framerate: 10 fps
  idle_framerate: 0.1 fps
  # reset_pin, power_down_pin, init_ldo: true
```

Needs `psram:` and `esp32: variant: esp32p4`. A camera that does not answer at boot marks the
component failed and leaves the rest of the device running; it never boot-loops.

## Recorder

```yaml
esp32p4_recorder:
  id: recorder
  camera_id: door_cam
  sd_card:
    clk_pin: GPIO43
    cmd_pin: GPIO44
    data_pins: [GPIO39, GPIO40, GPIO41, GPIO42]   # 1 or 4
    power_ldo_channel: 4     # on-chip LDO feeding the card, if the board uses one
    # slot: 0, frequency: 20MHz
  bitrate: 4000000
  min_qp: 25
  max_qp: 45
  gop: 30                    # one fragment per GOP
  microphone: door_mic       # optional: AAC-LC mono track, needs a 16 kHz microphone
  max_duration: 10min
  on_start:
    - logger.log: { format: "Recording %s", args: [file.c_str()] }
  on_finished:
    - logger.log: { format: "Saved %s", args: [file.c_str()] }
```

Actions and condition: `esp32p4_recorder.start` (with `duration`, templatable),
`esp32p4_recorder.stop`, `esp32p4_recorder.is_recording`. Files are `<prefix>-0001.mp4` and up.

The card is never formatted. On boards where `esp32_hosted` puts the ESP32-C6 on SDMMC slot 1, the
card goes on slot 0; the recorder mounts after Wi-Fi is up and never de-initialises the shared host,
so a bad card cannot take the Wi-Fi link down.

## Example

`examples/waveshare-p4-devkit.yaml` is a complete Waveshare ESP32-P4-Module-DEV-KIT build: Wi-Fi
through the on-board C6, camera, recorder, I2S mic and ES8311 talk-back. The audio side is stock
ESPHome (`i2s_audio`, `es8311`), see `examples/p4-devkit-audio.yaml`.

## Not yet

- Measured A/V alignment. Audio starts at the first IDR frame and ends at the last video frame, as in
  the ESP-IDF build, but sample times come from the microphone callback here, so any constant capture
  latency is not corrected. It needs a clap test on hardware.
- Camera output sizes other than 640x360.

## Licence

MIT, see `LICENSE`. The exception is `ipa/sc2336_yondoor.json`, a modified copy of an Espressif
esp_cam_sensor file under the Apache License 2.0; see `ipa/README.md`.

`.clang-format`, `.clang-tidy` and the ruff settings in `pyproject.toml` are taken from
[ESPHome](https://github.com/esphome/esphome) so the code follows its style.
