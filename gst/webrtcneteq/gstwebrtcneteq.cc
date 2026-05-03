/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "api/audio/audio_frame.h"
#include "api/audio_codecs/audio_format.h"
#include "api/audio_codecs/opus_audio_decoder_factory.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/neteq/default_neteq_factory.h"
#include "api/neteq/neteq.h"
#include "api/rtp_headers.h"
#include "api/units/timestamp.h"
#include "rtc_base/logging.h"

#ifndef PACKAGE
#define PACKAGE "gstwebrtcneteq"
#endif

namespace {

constexpr int kDefaultLatencyMs = 200;
constexpr int kDefaultMaxLatencyMs = 0;
constexpr int kDefaultEosDrainMs = 1000;
constexpr int kMaxNetEqDelayMs = 10000;
constexpr int kMinClockRateHz = 8000;
constexpr int kMaxClockRateHz = 48000;
constexpr int kOutputFrameMs = 10;
constexpr guint kMaxOutputChannels = 2;
constexpr guint kMaxOutputBufferBytes =
    (kMaxClockRateHz * kOutputFrameMs / 1000) * kMaxOutputChannels *
    sizeof(int16_t);

struct NetEqState {
  explicit NetEqState(webrtc::Environment environment)
      : env(std::move(environment)) {}

  webrtc::Environment env;
  std::unique_ptr<webrtc::NetEq> neteq;
};

struct PlayoutState {
  gboolean stop;
  gboolean eos_received;
  gboolean joining;
};

enum class ClockWaitResult {
  kReady,
  kStopped,
  kError,
};

class GMutexLock {
 public:
  explicit GMutexLock(GMutex* mutex) : mutex_(mutex) { g_mutex_lock(mutex_); }
  ~GMutexLock() { g_mutex_unlock(mutex_); }

  GMutexLock(const GMutexLock&) = delete;
  GMutexLock& operator=(const GMutexLock&) = delete;

  GMutex* get() const { return mutex_; }

 private:
  GMutex* const mutex_;
};

class GstBufferPtr {
 public:
  explicit GstBufferPtr(GstBuffer* buffer = nullptr) : buffer_(buffer) {}
  ~GstBufferPtr() {
    if (buffer_ != nullptr) {
      gst_buffer_unref(buffer_);
    }
  }

  GstBufferPtr(const GstBufferPtr&) = delete;
  GstBufferPtr& operator=(const GstBufferPtr&) = delete;

  GstBuffer* get() const { return buffer_; }
  explicit operator bool() const { return buffer_ != nullptr; }

  GstBuffer* release() {
    GstBuffer* buffer = buffer_;
    buffer_ = nullptr;
    return buffer;
  }

 private:
  GstBuffer* buffer_;
};

static GOnce output_buffer_pool_once = G_ONCE_INIT;

static gpointer gst_webrtc_net_eq_init_output_buffer_pool(gpointer) {
  GstBufferPool* pool = gst_buffer_pool_new();
  if (pool == nullptr) {
    return nullptr;
  }

  GstStructure* config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, nullptr, kMaxOutputBufferBytes, 0,
                                    0);
  if (!gst_buffer_pool_set_config(pool, config)) {
    gst_object_unref(pool);
    return nullptr;
  }

  if (!gst_buffer_pool_set_active(pool, TRUE)) {
    gst_object_unref(pool);
    return nullptr;
  }

  return pool;
}

static GstBufferPool* gst_webrtc_net_eq_output_buffer_pool() {
  return static_cast<GstBufferPool*>(
      g_once(&output_buffer_pool_once,
             gst_webrtc_net_eq_init_output_buffer_pool, nullptr));
}

static GstBuffer* gst_webrtc_net_eq_new_output_buffer(size_t output_size) {
  GstBufferPool* pool = gst_webrtc_net_eq_output_buffer_pool();
  if (pool != nullptr && output_size <= kMaxOutputBufferBytes) {
    GstBuffer* buffer = nullptr;
    const GstFlowReturn flow =
        gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr);
    if (flow == GST_FLOW_OK && buffer != nullptr) {
      gst_buffer_resize(buffer, 0, output_size);
      return buffer;
    }
  }

  return gst_buffer_new_allocate(nullptr, output_size, nullptr);
}

}  // namespace

typedef struct _GstWebrtcNetEq GstWebrtcNetEq;
typedef struct _GstWebrtcNetEqClass GstWebrtcNetEqClass;

struct _GstWebrtcNetEq {
  GstElement parent;

  GstPad* sinkpad;
  GstPad* srcpad;

  GMutex lock;
  GCond cond;
  // Published only while the playout task is blocked in gst_clock_id_wait().
  // Other threads unschedule this ID to wake the task during stop/flush/EOS.
  GstClockID current_wait_clock_id;
  NetEqState* state;

  gint payload_type;
  gint clock_rate_hz;
  gint latency_ms;
  gint max_latency_ms;
  gint channels;
  gint eos_drain_ms;
  gboolean configured;

  GstSegment segment;
  gboolean segment_received;
  gboolean output_base_pts_set;
  GstClockTime output_base_pts;

  PlayoutState playout;
};

struct _GstWebrtcNetEqClass {
  GstElementClass parent_class;
};

namespace {

class PlayoutClockIdScope {
 public:
  PlayoutClockIdScope(GstWebrtcNetEq* self, GstClockID clock_id)
      : self_(self), clock_id_(clock_id) {}
  ~PlayoutClockIdScope() {
    if (clock_id_ == nullptr) {
      return;
    }
    {
      GMutexLock lock(&self_->lock);
      if (self_->current_wait_clock_id == clock_id_) {
        self_->current_wait_clock_id = nullptr;
      }
    }
    gst_clock_id_unref(clock_id_);
  }

  PlayoutClockIdScope(const PlayoutClockIdScope&) = delete;
  PlayoutClockIdScope& operator=(const PlayoutClockIdScope&) = delete;

  GstClockID get() const { return clock_id_; }

 private:
  GstWebrtcNetEq* const self_;
  GstClockID const clock_id_;
};

}  // namespace

#define GST_TYPE_WEBRTC_NET_EQ (gst_webrtc_net_eq_get_type())
#define GST_WEBRTC_NET_EQ(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_WEBRTC_NET_EQ, GstWebrtcNetEq))

G_DEFINE_TYPE(GstWebrtcNetEq, gst_webrtc_net_eq, GST_TYPE_ELEMENT)

