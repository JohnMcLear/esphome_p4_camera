/* Minimal fragmented MP4 writer. See fmp4_mux.h. */
#include "fmp4_mux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace esphome::esp32p4_recorder {

// This module is plain-C style on purpose: no RTOS or ESPHome calls, so tests/host/run.sh builds and checks
// it on the PC. The checks below would turn its few transient buffers into containers and its stdio offsets
// (ftell/fseek take long) into another type; both change a host-tested file for no runtime gain.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,google-runtime-int)

/* ---- tiny box builder ---- */
struct BoxBuilder {
  uint8_t *b;
  size_t n, cap;
  size_t stack[12];
  int sp;
};

static void put(BoxBuilder *w, const void *p, size_t n) {
  if (w->n + n <= w->cap) {
    memcpy(w->b + w->n, p, n);
  }
  w->n += n;
}
static void u8(BoxBuilder *w, uint8_t v) { put(w, &v, 1); }
static void u16(BoxBuilder *w, uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  put(w, b, 2);
}
static void u24(BoxBuilder *w, uint32_t v) {
  uint8_t b[3] = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  put(w, b, 3);
}
static void u32(BoxBuilder *w, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                  static_cast<uint8_t>(v)};
  put(w, b, 4);
}
static void u64(BoxBuilder *w, uint64_t v) {
  u32(w, v >> 32);
  u32(w, (uint32_t) v);
}
static void zeros(BoxBuilder *w, size_t n) {
  while (n--)
    u8(w, 0);
}
static void wr_be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static void box(BoxBuilder *w, const char *type) {
  w->stack[w->sp++] = w->n;
  u32(w, 0);
  put(w, type, 4);
}
static void fullbox(BoxBuilder *w, const char *type, uint8_t ver, uint32_t flags) {
  box(w, type);
  u8(w, ver);
  u24(w, flags);
}
static void end(BoxBuilder *w) {
  size_t s = w->stack[--w->sp];
  if (s + 4 <= w->cap) {
    wr_be32(w->b + s, (uint32_t) (w->n - s));
  }
}
static void matrix(BoxBuilder *w) {
  static const uint32_t MATRIX[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  for (uint32_t v : MATRIX) {
    u32(w, v);
  }
}

/* ---- Annex B helpers ---- */
/* Next start code (00 00 01) at or after i; returns n if none. */
static size_t find_sc(const uint8_t *p, size_t i, size_t n) {
  while (i + 3 <= n) {
    if (p[i + 2] > 1) {
      i += 3;
    } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      return i;
    } else {
      i++;
    }
  }
  return n;
}

static bool frame_is_key(const uint8_t *p, size_t n) {
  size_t lim = n < 256 ? n : 256;
  for (size_t i = 0; i + 4 < lim; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      uint8_t t = p[i + 3] & 0x1f;
      if (t == 7 || t == 5) {
        return true;
      }
    }
  }
  return false;
}

