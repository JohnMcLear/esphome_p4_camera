# ESPHome P4 camera, first light on the Waveshare ESP32-P4-NANO (31 Oct 2026)

Build: e5ea2d1 + `examples/waveshare-p4-nano.yaml` (rev 3 silicon, `CONFIG_P4_REV3_MSPI_*` as the IDF Nano profile,
logger on UART0) + a write-error log line in the recorder. ESPHome 2026.10.0 (2026.7 rejects `engineering_sample: false`
on board esp32-p4). Bench copy with real Wi-Fi: `~/doorplan/bench/p4-nano/` (secrets outside the repo). Board on
/dev/ttyACM0 (CH343, UART0). Flash approved by John (JA, 13:48).

## Shown, with logs
- **Boot loop, found and fixed.** With the logger at DEBUG, the IPA's per-frame ESP-IDF debug lines (esp_ipa_agc, from
  `isp_task` on core 1) were written synchronously to UART0 and starved `loopTask`: task WDT on core 1 every ~10 s
  (`boot2_serial.log`, backtrace in `uart_tx_all` from `log_vprintf_non_main_thread_`). `logger: logs: esp-idf: WARN`
  fixed it: one POWERON reset in 90 s (`boot3_serial.log`) and since. **Component bug to fix properly:** ESPHome raises
  the IDF log level to the logger level, so any user with logger DEBUG gets this. The component should pin the IPA tags
  to WARN itself.
- Wi-Fi via the C6 joined, IP 10.0.0.147 (`boot4_serial.log`).
- Snapshot :8081: HTTP 200, 640x360 JPEG, 21.5 kB, 65 ms. MJPEG :8080: 86 frames in 10 s (~8.6 fps, max_framerate 10).
  Frames show John: kept in `~/yondoor_private/nano-first-light-2026-10-31/`, not here.
- Encoder: 60 s clip, 1628 frames, 27.1 fps, 0 stalls, 0 encode errors (`rec60_serial.log`).

## Not working
- **SD writes fail: every 64 kB write returns -1, EIO** (`rec60b_serial.log`), so clips are 0 bytes (420 write errors).
  Mount at 20 MHz 4-bit worked on 3 of 3 boots with the card in (30 GB, label "asdfg"); at 10 MHz the mount timed out
  (`rec60c_10mhz_serial.log`), so it is not simply bus speed. SD on the Nano was never proven by the IDF build either
  (19 Oct: no card fitted). **A second card (29.8 GB, label "SD") fails the same way** (`rec60d_card2_serial.log`,
  424 write errors), so it is not the card. Suspects: the SDMMC controller shared with the C6 link on slot 1 (writes
  happen while Wi-Fi is busy; mount happens at boot), LDO VO4 under write current. Next (JB, needs John's yes): the IDF
  build's recorder on the Nano with the same card, to split board from component.

## Not yet tested
Boot with the camera unplugged; boot with no card; A/V sync; mic and talk-back (old IDF firmware read the mic at
-180 dBFS, so no mic is fitted); 1080p still (e5ea2d1 feature); Wi-Fi throughput while recording.

## Bench SD test on the Nano (15:00, second card back in)
Same `-DYD_SD_BENCH_TEST` build as the dev kit (`../devkit-sd-2026-10-31/`). The Nano boot-looped on task WDT
(`sdbench_nano_serial.log`): the first 64 kB write blocked for over 5 s, with no "chunk 0 took" line. The WDT is fed
before every chunk, so one write is stalling (the dev kit does a chunk in 75-105 ms). This runs in setup, before Wi-Fi
joins, so contention with the C6 link is unlikely to be the whole story. Suspects now: the Nano's SD hardware or LDO
VO4 under write load, or rev-3 silicon in the IDF 5.5 SDMMC driver. Board returned to the normal build at once (no
bench flag): one reset, Wi-Fi up (`boot9_serial.log`).
Next (JB): Waveshare's own SD example or the IDF camera build on the Nano with this card.