GST_DEBUG_CATEGORY_STATIC(gst_webrtc_net_eq_debug);
#define GST_CAT_DEFAULT gst_webrtc_net_eq_debug

namespace {

static GstDebugLevel gst_webrtc_net_eq_debug_level_for_webrtc(
    webrtc::LoggingSeverity severity) {
  switch (severity) {
    case webrtc::LS_ERROR:
      return GST_LEVEL_ERROR;
    case webrtc::LS_WARNING:
      return GST_LEVEL_WARNING;
    case webrtc::LS_INFO:
      return GST_LEVEL_INFO;
    case webrtc::LS_VERBOSE:
      return GST_LEVEL_LOG;
    case webrtc::LS_NONE:
      return GST_LEVEL_NONE;
  }
}

static void gst_webrtc_net_eq_trim_log_message(std::string* message) {
  while (!message->empty() &&
         (message->back() == '\n' || message->back() == '\r')) {
    message->pop_back();
  }
}

class GstWebrtcNetEqLogSink final : public webrtc::LogSink {
 public:
  void OnLogMessage(const webrtc::LogLineRef& line) override {
    std::string message(line.message());
    gst_webrtc_net_eq_trim_log_message(&message);

    std::string file(line.filename());
    gst_debug_log(gst_webrtc_net_eq_debug,
                  gst_webrtc_net_eq_debug_level_for_webrtc(line.severity()),
                  file.empty() ? "webrtc" : file.c_str(), "RTC_LOG",
                  line.line(), nullptr, "webrtc: %s", message.c_str());
  }

  void OnLogMessage(const std::string& message,
                    webrtc::LoggingSeverity severity) override {
    LogString(message, severity);
  }

  void OnLogMessage(const std::string& message) override {
    LogString(message, webrtc::LS_INFO);
  }

 private:
  static void LogString(const std::string& raw_message,
                        webrtc::LoggingSeverity severity) {
    std::string message(raw_message);
    gst_webrtc_net_eq_trim_log_message(&message);

    gst_debug_log(gst_webrtc_net_eq_debug,
                  gst_webrtc_net_eq_debug_level_for_webrtc(severity),
                  "webrtc", "RTC_LOG", 0, nullptr, "webrtc: %s",
                  message.c_str());
  }
};

static GOnce webrtc_log_sink_once = G_ONCE_INIT;

static gpointer gst_webrtc_net_eq_install_webrtc_log_sink_once(gpointer) {
  auto* sink = new GstWebrtcNetEqLogSink();
  webrtc::LogMessage::SetLogToStderr(false);
  webrtc::LogMessage::AddLogToStream(sink, webrtc::LS_INFO);
  return sink;
}

static void gst_webrtc_net_eq_install_webrtc_log_sink() {
  g_once(&webrtc_log_sink_once,
         gst_webrtc_net_eq_install_webrtc_log_sink_once, nullptr);
}

}  // namespace

enum {
  PROP_0,
  PROP_LATENCY_MS,
  PROP_MAX_LATENCY_MS,
  PROP_EOS_DRAIN_MS,
  PROP_NETWORK_STATS,
  PROP_LIFETIME_STATS,
};

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink",
                            GST_PAD_SINK,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("application/x-rtp, "
                                            "media = (string) audio, "
                                            "encoding-name = (string) OPUS, "
                                            "clock-rate = (int) [ 8000, 48000 ], "
                                            "encoding-params = (string) { 1, 2 }, "
                                            "payload = (int) [ 0, 127 ]"));

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src",
                            GST_PAD_SRC,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("audio/x-raw, "
                                            "format = (string) S16LE, "
                                            "layout = (string) interleaved, "
                                            "rate = (int) [ 8000, 48000 ], "
                                            "channels = (int) [ 1, 2 ]"));

static void gst_webrtc_net_eq_playout_task(gpointer data);

static gboolean gst_webrtc_net_eq_task_failed(GstWebrtcNetEq* self) {
  return self->state != nullptr &&
         gst_pad_get_task_state(self->srcpad) == GST_TASK_PAUSED;
}

static GstClockTime gst_webrtc_net_eq_add_time(GstClockTime time,
                                               GstClockTime delta) {
  if (time == GST_CLOCK_TIME_NONE) {
    return GST_CLOCK_TIME_NONE;
  }
  if (G_MAXUINT64 - time < delta) {
    return GST_CLOCK_TIME_NONE;
  }
  return time + delta;
}

static std::optional<GstClockTime> gst_webrtc_net_eq_buffer_timestamp(
    GstBuffer* buffer) {
  GstClockTime timestamp = GST_BUFFER_PTS(buffer);
  if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
    timestamp = GST_BUFFER_DTS(buffer);
  }
  if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
    return std::nullopt;
  }
  return timestamp;
}

static std::optional<GstClockTime> gst_webrtc_net_eq_output_base_pts_locked(
    GstWebrtcNetEq* self,
    GstBuffer* buffer) {
  const std::optional<GstClockTime> timestamp =
      gst_webrtc_net_eq_buffer_timestamp(buffer);
  if (timestamp.has_value()) {
    return timestamp;
  }
  if (!self->segment_received || self->segment.format != GST_FORMAT_TIME) {
    return std::nullopt;
  }
  if (GST_CLOCK_TIME_IS_VALID(self->segment.position)) {
    return self->segment.position;
  }
  if (GST_CLOCK_TIME_IS_VALID(self->segment.start)) {
    return self->segment.start;
  }
  return std::nullopt;
}

static std::optional<GstClockTime> gst_webrtc_net_eq_running_time(
    GstWebrtcNetEq* self) {
  GstClock* clock = gst_element_get_clock(GST_ELEMENT(self));
  if (clock == nullptr) {
    return std::nullopt;
  }

  const GstClockTime now = gst_clock_get_time(clock);
  const GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT(self));
  gst_object_unref(clock);

  if (!GST_CLOCK_TIME_IS_VALID(now) || !GST_CLOCK_TIME_IS_VALID(base_time) ||
      now < base_time) {
    return std::nullopt;
  }

  return now - base_time;
}