/* ---- init segment ---- */
static void write_init(Fmp4Muxer *m) {
  uint8_t buf[1536];
  BoxBuilder w = {.b = buf, .cap = sizeof(buf)};
  box(&w, "ftyp");
  put(&w, "isom", 4);
  u32(&w, 0x200);
  put(&w, "isomiso6iso2avc1mp41", 20);
  end(&w);

  box(&w, "moov");
  fullbox(&w, "mvhd", 0, 0);
  u32(&w, 0);
  u32(&w, 0);
  u32(&w, 1000);
  m->off_mvhd_dur = m->pos + w.n;
  u32(&w, 0);
  u32(&w, 0x00010000);
  u16(&w, 0x0100);
  zeros(&w, 10);
  matrix(&w);
  zeros(&w, 24);
  u32(&w, m->audio ? 3 : 2);
  end(&w);

  for (int tr = 0; tr < (m->audio ? 2 : 1); tr++) {
    bool vid = tr == 0;
    box(&w, "trak");
    fullbox(&w, "tkhd", 0, 3);
    u32(&w, 0);
    u32(&w, 0);
    u32(&w, tr + 1);
    u32(&w, 0);
    m->off_tkhd_dur[tr] = m->pos + w.n;
    u32(&w, 0);
    zeros(&w, 8);
    u16(&w, 0);
    u16(&w, 0);
    u16(&w, vid ? 0 : 0x0100);
    u16(&w, 0);
    matrix(&w);
    u32(&w, vid ? m->width << 16 : 0);
    u32(&w, vid ? m->height << 16 : 0);
    end(&w);
    if (!vid && m->a_delay) {
      /* skip the encoder's priming samples */
      box(&w, "edts");
      fullbox(&w, "elst", 0, 0);
      u32(&w, 1);
      m->off_elst_dur = m->pos + w.n;
      u32(&w, 0);          /* segment duration, patched at the end (0 = unknown) */
      u32(&w, m->a_delay); /* media time */
      u32(&w, 0x00010000); /* rate 1.0 */
      end(&w);
      end(&w);
    }
    box(&w, "mdia");
    fullbox(&w, "mdhd", 0, 0);
    u32(&w, 0);
    u32(&w, 0);
    u32(&w, vid ? 90000 : m->a_rate);
    m->off_mdhd_dur[tr] = m->pos + w.n;
    u32(&w, 0);
    u16(&w, 0x55c4); /* "und" */
    u16(&w, 0);
    end(&w);
    fullbox(&w, "hdlr", 0, 0);
    u32(&w, 0);
    put(&w, vid ? "vide" : "soun", 4);
    zeros(&w, 12);
    put(&w, vid ? "VideoHandler" : "SoundHandler", 13);
    end(&w);
    box(&w, "minf");
    if (vid) {
      fullbox(&w, "vmhd", 0, 1);
      zeros(&w, 8);
    } else {
      fullbox(&w, "smhd", 0, 0);
      zeros(&w, 4);
    }
    end(&w);
    box(&w, "dinf");
    fullbox(&w, "dref", 0, 0);
    u32(&w, 1);
    fullbox(&w, "url ", 0, 1);
    end(&w);
    end(&w);
    end(&w);
    box(&w, "stbl");
    fullbox(&w, "stsd", 0, 0);
    u32(&w, 1);
    if (vid) {
      box(&w, "avc1");
      zeros(&w, 6);
      u16(&w, 1);
      zeros(&w, 16);
      u16(&w, m->width);
      u16(&w, m->height);
      u32(&w, 0x00480000);
      u32(&w, 0x00480000);
      u32(&w, 0);
      u16(&w, 1);
      zeros(&w, 32);
      u16(&w, 0x0018);
      u16(&w, 0xffff);
      box(&w, "avcC");
      u8(&w, 1);
      u8(&w, m->sps[1]);
      u8(&w, m->sps[2]);
      u8(&w, m->sps[3]);
      u8(&w, 0xff);
      u8(&w, 0xe1);
      u16(&w, m->sps_len);
      put(&w, m->sps, m->sps_len);
      u8(&w, 1);
      u16(&w, m->pps_len);
      put(&w, m->pps, m->pps_len);
      end(&w);
      end(&w);
    } else {
      static const uint32_t AAC_RATES[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                           22050, 16000, 12000, 11025, 8000,  7350};
      int fi = 8;
      for (int i = 0; i < 13; i++) {
        if (AAC_RATES[i] == m->a_rate) {
          fi = i;
        }
      }
      uint16_t asc = (2 << 11) | (fi << 7) | (1 << 3); /* AAC-LC, rate index, mono */
      box(&w, "mp4a");
      zeros(&w, 6);
      u16(&w, 1);
      zeros(&w, 8);
      u16(&w, 1);
      u16(&w, 16);
      u16(&w, 0);
      u16(&w, 0);
      u32(&w, m->a_rate << 16);
      fullbox(&w, "esds", 0, 0);
      u8(&w, 0x03);
      u8(&w, 3 + 2 + 13 + 2 + 2 + 3);
      u16(&w, 0);
      u8(&w, 0);
      u8(&w, 0x04);
      u8(&w, 13 + 2 + 2);
      u8(&w, 0x40);
      u8(&w, 0x15);
      u24(&w, 0);
      u32(&w, 64000);
      u32(&w, 32000);
      u8(&w, 0x05);
      u8(&w, 2);
      u16(&w, asc);
      u8(&w, 0x06);
      u8(&w, 1);
      u8(&w, 0x02);
      end(&w);
      end(&w);
    }
    end(&w);
    fullbox(&w, "stts", 0, 0);
    u32(&w, 0);
    end(&w);
    fullbox(&w, "stsc", 0, 0);
    u32(&w, 0);
    end(&w);
    fullbox(&w, "stsz", 0, 0);
    u32(&w, 0);
    u32(&w, 0);
    end(&w);
    fullbox(&w, "stco", 0, 0);
    u32(&w, 0);
    end(&w);
    end(&w); /* stbl */
    end(&w); /* minf */
    end(&w); /* mdia */
    end(&w); /* trak */
  }
  box(&w, "mvex");
  fullbox(&w, "mehd", 0, 0);
  m->off_mehd_dur = m->pos + w.n;
  u32(&w, 0);
  end(&w);
  for (int tr = 0; tr < (m->audio ? 2 : 1); tr++) {
    fullbox(&w, "trex", 0, 0);
    u32(&w, tr + 1);
    u32(&w, 1);
    u32(&w, 0);
    u32(&w, 0);
    u32(&w, 0);
    end(&w);
  }
  end(&w);
  end(&w); /* moov */
  if (w.n <= w.cap) {
    m->out(m->ctx, buf, w.n);
    m->pos += w.n;
    m->init_done = true;
    if (m->max_frags) {
      /* placeholder for the sidx: a free box, overwritten at finish */
      uint32_t r = fmp4_sidx_reserve(m->max_frags);
      uint8_t z[256] = {0};
      wr_be32(z, r);
      memcpy(z + 4, "free", 4);  // NOLINT(bugprone-not-null-terminated-result): a box type, not a string
      m->off_sidx = m->pos;
      for (uint32_t left = r; left;) {
        uint32_t k = left < sizeof(z) ? left : sizeof(z);
        m->out(m->ctx, z, k);
        if (left == r) {
          memset(z, 0, 8);
        }
        left -= k;
      }
      m->pos += r;
    }
  }
}

