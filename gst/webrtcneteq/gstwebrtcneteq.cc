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

}  // namespace

typedef struct _GstWebrtcNetEq GstWebrtcNetEq;
typedef struct _GstWebrtcNetEqClass GstWebrtcNetEqClass;

struct _GstWebrtcNetEq {
  GstElement parent;

  GstPad* sinkpad;
  GstPad* srcpad;

  GMutex lock;
  NetEqState* state;

  gint payload_type;
  gint latency_ms;
  gint channels;
  gint eos_drain_ms;

  gboolean stream_started;
  gboolean segment_started;
  gboolean src_caps_sent;

  gboolean timing_started;
  gint64 first_receive_ms;
  gint64 last_receive_ms;
  gint64 next_playout_ms;
  guint64 output_samples;
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

static void gst_webrtc_net_eq_reset_state(GstWebrtcNetEq* self) {
  delete self->state;
  self->state = nullptr;

  self->src_caps_sent = FALSE;
  self->segment_started = FALSE;
  self->timing_started = FALSE;
  self->first_receive_ms = 0;
  self->last_receive_ms = 0;
  self->next_playout_ms = 0;
  self->output_samples = 0;
}

static gboolean gst_webrtc_net_eq_ensure_neteq(GstWebrtcNetEq* self) {
  if (self->state != nullptr) {
    return TRUE;
  }

  auto* state = new NetEqState(webrtc::CreateEnvironment());

  webrtc::NetEq::Config config;
  config.sample_rate_hz = kOutputRateHz;
  config.min_delay_ms = self->latency_ms;
  config.enable_fast_accelerate = true;

  state->neteq = webrtc::DefaultNetEqFactory().Create(
      state->env, config, webrtc::CreateOpusAudioDecoderFactory());
  if (!state->neteq) {
    GST_ERROR_OBJECT(self, "Failed to create WebRTC NetEQ");
    delete state;
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
    delete state;
    return FALSE;
  }
  state->neteq->SetMinimumDelay(self->latency_ms);
  state->neteq->CreateDecoder(self->payload_type);

  self->state = state;
  GST_INFO_OBJECT(self,
                  "Created NetEQ for Opus payload type %d, channels %d, "
                  "latency %d ms",
                  self->payload_type, self->channels, self->latency_ms);
  return TRUE;
}

static gint64 gst_webrtc_net_eq_receive_time_ms(GstWebrtcNetEq* self,
                                                GstBuffer* buffer) {
  GstClockTime receive_time = GST_BUFFER_PTS(buffer);
  if (!GST_CLOCK_TIME_IS_VALID(receive_time)) {
    receive_time = GST_BUFFER_DTS(buffer);
  }

  if (!GST_CLOCK_TIME_IS_VALID(receive_time)) {
    GstClock* clock = gst_element_get_clock(GST_ELEMENT(self));
    if (clock != nullptr) {
      const GstClockTime now = gst_clock_get_time(clock);
      const GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT(self));
      if (GST_CLOCK_TIME_IS_VALID(now) && now >= base_time) {
        receive_time = now - base_time;
      }
      gst_object_unref(clock);
    }
  }

  if (!GST_CLOCK_TIME_IS_VALID(receive_time)) {
    if (self->timing_started) {
      return self->last_receive_ms + 20;
    }
    return 0;
  }

