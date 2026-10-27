#ifdef USE_ESP32

#include "esp32p4_camera.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <driver/i2c_master.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <linux/videodev2.h>
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"

// From esp_video_isp_pipeline.h, which cannot be included here: it pulls in esp_ipa.h, a private
// requirement of esp_video. Status is esp_video_isp_pipeline_agc_status_t: 0 disables AGC, 1 enables it.
extern "C" esp_err_t esp_video_isp_pipeline_set_agc_status(int status);
static constexpr int ISP_AGC_DISABLE = 0;
static constexpr int ISP_AGC_ENABLE = 1;

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::esp32p4_camera {

static const char *const TAG = "esp32p4_camera";
static constexpr uint32_t FRAME_LOG_INTERVAL_MS = 60000;

/* ---------------- setup ---------------- */

void ESP32P4Camera::setup() {
  for (auto &f : this->slot_free_)
    f = true;

  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(this->i2c_bus_->get_port()), &bus) != ESP_OK) {
    this->mark_failed(LOG_STR("I2C bus handle not available"));
    return;
  }

  esp_video_init_csi_config_t csi{};
  csi.sccb_config.init_sccb = false;
  csi.sccb_config.i2c_handle = bus;
  csi.sccb_config.freq = 100000;
  csi.reset_pin = static_cast<gpio_num_t>(this->reset_pin_);
  csi.pwdn_pin = static_cast<gpio_num_t>(this->pwdn_pin_);
  csi.dont_init_ldo = !this->init_ldo_;
  esp_video_init_config_t cfg{};
  cfg.csi = &csi;
  uint32_t flags = ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP;
#ifdef CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE
  flags |= ESP_VIDEO_INIT_FLAGS_H264;  // the recorder opens the encoder device
#endif
  esp_err_t err = esp_video_init_with_flags(&cfg, flags);
  if (err != ESP_OK) {
    // A missing or faulty camera must never boot-loop the device: fail this component only.
    ESP_LOGE(TAG, "esp_video_init failed: %s (did a sensor answer on I2C?)", esp_err_to_name(err));
    this->mark_failed(LOG_STR("No camera sensor detected"));
    return;
  }
  if (!this->video_setup_()) {
    this->mark_failed(LOG_STR("Camera stream setup failed"));
    return;
  }

  // With stills enabled the UYVY buffer holds a full frame; the decimated path uses its start.
  const size_t uyvy_px = this->still_quality_ ? size_t(SRC_W) * SRC_H : size_t(OUT_W) * OUT_H;
  size_t uyvy_size = (uyvy_px * 2 + 127) & ~size_t(127);
  this->uyvy_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(128, 1, uyvy_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  jpeg_encode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 200};
  jpeg_encode_memory_alloc_cfg_t mcfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  if (this->uyvy_ == nullptr || jpeg_new_encoder_engine(&ecfg, &this->jpeg_) != ESP_OK) {
    this->mark_failed(LOG_STR("JPEG encoder init failed"));
    return;
  }
  for (auto &buf : this->jpeg_buf_) {
    buf = static_cast<uint8_t *>(
        jpeg_alloc_encoder_mem(this->still_quality_ ? STILL_JPEG_MAX : JPEG_MAX, &mcfg, &this->jpeg_buf_size_));
    if (buf == nullptr) {
      this->mark_failed(LOG_STR("JPEG buffer allocation failed"));
      return;
    }
  }

  this->ready_queue_ = xQueueCreate(JPEG_SLOTS, sizeof(uint8_t));
  this->decimate_done_ = xSemaphoreCreateBinary();
  // Core 0, as in the ESP-IDF build: capture at priority 10, JPEG below it at 6. The recorder's SD writer
  // busy-polls on core 1.
  if (this->ready_queue_ == nullptr || this->decimate_done_ == nullptr ||
      xTaskCreatePinnedToCore(&ESP32P4Camera::jpeg_task, "p4cam_jpeg", 4096, this, 6, &this->jpeg_task_, 0) != pdPASS ||
      xTaskCreatePinnedToCore(&ESP32P4Camera::capture_task, "p4cam", 8192, this, 10, &this->capture_task_, 0) !=
          pdPASS) {
    this->mark_failed(LOG_STR("Capture task creation failed"));
    return;
  }
}