void fmp4_begin(Fmp4Muxer *m, fmp4_out_fn out, void *ctx, uint32_t w, uint32_t h, bool audio, uint32_t a_rate,
                uint32_t a_delay, uint32_t max_frags) {
  free(m->fr_size);
  free(m->fr_dur);
  free(m->fr_adur);
  memset(m, 0, sizeof(*m));
  if (max_frags) {
    m->fr_size = static_cast<uint32_t *>(calloc(max_frags, 4));
    m->fr_dur = static_cast<uint32_t *>(calloc(max_frags, 4));
    m->fr_adur = static_cast<uint32_t *>(calloc(max_frags, 4));
    m->max_frags = m->fr_size && m->fr_dur && m->fr_adur ? max_frags : 0;
  }
  m->out = out;
  m->ctx = ctx;
  m->width = w;
  m->height = h;
  m->audio = audio;
  m->a_rate = a_rate;
  m->a_delay = a_delay;
  m->seq = 1;
}

bool fmp4_video_needs_cut(Fmp4Muxer *m, const uint8_t *annexb, size_t len) {
  if (m->nv == 0) {
    return false;
  }
  return m->nv >= FMP4_MAX_VFRAMES || frame_is_key(annexb, len);
}

void fmp4_video(Fmp4Muxer *m, uint8_t *p, size_t n, int64_t t_us) {
  if (m->nv >= FMP4_MAX_VFRAMES) {
    return;
  }
  /* walk the NAL units: keep SPS/PPS aside, rewrite each start code of a kept NAL as its 4-byte length */
  bool key = false;
  uint8_t *first = NULL;
  size_t i = find_sc(p, 0, n);
  while (i < n) {
    size_t s = i + 3; /* NAL header */
    size_t nx = find_sc(p, s, n);
    size_t e = nx; /* trailing zero belongs to the next 4-byte start code */
    if (nx < n && e > s && p[e - 1] == 0) {
      e--;
    }
    uint8_t t = p[s] & 0x1f;
    if (t == 7 && e - s <= sizeof(m->sps)) {
      memcpy(m->sps, p + s, e - s);
      m->sps_len = e - s;
    } else if (t == 8 && e - s <= sizeof(m->pps)) {
      memcpy(m->pps, p + s, e - s);
      m->pps_len = e - s;
    }
    /* Leading SPS/PPS/AUD go to avcC only; from the first slice on, every NAL is kept. This encoder writes
     * 4-byte start codes, so each one becomes the 4-byte length in place (3-byte codes are not handled). */
    if ((first || (t != 7 && t != 8 && t != 9)) && s >= 4) {
      if (!first) {
        first = p + s - 4;
      }
      if (t == 5) {
        key = true;
      }
      wr_be32(p + s - 4, (uint32_t) (e - s));
    }
    i = nx;
  }
  if (!first) {
    return;
  }
  if (m->nv == 0 && !m->init_done) {
    if (!key || !m->sps_len || !m->pps_len) {
      return; /* wait for an IDR with SPS/PPS */
    }
    m->t0_us = t_us;
    write_init(m);
  }
  Fmp4VideoFrame *f = &m->vf[m->nv++];
  f->p = first;
  f->len = (uint32_t) (p + n - first);
  f->t_us = t_us;
  f->key = key;
}