static GstClockID gst_webrtc_net_eq_new_periodic_clock_id(
    GstWebrtcNetEq* self,
    GstClockTime start_running_time,
    GstClockTime interval) {
  GstClock* clock = gst_element_get_clock(GST_ELEMENT(self));
  if (clock == nullptr) {
    GST_WARNING_OBJECT(self, "Cannot pace playout without a pipeline clock");
    return nullptr;
  }

  const GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT(self));
  if (!GST_CLOCK_TIME_IS_VALID(start_running_time) ||
      !GST_CLOCK_TIME_IS_VALID(interval) ||
      !GST_CLOCK_TIME_IS_VALID(base_time) ||
      G_MAXUINT64 - base_time < start_running_time) {
    gst_object_unref(clock);
    GST_WARNING_OBJECT(self, "Cannot convert running time to clock time");
    return nullptr;
  }

  GstClockID raw_clock_id = gst_clock_new_periodic_id(
      clock, base_time + start_running_time, interval);
  gst_object_unref(clock);
  if (raw_clock_id == nullptr) {
    GST_WARNING_OBJECT(self, "Cannot create periodic clock ID");
  }
  return raw_clock_id;
}

static ClockWaitResult gst_webrtc_net_eq_wait_on_clock_id(
    GstWebrtcNetEq* self,
    GstClockID clock_id,
    gboolean stop_on_stop) {
  {
    GMutexLock lock(&self->lock);
    if (stop_on_stop && self->playout.stop) {
      return ClockWaitResult::kStopped;
    }
  }

  const GstClockReturn result = gst_clock_id_wait(clock_id, nullptr);

  gboolean stopped = FALSE;
  {
    GMutexLock lock(&self->lock);
    stopped = self->playout.stop;
  }

  if (stop_on_stop && (stopped || result == GST_CLOCK_UNSCHEDULED)) {
    return ClockWaitResult::kStopped;
  }
  if (result == GST_CLOCK_OK || result == GST_CLOCK_EARLY ||
      result == GST_CLOCK_UNSCHEDULED) {
    return ClockWaitResult::kReady;
  }

  GST_WARNING_OBJECT(self, "Clock wait failed: %d", result);
  return ClockWaitResult::kError;
}

static GstClockID gst_webrtc_net_eq_start_periodic_wait_locked(
    GstWebrtcNetEq* self,
    GstClockTime start_running_time,
    GstClockTime interval) {
  GstClockID raw_clock_id = gst_webrtc_net_eq_new_periodic_clock_id(
      self, start_running_time, interval);
  if (raw_clock_id == nullptr) {
    return nullptr;
  }

  if (self->playout.stop && !self->playout.eos_received) {
    gst_clock_id_unref(raw_clock_id);
    return nullptr;
  }
  self->current_wait_clock_id = raw_clock_id;
  return raw_clock_id;
}

static GstClockID gst_webrtc_net_eq_start_periodic_wait_from_now(
    GstWebrtcNetEq* self,
    GstClockTime delay,
    GstClockTime interval) {
  const std::optional<GstClockTime> running_time =
      gst_webrtc_net_eq_running_time(self);
  if (!running_time.has_value() || G_MAXUINT64 - *running_time < delay) {
    GST_WARNING_OBJECT(self, "Cannot schedule periodic clock ID");
    return nullptr;
  }

  GMutexLock lock(&self->lock);
  return gst_webrtc_net_eq_start_periodic_wait_locked(
      self, *running_time + delay, interval);
}

static void gst_webrtc_net_eq_stop_task(GstWebrtcNetEq* self,
                                        gboolean drain_eos) {
  {
    GMutexLock lock(&self->lock);
    while (self->playout.joining) {
      g_cond_wait(&self->cond, lock.get());
    }
    if (gst_pad_get_task_state(self->srcpad) == GST_TASK_STOPPED) {
      return;
    }

    self->playout.joining = TRUE;
    self->playout.stop = TRUE;
    self->playout.eos_received = drain_eos;
    if (self->current_wait_clock_id != nullptr) {
      gst_clock_id_unschedule(self->current_wait_clock_id);
    }
    g_cond_broadcast(&self->cond);
  }

  gst_pad_stop_task(self->srcpad);

  GMutexLock lock(&self->lock);
  self->playout.joining = FALSE;
  self->playout.eos_received = FALSE;
  g_cond_broadcast(&self->cond);
}

static void gst_webrtc_net_eq_reset_neteq_state(GstWebrtcNetEq* self) {
  delete self->state;
  self->state = nullptr;

  self->output_base_pts_set = FALSE;
  self->output_base_pts = GST_CLOCK_TIME_NONE;
  self->current_wait_clock_id = nullptr;

  self->playout.stop = FALSE;
  self->playout.eos_received = FALSE;
  self->playout.joining = FALSE;
}

static void gst_webrtc_net_eq_reset_state(GstWebrtcNetEq* self,
                                          gboolean reset_configuration) {
  gst_webrtc_net_eq_reset_neteq_state(self);

  if (reset_configuration) {
    self->configured = FALSE;
    self->payload_type = -1;
    self->clock_rate_hz = 0;
    self->channels = 0;
  }
  gst_segment_init(&self->segment, GST_FORMAT_TIME);
  self->segment_received = FALSE;
}

static gboolean gst_webrtc_net_eq_start_task_locked(GstWebrtcNetEq* self) {
  const GstTaskState task_state = gst_pad_get_task_state(self->srcpad);
  if (task_state == GST_TASK_STARTED) {
    return TRUE;
  }
  if (task_state == GST_TASK_PAUSED) {
    return FALSE;
  }
  if (self->playout.stop) {
    return FALSE;
  }

  self->playout.eos_received = FALSE;
  self->playout.joining = FALSE;
  if (gst_pad_start_task(self->srcpad, gst_webrtc_net_eq_playout_task, self,
                         nullptr)) {
    return TRUE;
  }

  return FALSE;
}

