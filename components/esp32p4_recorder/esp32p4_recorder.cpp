#ifdef USE_ESP32

#include "esp32p4_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <linux/videodev2.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include "esp_video_device.h"
#ifdef USE_ESP32P4_RECORDER_AUDIO
#include "encoder/impl/esp_aac_enc.h"
#endif

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::esp32p4_recorder {

static const char *const TAG = "esp32p4_recorder";

static constexpr size_t RING_SIZE = 4 * 1024 * 1024;
static constexpr size_t CHUNK_SIZE = 64 * 1024;
/// Commit the FAT size this often, so a power cut keeps the clip up to here.
static constexpr int64_t FSYNC_INTERVAL_US = 5 * 1000000;
static constexpr uint32_t FRAME_MAGIC = 0x52464459;  // a frame record in the ring
static constexpr uint32_t SKIP_MAGIC = 0x50494b53;   // the rest of the ring is padding

/// AAC-LC, mono. The encoder holds this many samples back (measured for esp_audio_codec 2.5 at
/// 16 kHz, 32 kbit/s); the MP4 edit list skips them so audio lines up with video.
static constexpr int AAC_BITRATE = 32000;
static constexpr uint32_t AAC_DELAY_SAMPLES = 1600;
static constexpr uint32_t PCM_RING_SECONDS = 4;
/// How long a finishing clip waits for the microphone to deliver audio up to the last video frame.
static constexpr int64_t AUDIO_END_TIMEOUT_US = 300000;

struct FrameHeader {
  uint32_t magic;
  uint32_t len;
  int64_t t_us;
};

static size_t record_size(size_t len) { return (sizeof(FrameHeader) + len + 7) & ~size_t(7); }

/// True if an Annex B access unit holds an SPS or an IDR slice (looks at the first 256 bytes).
static bool has_idr(const uint8_t *p, size_t n) {
  size_t lim = std::min<size_t>(n, 256);
  for (size_t i = 0; i + 4 < lim; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      uint8_t type = p[i + 3] & 0x1f;
      if (type == 7 || type == 5)
        return true;
    }
  }
  return false;
}

static bool set_ctrl(int fd, uint32_t id, int32_t value) {
  struct v4l2_ext_control c {};
  c.id = id;
  c.value = value;
  struct v4l2_ext_controls cs {};
  cs.ctrl_class = V4L2_CID_CODEC_CLASS;
  cs.count = 1;
  cs.controls = &c;
  return ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) == 0;
}

/* ---------------- setup ---------------- */

void ESP32P4Recorder::setup() {
  if (this->camera_->is_failed()) {
    this->mark_failed(LOG_STR("Camera is not running"));
    return;
  }
  if (!this->mount_card_()) {
    // No card, or an unreadable one: never format it, and leave the rest of the device running.
    this->mark_failed(LOG_STR("SD card mount failed"));
    return;
  }
  if (!this->encoder_setup_()) {
    this->mark_failed(LOG_STR("H.264 encoder setup failed"));
    return;
  }
  if (!this->writer_setup_()) {
    this->mark_failed(LOG_STR("Writer setup failed"));
    return;
  }
  if (!this->audio_setup_())
    ESP_LOGW(TAG, "Audio setup failed: recordings will be video only");
  this->ready_ = true;
}

/* The SDMMC controller is shared with esp32_hosted, whose ESP32-C6 link sits on slot 1. The host is
 * initialised once (IDF 5.5 skips a second init) and never de-initialised here: a failed mount would
 * otherwise take the Wi-Fi link down with it. Espressif's workaround, esp-idf#17889. */
static esp_err_t shared_host_init() { return ESP_OK; }
static esp_err_t shared_host_deinit() { return ESP_OK; }