bool fmp4_audio(Fmp4Muxer *m, const uint8_t *aac, size_t len) {
  if (!m->audio || m->na >= FMP4_MAX_AFRAMES || m->alen + len > FMP4_ABUF || len > 0xffff) {
    return false;
  }
  memcpy(m->abuf + m->alen, aac, len);
  m->alen += len;
  m->asz[m->na++] = (uint16_t) len;
  return true;
}

static uint64_t to90k(const Fmp4Muxer *m, int64_t t_us) {
  int64_t d = t_us - m->t0_us;
  return d <= 0 ? 0 : (uint64_t) ((d * 9 + 50) / 100);
}

void fmp4_fragment(Fmp4Muxer *m, int64_t next_t_us) {
  if (!m->init_done || (m->nv == 0 && m->na == 0)) {
    return;
  }
  uint8_t buf[256 + FMP4_MAX_VFRAMES * 12 + FMP4_MAX_AFRAMES * 8];
  BoxBuilder w = {.b = buf, .cap = sizeof(buf)};
  size_t vbytes = 0;
  for (int i = 0; i < m->nv; i++) {
    vbytes += m->vf[i].len;
  }
  size_t moof_at = 0;
  size_t off_vdata = 0, off_adata = 0;
  box(&w, "moof");
  fullbox(&w, "mfhd", 0, 0);
  u32(&w, m->seq++);
  end(&w);
  uint64_t vbase = m->v_dts;
  if (m->nv) {
    box(&w, "traf");
    fullbox(&w, "tfhd", 0, 0x020000);
    u32(&w, 1);
    end(&w);
    fullbox(&w, "tfdt", 1, 0);
    u64(&w, vbase);
    end(&w);
    fullbox(&w, "trun", 0, 0x000701);
    u32(&w, m->nv);
    off_vdata = w.n;
    u32(&w, 0);
    uint64_t prev = vbase;
    for (int i = 0; i < m->nv; i++) {
      uint64_t nd = i + 1 < m->nv ? to90k(m, m->vf[i + 1].t_us) : to90k(m, next_t_us);
      if (nd <= prev) {
        nd = prev + 1;
      }
      u32(&w, (uint32_t) (nd - prev));
      u32(&w, m->vf[i].len);
      u32(&w, m->vf[i].key ? 0x02000000 : 0x01010000);
      prev = nd;
    }
    m->v_dts = prev;
    end(&w);
    end(&w);
  }
  if (m->na) {
    box(&w, "traf");
    fullbox(&w, "tfhd", 0, 0x020000);
    u32(&w, 2);
    end(&w);
    fullbox(&w, "tfdt", 1, 0);
    u64(&w, m->a_dts);
    end(&w);
    fullbox(&w, "trun", 0, 0x000301);
    u32(&w, m->na);
    off_adata = w.n;
    u32(&w, 0);
    for (int i = 0; i < m->na; i++) {
      u32(&w, 1024);
      u32(&w, m->asz[i]);
    }
    m->a_dts += 1024ull * m->na;
    end(&w);
    end(&w);
  }
  end(&w);
  size_t moof_sz = w.n - moof_at;
  if (off_vdata) {
    wr_be32(buf + off_vdata, (uint32_t) (moof_sz + 8));
  }
  if (off_adata) {
    wr_be32(buf + off_adata, (uint32_t) (moof_sz + 8 + vbytes));
  }
  int na = m->na;
  uint8_t mh[8];
  wr_be32(mh, (uint32_t) (8 + vbytes + m->alen));
  memcpy(mh + 4, "mdat", 4);  // NOLINT(bugprone-not-null-terminated-result): a box type, not a string
  m->out(m->ctx, buf, w.n);
  m->out(m->ctx, mh, 8);
  for (int i = 0; i < m->nv; i++) {
    m->out(m->ctx, m->vf[i].p, m->vf[i].len);
  }
  if (m->alen) {
    m->out(m->ctx, m->abuf, m->alen);
  }
  m->pos += w.n + 8 + vbytes + m->alen;
  if (m->nfr < m->max_frags) {
    m->fr_size[m->nfr] = (uint32_t) (w.n + 8 + vbytes + m->alen);
    m->fr_dur[m->nfr] = (uint32_t) (m->v_dts - vbase);
    m->fr_adur[m->nfr] = 1024u * na;
  }
  m->nfr++;
  m->nv = 0;
  m->na = 0;
  m->alen = 0;
  m->fragments++;
}