static gboolean gst_webrtc_net_eq_ensure_neteq(GstWebrtcNetEq* self) {
  if (self->state != nullptr) {
    return TRUE;
  }
  if (self->channels != 1 && self->channels != 2) {
    GST_ERROR_OBJECT(self,
                     "Cannot create NetEQ before RTP caps configure channels");
    return FALSE;
  }
  if (self->clock_rate_hz <= 0) {
    GST_ERROR_OBJECT(self,
                     "Cannot create NetEQ before RTP caps configure clock-rate");
    return FALSE;
  }
  if (self->payload_type < 0 || self->payload_type > 127) {
    GST_ERROR_OBJECT(self,
                     "Cannot create NetEQ before RTP caps configure payload");
    return FALSE;
  }
  if (self->max_latency_ms != 0 &&
      self->max_latency_ms < self->latency_ms) {
    GST_ERROR_OBJECT(self,
                     "NetEQ max latency %d ms is lower than min latency %d ms",
                     self->max_latency_ms, self->latency_ms);
    return FALSE;
  }

  auto state = std::make_unique<NetEqState>(webrtc::CreateEnvironment());

  webrtc::NetEq::Config config;
  config.sample_rate_hz = self->clock_rate_hz;
  config.max_delay_ms = self->max_latency_ms;
  config.min_delay_ms = self->latency_ms;
  config.enable_fast_accelerate = true;
  config.enable_muted_state = true;

  state->neteq = webrtc::DefaultNetEqFactory().Create(
      state->env, config, webrtc::CreateOpusAudioDecoderFactory());
  if (!state->neteq) {
    GST_ERROR_OBJECT(self, "Failed to create WebRTC NetEQ");
    return FALSE;
  }

  webrtc::CodecParameterMap parameters;
  if (self->channels == 2) {
    parameters.emplace("stereo", "1");
  }
  const webrtc::SdpAudioFormat opus_format("opus", self->clock_rate_hz,
                                           static_cast<size_t>(self->channels),
                                           std::move(parameters));
  if (!state->neteq->RegisterPayloadType(self->payload_type, opus_format)) {
    GST_ERROR_OBJECT(self, "Failed to register Opus payload type %d",
                     self->payload_type);
    return FALSE;
  }
  if (!state->neteq->SetMinimumDelay(self->latency_ms)) {
    GST_ERROR_OBJECT(self, "Failed to set NetEQ minimum delay %d ms",
                     self->latency_ms);
    return FALSE;
  }
  if (!state->neteq->SetMaximumDelay(self->max_latency_ms)) {
    GST_ERROR_OBJECT(self, "Failed to set NetEQ maximum delay %d ms",
                     self->max_latency_ms);
    return FALSE;
  }
  state->neteq->CreateDecoder(self->payload_type);

  self->state = state.release();
  GST_INFO_OBJECT(self,
                  "Created NetEQ for Opus payload type %d, channels %d, "
                  "clock rate %d Hz, latency %d..%d ms",
                  self->payload_type, self->channels, self->clock_rate_hz,
                  self->latency_ms, self->max_latency_ms);
  return TRUE;
}

static gboolean gst_webrtc_net_eq_parse_caps(GstWebrtcNetEq* self,
                                             GstCaps* caps) {
  if (caps == nullptr || gst_caps_is_empty(caps)) {
    return FALSE;
  }

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* encoding_name =
      gst_structure_get_string(structure, "encoding-name");
  if (encoding_name == nullptr ||
      g_ascii_strcasecmp(encoding_name, "OPUS") != 0) {
    GST_WARNING_OBJECT(self, "RTP caps must use OPUS encoding-name");
    return FALSE;
  }
  gint clock_rate = 0;
  if (!gst_structure_get_int(structure, "clock-rate", &clock_rate) ||
      clock_rate < kMinClockRateHz || clock_rate > kMaxClockRateHz) {
    GST_WARNING_OBJECT(self,
                       "RTP Opus caps must include clock-rate in [%d, %d]",
                       kMinClockRateHz, kMaxClockRateHz);
    return FALSE;
  }

  gint payload = 0;
  if (!gst_structure_get_int(structure, "payload", &payload) || payload < 0 ||
      payload > 127) {
    GST_WARNING_OBJECT(self, "RTP Opus caps must include payload in [0, 127]");
    return FALSE;
  }

  gint channels = 0;
  gint int_encoding_params = 0;
  if (gst_structure_get_int(structure, "encoding-params",
                            &int_encoding_params)) {
    channels = int_encoding_params;
  } else {
    const gchar* encoding_params =
        gst_structure_get_string(structure, "encoding-params");
    if (encoding_params != nullptr) {
      gchar* end = nullptr;
      const gint64 parsed = g_ascii_strtoll(encoding_params, &end, 10);
      if (end != encoding_params && *end == '\0') {
        channels = static_cast<gint>(parsed);
      }
    }
  }

  if (channels != 1 && channels != 2) {
    GST_WARNING_OBJECT(self,
                       "RTP Opus caps must include encoding-params 1 or 2");
    return FALSE;
  }

  self->payload_type = payload;
  self->clock_rate_hz = clock_rate;
  self->channels = channels;
  self->configured = TRUE;

  GST_INFO_OBJECT(self,
                  "Configured from caps: payload type %d, clock rate %d Hz, "
                  "channels %d",
                  self->payload_type, self->clock_rate_hz, self->channels);
  return TRUE;
}

static gboolean gst_webrtc_net_eq_push_src_caps(GstWebrtcNetEq* self,
                                                int channels,
                                                int clock_rate_hz) {
  GstCaps* caps = gst_caps_new_simple(
      "audio/x-raw", "format", G_TYPE_STRING, "S16LE", "layout", G_TYPE_STRING,
      "interleaved", "rate", G_TYPE_INT, clock_rate_hz, "channels",
      G_TYPE_INT, channels, nullptr);
  const gboolean pushed =
      gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  return pushed;
}

static double gst_webrtc_net_eq_q14_percent(uint16_t value) {
  return static_cast<double>(value) * 100.0 / 16384.0;
}

static gchar* gst_webrtc_net_eq_network_stats_string_locked(
    GstWebrtcNetEq* self) {
  if (self->state == nullptr || self->state->neteq == nullptr) {
    return g_strdup("uninitialized");
  }

  const webrtc::NetEqNetworkStatistics stats =
      self->state->neteq->CurrentNetworkStatistics();
  return g_strdup_printf(
      "buf=%ums pref=%ums peak=%u expand=%.1f%% speech=%.1f%% "
      "pre=%.1f%% acc=%.1f%%",
      stats.current_buffer_size_ms, stats.preferred_buffer_size_ms,
      stats.jitter_peaks_found,
      gst_webrtc_net_eq_q14_percent(stats.expand_rate),
      gst_webrtc_net_eq_q14_percent(stats.speech_expand_rate),
      gst_webrtc_net_eq_q14_percent(stats.preemptive_rate),
      gst_webrtc_net_eq_q14_percent(stats.accelerate_rate));
}

