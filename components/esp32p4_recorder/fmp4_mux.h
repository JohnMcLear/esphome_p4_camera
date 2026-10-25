/* Minimal fragmented MP4 writer, H.264 (Annex B in) + AAC-LC (raw frames in).
 * Plain C++, no RTOS calls, so it builds and is tested on the PC too (tests/host/run.sh).
 *
 * Layout: ftyp, moov (with mvex, durations patched at the end), then one moof+mdat per GOP.
 * A clip cut short (power loss, crash) keeps every fragment that reached the card; the durations
 * in moov then stay 0 ("unknown") and players work them out from the fragments. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace esphome::esp32p4_recorder {

static constexpr int FMP4_MAX_VFRAMES = 160; /* per fragment; a fragment is forced at this many */
static constexpr int FMP4_MAX_AFRAMES = 256;
static constexpr size_t FMP4_ABUF = 48 * 1024;

using fmp4_out_fn = void (*)(void *ctx, const void *p, size_t n);
/* write n bytes at absolute file offset off (used once, at finish, to patch durations) */
using fmp4_patch_fn = void (*)(void *ctx, uint64_t off, const void *p, size_t n);

struct Fmp4VideoFrame {
  uint8_t *p; /* sample data, AVCC (length prefixed), converted in place from Annex B */
  uint32_t len;
  int64_t t_us;
  bool key;
};

struct Fmp4Muxer {
  fmp4_out_fn out;
  void *ctx;
  uint32_t width, height;
  bool audio;
  uint32_t a_rate;  /* 16000 */
  uint32_t a_delay; /* encoder priming in samples, written as an edit list */
  /* state */
  uint8_t sps[128], pps[64];
  size_t sps_len, pps_len;
  bool init_done;
  uint64_t pos; /* bytes emitted so far */
  uint32_t seq;
  int64_t t0_us;  /* first video frame */
  uint64_t v_dts; /* 90 kHz, of the next fragment's first frame */
  uint64_t a_dts; /* samples emitted so far */
  Fmp4VideoFrame vf[FMP4_MAX_VFRAMES];
  int nv;
  uint8_t abuf[FMP4_ABUF];
  size_t alen;
  uint16_t asz[FMP4_MAX_AFRAMES];
  int na;
  uint32_t fragments;
  uint64_t a_len;
  /* segment index (sidx), written at finish into space reserved after moov. Without it Chrome's demuxer
   * walks every fragment over HTTP before it starts (20 s for a 200 s clip); with it, it reads one box. */
  uint32_t max_frags, nfr;
  uint32_t *fr_size, *fr_dur, *fr_adur;
  uint64_t off_sidx; /* real audio length in samples, if known (set before finish): trims the edit */
  /* patch points in the init segment */
  uint64_t off_mvhd_dur, off_tkhd_dur[2], off_mdhd_dur[2], off_mehd_dur, off_elst_dur;
};

void fmp4_begin(Fmp4Muxer *m, fmp4_out_fn out, void *ctx, uint32_t w, uint32_t h, bool audio, uint32_t a_rate,
                uint32_t a_delay, uint32_t max_frags);
constexpr uint32_t fmp4_sidx_reserve(uint32_t n) {
  return 2 * (32 + 12 * n) + 8;
} /* one sidx per track: Chrome wants both */
/* Annex B frame, converted in place: the buffer must stay valid until the fragment holding it is emitted.
 * Returns true if the caller should call fmp4_fragment() BEFORE adding this frame (it is a keyframe and
 * frames are pending, or the fragment is full). It does not add the frame in that case; call again after. */
bool fmp4_video_needs_cut(Fmp4Muxer *m, const uint8_t *annexb, size_t len);
void fmp4_video(Fmp4Muxer *m, uint8_t *p, size_t n, int64_t t_us);
bool fmp4_audio(Fmp4Muxer *m, const uint8_t *aac, size_t len); /* one raw AAC frame (1024 samples) */
/* Emit pending frames as one fragment. next_t_us is the time of the frame after the last one (sets its duration). */
void fmp4_fragment(Fmp4Muxer *m, int64_t next_t_us);
/* Patch the durations into moov. Call after the last fragment. */
void fmp4_finish(Fmp4Muxer *m, fmp4_patch_fn patch);
size_t fmp4_pending_bytes(const Fmp4Muxer *m);

/* Power-cut repair. A clip that was never finished has 0 durations in moov and may end in a partial
 * fragment (the card keeps what the last fsync committed). This walks the fragments, truncates the file
 * after the last complete moof+mdat pair and patches the durations, so browsers show the right length and
 * do not stop on a decode error at the cut. Returns 0 if the file was already finished, 1 if repaired,
 * negative on error. *kept_ms gets the repaired length. Works with stdio, so it runs on the PC too. */
int fmp4_repair(const char *path, uint32_t *kept_ms, uint64_t *cut_bytes);

}  // namespace esphome::esp32p4_recorder