/* One sidx per track (video, then audio if adur), each indexing all n fragments, filling exactly `reserve`
 * bytes: a free box first, then the sidx boxes, so the last sidx sits right before the first moof. Chrome's
 * demuxer only skips its walk over every fragment (26 s for a 200 s clip from the board) when every track has
 * a sidx AND they adjoin the first moof; with the free box between them it still walked (tested on the PC
 * with tools/browser/range_server.py: 13.4 s vs 0.15 s to metadata). This is ffmpeg's global_sidx layout. */
static size_t build_sidx(uint8_t *b, uint32_t reserve, uint32_t n, const uint32_t *size, const uint32_t *vdur,
                         const uint32_t *adur, uint32_t a_rate) {
  uint32_t one = 32 + 12 * n, ntr = adur ? 2 : 1;
  if (one * ntr + 8 > reserve) {
    return 0;
  }
  uint32_t pad = reserve - one * ntr;
  BoxBuilder w = {.b = b, .cap = reserve};
  u32(&w, pad);
  put(&w, "free", 4);
  zeros(&w, pad - 8);
  for (uint32_t t = 0; t < ntr; t++) {
    fullbox(&w, "sidx", 0, 0);
    u32(&w, t + 1);
    u32(&w, t ? a_rate : 90000);
    u32(&w, 0);
    u32(&w, (ntr - 1 - t) * one); /* first_offset: the sidx boxes after this one */
    u16(&w, 0);
    u16(&w, n);
    for (uint32_t i = 0; i < n; i++) {
      u32(&w, size[i] & 0x7fffffff);
      u32(&w, t ? adur[i] : vdur[i]);
      u32(&w, 0x80000000); /* starts with SAP (as ffmpeg writes it) */
    }
    end(&w);
  }
  return w.n;
}

