#!/usr/bin/env bash
# PC test of the fMP4 muxer: synthetic H.264 + AAC through fmp4_mux.c, then ffprobe/ffmpeg must
# read every frame back with no decode errors, both for a finished clip and for one cut short
# (no finish call, as after a power cut). Needs cc, ffmpeg, ffprobe.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../components/esp32p4_recorder
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
c++ -std=c++20 -O2 -I"$src" "$here/fmp4_host_test.cpp" "$src/fmp4_mux.cpp" -o fmp4_host_test
ffmpeg -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 6 -c:v libx264 -threads 1 \
  -profile:v baseline -g 30 -bf 0 -x264-params sei=0 -bsf:v h264_mp4toannexb -f h264 raw.h264
# The P4 encoder writes 4-byte start codes and the muxer relies on that; x264 mixes in 3-byte ones.
python3 -c "import re,sys; d=open('raw.h264','rb').read(); open('in.h264','wb').write(re.sub(rb'(?<!\x00)\x00\x00\x01', b'\x00\x00\x00\x01', d))"
ffmpeg -loglevel error -f lavfi -i sine=frequency=440:sample_rate=16000 -t 6 -ac 1 -c:a aac -b:a 32k -f adts in.adts
fail=0
for mode in finished cut; do
  if [ $mode = cut ]; then export NOFINISH=1; fi
  ./fmp4_host_test in.h264 in.adts $mode.mp4 30 1600
  frames=$(ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 $mode.mp4)
  errors=$(ffmpeg -v error -i $mode.mp4 -f null - 2>&1 | wc -l)
  echo "$mode: $frames video frames, $errors decode errors"
  [ "$frames" = 180 ] && [ "$errors" = 0 ] || fail=1
done
[ $fail = 0 ] && echo PASS || { echo FAIL; exit 1; }
