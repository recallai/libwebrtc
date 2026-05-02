/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/rtp/rtp.h>

#include "api/audio/audio_frame.h"
#include "api/audio_codecs/audio_format.h"
#include "api/audio_codecs/opus_audio_decoder_factory.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/neteq/default_neteq_factory.h"
#include "api/neteq/neteq.h"
#include "api/rtp_headers.h"
#include "api/units/timestamp.h"

#ifndef PACKAGE
#define PACKAGE "gstwebrtcneteq"
#endif

namespace {

constexpr int kDefaultPayloadType = 111;
constexpr int kDefaultLatencyMs = 200;
constexpr int kDefaultChannels = 2;
constexpr int kDefaultEosDrainMs = 1000;
constexpr int kOutputRateHz = 48000;
constexpr int kOutputFrameMs = 10;

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

}  // namespace

typedef struct _GstWebrtcNetEq GstWebrtcNetEq;
typedef struct _GstWebrtcNetEqClass GstWebrtcNetEqClass;

struct _GstWebrtcNetEq {
  GstElement parent;

  GstPad* sinkpad;
  GstPad* srcpad;

  GMutex lock;
  GCond cond;
  GThread* playout_thread;
  NetEqState* state;

  gint payload_type;
  gint latency_ms;
  gint channels;
  gint eos_drain_ms;

  GstSegment segment;
  gboolean segment_received;
  gboolean output_base_pts_set;
  GstClockTime output_base_pts;

  PlayoutState playout;
};

struct _GstWebrtcNetEqClass {
  GstElementClass parent_class;
};

#define GST_TYPE_WEBRTC_NET_EQ (gst_webrtc_net_eq_get_type())
#define GST_WEBRTC_NET_EQ(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_WEBRTC_NET_EQ, GstWebrtcNetEq))

G_DEFINE_TYPE(GstWebrtcNetEq, gst_webrtc_net_eq, GST_TYPE_ELEMENT)

GST_DEBUG_CATEGORY_STATIC(gst_webrtc_net_eq_debug);
#define GST_CAT_DEFAULT gst_webrtc_net_eq_debug

enum {
  PROP_0,
  PROP_PAYLOAD_TYPE,
  PROP_LATENCY_MS,
  PROP_CHANNELS,
  PROP_EOS_DRAIN_MS,
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("application/x-rtp, "
                    "media = (string) audio, "
                    "encoding-name = (string) OPUS, "
                    "clock-rate = (int) 48000, "
                    "payload = (int) [ 0, 127 ]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
                    "format = (string) S16LE, "
                    "layout = (string) interleaved, "
                    "rate = (int) 48000, "
                    "channels = (int) [ 1, 2 ]"));

static gpointer gst_webrtc_net_eq_playout_thread(gpointer data);