void fmp4_finish(Fmp4Muxer *m, fmp4_patch_fn patch) {
  if (m->init_done && m->max_frags && m->nfr <= m->max_frags && m->nfr <= 0xffff) {
    uint32_t r = fmp4_sidx_reserve(m->max_frags);
    auto *b = static_cast<uint8_t *>(malloc(r));
    size_t k = b ? build_sidx(b, r, m->nfr, m->fr_size, m->fr_dur, m->audio ? m->fr_adur : NULL, m->a_rate) : 0;
    if (k) {
      patch(m->ctx, m->off_sidx, b, k);
    }
    free(b);
  }
  if (!m->init_done) {
    return;
  }
  uint8_t b[4];
  uint32_t v_ms = (uint32_t) (m->v_dts / 90);
  uint64_t a_pres = m->a_dts > m->a_delay ? m->a_dts - m->a_delay : 0;
  if (m->a_len && m->a_len < a_pres) {
    a_pres = m->a_len;
  }
  uint32_t a_ms = m->audio ? (uint32_t) (a_pres * 1000 / m->a_rate) : 0;
  uint32_t mv = v_ms > a_ms ? v_ms : a_ms;
  wr_be32(b, mv);
  patch(m->ctx, m->off_mvhd_dur, b, 4);
  patch(m->ctx, m->off_mehd_dur, b, 4);
  wr_be32(b, v_ms);
  patch(m->ctx, m->off_tkhd_dur[0], b, 4);
  wr_be32(b, (uint32_t) m->v_dts);
  patch(m->ctx, m->off_mdhd_dur[0], b, 4);
  if (m->audio) {
    wr_be32(b, a_ms);
    patch(m->ctx, m->off_tkhd_dur[1], b, 4);
    if (m->a_delay) {
      patch(m->ctx, m->off_elst_dur, b, 4);
    }
    wr_be32(b, (uint32_t) m->a_dts);
    patch(m->ctx, m->off_mdhd_dur[1], b, 4);
  }
}

size_t fmp4_pending_bytes(const Fmp4Muxer *m) {
  size_t v = 0;
  for (int i = 0; i < m->nv; i++) {
    v += m->vf[i].len;
  }
  return v + m->alen + 4096;
}

/* ---- power-cut repair ---- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

struct MoovMap {
  uint32_t track_id[2], timescale[2];
  long off_tkhd[2], off_mdhd[2], off_elst[2];
  uint32_t media_time[2];
  long off_mvhd, off_mehd;
  uint32_t mvhd_dur;
  int nt;
};

/* walk the boxes in buf[0..n), which sit at file offset base */
static void map_boxes(const uint8_t *b, size_t n, long base, MoovMap *mm) {
  size_t i = 0;
  while (i + 8 <= n) {
    uint32_t sz = rd32(b + i);
    const uint8_t *t = b + i + 4;
    if (sz < 8 || i + sz > n) {
      return;
    }
    if (!memcmp(t, "trak", 4) || !memcmp(t, "mdia", 4) || !memcmp(t, "mvex", 4) || !memcmp(t, "edts", 4)) {
      if (!memcmp(t, "trak", 4) && mm->nt < 2) {
        mm->nt++;
      }
      map_boxes(b + i + 8, sz - 8, base + (long) i + 8, mm);
    } else if (!memcmp(t, "mvhd", 4) && sz >= 28) {
      mm->off_mvhd = base + (long) i + 24;
      mm->mvhd_dur = rd32(b + i + 24);
    } else if (!memcmp(t, "mehd", 4) && sz >= 16) {
      mm->off_mehd = base + (long) i + 12;
    } else if (mm->nt >= 1) {
      int k = mm->nt - 1;
      if (!memcmp(t, "tkhd", 4) && sz >= 32) {
        mm->off_tkhd[k] = base + (long) i + 28;
        mm->track_id[k] = rd32(b + i + 20);
      } else if (!memcmp(t, "mdhd", 4) && sz >= 28) {
        mm->off_mdhd[k] = base + (long) i + 24;
        mm->timescale[k] = rd32(b + i + 20);
      } else if (!memcmp(t, "elst", 4) && sz >= 24) {
        mm->off_elst[k] = base + (long) i + 16;
        mm->media_time[k] = rd32(b + i + 20);
      }
    }
    i += sz;
  }
}

