#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include "esphome/components/esp32p4_camera/esp32p4_camera.h"
#ifdef USE_ESP32P4_RECORDER_AUDIO
#include "esphome/components/microphone/microphone_source.h"
#endif
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "fmp4_mux.h"

namespace esphome::esp32p4_recorder {

enum class State : uint8_t {
  IDLE,       ///< no recording, a start is accepted
  STARTING,   ///< start requested, applied on the capture task
  RECORDING,  ///< frames go to the ring
  FINISHING,  ///< capture has stopped feeding; the writer drains the ring and closes the file
};

/** H.264 recordings of the camera's 1920x1080 frames to an SD card, as fragmented MP4 (one fragment
 *  per GOP), so a clip cut short by a power loss still plays up to its last fragment.
 *
 *  Threads: the H.264 encode runs inline on the camera's capture task (on_raw_frame). Encoded frames
 *  go to a PSRAM ring; the writer task muxes them and writes the card in 64 KB pieces through an
 *  internal DMA bounce buffer (single-frame writes managed only ~290 kB/s on a test card). The main
 *  loop only sets requests and reports state changes. */
class ESP32P4Recorder : public Component, public esp32p4_camera::FrameListener {
 public:
  void set_camera(esp32p4_camera::ESP32P4Camera *camera) { this->camera_ = camera; }
  void set_mount_point(const char *mount_point) { this->mount_point_ = mount_point; }
  void set_file_prefix(const char *prefix) { this->file_prefix_ = prefix; }
  void set_sd_pins(int8_t clk, int8_t cmd, int8_t d0, int8_t d1, int8_t d2, int8_t d3) {
    this->clk_pin_ = clk;
    this->cmd_pin_ = cmd;
    this->d_pins_[0] = d0;
    this->d_pins_[1] = d1;
    this->d_pins_[2] = d2;
    this->d_pins_[3] = d3;
  }
  void set_sd_slot(uint8_t slot) { this->sd_slot_ = slot; }
  void set_sd_frequency_khz(uint32_t khz) { this->sd_freq_khz_ = khz; }
  void set_sd_power_ldo_channel(int8_t channel) { this->sd_ldo_channel_ = channel; }
  void set_bitrate(uint32_t bitrate) { this->bitrate_ = bitrate; }
  void set_qp_range(uint8_t min_qp, uint8_t max_qp) {
    this->min_qp_ = min_qp;
    this->max_qp_ = max_qp;
  }
  void set_gop(uint8_t gop) { this->gop_ = gop; }
  void set_max_duration(uint32_t seconds) { this->max_duration_s_ = seconds; }
#ifdef USE_ESP32P4_RECORDER_AUDIO
  void set_microphone_source(microphone::MicrophoneSource *source) { this->mic_source_ = source; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the camera, which registers the encoder, and after Wi-Fi: esp32_hosted brings its SDIO slot
  // up first, the order that worked with an SD card on the other slot in the ESP-IDF build.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Start a recording of `seconds` (capped at max_duration). Ignored unless idle.
  void start(uint32_t seconds);
  void stop();
  bool is_recording() const;
  const std::string &get_last_file() const { return this->last_file_; }

  void add_on_start_callback(std::function<void(const std::string &)> &&cb) {
    this->start_callback_.add(std::move(cb));
  }
  void add_on_finished_callback(std::function<void(const std::string &)> &&cb) {
    this->finished_callback_.add(std::move(cb));
  }

  void on_raw_frame(uint8_t *yuv420, size_t length, int64_t t_us) override;

 protected:
  bool mount_card_();
  bool encoder_setup_();
  bool writer_setup_();
  bool open_clip_();
  void begin_finish_();
  void ring_put_(const uint8_t *data, size_t len, int64_t t_us);
  static void writer_task(void *arg);
  void writer_loop_();
  void finish_clip_();
  void bounce_write_(size_t n);
  static void mux_out(void *ctx, const void *data, size_t n);
  static void mux_patch(void *ctx, uint64_t offset, const void *data, size_t n);
  // Audio: the microphone task pushes PCM, the writer task encodes it to AAC.
  bool audio_setup_();
  void on_audio_(const std::vector<uint8_t> &data);
  uint64_t sample_at_(int64_t t_us) const;
  void pcm_push_(const int16_t *samples, size_t n);
  void audio_pump_(bool all);
  void aac_encode_frame_();

  esp32p4_camera::ESP32P4Camera *camera_{nullptr};
  const char *mount_point_{"/sdcard"};
  const char *file_prefix_{"rec"};
  uint8_t sd_slot_{0};
  int8_t clk_pin_{-1};
  int8_t cmd_pin_{-1};
  int8_t d_pins_[4]{-1, -1, -1, -1};
  uint32_t sd_freq_khz_{SDMMC_FREQ_DEFAULT};
  int8_t sd_ldo_channel_{-1};
  uint32_t bitrate_{4000000};
  uint8_t min_qp_{25};
  uint8_t max_qp_{45};
  uint8_t gop_{30};
  uint32_t max_duration_s_{600};

  sdmmc_card_t *card_{nullptr};
  int enc_fd_{-1};
  uint8_t *enc_out_{nullptr};

  // Recording state. Requests come from the main loop and are applied on the capture task, which owns
  // the encoder; the file belongs to the writer task once a recording is running.
  std::atomic<bool> ready_{false};
  std::atomic<State> state_{State::IDLE};
  std::atomic<uint32_t> requested_secs_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint32_t> clips_started_{0};
  std::atomic<uint32_t> clips_finished_{0};
  uint32_t reported_started_{0};
  uint32_t reported_finished_{0};
  bool mic_running_{false};
  bool wait_idr_{false};
  int64_t end_us_{0};
  int64_t first_us_{0};
  int64_t last_us_{0};
  uint32_t frames_{0};
  uint64_t frame_bytes_{0};
  char path_[96]{};
  char finished_path_[96]{};
  std::string last_file_;

  // PSRAM ring of encoded frames, each with a header; a frame never wraps.
  uint8_t *ring_{nullptr};
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};
  uint64_t parse_{0};
  std::atomic<int64_t> end_frame_us_{0};