static GstClockTime gst_webrtc_net_eq_add_latency(GstClockTime latency,
                                                  GstClockTime extra_latency) {
  if (latency == GST_CLOCK_TIME_NONE) {
    return GST_CLOCK_TIME_NONE;
  }
  if (G_MAXUINT64 - latency < extra_latency) {
    return GST_CLOCK_TIME_NONE;
  }
  return latency + extra_latency;
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

static std::optional<GstClockTime>
gst_webrtc_net_eq_segment_position_locked(GstWebrtcNetEq* self) {
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

static std::optional<GstClockTime> gst_webrtc_net_eq_output_base_pts_locked(
    GstWebrtcNetEq* self,
    GstBuffer* buffer) {
  const std::optional<GstClockTime> timestamp =
      gst_webrtc_net_eq_buffer_timestamp(buffer);
  if (timestamp.has_value()) {
    return timestamp;
  }
  return gst_webrtc_net_eq_segment_position_locked(self);
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

  if (!GST_CLOCK_TIME_IS_VALID(now) ||
      !GST_CLOCK_TIME_IS_VALID(base_time) ||
      now < base_time) {
    return std::nullopt;
  }

  return now - base_time;
}

static void gst_webrtc_net_eq_stop_task(GstWebrtcNetEq* self,
                                        gboolean drain_eos) {
  GThread* thread = nullptr;

  {
    GMutexLock lock(&self->lock);
    while (self->playout.joining) {
      g_cond_wait(&self->cond, lock.get());
    }
    if (self->playout_thread == nullptr) {
      return;
    }

    self->playout.joining = TRUE;
    self->playout.stop = TRUE;
    self->playout.eos_received = drain_eos;
    thread = self->playout_thread;
    self->playout_thread = nullptr;
    g_cond_broadcast(&self->cond);
  }

  g_thread_join(thread);

  GMutexLock lock(&self->lock);
  self->playout.joining = FALSE;
  self->playout.eos_received = FALSE;
  g_cond_broadcast(&self->cond);
}

static void gst_webrtc_net_eq_reset_state(GstWebrtcNetEq* self) {
  delete self->state;
  self->state = nullptr;

  gst_segment_init(&self->segment, GST_FORMAT_TIME);
  self->segment_received = FALSE;
  self->output_base_pts_set = FALSE;
  self->output_base_pts = GST_CLOCK_TIME_NONE;

  self->playout.stop = FALSE;
  self->playout.eos_received = FALSE;
  self->playout.joining = FALSE;
}

static gboolean gst_webrtc_net_eq_start_task_locked(GstWebrtcNetEq* self) {
  if (self->playout_thread != nullptr) {
    return TRUE;
  }
  if (self->playout.stop) {
    return FALSE;
  }

  self->playout.eos_received = FALSE;
  self->playout.joining = FALSE;
  self->playout_thread =
      g_thread_new("webrtcneteq-playout", gst_webrtc_net_eq_playout_thread,
                   self);
  return self->playout_thread != nullptr;
}

static gboolean gst_webrtc_net_eq_ensure_neteq(GstWebrtcNetEq* self) {
  if (self->state != nullptr) {
    return TRUE;
  }

  auto state = std::make_unique<NetEqState>(webrtc::CreateEnvironment());

  webrtc::NetEq::Config config;
  config.sample_rate_hz = kOutputRateHz;
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
  const webrtc::SdpAudioFormat opus_format(
      "opus", kOutputRateHz, static_cast<size_t>(self->channels),
      std::move(parameters));
  if (!state->neteq->RegisterPayloadType(self->payload_type, opus_format)) {
    GST_ERROR_OBJECT(self, "Failed to register Opus payload type %d",
                     self->payload_type);
    return FALSE;
  }
  state->neteq->SetMinimumDelay(self->latency_ms);
  state->neteq->CreateDecoder(self->payload_type);

  self->state = state.release();
  GST_INFO_OBJECT(self,
                  "Created NetEQ for Opus payload type %d, channels %d, "
                  "latency %d ms",
                  self->payload_type, self->channels, self->latency_ms);
  return TRUE;
}

static webrtc::Timestamp gst_webrtc_net_eq_receive_time(GstWebrtcNetEq* self,
                                                        GstBuffer* buffer) {
  std::optional<GstClockTime> receive_time =
      gst_webrtc_net_eq_buffer_timestamp(buffer);
  if (!receive_time.has_value()) {
    receive_time = gst_webrtc_net_eq_segment_position_locked(self);
  }

  if (!receive_time.has_value()) {
    const std::optional<GstClockTime> running_time =
        gst_webrtc_net_eq_running_time(self);
    if (running_time.has_value()) {
      receive_time = *running_time;
    }
  }

  if (!receive_time.has_value()) {
    return webrtc::Timestamp::MinusInfinity();
  }

  return webrtc::Timestamp::Millis(
      static_cast<int64_t>(*receive_time / GST_MSECOND));
}

static gboolean gst_webrtc_net_eq_parse_caps(GstWebrtcNetEq* self,
                                             GstCaps* caps) {
  if (caps == nullptr || gst_caps_is_empty(caps)) {
    return FALSE;
  }

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  gint payload = self->payload_type;
  if (gst_structure_get_int(structure, "payload", &payload) &&
      payload >= 0 && payload <= 127) {
    self->payload_type = payload;
  }

  gint channels = self->channels;
  gint int_encoding_params = 0;
  if (gst_structure_get_int(structure, "encoding-params",
                            &int_encoding_params)) {
    channels = int_encoding_params;
  } else {
    const gchar* encoding_params =
        gst_structure_get_string(structure, "encoding-params");
    if (encoding_params != nullptr) {
      channels = static_cast<gint>(g_ascii_strtoll(encoding_params, nullptr, 10));
    }
  }

  if (channels == 1 || channels == 2) {
    self->channels = channels;
  }

  GST_INFO_OBJECT(self, "Configured from caps: payload type %d, channels %d",
                  self->payload_type, self->channels);
  return TRUE;
}

static gboolean gst_webrtc_net_eq_push_src_caps(GstWebrtcNetEq* self,
                                                int channels) {
  GstCaps* caps = gst_caps_new_simple("audio/x-raw",
                                      "format", G_TYPE_STRING, "S16LE",
                                      "layout", G_TYPE_STRING, "interleaved",
                                      "rate", G_TYPE_INT, kOutputRateHz,
                                      "channels", G_TYPE_INT, channels,
                                      nullptr);
  const gboolean pushed =
      gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  return pushed;
}

static GstFlowReturn gst_webrtc_net_eq_create_audio_locked(
    GstWebrtcNetEq* self,
    GstClockTime output_base_pts,
    guint64* output_samples,
    GstBuffer** out_buffer) {
  webrtc::AudioFrame frame;
  bool muted = false;
  int sample_rate_hz = 0;
  const int result =
      self->state->neteq->GetAudio(&frame, &muted, &sample_rate_hz);
  if (result != webrtc::NetEq::kOK) {
    GST_WARNING_OBJECT(self, "NetEQ GetAudio failed");
    return GST_FLOW_ERROR;
  }

  const int rate = frame.sample_rate_hz() > 0 ? frame.sample_rate_hz()
                                              : sample_rate_hz;
  if (rate != kOutputRateHz) {
    GST_WARNING_OBJECT(self, "Unexpected NetEQ output rate %d Hz", rate);
    return GST_FLOW_ERROR;
  }

  const int channels =
      static_cast<int>(std::clamp<size_t>(frame.num_channels(), 1, 2));
  const size_t samples_per_channel = frame.samples_per_channel();
  const size_t sample_count = samples_per_channel * channels;
  const size_t output_size = sample_count * sizeof(int16_t);

  GstBufferPtr output(gst_buffer_new_allocate(nullptr, output_size, nullptr));
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
    std::memcpy(map.data, frame.data(), output_size);
  }
  gst_buffer_unmap(output.get(), &map);

  GstAudioInfo audio_info;
  gst_audio_info_set_format(&audio_info, GST_AUDIO_FORMAT_S16LE, kOutputRateHz,
                            channels, nullptr);
  if (gst_buffer_add_audio_meta(output.get(), &audio_info, samples_per_channel,
                                nullptr) == nullptr) {
    GST_WARNING_OBJECT(self, "Failed to add audio metadata");
    return GST_FLOW_ERROR;
  }

  GST_BUFFER_PTS(output.get()) =
      output_base_pts +
      gst_util_uint64_scale(*output_samples, GST_SECOND, kOutputRateHz);
  GST_BUFFER_DURATION(output.get()) =
      gst_util_uint64_scale(samples_per_channel, GST_SECOND, kOutputRateHz);
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
  {
    GMutexLock lock(&self->lock);
    self->playout.stop = TRUE;
    g_cond_broadcast(&self->cond);
  }
  return FALSE;
}

static gboolean gst_webrtc_net_eq_maybe_pull_locked(
    GstWebrtcNetEq* self,
    GstClockTime output_base_pts,
    guint64* output_samples,
    gint64* next_pull_us,
    GstBuffer** out_buffer) {
  *out_buffer = nullptr;

  const gint64 now_us = g_get_monotonic_time();
  if (now_us < *next_pull_us) {
    g_cond_wait_until(&self->cond, &self->lock, *next_pull_us);
    return TRUE;
  }

  const GstFlowReturn flow = gst_webrtc_net_eq_create_audio_locked(
      self, output_base_pts, output_samples, out_buffer);
  if (flow != GST_FLOW_OK) {
    self->playout.stop = TRUE;
    return FALSE;
  }

  *next_pull_us += kOutputFrameMs * 1000;
  if (*next_pull_us < now_us - kOutputFrameMs * 1000) {
    *next_pull_us = now_us + kOutputFrameMs * 1000;
  }
  return TRUE;
}

static gpointer gst_webrtc_net_eq_playout_thread(gpointer data) {
  auto* self = GST_WEBRTC_NET_EQ(data);
  GstClockTime output_base_pts;
  {
    GMutexLock lock(&self->lock);
    if (!self->output_base_pts_set) {
      GST_ERROR_OBJECT(self, "Cannot start playout without output base PTS");
      self->playout.stop = TRUE;
      return nullptr;
    }
    output_base_pts = self->output_base_pts;
  }
  guint64 output_samples = 0;
  gint64 next_pull_us = g_get_monotonic_time();

  while (true) {
    GstBuffer* raw_buffer = nullptr;

    {
      GMutexLock lock(&self->lock);
      while (!self->playout.stop && raw_buffer == nullptr) {
        if (!gst_webrtc_net_eq_maybe_pull_locked(
            self, output_base_pts, &output_samples, &next_pull_us,
            &raw_buffer)) {
          return nullptr;
        }
      }
      if (raw_buffer == nullptr) {
        break;
      }
    }

    if (!gst_webrtc_net_eq_push_buffer(self, raw_buffer)) {
      return nullptr;
    }
  }

  guint eos_drain_pulls;
  {
    GMutexLock lock(&self->lock);
    if (!self->playout.eos_received) {
      return nullptr;
    }
    eos_drain_pulls = static_cast<guint>(
        (self->eos_drain_ms + kOutputFrameMs - 1) / kOutputFrameMs);
  }

  for (guint i = 0; i < eos_drain_pulls; ++i) {
    GstBuffer* raw_buffer = nullptr;

    {
      GMutexLock lock(&self->lock);
      while (raw_buffer == nullptr) {
        if (!gst_webrtc_net_eq_maybe_pull_locked(
            self, output_base_pts, &output_samples, &next_pull_us,
            &raw_buffer)) {
          return nullptr;
        }
      }
    }

    if (!gst_webrtc_net_eq_push_buffer(self, raw_buffer)) {
      return nullptr;
    }
  }

  return nullptr;
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

  const webrtc::Timestamp receive_time =
      gst_webrtc_net_eq_receive_time(self, input.get());

  webrtc::RTPHeader header;
  header.markerBit = gst_rtp_buffer_get_marker(&rtp);
  header.payloadType = gst_rtp_buffer_get_payload_type(&rtp);
  header.sequenceNumber = gst_rtp_buffer_get_seq(&rtp);
  header.timestamp = gst_rtp_buffer_get_timestamp(&rtp);
  header.ssrc = gst_rtp_buffer_get_ssrc(&rtp);
  header.numCSRCs = static_cast<uint8_t>(std::min<int>(
      gst_rtp_buffer_get_csrc_count(&rtp), webrtc::kRtpCsrcSize));
  for (uint8_t i = 0; i < header.numCSRCs; ++i) {
    header.arrOfCSRCs[i] = gst_rtp_buffer_get_csrc(&rtp, i);
  }
  header.paddingLength = 0;
  header.headerLength = 0;

  const auto* payload =
      static_cast<const uint8_t*>(gst_rtp_buffer_get_payload(&rtp));
  const guint payload_len = gst_rtp_buffer_get_payload_len(&rtp);
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
      {
        GMutexLock lock(&self->lock);
        latency_ms = self->latency_ms;
      }

      const GstClockTime neteq_latency =
          static_cast<GstClockTime>(latency_ms) * GST_MSECOND;
      min_latency = gst_webrtc_net_eq_add_latency(min_latency, neteq_latency);
      max_latency = gst_webrtc_net_eq_add_latency(max_latency, neteq_latency);

      gst_query_set_latency(query, TRUE, min_latency, max_latency);
      GST_DEBUG_OBJECT(self,
                       "Latency query: upstream_live=%d, neteq_latency=%"
                       GST_TIME_FORMAT ", min=%" GST_TIME_FORMAT ", max=%"
                       GST_TIME_FORMAT,
                       live, GST_TIME_ARGS(neteq_latency),
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
      {
        GMutexLock lock(&self->lock);
        gst_webrtc_net_eq_reset_state(self);
        if (!gst_webrtc_net_eq_parse_caps(self, caps)) {
          gst_event_unref(event);
          return FALSE;
        }
        channels = self->channels;
      }
      gst_event_unref(event);
      return gst_webrtc_net_eq_push_src_caps(self, channels);
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

    case GST_EVENT_FLUSH_STOP:
      gst_webrtc_net_eq_stop_task(self, FALSE);
      {
        GMutexLock lock(&self->lock);
        gst_webrtc_net_eq_reset_state(self);
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
    case PROP_PAYLOAD_TYPE: {
      gst_webrtc_net_eq_stop_task(self, FALSE);
      GMutexLock lock(&self->lock);
      self->payload_type = g_value_get_int(value);
      gst_webrtc_net_eq_reset_state(self);
      return;
    }
    case PROP_LATENCY_MS: {
      gst_webrtc_net_eq_stop_task(self, FALSE);
      {
        GMutexLock lock(&self->lock);
        self->latency_ms = g_value_get_int(value);
        gst_webrtc_net_eq_reset_state(self);
      }
      gst_element_post_message(
          GST_ELEMENT(self), gst_message_new_latency(GST_OBJECT(self)));
      return;
    }
    case PROP_CHANNELS: {
      gst_webrtc_net_eq_stop_task(self, FALSE);
      GMutexLock lock(&self->lock);
      self->channels = g_value_get_int(value);
      gst_webrtc_net_eq_reset_state(self);
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
    case PROP_PAYLOAD_TYPE:
      g_value_set_int(value, self->payload_type);
      return;
    case PROP_LATENCY_MS:
      g_value_set_int(value, self->latency_ms);
      return;
    case PROP_CHANNELS:
      g_value_set_int(value, self->channels);
      return;
    case PROP_EOS_DRAIN_MS:
      g_value_set_int(value, self->eos_drain_ms);
      return;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      return;
  }
}

static void gst_webrtc_net_eq_finalize(GObject* object) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  gst_webrtc_net_eq_stop_task(self, FALSE);

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

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    gst_webrtc_net_eq_stop_task(self, FALSE);
    {
      GMutexLock lock(&self->lock);
      gst_webrtc_net_eq_reset_state(self);
    }
  }

  return GST_ELEMENT_CLASS(gst_webrtc_net_eq_parent_class)
      ->change_state(element, transition);
}

static void gst_webrtc_net_eq_class_init(GstWebrtcNetEqClass* klass) {
  auto* gobject_class = G_OBJECT_CLASS(klass);
  auto* element_class = GST_ELEMENT_CLASS(klass);

  gobject_class->set_property = gst_webrtc_net_eq_set_property;
  gobject_class->get_property = gst_webrtc_net_eq_get_property;
  gobject_class->finalize = gst_webrtc_net_eq_finalize;
  element_class->change_state = gst_webrtc_net_eq_change_state;

  g_object_class_install_property(
      gobject_class, PROP_PAYLOAD_TYPE,
      g_param_spec_int("payload-type", "Payload type",
                       "RTP payload type to register as Opus", 0, 127,
                       kDefaultPayloadType,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_LATENCY_MS,
      g_param_spec_int("latency-ms", "Latency",
                       "NetEQ minimum playout latency in milliseconds", 0, 5000,
                       kDefaultLatencyMs,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_CHANNELS,
      g_param_spec_int("channels", "Channels",
                       "Opus channel count to register with NetEQ", 1, 2,
                       kDefaultChannels,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_EOS_DRAIN_MS,
      g_param_spec_int("eos-drain-ms", "EOS drain",
                       "Extra NetEQ playout time to emit after the last packet",
                       0, 30000, kDefaultEosDrainMs,
                       static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                G_PARAM_STATIC_STRINGS)));

  gst_element_class_set_static_metadata(
      element_class, "WebRTC NetEQ RTP Opus decoder", "Codec/Decoder/Audio",
      "Decodes RTP/Opus through libwebrtc NetEQ", "Recall.ai");
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);

  GST_DEBUG_CATEGORY_INIT(gst_webrtc_net_eq_debug, "webrtcneteq", 0,
                          "WebRTC NetEQ GStreamer element");
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
  self->playout_thread = nullptr;
  self->state = nullptr;
  self->payload_type = kDefaultPayloadType;
  self->latency_ms = kDefaultLatencyMs;
  self->channels = kDefaultChannels;
  self->eos_drain_ms = kDefaultEosDrainMs;
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