/* sum the sample durations per track in one moof */
static void moof_ends(const uint8_t *b, size_t n, const MoovMap *mm, uint64_t end[2]) {
  size_t i = 8; /* skip the moof header */
  while (i + 8 <= n) {
    uint32_t sz = rd32(b + i);
    if (sz < 8 || i + sz > n) {
      return;
    }
    if (!memcmp(b + i + 4, "traf", 4)) {
      int k = -1;
      uint64_t base = 0, sum = 0;
      size_t j = i + 8;
      while (j + 8 <= i + sz) {
        uint32_t s2 = rd32(b + j);
        if (s2 < 8 || j + s2 > i + sz) {
          break;
        }
        const uint8_t *c = b + j;
        if (!memcmp(c + 4, "tfhd", 4)) {
          uint32_t id = rd32(c + 12);
          for (int q = 0; q < mm->nt; q++) {
            if (mm->track_id[q] == id) {
              k = q;
            }
          }
        } else if (!memcmp(c + 4, "tfdt", 4)) {
          base = c[8] == 1 ? ((uint64_t) rd32(c + 12) << 32 | rd32(c + 16)) : rd32(c + 12);
        } else if (!memcmp(c + 4, "trun", 4)) {
          uint32_t fl = rd32(c + 8) & 0xffffff, cnt = rd32(c + 12);
          size_t p = 16 + (fl & 1 ? 4 : 0) + (fl & 4 ? 4 : 0);
          size_t per = (fl & 0x100 ? 4 : 0) + (fl & 0x200 ? 4 : 0) + (fl & 0x400 ? 4 : 0) + (fl & 0x800 ? 4 : 0);
          if (fl & 0x100) {
            for (uint32_t s = 0; s < cnt && p + 4 <= s2; s++, p += per) {
              sum += rd32(c + p);
            }
          }
        }
        j += s2;
      }
      if (k >= 0 && base + sum > end[k]) {
        end[k] = base + sum;
      }
    }
    i += sz;
  }
}

