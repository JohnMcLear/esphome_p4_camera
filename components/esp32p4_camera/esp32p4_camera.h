#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/jpeg_encode.h>

#include "esphome/components/camera/camera.h"
#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

namespace esphome::esp32p4_camera {

/// Sensor frame is 1920x1080, decimated by 3 on the CPU.
static constexpr uint16_t SRC_W = 1920;
static constexpr uint16_t SRC_H = 1080;
static constexpr uint16_t OUT_W = SRC_W / 3;
static constexpr uint16_t OUT_H = SRC_H / 3;
/// Five, not the stock three: the JPEG path holds one buffer while it decimates, and a frame
/// listener (the recorder) must never wait for it.
static constexpr uint8_t CAPTURE_BUFFERS = 5;
/// Two JPEG slots: one can be held by API or web clients while the next is encoded.
static constexpr uint8_t JPEG_SLOTS = 2;
static constexpr size_t JPEG_MAX = 256 * 1024;
/// A full-frame still (1920x1080, no decimation) at high quality needs a larger slot.
static constexpr size_t STILL_JPEG_MAX = 1024 * 1024;

/** Receives every captured frame, on the capture task, before the buffer goes back to the sensor.
 *  The frame is 1920x1080 P4 ISP YUV420 (the layout the hardware H.264 encoder takes). The buffer
 *  is only valid during the call. Keep the work bounded: a slow listener lowers the frame rate. */
class FrameListener {
 public:
  virtual void on_raw_frame(uint8_t *yuv420, size_t length, int64_t t_us) = 0;
  virtual ~FrameListener() = default;
};

class ESP32P4CameraImage : public camera::CameraImage {
 public:
  ESP32P4CameraImage(uint8_t slot, uint8_t *data, size_t length, uint8_t requesters)
      : slot_(slot), data_(data), length_(length), requesters_(requesters) {}
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1 << requester)) != 0;
  }
  uint8_t get_slot() const { return this->slot_; }

 protected:
  uint8_t slot_;
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
};

class ESP32P4CameraImageReader : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override;
  size_t available() const override;
  uint8_t *peek_data_buffer() override;
  void consume_data(size_t consumed) override;
  void return_image() override;

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

class ESP32P4Camera final : public camera::Camera {
 public:
  void set_i2c_bus(i2c::InternalI2CBus *bus) { this->i2c_bus_ = bus; }
  void set_reset_pin(int8_t pin) { this->reset_pin_ = pin; }
  void set_power_down_pin(int8_t pin) { this->pwdn_pin_ = pin; }
  void set_init_ldo(bool init_ldo) { this->init_ldo_ = init_ldo; }
  void set_jpeg_quality(uint8_t quality) { this->jpeg_quality_ = quality; }
  void set_max_update_interval(uint32_t ms) { this->max_update_interval_ = ms; }
  void set_idle_update_interval(uint32_t ms) { this->idle_update_interval_ = ms; }
  /// Non-zero enables full-frame stills (and allocates the larger buffers they need).
  void set_still_jpeg_quality(uint8_t quality) { this->still_quality_ = quality; }
  /// Register before setup(). Called on the capture task for every frame.
  void add_frame_listener(FrameListener *listener) { this->frame_listeners_.push_back(listener); }
  uint32_t get_width() const { return this->width_; }
  uint32_t get_height() const { return this->height_; }

  /// The next image delivered to any requester is the full sensor frame, not the decimated one.
  void capture_still();
  /// Freezes (true) or resumes (false) the ISP's auto exposure and gain. Returns false if the ISP refused.
  bool set_exposure_lock(bool locked);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new ESP32P4CameraImageReader; }
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  bool video_setup_();
  bool has_requested_image_() const { return this->single_requesters_ || this->stream_requesters_; }
  int free_slot_() const;
  static void capture_task(void *arg);
  static void jpeg_task(void *arg);
  void capture_loop_();
  void jpeg_loop_();
  void decimate3_(const uint8_t *src);
  void convert_full_(const uint8_t *src);

  i2c::InternalI2CBus *i2c_bus_{nullptr};
  int8_t reset_pin_{-1};
  int8_t pwdn_pin_{-1};
  bool init_ldo_{true};
  uint8_t jpeg_quality_{40};
  uint32_t max_update_interval_{100};
  uint32_t idle_update_interval_{10000};
  uint8_t still_quality_{0};

  int cap_fd_{-1};
  uint8_t *cap_buf_[CAPTURE_BUFFERS]{};
  uint32_t width_{0};
  uint32_t height_{0};
  char sensor_name_[33]{"unknown"};

  uint8_t *uyvy_{nullptr};
  jpeg_encoder_handle_t jpeg_{nullptr};
  uint8_t *jpeg_buf_[JPEG_SLOTS]{};
  size_t jpeg_buf_size_{0};
  size_t jpeg_len_[JPEG_SLOTS]{};
  /// Slot ownership: true while a slot is free for the JPEG task to fill.
  std::atomic<bool> slot_free_[JPEG_SLOTS];
  /// Filled slots, JPEG task -> main loop.
  QueueHandle_t ready_queue_{nullptr};
  /// Capture -> JPEG hand-off: source buffer and target slot, valid while jpeg_idle_ is false.
  const uint8_t *jpeg_src_{nullptr};
  uint8_t jpeg_slot_{0};
  bool jpeg_still_{false};
  /// Set by capture_still(), taken by the capture task when it starts the next JPEG.
  std::atomic<bool> still_pending_{false};
  std::atomic<bool> jpeg_idle_{true};
  /// Given by the JPEG task once it has finished reading the capture buffer.
  SemaphoreHandle_t decimate_done_{nullptr};
  TaskHandle_t capture_task_{nullptr};
  TaskHandle_t jpeg_task_{nullptr};

  std::shared_ptr<ESP32P4CameraImage> current_image_;
  std::atomic<uint8_t> single_requesters_{0};
  std::atomic<uint8_t> stream_requesters_{0};
  std::vector<camera::CameraListener *> listeners_;
  std::vector<FrameListener *> frame_listeners_;

  uint32_t last_idle_request_{0};
  uint32_t last_log_time_{0};
  uint16_t frame_count_{0};
  std::atomic<uint32_t> captured_{0};
  std::atomic<uint32_t> drained_{0};
  std::atomic<uint32_t> encode_errors_{0};
};

template<typename... Ts> class CaptureStillAction : public Action<Ts...>, public Parented<ESP32P4Camera> {
 public:
  void play(const Ts &...x) override { this->parent_->capture_still(); }
};

template<typename... Ts> class ExposureLockAction : public Action<Ts...>, public Parented<ESP32P4Camera> {
 public:
  TEMPLATABLE_VALUE(bool, locked)
  void play(const Ts &...x) override { this->parent_->set_exposure_lock(this->locked_.value(x...)); }
};

}  // namespace esphome::esp32p4_camera

#endif  // USE_ESP32
