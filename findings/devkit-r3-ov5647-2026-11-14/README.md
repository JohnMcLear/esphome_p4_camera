# Dev kit, P4 rev 3.2 silicon, OV5647: ESPHome build bench test (14 Nov 2026)

Board: Waveshare ESP32-P4 dev kit, chip rev v3.2 (ECO7), Arducam OV5647, SD card fitted ("SB32G", 30436 MB).
Config: examples/waveshare-p4-devkit.yaml with the rev 3 sdkconfig options (CONFIG_ESP32P4_SELECTS_REV_LESS_V3: n,
CONFIG_ESP32P4_REV_MIN_300: y, cpu_frequency 360MHz, no engineering_sample), sensors: [ov5647], plus the dev kit
audio package. Components: this repo, branch full-res-still with the still server.
Logs here have the network name and MAC addresses redacted and register/stack dumps removed.

## Results
1. FAIL, boot loop with -DYD_SD_BENCH_TEST (boot1.log): task watchdog on loopTask about 16 s into every boot,
   five resets in 45 s. No SD log line before the watchdog. Removing the flag gives a clean boot (boot2.log:
   one boot in 60 s). A boot-time card write must never be in a product build; the bench flag is the only path
   that does it, but the stall itself is not explained.
2. PASS, camera: snapshot :8081 returned a 640x360 JPEG, still server :8082 a 1920x1080 JPEG (896 kB).
   Frames not kept here. Image content not inspected.
3. FAIL at 20 MHz (record1.log): 60 s recording ran, 1604 frames, 26.71 fps, no reset, but every card write
   returned EIO: 408 write errors, 0-byte file.
4. PASS at 10 MHz (record2_10mhz.log): rec-0002.mp4, 1602 frames over 60.0 s (26.67 fps), 27,240,482 bytes,
   0 stalls, 0 write errors, 0 encode errors.
   This is the same fault and the same fix as the P4-NANO on 31 Oct. On 31 Oct the older dev kit (rev 1.3)
   wrote 64/64 chunks at 20 MHz, so 20 MHz is not safe as the component default.
5. Wi-Fi: the first association attempt fails, a retry connects, on every boot seen.

6. PASS, live view while recording (record3_stream_while_recording.log): MJPEG on :8080 for 30 s gave 8.1 fps,
   1444 kbit/s idle and 8.2 fps, 1465 kbit/s during a recording; that clip (rec-0003.mp4) finished with 1537 frames
   (25.60 fps), 25,920,710 bytes, 0 stalls, 0 write errors. The stream is frame-rate limited, so this shows
   recording does not degrade the live view, not the maximum Wi-Fi throughput.

7. Live view with max_framerate raised from 10 to 30 fps (record4_stream_cap30.log): 16.2 fps, 2806 kbit/s idle;
   14.1 fps, 2413 kbit/s during a recording. That clip (rec-0004.mp4): 1496 frames (24.91 fps), 25,307,956 bytes,
   0 stalls, 0 write errors. So about 16 fps is what this 640x360 MJPEG path delivers over Wi-Fi here, and the
   recording loses about 1 fps to it. What limits it (JPEG encode, the web server, or Wi-Fi) is not measured.

## Not shown
- The clip playing (card has to come out for ffprobe).
- Maximum Wi-Fi throughput while recording, A/V sync, mic, talk-back (the audio components set up without error; whether
  a mic or speaker is wired is unknown, so the AAC track may be silence).
- Boot with the camera unplugged or the card removed on this unit.

## To do
- Make 10 MHz the recorder's default frequency, or step down automatically on write CRC errors.
- Find why the boot-time bench write stalls instead of failing with EIO.