int fmp4_repair(const char *path, uint32_t *kept_ms, uint64_t *cut_bytes) {
  FILE *f = fopen(path, "r+b");
  if (!f) {
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f), off = 0, good_end = -1, pending_moof = -1, moof_sz = 0;
  MoovMap mm{};
  uint64_t end[2] = {0, 0};
  long free_off = -1;
  uint32_t free_sz = 0, nfr = 0, capfr = 0, *fsz = NULL, *fdur = NULL, *fadur = NULL;
  enum { BUFSZ = 8192 };
  auto *buf = static_cast<uint8_t *>(malloc(BUFSZ)); /* transient, at boot */
  int ret = 0;
  if (!buf) {
    fclose(f);
    return -5;
  }
  while (off + 8 <= fsize) {
    uint8_t h[8];
    fseek(f, off, SEEK_SET);
    if (fread(h, 1, 8, f) != 8) {
      break;
    }
    uint32_t sz = rd32(h);
    if (sz < 8 || off + (long) sz > fsize) {
      break; /* partial box: the cut is here */
    }
    if (!memcmp(h + 4, "moov", 4)) {
      fseek(f, off, SEEK_SET);
      if (sz > BUFSZ || fread(buf, 1, sz, f) != sz) {
        fclose(f);
        free(buf);
        return -2;
      }
      map_boxes(buf + 8, sz - 8, off + 8, &mm);
      if (mm.mvhd_dur != 0) {
        fclose(f); /* finished normally */
        free(buf);
        return 0;
      }
      good_end = off + sz;
    } else if (!memcmp(h + 4, "free", 4) && free_off < 0 && pending_moof < 0 && nfr == 0) {
      free_off = off; /* the sidx placeholder */
      free_sz = sz;
      good_end = off + sz;
    } else if (!memcmp(h + 4, "moof", 4)) {
      pending_moof = off;
      moof_sz = sz;
    } else if (!memcmp(h + 4, "mdat", 4) && pending_moof >= 0) {
      fseek(f, pending_moof, SEEK_SET);
      uint64_t v0 = end[0], a0 = end[1];
      if (moof_sz <= BUFSZ && fread(buf, 1, moof_sz, f) == (size_t) moof_sz) {
        moof_ends(buf, moof_sz, &mm, end);
      }
      if (nfr == capfr) {
        capfr = capfr ? capfr * 2 : 256;
        auto *a = static_cast<uint32_t *>(realloc(fsz, capfr * 4));
        auto *b2 = a ? static_cast<uint32_t *>(realloc(fdur, capfr * 4)) : nullptr;
        auto *c2 = b2 ? static_cast<uint32_t *>(realloc(fadur, capfr * 4)) : nullptr;
        if (a) {
          fsz = a;
        }
        if (b2) {
          fdur = b2;
        }
        if (c2) {
          fadur = c2;
        }
        if (!a || !b2 || !c2) {
          capfr = 0;
          nfr = 0x7fffffff; /* give up on the sidx, keep repairing */
        }
      }
      if (nfr < capfr) {
        fsz[nfr] = (uint32_t) (off + sz - pending_moof);
        fdur[nfr] = (uint32_t) (end[0] - v0);
        fadur[nfr] = (uint32_t) (end[1] - a0);
        nfr++;
      }
      pending_moof = -1;
      good_end = off + sz;
    } else if (memcmp(h + 4, "ftyp", 4) != 0) {
      break;
    }
    off += sz;
  }
  if (good_end < 0 || mm.off_mvhd == 0) {
    free(buf);
    free(fsz);
    free(fdur);
    free(fadur);
    fclose(f);
    return -3;
  }
  if (free_off >= 0 && nfr && nfr < 0x7fffffff) {
    auto *sb = static_cast<uint8_t *>(malloc(free_sz));
    size_t k = sb ? build_sidx(sb, free_sz, nfr, fsz, fdur, mm.nt > 1 ? fadur : NULL, mm.timescale[1]) : 0;
    if (k) {
      fseek(f, free_off, SEEK_SET);
      fwrite(sb, 1, k, f);
    }
    free(sb);
  }
  free(buf);
  free(fsz);
  free(fdur);
  free(fadur);
  fflush(f);
  if (good_end < fsize) {
    ret = ftruncate(fileno(f), good_end) == 0 ? 1 : -4;
  } else {
    ret = 1;
  }
  uint32_t ms[2] = {0, 0}, mv = 0;
  uint8_t b[4];
  for (int k = 0; k < mm.nt; k++) {
    uint64_t pres = end[k] > mm.media_time[k] ? end[k] - mm.media_time[k] : 0;
    ms[k] = mm.timescale[k] ? (uint32_t) (pres * 1000 / mm.timescale[k]) : 0;
    if (ms[k] > mv) {
      mv = ms[k];
    }
    wr_be32(b, ms[k]);
    fseek(f, mm.off_tkhd[k], SEEK_SET);
    fwrite(b, 1, 4, f);
    if (mm.off_elst[k]) {
      fseek(f, mm.off_elst[k], SEEK_SET);
      fwrite(b, 1, 4, f);
    }
    wr_be32(b, (uint32_t) end[k]);
    fseek(f, mm.off_mdhd[k], SEEK_SET);
    fwrite(b, 1, 4, f);
  }
  wr_be32(b, mv);
  fseek(f, mm.off_mvhd, SEEK_SET);
  fwrite(b, 1, 4, f);
  if (mm.off_mehd) {
    fseek(f, mm.off_mehd, SEEK_SET);
    fwrite(b, 1, 4, f);
  }
  fclose(f);
  if (kept_ms) {
    *kept_ms = mv;
  }
  if (cut_bytes) {
    *cut_bytes = (uint64_t) (fsize - good_end);
  }
  return ret;
}

// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,google-runtime-int)

}  // namespace esphome::esp32p4_recorder