/// Logs a failed V4L2 step. Returns true when the call succeeded.
static bool v4l2_ok(int ret, const char *step) {
  if (ret == 0)
    return true;
  ESP_LOGE(TAG, "Camera setup failed at %s (errno %d)", step, errno);
  return false;
}

bool ESP32P4Camera::video_setup_() {
  this->cap_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (!v4l2_ok(this->cap_fd_ < 0 ? -1 : 0, "open " ESP_VIDEO_MIPI_CSI_DEVICE_NAME))
    return false;

  struct v4l2_capability cap {};
  if (ioctl(this->cap_fd_, VIDIOC_QUERYCAP, &cap) == 0) {
    size_t n = std::min(sizeof(cap.card), sizeof(this->sensor_name_) - 1);
    memcpy(this->sensor_name_, cap.card, n);
    this->sensor_name_[n] = '\0';
  }

  struct v4l2_format fmt {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (!v4l2_ok(ioctl(this->cap_fd_, VIDIOC_G_FMT, &fmt), "VIDIOC_G_FMT"))
    return false;
  this->width_ = fmt.fmt.pix.width;
  this->height_ = fmt.fmt.pix.height;
  if (this->width_ != SRC_W || this->height_ != SRC_H) {
    ESP_LOGE(TAG, "Sensor default format is %" PRIu32 "x%" PRIu32 ", need %ux%u", this->width_, this->height_, SRC_W,
             SRC_H);
    return false;
  }

  fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = SRC_W;
  fmt.fmt.pix.height = SRC_H;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  if (!v4l2_ok(ioctl(this->cap_fd_, VIDIOC_S_FMT, &fmt), "VIDIOC_S_FMT"))
    return false;

  struct v4l2_requestbuffers req {};
  req.count = CAPTURE_BUFFERS;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (!v4l2_ok(ioctl(this->cap_fd_, VIDIOC_REQBUFS, &req), "VIDIOC_REQBUFS"))
    return false;
  for (uint8_t i = 0; i < CAPTURE_BUFFERS; i++) {
    struct v4l2_buffer b {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (!v4l2_ok(ioctl(this->cap_fd_, VIDIOC_QUERYBUF, &b), "VIDIOC_QUERYBUF"))
      return false;
    void *p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->cap_fd_, b.m.offset);
    if (!v4l2_ok(p == nullptr || p == MAP_FAILED ? -1 : 0, "mmap"))
      return false;
    this->cap_buf_[i] = static_cast<uint8_t *>(p);
    if (!v4l2_ok(ioctl(this->cap_fd_, VIDIOC_QBUF, &b), "VIDIOC_QBUF"))
      return false;
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  return v4l2_ok(ioctl(this->cap_fd_, VIDIOC_STREAMON, &type), "VIDIOC_STREAMON");
}

/* ---------------- capture task ---------------- */

void ESP32P4Camera::capture_task(void *arg) { static_cast<ESP32P4Camera *>(arg)->capture_loop_(); }

int ESP32P4Camera::free_slot_() const {
  for (uint8_t i = 0; i < JPEG_SLOTS; i++) {
    if (this->slot_free_[i])
      return i;
  }
  return -1;
}

/* One loop owns the sensor. Every frame goes to the frame listeners (the recorder's H.264 encode runs
 * here, inline). A JPEG is only started when one is wanted and the JPEG task is idle; the capture loop
 * never waits for it. The JPEG task holds the capture buffer while it decimates, and the buffer goes back
 * to the sensor on a later pass, once it has finished. */
void ESP32P4Camera::capture_loop_() {
  int held = -1;
  int64_t last_jpeg_us = 0;
  const struct timeval no_wait = {0, 0};
  const struct timeval wait = {1, 0};

  for (;;) {
    struct v4l2_buffer cap {};
    cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap.memory = V4L2_MEMORY_MMAP;
    int64_t t_ask = esp_timer_get_time();
    if (ioctl(this->cap_fd_, VIDIOC_DQBUF, &cap) != 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    int64_t now = esp_timer_get_time();
    bool fresh = now - t_ask > 3000;  // we waited for it, so it has just completed

    // When a listener makes the loop slower than the sensor, completed frames queue up FIFO. Drain to
    // the newest so a frame is at most one period old; older ones go straight back to the sensor.
    ioctl(this->cap_fd_, VIDIOC_S_DQBUF_TIMEOUT, &no_wait);
    for (;;) {
      struct v4l2_buffer next {};
      next.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      next.memory = V4L2_MEMORY_MMAP;
      if (ioctl(this->cap_fd_, VIDIOC_DQBUF, &next) != 0)
        break;
      ioctl(this->cap_fd_, VIDIOC_QBUF, &cap);
      cap = next;
      fresh = false;
      this->drained_++;
    }
    ioctl(this->cap_fd_, VIDIOC_S_DQBUF_TIMEOUT, &wait);
    // Capture-complete time: exact if we waited, otherwise somewhere in the last period (~half of 33 ms).
    const int64_t t_frame = fresh ? now : now - 17000;
    this->captured_++;

    if (held >= 0 && xSemaphoreTake(this->decimate_done_, 0) == pdTRUE) {
      struct v4l2_buffer hb {};
      hb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      hb.memory = V4L2_MEMORY_MMAP;
      hb.index = held;
      ioctl(this->cap_fd_, VIDIOC_QBUF, &hb);
      held = -1;
    }

    bool kicked = false;
    if (held < 0 && this->jpeg_idle_ && this->has_requested_image_() &&
        now - last_jpeg_us >= int64_t(this->max_update_interval_) * 1000) {
      int slot = this->free_slot_();
      if (slot >= 0) {
        this->jpeg_src_ = this->cap_buf_[cap.index];
        this->jpeg_slot_ = slot;
        this->jpeg_still_ = this->still_quality_ && this->still_pending_.exchange(false);
        this->jpeg_idle_ = false;
        xTaskNotifyGive(this->jpeg_task_);
        last_jpeg_us = now;
        kicked = true;
      }
    }

    for (auto *listener : this->frame_listeners_)
      listener->on_raw_frame(this->cap_buf_[cap.index], cap.bytesused, t_frame);

    if (kicked) {
      held = cap.index;
    } else {
      ioctl(this->cap_fd_, VIDIOC_QBUF, &cap);
    }
  }
}

/* ---------------- JPEG task ---------------- */

void ESP32P4Camera::jpeg_task(void *arg) { static_cast<ESP32P4Camera *>(arg)->jpeg_loop_(); }

void ESP32P4Camera::jpeg_loop_() {
  jpeg_encode_cfg_t jcfg{};
  jcfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;  // UYVY
  jcfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;   // MCU 16x8: 360 and 1080 are multiples of 8, not 16

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint8_t slot = this->jpeg_slot_;
    const bool still = this->jpeg_still_;
    if (still) {
      this->convert_full_(this->jpeg_src_);
      jcfg.width = SRC_W;
      jcfg.height = SRC_H;
      jcfg.image_quality = this->still_quality_;
    } else {
      this->decimate3_(this->jpeg_src_);
      jcfg.width = OUT_W;
      jcfg.height = OUT_H;
      jcfg.image_quality = this->jpeg_quality_;
    }
    xSemaphoreGive(this->decimate_done_);  // the capture buffer may go back to the sensor now

    uint32_t out_len = 0;
    esp_err_t err = jpeg_encoder_process(this->jpeg_, &jcfg, this->uyvy_, jcfg.width * jcfg.height * 2,
                                         this->jpeg_buf_[slot], this->jpeg_buf_size_, &out_len);
    if (err != ESP_OK || out_len == 0) {
      this->encode_errors_++;
    } else {
      this->jpeg_len_[slot] = out_len;
      this->slot_free_[slot] = false;
      xQueueSend(this->ready_queue_, &slot, 0);
      App.wake_loop_threadsafe();
    }
    this->jpeg_idle_ = true;
  }
}

/* P4 ISP YUV420 is packed per line, 3 bytes per 2 pixels: even lines U Y Y, odd lines V Y Y.
 * Output rows r (even) and r+1 come from source rows 3r (U line) and 3r+3 (V line), so each
 * pair of output rows shares one U and one V: 4:2:0 chroma, which the JPEG keeps anyway.
 * Reads one source row in three (~1 MB a frame) and needs no 2D-DMA, so it does not contend
 * with an H.264 encode the way the PPA does. */
void ESP32P4Camera::decimate3_(const uint8_t *src) {
  const int stride = SRC_W * 3 / 2;
  esp_cache_msync((void *) src, stride * SRC_H, ESP_CACHE_MSYNC_FLAG_DIR_M2C);  // CSI wrote it by DMA
  for (int r = 0; r < OUT_H; r += 2) {
    const uint8_t *a = src + (3 * r) * stride;
    const uint8_t *b = src + (3 * r + 3) * stride;
    uint8_t *oa = this->uyvy_ + r * OUT_W * 2;
    uint8_t *ob = oa + OUT_W * 2;
    for (int k = 0; k < OUT_W / 2; k++) {
      const uint8_t *pa = a + 9 * k, *pb = b + 9 * k;
      uint8_t u = pa[0], v = pb[0];
      oa[0] = u;
      oa[1] = pa[1];
      oa[2] = v;
      oa[3] = pa[5];
      ob[0] = u;
      ob[1] = pb[1];
      ob[2] = v;
      ob[3] = pb[5];
      oa += 4;
      ob += 4;
    }
  }
}

/* Same source layout as decimate3_, every row and pixel kept: rows 2r (U line) and 2r+1 (V line) share
 * their chroma, as in the decimated path. A full frame is ~3 MB read and ~4 MB written, so the JPEG task
 * holds the capture buffer for longer; stills are rare and the capture loop has buffers to spare. */
void ESP32P4Camera::convert_full_(const uint8_t *src) {
  const int stride = SRC_W * 3 / 2;
  esp_cache_msync((void *) src, stride * SRC_H, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  for (int r = 0; r < SRC_H; r += 2) {
    const uint8_t *a = src + r * stride;
    const uint8_t *b = a + stride;
    uint8_t *oa = this->uyvy_ + r * SRC_W * 2;
    uint8_t *ob = oa + SRC_W * 2;
    for (int k = 0; k < SRC_W / 2; k++) {
      uint8_t u = a[0], v = b[0];
      oa[0] = u;
      oa[1] = a[1];
      oa[2] = v;
      oa[3] = a[2];
      ob[0] = u;
      ob[1] = b[1];
      ob[2] = v;
      ob[3] = b[2];
      a += 3;
      b += 3;
      oa += 4;
      ob += 4;
    }
  }
}

/* ---------------- main loop ---------------- */

void ESP32P4Camera::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->current_image_ && !this->has_requested_image_()) {
    if (this->idle_update_interval_ != 0 && now - this->last_idle_request_ > this->idle_update_interval_) {
      this->last_idle_request_ = now;
      this->request_image(camera::IDLE);
    } else {
      // Frames that finished after their request was served are stale: free the slots.
      uint8_t stale;
      while (xQueueReceive(this->ready_queue_, &stale, 0) == pdTRUE)
        this->slot_free_[stale] = true;
      return;
    }
  }

  // Every listener has finished with the current image: its slot can be refilled.
  if (this->current_image_ && this->current_image_.use_count() == 1) {
    this->slot_free_[this->current_image_->get_slot()] = true;
    this->current_image_.reset();
  }

  uint8_t slot;
  if (xQueueReceive(this->ready_queue_, &slot, 0) != pdTRUE)
    return;
  if (!this->has_requested_image_() || this->current_image_) {
    // Nobody wants it any more, or clients are still reading the previous frame: drop it.
    this->slot_free_[slot] = true;
    return;
  }

  this->current_image_ = std::make_shared<ESP32P4CameraImage>(slot, this->jpeg_buf_[slot], this->jpeg_len_[slot],
                                                              this->single_requesters_ | this->stream_requesters_);
  if (this->frame_count_ == 0)
    this->last_log_time_ = now;
  this->frame_count_++;
  if (now - this->last_log_time_ >= FRAME_LOG_INTERVAL_MS) {
    // Totals since boot: captured and drained show whether a frame listener is slowing the sensor.
    ESP_LOGD(TAG,
             "Sent %u images in last %" PRIu32 "s; since boot %" PRIu32 " captured, %" PRIu32 " drained, %" PRIu32
             " JPEG errors",
             this->frame_count_, FRAME_LOG_INTERVAL_MS / 1000, this->captured_.load(), this->drained_.load(),
             this->encode_errors_.load());
    this->last_log_time_ = now;
    this->frame_count_ = 0;
  }
  for (auto *listener : this->listeners_)
    listener->on_camera_image(this->current_image_);
  this->single_requesters_ = 0;
}

void ESP32P4Camera::capture_still() {
  if (this->still_quality_ == 0) {
    ESP_LOGW(TAG, "capture_still ignored: set still_jpeg_quality to enable stills");
    return;
  }
  this->still_pending_ = true;
}

bool ESP32P4Camera::set_exposure_lock(bool locked) {
  esp_err_t err = esp_video_isp_pipeline_set_agc_status(locked ? ISP_AGC_DISABLE : ISP_AGC_ENABLE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Exposure %s failed: %s", locked ? "lock" : "unlock", esp_err_to_name(err));
    return false;
  }
  ESP_LOGD(TAG, "Exposure %s", locked ? "locked" : "unlocked");
  return true;
}

void ESP32P4Camera::request_image(camera::CameraRequester requester) { this->single_requesters_ |= (1U << requester); }

void ESP32P4Camera::start_stream(camera::CameraRequester requester) {
  for (auto *listener : this->listeners_)
    listener->on_stream_start();
  this->stream_requesters_ |= (1U << requester);
}

void ESP32P4Camera::stop_stream(camera::CameraRequester requester) {
  for (auto *listener : this->listeners_)
    listener->on_stream_stop();
  this->stream_requesters_ &= ~(1U << requester);
}

void ESP32P4Camera::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32-P4 camera:\n"
                "  Sensor: %s\n"
                "  Capture: %" PRIu32 "x%" PRIu32 " YUV420, output %ux%u JPEG q%u\n"
                "  Max interval: %" PRIu32 " ms, idle interval: %" PRIu32 " ms\n"
                "  Stills: %s, q%u",
                this->sensor_name_, this->width_, this->height_, OUT_W, OUT_H, this->jpeg_quality_,
                this->max_update_interval_, this->idle_update_interval_, this->still_quality_ ? "1920x1080" : "off",
                this->still_quality_);
}

/* ---------------- image reader ---------------- */

void ESP32P4CameraImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}
size_t ESP32P4CameraImageReader::available() const {
  if (!this->image_)
    return 0;
  return this->image_->get_data_length() - this->offset_;
}
uint8_t *ESP32P4CameraImageReader::peek_data_buffer() { return this->image_->get_data_buffer() + this->offset_; }
void ESP32P4CameraImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }
void ESP32P4CameraImageReader::return_image() { this->image_.reset(); }

}  // namespace esphome::esp32p4_camera

#endif  // USE_ESP32
