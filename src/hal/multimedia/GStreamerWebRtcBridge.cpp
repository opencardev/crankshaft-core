/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2025 OpenCarDev Team
 *
 *  Crankshaft is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Crankshaft is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Crankshaft. If not, see <http://www.gnu.org/licenses/>.
 */

#include "GStreamerWebRtcBridge.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#if HAVE_GSTREAMER_WEBRTC
#include <gst/sdp/gstsdpmessage.h>
#include <gst/webrtc/webrtc.h>
#endif

#include "../../services/logging/Logger.h"

namespace {
constexpr guint kWebRtcPayloadType = 96;
constexpr auto kSignalingPrefix = "android-auto/webrtc/";

#if HAVE_GSTREAMER_WEBRTC
static auto extractStringField(const QVariantMap& payload, const QString& key) -> QString {
  return payload.value(key).toString().trimmed();
}

static auto requestWebRtcSinkPad(GstElement* webrtcBin, QString* requestedTemplateName)
    -> GstPad* {
  if (!webrtcBin) {
    return nullptr;
  }

  static constexpr const char* kPadTemplates[] = {
      "sink_%u",
      "send_rtp_sink_%u",
  };

  for (const char* padTemplate : kPadTemplates) {
    GstPad* pad = gst_element_request_pad_simple(webrtcBin, padTemplate);
    if (pad) {
      if (requestedTemplateName) {
        *requestedTemplateName = QString::fromLatin1(padTemplate);
      }
      return pad;
    }
  }

  return nullptr;
}
#endif
}  // namespace

struct GStreamerWebRtcBridge::Private {
  QMutex mutex;
  QSize streamResolution;
  int fps{30};
  bool initialized{false};

#if HAVE_GSTREAMER_WEBRTC
  GstElement* pipeline{nullptr};
  GstElement* appSrc{nullptr};
  GstElement* h264Parse{nullptr};
  GstElement* queue{nullptr};
  GstElement* payloader{nullptr};
  GstElement* webrtcBin{nullptr};
  guint watchId{0};
#endif
};

GStreamerWebRtcBridge::GStreamerWebRtcBridge(QObject* parent)
    : QObject(parent), m_private(std::make_unique<Private>()) {
  gst_init(nullptr, nullptr);
}

GStreamerWebRtcBridge::~GStreamerWebRtcBridge() {
  deinitialize();
}

bool GStreamerWebRtcBridge::isInitialized() const {
  const QMutexLocker locker(&m_private->mutex);
  return m_private->initialized;
}

