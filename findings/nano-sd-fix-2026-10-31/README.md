# Nano SD writes: found and fixed (31 Oct 2026)

## Cause
On the Waveshare ESP32-P4-NANO (rev 3), SDMMC slot 0 at **20 MHz 4-bit cannot write**: reads run at 8.6 MB/s, but
after the first ~32 kB every write fails the CRC status (`sdmmc_write_sectors_dma ... 0x109`, `send_op 0x107`). Shown
with plain ESP-IDF 5.5.4, no ESPHome and no Wi-Fi (`m0l.log`, test app `~/esp/yondoor-sd-coexist`, Nano build in
`build_nano/`). The same code writes fine on the rev 1.3 dev kit (`../devkit-sd-2026-10-31/`). So it is the Nano's SD
path at 20 MHz (board layout or rev-3 pads), not ESPHome and not the C6 link.

## Sweep (each run 30-60 s of flat-out 32 kB writes, LDO VO4 on; logs here)
| Mode | Write errors | Avg write |
|---|---|---|
| 20 MHz 4-bit, input delay phase 0/1/2/3 (`m0la`-`m0ld`) | 429-437 | 32 kB/s (fails) |
| 10 MHz 4-bit (`m0lh`) | 0 | 651 kB/s |
| 5 MHz 4-bit (`m0ls`) | 0 | 608 kB/s |
| 20 MHz 1-bit (`m0lw`) | 0 | 546 kB/s |
| 40 MHz 1-bit (`m0lwf`) | 0 | 653 kB/s |
| 10 MHz 4-bit + Wi-Fi up, rate 600 kB/s (`m2lhr`) | 0 | 584 kB/s, HTTP stream 386 kB/s |
| 10 MHz 4-bit + Wi-Fi up, flat out (`m2lh`) | 0 | 637 kB/s |
Rates look card-limited (the same ~600-650 kB/s whatever the clock), so 10 MHz costs nothing. A 4 Mbit/s clip needs 500.

## Fix in ESPHome
- `examples/waveshare-p4-nano.yaml`: `sd_card: frequency: 10MHz`.
- `components/esp32p4_recorder/__init__.py`: `CONFIG_VFS_SUPPORT_DIR` on. ESPHome 2026.10 turns it off, so `stat()` and
  `opendir()` returned ENOSYS: every clip was named rec-0001 (overwriting the last) and the logged size was always 0.

## Result (`esphome_rec_serial.log`)
Two 60 s clips, back to back: rec-0002.mp4 28,289,554 bytes (1669 frames, 27.8 fps) and rec-0003.mp4 28,066,740 bytes,
0 stalls, **0 write errors**, 0 encode errors. Numbering now advances.
**Playback (card read on the PC, 16:15):** both play. rec-0002: H.264 1920x1080, 1669 frames decoded (matches the
recorder's count), 60.07 s video, AAC 16 kHz mono 60.16 s, 3.76 Mbit/s; rec-0003: 1659 frames, 59.64 / 59.71 s.
Container DTS strictly increasing, largest frame gap 75 ms (no stalls). ffmpeg's null muxer prints ~9 "non monotonically
increasing dts" warnings per clip when it rescales the timestamps; the file's own DTS has no repeats, so worth a look
at the mux timescale, not a playback fault. Audio is silent (-91 dB): no mic is fitted on this Nano.
Clips (frames may show John) are in ~/yondoor_private/nano-sd-fix-2026-10-31/, not here.
Note: the card is a Raspberry Pi boot card; FATFS mounted its 512 MB bootfs partition, which is now 77% full.
