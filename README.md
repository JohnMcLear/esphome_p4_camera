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
| Compiles | CI, every push: both component tests and the dev kit example, on ESPHome 2026.7.0 and 2026.10.0 (ESP-IDF, `esp_video` 2.5.0) |
| Lint | CI, every push: `ruff format --check`, `ruff check`, `clang-format` |
| fMP4 muxer | CI, every push: host test (`tests/host/run.sh`) muxes synthetic H.264 + AAC, then `ffprobe`/`ffmpeg` must read all 180 frames back with no decode errors, for a finished clip and for one cut short |
| Runs on hardware | **unproven**: this component has not been flashed yet |
| Pipeline itself | an earlier standalone ESP-IDF version has run on the same dev kit; no logs are published |

Requirements: ESPHome 2026.7.0 or later (oldest version tested), ESP-IDF framework, ESP32-P4 with
PSRAM.

## Installation

```yaml
external_components:
  - source: github://JohnMcLear/esphome_p4_camera
    components: [esp32p4_camera, esp32p4_recorder]
```

A local checkout works too: `source: { type: local, path: path/to/esphome_p4_camera/components }`.

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
  ipa_json: sc2336_yondoor.json   # optional ISP tuning file, relative to this YAML; needs exactly one sensor
  jpeg_quality: 40
  max_framerate: 10 fps
  idle_framerate: 0.1 fps
  # reset_pin, power_down_pin, init_ldo: true, esp_video_version: 2.5.0
```

`sensors` defaults to both. `ipa_json` takes a local file: copy `ipa/sc2336_yondoor.json` from this
repository next to your YAML to use it. Needs `psram:` and `esp32: variant: esp32p4`.

A camera that does not answer at boot is meant to mark the component failed and leave the rest of
the device running rather than boot-loop. That is by design and not yet shown on hardware.

## Stills

`still_jpeg_quality` enables full-frame stills: after `esp32p4_camera.capture_still`, the next image the
camera delivers (to Home Assistant, the web server snapshot, or a stream) is the full 1920x1080 frame
instead of the 640x360 one. It costs about 6 MB more PSRAM. Larger stills are not possible yet:
`esp_cam_sensor` 2.6.0 has no full-sensor mode for either sensor (OV5647 tops out at a 1920x1080 crop).

`esp32p4_camera.set_exposure_lock` freezes the ISP's auto exposure and gain so a burst of stills, a
focus sweep for instance, all share one exposure. `esp_video` 2.5.0 has no runtime AWB lock; for
fixed white balance, use an `ipa_json` with fixed gains.

```yaml
esp32p4_camera:
  # ...
  still_jpeg_quality: 90

button:
  - platform: template
    name: Still
    on_press:
      - esp32p4_camera.set_exposure_lock:
          locked: true
      - esp32p4_camera.capture_still
```

Then take a snapshot as usual (`camera.snapshot` in Home Assistant). Untested on hardware, like the rest.

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
  max_duration: 10min        # at most 1 h
  # mount_point: /sdcard, file_prefix: rec
  on_start:
    - logger.log: { format: "Recording %s", args: [file.c_str()] }
  on_finished:
    - logger.log: { format: "Saved %s", args: [file.c_str()] }
```

Actions and condition: `esp32p4_recorder.start` (with `duration`, templatable, default 60 s),
`esp32p4_recorder.stop`, `esp32p4_recorder.is_recording`. Files are `<file_prefix>-0001.mp4` and up.

The card is never formatted. On boards where `esp32_hosted` puts the ESP32-C6 on SDMMC slot 1, the
card goes on slot 0; the recorder mounts after Wi-Fi is up and never de-initialises the shared host,
so that a bad card does not take the Wi-Fi link down.

## Example

`examples/waveshare-p4-devkit.yaml` is a complete Waveshare ESP32-P4-Module-DEV-KIT config: Wi-Fi
through the on-board C6, camera, recorder, I2S mic and ES8311 talk-back. The audio side is stock
ESPHome (`i2s_audio`, `es8311`), see `examples/p4-devkit-audio.yaml`.

## Not yet

- Measured A/V alignment. Audio starts at the first IDR frame and ends at the last video frame, but
  sample times come from the microphone callback, so any constant capture latency is not corrected. It needs a clap test on hardware.
- Camera output sizes other than 640x360.

## Licence

MIT, see `LICENSE`. The exception is `ipa/sc2336_yondoor.json`, a modified copy of an Espressif
esp_cam_sensor file under the Apache License 2.0; see `ipa/README.md`.

`.clang-format`, `.clang-tidy` and the ruff settings in `pyproject.toml` are taken from
[ESPHome](https://github.com/esphome/esphome) so the code follows its style.