  return static_cast<gint64>(receive_time / GST_MSECOND);
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

static gboolean gst_webrtc_net_eq_start_stream(GstWebrtcNetEq* self) {
  if (!self->stream_started) {
    gchar* stream_id =
        gst_pad_create_stream_id(self->srcpad, GST_ELEMENT(self), nullptr);
    const gboolean pushed =
        gst_pad_push_event(self->srcpad, gst_event_new_stream_start(stream_id));
    g_free(stream_id);
    if (!pushed) {
      return FALSE;
    }
    self->stream_started = TRUE;
  }

  return TRUE;
}

static gboolean gst_webrtc_net_eq_start_segment(GstWebrtcNetEq* self) {
  if (!self->segment_started) {
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    if (!gst_pad_push_event(self->srcpad, gst_event_new_segment(&segment))) {
      return FALSE;
    }
    self->segment_started = TRUE;
  }

  return TRUE;
}

static gboolean gst_webrtc_net_eq_set_src_caps(GstWebrtcNetEq* self,
                                               int channels) {
  if (self->src_caps_sent) {
    return TRUE;
  }

  GstCaps* caps = gst_caps_new_simple("audio/x-raw",
                                      "format", G_TYPE_STRING, "S16LE",
                                      "layout", G_TYPE_STRING, "interleaved",
                                      "rate", G_TYPE_INT, kOutputRateHz,
                                      "channels", G_TYPE_INT, channels,
                                      nullptr);
  const gboolean pushed =
      gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  if (pushed) {
    self->src_caps_sent = TRUE;
  }
  return pushed;
}

static GstFlowReturn gst_webrtc_net_eq_push_audio(GstWebrtcNetEq* self) {
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

  if (!gst_webrtc_net_eq_start_stream(self) ||
      !gst_webrtc_net_eq_set_src_caps(self, channels) ||
      !gst_webrtc_net_eq_start_segment(self)) {
    return GST_FLOW_ERROR;
  }

  GstBuffer* output = gst_buffer_new_allocate(nullptr, output_size, nullptr);
  if (output == nullptr) {
    return GST_FLOW_ERROR;
  }

  GstMapInfo map;
  if (!gst_buffer_map(output, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(output);
    return GST_FLOW_ERROR;
  }

  if (muted) {
    std::memset(map.data, 0, output_size);
  } else {
    std::memcpy(map.data, frame.data(), output_size);
  }
  gst_buffer_unmap(output, &map);

  GST_BUFFER_PTS(output) =
      gst_util_uint64_scale(self->output_samples, GST_SECOND, kOutputRateHz);
  GST_BUFFER_DURATION(output) =
      gst_util_uint64_scale(samples_per_channel, GST_SECOND, kOutputRateHz);
  self->output_samples += samples_per_channel;

  return gst_pad_push(self->srcpad, output);
}

static GstFlowReturn gst_webrtc_net_eq_produce_until(GstWebrtcNetEq* self,
                                                     gint64 target_ms) {
  GstFlowReturn flow = GST_FLOW_OK;
  while (self->next_playout_ms <= target_ms) {
    flow = gst_webrtc_net_eq_push_audio(self);
    if (flow != GST_FLOW_OK) {
      return flow;
    }
    self->next_playout_ms += kOutputFrameMs;
  }
  return flow;
}

static GstFlowReturn gst_webrtc_net_eq_chain(GstPad* pad,
                                             GstObject* parent,
                                             GstBuffer* buffer) {
  auto* self = GST_WEBRTC_NET_EQ(parent);
  GstFlowReturn flow = GST_FLOW_OK;

  g_mutex_lock(&self->lock);
  if (!gst_webrtc_net_eq_ensure_neteq(self)) {
    g_mutex_unlock(&self->lock);
    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
  }

  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
    GST_WARNING_OBJECT(self, "Failed to map RTP buffer");
    g_mutex_unlock(&self->lock);
    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
  }

  const gint64 receive_ms = gst_webrtc_net_eq_receive_time_ms(self, buffer);
  if (!self->timing_started) {
    self->timing_started = TRUE;
    self->first_receive_ms = receive_ms;
    self->next_playout_ms = receive_ms + self->latency_ms;
  }
  self->last_receive_ms = receive_ms;

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
      header, std::span<const uint8_t>(payload, payload_len),
      webrtc::Timestamp::Millis(receive_ms));
  gst_rtp_buffer_unmap(&rtp);

  if (insert_result != webrtc::NetEq::kOK) {
    GST_WARNING_OBJECT(self,
                       "NetEQ InsertPacket failed for seq=%u ts=%u ssrc=%u",
                       header.sequenceNumber, header.timestamp, header.ssrc);
  }

  flow = gst_webrtc_net_eq_produce_until(self, receive_ms);
  g_mutex_unlock(&self->lock);

  gst_buffer_unref(buffer);
  return flow;
}