static gchar* gst_webrtc_net_eq_lifetime_stats_string_locked(
    GstWebrtcNetEq* self) {
  if (self->state == nullptr || self->state->neteq == nullptr) {
    return g_strdup("uninitialized");
  }

  const webrtc::NetEqLifetimeStatistics stats =
      self->state->neteq->GetLifetimeStatistics();
  return g_strdup_printf(
      "rx=%" G_GUINT64_FORMAT " conceal=%" G_GUINT64_FORMAT
      " ev=%" G_GUINT64_FORMAT " drop=%" G_GUINT64_FORMAT
      " jb=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
      "ms int=%d/%dms",
      static_cast<guint64>(stats.total_samples_received),
      static_cast<guint64>(stats.concealed_samples),
      static_cast<guint64>(stats.concealment_events),
      static_cast<guint64>(stats.packets_discarded),
      static_cast<guint64>(stats.jitter_buffer_delay_ms),
      static_cast<guint64>(stats.jitter_buffer_emitted_count),
      stats.interruption_count, stats.total_interruption_duration_ms);
}

static GstFlowReturn gst_webrtc_net_eq_create_audio_locked(
    GstWebrtcNetEq* self,
    webrtc::AudioFrame* frame,
    GstClockTime output_base_pts,
    guint64* output_samples,
    GstBuffer** out_buffer) {
  bool muted = false;
  int decoded_rate_hz = 0;
  const int result = self->state->neteq->GetAudio(frame, &muted,
                                                  &decoded_rate_hz);
  if (result != webrtc::NetEq::kOK) {
    GST_WARNING_OBJECT(self, "NetEQ GetAudio failed");
    return GST_FLOW_ERROR;
  }

  const int rate =
      frame->sample_rate_hz() > 0 ? frame->sample_rate_hz() : decoded_rate_hz;
  if (rate != self->clock_rate_hz) {
    GST_WARNING_OBJECT(self,
                       "NetEQ output rate %d Hz does not match configured "
                       "clock rate %d Hz",
                       rate, self->clock_rate_hz);
    return GST_FLOW_ERROR;
  }

  const size_t channels = frame->num_channels();
  if (channels != static_cast<size_t>(self->channels)) {
    GST_WARNING_OBJECT(self,
                       "NetEQ output channel count %zu does not match "
                       "configured channel count %d",
                       channels, self->channels);
    return GST_FLOW_NOT_NEGOTIATED;
  }
  const size_t samples_per_channel = frame->samples_per_channel();
  const size_t sample_count = samples_per_channel * channels;
  const size_t output_size = sample_count * sizeof(int16_t);

  GstBufferPtr output(gst_webrtc_net_eq_new_output_buffer(output_size));
  if (!output) {
    return GST_FLOW_ERROR;
  }

  GstMapInfo map;
  if (!gst_buffer_map(output.get(), &map, GST_MAP_WRITE)) {
    return GST_FLOW_ERROR;
  }

  if (muted) {
    std::memset(map.data, 0, output_size);
  } else {
    std::memcpy(map.data, frame->data(), output_size);
  }
  gst_buffer_unmap(output.get(), &map);

  GstAudioInfo audio_info;
  gst_audio_info_set_format(&audio_info, GST_AUDIO_FORMAT_S16LE,
                            self->clock_rate_hz, self->channels, nullptr);
  if (gst_buffer_add_audio_meta(output.get(), &audio_info, samples_per_channel,
                                nullptr) == nullptr) {
    GST_WARNING_OBJECT(self, "Failed to add audio metadata");
    return GST_FLOW_ERROR;
  }

  GST_BUFFER_PTS(output.get()) =
      output_base_pts +
      gst_util_uint64_scale(*output_samples, GST_SECOND,
                            self->clock_rate_hz);
  GST_BUFFER_DURATION(output.get()) =
      gst_util_uint64_scale(samples_per_channel, GST_SECOND,
                            self->clock_rate_hz);
  *output_samples += samples_per_channel;

  *out_buffer = output.release();
  return GST_FLOW_OK;
}

static gboolean gst_webrtc_net_eq_push_buffer(GstWebrtcNetEq* self,
                                              GstBuffer* buffer) {
  const GstFlowReturn flow = gst_pad_push(self->srcpad, buffer);
  if (flow == GST_FLOW_OK) {
    return TRUE;
  }

  GST_DEBUG_OBJECT(self, "Stopping playout after downstream flow %s",
                   gst_flow_get_name(flow));
  return FALSE;
}

static gboolean gst_webrtc_net_eq_pull_locked(GstWebrtcNetEq* self,
                                              webrtc::AudioFrame* frame,
                                              GstClockTime output_base_pts,
                                              guint64* output_samples,
                                              GstBuffer** out_buffer) {
  *out_buffer = nullptr;

  const GstFlowReturn flow = gst_webrtc_net_eq_create_audio_locked(
      self, frame, output_base_pts, output_samples, out_buffer);
  if (flow != GST_FLOW_OK) {
    return FALSE;
  }

  return TRUE;
}