bool ESP32P4Recorder::mount_card_() {
  esp_err_t err = sdmmc_host_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(err));
    return false;
  }
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.init = &shared_host_init;
  host.flags &= ~SDMMC_HOST_FLAG_DEINIT_ARG;  // deinit shares a union with deinit_p(slot)
  host.deinit = &shared_host_deinit;
  // Always set: the IDF default is slot 1, which esp32_hosted uses for the C6 on this board.
  host.slot = this->sd_slot_;
  host.max_freq_khz = this->sd_freq_khz_;
  if (this->sd_ldo_channel_ >= 0) {
    sd_pwr_ctrl_ldo_config_t ldo{};
    ldo.ldo_chan_id = this->sd_ldo_channel_;
    sd_pwr_ctrl_handle_t pwr = nullptr;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr) != ESP_OK) {
      ESP_LOGE(TAG, "SD power LDO channel %d failed", this->sd_ldo_channel_);
      return false;
    }
    host.pwr_ctrl_handle = pwr;
  }

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = this->d_pins_[1] >= 0 ? 4 : 1;
  slot.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot.d0 = static_cast<gpio_num_t>(this->d_pins_[0]);
  if (slot.width == 4) {
    slot.d1 = static_cast<gpio_num_t>(this->d_pins_[1]);
    slot.d2 = static_cast<gpio_num_t>(this->d_pins_[2]);
    slot.d3 = static_cast<gpio_num_t>(this->d_pins_[3]);
  }
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed = false;
  mount.max_files = 4;
  mount.allocation_unit_size = 16 * 1024;
  err = esp_vfs_fat_sdmmc_mount(this->mount_point_, &host, &slot, &mount, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Mounting the card at %s failed: %s", this->mount_point_, esp_err_to_name(err));
    this->card_ = nullptr;
    return false;
  }
  return true;
}

bool ESP32P4Recorder::encoder_setup_() {
  const uint32_t w = this->camera_->get_width();
  const uint32_t h = this->camera_->get_height();
  this->enc_fd_ = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
  if (this->enc_fd_ < 0) {
    ESP_LOGE(TAG, "open %s failed (errno %d)", ESP_VIDEO_H264_DEVICE_NAME, errno);
    return false;
  }
  if (!set_ctrl(this->enc_fd_, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, this->gop_) ||
      !set_ctrl(this->enc_fd_, V4L2_CID_MPEG_VIDEO_BITRATE, this->bitrate_) ||
      !set_ctrl(this->enc_fd_, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, this->min_qp_) ||
      !set_ctrl(this->enc_fd_, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, this->max_qp_)) {
    ESP_LOGE(TAG, "Setting the H.264 controls failed");
    return false;
  }

  struct v4l2_format fmt {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = w;
  fmt.fmt.pix.height = h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  struct v4l2_requestbuffers req {};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  req.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(this->enc_fd_, VIDIOC_S_FMT, &fmt) != 0 || ioctl(this->enc_fd_, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "Encoder input setup failed (errno %d)", errno);
    return false;
  }

  fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = w;
  fmt.fmt.pix.height = h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
  req = {};
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  struct v4l2_buffer b {};
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = 0;
  if (ioctl(this->enc_fd_, VIDIOC_S_FMT, &fmt) != 0 || ioctl(this->enc_fd_, VIDIOC_REQBUFS, &req) != 0 ||
      ioctl(this->enc_fd_, VIDIOC_QUERYBUF, &b) != 0) {
    ESP_LOGE(TAG, "Encoder output setup failed (errno %d)", errno);
    return false;
  }
  void *p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->enc_fd_, b.m.offset);
  if (p == nullptr || p == MAP_FAILED || ioctl(this->enc_fd_, VIDIOC_QBUF, &b) != 0) {
    ESP_LOGE(TAG, "Encoder output buffer failed");
    return false;
  }
  this->enc_out_ = static_cast<uint8_t *>(p);

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->enc_fd_, VIDIOC_STREAMON, &type) != 0)
    return false;
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  return ioctl(this->enc_fd_, VIDIOC_STREAMON, &type) == 0;
}

