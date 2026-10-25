/* PC test of the fMP4 muxer: H.264 Annex B file + ADTS AAC file -> fragmented MP4.
 * Built and run by run.sh, which checks the output with ffprobe.
 * ./fmp4_host_test in.h264 in.adts out.mp4 fps a_delay [jitter_us] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmp4_mux.h"
using namespace esphome::esp32p4_recorder;
static FILE *fo;
static void out(void *c, const void *p, size_t n) { fwrite(p, 1, n, fo); }
static void patch(void *c, uint64_t off, const void *p, size_t n) {
  long cur = ftell(fo);
  fseek(fo, off, SEEK_SET);
  fwrite(p, 1, n, fo);
  fseek(fo, cur, SEEK_SET);
}
static uint8_t *slurp(const char *f, size_t *n) {
  FILE *x = fopen(f, "rb");
  fseek(x, 0, SEEK_END);
  *n = ftell(x);
  rewind(x);
  auto *b = static_cast<uint8_t *>(malloc(*n));
  if (fread(b, 1, *n, x) != *n)
    *n = 0;
  fclose(x);
  return b;
}
int main(int argc, char **argv) {
  size_t vn, an;
  uint8_t *v = slurp(argv[1], &vn), *a = slurp(argv[2], &an);
  double fps = atof(argv[4]);
  int delay = atoi(argv[5]);
  fo = fopen(argv[3], "wb");
  static Fmp4Muxer m;
  fmp4_begin(&m, out, NULL, 1920, 1080, 1, 16000, delay, 64);
  /* split into access units: each starts at an SPS or a slice (this encoder: SPS PPS IDR | P) */
  size_t starts[20000];
  int ns = 0;
  int prevt = -1;
  for (size_t i = 0; i + 4 < vn; i++)
    if (!v[i] && !v[i + 1] && !v[i + 2] && v[i + 3] == 1) {
      int t = v[i + 4] & 0x1f;
      if (t == 7 || ((t == 1 || t == 5) && prevt != 8))
        starts[ns++] = i;
      prevt = t;
    }
  starts[ns] = vn;
  size_t ap = 0;
  long asamp = 0;
  int nf = 0;
  for (int k = 0; k < ns; k++) {
    int64_t t = (int64_t) (k * 1e6 / fps) - (argc > 6 && (rand() & 1) ? atoi(argv[6]) : 0);
    uint8_t *p = v + starts[k];
    size_t len = starts[k + 1] - starts[k];
    /* audio up to this time */
    while (ap + 7 < an && asamp * 1000000LL / 16000 < t) {
      size_t fl = ((a[ap + 3] & 3) << 11) | (a[ap + 4] << 3) | (a[ap + 5] >> 5);
      int hl = (a[ap + 1] & 1) ? 7 : 9;
      if (!fmp4_audio(&m, a + ap + hl, fl - hl))
        break;
      ap += fl;
      asamp += 1024;
    }
    if (fmp4_video_needs_cut(&m, p, len))
      fmp4_fragment(&m, t);
    fmp4_video(&m, p, len, t);
    nf++;
  }
  fmp4_fragment(&m, (int64_t) (ns * 1e6 / fps));
  if (!getenv("NOFINISH"))
    fmp4_finish(&m, patch); /* NOFINISH=1: as after a power cut */
  fclose(fo);
  fprintf(stderr, "frames %d fragments %u audio frames %ld\n", nf, m.fragments, asamp / 1024);
}