  // Writer task: muxer, bounce buffer and file.
  Fmp4Muxer *mux_{nullptr};
  uint8_t *bounce_{nullptr};
  size_t bounce_fill_{0};
  int fd_{-1};
  int64_t last_sync_us_{0};
  TaskHandle_t writer_task_{nullptr};
  TaskHandle_t capture_task_{nullptr};

  // Audio. Sample indices count samples since boot; the newest callback anchors index to time.
#ifdef USE_ESP32P4_RECORDER_AUDIO
  microphone::MicrophoneSource *mic_source_{nullptr};
#endif
  uint32_t sample_rate_{16000};
  bool clip_audio_{false};  ///< this clip has an audio track
  uint8_t *pcm_ring_{nullptr};
  std::atomic<uint64_t> pcm_head_{0};  // bytes
  std::atomic<uint64_t> pcm_tail_{0};
  uint64_t samples_total_{0};
  int64_t anchor_us_{0};
  uint64_t anchor_total_{0};
  std::atomic<bool> audio_pending_{false};
  std::atomic<bool> audio_on_{false};
  std::atomic<bool> audio_ending_{false};
  std::atomic<bool> audio_done_{false};
  int64_t audio_t0_us_{0};
  int64_t audio_end_us_{0};
  uint64_t audio_next_{0};
  uint64_t audio_last_{UINT64_MAX};
  int64_t finish_started_us_{0};
  void *aac_{nullptr};
  int aac_in_{2048};
  int aac_out_{1024};
  uint8_t *pcm_frame_{nullptr};
  uint8_t *aac_frame_{nullptr};
  size_t pcm_len_{0};
  uint64_t pcm_total_{0};
  uint64_t aac_frames_{0};
  std::atomic<uint32_t> audio_overflow_{0};

  // Counters since boot.
  std::atomic<uint32_t> stalls_{0};
  std::atomic<uint32_t> oversize_{0};
  std::atomic<uint32_t> write_errors_{0};
  std::atomic<uint32_t> encode_errors_{0};

  CallbackManager<void(const std::string &)> start_callback_;
  CallbackManager<void(const std::string &)> finished_callback_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<ESP32P4Recorder> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->start(this->duration_.value(x...) / 1000); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<ESP32P4Recorder> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts> class IsRecordingCondition : public Condition<Ts...>, public Parented<ESP32P4Recorder> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_recording(); }
};

class StartTrigger : public Trigger<std::string> {
 public:
  explicit StartTrigger(ESP32P4Recorder *parent) {
    parent->add_on_start_callback([this](const std::string &file) { this->trigger(file); });
  }
};

class FinishedTrigger : public Trigger<std::string> {
 public:
  explicit FinishedTrigger(ESP32P4Recorder *parent) {
    parent->add_on_finished_callback([this](const std::string &file) { this->trigger(file); });
  }
};

}  // namespace esphome::esp32p4_recorder

#endif  // USE_ESP32