static void gst_webrtc_net_eq_playout_loop(GstWebrtcNetEq* self) {
  GstClockTime output_base_pts;
  {
    GMutexLock lock(&self->lock);
    if (!self->output_base_pts_set) {
      GST_ERROR_OBJECT(self, "Cannot start playout without output base PTS");
      return;
    }
    output_base_pts = self->output_base_pts;
  }
  guint64 output_samples = 0;
  std::optional<GstClockTime> running_time =
      gst_webrtc_net_eq_running_time(self);
  if (!running_time.has_value()) {
    GST_ERROR_OBJECT(self, "Cannot start playout without a pipeline clock");
    return;
  }
  const GstClockTime output_frame_duration = kOutputFrameMs * GST_MSECOND;
  webrtc::AudioFrame frame;

  GstClockID raw_clock_id = nullptr;
  {
    GMutexLock lock(&self->lock);
    raw_clock_id = gst_webrtc_net_eq_start_periodic_wait_locked(
        self, *running_time, output_frame_duration);
    if (raw_clock_id == nullptr) {
      return;
    }
  }

  {
    PlayoutClockIdScope clock_id(self, raw_clock_id);
    while (true) {
      const ClockWaitResult wait_result =
          gst_webrtc_net_eq_wait_on_clock_id(self, clock_id.get(), TRUE);
      if (wait_result == ClockWaitResult::kStopped) {
        break;
      }
      if (wait_result == ClockWaitResult::kError) {
        return;
      }

      GstBuffer* raw_buffer = nullptr;

      {
        GMutexLock lock(&self->lock);
        if (self->playout.stop) {
          break;
        }
        if (!gst_webrtc_net_eq_pull_locked(self, &frame, output_base_pts,
                                           &output_samples, &raw_buffer)) {
          return;
        }
      }

      if (!gst_webrtc_net_eq_push_buffer(self, raw_buffer)) {
        return;
      }
    }
  }

  guint eos_drain_pulls;
  {
    GMutexLock lock(&self->lock);
    if (!self->playout.eos_received) {
      return;
    }
    const int buffered_ms =
        self->state != nullptr
            ? self->state->neteq->CurrentNetworkStatistics()
                  .current_buffer_size_ms
            : 0;
    const int drain_ms = std::min(buffered_ms, self->eos_drain_ms);
    eos_drain_pulls =
        static_cast<guint>((drain_ms + kOutputFrameMs - 1) / kOutputFrameMs);
    GST_DEBUG_OBJECT(self,
                     "Draining %d ms of buffered NetEQ audio at EOS, capped "
                     "by eos-drain-ms=%d",
                     drain_ms, self->eos_drain_ms);
  }
  if (eos_drain_pulls == 0) {
    return;
  }

  raw_clock_id = gst_webrtc_net_eq_start_periodic_wait_from_now(
      self, output_frame_duration, output_frame_duration);
  if (raw_clock_id == nullptr) {
    return;
  }
  PlayoutClockIdScope drain_clock_id(self, raw_clock_id);

  for (guint i = 0; i < eos_drain_pulls; ++i) {
    const ClockWaitResult wait_result =
        gst_webrtc_net_eq_wait_on_clock_id(self, drain_clock_id.get(), FALSE);
    if (wait_result == ClockWaitResult::kError) {
      return;
    }

    GstBuffer* raw_buffer = nullptr;

    {
      GMutexLock lock(&self->lock);
      if (!gst_webrtc_net_eq_pull_locked(self, &frame, output_base_pts,
                                         &output_samples, &raw_buffer)) {
        return;
      }
    }

    if (!gst_webrtc_net_eq_push_buffer(self, raw_buffer)) {
      return;
    }
  }
}

static void gst_webrtc_net_eq_playout_task(gpointer data) {
  auto* self = GST_WEBRTC_NET_EQ(data);
  gst_webrtc_net_eq_playout_loop(self);

  {
    GMutexLock lock(&self->lock);
    g_cond_broadcast(&self->cond);
  }
  gst_pad_pause_task(self->srcpad);
}

static GstFlowReturn gst_webrtc_net_eq_chain(GstPad* pad,
                                             GstObject* parent,
                                             GstBuffer* buffer) {
  auto* self = GST_WEBRTC_NET_EQ(parent);
  GstBufferPtr input(buffer);

  GMutexLock lock(&self->lock);
  if (self->playout.stop) {
    GST_WARNING_OBJECT(self, "Rejecting RTP buffer after playout stopped");
    return GST_FLOW_ERROR;
  }
  if (gst_webrtc_net_eq_task_failed(self)) {
    GST_WARNING_OBJECT(self, "Rejecting RTP buffer after playout task stopped");
    return GST_FLOW_ERROR;
  }

  if (!self->configured) {
    GST_WARNING_OBJECT(self, "Rejecting RTP buffer before CAPS configuration");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (!gst_webrtc_net_eq_ensure_neteq(self)) {
    return GST_FLOW_ERROR;
  }

  if (!self->output_base_pts_set) {
    const std::optional<GstClockTime> output_base_pts =
        gst_webrtc_net_eq_output_base_pts_locked(self, input.get());
    if (!output_base_pts.has_value()) {
      GST_WARNING_OBJECT(self,
                         "Cannot determine output base PTS from buffer or "
                         "TIME segment");
      return GST_FLOW_ERROR;
    }
    self->output_base_pts = *output_base_pts;
    self->output_base_pts_set = TRUE;
  }

  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(input.get(), GST_MAP_READ, &rtp)) {
    GST_WARNING_OBJECT(self, "Failed to map RTP buffer");
    return GST_FLOW_ERROR;
  }

  webrtc::RTPHeader header;
  header.markerBit = gst_rtp_buffer_get_marker(&rtp);
  header.payloadType = gst_rtp_buffer_get_payload_type(&rtp);
  header.sequenceNumber = gst_rtp_buffer_get_seq(&rtp);
  header.timestamp = gst_rtp_buffer_get_timestamp(&rtp);
  header.ssrc = gst_rtp_buffer_get_ssrc(&rtp);
  header.numCSRCs = static_cast<uint8_t>(
      std::min<int>(gst_rtp_buffer_get_csrc_count(&rtp), webrtc::kRtpCsrcSize));
  for (uint8_t i = 0; i < header.numCSRCs; ++i) {
    header.arrOfCSRCs[i] = gst_rtp_buffer_get_csrc(&rtp, i);
  }
  header.paddingLength = 0;
  header.headerLength = 0;

  if (header.payloadType != self->payload_type) {
    GST_DEBUG_OBJECT(self,
                     "Dropping RTP packet with payload type %u, expected %d "
                     "seq=%u ts=%u ssrc=%u",
                     header.payloadType, self->payload_type,
                     header.sequenceNumber, header.timestamp, header.ssrc);
    gst_rtp_buffer_unmap(&rtp);
    return GST_FLOW_OK;
  }

  const auto* payload =
      static_cast<const uint8_t*>(gst_rtp_buffer_get_payload(&rtp));
  const guint payload_len = gst_rtp_buffer_get_payload_len(&rtp);
  const webrtc::Timestamp receive_time = self->state->env.clock().CurrentTime();
  const int insert_result = self->state->neteq->InsertPacket(
      header, std::span<const uint8_t>(payload, payload_len), receive_time);
  gst_rtp_buffer_unmap(&rtp);

  if (insert_result != webrtc::NetEq::kOK) {
    GST_WARNING_OBJECT(self,
                       "NetEQ InsertPacket failed for seq=%u ts=%u ssrc=%u",
                       header.sequenceNumber, header.timestamp, header.ssrc);
  }

  if (!gst_webrtc_net_eq_start_task_locked(self)) {
    GST_WARNING_OBJECT(self, "Failed to start NetEQ playout task");
    g_cond_broadcast(&self->cond);
    return GST_FLOW_ERROR;
  }

  g_cond_broadcast(&self->cond);
  return GST_FLOW_OK;
}