bool ESP32P4Recorder::writer_setup_() {
  this->ring_ = static_cast<uint8_t *>(heap_caps_malloc(RING_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
  this->bounce_ = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(64, CHUNK_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  this->mux_ = static_cast<Fmp4Muxer *>(heap_caps_calloc(1, sizeof(Fmp4Muxer), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
  if (this->ring_ == nullptr || this->bounce_ == nullptr || this->mux_ == nullptr)
    return false;
  // Core 1: the camera's capture and JPEG tasks are on core 0.
  return xTaskCreatePinnedToCore(&ESP32P4Recorder::writer_task, "p4rec_writer", 8192, this, 7, &this->writer_task_,
                                 1) == pdPASS;
}

/* ---------------- main loop: requests and reporting ---------------- */

void ESP32P4Recorder::start(uint32_t seconds) {
  if (this->is_failed() || !this->ready_) {
    ESP_LOGW(TAG, "Not ready, start ignored");
    return;
  }
  this->requested_secs_ = std::clamp<uint32_t>(seconds, 1, this->max_duration_s_);
  this->stop_requested_ = false;
  State expected = State::IDLE;
  if (!this->state_.compare_exchange_strong(expected, State::STARTING)) {
    ESP_LOGW(TAG, "Already recording, start ignored");
    return;
  }
#ifdef USE_ESP32P4_RECORDER_AUDIO
  // Audio that arrives before the first video frame is skipped, and a late start is padded with silence.
  if (this->aac_ != nullptr && !this->mic_running_) {
    this->mic_source_->start();
    this->mic_running_ = true;
  }
#endif
}

void ESP32P4Recorder::stop() {
  State st = this->state_;
  if (st == State::STARTING || st == State::RECORDING)
    this->stop_requested_ = true;
}

bool ESP32P4Recorder::is_recording() const { return this->state_ != State::IDLE; }

void ESP32P4Recorder::loop() {
  // Counters, not states: a short clip can start and finish between two passes of the loop.
  const uint32_t started = this->clips_started_;
  if (started != this->reported_started_) {
    this->reported_started_ = started;
    ESP_LOGI(TAG, "Recording %s", this->path_);
    this->start_callback_.call(std::string(this->path_));
  }
  const uint32_t finished = this->clips_finished_;
  if (finished != this->reported_finished_) {
    this->reported_finished_ = finished;
    this->last_file_ = this->finished_path_;
    struct stat info {};
    stat(this->finished_path_, &info);
    double secs = this->frames_ > 1 ? (this->last_us_ - this->first_us_) / 1e6 : 0.0;
    ESP_LOGI(TAG,
             "Finished %s: %" PRIu32 " frames over %.1f s (%.2f fps), %ld bytes; since boot %" PRIu32
             " stalls, %" PRIu32 " write errors, %" PRIu32 " encode errors, %" PRIu32 " oversize frames, %" PRIu32
             " audio overflows",
             this->finished_path_, this->frames_, secs, secs > 0 ? (this->frames_ - 1) / secs : 0.0,
             (long) info.st_size, this->stalls_.load(), this->write_errors_.load(), this->encode_errors_.load(),
             this->oversize_.load(), this->audio_overflow_.load());
    this->finished_callback_.call(this->last_file_);
  }
#ifdef USE_ESP32P4_RECORDER_AUDIO
  if (this->mic_running_ && this->state_ == State::IDLE) {
    this->mic_source_->stop();
    this->mic_running_ = false;
  }
#endif
}

/* ---------------- capture task: encode and queue ---------------- */

void ESP32P4Recorder::on_raw_frame(uint8_t *yuv420, size_t length, int64_t t_us) {
  if (!this->ready_)
    return;
  this->capture_task_ = xTaskGetCurrentTaskHandle();

  if (this->state_ == State::STARTING) {
    if (this->open_clip_()) {
      this->end_us_ = t_us + int64_t(this->requested_secs_) * 1000000;
      this->wait_idr_ = true;
      this->state_ = State::RECORDING;
      this->clips_started_++;
    } else {
      this->path_[0] = '\0';
      this->state_ = State::IDLE;
    }
  }
  if (this->state_ != State::RECORDING)
    return;

  struct v4l2_buffer in {};
  in.index = 0;
  in.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  in.memory = V4L2_MEMORY_USERPTR;
  in.m.userptr = reinterpret_cast<unsigned long>(yuv420);  // NOLINT(google-runtime-int): the V4L2 field type
  in.length = length;
  struct v4l2_buffer out {};
  out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  out.memory = V4L2_MEMORY_MMAP;
  if (ioctl(this->enc_fd_, VIDIOC_QBUF, &in) != 0 || ioctl(this->enc_fd_, VIDIOC_DQBUF, &out) != 0 ||
      ioctl(this->enc_fd_, VIDIOC_DQBUF, &in) != 0) {
    this->encode_errors_++;
    this->begin_finish_();
    return;
  }
  const size_t len = out.bytesused;
  if (this->wait_idr_ && has_idr(this->enc_out_, len)) {
    this->wait_idr_ = false;
    this->first_us_ = t_us;
    if (this->clip_audio_) {
      this->audio_t0_us_ = t_us;
      this->audio_pending_ = true;  // the microphone task picks its first sample from this time
    }
  }
  if (!this->wait_idr_) {
    if (len <= RING_SIZE / 2) {
      this->ring_put_(this->enc_out_, len, t_us);
      this->last_us_ = t_us;
      this->frames_++;
      this->frame_bytes_ += len;
    } else {
      this->oversize_++;
    }
  }
  out.index = 0;
  out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  out.memory = V4L2_MEMORY_MMAP;
  ioctl(this->enc_fd_, VIDIOC_QBUF, &out);

  if (this->stop_requested_ || t_us >= this->end_us_)
    this->begin_finish_();
}

bool ESP32P4Recorder::open_clip_() {
  uint64_t total = 0, free_bytes = 0;
  esp_vfs_fat_info(this->mount_point_, &total, &free_bytes);
  const uint32_t secs = this->requested_secs_;
  // Twice the nominal bitrate for the clip, plus 32 MB of headroom.
  const uint64_t need = uint64_t(secs) * this->bitrate_ / 8 * 2 + 32ULL * 1024 * 1024;
  if (free_bytes < need) {
    ESP_LOGE(TAG, "Not enough space: %" PRIu64 " bytes free, %" PRIu64 " wanted", free_bytes, need);
    return false;
  }
  struct stat info {};
  int n = 1;
  for (; n < 10000; n++) {
    snprintf(this->path_, sizeof(this->path_), "%s/%s-%04d.mp4", this->mount_point_, this->file_prefix_, n);
    if (stat(this->path_, &info) != 0)
      break;
  }
  if (n == 10000) {
    ESP_LOGE(TAG, "No free file name");
    return false;
  }
  this->fd_ = open(this->path_, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "Opening %s failed (errno %d)", this->path_, errno);
    return false;
  }
  this->clip_audio_ = this->aac_ != nullptr;
  if (this->clip_audio_) {
#ifdef USE_ESP32P4_RECORDER_AUDIO
    esp_aac_enc_reset(this->aac_);
#endif
    // Drop any samples a late microphone callback left after the last clip. Safe here: the writer is
    // idle until RECORDING, and the microphone task only writes once audio_pending_ is set.
    this->pcm_tail_.store(this->pcm_head_.load(std::memory_order_acquire), std::memory_order_release);
    this->pcm_len_ = 0;
    this->pcm_total_ = 0;
    this->aac_frames_ = 0;
    this->audio_on_ = false;
    this->audio_ending_ = false;
    this->audio_done_ = false;
    this->audio_pending_ = false;
  }
  // At most one fragment a second (one GOP), plus slack, for the segment index.
  fmp4_begin(this->mux_, &ESP32P4Recorder::mux_out, this, this->camera_->get_width(), this->camera_->get_height(),
             this->clip_audio_, this->sample_rate_, AAC_DELAY_SAMPLES, secs + 32);
  this->bounce_fill_ = 0;
  this->last_sync_us_ = esp_timer_get_time();
  this->frames_ = 0;
  this->frame_bytes_ = 0;
  uint64_t head = this->head_;
  this->parse_ = head;
  this->tail_ = head;
  return true;
}

void ESP32P4Recorder::begin_finish_() {
  // End of the video: first to last frame plus one mean frame period.
  int64_t span = this->last_us_ - this->first_us_;
  int64_t end = this->frames_ > 1 ? this->first_us_ + span * this->frames_ / (this->frames_ - 1) : this->last_us_;
  this->end_frame_us_ = end;
  if (this->clip_audio_) {
    this->audio_end_us_ = end;
    this->audio_ending_ = true;
  }
  this->finish_started_us_ = esp_timer_get_time();
  this->state_ = State::FINISHING;
  xTaskNotifyGive(this->writer_task_);
}

/* Never drops a recorded frame: waits if the ring is full (counted as a stall). A frame never wraps;
 * if it does not fit before the end of the ring, the rest of the ring is skipped. */
void ESP32P4Recorder::ring_put_(const uint8_t *data, size_t len, int64_t t_us) {
  const size_t need = record_size(len);
  size_t skip = 0;
  bool stalled = false;
  for (;;) {
    uint64_t head = this->head_.load(std::memory_order_relaxed);
    size_t off = head % RING_SIZE;
    skip = RING_SIZE - off < need ? RING_SIZE - off : 0;
    if (RING_SIZE - (head - this->tail_.load(std::memory_order_acquire)) >= skip + need)
      break;
    stalled = true;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  }
  if (stalled)
    this->stalls_++;
  uint64_t head = this->head_.load(std::memory_order_relaxed);
  if (skip != 0) {
    if (skip >= sizeof(FrameHeader)) {
      FrameHeader pad{SKIP_MAGIC, 0, 0};
      memcpy(this->ring_ + head % RING_SIZE, &pad, sizeof(pad));
    }
    head += skip;
  }
  FrameHeader hdr{FRAME_MAGIC, static_cast<uint32_t>(len), t_us};
  memcpy(this->ring_ + head % RING_SIZE, &hdr, sizeof(hdr));
  memcpy(this->ring_ + head % RING_SIZE + sizeof(hdr), data, len);
  this->head_.store(head + need, std::memory_order_release);
  xTaskNotifyGive(this->writer_task_);
}

/* ---------------- writer task ---------------- */

void ESP32P4Recorder::writer_task(void *arg) { static_cast<ESP32P4Recorder *>(arg)->writer_loop_(); }

void ESP32P4Recorder::writer_loop_() {
  for (;;) {
    bool did = false;
    const State st = this->state_;
    if ((st == State::RECORDING || st == State::FINISHING) && this->clip_audio_)
      this->audio_pump_(false);
    if (st == State::RECORDING || st == State::FINISHING) {
      const uint64_t head = this->head_.load(std::memory_order_acquire);
      while (this->parse_ < head) {
        size_t off = this->parse_ % RING_SIZE;
        if (RING_SIZE - off < sizeof(FrameHeader)) {
          this->parse_ += RING_SIZE - off;
          continue;
        }
        FrameHeader hdr;
        memcpy(&hdr, this->ring_ + off, sizeof(hdr));
        if (hdr.magic == SKIP_MAGIC) {
          this->parse_ += RING_SIZE - off;
          continue;
        }
        if (hdr.magic != FRAME_MAGIC) {
          ESP_LOGE(TAG, "Ring record corrupt, dropping the rest");
          this->write_errors_++;
          this->parse_ = head;
          break;
        }
        uint8_t *frame = this->ring_ + off + sizeof(FrameHeader);
        if (fmp4_video_needs_cut(this->mux_, frame, hdr.len)) {
          if (this->clip_audio_)
            this->audio_pump_(false);
          // The fragment is on its way to the card: its frames' ring space can be reused.
          fmp4_fragment(this->mux_, hdr.t_us);
          this->tail_.store(this->parse_, std::memory_order_release);
          if (this->capture_task_ != nullptr)
            xTaskNotifyGive(this->capture_task_);
        }
        fmp4_video(this->mux_, frame, hdr.len, hdr.t_us);
        this->parse_ += record_size(hdr.len);
        did = true;
      }
    }
    if (!did && st == State::FINISHING) {
      // Wait (briefly) for the microphone to pass the last video frame, so the audio track is complete.
      bool audio_pending = this->clip_audio_ && !this->audio_done_ &&
                           esp_timer_get_time() - this->finish_started_us_ < AUDIO_END_TIMEOUT_US;
      if (!audio_pending)
        this->finish_clip_();
    }
    if (!did)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
}

void ESP32P4Recorder::finish_clip_() {
  if (this->clip_audio_) {
    this->audio_on_ = false;
    this->audio_pump_(true);
  }
  fmp4_fragment(this->mux_, this->end_frame_us_);
  if (this->bounce_fill_ != 0) {
    this->bounce_write_(this->bounce_fill_);
    this->bounce_fill_ = 0;
  }
  fmp4_finish(this->mux_, &ESP32P4Recorder::mux_patch);
  fsync(this->fd_);
  close(this->fd_);
  this->fd_ = -1;
  this->tail_.store(this->parse_, std::memory_order_release);
  memcpy(this->finished_path_, this->path_, sizeof(this->finished_path_));
  this->clips_finished_++;
  this->state_ = State::IDLE;
  App.wake_loop_threadsafe();
}

void ESP32P4Recorder::bounce_write_(size_t n) {
  size_t done = 0;
  while (done < n && this->fd_ >= 0) {
    ssize_t w = write(this->fd_, this->bounce_ + done, n - done);
    if (w <= 0) {
      this->write_errors_++;
      break;
    }
    done += w;
  }
  int64_t now = esp_timer_get_time();
  if (this->fd_ >= 0 && now - this->last_sync_us_ >= FSYNC_INTERVAL_US) {
    fsync(this->fd_);
    this->last_sync_us_ = esp_timer_get_time();
  }
}

void ESP32P4Recorder::mux_out(void *ctx, const void *data, size_t n) {
  auto *self = static_cast<ESP32P4Recorder *>(ctx);
  const auto *q = static_cast<const uint8_t *>(data);
  while (n != 0) {
    size_t k = std::min(CHUNK_SIZE - self->bounce_fill_, n);
    memcpy(self->bounce_ + self->bounce_fill_, q, k);
    self->bounce_fill_ += k;
    q += k;
    n -= k;
    if (self->bounce_fill_ == CHUNK_SIZE) {
      self->bounce_write_(CHUNK_SIZE);
      self->bounce_fill_ = 0;
    }
  }
}

void ESP32P4Recorder::mux_patch(void *ctx, uint64_t offset, const void *data, size_t n) {
  auto *self = static_cast<ESP32P4Recorder *>(ctx);
  if (self->fd_ >= 0 && lseek(self->fd_, static_cast<off_t>(offset), SEEK_SET) == static_cast<off_t>(offset)) {
    if (write(self->fd_, data, n) != static_cast<ssize_t>(n))
      self->write_errors_++;
  }
}

/* ---------------- audio ---------------- */

bool ESP32P4Recorder::audio_setup_() {
#ifdef USE_ESP32P4_RECORDER_AUDIO
  this->sample_rate_ = this->mic_source_->get_audio_stream_info().get_sample_rate();
  this->pcm_ring_ = static_cast<uint8_t *>(
      heap_caps_malloc(PCM_RING_SECONDS * this->sample_rate_ * 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
  if (this->pcm_ring_ == nullptr)
    return false;
    // The encoder allocates ~38 KB with plain malloc. With SPIRAM_USE_MALLOC, point every malloc at PSRAM
    // while it opens, as in the ESP-IDF build. ESPHome's default (SPIRAM_USE_CAPS_ALLOC) keeps malloc
    // internal; it opens after the H.264 encoder has taken its ~135 KB block, so that one is safe.
#ifdef CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(0);
#endif
  esp_aac_enc_config_t cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
  cfg.sample_rate = this->sample_rate_;
  cfg.channel = 1;
  cfg.bits_per_sample = 16;
  cfg.bitrate = AAC_BITRATE;
  cfg.adts_used = false;
  void *aac = nullptr;
  bool ok = esp_aac_enc_open(&cfg, sizeof(cfg), &aac) == ESP_AUDIO_ERR_OK;
#ifdef CONFIG_SPIRAM_USE_MALLOC
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
  if (!ok)
    return false;
  ESP_LOGD(TAG, "AAC encoder open, largest internal block now %u B",
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  esp_aac_enc_get_frame_size(aac, &this->aac_in_, &this->aac_out_);
  this->pcm_frame_ = static_cast<uint8_t *>(heap_caps_malloc(this->aac_in_, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
  this->aac_frame_ = static_cast<uint8_t *>(heap_caps_malloc(this->aac_out_, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
  if (this->pcm_frame_ == nullptr || this->aac_frame_ == nullptr) {
    esp_aac_enc_close(aac);
    return false;
  }
  this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_audio_(data); });
  this->aac_ = aac;
#endif
  return true;
}

/// Index of the sample captured at t_us, from the newest callback: its last sample arrived "now". The
/// callback latency is a constant offset this ignores; the A/V alignment of ESPHome builds is unmeasured.
uint64_t ESP32P4Recorder::sample_at_(int64_t t_us) const {
  int64_t back = (this->anchor_us_ - t_us) * int64_t(this->sample_rate_) / 1000000;
  int64_t idx = int64_t(this->anchor_total_) - back;
  return idx < 0 ? 0 : uint64_t(idx);
}

/// Microphone task. Keeps only the samples between the first and the last video frame of the clip.
void ESP32P4Recorder::on_audio_(const std::vector<uint8_t> &data) {
  const auto *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t n = data.size() / 2;
  const uint64_t idx0 = this->samples_total_;
  this->samples_total_ += n;
  this->anchor_us_ = esp_timer_get_time();
  this->anchor_total_ = this->samples_total_;

  if (this->audio_pending_) {
    this->audio_pending_ = false;
    // The PCM ring is not reset here: the writer may be reading it, and the last clip drained it anyway.
    this->audio_last_ = UINT64_MAX;
    uint64_t first = this->sample_at_(this->audio_t0_us_);
    if (first < idx0) {
      // The clip began before this block: those samples are gone, so pad with silence to keep sync.
      static const int16_t SILENCE[256] = {};
      for (uint64_t gap = idx0 - first; gap != 0;) {
        size_t k = std::min<uint64_t>(gap, 256);
        this->pcm_push_(SILENCE, k);
        gap -= k;
      }
      first = idx0;
    }
    this->audio_next_ = first;
    this->audio_on_ = true;
  }
  if (!this->audio_on_)
    return;
  if (this->audio_ending_ && this->audio_last_ == UINT64_MAX)
    this->audio_last_ = this->sample_at_(this->audio_end_us_);

  uint64_t from = std::max(this->audio_next_, idx0);
  uint64_t to = std::min(idx0 + n, this->audio_last_);
  if (to > from) {
    this->pcm_push_(samples + (from - idx0), to - from);
    this->audio_next_ = to;
  }
  if (this->audio_ending_ && this->audio_next_ >= this->audio_last_) {
    this->audio_on_ = false;
    this->audio_done_ = true;
    xTaskNotifyGive(this->writer_task_);
  }
}

void ESP32P4Recorder::pcm_push_(const int16_t *samples, size_t n) {
  const size_t ring = PCM_RING_SECONDS * this->sample_rate_ * 2;
  uint64_t head = this->pcm_head_.load(std::memory_order_relaxed);
  const uint64_t tail = this->pcm_tail_.load(std::memory_order_acquire);
  size_t bytes = n * 2;
  if (head + bytes - tail > ring) {
    this->audio_overflow_++;
    return;
  }
  // The ring size is even, so a sample never splits; a block may wrap once.
  size_t off = head % ring;
  size_t k = std::min(bytes, ring - off);
  memcpy(this->pcm_ring_ + off, samples, k);
  memcpy(this->pcm_ring_, reinterpret_cast<const uint8_t *>(samples) + k, bytes - k);
  this->pcm_head_.store(head + bytes, std::memory_order_release);
}

void ESP32P4Recorder::aac_encode_frame_() {
#ifdef USE_ESP32P4_RECORDER_AUDIO
  esp_audio_enc_in_frame_t in{};
  in.buffer = this->pcm_frame_;
  in.len = this->aac_in_;
  esp_audio_enc_out_frame_t out{};
  out.buffer = this->aac_frame_;
  out.len = this->aac_out_;
  if (esp_aac_enc_process(this->aac_, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0)
    fmp4_audio(this->mux_, this->aac_frame_, out.encoded_bytes);
  this->aac_frames_++;
#endif
}

/// Writer task. Encodes every whole AAC frame of PCM that has arrived. With all set (end of clip), pads
/// with silence until the decoded length, less the encoder delay the edit list skips, matches the PCM
/// length to the nearest frame: audio then ends within 32 ms of the last video frame.
void ESP32P4Recorder::audio_pump_(bool all) {
  const size_t ring = PCM_RING_SECONDS * this->sample_rate_ * 2;
  for (;;) {
    const uint64_t head = this->pcm_head_.load(std::memory_order_acquire);
    uint64_t tail = this->pcm_tail_.load(std::memory_order_relaxed);
    size_t avail = head - tail;
    if (avail == 0)
      break;
    size_t k = std::min(avail, size_t(this->aac_in_) - this->pcm_len_);
    size_t off = tail % ring;
    size_t first = std::min(k, ring - off);
    memcpy(this->pcm_frame_ + this->pcm_len_, this->pcm_ring_ + off, first);
    memcpy(this->pcm_frame_ + this->pcm_len_ + first, this->pcm_ring_, k - first);
    this->pcm_tail_.store(tail + k, std::memory_order_release);
    this->pcm_len_ += k;
    if (this->pcm_len_ == size_t(this->aac_in_)) {
      this->aac_encode_frame_();
      this->pcm_total_ += this->aac_in_ / 2;
      this->pcm_len_ = 0;
    }
  }
  if (!all)
    return;
  const uint64_t pcm = this->pcm_total_ + this->pcm_len_ / 2;
  const uint64_t want = (pcm + AAC_DELAY_SAMPLES + 512) / 1024;
  if (this->pcm_len_ != 0) {
    memset(this->pcm_frame_ + this->pcm_len_, 0, this->aac_in_ - this->pcm_len_);
    this->aac_encode_frame_();
  }
  this->pcm_len_ = 0;
  memset(this->pcm_frame_, 0, this->aac_in_);
  while (this->aac_frames_ < want)
    this->aac_encode_frame_();
  this->mux_->a_len = pcm;
}

void ESP32P4Recorder::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32-P4 recorder:\n"
                "  Mount point: %s, file prefix: %s\n"
                "  SD slot %u, %u-bit, %" PRIu32 " kHz, power LDO channel %d\n"
                "  H.264: %" PRIu32 " bit/s, QP %u-%u, GOP %u, max %" PRIu32 " s",
                this->mount_point_, this->file_prefix_, this->sd_slot_, this->d_pins_[1] >= 0 ? 4u : 1u,
                this->sd_freq_khz_, this->sd_ldo_channel_, this->bitrate_, this->min_qp_, this->max_qp_, this->gop_,
                this->max_duration_s_);
  if (this->aac_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Audio: AAC-LC %" PRIu32 " Hz mono, %d bit/s", this->sample_rate_, AAC_BITRATE);
  } else {
    ESP_LOGCONFIG(TAG, "  Audio: none");
  }
  if (this->card_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Card: %s, %" PRIu64 " MB", this->card_->cid.name,
                  uint64_t(this->card_->csd.capacity) * this->card_->csd.sector_size / (1024 * 1024));
  }
}

}  // namespace esphome::esp32p4_recorder

#endif  // USE_ESP32
