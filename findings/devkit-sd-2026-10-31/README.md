# SD write test on the Waveshare P4 dev kit (rev 1.3), 31 Oct 2026

John swapped the Nano for the dev kit to test the card path (he'd rather not move the finicky camera ribbon, so no
camera is fitted). Bench config `~/doorplan/bench/p4-devkit/p4-camera.yaml` with `-DYD_SD_BENCH_TEST`: the recorder
mounts the card and writes 64 x 64 kB at setup, before the camera check and before Wi-Fi is up.

- No camera fitted: no boot loop; camera, recorder and web server are marked failed and the rest runs
  (`rec60_serial.log`, one reset, Wi-Fi up at 10.0.0.90).
- The first bench build boot-looped: 64 writes in setup ran past the 5 s task WDT (`sdbench_serial.log`). Fixed with
  `App.feed_wdt()` per chunk.
- **Dev kit, same SDMMC settings as the Nano (slot 0, 4-bit, 20 MHz, LDO VO4): 64 of 64 writes OK, fsync 0, close 0**,
  75-105 ms per 64 kB, about 0.5-0.8 MB/s (`sdbench2_serial.log`, `sdbench3_serial.log`).
- `stat()` returns -1 ENOSYS in this build, so the recorder's "N bytes" in its Finished line is always 0 and means
  nothing. The Nano's 420 write errors were real (each write returned -1 EIO); its "0 bytes" was not a measurement.

So the card path works on the dev kit and fails on the Nano with the same code. Next: the same bench test on the Nano.
It runs before Wi-Fi, so it splits the Nano's SD hardware/rev-3 silicon from contention with the C6 link.