static gboolean gst_webrtc_net_eq_src_query(GstPad* pad,
                                            GstObject* parent,
                                            GstQuery* query) {
  auto* self = GST_WEBRTC_NET_EQ(parent);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_LATENCY: {
      gboolean live = FALSE;
      GstClockTime min_latency = 0;
      GstClockTime max_latency = GST_CLOCK_TIME_NONE;

      if (gst_pad_peer_query(self->sinkpad, query)) {
        gst_query_parse_latency(query, &live, &min_latency, &max_latency);
      } else {
        GST_DEBUG_OBJECT(self,
                         "Upstream latency query failed; reporting local "
                         "NetEQ latency only");
      }

      gint latency_ms;
      gint max_latency_ms;
      {
        GMutexLock lock(&self->lock);
        latency_ms = self->latency_ms;
        max_latency_ms = self->max_latency_ms;
      }

      const GstClockTime neteq_latency =
          static_cast<GstClockTime>(latency_ms) * GST_MSECOND;
      const GstClockTime neteq_max_latency =
          max_latency_ms == 0
              ? GST_CLOCK_TIME_NONE
              : static_cast<GstClockTime>(max_latency_ms) * GST_MSECOND;
      min_latency = gst_webrtc_net_eq_add_time(min_latency, neteq_latency);
      max_latency = gst_webrtc_net_eq_add_time(max_latency, neteq_max_latency);

      gst_query_set_latency(query, TRUE, min_latency, max_latency);
      GST_DEBUG_OBJECT(
          self,
          "Latency query: upstream_live=%d, neteq_latency=%" GST_TIME_FORMAT
          ", neteq_max_latency=%" GST_TIME_FORMAT ", min=%" GST_TIME_FORMAT
          ", max=%" GST_TIME_FORMAT,
          live, GST_TIME_ARGS(neteq_latency), GST_TIME_ARGS(neteq_max_latency),
          GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency));
      return TRUE;
    }

    default:
      return gst_pad_query_default(pad, parent, query);
  }
}

static gboolean gst_webrtc_net_eq_sink_event(GstPad* pad,
                                             GstObject* parent,
                                             GstEvent* event) {
  auto* self = GST_WEBRTC_NET_EQ(parent);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_STREAM_START:
      return gst_pad_push_event(self->srcpad, event);

    case GST_EVENT_CAPS: {
      GstCaps* caps = nullptr;
      gst_event_parse_caps(event, &caps);
      gst_webrtc_net_eq_stop_task(self, FALSE);
      gint channels;
      gint clock_rate_hz;
      {
        GMutexLock lock(&self->lock);
        gst_webrtc_net_eq_reset_state(self, TRUE);
        if (!gst_webrtc_net_eq_parse_caps(self, caps)) {
          gst_event_unref(event);
          return FALSE;
        }
        channels = self->channels;
        clock_rate_hz = self->clock_rate_hz;
      }
      gst_event_unref(event);
      return gst_webrtc_net_eq_push_src_caps(self, channels, clock_rate_hz);
    }

    case GST_EVENT_SEGMENT: {
      GstSegment segment;
      gst_event_copy_segment(event, &segment);
      {
        GMutexLock lock(&self->lock);
        if (segment.format == GST_FORMAT_TIME) {
          self->segment = segment;
          self->segment_received = TRUE;
          self->output_base_pts_set = FALSE;
          self->output_base_pts = GST_CLOCK_TIME_NONE;
        } else {
          GST_WARNING_OBJECT(self, "Ignoring non-TIME segment");
          self->segment_received = FALSE;
        }
      }
      return gst_pad_push_event(self->srcpad, event);
    }

    case GST_EVENT_FLUSH_START: {
      const gboolean pushed = gst_pad_push_event(self->srcpad, event);
      gst_webrtc_net_eq_stop_task(self, FALSE);
      return pushed;
    }

    case GST_EVENT_FLUSH_STOP:
      gst_webrtc_net_eq_stop_task(self, FALSE);
      {
        GMutexLock lock(&self->lock);
        gst_webrtc_net_eq_reset_state(self, FALSE);
      }
      return gst_pad_push_event(self->srcpad, event);

    case GST_EVENT_EOS: {
      gst_webrtc_net_eq_stop_task(self, TRUE);
      gst_event_unref(event);
      return gst_pad_push_event(self->srcpad, gst_event_new_eos());
    }

    default:
      return gst_pad_event_default(pad, parent, event);
  }
}

static void gst_webrtc_net_eq_set_property(GObject* object,
                                           guint prop_id,
                                           const GValue* value,
                                           GParamSpec* pspec) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  switch (prop_id) {
    case PROP_LATENCY_MS: {
      gboolean changed = FALSE;
      {
        GMutexLock lock(&self->lock);
        const gint latency_ms = g_value_get_int(value);
        if (self->max_latency_ms != 0 && latency_ms > self->max_latency_ms) {
          GST_WARNING_OBJECT(
              self,
              "Ignoring latency-ms %d because it exceeds max-latency-ms %d",
              latency_ms, self->max_latency_ms);
          return;
        }
        if (self->state != nullptr &&
            !self->state->neteq->SetMinimumDelay(latency_ms)) {
          GST_WARNING_OBJECT(self, "Failed to set NetEQ minimum delay %d ms",
                             latency_ms);
          return;
        }
        changed = self->latency_ms != latency_ms;
        self->latency_ms = latency_ms;
      }
      if (changed) {
        gst_element_post_message(GST_ELEMENT(self),
                                 gst_message_new_latency(GST_OBJECT(self)));
      }
      return;
    }
    case PROP_MAX_LATENCY_MS: {
      gboolean changed = FALSE;
      {
        GMutexLock lock(&self->lock);
        const gint max_latency_ms = g_value_get_int(value);
        if (max_latency_ms != 0 && max_latency_ms < self->latency_ms) {
          GST_WARNING_OBJECT(
              self,
              "Ignoring max-latency-ms %d because it is lower than "
              "latency-ms %d",
              max_latency_ms, self->latency_ms);
          return;
        }
        if (self->state != nullptr &&
            !self->state->neteq->SetMaximumDelay(max_latency_ms)) {
          GST_WARNING_OBJECT(self, "Failed to set NetEQ maximum delay %d ms",
                             max_latency_ms);
          return;
        }
        changed = self->max_latency_ms != max_latency_ms;
        self->max_latency_ms = max_latency_ms;
      }
      if (changed) {
        gst_element_post_message(GST_ELEMENT(self),
                                 gst_message_new_latency(GST_OBJECT(self)));
      }
      return;
    }
    case PROP_EOS_DRAIN_MS: {
      GMutexLock lock(&self->lock);
      self->eos_drain_ms = g_value_get_int(value);
      return;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      return;
  }
}