bool GStreamerWebRtcBridge::initialize(const QSize& streamResolution, int fps) {
  QMutexLocker locker(&m_private->mutex);
  if (m_private->initialized) {
    return true;
  }

  m_private->streamResolution = streamResolution;
  m_private->fps = fps;

#if !HAVE_GSTREAMER_WEBRTC
  Q_UNUSED(streamResolution)
  Q_UNUSED(fps)
  emit errorOccurred(QStringLiteral("GStreamer WebRTC support is not available in this build"));
  return false;
#else
  m_private->pipeline = gst_pipeline_new("android-auto-webrtc");
  m_private->appSrc = gst_element_factory_make("appsrc", "webrtc-source");
  m_private->h264Parse = gst_element_factory_make("h264parse", "webrtc-h264parse");
  m_private->queue = gst_element_factory_make("queue", "webrtc-queue");
  m_private->payloader = gst_element_factory_make("rtph264pay", "webrtc-payloader");
  m_private->webrtcBin = gst_element_factory_make("webrtcbin", "webrtc-bin");

  if (!m_private->pipeline || !m_private->appSrc || !m_private->h264Parse || !m_private->queue ||
      !m_private->payloader || !m_private->webrtcBin) {
    emit errorOccurred(QStringLiteral("Failed to create GStreamer WebRTC pipeline elements"));
    deinitialize();
    return false;
  }

  g_object_set(G_OBJECT(m_private->appSrc), "is-live", TRUE, "format", GST_FORMAT_TIME,
               "do-timestamp", TRUE, "stream-type", GST_APP_STREAM_TYPE_STREAM, nullptr);
  GstCaps* appSrcCaps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING,
                                            "byte-stream", "alignment", G_TYPE_STRING, "nal",
                                            nullptr);
  g_object_set(G_OBJECT(m_private->appSrc), "caps", appSrcCaps, nullptr);
  gst_caps_unref(appSrcCaps);

  g_object_set(G_OBJECT(m_private->payloader), "pt", static_cast<int>(kWebRtcPayloadType),
               "config-interval", 1, nullptr);

  gst_bin_add_many(GST_BIN(m_private->pipeline), m_private->appSrc, m_private->h264Parse,
                   m_private->queue, m_private->payloader, m_private->webrtcBin, nullptr);

  if (!gst_element_link(m_private->appSrc, m_private->h264Parse) ||
      !gst_element_link(m_private->h264Parse, m_private->queue) ||
      !gst_element_link(m_private->queue, m_private->payloader)) {
    emit errorOccurred(QStringLiteral("Failed to link GStreamer WebRTC source pipeline"));
    deinitialize();
    return false;
  }

  GstPad* payloaderSrcPad = gst_element_get_static_pad(m_private->payloader, "src");
  QString webrtcSinkTemplateName;
  GstPad* webrtcSinkPad = requestWebRtcSinkPad(m_private->webrtcBin, &webrtcSinkTemplateName);
  if (!payloaderSrcPad || !webrtcSinkPad) {
    if (payloaderSrcPad) {
      gst_object_unref(payloaderSrcPad);
    }
    if (webrtcSinkPad) {
      gst_object_unref(webrtcSinkPad);
    }
    emit errorOccurred(QStringLiteral("Failed to acquire WebRTC RTP sink pad (tried sink_%u, send_rtp_sink_%u)"));
    deinitialize();
    return false;
  }

  const GstPadLinkReturn linkResult = gst_pad_link(payloaderSrcPad, webrtcSinkPad);
  if (linkResult != GST_PAD_LINK_OK) {
    const gchar* linkName = gst_pad_link_get_name(linkResult);
    const QString linkReason = linkName ? QString::fromUtf8(linkName) : QStringLiteral("unknown");
    if (linkName) {
      g_free(const_cast<gchar*>(linkName));
    }

    gst_object_unref(payloaderSrcPad);
    gst_object_unref(webrtcSinkPad);
    emit errorOccurred(
      QStringLiteral("Failed to connect RTP payload to webrtcbin (template=%1 pad_link=%2)")
        .arg(webrtcSinkTemplateName, linkReason));
    deinitialize();
    return false;
  }
  gst_object_unref(payloaderSrcPad);
  gst_object_unref(webrtcSinkPad);

  g_signal_connect(m_private->webrtcBin, "on-negotiation-needed", G_CALLBACK(+[](GstElement*,
                                                                                  gpointer userData) {
    auto* self = static_cast<GStreamerWebRtcBridge*>(userData);
    QVariantMap payload;
    payload[QStringLiteral("event")] = QStringLiteral("negotiation-needed");
    emit self->statusChanged(QJsonObject{{QStringLiteral("state"), QStringLiteral("negotiation-needed")}});
    GstPromise* offerPromise = gst_promise_new_with_change_func(
        +[](GstPromise* promise, gpointer promiseUserData) {
          auto* bridge = static_cast<GStreamerWebRtcBridge*>(promiseUserData);
          const GstStructure* reply = gst_promise_get_reply(promise);
          if (!reply) {
            emit bridge->errorOccurred(QStringLiteral("WebRTC offer promise had no reply"));
            gst_promise_unref(promise);
            return;
          }
          GstWebRTCSessionDescription* offer = nullptr;
          gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer,
                            nullptr);
          if (!offer || !offer->sdp) {
            emit bridge->errorOccurred(QStringLiteral("WebRTC offer creation failed"));
            if (offer) {
              gst_webrtc_session_description_free(offer);
            }
            gst_promise_unref(promise);
            return;
          }

          GstPromise* setLocalPromise = gst_promise_new();
          g_signal_emit_by_name(bridge->m_private->webrtcBin, "set-local-description", offer,
                                setLocalPromise);
          gst_promise_interrupt(setLocalPromise);
          gst_promise_unref(setLocalPromise);

          gchar* sdpText = gst_sdp_message_as_text(offer->sdp);
          QVariantMap offerPayload;
          offerPayload[QStringLiteral("type")] = QStringLiteral("offer");
          offerPayload[QStringLiteral("sdp")] = QString::fromUtf8(sdpText);
          offerPayload[QStringLiteral("media")] = QStringLiteral("video");
          offerPayload[QStringLiteral("codec")] = QStringLiteral("H264");
          emit bridge->signalingMessageReady(QStringLiteral("android-auto/webrtc/offer"),
                                             offerPayload);
          g_free(sdpText);
          gst_webrtc_session_description_free(offer);
          gst_promise_unref(promise);
        },
        self, nullptr);

    g_signal_emit_by_name(self->m_private->webrtcBin, "create-offer", nullptr, offerPromise);
  }), this);

  g_signal_connect(m_private->webrtcBin, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint mlineIndex,
                                                                            gchar* candidate,
                                                                            gpointer userData) {
    auto* self = static_cast<GStreamerWebRtcBridge*>(userData);
    QVariantMap payload;
    payload[QStringLiteral("type")] = QStringLiteral("ice-candidate");
    payload[QStringLiteral("sdpMid")] = QStringLiteral("video");
    payload[QStringLiteral("sdpMLineIndex")] = static_cast<int>(mlineIndex);
    payload[QStringLiteral("candidate")] = QString::fromUtf8(candidate);
    emit self->signalingMessageReady(QStringLiteral("android-auto/webrtc/ice-candidate"), payload);
  }), this);

  GstStateChangeReturn stateChange = gst_element_set_state(m_private->pipeline, GST_STATE_PLAYING);
  if (stateChange == GST_STATE_CHANGE_FAILURE) {
    emit errorOccurred(QStringLiteral("Failed to start GStreamer WebRTC pipeline"));
    deinitialize();
    return false;
  }

  m_private->initialized = true;
  emit statusChanged(QJsonObject{{QStringLiteral("state"), QStringLiteral("initialized")},
                                 {QStringLiteral("resolution_width"), streamResolution.width()},
                                 {QStringLiteral("resolution_height"), streamResolution.height()},
                                 {QStringLiteral("fps"), fps}});
  return true;