static gboolean gst_webrtc_net_eq_sink_event(GstPad* pad,
                                             GstObject* parent,
                                             GstEvent* event) {
  auto* self = GST_WEBRTC_NET_EQ(parent);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_STREAM_START:
      self->stream_started = TRUE;
      return gst_pad_push_event(self->srcpad, event);

    case GST_EVENT_CAPS: {
      GstCaps* caps = nullptr;
      gst_event_parse_caps(event, &caps);
      g_mutex_lock(&self->lock);
      const gboolean parsed = gst_webrtc_net_eq_parse_caps(self, caps);
      gst_webrtc_net_eq_reset_state(self);
      g_mutex_unlock(&self->lock);
      gst_event_unref(event);
      return parsed;
    }

    case GST_EVENT_SEGMENT:
      gst_event_unref(event);
      return TRUE;

    case GST_EVENT_FLUSH_STOP:
      g_mutex_lock(&self->lock);
      gst_webrtc_net_eq_reset_state(self);
      g_mutex_unlock(&self->lock);
      return gst_pad_push_event(self->srcpad, event);

    case GST_EVENT_EOS: {
      GstFlowReturn flow = GST_FLOW_OK;
      g_mutex_lock(&self->lock);
      if (self->state != nullptr && self->timing_started) {
        const gint64 drain_until =
            self->last_receive_ms + self->latency_ms + self->eos_drain_ms;
        flow = gst_webrtc_net_eq_produce_until(self, drain_until);
      }
      g_mutex_unlock(&self->lock);
      gst_event_unref(event);
      if (flow != GST_FLOW_OK) {
        return FALSE;
      }
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

  g_mutex_lock(&self->lock);
  switch (prop_id) {
    case PROP_PAYLOAD_TYPE:
      self->payload_type = g_value_get_int(value);
      gst_webrtc_net_eq_reset_state(self);
      break;
    case PROP_LATENCY_MS:
      self->latency_ms = g_value_get_int(value);
      gst_webrtc_net_eq_reset_state(self);
      break;
    case PROP_CHANNELS:
      self->channels = g_value_get_int(value);
      gst_webrtc_net_eq_reset_state(self);
      break;
    case PROP_EOS_DRAIN_MS:
      self->eos_drain_ms = g_value_get_int(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  g_mutex_unlock(&self->lock);
}

static void gst_webrtc_net_eq_get_property(GObject* object,
                                           guint prop_id,
                                           GValue* value,
                                           GParamSpec* pspec) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  g_mutex_lock(&self->lock);
  switch (prop_id) {
    case PROP_PAYLOAD_TYPE:
      g_value_set_int(value, self->payload_type);
      break;
    case PROP_LATENCY_MS:
      g_value_set_int(value, self->latency_ms);
      break;
    case PROP_CHANNELS:
      g_value_set_int(value, self->channels);
      break;
    case PROP_EOS_DRAIN_MS:
      g_value_set_int(value, self->eos_drain_ms);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  g_mutex_unlock(&self->lock);
}

static void gst_webrtc_net_eq_finalize(GObject* object) {
  auto* self = GST_WEBRTC_NET_EQ(object);

  g_mutex_lock(&self->lock);
  delete self->state;
  self->state = nullptr;
  g_mutex_unlock(&self->lock);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gst_webrtc_net_eq_parent_class)->finalize(object);
}

static void gst_webrtc_net_eq_class_init(GstWebrtcNetEqClass* klass) {
  auto* gobject_class = G_OBJECT_CLASS(klass);
  auto* element_class = GST_ELEMENT_CLASS(klass);

  gobject_class->set_property = gst_webrtc_net_eq_set_property;
  gobject_class->get_property = gst_webrtc_net_eq_get_property;
  gobject_class->finalize = gst_webrtc_net_eq_finalize;

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
  gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

  g_mutex_init(&self->lock);
  self->state = nullptr;
  self->payload_type = kDefaultPayloadType;
  self->latency_ms = kDefaultLatencyMs;
  self->channels = kDefaultChannels;
  self->eos_drain_ms = kDefaultEosDrainMs;
  self->stream_started = FALSE;
  self->segment_started = FALSE;
  self->src_caps_sent = FALSE;
  self->timing_started = FALSE;
  self->first_receive_ms = 0;
  self->last_receive_ms = 0;
  self->next_playout_ms = 0;
  self->output_samples = 0;
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