static void gst_webrtc_net_eq_get_property(GObject* object,
                                           guint prop_id,
                                           GValue* value,
                                           GParamSpec* pspec) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  GMutexLock lock(&self->lock);
  switch (prop_id) {
    case PROP_LATENCY_MS:
      g_value_set_int(value, self->latency_ms);
      return;
    case PROP_MAX_LATENCY_MS:
      g_value_set_int(value, self->max_latency_ms);
      return;
    case PROP_EOS_DRAIN_MS:
      g_value_set_int(value, self->eos_drain_ms);
      return;
    case PROP_NETWORK_STATS:
      g_value_take_string(
          value, gst_webrtc_net_eq_network_stats_string_locked(self));
      return;
    case PROP_LIFETIME_STATS:
      g_value_take_string(
          value, gst_webrtc_net_eq_lifetime_stats_string_locked(self));
      return;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      return;
  }
}

static void gst_webrtc_net_eq_dispose(GObject* object) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  gst_webrtc_net_eq_stop_task(self, FALSE);

  G_OBJECT_CLASS(gst_webrtc_net_eq_parent_class)->dispose(object);
}

static void gst_webrtc_net_eq_finalize(GObject* object) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  {
    GMutexLock lock(&self->lock);
    delete self->state;
    self->state = nullptr;
  }
  g_cond_clear(&self->cond);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gst_webrtc_net_eq_parent_class)->finalize(object);
}

static GstStateChangeReturn gst_webrtc_net_eq_change_state(
    GstElement* element,
    GstStateChange transition) {
  auto* self = GST_WEBRTC_NET_EQ(element);

  if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED) {
    gst_webrtc_net_eq_stop_task(self, FALSE);
    {
      GMutexLock lock(&self->lock);
      self->playout.stop = FALSE;
      g_cond_broadcast(&self->cond);
    }
  }

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    gst_webrtc_net_eq_stop_task(self, FALSE);
    {
      GMutexLock lock(&self->lock);
      gst_webrtc_net_eq_reset_state(self, TRUE);
    }
  }

  return GST_ELEMENT_CLASS(gst_webrtc_net_eq_parent_class)
      ->change_state(element, transition);
}

static void gst_webrtc_net_eq_class_init(GstWebrtcNetEqClass* klass) {
  auto* gobject_class = G_OBJECT_CLASS(klass);
  auto* element_class = GST_ELEMENT_CLASS(klass);

  GST_DEBUG_CATEGORY_INIT(gst_webrtc_net_eq_debug, "webrtcneteq", 0,
                          "WebRTC NetEQ GStreamer element");
  gst_webrtc_net_eq_install_webrtc_log_sink();

  gobject_class->set_property = gst_webrtc_net_eq_set_property;
  gobject_class->get_property = gst_webrtc_net_eq_get_property;
  gobject_class->dispose = gst_webrtc_net_eq_dispose;
  gobject_class->finalize = gst_webrtc_net_eq_finalize;
  element_class->change_state = gst_webrtc_net_eq_change_state;

  g_object_class_install_property(
      gobject_class, PROP_LATENCY_MS,
      g_param_spec_int("latency-ms", "Latency",
                       "NetEQ minimum playout latency in milliseconds", 0,
                       kMaxNetEqDelayMs, kDefaultLatencyMs,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                GST_PARAM_MUTABLE_PLAYING |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_MAX_LATENCY_MS,
      g_param_spec_int("max-latency-ms", "Max latency",
                       "NetEQ maximum playout latency in milliseconds; 0 "
                       "leaves it unconstrained",
                       0, kMaxNetEqDelayMs, kDefaultMaxLatencyMs,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                GST_PARAM_MUTABLE_PLAYING |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_EOS_DRAIN_MS,
      g_param_spec_int("eos-drain-ms", "EOS drain",
                       "Maximum NetEQ buffered audio to emit after EOS",
                       0, 30000, kDefaultEosDrainMs,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_NETWORK_STATS,
      g_param_spec_string("network-stats", "Network stats",
                          "Human-readable current NetEQ network statistics",
                          nullptr,
                          static_cast<GParamFlags>(G_PARAM_READABLE |
                                                   G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_LIFETIME_STATS,
      g_param_spec_string("lifetime-stats", "Lifetime stats",
                          "Human-readable cumulative NetEQ lifetime "
                          "statistics",
                          nullptr,
                          static_cast<GParamFlags>(G_PARAM_READABLE |
                                                   G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata(
      element_class, "WebRTC NetEQ RTP Opus decoder", "Codec/Decoder/Audio",
      "Decodes RTP/Opus through libwebrtc NetEQ", "Recall.ai");
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);
}

static void gst_webrtc_net_eq_init(GstWebrtcNetEq* self) {
  self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
  gst_pad_set_chain_function(self->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_webrtc_net_eq_chain));
  gst_pad_set_event_function(self->sinkpad,
                             GST_DEBUG_FUNCPTR(gst_webrtc_net_eq_sink_event));
  gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
  gst_pad_set_query_function(self->srcpad,
                             GST_DEBUG_FUNCPTR(gst_webrtc_net_eq_src_query));
  gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

  g_mutex_init(&self->lock);
  g_cond_init(&self->cond);
  self->current_wait_clock_id = nullptr;
  self->state = nullptr;
  self->payload_type = -1;
  self->clock_rate_hz = 0;
  self->latency_ms = kDefaultLatencyMs;
  self->max_latency_ms = kDefaultMaxLatencyMs;
  self->channels = 0;
  self->eos_drain_ms = kDefaultEosDrainMs;
  self->configured = FALSE;
  gst_segment_init(&self->segment, GST_FORMAT_TIME);
  self->segment_received = FALSE;
  self->output_base_pts_set = FALSE;
  self->output_base_pts = GST_CLOCK_TIME_NONE;
  self->playout.stop = FALSE;
  self->playout.eos_received = FALSE;
  self->playout.joining = FALSE;
}

static gboolean plugin_init(GstPlugin* plugin) {
  return gst_element_register(plugin, "webrtcneteq", GST_RANK_NONE,
                              GST_TYPE_WEBRTC_NET_EQ);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  webrtcneteq,
                  "WebRTC NetEQ RTP Opus decoder",
                  plugin_init,
                  "0.1.0",
                  "BSD",
                  "webrtcneteq",
                  "https://webrtc.org/")