#endif
}

void GStreamerWebRtcBridge::deinitialize() {
  QMutexLocker locker(&m_private->mutex);
#if HAVE_GSTREAMER_WEBRTC
  if (m_private->pipeline) {
    gst_element_set_state(m_private->pipeline, GST_STATE_NULL);
  }
  if (m_private->pipeline) {
    gst_object_unref(m_private->pipeline);
    m_private->pipeline = nullptr;
  }
  m_private->appSrc = nullptr;
  m_private->h264Parse = nullptr;
  m_private->queue = nullptr;
  m_private->payloader = nullptr;
  m_private->webrtcBin = nullptr;
  m_private->watchId = 0;
#endif
  m_private->initialized = false;
}

bool GStreamerWebRtcBridge::pushVideoFrame(const QByteArray& encodedFrame) {
  QMutexLocker locker(&m_private->mutex);
#if !HAVE_GSTREAMER_WEBRTC
  Q_UNUSED(encodedFrame)
  return false;
#else
  if (!m_private->initialized || !m_private->appSrc || encodedFrame.isEmpty()) {
    return false;
  }

  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, encodedFrame.size(), nullptr);
  if (!buffer) {
    return false;
  }
  gst_buffer_fill(buffer, 0, encodedFrame.constData(), encodedFrame.size());
  const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(m_private->appSrc), buffer);
  return result == GST_FLOW_OK;
#endif
}

bool GStreamerWebRtcBridge::handleWebRtcSignalingMessage(const QString& topic,
                                                         const QVariantMap& payload) {
  QMutexLocker locker(&m_private->mutex);
#if !HAVE_GSTREAMER_WEBRTC
  Q_UNUSED(topic)
  Q_UNUSED(payload)
  return false;
#else
  if (!m_private->initialized || !m_private->webrtcBin) {
    return false;
  }

  if (!topic.startsWith(QString::fromLatin1(kSignalingPrefix))) {
    return false;
  }

  const QString messageType = topic.mid(QString::fromLatin1(kSignalingPrefix).size());
  if (messageType == QStringLiteral("answer")) {
    const QString sdpText = extractStringField(payload, QStringLiteral("sdp"));
    if (sdpText.isEmpty()) {
      emit errorOccurred(QStringLiteral("WebRTC answer missing SDP"));
      return false;
    }

    const QByteArray sdpBytes = sdpText.toUtf8();

    GstSDPMessage* sdpMessage = nullptr;
    if (gst_sdp_message_new(&sdpMessage) != GST_SDP_OK ||
        gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(sdpBytes.constData()),
                                     sdpBytes.size(), sdpMessage) != GST_SDP_OK) {
      emit errorOccurred(QStringLiteral("Failed to parse WebRTC answer SDP"));
      if (sdpMessage) {
        gst_sdp_message_free(sdpMessage);
      }
      return false;
    }

    GstWebRTCSessionDescription* answer =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpMessage);
    GstPromise* setRemotePromise = gst_promise_new();
    g_signal_emit_by_name(m_private->webrtcBin, "set-remote-description", answer,
                setRemotePromise);
    gst_promise_interrupt(setRemotePromise);
    gst_promise_unref(setRemotePromise);
    gst_webrtc_session_description_free(answer);
    emit statusChanged(QJsonObject{{QStringLiteral("state"), QStringLiteral("remote-answer-set")}});
    return true;
  }

  if (messageType == QStringLiteral("ice-candidate")) {
    const QString candidate = extractStringField(payload, QStringLiteral("candidate"));
    const int mlineIndex = payload.value(QStringLiteral("sdpMLineIndex"), 0).toInt();
    const QString sdpMid = extractStringField(payload, QStringLiteral("sdpMid"));
    if (candidate.isEmpty()) {
      emit errorOccurred(QStringLiteral("WebRTC ICE candidate missing candidate string"));
      return false;
    }
    g_signal_emit_by_name(m_private->webrtcBin, "add-ice-candidate", mlineIndex,
                          candidate.toUtf8().constData());
    Q_UNUSED(sdpMid)
    return true;
  }

  return false;
#endif
}
