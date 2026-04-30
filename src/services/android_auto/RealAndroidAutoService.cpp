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

#include "RealAndroidAutoService.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <functional>

#include "../../hal/multimedia/AudioMixer.h"
#include "../../hal/multimedia/GStreamerVideoDecoder.h"
#include "../audio/AudioRouter.h"
#include "../config/ConfigService.h"
#include "../eventbus/EventBus.h"
#include "../logging/Logger.h"
#include "../session/SessionStore.h"
#include "ProtocolHelpers.h"
#include "IWirelessNetworkManager.h"

// AASDK includes
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <aasdk/Channel/Bluetooth/BluetoothService.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/InputSource/InputSourceService.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/GuidanceAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/MediaAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/SystemAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Audio/Channel/TelephonyAudioChannel.hpp>
#include <aasdk/Channel/MediaSink/Video/Channel/VideoChannel.hpp>
#include <aasdk/Channel/MediaSource/Audio/MicrophoneAudioChannel.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Channel/WifiProjection/WifiProjectionService.hpp>
#include <aasdk/Common/ModernLogger.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/TCPTransport.hpp>
#include <aasdk/Transport/USBTransport.hpp>
#include <aasdk/USB/AOAPDevice.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/IAccessoryModeQueryChain.hpp>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/USBWrapper.hpp>
#include <boost/asio.hpp>

/**
 * @brief Log info message through standard structured logger
 */
static void logInfo(const std::string& msg) {
  Logger::instance().info(QString::fromStdString(msg));
}

/**
 * @brief Log error message through standard structured logger
 */
static void logError(const std::string& msg) {
  Logger::instance().error(QString::fromStdString(msg));
}

static void aaLogDebug(const char* area, const QString& message) {
  Logger::instance().debug(QString("[AA][%1] %2").arg(area, message));
}

static void aaLogInfo(const char* area, const QString& message) {
  Logger::instance().info(QString("[AA][%1] %2").arg(area, message));
}

static void aaLogWarning(const char* area, const QString& message) {
  Logger::instance().warning(QString("[AA][%1] %2").arg(area, message));
}

static std::atomic_bool g_channelDebugTelemetryEnabled{true};

static auto connectionStateToString(AndroidAutoService::ConnectionState state) -> QString {
  switch (state) {
    case AndroidAutoService::ConnectionState::DISCONNECTED:
      return QStringLiteral("DISCONNECTED");
    case AndroidAutoService::ConnectionState::SEARCHING:
      return QStringLiteral("SEARCHING");
    case AndroidAutoService::ConnectionState::CONNECTING:
      return QStringLiteral("CONNECTING");
    case AndroidAutoService::ConnectionState::AUTHENTICATING:
      return QStringLiteral("AUTHENTICATING");
    case AndroidAutoService::ConnectionState::SECURING:
      return QStringLiteral("SECURING");
    case AndroidAutoService::ConnectionState::CONNECTED:
      return QStringLiteral("CONNECTED");
    case AndroidAutoService::ConnectionState::DISCONNECTING:
      return QStringLiteral("DISCONNECTING");
    case AndroidAutoService::ConnectionState::ERROR:
      return QStringLiteral("ERROR");
    default:
      return QStringLiteral("UNKNOWN");
  }
}

static constexpr qint64 kChannelDebugIntervalMs = 2000;

static auto shouldEmitChannelDebugSample(quint64* sampleCounter, qint64* lastEmitMs) -> bool {
  if (!g_channelDebugTelemetryEnabled.load()) {
    return false;
  }

  if (!sampleCounter || !lastEmitMs) {
    return true;
  }

  ++(*sampleCounter);
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (*lastEmitMs == 0 || (nowMs - *lastEmitMs) >= kChannelDebugIntervalMs) {
    *lastEmitMs = nowMs;
    return true;
  }

  return false;
}

static auto describeChannelConfig(const RealAndroidAutoService::ChannelConfig& config) -> QString {
  return QStringLiteral(
             "video=%1, mediaAudio=%2, systemAudio=%3, speechAudio=%4, "
             "telephonyAudio=%5, microphone=%6, input=%7, sensor=%8, bluetooth=%9")
      .arg(config.videoEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.mediaAudioEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.systemAudioEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.speechAudioEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.telephonyAudioEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.microphoneEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.inputEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.sensorEnabled ? QStringLiteral("on") : QStringLiteral("off"))
      .arg(config.bluetoothEnabled ? QStringLiteral("on") : QStringLiteral("off"));
}

static auto parseUsbVendorIdToken(const QString& token, const QString& key, bool* ok) -> uint16_t {
  if (ok != nullptr) {
    *ok = false;
  }

  const QString trimmed = token.trimmed();
  if (trimmed.isEmpty()) {
    return 0;
  }

  bool parsed = false;
  uint value = trimmed.toUInt(&parsed, 0);
  if (!parsed) {
    const QString hexToken =
        trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? trimmed.mid(2) : trimmed;
    value = hexToken.toUInt(&parsed, 16);
  }

  if (!parsed || value == 0 || value > 0xFFFF) {
    Logger::instance().warning(
        QString("[RealAndroidAutoService] Ignoring invalid USB vendor entry '%1' in %2")
            .arg(trimmed)
            .arg(key));
    return 0;
  }

  if (ok != nullptr) {
    *ok = true;
  }

  return static_cast<uint16_t>(value);
}

static auto readUsbVendorFilterSet(const QString& key) -> QSet<uint16_t> {
  const QVariant rawValue = ConfigService::instance().get(key, QVariant());
  QSet<uint16_t> vendorIds;

  auto addToken = [&vendorIds, &key](const QString& token) {
    bool ok = false;
    const uint16_t vendorId = parseUsbVendorIdToken(token, key, &ok);
    if (ok) {
      vendorIds.insert(vendorId);
    }
  };

  if (!rawValue.isValid() || rawValue.isNull()) {
    return vendorIds;
  }

  if (rawValue.canConvert<QVariantList>()) {
    const QVariantList list = rawValue.toList();
    for (const QVariant& entry : list) {
      addToken(entry.toString());
    }
    return vendorIds;
  }

  if (rawValue.canConvert<QString>()) {
    QString tokenList = rawValue.toString();
    tokenList.replace(';', ',');
    const QStringList tokens = tokenList.split(',', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
      addToken(token);
    }
    return vendorIds;
  }

  addToken(rawValue.toString());
  return vendorIds;
}

static auto formatUsbVendorFilterSet(const QSet<uint16_t>& vendorIds) -> QString {
  if (vendorIds.isEmpty()) {
    return QStringLiteral("<none>");
  }

  QList<uint16_t> sorted = vendorIds.values();
  std::sort(sorted.begin(), sorted.end());

  QStringList formatted;
  formatted.reserve(sorted.size());
  for (const uint16_t vendorId : sorted) {
    formatted.append(QStringLiteral("0x%1").arg(vendorId, 4, 16, QLatin1Char('0')));
  }

  return formatted.join(QStringLiteral(", "));
}

static int getBoundedConfigValue(const QString& key, int defaultValue, int minValue, int maxValue) {
  bool ok = false;
  const int configuredValue = ConfigService::instance().get(key, defaultValue).toInt(&ok);
  if (!ok) {
    return defaultValue;
  }

  return std::clamp(configuredValue, minValue, maxValue);
}

enum class AAStartupProfile {
  Resilient,
  CompatOpenAuto,
};

static auto aaStartupProfileToString(AAStartupProfile profile) -> QString {
  switch (profile) {
    case AAStartupProfile::CompatOpenAuto:
      return QStringLiteral("compat_openauto_profile");
    case AAStartupProfile::Resilient:
    default:
      return QStringLiteral("resilient_profile");
  }
}

static auto resolveAAStartupProfile() -> AAStartupProfile {
  const QString configuredProfile = ConfigService::instance()
                                        .get("core.android_auto.startup_profile", QString())
                                        .toString()
                                        .trimmed()
                                        .toLower();

  if (configuredProfile == QStringLiteral("compat_openauto_profile") ||
      configuredProfile == QStringLiteral("compat_openauto") ||
      configuredProfile == QStringLiteral("openauto_compat") ||
      configuredProfile == QStringLiteral("openauto")) {
    return AAStartupProfile::CompatOpenAuto;
  }

  if (configuredProfile == QStringLiteral("resilient_profile") ||
      configuredProfile == QStringLiteral("resilient") || configuredProfile.isEmpty()) {
    const bool legacyCompatFlag =
        ConfigService::instance().get("core.android_auto.compat_openauto_profile", false).toBool();
    return legacyCompatFlag ? AAStartupProfile::CompatOpenAuto : AAStartupProfile::Resilient;
  }

  Logger::instance().warning(
      QString("[RealAndroidAutoService] Unknown startup profile '%1', using resilient_profile")
          .arg(configuredProfile));
  return AAStartupProfile::Resilient;
}

static auto isCompatOpenAutoProfileEnabled() -> bool {
  return resolveAAStartupProfile() == AAStartupProfile::CompatOpenAuto;
}

static auto isResilientStartupProfileEnabled() -> bool {
  return resolveAAStartupProfile() == AAStartupProfile::Resilient;
}

static auto shouldDeferInitialChannelReceiveUntilServiceDiscovery() -> bool {
  if (isCompatOpenAutoProfileEnabled()) {
    return false;
  }

  return ConfigService::instance()
      .get("core.android_auto.channels.defer_initial_receive_until_service_discovery", true)
      .toBool();
}

static auto shouldDeferNonControlReceiveUntilControlReady() -> bool {
  if (isCompatOpenAutoProfileEnabled()) {
    return false;
  }

  return ConfigService::instance()
      .get("core.android_auto.channels.defer_non_control_receive_until_control_ready", false)
      .toBool();
}

static auto shouldAllowOptionalChannelsBeforePrimaryStart() -> bool {
  if (isCompatOpenAutoProfileEnabled()) {
    return true;
  }

  return ConfigService::instance()
      .get("core.android_auto.channels.allow_optional_before_primary_start", true)
      .toBool();
}

static auto getInitialMicrophoneReceiveDelayMs() -> int {
  if (isCompatOpenAutoProfileEnabled()) {
    return 0;
  }

  return getBoundedConfigValue("core.android_auto.microphone.initial_receive_delay_ms", 1200, 0,
                               10000);
}

static auto isProjectionIdleReconnectEnabled() -> bool {
  if (isCompatOpenAutoProfileEnabled()) {
    return false;
  }

  return ConfigService::instance()
      .get("core.android_auto.projection.idle_reconnect_enabled", true)
      .toBool();
}

static auto parseLoggerLevel(const QString& value, bool* valid = nullptr) -> Logger::Level {
  const QString normalised = value.trimmed().toLower();
  if (normalised == "trace" || normalised == "debug") {
    if (valid != nullptr) {
      *valid = true;
    }
    return Logger::Level::Debug;
  }
  if (normalised == "info") {
    if (valid != nullptr) {
      *valid = true;
    }
    return Logger::Level::Info;
  }
  if (normalised == "warning" || normalised == "warn") {
    if (valid != nullptr) {
      *valid = true;
    }
    return Logger::Level::Warning;
  }
  if (normalised == "error") {
    if (valid != nullptr) {
      *valid = true;
    }
    return Logger::Level::Error;
  }
  if (normalised == "fatal") {
    if (valid != nullptr) {
      *valid = true;
    }
    return Logger::Level::Fatal;
  }

  if (valid != nullptr) {
    *valid = false;
  }
  return Logger::Level::Info;
}

static auto toAasdkLogLevel(Logger::Level level) -> aasdk::common::LogLevel {
  switch (level) {
    case Logger::Level::Debug:
      return aasdk::common::LogLevel::DEBUG;
    case Logger::Level::Info:
      return aasdk::common::LogLevel::INFO;
    case Logger::Level::Warning:
      return aasdk::common::LogLevel::WARN;
    case Logger::Level::Error:
      return aasdk::common::LogLevel::ERROR;
    case Logger::Level::Fatal:
      return aasdk::common::LogLevel::FATAL;
    default:
      return aasdk::common::LogLevel::INFO;
  }
}

static auto resolveSettingString(const QMap<QString, QVariant>& settings,
                                 const QString& deviceSettingKey, const QString& serviceConfigKey,
                                 const QString& legacyServiceConfigKey,
                                 const QString& coreFallbackKey, const QString& defaultValue)
    -> QString {
  if (settings.contains(deviceSettingKey)) {
    const QString value = settings.value(deviceSettingKey).toString().trimmed();
    if (!value.isEmpty()) {
      return value;
    }
  }

  const QString serviceValue =
      ConfigService::instance().get(serviceConfigKey, QString()).toString().trimmed();
  if (!serviceValue.isEmpty()) {
    return serviceValue;
  }

  const QString legacyServiceValue =
      ConfigService::instance().get(legacyServiceConfigKey, QString()).toString().trimmed();
  if (!legacyServiceValue.isEmpty()) {
    return legacyServiceValue;
  }

  const QString coreFallbackValue =
      ConfigService::instance().get(coreFallbackKey, defaultValue).toString().trimmed();
  if (!coreFallbackValue.isEmpty()) {
    return coreFallbackValue;
  }

  return defaultValue;
}

static auto resolveSettingBool(const QMap<QString, QVariant>& settings,
                               const QString& deviceSettingKey, const QString& serviceConfigKey,
                               const QString& legacyServiceConfigKey, bool defaultValue) -> bool {
  if (settings.contains(deviceSettingKey)) {
    return settings.value(deviceSettingKey).toBool();
  }

  const QVariant serviceValue = ConfigService::instance().get(serviceConfigKey, QVariant());
  if (serviceValue.isValid()) {
    return serviceValue.toBool();
  }

  const QVariant legacyServiceValue =
      ConfigService::instance().get(legacyServiceConfigKey, QVariant());
  if (legacyServiceValue.isValid()) {
    return legacyServiceValue.toBool();
  }

  return defaultValue;
}

static void applyAndroidAutoLoggingConfig(const QMap<QString, QVariant>& settings) {
  const bool loggingEnabled =
      resolveSettingBool(settings, "logging.enabled", "core.services.android_auto.logging.enabled",
                         "core.android_auto.logging.enabled", true);

  const QString configuredLevel =
      resolveSettingString(settings, "logging.level", "core.services.android_auto.logging.level",
                           "core.android_auto.logging.level", "core.logging.level", "info");

  bool levelValid = false;
  const Logger::Level parsedLevel = parseLoggerLevel(configuredLevel, &levelValid);
  if (!levelValid) {
    Logger::instance().warning(
        QString("[RealAndroidAutoService] Invalid logging.level '%1', using 'info'")
            .arg(configuredLevel));
  }

  const QString configuredLogFile = resolveSettingString(
      settings, "logging.file_path", "core.services.android_auto.logging.file_path",
      "core.android_auto.logging.file_path", "core.logging.file_path", QString());

  const QString configuredLogFileLegacy =
      resolveSettingString(settings, "logging.file", "core.services.android_auto.logging.file",
                           "core.android_auto.logging.file", "core.logging.file", QString());

  QString logFilePath = configuredLogFile;
  if (logFilePath.isEmpty()) {
    logFilePath = configuredLogFileLegacy;
  }

  const bool verboseUsb = resolveSettingBool(settings, "logging.verbose_usb",
                                             "core.services.android_auto.logging.verbose_usb",
                                             "core.android_auto.logging.verbose_usb", false);
  const bool channelDebugTelemetry = resolveSettingBool(
      settings, "logging.channel_debug", "core.services.android_auto.logging.channel_debug",
      "core.android_auto.logging.channel_debug", true);
  const bool traceMessages = resolveSettingBool(settings, "logging.trace_messages",
                                                "core.services.android_auto.logging.trace_messages",
                                                "core.android_auto.logging.trace_messages", false);
  const bool traceMessagesVideoOnly =
      resolveSettingBool(settings, "logging.trace_messages_video_only",
                         "core.services.android_auto.logging.trace_messages_video_only",
                         "core.android_auto.logging.trace_messages_video_only", true);
  const int traceMessagesSampleEvery =
      getBoundedConfigValue("core.android_auto.logging.trace_messages_sample_every", 1, 1, 1000);

  g_channelDebugTelemetryEnabled.store(channelDebugTelemetry);
  qputenv("AASDK_VERBOSE_USB", verboseUsb ? "1" : "0");
  qputenv("AASDK_TRACE_MESSAGE", traceMessages ? "1" : "0");
  qputenv("AASDK_TRACE_MESSAGE_VIDEO_ONLY", traceMessagesVideoOnly ? "1" : "0");
  qputenv("AASDK_TRACE_MESSAGE_SAMPLE_EVERY", QByteArray::number(traceMessagesSampleEvery));

  aasdk::common::LogLevel aasdkLogLevel = toAasdkLogLevel(parsedLevel);
  if (traceMessages && aasdkLogLevel > aasdk::common::LogLevel::DEBUG) {
    aasdkLogLevel = aasdk::common::LogLevel::DEBUG;
  }
  aasdk::common::ModernLogger::getInstance().setLevel(aasdkLogLevel);

  if (!loggingEnabled) {
    Logger::instance().warning(
        "[RealAndroidAutoService] Android Auto logging disabled via configuration");
    return;
  }

  Logger::instance().setLevel(parsedLevel);

  if (!logFilePath.isEmpty()) {
    QFileInfo logFileInfo(logFilePath);
    if (!logFileInfo.absolutePath().isEmpty()) {
      QDir logDir;
      if (!logDir.exists(logFileInfo.absolutePath())) {
        logDir.mkpath(logFileInfo.absolutePath());
      }
    }
    Logger::instance().setLogFile(logFilePath);
  }

  Logger::instance().info(
      QString("[RealAndroidAutoService] Applied logging config: level=%1, file=%2, verbose_usb=%3, "
              "channel_debug=%4, trace_messages=%5, trace_video_only=%6, trace_sample_every=%7")
          .arg(configuredLevel)
          .arg(logFilePath.isEmpty() ? "<default>" : logFilePath)
          .arg(verboseUsb ? "true" : "false")
          .arg(channelDebugTelemetry ? "true" : "false")
          .arg(traceMessages ? "true" : "false")
          .arg(traceMessagesVideoOnly ? "true" : "false")
          .arg(traceMessagesSampleEvery));
}

static auto resolveAndroidAutoChannelConfig(const QMap<QString, QVariant>& settings,
                                            const RealAndroidAutoService::ChannelConfig& baseConfig)
    -> RealAndroidAutoService::ChannelConfig {
  RealAndroidAutoService::ChannelConfig resolved = baseConfig;

  const bool allChannelsEnabled = resolveSettingBool(
      settings, "channels.all_enabled", "core.services.android_auto.channels.all_enabled",
      "core.android_auto.channels.all_enabled", true);

  const bool defaultWhenMissing = allChannelsEnabled;
  const bool compatOpenAutoProfile = isCompatOpenAutoProfileEnabled();

  resolved.videoEnabled = resolveSettingBool(
      settings, "channels.video_enabled", "core.services.android_auto.channels.video_enabled",
      "core.android_auto.channels.video_enabled", defaultWhenMissing);
  resolved.mediaAudioEnabled =
      resolveSettingBool(settings, "channels.media_audio_enabled",
                         "core.services.android_auto.channels.media_audio_enabled",
                         "core.android_auto.channels.media_audio_enabled", defaultWhenMissing);
  resolved.systemAudioEnabled =
      resolveSettingBool(settings, "channels.system_audio_enabled",
                         "core.services.android_auto.channels.system_audio_enabled",
                         "core.android_auto.channels.system_audio_enabled", defaultWhenMissing);
  resolved.speechAudioEnabled =
      resolveSettingBool(settings, "channels.speech_audio_enabled",
                         "core.services.android_auto.channels.speech_audio_enabled",
                         "core.android_auto.channels.speech_audio_enabled", defaultWhenMissing);
  resolved.telephonyAudioEnabled = resolveSettingBool(
      settings, "channels.telephony_audio_enabled",
      "core.services.android_auto.channels.telephony_audio_enabled",
      "core.android_auto.channels.telephony_audio_enabled", compatOpenAutoProfile ? false : false);
  resolved.microphoneEnabled =
      resolveSettingBool(settings, "channels.microphone_enabled",
                         "core.services.android_auto.channels.microphone_enabled",
                         "core.android_auto.channels.microphone_enabled", defaultWhenMissing);
  resolved.inputEnabled = resolveSettingBool(
      settings, "channels.input_enabled", "core.services.android_auto.channels.input_enabled",
      "core.android_auto.channels.input_enabled", defaultWhenMissing);
  resolved.sensorEnabled = resolveSettingBool(
      settings, "channels.sensor_enabled", "core.services.android_auto.channels.sensor_enabled",
      "core.android_auto.channels.sensor_enabled", defaultWhenMissing);
  resolved.bluetoothEnabled =
      resolveSettingBool(settings, "channels.bluetooth_enabled",
                         "core.services.android_auto.channels.bluetooth_enabled",
                         "core.android_auto.channels.bluetooth_enabled", defaultWhenMissing);

  // Keep channel toggles explicit: do not auto-enable optional channels when
  // video is enabled. This allows targeted compatibility profiles (minimal
  // channel sets) for troubleshooting startup/interoperability issues.

  Logger::instance().info(QString("[RealAndroidAutoService] Resolved channel config "
                                  "(all_enabled=%1, startup_profile=%2): %3")
                              .arg(allChannelsEnabled ? "true" : "false")
                              .arg(aaStartupProfileToString(resolveAAStartupProfile()))
                              .arg(describeChannelConfig(resolved)));

  if (!resolved.telephonyAudioEnabled) {
    Logger::instance().info(
        QString("[RealAndroidAutoService] Telephony audio advertisement disabled; "
                "OpenAuto-compatible default retained unless explicitly enabled"));
  }

  return resolved;
}

static auto selectVideoResolution(const QSize& resolution)
    -> aap_protobuf::service::media::sink::message::VideoCodecResolutionType {
  using ResolutionType = aap_protobuf::service::media::sink::message::VideoCodecResolutionType;
  if (resolution.width() >= 1920 || resolution.height() >= 1080) {
    return ResolutionType::VIDEO_1920x1080;
  }

  if (resolution.width() >= 1280 || resolution.height() >= 720) {
    return ResolutionType::VIDEO_1280x720;
  }

  return ResolutionType::VIDEO_800x480;
}

static auto selectVideoFrameRate(int fps)
    -> aap_protobuf::service::media::sink::message::VideoFrameRateType {
  using FrameRateType = aap_protobuf::service::media::sink::message::VideoFrameRateType;
  return fps >= 60 ? FrameRateType::VIDEO_FPS_60 : FrameRateType::VIDEO_FPS_30;
}

static void appendVideoSinkFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::mediasink::video::channel::VideoChannel>& videoChannel,
    const QSize& resolution, int fps) {
  if (!videoChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(videoChannel->getId()));

  auto* mediaSink = service->mutable_media_sink_service();
  mediaSink->set_available_type(
      aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
  mediaSink->set_available_while_in_call(true);

  auto* videoConfig = mediaSink->add_video_configs();
  videoConfig->set_codec_resolution(selectVideoResolution(resolution));
  videoConfig->set_frame_rate(selectVideoFrameRate(fps));
  videoConfig->set_width_margin(0);
  videoConfig->set_height_margin(0);
  videoConfig->set_density(160);
}

static void appendAudioSinkFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::mediasink::audio::IAudioMediaSinkService>& channel,
    aap_protobuf::service::media::sink::message::AudioStreamType audioType, uint32_t sampleRate,
    uint32_t channels) {
  if (!channel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(channel->getId()));

  auto* mediaSink = service->mutable_media_sink_service();
  mediaSink->set_available_type(
      aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
  mediaSink->set_audio_type(audioType);
  mediaSink->set_available_while_in_call(true);

  auto* audioConfig = mediaSink->add_audio_configs();
  audioConfig->set_sampling_rate(sampleRate);
  audioConfig->set_number_of_bits(16);
  audioConfig->set_number_of_channels(channels);
}

static void appendInputSourceFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::inputsource::InputSourceService>& inputChannel,
    const QSize& resolution) {
  if (!inputChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(inputChannel->getId()));
  auto* inputService = service->mutable_input_source_service();

  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_HOME);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_BACK);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_DPAD_UP);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_DPAD_DOWN);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_DPAD_LEFT);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_DPAD_RIGHT);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_DPAD_CENTER);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_CALL);
  inputService->add_keycodes_supported(
      aap_protobuf::service::media::sink::message::KeyCode::KEYCODE_ENDCALL);

    // Advertise a broad keyboard set so AA can accept physical keyboard typing.
    const std::initializer_list<int> keyboardCodes = {
      7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  // 0-9
      29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  // A-J
      39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  // K-T
      49,  50,  51,  52,  53,  54,                        // U-Z
      55,  56,  61,  62,  66,  67,  68,  69,  70,  71,
      72,  73,  74,  75,  76,  81,  111};

    for (const int code : keyboardCodes) {
    inputService->add_keycodes_supported(
      static_cast<aap_protobuf::service::media::sink::message::KeyCode>(code));
    }

  auto* touchscreen = inputService->add_touchscreen();
  touchscreen->set_width(static_cast<uint32_t>(resolution.width()));
  touchscreen->set_height(static_cast<uint32_t>(resolution.height()));
}

static void appendSensorSourceFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::sensorsource::SensorSourceService>& sensorChannel) {
  if (!sensorChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(sensorChannel->getId()));
  auto* sensorService = service->mutable_sensor_source_service();
  sensorService->add_sensors()->set_sensor_type(
      aap_protobuf::service::sensorsource::message::SensorType::SENSOR_DRIVING_STATUS_DATA);
  sensorService->add_sensors()->set_sensor_type(
      aap_protobuf::service::sensorsource::message::SensorType::SENSOR_LOCATION);
  sensorService->add_sensors()->set_sensor_type(
      aap_protobuf::service::sensorsource::message::SensorType::SENSOR_NIGHT_MODE);
}

static void appendBluetoothFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::bluetooth::BluetoothService>& bluetoothChannel) {
  if (!bluetoothChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(bluetoothChannel->getId()));
  auto* bluetoothService = service->mutable_bluetooth_service();
  bluetoothService->set_car_address("00:11:22:33:44:55");
  bluetoothService->add_supported_pairing_methods(
      aap_protobuf::service::bluetooth::message::BLUETOOTH_PAIRING_NUMERIC_COMPARISON);
}

static void appendWifiProjectionFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::wifiprojection::IWifiProjectionService>&
        wifiProjectionChannel,
    const QString& hotspotBssid) {
  if (!wifiProjectionChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(wifiProjectionChannel->getId()));
  auto* wifiProjectionService = service->mutable_wifi_projection_service();
  if (!hotspotBssid.isEmpty()) {
    wifiProjectionService->set_car_wifi_bssid(hotspotBssid.toStdString());
  }
}

static void appendMicrophoneSourceFeature(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response,
    const std::shared_ptr<aasdk::channel::mediasource::audio::MicrophoneAudioChannel>&
        microphoneChannel) {
  if (!microphoneChannel) {
    return;
  }

  auto* service = response.add_channels();
  service->set_id(static_cast<uint32_t>(microphoneChannel->getId()));

  auto* mediaSource = service->mutable_media_source_service();
  mediaSource->set_available_type(
      aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
  auto* audioConfig = mediaSource->mutable_audio_config();
  audioConfig->set_sampling_rate(16000);
  audioConfig->set_number_of_bits(16);
  audioConfig->set_number_of_channels(1);
}

static auto isRecoverableUsbTransferTimeout(const aasdk::error::Error& error) -> bool {
  static constexpr uint32_t kLibusbTransferTimedOut = 2;
  static constexpr uint32_t kLibusbTransferInterrupted = 4294967292u;  // -4
  return error.getCode() == aasdk::error::ErrorCode::USB_TRANSFER &&
         (error.getNativeCode() == kLibusbTransferTimedOut ||
          error.getNativeCode() == kLibusbTransferInterrupted);
}

static auto isRecoverableUsbReceiveError(const aasdk::error::Error& error) -> bool {
  static constexpr uint32_t kLibusbTransferError = 1;
  static constexpr uint32_t kLibusbTransferTimedOut = 2;
  static constexpr uint32_t kLibusbTransferInterrupted = 4294967292u;  // -4
  return error.getCode() == aasdk::error::ErrorCode::USB_TRANSFER &&
         (error.getNativeCode() == kLibusbTransferError ||
          error.getNativeCode() == kLibusbTransferTimedOut ||
          error.getNativeCode() == kLibusbTransferInterrupted);
}

static auto isOperationInProgressError(const aasdk::error::Error& error) -> bool {
  return error.getCode() == aasdk::error::ErrorCode::OPERATION_IN_PROGRESS;
}

static auto isUsbTransferErrorText(const QString& errorText, uint32_t nativeCode) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 10")) &&
         errorText.contains(QStringLiteral("Native Code: %1").arg(nativeCode));
}

static auto isUsbTransferTimeoutErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kLibusbTransferTimedOut = 2;
  return isUsbTransferErrorText(errorText, kLibusbTransferTimedOut);
}

static auto isUsbTransferNoDeviceErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kLibusbTransferNoDevice = 5;
  return isUsbTransferErrorText(errorText, kLibusbTransferNoDevice);
}

static auto isTransportNoDeviceErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kNativeNoDevice = 5;

  if (!errorText.contains(QStringLiteral("Native Code: %1").arg(kNativeNoDevice))) {
    return false;
  }

  return errorText.contains(QStringLiteral("AASDK Error: 10")) ||
         errorText.contains(QStringLiteral("AASDK Error: 25")) ||
         errorText.contains(QStringLiteral("AASDK Error: 26")) ||
         errorText.contains(QStringLiteral("AASDK Error: 27")) ||
         errorText.contains(QStringLiteral("AASDK Error: 28")) ||
         errorText.contains(QStringLiteral("AASDK Error: 33"));
}

static auto isSslWrapperNoDeviceErrorText(const QString& errorText) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 25")) &&
         errorText.contains(QStringLiteral("Native Code: 5"));
}

static auto isOperationAbortedErrorText(const QString& errorText) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 30"));
}

// Helper: perform USBDEVFS_RESET ioctl on a device node
static bool resetUsbDeviceNode(const QString& devNode, QString* errorOut = nullptr) {
  int fd = open(devNode.toStdString().c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fd = open(devNode.toStdString().c_str(), O_WRONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    if (errorOut) *errorOut = QString("open failed: %1").arg(strerror(errno));
    return false;
  }
  int rc = ioctl(fd, USBDEVFS_RESET, 0);
  if (rc < 0) {
    if (errorOut) *errorOut = QString("ioctl failed: %1").arg(strerror(errno));
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

// Helper: find device node path for a libusb_device
static QString findDeviceNodeForLibusb(libusb_device* dev) {
  if (!dev) return QString();
  // Get bus and address
  uint8_t bus = libusb_get_bus_number(dev);
  uint8_t addr = libusb_get_device_address(dev);
  // Try /dev/bus/usb/BBB/DDD
  QString path =
      QString("/dev/bus/usb/%1/%2").arg(bus, 3, 10, QChar('0')).arg(addr, 3, 10, QChar('0'));
  if (QFileInfo::exists(path)) return path;
  // Try /dev/usbdevBBB.DDD
  path = QString("/dev/usbdev%1.%2").arg(bus, 3, 10, QChar('0')).arg(addr, 3, 10, QChar('0'));
  if (QFileInfo::exists(path)) return path;
  return QString();
}

static auto formatUsbDescriptorSummary(const libusb_device_descriptor& desc, libusb_device* device)
    -> QString {
  const uint8_t busNumber = device ? libusb_get_bus_number(device) : 0;
  const uint8_t deviceAddress = device ? libusb_get_device_address(device) : 0;

  return QString("vid=0x%1 pid=0x%2 bus=%3 addr=%4 class=0x%5 subclass=0x%6 proto=0x%7 cfg=%8")
      .arg(QString::asprintf("%04x", desc.idVendor))
      .arg(QString::asprintf("%04x", desc.idProduct))
      .arg(busNumber)
      .arg(deviceAddress)
      .arg(QString::asprintf("%02x", desc.bDeviceClass))
      .arg(QString::asprintf("%02x", desc.bDeviceSubClass))
      .arg(QString::asprintf("%02x", desc.bDeviceProtocol))
      .arg(desc.bNumConfigurations);
}

static auto makeAoapTraceToken(int checkCount, int candidateIndex, int nextAttempt,
                               const libusb_device_descriptor& desc, libusb_device* device)
    -> QString {
  static std::atomic<uint64_t> traceCounter{1};

  const uint64_t traceId = traceCounter.fetch_add(1, std::memory_order_relaxed);
  const uint8_t busNumber = device ? libusb_get_bus_number(device) : 0;
  const uint8_t deviceAddress = device ? libusb_get_device_address(device) : 0;

  return QString("trace=%1 check=%2 candidate=%3 nextAttempt=%4 vid=0x%5 pid=0x%6 bus=%7 addr=%8")
      .arg(traceId)
      .arg(checkCount)
      .arg(candidateIndex)
      .arg(nextAttempt)
      .arg(QString::asprintf("%04x", desc.idVendor))
      .arg(QString::asprintf("%04x", desc.idProduct))
      .arg(busNumber)
      .arg(deviceAddress);
}

static auto hasAndroidLikeUsbInterface(libusb_device* device) -> bool {
  if (!device) {
    return false;
  }

  libusb_config_descriptor* configDescriptor = nullptr;
  int descriptorResult = libusb_get_active_config_descriptor(device, &configDescriptor);
  if (descriptorResult != 0 || configDescriptor == nullptr) {
    descriptorResult = libusb_get_config_descriptor(device, 0, &configDescriptor);
    if (descriptorResult != 0 || configDescriptor == nullptr) {
      return false;
    }
  }

  bool hasAndroidLikeInterface = false;

  for (uint8_t ifaceIndex = 0; ifaceIndex < configDescriptor->bNumInterfaces; ++ifaceIndex) {
    const libusb_interface& iface = configDescriptor->interface[ifaceIndex];
    for (int altIndex = 0; altIndex < iface.num_altsetting; ++altIndex) {
      const libusb_interface_descriptor& alt = iface.altsetting[altIndex];

      const bool isAdb = alt.bInterfaceClass == 0xFF && alt.bInterfaceSubClass == 0x42 &&
                         (alt.bInterfaceProtocol == 0x01 || alt.bInterfaceProtocol == 0x02);
      const bool isMtpOrPtp = alt.bInterfaceClass == 0x06 &&
                              (alt.bInterfaceSubClass == 0x01 || alt.bInterfaceSubClass == 0x00);
      const bool isRndis = alt.bInterfaceClass == 0xE0 && alt.bInterfaceSubClass == 0x01 &&
                           alt.bInterfaceProtocol == 0x03;

      if (isAdb || isMtpOrPtp || isRndis) {
        hasAndroidLikeInterface = true;
        break;
      }
    }

    if (hasAndroidLikeInterface) {
      break;
    }
  }

  libusb_free_config_descriptor(configDescriptor);
  return hasAndroidLikeInterface;
}

static auto formatAasdkErrorDetails(const aasdk::error::Error& error) -> QString {
  return QString("code=%1 native=%2 signed_native=%3 what=%4")
      .arg(static_cast<int>(error.getCode()))
      .arg(error.getNativeCode())
      .arg(static_cast<qint32>(error.getNativeCode()))
      .arg(QString::fromStdString(error.what()));
}

class AAControlEventHandler final
    : public aasdk::channel::control::IControlServiceChannelEventHandler,
      public std::enable_shared_from_this<AAControlEventHandler> {
 public:
  explicit AAControlEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                         aap_protobuf::shared::MessageStatus status) override {
    if (!m_service || !m_service->m_controlChannel) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("version_response"),
                                 QStringLiteral("major=%1 minor=%2 status=%3")
                                     .arg(majorCode)
                                     .arg(minorCode)
                                     .arg(static_cast<int>(status)));

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 totalElapsedMs = (m_service->m_controlVersionFirstRequestMs > 0)
                                      ? (nowMs - m_service->m_controlVersionFirstRequestMs)
                                      : -1;
    const qint64 sinceLastRequestMs = (m_service->m_controlVersionLastRequestMs > 0)
                                          ? (nowMs - m_service->m_controlVersionLastRequestMs)
                                          : -1;

    m_service->m_controlVersionRequestAttempts = 0;
    m_service->m_controlVersionFirstRequestMs = 0;
    m_service->m_controlVersionLastRequestMs = 0;
    m_service->m_controlSendNative2ConsecutiveTimeouts = 0;
    m_service->m_controlTimeoutRecoveryCount = 0;

    aaLogInfo(
        "control",
        QString("Version response %1.%2 status=%3").arg(majorCode).arg(minorCode).arg(status));
    aaLogInfo("control",
              QString("Version response timing: since_last_request_ms=%1 total_elapsed_ms=%2")
                  .arg(sinceLastRequestMs)
                  .arg(totalElapsedMs));

    m_service->m_controlVersionReceived = true;
    m_service->publishProjectionStatus(QStringLiteral("control_version_response"));
    m_service->armNonControlReceivesAfterControlReady();

    if (status == aap_protobuf::shared::MessageStatus::STATUS_NO_COMPATIBLE_VERSION) {
      m_service->onChannelError(QStringLiteral("control"),
                                QStringLiteral("No compatible protocol version"));
      m_service->transitionToState(AndroidAutoService::ConnectionState::ERROR);
      return;
    }

    try {
      const bool handshakeComplete = m_service->m_cryptor->doHandshake();

      auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
      promise->then([]() {},
                    [self = shared_from_this()](const aasdk::error::Error& error) {
                      self->onChannelError(error);
                    });

      if (!handshakeComplete) {
        m_service->traceControlEvent(QStringLiteral("handshake_tx_initial"));
        m_service->m_controlChannel->sendHandshake(m_service->m_cryptor->readHandshakeBuffer(),
                                                   std::move(promise));
      } else {
        m_service->traceControlEvent(QStringLiteral("auth_complete_tx"));
        aap_protobuf::service::control::message::AuthResponse authResponse;
        authResponse.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);
        m_service->m_controlChannel->sendAuthComplete(authResponse, std::move(promise));
      }

      const int handshakeActivationTimeoutMs = getBoundedConfigValue(
          "core.android_auto.control.handshake_activation_timeout_ms", 4000, 500, 20000);
      const int handshakeActivationMaxRetries =
          getBoundedConfigValue("core.android_auto.control.handshake_activation_retries", 1, 0, 1);
      m_service->m_controlHandshakeAwaitingActivation = true;
      m_service->m_controlHandshakeStartedMs = QDateTime::currentMSecsSinceEpoch();
      const quint64 handshakeEpoch = ++m_service->m_controlHandshakeEpoch;
      aaLogInfo("control", QString("Handshake activation watchdog armed timeout_ms=%1 epoch=%2")
                               .arg(handshakeActivationTimeoutMs)
                               .arg(handshakeEpoch));

      QTimer::singleShot(
          handshakeActivationTimeoutMs, m_service,
          [service = m_service, handshakeEpoch, handshakeActivationTimeoutMs,
           handshakeActivationMaxRetries]() {
            if (!service) {
              return;
            }

            if (!service->m_controlHandshakeAwaitingActivation ||
                service->m_controlHandshakeEpoch != handshakeEpoch) {
              return;
            }

            if (service->m_serviceDiscoveryCompleted) {
              return;
            }

            if (service->m_state != AndroidAutoService::ConnectionState::CONNECTED &&
                service->m_state != AndroidAutoService::ConnectionState::CONNECTING) {
              return;
            }

            const qint64 elapsedMs =
                service->m_controlHandshakeStartedMs > 0
                    ? (QDateTime::currentMSecsSinceEpoch() - service->m_controlHandshakeStartedMs)
                    : handshakeActivationTimeoutMs;

            service->traceControlEvent(
                QStringLiteral("handshake_watchdog_timeout"),
                QStringLiteral("elapsed_ms=%1 epoch=%2").arg(elapsedMs).arg(handshakeEpoch));

            aaLogWarning("control",
                         QString("Handshake activation timeout elapsed_ms=%1 epoch=%2 state=%3")
                             .arg(elapsedMs)
                             .arg(handshakeEpoch)
                             .arg(connectionStateToString(service->m_state)));

            if (service->m_controlHandshakeActivationRetryCount < handshakeActivationMaxRetries) {
              service->m_controlHandshakeActivationRetryCount++;
              const int retry = service->m_controlHandshakeActivationRetryCount;
              service->traceControlEvent(QStringLiteral("handshake_watchdog_rearm"),
                                         QStringLiteral("retry=%1/%2 timeout_ms=%3")
                                             .arg(retry)
                                             .arg(handshakeActivationMaxRetries)
                                             .arg(handshakeActivationTimeoutMs));

              aaLogInfo("control",
                        QString("Handshake activation timeout re-arm retry=%1/%2 timeout_ms=%3")
                            .arg(retry)
                            .arg(handshakeActivationMaxRetries)
                            .arg(handshakeActivationTimeoutMs));

              if (service->m_controlChannel && service->m_cryptor && service->m_strand) {
                try {
                  auto retryPromise = aasdk::channel::SendPromise::defer(*service->m_strand);
                  retryPromise->then([]() {},
                                     [weakService = QPointer<RealAndroidAutoService>(service)](
                                         const aasdk::error::Error& error) {
                                       if (!weakService) {
                                         return;
                                       }
                                       weakService->onChannelError(
                                           QStringLiteral("control"),
                                           QStringLiteral("Handshake retry send failed: %1")
                                               .arg(QString::fromStdString(error.what())));
                                     });

                  service->m_controlChannel->sendHandshake(
                      service->m_cryptor->readHandshakeBuffer(), std::move(retryPromise));
                  service->traceControlEvent(QStringLiteral("handshake_watchdog_rearm_tx"),
                                             QStringLiteral("retry=%1").arg(retry));
                  aaLogInfo("control", QString("Handshake retry transmitted retry=%1").arg(retry));
                } catch (const aasdk::error::Error& retryError) {
                  service->m_controlHandshakeAwaitingActivation = false;
                  service->onChannelError(QStringLiteral("control"),
                                          QStringLiteral("Handshake retry failed: %1")
                                              .arg(QString::fromStdString(retryError.what())));
                  return;
                }
              }

              service->m_controlHandshakeStartedMs = QDateTime::currentMSecsSinceEpoch();
              const quint64 nextHandshakeEpoch = ++service->m_controlHandshakeEpoch;
              QTimer::singleShot(
                  handshakeActivationTimeoutMs, service,
                  [service, nextHandshakeEpoch, handshakeActivationTimeoutMs]() {
                    if (!service) {
                      return;
                    }

                    if (!service->m_controlHandshakeAwaitingActivation ||
                        service->m_controlHandshakeEpoch != nextHandshakeEpoch) {
                      return;
                    }

                    if (service->m_serviceDiscoveryCompleted) {
                      return;
                    }

                    if (service->m_state != AndroidAutoService::ConnectionState::CONNECTED &&
                        service->m_state != AndroidAutoService::ConnectionState::CONNECTING) {
                      return;
                    }

                    const qint64 retryElapsedMs = service->m_controlHandshakeStartedMs > 0
                                                      ? (QDateTime::currentMSecsSinceEpoch() -
                                                         service->m_controlHandshakeStartedMs)
                                                      : handshakeActivationTimeoutMs;

                    service->traceControlEvent(QStringLiteral("handshake_watchdog_timeout"),
                                               QStringLiteral("elapsed_ms=%1 epoch=%2")
                                                   .arg(retryElapsedMs)
                                                   .arg(nextHandshakeEpoch));

                    aaLogWarning(
                        "control",
                        QString("Handshake activation timeout elapsed_ms=%1 epoch=%2 state=%3")
                            .arg(retryElapsedMs)
                            .arg(nextHandshakeEpoch)
                            .arg(connectionStateToString(service->m_state)));
                    service->m_controlHandshakeAwaitingActivation = false;
                    service->onChannelError(
                        QStringLiteral("control"),
                        QStringLiteral("Handshake activation timed out after %1 ms")
                            .arg(retryElapsedMs));
                  });
              return;
            }

            service->m_controlHandshakeAwaitingActivation = false;
            service->onChannelError(
                QStringLiteral("control"),
                QStringLiteral("Handshake activation timed out after %1 ms").arg(elapsedMs));
          });

    } catch (const aasdk::error::Error& error) {
      onChannelError(error);
      return;
    }

    armReceive();
  }

  void onHandshake(const aasdk::common::DataConstBuffer& payload) override {
    if (!m_service || !m_service->m_controlChannel || !m_service->m_cryptor) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("handshake_rx"),
                                 QStringLiteral("bytes=%1").arg(payload.size));

    if (m_service->m_controlHandshakeAwaitingActivation) {
      const qint64 elapsedMs =
          m_service->m_controlHandshakeStartedMs > 0
              ? (QDateTime::currentMSecsSinceEpoch() - m_service->m_controlHandshakeStartedMs)
              : -1;
      m_service->m_controlHandshakeAwaitingActivation = false;
      m_service->m_controlHandshakeActivationRetryCount = 0;
      aaLogInfo("control", QString("Handshake activation observed elapsed_ms=%1").arg(elapsedMs));
      m_service->traceControlEvent(
          QStringLiteral("handshake_watchdog_cleared"),
          QStringLiteral("reason=handshake_rx elapsed_ms=%1").arg(elapsedMs));
    }

    try {
      m_service->m_cryptor->writeHandshakeBuffer(payload);

      if (!m_service->m_cryptor->doHandshake()) {
        m_service->traceControlEvent(QStringLiteral("handshake_tx_continue"));
        auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
        promise->then([]() {},
                      [self = shared_from_this()](const aasdk::error::Error& error) {
                        self->onChannelError(error);
                      });

        m_service->m_controlChannel->sendHandshake(m_service->m_cryptor->readHandshakeBuffer(),
                                                   std::move(promise));
      } else {
        m_service->traceControlEvent(QStringLiteral("auth_complete_tx"));
        aap_protobuf::service::control::message::AuthResponse authResponse;
        authResponse.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

        auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
        promise->then([]() {},
                      [self = shared_from_this()](const aasdk::error::Error& error) {
                        self->onChannelError(error);
                      });

        m_service->m_controlChannel->sendAuthComplete(authResponse, std::move(promise));
      }
    } catch (const aasdk::error::Error& error) {
      onChannelError(error);
      return;
    }

    armReceive();
  }

  void onServiceDiscoveryRequest(
      const aap_protobuf::service::control::message::ServiceDiscoveryRequest& request) override {
    if (!m_service || !m_service->m_controlChannel) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("service_discovery_request"),
                                 QStringLiteral("label=%1 device=%2")
                                     .arg(QString::fromStdString(request.label_text()))
                                     .arg(QString::fromStdString(request.device_name())));

    aaLogInfo("control", QString("Service discovery request label=%1 device=%2")
                             .arg(QString::fromStdString(request.label_text()))
                             .arg(QString::fromStdString(request.device_name())));

    if (m_service->m_controlHandshakeAwaitingActivation) {
      const qint64 elapsedMs =
          m_service->m_controlHandshakeStartedMs > 0
              ? (QDateTime::currentMSecsSinceEpoch() - m_service->m_controlHandshakeStartedMs)
              : -1;
      m_service->m_controlHandshakeAwaitingActivation = false;
      m_service->m_controlHandshakeActivationRetryCount = 0;
      m_service->traceControlEvent(
          QStringLiteral("handshake_watchdog_cleared"),
          QStringLiteral("reason=service_discovery_request elapsed_ms=%1").arg(elapsedMs));
    }

    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    response.mutable_channels()->Reserve(256);
    response.set_driver_position(
        aap_protobuf::service::control::message::DriverPosition::DRIVER_POSITION_RIGHT);
    response.set_display_name("Crankshaft-NG");
    response.set_probe_for_support(false);

    auto* connectionConfiguration = response.mutable_connection_configuration();
    auto* pingConfiguration = connectionConfiguration->mutable_ping_configuration();
    pingConfiguration->set_tracked_ping_count(5);
    pingConfiguration->set_timeout_ms(3000);
    pingConfiguration->set_interval_ms(1000);
    pingConfiguration->set_high_latency_threshold_ms(200);

    auto* headUnitInfo = response.mutable_headunit_info();
    headUnitInfo->set_make("Crankshaft");
    headUnitInfo->set_model("Universal");
    headUnitInfo->set_year("2018");
    headUnitInfo->set_vehicle_id("2024110822150988");
    headUnitInfo->set_head_unit_make("f1x");
    headUnitInfo->set_head_unit_model("Crankshaft-NG Autoapp");
    headUnitInfo->set_head_unit_software_build("1");
    headUnitInfo->set_head_unit_software_version("1.0");

    appendVideoSinkFeature(response, m_service->m_videoChannel, m_service->m_resolution,
                           m_service->m_fps);
    appendAudioSinkFeature(response, m_service->m_mediaAudioChannel,
                           aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA, 48000,
                           2);
    appendAudioSinkFeature(response, m_service->m_systemAudioChannel,
                           aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO,
                           16000, 1);
    appendAudioSinkFeature(response, m_service->m_speechAudioChannel,
                           aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE,
                           16000, 1);
    appendAudioSinkFeature(response, m_service->m_telephonyAudioChannel,
                           aap_protobuf::service::media::sink::message::AUDIO_STREAM_TELEPHONY,
                           16000, 1);
    appendInputSourceFeature(response, m_service->m_inputChannel, m_service->m_resolution);
    appendSensorSourceFeature(response, m_service->m_sensorChannel);
    appendBluetoothFeature(response, m_service->m_bluetoothChannel);
    appendWifiProjectionFeature(response, m_service->m_wifiProjectionChannel,
                  m_service->m_wirelessHotspotBssid);
    appendMicrophoneSourceFeature(response, m_service->m_microphoneChannel);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_controlChannel->sendServiceDiscoveryResponse(response, std::move(promise));
    m_service->m_serviceDiscoveryCompleted = true;
    m_service->m_controlTimeoutRecoveryCount = 0;
    m_service->traceServiceDiscoveryResponse();
    m_service->scheduleVideoFocusKickAfterServiceDiscovery();

    m_service->publishProjectionStatus(QStringLiteral("service_discovery_response"));
    m_service->armDeferredChannelReceivesAfterServiceDiscovery();

    armReceive();
  }

  void onAudioFocusRequest(
      const aap_protobuf::service::control::message::AudioFocusRequest& request) override {
    if (!m_service || !m_service->m_controlChannel) {
      return;
    }

    m_service->traceControlEvent(
        QStringLiteral("audio_focus_request"),
        QStringLiteral("focus_type=%1").arg(static_cast<int>(request.audio_focus_type())));

    aap_protobuf::service::control::message::AudioFocusNotification response;
    using RequestType = aap_protobuf::service::control::message::AudioFocusRequestType;
    using StateType = aap_protobuf::service::control::message::AudioFocusStateType;

    StateType state = StateType::AUDIO_FOCUS_STATE_GAIN;
    const bool releaseMapsToGain =
        ConfigService::instance()
            .get("core.android_auto.control.audio_focus_release_maps_to_gain", false)
            .toBool();
    switch (request.audio_focus_type()) {
      case RequestType::AUDIO_FOCUS_GAIN:
        state = StateType::AUDIO_FOCUS_STATE_GAIN;
        break;
      case RequestType::AUDIO_FOCUS_GAIN_TRANSIENT:
        state = StateType::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
        break;
      case RequestType::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
        state = StateType::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
        break;
      case RequestType::AUDIO_FOCUS_RELEASE:
        state = releaseMapsToGain ? StateType::AUDIO_FOCUS_STATE_GAIN
                                  : StateType::AUDIO_FOCUS_STATE_LOSS;
        break;
      default:
        state = StateType::AUDIO_FOCUS_STATE_GAIN;
        break;
    }

    response.set_focus_state(state);
    aaLogInfo("control",
              QString("Audio focus request type=%1 -> state=%2 (release_maps_to_gain=%3)")
                  .arg(static_cast<int>(request.audio_focus_type()))
                  .arg(static_cast<int>(state))
                  .arg(releaseMapsToGain ? QStringLiteral("true") : QStringLiteral("false")));

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_controlChannel->sendAudioFocusResponse(response, std::move(promise));
    armReceive();
  }

  void onByeByeRequest(
      const aap_protobuf::service::control::message::ByeByeRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_controlChannel) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("byebye_request"));

    aap_protobuf::service::control::message::ByeByeResponse response;
    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_controlChannel->sendShutdownResponse(response, std::move(promise));
    m_service->handleConnectionLost();
  }

  void onByeByeResponse(
      const aap_protobuf::service::control::message::ByeByeResponse& response) override {
    Q_UNUSED(response)
    if (m_service) {
      m_service->traceControlEvent(QStringLiteral("byebye_response"));
      m_service->handleConnectionLost();
    }
  }

  void onBatteryStatusNotification(
      const aap_protobuf::service::control::message::BatteryStatusNotification& notification)
      override {
    Q_UNUSED(notification)
    if (m_service) {
      m_service->traceControlEvent(QStringLiteral("battery_status_notification"));
    }
    armReceive();
  }

  void onNavigationFocusRequest(
      const aap_protobuf::service::control::message::NavFocusRequestNotification& request)
      override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_controlChannel) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("nav_focus_request"));

    aap_protobuf::service::control::message::NavFocusNotification response;
    response.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_controlChannel->sendNavigationFocusResponse(response, std::move(promise));
    armReceive();
  }

  void onVoiceSessionRequest(
      const aap_protobuf::service::control::message::VoiceSessionNotification& request) override {
    Q_UNUSED(request)
    if (m_service) {
      m_service->traceControlEvent(QStringLiteral("voice_session_request"));
    }
    armReceive();
  }

  void onPingRequest(const aap_protobuf::service::control::message::PingRequest& request) override {
    Q_UNUSED(request)
    if (m_service) {
      m_service->traceControlEvent(QStringLiteral("ping_request"));
    }
    armReceive();
  }

  void onPingResponse(
      const aap_protobuf::service::control::message::PingResponse& response) override {
    Q_UNUSED(response)
    if (m_service) {
      m_service->traceControlEvent(QStringLiteral("ping_response"));
    }
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    m_service->traceControlEvent(QStringLiteral("control_handler_error"),
                                 formatAasdkErrorDetails(e));

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=control recoverable receive error (code=%1 native=%2), "
                         "scheduling receive re-arm")
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug(
          "control",
          QString(
              "Control receive already armed (operation in progress), keeping current receive"));
      return;
    }

    m_service->onChannelError(QStringLiteral("control"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (m_service && !m_service->m_aasdkTeardownInProgress && m_service->m_controlChannel) {
      const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
      const qint64 sinceLastRequestMs = (m_service->m_controlVersionLastRequestMs > 0)
                                            ? (nowMs - m_service->m_controlVersionLastRequestMs)
                                            : -1;
      const qint64 totalElapsedMs = (m_service->m_controlVersionFirstRequestMs > 0)
                                        ? (nowMs - m_service->m_controlVersionFirstRequestMs)
                                        : -1;
      aaLogDebug(
          "control",
          QString("Receive armed (attempt=%1/%2 since_last_request_ms=%3 total_elapsed_ms=%4)")
              .arg(m_service->m_controlVersionRequestAttempts)
              .arg(m_service->m_controlVersionRequestMaxAttempts)
              .arg(sinceLastRequestMs)
              .arg(totalElapsedMs));
      m_service->traceControlEvent(
          QStringLiteral("control_receive_armed"),
          QStringLiteral("attempt=%1/%2 since_last_ms=%3 total_elapsed_ms=%4")
              .arg(m_service->m_controlVersionRequestAttempts)
              .arg(m_service->m_controlVersionRequestMaxAttempts)
              .arg(sinceLastRequestMs)
              .arg(totalElapsedMs));
      m_service->m_controlChannel->receive(shared_from_this());
    }
  }

  QPointer<RealAndroidAutoService> m_service;
};

class AAVideoEventHandler final
    : public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler,
      public std::enable_shared_from_this<AAVideoEventHandler> {
 public:
  AAVideoEventHandler(
      RealAndroidAutoService* service,
      const std::shared_ptr<aasdk::channel::mediasink::video::channel::VideoChannel>& channel)
      : m_service(service), m_channel(channel) {}

  void sendProjectedFocusKick() {
    sendProjectedVideoFocusIndication(true);
  }

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    auto channel = m_channel.lock();
    if (!channel || !m_service) {
      return;
    }

    aaLogInfo("videoChannel", "Video channel open request received");

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    channel->sendChannelOpenResponse(response, std::move(promise));
    m_service->m_videoChannelOpened = true;
    m_service->publishProjectionStatus(QStringLiteral("video_channel_opened"));
    armReceive();
  }

  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request) override {
    auto channel = m_channel.lock();
    if (!channel || !m_service) {
      return;
    }

    aaLogInfo("videoChannel",
              QString("Setup request: codec_type=%1").arg(static_cast<int>(request.type())));

    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([self = shared_from_this()]() { self->sendProjectedVideoFocusIndication(false); },
                  [self = shared_from_this()](const aasdk::error::Error& error) {
                    self->onChannelError(error);
                  });

    channel->sendChannelSetupResponse(response, std::move(promise));
    m_service->m_videoConfigured = true;
    m_service->publishProjectionStatus(QStringLiteral("video_configured"));
    armReceive();
  }

  void onMediaChannelStartIndication(
      const aap_protobuf::service::media::shared::message::Start& indication) override {
    m_sessionId = indication.session_id();
    aaLogInfo("videoChannel", QString("Video start indication session_id=%1").arg(m_sessionId));
    if (m_service) {
      m_service->m_videoStarted = true;
      m_service->publishProjectionStatus(QStringLiteral("video_started"));
      m_service->armOptionalChannelReceivesAfterPrimaryStart();
    }
    armReceive();
  }

  void onMediaChannelStopIndication(
      const aap_protobuf::service::media::shared::message::Stop& indication) override {
    Q_UNUSED(indication)
    aaLogInfo("videoChannel",
              QString("Video stop indication session_id=%1 payload_count=%2 ack_count=%3")
                  .arg(m_sessionId)
                  .arg(m_payloadCount)
                  .arg(m_ackCount));
    m_sessionId = -1;
    if (m_service) {
      m_service->m_videoStarted = false;
      m_service->m_videoFrameReceived = false;
      m_service->publishProjectionStatus(QStringLiteral("video_stopped"));
    }
    armReceive();
  }

  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer& buffer) override {
    auto channel = m_channel.lock();
    if (!channel || !m_service) {
      return;
    }

    if (!m_firstPayloadLogged) {
      m_firstPayloadLogged = true;
      aaLogInfo("videoChannel",
                QString("First media payload received: bytes=%1 session_id=%2 timestamp=%3")
                    .arg(buffer.size)
                    .arg(m_sessionId)
                    .arg(static_cast<qulonglong>(timestamp)));
    }
    if (m_sessionId < 0) {
      aaLogWarning("videoChannel",
                   QString("payload received without active session bytes=%1 timestamp=%2")
                       .arg(buffer.size)
                       .arg(static_cast<qulonglong>(timestamp)));
    }
    m_payloadCount++;
    if ((m_payloadCount % 120) == 0) {
      aaLogDebug("videoChannel", QString("payload_count=%1 bytes=%2 session_id=%3 timestamp=%4")
                                     .arg(m_payloadCount)
                                     .arg(buffer.size)
                                     .arg(m_sessionId)
                                     .arg(static_cast<qulonglong>(timestamp)));
    }
    m_service->m_videoPayloadSeen = true;

    const QByteArray frameData(reinterpret_cast<const char*>(buffer.cdata),
                               static_cast<int>(buffer.size));
    m_service->onVideoChannelUpdate(frameData, m_service->m_resolution.width(),
                                    m_service->m_resolution.height());

    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(m_sessionId);
    ack.set_ack(1);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });
    channel->sendMediaAckIndication(ack, std::move(promise));
    m_ackCount++;
    if ((m_ackCount % 120) == 0) {
      aaLogDebug("videoChannel",
                 QString("ack_count=%1 session_id=%2").arg(m_ackCount).arg(m_sessionId));
    }
    armReceive();
  }

  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override {
    onMediaWithTimestampIndication(0, buffer);
  }

  void onVideoFocusRequest(
      const aap_protobuf::service::media::video::message::VideoFocusRequestNotification& request)
      override {
    auto channel = m_channel.lock();
    if (!channel || !m_service) {
      return;
    }

    aaLogInfo("videoChannel", QString("Video focus request received mode=%1 reason=%2")
                                  .arg(static_cast<int>(request.mode()))
                                  .arg(static_cast<int>(request.reason())));

    sendProjectedVideoFocusIndication(false);
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=video recoverable receive error (code=%1 native=%2), "
                         "scheduling receive re-arm")
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug("channelError", "channel=video operation-in-progress, keeping current receive");
      return;
    }

    m_service->onChannelError(QStringLiteral("video"), QString::fromStdString(e.what()));
  }

 private:
  void sendProjectedVideoFocusIndication(bool unsolicited) {
    auto channel = m_channel.lock();
    if (!channel || !m_service || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::media::video::message::VideoFocusNotification indication;
    indication.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    indication.set_unsolicited(unsolicited);

    // Mark a new logical stream-nudge attempt (epoch) so we can count at most
    // one timeout per attempt even if multiple channel sends fail (focus + audio)
    if (m_service) {
      ++m_service->m_projectionStreamNudgeEpoch;
      m_service->m_projectionStreamLastNudgeMs = QDateTime::currentMSecsSinceEpoch();
    }

    const int currentEpoch = m_service ? m_service->m_projectionStreamNudgeEpoch : 0;

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then(
        []() {},
        [self = shared_from_this(), epoch = currentEpoch](const aasdk::error::Error& error) {
          // Only count one timeout per logical nudge attempt (epoch).
          if (self->m_service) {
            const auto native = error.getNativeCode();
            const bool isTimeoutNative2 = (native == 2 || native == 4294967292u);
            if (isTimeoutNative2 && self->m_service->m_preStartNudgeLastCountedEpoch != epoch) {
              ++self->m_service->m_preStartNudgeNative2ConsecutiveTimeouts;
              self->m_service->m_preStartNudgeLastCountedEpoch = epoch;
              aaLogDebug(
                  "projectionWatchdog",
                  QString("pre-start nudge timeout counted epoch=%1 consecutive=%2 native=%3")
                      .arg(epoch)
                      .arg(self->m_service->m_preStartNudgeNative2ConsecutiveTimeouts)
                      .arg(static_cast<quint64>(native)));
            }
          }
          self->onChannelError(error);
        });

    channel->sendVideoFocusIndication(indication, std::move(promise));
    aaLogInfo("videoChannel",
              QString("Sent projected video focus indication (unsolicited=%1)")
                  .arg(unsolicited ? QStringLiteral("true") : QStringLiteral("false")));
  }

  void armReceive() {
    auto channel = m_channel.lock();
    if (channel && m_service && !m_service->m_aasdkTeardownInProgress) {
      m_receiveArmCount++;
      if ((m_receiveArmCount % 200) == 0) {
        aaLogDebug("videoChannel",
                   QString("receive_armed count=%1 session_id=%2 payload_count=%3 ack_count=%4")
                       .arg(m_receiveArmCount)
                       .arg(m_sessionId)
                       .arg(m_payloadCount)
                       .arg(m_ackCount));
      }
      channel->receive(shared_from_this());
    }
  }

  QPointer<RealAndroidAutoService> m_service;
  std::weak_ptr<aasdk::channel::mediasink::video::channel::VideoChannel> m_channel;
  int32_t m_sessionId{-1};
  bool m_firstPayloadLogged{false};
  quint64 m_payloadCount{0};
  quint64 m_ackCount{0};
  quint64 m_receiveArmCount{0};
};

class AAAudioEventHandler final
    : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler,
      public std::enable_shared_from_this<AAAudioEventHandler> {
 public:
  using ReceiveFn = std::function<void(
      aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler::Pointer)>;
  using OpenResponseFn =
      std::function<void(const aap_protobuf::service::control::message::ChannelOpenResponse&,
                         aasdk::channel::SendPromise::Pointer)>;
  using SetupResponseFn =
      std::function<void(const aap_protobuf::service::media::shared::message::Config&,
                         aasdk::channel::SendPromise::Pointer)>;
  using AckFn = std::function<void(const aap_protobuf::service::media::source::message::Ack&,
                                   aasdk::channel::SendPromise::Pointer)>;

  AAAudioEventHandler(RealAndroidAutoService* service, QString channelName, ReceiveFn receiveFn,
                      OpenResponseFn openResponseFn, SetupResponseFn setupResponseFn, AckFn ackFn,
                      std::function<void(const QByteArray&)> mediaCallback)
      : m_service(service),
        m_channelName(std::move(channelName)),
        m_receiveFn(std::move(receiveFn)),
        m_openResponseFn(std::move(openResponseFn)),
        m_setupResponseFn(std::move(setupResponseFn)),
        m_ackFn(std::move(ackFn)),
        m_mediaCallback(std::move(mediaCallback)) {}

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_openResponseFn) {
      return;
    }

    aaLogInfo("audioChannel", QString("channel=%1 open request received").arg(m_channelName));

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });
    m_openResponseFn(response, std::move(promise));
    armReceive();
  }

  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request) override {
    if (!m_service || !m_setupResponseFn) {
      return;
    }

    aaLogInfo("audioChannel", QString("channel=%1 setup request: codec_type=%2")
                                  .arg(m_channelName)
                                  .arg(static_cast<int>(request.type())));

    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_setupResponseFn(response, std::move(promise));
    if (m_service && m_channelName == QStringLiteral("mediaAudio")) {
      m_service->m_mediaAudioConfigured = true;
      m_service->publishProjectionStatus(QStringLiteral("media_audio_configured"));
    }
    armReceive();
  }

  void onMediaChannelStartIndication(
      const aap_protobuf::service::media::shared::message::Start& indication) override {
    m_sessionId = indication.session_id();
    aaLogInfo(
        "audioChannel",
        QString("channel=%1 start indication session_id=%2").arg(m_channelName).arg(m_sessionId));
    if (m_service && m_channelName == QStringLiteral("mediaAudio")) {
      m_service->m_mediaAudioStarted = true;
      m_service->publishProjectionStatus(QStringLiteral("media_audio_started"));
      m_service->armOptionalChannelReceivesAfterPrimaryStart();
    }
    armReceive();
  }

  void onMediaChannelStopIndication(
      const aap_protobuf::service::media::shared::message::Stop& indication) override {
    Q_UNUSED(indication)
    aaLogInfo("audioChannel",
              QString("channel=%1 stop indication session_id=%2 payload_count=%3 ack_count=%4")
                  .arg(m_channelName)
                  .arg(m_sessionId)
                  .arg(m_payloadCount)
                  .arg(m_ackCount));
    m_sessionId = -1;
    if (m_service && m_channelName == QStringLiteral("mediaAudio")) {
      m_service->m_mediaAudioStarted = false;
      m_service->m_mediaAudioFrameReceived = false;
      m_service->publishProjectionStatus(QStringLiteral("media_audio_stopped"));
    }
    armReceive();
  }

  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer& buffer) override {
    if (!m_service || !m_ackFn) {
      return;
    }

    if (!m_firstPayloadLogged) {
      m_firstPayloadLogged = true;
      aaLogInfo(
          "audioChannel",
          QString("channel=%1 first media payload received: bytes=%2 session_id=%3 timestamp=%4")
              .arg(m_channelName)
              .arg(buffer.size)
              .arg(m_sessionId)
              .arg(static_cast<qulonglong>(timestamp)));
    }
    if (m_sessionId < 0) {
      aaLogWarning(
          "audioChannel",
          QString("channel=%1 payload received without active session bytes=%2 timestamp=%3")
              .arg(m_channelName)
              .arg(buffer.size)
              .arg(static_cast<qulonglong>(timestamp)));
    }
    m_payloadCount++;
    if ((m_payloadCount % 120) == 0) {
      aaLogDebug("audioChannel",
                 QString("channel=%1 payload_count=%2 bytes=%3 session_id=%4 timestamp=%5")
                     .arg(m_channelName)
                     .arg(m_payloadCount)
                     .arg(buffer.size)
                     .arg(m_sessionId)
                     .arg(static_cast<qulonglong>(timestamp)));
    }
    m_service->m_audioPayloadSeen = true;

    if (m_mediaCallback) {
      const QByteArray audioData(reinterpret_cast<const char*>(buffer.cdata),
                                 static_cast<int>(buffer.size));
      m_mediaCallback(audioData);
    }

    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(m_sessionId);
    ack.set_ack(1);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });
    m_ackFn(ack, std::move(promise));
    m_ackCount++;
    if ((m_ackCount % 120) == 0) {
      aaLogDebug("audioChannel", QString("channel=%1 ack_count=%2 session_id=%3")
                                     .arg(m_channelName)
                                     .arg(m_ackCount)
                                     .arg(m_sessionId));
    }
    armReceive();
  }

  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override {
    onMediaWithTimestampIndication(0, buffer);
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=%1 recoverable receive error (code=%2 native=%3), "
                         "scheduling receive re-arm")
                     .arg(m_channelName)
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug(
          "channelError",
          QString("channel=%1 operation-in-progress, keeping current receive").arg(m_channelName));
      return;
    }

    m_service->onChannelError(m_channelName, QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (m_service && !m_service->m_aasdkTeardownInProgress && m_receiveFn) {
      m_receiveArmCount++;
      if ((m_receiveArmCount % 200) == 0) {
        aaLogDebug("audioChannel",
                   QString("channel=%1 receive_armed count=%2 session_id=%3 payload_count=%4 "
                           "ack_count=%5")
                       .arg(m_channelName)
                       .arg(m_receiveArmCount)
                       .arg(m_sessionId)
                       .arg(m_payloadCount)
                       .arg(m_ackCount));
      }
      m_receiveFn(shared_from_this());
    }
  }

  QPointer<RealAndroidAutoService> m_service;
  QString m_channelName;
  ReceiveFn m_receiveFn;
  OpenResponseFn m_openResponseFn;
  SetupResponseFn m_setupResponseFn;
  AckFn m_ackFn;
  std::function<void(const QByteArray&)> m_mediaCallback;
  int32_t m_sessionId{-1};
  bool m_firstPayloadLogged{false};
  quint64 m_payloadCount{0};
  quint64 m_ackCount{0};
  quint64 m_receiveArmCount{0};
};

class AAInputEventHandler final
    : public aasdk::channel::inputsource::IInputSourceServiceEventHandler,
      public std::enable_shared_from_this<AAInputEventHandler> {
 public:
  explicit AAInputEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_inputChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_inputChannel->sendChannelOpenResponse(response, std::move(promise));
    aaLogInfo("inputChannel", "Input channel open request accepted");
    armReceive();
  }

  void onKeyBindingRequest(
      const aap_protobuf::service::media::sink::message::KeyBindingRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_inputChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::media::sink::message::KeyBindingResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_inputChannel->sendKeyBindingResponse(response, std::move(promise));
    aaLogInfo("inputChannel", "Input key binding request accepted");
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=input recoverable receive error (code=%1 native=%2), "
                         "scheduling receive re-arm")
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug("channelError", "channel=input operation-in-progress, keeping current receive");
      return;
    }

    m_service->onChannelError(QStringLiteral("input"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (m_service && !m_service->m_aasdkTeardownInProgress && m_service->m_inputChannel) {
      m_service->m_inputChannel->receive(shared_from_this());
    }
  }

  QPointer<RealAndroidAutoService> m_service;
};

class AASensorEventHandler final
    : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler,
      public std::enable_shared_from_this<AASensorEventHandler> {
 public:
  explicit AASensorEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_sensorChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_sensorChannel->sendChannelOpenResponse(response, std::move(promise));
    aaLogInfo("sensorChannel", "Sensor channel open request accepted");
    armReceive();
  }

  void onSensorStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request) override {
    if (!m_service || !m_service->m_sensorChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::sensorsource::message::SensorStartResponseMessage response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([self = shared_from_this(),
                   sensorType = request.type()]() { self->sendInitialSensorEvent(sensorType); },
                  [self = shared_from_this()](const aasdk::error::Error& error) {
                    self->onChannelError(error);
                  });

    m_service->m_sensorChannel->sendSensorStartResponse(response, std::move(promise));
    aaLogInfo("sensorChannel",
              QString("Sensor start request accepted (type=%1)").arg(request.type()));
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=sensor recoverable receive error (code=%1 native=%2), "
                         "scheduling receive re-arm")
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug("channelError", "channel=sensor operation-in-progress, keeping current receive");
      return;
    }

    m_service->onChannelError(QStringLiteral("sensor"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (m_service && !m_service->m_aasdkTeardownInProgress && m_service->m_sensorChannel) {
      m_service->m_sensorChannel->receive(shared_from_this());
    }
  }

  void sendInitialSensorEvent(aap_protobuf::service::sensorsource::message::SensorType sensorType) {
    if (!m_service || !m_service->m_sensorChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::sensorsource::message::SensorBatch indication;

    switch (sensorType) {
      case aap_protobuf::service::sensorsource::message::SENSOR_LOCATION: {
        auto* location = indication.add_location_data();
        // Provide a baseline location payload when MD subscribes to location.
        location->set_latitude_e7(0);
        location->set_longitude_e7(0);
        location->set_accuracy_e3(1000000);
        location->set_altitude_e2(0);
        location->set_speed_e3(0);
        location->set_bearing_e6(0);
        break;
      }
      case aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA: {
        auto* drivingStatus = indication.add_driving_status_data();
        // 0 = DRIVE_STATUS_UNRESTRICTED. 1 would be DRIVE_STATUS_NO_VIDEO.
        drivingStatus->set_status(0);
        break;
      }
      case aap_protobuf::service::sensorsource::message::SENSOR_NIGHT_MODE:
        indication.add_night_mode_data()->set_night_mode(false);
        break;
      default:
        return;
    }

    if (sensorType == aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA) {
      const auto& drivingStatusData = indication.driving_status_data();
      const bool hasStatus = drivingStatusData.size() > 0 && drivingStatusData.Get(0).has_status();
      const int statusValue = drivingStatusData.size() > 0 ? drivingStatusData.Get(0).status() : -1;
      aaLogDebug("sensorChannel",
                 QString("Initial driving status payload: count=%1 has_status=%2 status=%3")
                     .arg(drivingStatusData.size())
                     .arg(hasStatus ? "true" : "false")
                     .arg(statusValue));
    }

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_sensorChannel->sendSensorEventIndication(indication, std::move(promise));
    aaLogInfo("sensorChannel",
              QString("Initial sensor event sent (type=%1)").arg(static_cast<int>(sensorType)));
  }

  QPointer<RealAndroidAutoService> m_service;
};

class AAMicrophoneEventHandler final
    : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler,
      public std::enable_shared_from_this<AAMicrophoneEventHandler> {
 public:
  explicit AAMicrophoneEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_microphoneChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_microphoneChannel->sendChannelOpenResponse(response, std::move(promise));
    aaLogInfo("microphoneChannel", "Microphone channel open request accepted");
    armReceive();
  }

  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_microphoneChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.set_max_unacked(1);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_microphoneChannel->sendChannelSetupResponse(response, std::move(promise));
    aaLogInfo("microphoneChannel", "Microphone setup request accepted");
    armReceive();
  }

  void onMediaSourceOpenRequest(
      const aap_protobuf::service::media::source::message::MicrophoneRequest& request) override {
    if (!m_service || !m_service->m_microphoneChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::media::source::message::MicrophoneResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);
    response.set_session_id(request.open() ? 1 : 0);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](
                               const aasdk::error::Error& error) { self->onChannelError(error); });

    m_service->m_microphoneChannel->sendMicrophoneOpenResponse(response, std::move(promise));
    aaLogInfo("microphoneChannel",
              QString("Microphone open request handled (open=%1)")
                  .arg(request.open() ? QStringLiteral("true") : QStringLiteral("false")));
    armReceive();
  }

  void onMediaChannelAckIndication(
      const aap_protobuf::service::media::source::message::Ack& indication) override {
    Q_UNUSED(indication)
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    if (isRecoverableUsbReceiveError(e)) {
      aaLogDebug("channelError",
                 QString("channel=microphone recoverable receive error (code=%1 native=%2), "
                         "scheduling receive re-arm")
                     .arg(static_cast<int>(e.getCode()))
                     .arg(e.getNativeCode()));
      auto self = shared_from_this();
      QTimer::singleShot(120, m_service, [self]() { self->armReceive(); });
      return;
    }

    if (isOperationInProgressError(e)) {
      aaLogDebug("channelError",
                 "channel=microphone operation-in-progress, keeping current receive");
      return;
    }

    m_service->onChannelError(QStringLiteral("microphone"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (m_service && !m_service->m_aasdkTeardownInProgress && m_service->m_microphoneChannel) {
      m_service->m_microphoneChannel->receive(shared_from_this());
    }
  }

  QPointer<RealAndroidAutoService> m_service;
};

class AABluetoothEventHandler final
    : public aasdk::channel::bluetooth::IBluetoothServiceEventHandler,
      public std::enable_shared_from_this<AABluetoothEventHandler> {
 public:
  explicit AABluetoothEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_bluetoothChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](const aasdk::error::Error& error) {
      self->onChannelError(error);
    });

    m_service->m_bluetoothChannel->sendChannelOpenResponse(response, std::move(promise));
    aaLogInfo("bluetoothChannel", "Bluetooth channel open request accepted");
    armReceive();
  }

  void onBluetoothPairingRequest(
      const aap_protobuf::service::bluetooth::message::BluetoothPairingRequest& request) override {
    if (!m_service || !m_service->m_bluetoothChannel || !m_service->m_strand) {
      return;
    }

    const QString phoneAddress = QString::fromStdString(request.phone_address());
    m_service->onBluetoothPairingRequest(phoneAddress);

    aap_protobuf::service::bluetooth::message::BluetoothPairingResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);
    response.set_already_paired(false);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](const aasdk::error::Error& error) {
      self->onChannelError(error);
    });

    m_service->m_bluetoothChannel->sendBluetoothPairingResponse(response, std::move(promise));
    aaLogInfo("bluetoothChannel",
              QString("Pairing request handled for phone=%1")
                  .arg(phoneAddress.isEmpty() ? QStringLiteral("<unknown>") : phoneAddress));
    armReceive();
  }

  void onBluetoothAuthenticationResult(
      const aap_protobuf::service::bluetooth::message::BluetoothAuthenticationResult& request)
      override {
    if (!m_service) {
      return;
    }

    aaLogInfo("bluetoothChannel",
              QString("Authentication result received status=%1")
                  .arg(static_cast<int>(request.status())));
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    m_service->onChannelError(QStringLiteral("bluetooth"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (!m_service || !m_service->m_bluetoothChannel) {
      return;
    }

    m_service->traceChannelReceiveArm(QStringLiteral("bluetooth"),
                                      QStringLiteral("bluetooth_event_handler"));
    m_service->m_bluetoothChannel->receive(shared_from_this());
  }

  QPointer<RealAndroidAutoService> m_service;
};

class AAWifiProjectionEventHandler final
    : public aasdk::channel::wifiprojection::IWifiProjectionServiceEventHandler,
      public std::enable_shared_from_this<AAWifiProjectionEventHandler> {
 public:
  explicit AAWifiProjectionEventHandler(RealAndroidAutoService* service) : m_service(service) {}

  void onWifiCredentialsRequest(
      const aap_protobuf::service::wifiprojection::message::WifiCredentialsRequest& request)
      override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_wifiProjectionChannel || !m_service->m_strand) {
      return;
    }

    if (m_service->m_wirelessNetworkManager) {
      const IWirelessNetworkManager::HotspotStatus hotspotStatus =
          m_service->m_wirelessNetworkManager->getHotspotStatus();
      if (hotspotStatus.active && !hotspotStatus.ssid.isEmpty()) {
        m_service->m_wirelessHotspotSsid = hotspotStatus.ssid;
        m_service->m_wirelessHotspotBssid = hotspotStatus.bssid;
      }
    }

    aap_protobuf::service::wifiprojection::message::WifiCredentialsResponse response;
    response.set_car_wifi_ssid(m_service->m_wirelessHotspotSsid.toStdString());
    response.set_car_wifi_password(m_service->m_wirelessHotspotPassword.toStdString());
    response.set_car_wifi_security_mode(
        m_service->m_wirelessHotspotPassword.isEmpty()
            ? aap_protobuf::service::wifiprojection::message::WifiSecurityMode::OPEN
            : aap_protobuf::service::wifiprojection::message::WifiSecurityMode::WPA2_PERSONAL);
    response.set_access_point_type(
        aap_protobuf::service::wifiprojection::message::AccessPointType::DYNAMIC);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](const aasdk::error::Error& error) {
      self->onChannelError(error);
    });

    m_service->m_wifiProjectionChannel->sendWifiCredentialsResponse(response, std::move(promise));
    aaLogInfo("wifiProjection",
              QString("Wifi credentials request served ssid=%1 bssid=%2")
                  .arg(m_service->m_wirelessHotspotSsid,
                       m_service->m_wirelessHotspotBssid.isEmpty()
                           ? QStringLiteral("<unknown>")
                           : m_service->m_wirelessHotspotBssid));
    armReceive();
  }

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override {
    Q_UNUSED(request)
    if (!m_service || !m_service->m_wifiProjectionChannel || !m_service->m_strand) {
      return;
    }

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(*m_service->m_strand);
    promise->then([]() {}, [self = shared_from_this()](const aasdk::error::Error& error) {
      self->onChannelError(error);
    });

    m_service->m_wifiProjectionChannel->sendChannelOpenResponse(response, std::move(promise));
    armReceive();
  }

  void onChannelError(const aasdk::error::Error& e) override {
    if (!m_service || m_service->m_aasdkTeardownInProgress) {
      return;
    }

    m_service->onChannelError(QStringLiteral("wifiProjection"), QString::fromStdString(e.what()));
  }

 private:
  void armReceive() {
    if (!m_service || !m_service->m_wifiProjectionChannel) {
      return;
    }

    m_service->traceChannelReceiveArm(QStringLiteral("wifiProjection"),
                                      QStringLiteral("wifi_projection_event_handler"));
    m_service->m_wifiProjectionChannel->receive(shared_from_this());
  }

  QPointer<RealAndroidAutoService> m_service;
};

RealAndroidAutoService::RealAndroidAutoService(MediaPipeline* mediaPipeline, QObject* parent)
    : AndroidAutoService(parent),
      m_mediaPipeline(mediaPipeline),
      m_aasdkThread(std::make_unique<QThread>()) {
  // Configure AASDK thread
  m_aasdkThread->setObjectName("AASDKThread");

  // Initialize SessionStore
  m_sessionStore = new SessionStore(QString(), this);
  if (!m_sessionStore->initialize()) {
    Logger::instance().error("[RealAndroidAutoService] Failed to initialize SessionStore");
    m_sessionStore->deleteLater();
    m_sessionStore = nullptr;
  }

  // Setup heartbeat timer for active sessions (30s interval)
  m_heartbeatTimer = new QTimer(this);
  m_heartbeatTimer->setInterval(30000);
  connect(m_heartbeatTimer, &QTimer::timeout, this,
          &RealAndroidAutoService::updateSessionHeartbeat);

  // Initialize AudioRouter for AA audio channel management
  m_audioRouter = new AudioRouter(mediaPipeline, this);
  if (!m_audioRouter->initialize()) {
    Logger::instance().warning(
        "[RealAndroidAutoService] Failed to initialize AudioRouter - audio may not work");
  }

  resetProjectionStatus(QStringLiteral("service_initialised"));
}

RealAndroidAutoService::~RealAndroidAutoService() {
  // Call cleanup directly to avoid virtual function call in destructor
  if (m_isInitialised) {
    endCurrentSession();
    stopSearching();
    if (isConnected()) {
      disconnect();
    }
    cleanupAASDK();
    m_isInitialised = false;
  }

  // m_aasdkThread is already initialized in constructor initialization list
}

void RealAndroidAutoService::setWirelessNetworkManager(
    const std::shared_ptr<IWirelessNetworkManager>& manager) {
  m_wirelessNetworkManager = manager;
}

void RealAndroidAutoService::configureTransport(const QMap<QString, QVariant>& settings) {
  applyAndroidAutoLoggingConfig(settings);
  setChannelConfig(resolveAndroidAutoChannelConfig(settings, m_channelConfig));

  Logger::instance().info(QString("[RealAndroidAutoService] Resolved startup profile: %1")
                              .arg(aaStartupProfileToString(resolveAAStartupProfile())));

  QString mode = settings.value("connectionMode", "auto").toString().toLower();
  Logger::instance().info(
      QString("[RealAndroidAutoService] Configuring transport mode: %1").arg(mode));

  if (mode == "usb") {
    m_transportMode = TransportMode::USB;
    m_wirelessEnabled = false;
  } else if (mode == "wireless") {
    m_transportMode = TransportMode::Wireless;
    m_wirelessEnabled = true;
  } else {
    m_transportMode = TransportMode::Auto;
    m_wirelessEnabled = settings.value("wireless.enabled", false).toBool();
  }

  if (m_wirelessEnabled || m_transportMode == TransportMode::Wireless) {
    m_wirelessHost = settings.value("wireless.host", "").toString();
    m_wirelessPort = settings.value("wireless.port", 5277).toUInt();
    m_wirelessHotspotAutoStart = settings.value("wireless.hotspot.auto_start", true).toBool();
    m_wirelessHotspotSsid =
        settings.value("wireless.hotspot.ssid", QStringLiteral("Crankshaft-AA")).toString();
    m_wirelessHotspotPassword =
        settings.value("wireless.hotspot.password", QStringLiteral("crankshaft-aa")).toString();
    m_wirelessHotspotChannel = settings.value("wireless.hotspot.channel", 0).toInt();

    if (m_wirelessHost.isEmpty() && m_transportMode == TransportMode::Wireless) {
      Logger::instance().info(
          "[RealAndroidAutoService] Wireless mode selected without host; TCP server/listen mode will be used.");
    }

    Logger::instance().info(
        QString("[RealAndroidAutoService] Wireless AA configured: host=%1 port=%2 hotspotAutoStart=%3 hotspotSsid=%4")
            .arg(m_wirelessHost.isEmpty() ? QStringLiteral("<listen>") : m_wirelessHost)
            .arg(m_wirelessPort)
            .arg(m_wirelessHotspotAutoStart ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_wirelessHotspotSsid));

    if (m_wirelessHotspotAutoStart && m_wirelessHotspotPassword.size() < 8) {
      Logger::instance().warning(
          "[RealAndroidAutoService] wireless.hotspot.password too short for WPA2 (min 8); hotspot startup may fail");
    }
  }
}

bool RealAndroidAutoService::initialise() {
  aaLogDebug("initialise", QString("enter, isInitialised=%1, state=%2")
                               .arg(m_isInitialised ? "true" : "false")
                               .arg(connectionStateToString(m_state)));

  if (m_isInitialised) {
    Logger::instance().warning("AndroidAutoService already initialised");
    return false;
  }

  try {
    setupAASDK();
    m_isInitialised = true;
    transitionToState(ConnectionState::DISCONNECTED);
    aaLogInfo("initialise", "setupAASDK completed and state set to DISCONNECTED");
    Logger::instance().info("AndroidAutoService initialised successfully");
    return true;
  } catch (const std::exception& e) {
    aaLogWarning("initialise", QString("exception during initialise: %1").arg(e.what()));
    Logger::instance().error(QString("Failed to initialise AndroidAutoService: %1").arg(e.what()));
    emit errorOccurred(QString("Initialisation failed: %1").arg(e.what()));
    return false;
  }
}

void RealAndroidAutoService::deinitialise() {
  if (!m_isInitialised) {
    return;
  }

  stopSearching();
  if (isConnected()) {
    disconnect();
  }

  cleanupAASDK();
  m_isInitialised = false;
  transitionToState(ConnectionState::DISCONNECTED);
  Logger::instance().info("AndroidAutoService deinitialised");
}

void RealAndroidAutoService::setupAASDK() {
  aaLogDebug("setupAASDK", "enter");
  m_aasdkTeardownInProgress = false;

  // Create io_service
#ifdef CRANKSHAFT_AASDK_OLD_API
  m_ioService = std::make_unique<boost::asio::io_service>();
#else
  m_ioService = std::make_shared<boost::asio::io_service>();
#endif
  aaLogDebug("setupAASDK", "io_service created");

  // Create strand for channel operations
  m_strand = std::make_unique<boost::asio::io_service::strand>(*m_ioService);
  aaLogDebug("setupAASDK", "io_service strand created");

  // Initialize libusb
  libusb_context* usbContext = nullptr;
  int ret = libusb_init(&usbContext);
  if (ret != 0) {
    Logger::instance().error(QString("Failed to initialize libusb: %1").arg(ret));
    throw std::runtime_error("libusb initialization failed");
  }
  m_libusbContext = usbContext;
  aaLogInfo("setupAASDK", QString("libusb initialised successfully (ret=%1)").arg(ret));

  // Create USB wrapper with libusb context
  m_usbWrapper = std::make_shared<aasdk::usb::USBWrapper>(usbContext);

  // Create query factories for AOAP device initialization
  m_queryFactory =
      std::make_shared<aasdk::usb::AccessoryModeQueryFactory>(*m_usbWrapper, *m_ioService);
  m_queryChainFactory = std::make_shared<aasdk::usb::AccessoryModeQueryChainFactory>(
      *m_usbWrapper, *m_ioService, *m_queryFactory);

  // Create USB hub for device hotplug detection
  m_usbHub =
      std::make_shared<aasdk::usb::USBHub>(*m_usbWrapper, *m_ioService, *m_queryChainFactory);

  // Start AASDK thread
  m_aasdkThread->start();
  aaLogDebug("setupAASDK",
             QString("AASDK thread started (name=%1)").arg(m_aasdkThread->objectName()));

  // Integrate io_service with Qt event loop via periodic polling
  if (m_ioServiceTimer == nullptr) {
    m_ioServiceTimer = new QTimer(this);
    m_ioServiceTimer->setObjectName("AASDKIoServicePoller");
    connect(m_ioServiceTimer, &QTimer::timeout, this, [this]() {
      // Drive libusb event loop in non-blocking mode so Qt main loop stays responsive.
      if (m_libusbContext) {
        timeval timeout{0, 0};
        const int usbRet =
            libusb_handle_events_timeout_completed(m_libusbContext, &timeout, nullptr);
        if (usbRet != 0 && usbRet != LIBUSB_ERROR_INTERRUPTED) {
          aaLogWarning("setupAASDK",
                       QString("libusb non-blocking event pump error: %1").arg(usbRet));
        }
      }

      // Process Boost.Asio tasks without blocking the Qt/main thread.
      if (m_ioService) {
        m_ioService->reset();
        constexpr int kMaxHandlersPerTick = 64;
        for (int handled = 0; handled < kMaxHandlersPerTick; ++handled) {
          if (m_ioService->poll_one() == 0) {
            break;
          }
        }
      }
    });
    m_ioServiceTimer->start(10);
    aaLogInfo("setupAASDK", "io_service event loop timer started (10 ms, non-blocking poll_one)");
  }

  Logger::instance().info("AASDK components initialised");
}

RealAndroidAutoService::TransportMode RealAndroidAutoService::getTransportMode() const {
  return m_transportMode;
}

bool RealAndroidAutoService::setupUSBTransport() {
  if (!m_usbHub || !m_ioService) {
    Logger::instance().error(
        "[RealAndroidAutoService] Cannot setup USB transport: components not ready");
    return false;
  }

  Logger::instance().info("[RealAndroidAutoService] Setting up USB transport...");
  // USB transport initialization is handled by USBHub in setupAASDK()
  // This method is called to explicitly select USB mode
  return true;
}

bool RealAndroidAutoService::setupTCPTransport(const QString& host, quint16 port) {
  if (host.isEmpty()) {
    Logger::instance().error("[RealAndroidAutoService] Cannot setup TCP transport: host is empty");
    return false;
  }

  if (!m_ioService) {
    Logger::instance().error(
        "[RealAndroidAutoService] Cannot setup TCP transport: io_service not ready");
    return false;
  }

  Logger::instance().info(
      QString("[RealAndroidAutoService] Setting up TCP transport to %1:%2").arg(host).arg(port));

  try {
    m_tcpWrapper = std::make_shared<aasdk::tcp::TCPWrapper>();

    // Create socket
    m_tcpSocket = std::make_shared<boost::asio::ip::tcp::socket>(*m_ioService);

    // Connect to phone (synchronous for now)
    boost::system::error_code ec = m_tcpWrapper->connect(*m_tcpSocket, host.toStdString(),
                                                          static_cast<uint16_t>(port));

    if (ec) {
      Logger::instance().error(QString("[RealAndroidAutoService] Failed to connect to %1:%2 - %3")
                                   .arg(host)
                                   .arg(port)
                                   .arg(QString::fromStdString(ec.message())));
      return false;
    }

    // Create TCP endpoint using the wrapper and socket
    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(*m_tcpWrapper, m_tcpSocket);

    // Create TCP transport from the endpoint
#ifdef CRANKSHAFT_AASDK_OLD_API
    m_transport = std::make_shared<aasdk::transport::TCPTransport>(*m_ioService, tcpEndpoint);
#else
    m_transport = std::make_shared<aasdk::transport::TCPTransport>(*m_ioService, tcpEndpoint);
#endif

    Logger::instance().info(
        QString("[RealAndroidAutoService] TCP transport connected to %1:%2").arg(host).arg(port));

    // Setup channels with the TCP transport
    setupChannelsWithTransport();

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(
        QString("[RealAndroidAutoService] Exception setting up TCP transport: %1").arg(e.what()));
    return false;
  }
}

bool RealAndroidAutoService::setupTCPServerTransport(quint16 port) {
  if (!m_ioService) {
    Logger::instance().error(
        "[RealAndroidAutoService] Cannot setup TCP server transport: io_service not ready");
    return false;
  }

  try {
    m_tcpWrapper = std::make_shared<aasdk::tcp::TCPWrapper>();
    m_tcpAcceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(*m_ioService);
    m_tcpSocket = std::make_shared<boost::asio::ip::tcp::socket>(*m_ioService);

    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(),
                                            static_cast<uint16_t>(port));

    m_tcpAcceptor->open(endpoint.protocol());
    m_tcpAcceptor->set_option(boost::asio::socket_base::reuse_address(true));
    m_tcpAcceptor->bind(endpoint);
    m_tcpAcceptor->listen();

    aaLogInfo("wireless",
              QString("Listening for Wireless AA TCP connection on 0.0.0.0:%1").arg(port));

    auto socket = m_tcpSocket;
    auto acceptor = m_tcpAcceptor;
    m_tcpAcceptor->async_accept(*socket, [this, socket, acceptor](const boost::system::error_code& ec) {
      if (ec) {
        Logger::instance().error(QString("[RealAndroidAutoService] Wireless AA accept failed: %1")
                                     .arg(QString::fromStdString(ec.message())));
        transitionToState(ConnectionState::DISCONNECTED);
        return;
      }

      auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(*m_tcpWrapper, socket);
#ifdef CRANKSHAFT_AASDK_OLD_API
      m_transport = std::make_shared<aasdk::transport::TCPTransport>(*m_ioService, tcpEndpoint);
#else
      m_transport = std::make_shared<aasdk::transport::TCPTransport>(*m_ioService, tcpEndpoint);
#endif

      aaLogInfo("wireless", "Inbound Wireless AA TCP connection accepted");
      setupChannelsWithTransport();
      handleConnectionEstablished();
    });

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(
        QString("[RealAndroidAutoService] Exception setting up TCP server transport: %1")
            .arg(e.what()));
    return false;
  }
}

void RealAndroidAutoService::setupChannels() {
  if (!m_aoapDevice || !m_ioService) {
    Logger::instance().error("Cannot setup channels: device or io_service not ready");
    return;
  }

  aaLogDebug("setupChannels",
             QString("channel config: %1").arg(describeChannelConfig(m_channelConfig)));

  try {
    // Create transport layer
    // Always pass a reference to io_service, regardless of smart pointer type
#ifdef CRANKSHAFT_AASDK_OLD_API
    m_transport = std::make_shared<aasdk::transport::USBTransport>(*m_ioService, m_aoapDevice);
#else
    m_transport = std::make_shared<aasdk::transport::USBTransport>(*m_ioService, m_aoapDevice);
#endif

    // Create SSL/encryption layer
    auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
    m_cryptor = std::make_shared<aasdk::messenger::Cryptor>(std::move(sslWrapper));
    m_cryptor->init();

    // Create messenger
    auto messageInStream =
        std::make_shared<aasdk::messenger::MessageInStream>(*m_ioService, m_transport, m_cryptor);
    auto messageOutStream =
        std::make_shared<aasdk::messenger::MessageOutStream>(*m_ioService, m_transport, m_cryptor);
    m_messenger = std::make_shared<aasdk::messenger::Messenger>(
        *m_ioService, std::move(messageInStream), std::move(messageOutStream));

    // Create control channel (required)
    m_controlChannel =
        std::make_shared<aasdk::channel::control::ControlServiceChannel>(*m_strand, m_messenger);

    // Create video channel
    if (m_channelConfig.videoEnabled) {
      m_videoChannel = std::make_shared<aasdk::channel::mediasink::video::channel::VideoChannel>(
          *m_strand, m_messenger);
      Logger::instance().info("Video channel enabled");
    }

    // Create media audio channel
    if (m_channelConfig.mediaAudioEnabled) {
      m_mediaAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::MediaAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Media audio channel enabled");
    }

    // Create system audio channel
    if (m_channelConfig.systemAudioEnabled) {
      m_systemAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("System audio channel enabled");
    }

    // Create speech audio channel (using GuidanceAudioChannel)
    if (m_channelConfig.speechAudioEnabled) {
      m_speechAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::GuidanceAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Speech audio channel enabled");
    }

    if (m_channelConfig.telephonyAudioEnabled) {
      m_telephonyAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::TelephonyAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Telephony audio channel enabled");
    }

    // Create input channel
    if (m_channelConfig.inputEnabled) {
      m_inputChannel =
          std::make_shared<aasdk::channel::inputsource::InputSourceService>(*m_strand, m_messenger);
      Logger::instance().info("Input channel enabled");
    }

    // Create microphone source channel
    if (m_channelConfig.microphoneEnabled) {
      m_microphoneChannel =
          std::make_shared<aasdk::channel::mediasource::audio::MicrophoneAudioChannel>(*m_strand,
                                                                                       m_messenger);
      Logger::instance().info("Microphone channel enabled");
    }

    // Create sensor channel
    if (m_channelConfig.sensorEnabled) {
      m_sensorChannel = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
          *m_strand, m_messenger);
      Logger::instance().info("Sensor channel enabled");
    }

    // Create bluetooth channel
    if (m_channelConfig.bluetoothEnabled) {
      m_bluetoothChannel =
          std::make_shared<aasdk::channel::bluetooth::BluetoothService>(*m_strand, m_messenger);
      Logger::instance().info("Bluetooth channel enabled");
    }

    if (m_wirelessEnabled || m_transportMode == TransportMode::Wireless) {
      m_wifiProjectionChannel =
          std::make_shared<aasdk::channel::wifiprojection::WifiProjectionService>(*m_strand,
                                                                                   m_messenger);
      Logger::instance().info("WiFi projection channel enabled");
    }

    // Initialize video decoder
    if (m_channelConfig.videoEnabled) {
      m_videoDecoder = std::make_unique<GStreamerVideoDecoder>(this);

      IVideoDecoder::DecoderConfig decoderConfig;
      decoderConfig.codec = IVideoDecoder::CodecType::H264;
      decoderConfig.width = m_resolution.width();
      decoderConfig.height = m_resolution.height();
      decoderConfig.fps = m_fps;
      decoderConfig.outputFormat = IVideoDecoder::PixelFormat::RGBA;
      decoderConfig.hardwareAcceleration = true;

      if (m_videoDecoder->initialize(decoderConfig)) {
        connect(m_videoDecoder.get(), &IVideoDecoder::frameDecoded, this,
                [this](int width, int height, const uint8_t* data, int size) {
                  Q_UNUSED(data)
                  m_videoDecodedFrameCount++;
                  if (shouldEmitChannelDebugSample(&m_videoDecodedFrameCount,
                                                   &m_lastVideoDecodeDebugMs)) {
                    aaLogDebug(
                        "videoDecoder",
                        QString("sample=%1 decoded frame size=%2 resolution=%3x%4 state=%5 "
                                "videoStarted=%6 firstFrameFlag=%7")
                            .arg(m_videoDecodedFrameCount)
                            .arg(size)
                            .arg(width)
                            .arg(height)
                            .arg(connectionStateToString(m_state))
                            .arg(m_videoStarted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(m_videoFrameReceived ? QStringLiteral("true")
                                                      : QStringLiteral("false")));
                  }
                  emit videoFrameReady(width, height, data, size);
                });

        connect(m_videoDecoder.get(), &IVideoDecoder::errorOccurred, this,
                [](const QString& error) {
                  Logger::instance().error("Video decoder error: " + error);
                });

        Logger::instance().info(
            QString("Video decoder initialized: %1").arg(m_videoDecoder->getDecoderName()));
      } else {
        Logger::instance().error("Failed to initialize video decoder");
        m_videoDecoder.reset();
      }
    }

    // Initialize audio mixer
    if (m_channelConfig.mediaAudioEnabled || m_channelConfig.systemAudioEnabled ||
        m_channelConfig.speechAudioEnabled || m_channelConfig.telephonyAudioEnabled) {
      m_audioMixer = std::make_unique<AudioMixer>(this);

      IAudioMixer::AudioFormat masterFormat;
      masterFormat.sampleRate = 48000;
      masterFormat.channels = 2;
      masterFormat.bitsPerSample = 16;

      if (m_audioMixer->initialize(masterFormat)) {
        // Add media audio channel (48kHz stereo)
        if (m_channelConfig.mediaAudioEnabled) {
          IAudioMixer::ChannelConfig mediaConfig;
          mediaConfig.id = IAudioMixer::ChannelId::MEDIA;
          mediaConfig.volume = 0.8f;
          mediaConfig.priority = 1;
          mediaConfig.format = masterFormat;
          m_audioMixer->addChannel(mediaConfig);
        }

        // Add system audio channel (16kHz mono)
        if (m_channelConfig.systemAudioEnabled) {
          IAudioMixer::ChannelConfig systemConfig;
          systemConfig.id = IAudioMixer::ChannelId::SYSTEM;
          systemConfig.volume = 1.0f;
          systemConfig.priority = 2;
          systemConfig.format = {16000, 1, 16};
          m_audioMixer->addChannel(systemConfig);
        }

        // Add speech audio channel (16kHz mono)
        if (m_channelConfig.speechAudioEnabled) {
          IAudioMixer::ChannelConfig speechConfig;
          speechConfig.id = IAudioMixer::ChannelId::SPEECH;
          speechConfig.volume = 1.0f;
          speechConfig.priority = 3;
          speechConfig.format = {16000, 1, 16};
          m_audioMixer->addChannel(speechConfig);
        }

        // Connect mixed audio output
        connect(m_audioMixer.get(), &IAudioMixer::audioMixed, this,
                [this](const QByteArray& mixedData) { emit audioDataReady(mixedData); });

        connect(m_audioMixer.get(), &IAudioMixer::errorOccurred, this, [](const QString& error) {
          Logger::instance().error("Audio mixer error: " + error);
        });

        Logger::instance().info("Audio mixer initialized with multiple channels");
      } else {
        Logger::instance().error("Failed to initialize audio mixer");
        m_audioMixer.reset();
      }
    }

    aaLogDebug("setupChannels",
                 QString("channel objects: video=%1, mediaAudio=%2, systemAudio=%3, speechAudio=%4, "
                     "telephonyAudio=%5, input=%6, microphone=%7, sensor=%8, bluetooth=%9, "
                     "wifiProjection=%10")
                   .arg(m_videoChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_mediaAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_systemAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_speechAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_telephonyAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_inputChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_microphoneChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_sensorChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_bluetoothChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_wifiProjectionChannel ? QStringLiteral("ready") : QStringLiteral("none")));
    m_aasdkTeardownInProgress = false;
    Logger::instance().info("All enabled channels created successfully");
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to setup channels: %1").arg(e.what()));
    emit errorOccurred(QString("Channel setup failed: %1").arg(e.what()));
  }
}

void RealAndroidAutoService::setupChannelsWithTransport() {
  if (!m_transport || !m_ioService) {
    Logger::instance().error("Cannot setup channels: transport or io_service not ready");
    return;
  }

  aaLogDebug("setupChannelsWithTransport",
             QString("channel config: %1").arg(describeChannelConfig(m_channelConfig)));

  try {
    // Create SSL/encryption layer
    auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
    m_cryptor = std::make_shared<aasdk::messenger::Cryptor>(std::move(sslWrapper));
    m_cryptor->init();

    // Create messenger
    auto messageInStream =
        std::make_shared<aasdk::messenger::MessageInStream>(*m_ioService, m_transport, m_cryptor);
    auto messageOutStream =
        std::make_shared<aasdk::messenger::MessageOutStream>(*m_ioService, m_transport, m_cryptor);
    m_messenger = std::make_shared<aasdk::messenger::Messenger>(
        *m_ioService, std::move(messageInStream), std::move(messageOutStream));

    // Create control channel (required)
    m_controlChannel =
        std::make_shared<aasdk::channel::control::ControlServiceChannel>(*m_strand, m_messenger);

    // Create video channel
    if (m_channelConfig.videoEnabled) {
      m_videoChannel = std::make_shared<aasdk::channel::mediasink::video::channel::VideoChannel>(
          *m_strand, m_messenger);
      Logger::instance().info("Video channel enabled (TCP)");
    }

    // Create media audio channel
    if (m_channelConfig.mediaAudioEnabled) {
      m_mediaAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::MediaAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Media audio channel enabled (TCP)");
    }

    // Create system audio channel
    if (m_channelConfig.systemAudioEnabled) {
      m_systemAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("System audio channel enabled (TCP)");
    }

    // Create speech audio channel (using GuidanceAudioChannel)
    if (m_channelConfig.speechAudioEnabled) {
      m_speechAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::GuidanceAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Speech audio channel enabled (TCP)");
    }

    if (m_channelConfig.telephonyAudioEnabled) {
      m_telephonyAudioChannel =
          std::make_shared<aasdk::channel::mediasink::audio::channel::TelephonyAudioChannel>(
              *m_strand, m_messenger);
      Logger::instance().info("Telephony audio channel enabled (TCP)");
    }

    // Create input channel
    if (m_channelConfig.inputEnabled) {
      m_inputChannel =
          std::make_shared<aasdk::channel::inputsource::InputSourceService>(*m_strand, m_messenger);
      Logger::instance().info("Input channel enabled (TCP)");
    }

    // Create microphone source channel
    if (m_channelConfig.microphoneEnabled) {
      m_microphoneChannel =
          std::make_shared<aasdk::channel::mediasource::audio::MicrophoneAudioChannel>(*m_strand,
                                                                                       m_messenger);
      Logger::instance().info("Microphone channel enabled (TCP)");
    }

    // Create sensor channel
    if (m_channelConfig.sensorEnabled) {
      m_sensorChannel = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
          *m_strand, m_messenger);
      Logger::instance().info("Sensor channel enabled (TCP)");
    }

    // Create bluetooth channel
    if (m_channelConfig.bluetoothEnabled) {
      m_bluetoothChannel =
          std::make_shared<aasdk::channel::bluetooth::BluetoothService>(*m_strand, m_messenger);
      Logger::instance().info("Bluetooth channel enabled (TCP)");
    }

    if (m_wirelessEnabled || m_transportMode == TransportMode::Wireless) {
      m_wifiProjectionChannel =
          std::make_shared<aasdk::channel::wifiprojection::WifiProjectionService>(*m_strand,
                                                                                   m_messenger);
      Logger::instance().info("WiFi projection channel enabled (TCP)");
    }

    // Initialize video decoder
    if (m_channelConfig.videoEnabled) {
      m_videoDecoder = std::make_unique<GStreamerVideoDecoder>(this);

      IVideoDecoder::DecoderConfig decoderConfig;
      decoderConfig.codec = IVideoDecoder::CodecType::H264;
      decoderConfig.width = m_resolution.width();
      decoderConfig.height = m_resolution.height();
      decoderConfig.fps = m_fps;
      decoderConfig.outputFormat = IVideoDecoder::PixelFormat::RGBA;
      decoderConfig.hardwareAcceleration = true;

      if (m_videoDecoder->initialize(decoderConfig)) {
        connect(m_videoDecoder.get(), &IVideoDecoder::frameDecoded, this,
                [this](int width, int height, const uint8_t* data, int size) {
                  Q_UNUSED(data)
                  m_videoDecodedFrameCount++;
                  if (shouldEmitChannelDebugSample(&m_videoDecodedFrameCount,
                                                   &m_lastVideoDecodeDebugMs)) {
                    aaLogDebug(
                        "videoDecoder",
                        QString("sample=%1 decoded frame size=%2 resolution=%3x%4 state=%5 "
                                "videoStarted=%6 firstFrameFlag=%7")
                            .arg(m_videoDecodedFrameCount)
                            .arg(size)
                            .arg(width)
                            .arg(height)
                            .arg(connectionStateToString(m_state))
                            .arg(m_videoStarted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(m_videoFrameReceived ? QStringLiteral("true")
                                                      : QStringLiteral("false")));
                  }
                  emit videoFrameReady(width, height, data, size);
                });

        connect(m_videoDecoder.get(), &IVideoDecoder::errorOccurred, this,
                [](const QString& error) {
                  Logger::instance().error("Video decoder error: " + error);
                });

        Logger::instance().info(
            QString("Video decoder initialized: %1").arg(m_videoDecoder->getDecoderName()));
      } else {
        Logger::instance().error("Failed to initialize video decoder");
        m_videoDecoder.reset();
      }
    }

    // Initialize audio mixer
    if (m_channelConfig.mediaAudioEnabled || m_channelConfig.systemAudioEnabled ||
        m_channelConfig.speechAudioEnabled || m_channelConfig.telephonyAudioEnabled) {
      m_audioMixer = std::make_unique<AudioMixer>(this);

      IAudioMixer::AudioFormat masterFormat;
      masterFormat.sampleRate = 48000;
      masterFormat.channels = 2;
      masterFormat.bitsPerSample = 16;

      if (m_audioMixer->initialize(masterFormat)) {
        // Add media audio channel (48kHz stereo)
        if (m_channelConfig.mediaAudioEnabled) {
          IAudioMixer::ChannelConfig mediaConfig;
          mediaConfig.id = IAudioMixer::ChannelId::MEDIA;
          mediaConfig.volume = 0.8f;
          mediaConfig.priority = 1;
          mediaConfig.format = masterFormat;
          m_audioMixer->addChannel(mediaConfig);
        }

        // Add system audio channel (16kHz mono)
        if (m_channelConfig.systemAudioEnabled) {
          IAudioMixer::ChannelConfig systemConfig;
          systemConfig.id = IAudioMixer::ChannelId::SYSTEM;
          systemConfig.volume = 1.0f;
          systemConfig.priority = 2;
          systemConfig.format = {16000, 1, 16};
          m_audioMixer->addChannel(systemConfig);
        }

        // Add speech audio channel (16kHz mono)
        if (m_channelConfig.speechAudioEnabled) {
          IAudioMixer::ChannelConfig speechConfig;
          speechConfig.id = IAudioMixer::ChannelId::SPEECH;
          speechConfig.volume = 1.0f;
          speechConfig.priority = 3;
          speechConfig.format = {16000, 1, 16};
          m_audioMixer->addChannel(speechConfig);
        }

        // Connect mixed audio output
        connect(m_audioMixer.get(), &IAudioMixer::audioMixed, this,
                [this](const QByteArray& mixedData) { emit audioDataReady(mixedData); });

        connect(m_audioMixer.get(), &IAudioMixer::errorOccurred, this, [](const QString& error) {
          Logger::instance().error("Audio mixer error: " + error);
        });

        Logger::instance().info("Audio mixer initialized with multiple channels");
      } else {
        Logger::instance().error("Failed to initialize audio mixer");
        m_audioMixer.reset();
      }
    }

    aaLogDebug("setupChannelsWithTransport",
                 QString("channel objects: video=%1, mediaAudio=%2, systemAudio=%3, speechAudio=%4, "
                     "telephonyAudio=%5, input=%6, microphone=%7, sensor=%8, bluetooth=%9, "
                     "wifiProjection=%10")
                   .arg(m_videoChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_mediaAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_systemAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_speechAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_telephonyAudioChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_inputChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_microphoneChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_sensorChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_bluetoothChannel ? QStringLiteral("ready") : QStringLiteral("none"))
                   .arg(m_wifiProjectionChannel ? QStringLiteral("ready") : QStringLiteral("none")));
    m_aasdkTeardownInProgress = false;
    Logger::instance().info("All enabled channels created successfully (TCP)");
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to setup channels (TCP): %1").arg(e.what()));
    emit errorOccurred(QString("Channel setup failed (TCP): %1").arg(e.what()));
  }
}

void RealAndroidAutoService::cleanupAASDK() {
  m_aasdkTeardownInProgress = true;

  if (m_aoapRetryResetTimer) {
    m_aoapRetryResetTimer->stop();
  }

  if (m_slowdownTimer) {
    m_slowdownTimer->stop();
  }

  if (m_deviceDetectionTimer) {
    m_deviceDetectionTimer->stop();
  }

  // Stop io_service poller
  if (m_ioServiceTimer) {
    m_ioServiceTimer->stop();
    delete m_ioServiceTimer;
    m_ioServiceTimer = nullptr;
  }

  // Stop io_service before tearing down channels to avoid late callbacks
  // racing channel destruction.
  if (m_ioService) {
    m_ioService->stop();
  }

  // Clean up messenger early so no additional protocol callbacks are queued.
  if (m_messenger) {
    m_messenger->stop();
    m_messenger.reset();
  }

  // Clean up channels after IO and messenger are quiesced.
  cleanupChannels();

  // Stop USB hub
  if (m_usbHub) {
    m_usbHub->cancel();
    m_usbHub.reset();
  }

  // Ensure any in-flight AOAP query chain is released
  m_activeAoapQueryChain.reset();

  // Clean up AOAP device
  if (m_aoapDevice) {
    m_aoapDevice.reset();
  }

  // Clean up USB wrapper
  if (m_usbWrapper) {
    m_usbWrapper.reset();
  }

  // Force the AOAP device to re-enumerate so the kernel releases any stale
  // interface claim.  This is the hotplug-gated recovery mechanism: after the
  // reset the bus generates a fresh attach event and the next claimInterface()
  // call succeeds on a clean device instance rather than hitting LIBUSB_ERROR_BUSY.
  if (m_libusbContext) {
    const bool forceCleanupUsbReset = m_forceCleanupUsbResetOnce;
    m_forceCleanupUsbResetOnce = false;

    const bool cleanupUsbResetEnabled =
        ConfigService::instance()
        .get("core.android_auto.usb.cleanup_reset_enabled", true)
            .toBool();

    const bool performCleanupUsbReset = cleanupUsbResetEnabled || forceCleanupUsbReset;

    if (forceCleanupUsbReset) {
      aaLogInfo("cleanupAASDK",
                "forcing one-shot USB reset for escalated control-timeout recovery");
    }

    if (!performCleanupUsbReset) {
      aaLogInfo("cleanupAASDK",
                "skipping USB device reset (core.android_auto.usb.cleanup_reset_enabled=false)");
    }

    static constexpr uint16_t kGoogleVid = 0x18D1;
    static constexpr uint16_t kAoapPid = 0x2D00;
    static constexpr uint16_t kAoapAdbPid = 0x2D01;

    if (performCleanupUsbReset) {
      libusb_device_handle* resetHandle =
          libusb_open_device_with_vid_pid(m_libusbContext, kGoogleVid, kAoapPid);
      if (!resetHandle) {
        resetHandle = libusb_open_device_with_vid_pid(m_libusbContext, kGoogleVid, kAoapAdbPid);
      }
      if (resetHandle) {
        aaLogInfo("cleanupAASDK",
                  "issuing USB device reset to clear stale interface claims for clean recovery");
        const int resetResult = libusb_reset_device(resetHandle);
        aaLogInfo("cleanupAASDK",
                  QString("USB device reset result=%1 (%2)")
                      .arg(resetResult)
                      .arg(resetResult == 0                        ? QStringLiteral("ok")
                           : resetResult == LIBUSB_ERROR_NOT_FOUND ? QStringLiteral("device-gone")
                                                                   : QStringLiteral("error")));
        libusb_close(resetHandle);
      } else {
        aaLogInfo("cleanupAASDK",
                  "AOAP device not found for USB reset (phone may be in Android mode or detached)");
      }
    }
  }

  if (m_libusbContext) {
    libusb_exit(m_libusbContext);
    m_libusbContext = nullptr;
  }

  // Release io_service
  if (m_ioService) {
    m_ioService.reset();
  }

  Logger::instance().info("AASDK components cleaned up");
}

void RealAndroidAutoService::performImmediateTransportRecovery(const QString& reason) {
  aaLogWarning("channelError", QString("performImmediateTransportRecovery reason=%1 state=%2")
                                   .arg(reason)
                                   .arg(connectionStateToString(m_state)));

  if (m_deviceGoneRecoveryScheduled) {
    aaLogInfo("channelError", "Immediate transport recovery already scheduled, ignoring");
    return;
  }

  m_deviceGoneRecoveryScheduled = true;
  ++m_controlTimeoutRecoveryCount;
  m_forceCleanupUsbResetOnce = true;

  // Stop timers and usb hub to prepare for cleanup
  if (m_deviceDetectionTimer) {
    m_deviceDetectionTimer->stop();
  }
  if (m_usbHub) {
    m_usbHub->cancel();
  }

  m_usbHubDetectionStarted = false;
  m_initialScanTriggered = false;
  m_aoapInProgress = false;
  m_aoapAttempts = 0;
  m_activeAoapQueryChain.reset();
  m_controlVersionRequestAttempts = 0;
  m_controlVersionFirstRequestMs = 0;
  m_controlVersionLastRequestMs = 0;

  // Soft immediate recovery: keep io_service/libusb context alive to avoid teardown races,
  // but still force a USB reset to clear stale interface claims.
  if (m_libusbContext) {
    static constexpr uint16_t kGoogleVid = 0x18D1;
    static constexpr uint16_t kAoapPid = 0x2D00;
    static constexpr uint16_t kAoapAdbPid = 0x2D01;

    libusb_device_handle* resetHandle =
        libusb_open_device_with_vid_pid(m_libusbContext, kGoogleVid, kAoapPid);
    if (!resetHandle) {
      resetHandle = libusb_open_device_with_vid_pid(m_libusbContext, kGoogleVid, kAoapAdbPid);
    }

    if (resetHandle) {
      aaLogInfo("channelError",
                "immediate transport recovery: issuing USB reset to clear stale claims");
      const int resetResult = libusb_reset_device(resetHandle);
      aaLogInfo("channelError",
                QString("immediate transport recovery USB reset result=%1 (%2)")
                    .arg(resetResult)
                    .arg(resetResult == 0                        ? QStringLiteral("ok")
                         : resetResult == LIBUSB_ERROR_NOT_FOUND ? QStringLiteral("device-gone")
                                                                 : QStringLiteral("error")));
      libusb_close(resetHandle);
    } else {
      aaLogInfo("channelError",
                "immediate transport recovery: AOAP device not found for USB reset");
    }
  }

  if (isConnected() || m_state == ConnectionState::CONNECTING) {
    disconnect();
  }

  cleanupChannels();
  transitionToState(ConnectionState::DISCONNECTED);

  const int recoveryReinitDelayMs =
      getBoundedConfigValue("core.android_auto.control.recovery_reinit_delay_ms", 5000, 500, 15000);

  aaLogInfo("channelError",
            QString("immediate transport recovery: reinit_delay_ms=%1 control_recovery_count=%2")
                .arg(recoveryReinitDelayMs)
                .arg(m_controlTimeoutRecoveryCount));

  QTimer::singleShot(recoveryReinitDelayMs, this, [this]() {
    m_deviceGoneRecoveryScheduled = false;
    const bool started = startSearching();
    aaLogInfo("channelError",
              QString("immediate transport recovery: restart search started=%1 state=%2")
                  .arg(started ? "true" : "false")
                  .arg(connectionStateToString(m_state)));
  });
}

void RealAndroidAutoService::cleanupChannels() {
  m_aasdkTeardownInProgress = true;

  // Cleanup multimedia components
  if (m_videoDecoder) {
    m_videoDecoder->deinitialize();
    m_videoDecoder.reset();
    Logger::instance().info("Video decoder cleaned up");
  }

  if (m_audioMixer) {
    m_audioMixer->deinitialize();
    m_audioMixer.reset();
    Logger::instance().info("Audio mixer cleaned up");
  }

  // Reset all channel pointers
  m_videoChannel.reset();
  m_mediaAudioChannel.reset();
  m_systemAudioChannel.reset();
  m_speechAudioChannel.reset();
  m_telephonyAudioChannel.reset();
  m_inputChannel.reset();
  m_microphoneChannel.reset();
  m_sensorChannel.reset();
  m_bluetoothChannel.reset();
  m_wifiProjectionChannel.reset();
  m_controlChannel.reset();
  m_videoEventHandler.reset();
  m_mediaAudioEventHandler.reset();
  m_systemAudioEventHandler.reset();
  m_speechAudioEventHandler.reset();
  m_telephonyAudioEventHandler.reset();
  m_controlEventHandler.reset();
  m_inputEventHandler.reset();
  m_microphoneEventHandler.reset();
  m_bluetoothEventHandler.reset();
  m_wifiProjectionEventHandler.reset();

  m_transport.reset();
  m_tcpSocket.reset();
  m_tcpAcceptor.reset();
  m_tcpWrapper.reset();
  m_cryptor.reset();

  // Ensure AOAP interface is released before any recovery re-attach attempt.
  if (m_aoapDevice) {
    m_aoapDevice.reset();
  }

  stopControlPingLoop();

  resetProjectionStatus(QStringLiteral("channels_cleaned_up"));

  Logger::instance().info("Channels cleaned up");
}

bool RealAndroidAutoService::startSearching() {
  aaLogDebug("startSearching", QString("enter, state=%1, transportMode=%2, wirelessEnabled=%3")
                                   .arg(connectionStateToString(m_state))
                                   .arg(static_cast<int>(m_transportMode))
                                   .arg(m_wirelessEnabled ? "true" : "false"));

  m_deviceGoneRecoveryScheduled = false;
  resetProjectionStatus(QStringLiteral("start_searching"));

  if (!m_isInitialised) {
    Logger::instance().error("Cannot start searching: service not initialised");
    return false;
  }

  if (m_state != ConnectionState::DISCONNECTED) {
    Logger::instance().warning("Already searching or connected");
    return false;
  }

  // If wireless mode is configured, try TCP connection instead of USB search
  if (m_transportMode == TransportMode::Wireless || m_wirelessEnabled) {
    if (m_wirelessHotspotAutoStart && m_wirelessNetworkManager) {
      IWirelessNetworkManager::HotspotConfig hotspotConfig;
      hotspotConfig.ssid = m_wirelessHotspotSsid;
      hotspotConfig.password = m_wirelessHotspotPassword;
      hotspotConfig.channel = m_wirelessHotspotChannel;
      hotspotConfig.security = m_wirelessHotspotPassword.isEmpty()
                                   ? IWirelessNetworkManager::SecurityMode::Open
                                   : IWirelessNetworkManager::SecurityMode::Wpa2Psk;

      if (!m_wirelessNetworkManager->startHotspot(hotspotConfig)) {
        Logger::instance().warning(
            "[RealAndroidAutoService] Failed to auto-start wireless hotspot; continuing anyway");
      }

      const IWirelessNetworkManager::HotspotStatus hotspotStatus =
          m_wirelessNetworkManager->getHotspotStatus();
      if (hotspotStatus.active) {
        m_wirelessHotspotSsid = hotspotStatus.ssid;
        m_wirelessHotspotBssid = hotspotStatus.bssid;
        if (!hotspotStatus.ipAddress.isEmpty()) {
          m_wirelessHost = hotspotStatus.ipAddress;
        }
      }
    }

    transitionToState(ConnectionState::CONNECTING);
    if (m_wirelessHost.isEmpty()) {
      logInfo(QString("[RealAndroidAutoService] Starting wireless listen mode on port %1")
                  .arg(m_wirelessPort)
                  .toStdString());

      if (setupTCPServerTransport(m_wirelessPort)) {
        transitionToState(ConnectionState::SEARCHING);
        return true;
      }

      logError(QString("[RealAndroidAutoService] Failed to listen on port %1").arg(m_wirelessPort)
                   .toStdString());
      transitionToState(ConnectionState::DISCONNECTED);
      return false;
    }

    logInfo(QString("[RealAndroidAutoService] Starting wireless outbound connection to %1:%2")
                .arg(m_wirelessHost)
                .arg(m_wirelessPort)
                .toStdString());

    if (!setupTCPTransport(m_wirelessHost, m_wirelessPort)) {
      logError(QString("[RealAndroidAutoService] Failed to connect to %1:%2")
                   .arg(m_wirelessHost)
                   .arg(m_wirelessPort)
                   .toStdString());
      transitionToState(ConnectionState::DISCONNECTED);
      return false;
    }

    logInfo("[RealAndroidAutoService] Wireless connection established");
    handleConnectionEstablished();
    return true;
  }

  // USB mode: start USB hub to detect devices
  m_usbHubDetectionStarted = false;
  m_initialScanTriggered = false;
  m_usbSearchGeneration++;
  transitionToState(ConnectionState::SEARCHING);
  logInfo("[RealAndroidAutoService] Starting USB device search");

  const int startupDetectionDelayMs =
      getBoundedConfigValue("core.android_auto.usb.startup_detection_delay_ms", 0, 0, 120000);
  aaLogDebug("startSearching",
             QString("startup_detection_delay_ms=%1").arg(startupDetectionDelayMs));

  // Give USB subsystem time to enumerate devices before starting detection
  // This is especially important on startup when devices may already be connected
  QTimer::singleShot(startupDetectionDelayMs, this, [this, startupDetectionDelayMs]() {
    if (m_state != ConnectionState::SEARCHING) {
      return;  // State changed, abort
    }

    logInfo(
        QString("[RealAndroidAutoService] Starting USB hub detection after initial delay: %1 ms")
            .arg(startupDetectionDelayMs)
            .toStdString());
    startUSBHubDetection();
  });

  return true;
}

void RealAndroidAutoService::startUSBHubDetection() {
  aaLogDebug("startUSBHubDetection",
             QString("enter, state=%1").arg(connectionStateToString(m_state)));

  if (!m_ioService || !m_usbHub) {
    logError("[RealAndroidAutoService] Cannot start USB hub: missing dependencies");
    aaLogWarning("startUSBHubDetection",
                 "missing io_service or usbHub; forcing DISCONNECTED state");
    transitionToState(ConnectionState::DISCONNECTED);
    return;
  }

  if (m_usbHubDetectionStarted) {
    Logger::instance().debug(
        "[RealAndroidAutoService] USB hub detection already started, skipping duplicate start");
    aaLogDebug("startUSBHubDetection", "duplicate start request ignored");
    return;
  }

  m_usbHubDetectionStarted = true;
  m_initialScanTriggered = false;
  const quint64 currentSearchGeneration = m_usbSearchGeneration;

  const int scanIntervalFastMs =
      getBoundedConfigValue("core.android_auto.usb.scan_interval_fast_ms", 1000, 100, 60000);
  const int scanIntervalSlowMs =
      getBoundedConfigValue("core.android_auto.usb.scan_interval_slow_ms", 3000, 500, 120000);
  const int scanSlowdownAfterMs =
      getBoundedConfigValue("core.android_auto.usb.scan_slowdown_after_ms", 15000, 1000, 300000);
  const int initialScanDelayMs =
      getBoundedConfigValue("core.android_auto.usb.initial_scan_delay_ms", 100, 0, 30000);
  const QSet<uint16_t> allowVendorIds =
      readUsbVendorFilterSet(QStringLiteral("core.android_auto.usb.vendor_allow_list"));
  const QSet<uint16_t> denyVendorIds =
      readUsbVendorFilterSet(QStringLiteral("core.android_auto.usb.vendor_deny_list"));
  aaLogInfo("startUSBHubDetection",
            QString("timers: fast=%1, slow=%2, slowdown_after=%3, initial_scan_delay=%4")
                .arg(scanIntervalFastMs)
                .arg(scanIntervalSlowMs)
                .arg(scanSlowdownAfterMs)
                .arg(initialScanDelayMs));
  aaLogInfo("startUSBHubDetection", QString("vendor filters: allow=%1 deny=%2")
                                        .arg(formatUsbVendorFilterSet(allowVendorIds))
                                        .arg(formatUsbVendorFilterSet(denyVendorIds)));

  // Start USB hub to detect devices
  auto promise = aasdk::usb::IUSBHub::Promise::defer(*m_ioService);
  promise->then(
      [this, currentSearchGeneration](aasdk::usb::DeviceHandle deviceHandle) {
        if (currentSearchGeneration != m_usbSearchGeneration) {
          aaLogInfo("startUSBHubDetection",
                    QString("Ignoring stale USBHub resolve callback (generation=%1 active=%2)")
                        .arg(currentSearchGeneration)
                        .arg(m_usbSearchGeneration));
          aaLogInfo("aoapTrace", QString("hub stage=promise-resolved-ignored "
                                         "reason=stale-generation generation=%1 active=%2")
                                     .arg(currentSearchGeneration)
                                     .arg(m_usbSearchGeneration));
          return;
        }

        if (m_state != ConnectionState::SEARCHING) {
          aaLogInfo("startUSBHubDetection",
                    QString("Ignoring USBHub resolve callback because state=%1")
                        .arg(connectionStateToString(m_state)));
          aaLogInfo("aoapTrace",
                    QString("hub stage=promise-resolved-ignored reason=state-%1 generation=%2")
                        .arg(connectionStateToString(m_state))
                        .arg(currentSearchGeneration));
          return;
        }

        aaLogInfo("aoapTrace", QString("hub stage=promise-resolved handle=%1 state=%2")
                                   .arg(deviceHandle ? "valid" : "null")
                                   .arg(connectionStateToString(m_state)));
        aaLogInfo("startUSBHubDetection", "USBHub promise resolved with device handle");

        if (deviceHandle && m_state == ConnectionState::SEARCHING &&
            m_state != ConnectionState::CONNECTING && m_state != ConnectionState::CONNECTED) {
          static constexpr uint16_t kGoogleVendorId = 0x18D1;
          static constexpr uint16_t kAoapProductIdAccessory = 0x2D00;
          static constexpr uint16_t kAoapProductIdAccessoryAdb = 0x2D01;

          libusb_device* resolvedDevice = nullptr;
          libusb_device_descriptor resolvedDescriptor{};
          bool hasDescriptor = false;
          bool isAoapModeDevice = false;

          if (m_usbWrapper) {
            resolvedDevice = m_usbWrapper->getDevice(deviceHandle);
            if (resolvedDevice &&
                m_usbWrapper->getDeviceDescriptor(resolvedDevice, resolvedDescriptor) == 0) {
              hasDescriptor = true;
              isAoapModeDevice = resolvedDescriptor.idVendor == kGoogleVendorId &&
                                 (resolvedDescriptor.idProduct == kAoapProductIdAccessory ||
                                  resolvedDescriptor.idProduct == kAoapProductIdAccessoryAdb);
            }
          }

          aaLogInfo("aoapTrace",
                    QString("hub stage=promise-resolved-descriptor descriptor=%1 aoapMode=%2")
                        .arg(hasDescriptor
                                 ? formatUsbDescriptorSummary(resolvedDescriptor, resolvedDevice)
                                 : QStringLiteral("unavailable"))
                        .arg(isAoapModeDevice ? QStringLiteral("true") : QStringLiteral("false")));

          if (isAoapModeDevice) {
            aaLogInfo("aoapTrace", "hub stage=promise-resolved-direct-attach attempt");

            try {
              m_aoapInProgress = false;
              m_aoapAttempts = 0;
              if (m_aoapRetryResetTimer) {
                m_aoapRetryResetTimer->stop();
              }

              if (m_deviceDetectionTimer) {
                m_deviceDetectionTimer->stop();
              }

              m_aoapDevice = aasdk::usb::AOAPDevice::create(*m_usbWrapper, *m_ioService,
                                                            std::move(deviceHandle));

              aaLogInfo("aoapTrace", "hub stage=promise-resolved-direct-attach success");
              transitionToState(ConnectionState::CONNECTING);
              setupChannels();
              handleConnectionEstablished();
              logInfo("[RealAndroidAutoService] USBHub direct attach succeeded");
              return;
            } catch (const std::exception& e) {
              aaLogWarning(
                  "aoapTrace",
                  QString("hub stage=promise-resolved-direct-attach exception=%1").arg(e.what()));
              aaLogWarning("aoapTrace", QString("USBHub direct attach failed: %1").arg(e.what()));
            }
          }

          aaLogInfo("aoapTrace",
                    "hub stage=promise-resolved-direct-attach skipped reason=non-aoap-or-fallback");
          logInfo(
              "[RealAndroidAutoService] Deferring USBHub-resolved handle to AOAP scan/open path");
        }

        logInfo("Device connected, triggering immediate AOAP detection scan");
        if (m_deviceDetectionTimer && !m_deviceDetectionTimer->isActive()) {
          m_deviceDetectionTimer->start();
        }

        QTimer::singleShot(0, this, [this, currentSearchGeneration]() {
          if (currentSearchGeneration != m_usbSearchGeneration ||
              m_state != ConnectionState::SEARCHING) {
            return;
          }

          checkForConnectedDevices();
        });
      },
      [this, currentSearchGeneration](const aasdk::error::Error& error) {
        if (currentSearchGeneration != m_usbSearchGeneration) {
          aaLogInfo("startUSBHubDetection",
                    QString("Ignoring stale USBHub reject callback (generation=%1 active=%2)")
                        .arg(currentSearchGeneration)
                        .arg(m_usbSearchGeneration));
          aaLogInfo("aoapTrace", QString("hub stage=promise-rejected-ignored "
                                         "reason=stale-generation generation=%1 active=%2")
                                     .arg(currentSearchGeneration)
                                     .arg(m_usbSearchGeneration));
          return;
        }

        if (m_state != ConnectionState::SEARCHING) {
          aaLogInfo("startUSBHubDetection",
                    QString("Ignoring USBHub reject callback because state=%1")
                        .arg(connectionStateToString(m_state)));
          aaLogInfo("aoapTrace",
                    QString("hub stage=promise-rejected-ignored reason=state-%1 generation=%2")
                        .arg(connectionStateToString(m_state))
                        .arg(currentSearchGeneration));
          return;
        }

        aaLogWarning("aoapTrace",
                     QString("hub stage=promise-rejected %1").arg(formatAasdkErrorDetails(error)));
        aaLogWarning(
            "startUSBHubDetection",
            QString("USBHub promise rejected: %1").arg(QString::fromStdString(error.what())));
        logError(
            QString("USB hub error: %1").arg(QString::fromStdString(error.what())).toStdString());
      });
  m_usbHub->start(std::move(promise));

  // Start a periodic check for connected devices (fallback if hotplug doesn't work)
  if (m_deviceDetectionTimer == nullptr) {
    m_deviceDetectionTimer = new QTimer(this);
    m_deviceDetectionTimer->setObjectName("AADeviceDetectionTimer");
    connect(m_deviceDetectionTimer, &QTimer::timeout, this,
            &RealAndroidAutoService::checkForConnectedDevices);
  }

  m_deviceDetectionTimer->setInterval(scanIntervalFastMs);
  if (!m_deviceDetectionTimer->isActive()) {
    m_deviceDetectionTimer->start();
  }
  logInfo(QString("[RealAndroidAutoService] Started periodic device detection timer: fast=%1 ms, "
                  "slow=%2 ms, slowdownAfter=%3 ms")
              .arg(scanIntervalFastMs)
              .arg(scanIntervalSlowMs)
              .arg(scanSlowdownAfterMs)
              .toStdString());

  QTimer::singleShot(scanSlowdownAfterMs, this, [this, scanIntervalSlowMs, scanSlowdownAfterMs]() {
    if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
      m_deviceDetectionTimer->setInterval(scanIntervalSlowMs);
      logInfo(QString("[RealAndroidAutoService] Reduced device detection "
                      "frequency to every %1 ms after %2 ms")
                  .arg(scanIntervalSlowMs)
                  .arg(scanSlowdownAfterMs)
                  .toStdString());
    }
  });

  // Trigger an immediate device check (don't wait for first timer tick)
  QTimer::singleShot(initialScanDelayMs, this, [this, initialScanDelayMs]() {
    if (m_state == ConnectionState::SEARCHING) {
      if (m_initialScanTriggered) {
        return;
      }

      m_initialScanTriggered = true;
      logInfo(QString("[RealAndroidAutoService] Running initial device scan after %1 ms")
                  .arg(initialScanDelayMs)
                  .toStdString());
      checkForConnectedDevices();
    }
  });

  logInfo("Started searching for Android Auto devices");
}

void RealAndroidAutoService::stopSearching() {
  if (m_state == ConnectionState::SEARCHING) {
    m_usbHub->cancel();

    if (m_deviceDetectionTimer) {
      m_deviceDetectionTimer->stop();
    }

    m_usbHubDetectionStarted = false;
    m_initialScanTriggered = false;
    resetProjectionStatus(QStringLiteral("stop_searching"));

    transitionToState(ConnectionState::DISCONNECTED);
    Logger::instance().info("Stopped searching for devices");
  }
}

bool RealAndroidAutoService::connectToDevice(const QString& serial) {
  if (!m_isInitialised) {
    Logger::instance().error("Cannot connect: service not initialised");
    return false;
  }

  if (serial != m_device.serialNumber) {
    Logger::instance().error(QString("Unknown device: %1").arg(serial));
    emit errorOccurred("Unknown device: " + serial);
    return false;
  }

  transitionToState(ConnectionState::CONNECTING);
  Logger::instance().info(QString("Connecting to device: %1").arg(serial));

  // Connection will be handled by AOAP device setup
  // This is initiated from onUSBHotplug callback

  return true;
}

bool RealAndroidAutoService::disconnect() {
  if (!isConnected() && m_state != ConnectionState::CONNECTING) {
    return false;
  }

  transitionToState(ConnectionState::DISCONNECTING);
  Logger::instance().info("Disconnecting from device");

  // Stop messenger
  if (m_messenger) {
    m_messenger->stop();
  }

  // Release AOAP device
  if (m_aoapDevice) {
    m_aoapDevice.reset();
  }

  m_device.connected = false;
  stopControlPingLoop();
  resetProjectionStatus(QStringLiteral("disconnect"));
  transitionToState(ConnectionState::DISCONNECTED);
  emit disconnected();

  return true;
}

bool RealAndroidAutoService::setDisplayResolution(const QSize& resolution) {
  if (resolution.width() <= 0 || resolution.height() <= 0) {
    Logger::instance().error("Invalid resolution");
    return false;
  }

  m_resolution = resolution;
  Logger::instance().info(
      QString("Display resolution set to %1x%2").arg(resolution.width()).arg(resolution.height()));

  return true;
}

bool RealAndroidAutoService::setFramerate(int fps) {
  if (fps <= 0 || fps > 60) {
    Logger::instance().error("Invalid framerate");
    return false;
  }

  m_fps = fps;
  Logger::instance().info(QString("Framerate set to %1").arg(fps));
  return true;
}

bool RealAndroidAutoService::sendTouchInput(int x, int y, int action) {
  if (!isConnected() || !m_inputChannel) {
    Logger::instance().warning("Cannot send touch input: not connected or input channel disabled");
    return false;
  }

  try {
    using namespace crankshaft::protocol;

    const int boundedX = qBound(0, x, qMax(0, m_resolution.width() - 1));
    const int boundedY = qBound(0, y, qMax(0, m_resolution.height() - 1));

    // Map action (0=DOWN, 1=UP, 2=MOVE)
    TouchAction touchAction;
    if (action == 0) {
      touchAction = TouchAction::ACTION_DOWN;
    } else if (action == 1) {
      touchAction = TouchAction::ACTION_UP;
    } else {
      touchAction = TouchAction::ACTION_MOVED;
    }

    auto data = createTouchInputReport(static_cast<uint32_t>(boundedX),
                                       static_cast<uint32_t>(boundedY), touchAction);

    auto promise = aasdk::channel::SendPromise::defer(*m_strand);
    promise->then(
        []() {
          // Success - touch input sent
        },
        [self = QPointer<RealAndroidAutoService>(this)](const aasdk::error::Error& error) {
          const QString errorText = QString::fromStdString(error.what());
          Logger::instance().warning(
              QString("Failed to send touch input: %1").arg(errorText));

          if (self &&
              (isSslWrapperNoDeviceErrorText(errorText) || isTransportNoDeviceErrorText(errorText) ||
               isUsbTransferNoDeviceErrorText(errorText))) {
            self->onChannelError(QStringLiteral("input"), errorText);
          }
        });

    m_inputChannel->sendInputReport(data, std::move(promise));

    Logger::instance().debug(QString("Touch input sent: x=%1, y=%2, action=%3")
                   .arg(boundedX)
                   .arg(boundedY)
                                 .arg(action));

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to send touch input: %1").arg(e.what()));
    return false;
  }
}

bool RealAndroidAutoService::sendKeyInput(int key_code, int action) {
  if (!isConnected() || !m_inputChannel) {
    Logger::instance().warning("Cannot send key input: not connected or input channel disabled");
    return false;
  }

  try {
    using namespace crankshaft::protocol;

    // Map action (0=DOWN, 1=UP)
    KeyAction keyAction = (action == 0) ? KeyAction::ACTION_DOWN : KeyAction::ACTION_UP;

    auto data = createKeyInputReport(key_code, keyAction);

    auto promise = aasdk::channel::SendPromise::defer(*m_strand);
    promise->then(
        []() {
          // Success - key input sent
        },
        [self = QPointer<RealAndroidAutoService>(this)](const aasdk::error::Error& error) {
          const QString errorText = QString::fromStdString(error.what());
          Logger::instance().warning(
              QString("Failed to send key input: %1").arg(errorText));

          if (self &&
              (isSslWrapperNoDeviceErrorText(errorText) || isTransportNoDeviceErrorText(errorText) ||
               isUsbTransferNoDeviceErrorText(errorText))) {
            self->onChannelError(QStringLiteral("input"), errorText);
          }
        });

    m_inputChannel->sendInputReport(data, std::move(promise));

    Logger::instance().debug(
        QString("Key input sent: code=%1, action=%2").arg(key_code).arg(action));

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to send key input: %1").arg(e.what()));
    return false;
  }
}

bool RealAndroidAutoService::requestAudioFocus() {
  if (!isConnected() || !m_controlChannel) {
    Logger::instance().warning("Cannot request audio focus: not connected");
    return false;
  }

  try {
    using namespace crankshaft::protocol;

    auto data = createAudioFocusNotification(AudioFocusState::GAIN);

    auto promise = aasdk::channel::SendPromise::defer(*m_strand);
    promise->then([]() { Logger::instance().info("Audio focus granted to Android Auto"); },
                  [](const aasdk::error::Error& error) {
                    Logger::instance().warning(QString("Failed to request audio focus: %1")
                                                   .arg(QString::fromStdString(error.what())));
                  });

    m_controlChannel->sendAudioFocusResponse(data, std::move(promise));

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to request audio focus: %1").arg(e.what()));
    return false;
  }
}

bool RealAndroidAutoService::abandonAudioFocus() {
  if (!isConnected() || !m_controlChannel) {
    Logger::instance().warning("Cannot abandon audio focus: not connected");
    return false;
  }

  try {
    using namespace crankshaft::protocol;

    auto data = createAudioFocusNotification(AudioFocusState::LOSS);

    auto promise = aasdk::channel::SendPromise::defer(*m_strand);
    promise->then([]() { Logger::instance().info("Audio focus removed from Android Auto"); },
                  [](const aasdk::error::Error& error) {
                    Logger::instance().warning(QString("Failed to abandon audio focus: %1")
                                                   .arg(QString::fromStdString(error.what())));
                  });

    m_controlChannel->sendAudioFocusResponse(data, std::move(promise));

    return true;
  } catch (const std::exception& e) {
    Logger::instance().error(QString("Failed to abandon audio focus: %1").arg(e.what()));
    return false;
  }
}

bool RealAndroidAutoService::setAudioEnabled(bool enabled) {
  m_audioEnabled = enabled;
  Logger::instance().info(QString("Audio %1").arg(enabled ? "enabled" : "disabled"));
  return true;
}

void RealAndroidAutoService::setChannelConfig(const ChannelConfig& config) {
  bool needsReconnect =
      isConnected() && (m_channelConfig.videoEnabled != config.videoEnabled ||
                        m_channelConfig.mediaAudioEnabled != config.mediaAudioEnabled ||
                        m_channelConfig.systemAudioEnabled != config.systemAudioEnabled ||
                        m_channelConfig.speechAudioEnabled != config.speechAudioEnabled ||
                        m_channelConfig.inputEnabled != config.inputEnabled ||
                        m_channelConfig.sensorEnabled != config.sensorEnabled ||
                        m_channelConfig.bluetoothEnabled != config.bluetoothEnabled);

  m_channelConfig = config;
  Logger::instance().info("Channel configuration updated");

  if (needsReconnect) {
    Logger::instance().info("Channel config changed while connected - reconnection required");
    emit errorOccurred("Channel configuration changed. Please reconnect.");
  }
}

QJsonObject RealAndroidAutoService::getAudioConfig() const {
  QJsonObject config;
  // TODO: Get actual audio config from AASDK
  config["sampleRate"] = 48000;
  config["channels"] = 2;
  config["bitsPerSample"] = 16;
  config["codec"] = "PCM";
  return config;
}

auto RealAndroidAutoService::extractDeviceSerialFromUSB() -> QString {
  // Try to extract device serial from USB descriptors if available
  // This is called during AOAP connection to get real device identifier

  if (!m_aoapDevice) {
    aaLogDebug("extractDeviceSerialFromUSB", "aoapDevice is null");
    return QString();
  }

  // Try to enumerate USB devices and find the one matching current connection
  // For now, use a placeholder - in future, access m_aoapDevice's internal handle
  // to read USB string descriptors (serial number is typically at index 3 in descriptor)

  // Example: libusb_get_string_descriptor_ascii() to read iSerialNumber
  // auto handle = /* get from m_aoapDevice */;
  // unsigned char serial[256] = {0};
  // libusb_get_string_descriptor_ascii(handle, 3, serial, sizeof(serial));
  // return QString::fromLatin1(reinterpret_cast<const char*>(serial));

  aaLogDebug("extractDeviceSerialFromUSB", "Unable to extract serial - returning empty");
  return QString();
}

void RealAndroidAutoService::onUSBHotplug(bool connected) {
  Logger::instance().info(
      QString("USB hotplug event: %1").arg(connected ? "connected" : "disconnected"));

  if (connected) {
    handleDeviceDetected();
  } else {
    handleDeviceRemoved();
  }
}

void RealAndroidAutoService::handleDeviceDetected() {
  // Device found, populate device info
  // Try to extract real serial from AOAP, fallback to generated ID
  QString extractedSerial = extractDeviceSerialFromUSB();
  m_device.serialNumber = !extractedSerial.isEmpty()
                              ? extractedSerial
                              : QString("AA_%1_%2")
                                    .arg(QDateTime::currentMSecsSinceEpoch())
                                    .arg(QRandomGenerator::global()->bounded(10000));

  m_device.manufacturer = "Unknown";
  m_device.model = "Android Device";
  m_device.androidVersion = "Unknown";
  m_device.connected = false;
  m_device.projectionMode = ProjectionMode::PROJECTION;

  aaLogInfo("handleDeviceDetected",
            QString("Device detected with serial: %1").arg(m_device.serialNumber));

  // Persist device to SessionStore
  if (m_sessionStore) {
    const QString deviceId = m_device.serialNumber;
    QVariantMap deviceInfo;
    deviceInfo["model"] = m_device.model;
    deviceInfo["android_version"] = m_device.androidVersion;
    deviceInfo["connection_type"] =
        (m_transportMode == TransportMode::Wireless || m_wirelessEnabled) ? "wireless" : "wired";
    deviceInfo["paired"] = true;
    deviceInfo["capabilities"] = QVariantList{"media", "maps"};

    if (!m_sessionStore->createDevice(deviceId, deviceInfo)) {
      // Device might already exist, update last_seen
      if (!m_sessionStore->updateDeviceLastSeen(deviceId)) {
        Logger::instance().warning(
            QString("[RealAndroidAutoService] Failed to update device last_seen: %1")
                .arg(deviceId));
      }
    }
  }

  emit deviceFound(m_device);
}

void RealAndroidAutoService::handleDeviceRemoved() {
  if (isConnected()) {
    disconnect();
  }
}

void RealAndroidAutoService::handleConnectionEstablished() {
  resetControlTrace(QStringLiteral("connection_established"));

  aaLogInfo("startupProfile",
            QString("active profile=%1").arg(aaStartupProfileToString(resolveAAStartupProfile())));

  aaLogDebug("handleConnectionEstablished",
             QString("enter, channelsReady=%1").arg(m_messenger ? "true" : "false"));

  resetProjectionStatus(QStringLiteral("connection_established"));

  const bool compatOpenAutoProfile = isCompatOpenAutoProfileEnabled();
  const bool deferInitialChannelReceiveUntilServiceDiscovery =
      shouldDeferInitialChannelReceiveUntilServiceDiscovery();
  const bool deferNonControlReceiveUntilControlReady =
      shouldDeferNonControlReceiveUntilControlReady();
  const bool shouldArmInitialNonControlReceives =
      !deferInitialChannelReceiveUntilServiceDiscovery && !deferNonControlReceiveUntilControlReady;

  m_nonControlReceivesArmed = false;

  const bool channelsMissing = !m_videoChannel && !m_mediaAudioChannel && !m_systemAudioChannel &&
                               !m_speechAudioChannel && !m_telephonyAudioChannel &&
                               !m_inputChannel && !m_microphoneChannel && !m_sensorChannel &&
                               !m_bluetoothChannel && !m_wifiProjectionChannel;

  if (channelsMissing) {
    aaLogWarning("handleConnectionEstablished",
                 "Channel objects missing at connection establish; running setupChannels fallback");
    setupChannels();
  } else {
    aaLogDebug("handleConnectionEstablished",
               "Channel objects already initialised; skipping duplicate setupChannels");
  }

  if (m_videoChannel && !m_videoEventHandler) {
    m_videoEventHandler = std::make_shared<AAVideoEventHandler>(this, m_videoChannel);
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("video"), QStringLiteral("initial_connect"));
      m_videoChannel->receive(m_videoEventHandler);
    }
  }

  if (m_mediaAudioChannel && !m_mediaAudioEventHandler) {
    const auto channel = m_mediaAudioChannel;
    m_mediaAudioEventHandler = std::make_shared<AAAudioEventHandler>(
        this, QStringLiteral("mediaAudio"),
        [channel](auto handler) { channel->receive(std::move(handler)); },
        [channel](const auto& response, auto promise) {
          channel->sendChannelOpenResponse(response, std::move(promise));
        },
        [channel](const auto& response, auto promise) {
          channel->sendChannelSetupResponse(response, std::move(promise));
        },
        [channel](const auto& indication, auto promise) {
          channel->sendMediaAckIndication(indication, std::move(promise));
        },
        [this](const QByteArray& data) { onMediaAudioChannelUpdate(data); });
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("mediaAudio"), QStringLiteral("initial_connect"));
      m_mediaAudioChannel->receive(m_mediaAudioEventHandler);
    }
  }

  if (m_systemAudioChannel && !m_systemAudioEventHandler) {
    const auto channel = m_systemAudioChannel;
    m_systemAudioEventHandler = std::make_shared<AAAudioEventHandler>(
        this, QStringLiteral("systemAudio"),
        [channel](auto handler) { channel->receive(std::move(handler)); },
        [channel](const auto& response, auto promise) {
          channel->sendChannelOpenResponse(response, std::move(promise));
        },
        [channel](const auto& response, auto promise) {
          channel->sendChannelSetupResponse(response, std::move(promise));
        },
        [channel](const auto& indication, auto promise) {
          channel->sendMediaAckIndication(indication, std::move(promise));
        },
        [this](const QByteArray& data) { onSystemAudioChannelUpdate(data); });
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("systemAudio"), QStringLiteral("initial_connect"));
      m_systemAudioChannel->receive(m_systemAudioEventHandler);
    }
  }

  if (m_speechAudioChannel && !m_speechAudioEventHandler) {
    const auto channel = m_speechAudioChannel;
    m_speechAudioEventHandler = std::make_shared<AAAudioEventHandler>(
        this, QStringLiteral("speechAudio"),
        [channel](auto handler) { channel->receive(std::move(handler)); },
        [channel](const auto& response, auto promise) {
          channel->sendChannelOpenResponse(response, std::move(promise));
        },
        [channel](const auto& response, auto promise) {
          channel->sendChannelSetupResponse(response, std::move(promise));
        },
        [channel](const auto& indication, auto promise) {
          channel->sendMediaAckIndication(indication, std::move(promise));
        },
        [this](const QByteArray& data) { onSpeechAudioChannelUpdate(data); });
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("speechAudio"), QStringLiteral("initial_connect"));
      m_speechAudioChannel->receive(m_speechAudioEventHandler);
    }
  }

  if (m_telephonyAudioChannel && !m_telephonyAudioEventHandler) {
    const auto channel = m_telephonyAudioChannel;
    m_telephonyAudioEventHandler = std::make_shared<AAAudioEventHandler>(
        this, QStringLiteral("telephonyAudio"),
        [channel](auto handler) { channel->receive(std::move(handler)); },
        [channel](const auto& response, auto promise) {
          channel->sendChannelOpenResponse(response, std::move(promise));
        },
        [channel](const auto& response, auto promise) {
          channel->sendChannelSetupResponse(response, std::move(promise));
        },
        [channel](const auto& indication, auto promise) {
          channel->sendMediaAckIndication(indication, std::move(promise));
        },
        [this](const QByteArray& data) { onSpeechAudioChannelUpdate(data); });
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("telephonyAudio"), QStringLiteral("initial_connect"));
      m_telephonyAudioChannel->receive(m_telephonyAudioEventHandler);
    }
  }

  if (m_inputChannel && !m_inputEventHandler) {
    m_inputEventHandler = std::make_shared<AAInputEventHandler>(this);
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("input"), QStringLiteral("initial_connect"));
      m_inputChannel->receive(m_inputEventHandler);
    }
  }

  if (m_sensorChannel && !m_sensorEventHandler) {
    m_sensorEventHandler = std::make_shared<AASensorEventHandler>(this);
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("sensor"), QStringLiteral("initial_connect"));
      m_sensorChannel->receive(m_sensorEventHandler);
    }
  }

  if (m_microphoneChannel && !m_microphoneEventHandler) {
    m_microphoneEventHandler = std::make_shared<AAMicrophoneEventHandler>(this);
    const int initialMicrophoneReceiveDelayMs = getInitialMicrophoneReceiveDelayMs();
    if (initialMicrophoneReceiveDelayMs > 0 && shouldArmInitialNonControlReceives) {
      aaLogInfo("microphoneChannel", QString("Delaying initial microphone receive by %1 ms")
                                         .arg(initialMicrophoneReceiveDelayMs));
      QTimer::singleShot(initialMicrophoneReceiveDelayMs, this, [this]() {
        if (m_state == ConnectionState::CONNECTED && m_serviceDiscoveryCompleted &&
            m_microphoneChannel && m_microphoneEventHandler) {
          traceChannelReceiveArm(QStringLiteral("microphone"),
                                 QStringLiteral("initial_connect_delayed"));
          m_microphoneChannel->receive(m_microphoneEventHandler);
        }
      });
    } else if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("microphone"), QStringLiteral("initial_connect"));
      m_microphoneChannel->receive(m_microphoneEventHandler);
    }
  }

  if (m_bluetoothChannel && !m_bluetoothEventHandler) {
    m_bluetoothEventHandler = std::make_shared<AABluetoothEventHandler>(this);
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("bluetooth"), QStringLiteral("initial_connect"));
      m_bluetoothChannel->receive(m_bluetoothEventHandler);
    }
  }

  if (m_wifiProjectionChannel && !m_wifiProjectionEventHandler) {
    m_wifiProjectionEventHandler = std::make_shared<AAWifiProjectionEventHandler>(this);
    if (shouldArmInitialNonControlReceives) {
      traceChannelReceiveArm(QStringLiteral("wifiProjection"), QStringLiteral("initial_connect"));
      m_wifiProjectionChannel->receive(m_wifiProjectionEventHandler);
    }
  }

  if (compatOpenAutoProfile && shouldArmInitialNonControlReceives) {
    m_nonControlReceivesArmed = true;
    m_optionalChannelsArmed = true;
    aaLogInfo("compat",
              "compat_openauto_profile enabled: all non-control receives armed immediately");
  } else if (isResilientStartupProfileEnabled()) {
    aaLogInfo("startupProfile",
              "resilient_profile enabled: startup deferrals and recovery policy remain active");
  }

  if (m_controlChannel && !m_controlEventHandler) {
    m_controlEventHandler = std::make_shared<AAControlEventHandler>(this);
    m_controlVersionRequestAttempts = 0;
    m_controlVersionFirstRequestMs = 0;
    m_controlVersionLastRequestMs = 0;
    m_controlSendNative2ConsecutiveTimeouts = 0;
    m_controlHandshakeActivationRetryCount = 0;

    // Arm control receive once on connect. Subsequent receive re-arming is
    // handled by AAControlEventHandler to avoid duplicate OPERATION_IN_PROGRESS
    // races during version request retries.
    m_controlChannel->receive(m_controlEventHandler);

    const int initialControlDelayMs = getBoundedConfigValue(
        "core.android_auto.control.initial_version_request_delay_ms", 500, 0, 5000);
    const int postAttachSettleDelayMs =
        m_aoapDevice ? getBoundedConfigValue("core.android_auto.usb.post_attach_settle_delay_ms",
                                             350, 0, 3000)
                     : 0;
    const int totalInitialControlDelayMs =
        std::min(15000, initialControlDelayMs + postAttachSettleDelayMs);

    if (totalInitialControlDelayMs > 0) {
      aaLogInfo("control", QString("Delaying initial version request by %1 ms after connection "
                                   "(base=%2 usb_settle=%3)")
                               .arg(totalInitialControlDelayMs)
                               .arg(initialControlDelayMs)
                               .arg(postAttachSettleDelayMs));
      QTimer::singleShot(totalInitialControlDelayMs, this, [this]() {
        if (m_state == ConnectionState::CONNECTED && m_controlChannel && m_controlEventHandler) {
          sendControlVersionRequest();
        }
      });
    } else {
      sendControlVersionRequest();
    }
  }

  // Extract real device serial if not yet set
  if (m_device.serialNumber.trimmed().isEmpty()) {
    QString extractedSerial = extractDeviceSerialFromUSB();
    m_device.serialNumber = !extractedSerial.isEmpty()
                                ? extractedSerial
                                : QString("AA_%1_%2")
                                      .arg(QDateTime::currentMSecsSinceEpoch())
                                      .arg(QRandomGenerator::global()->bounded(10000));
  }

  if (m_device.manufacturer.trimmed().isEmpty()) {
    m_device.manufacturer = QStringLiteral("Unknown");
  }
  if (m_device.model.trimmed().isEmpty()) {
    m_device.model = QStringLiteral("Android Device");
  }
  if (m_device.androidVersion.trimmed().isEmpty()) {
    m_device.androidVersion = QStringLiteral("Unknown");
  }

  m_device.connected = true;
  m_deviceGoneRecoveryScheduled = false;

  aaLogInfo("handleConnectionEstablished",
            QString("Device connected with serial: %1").arg(m_device.serialNumber));

  // Create session for this device
  const QString deviceId = m_device.serialNumber;
  createSessionForDevice(deviceId);

  transitionToState(ConnectionState::CONNECTED);
  publishProjectionStatus(QStringLiteral("connection_state_connected"));
  emit connected(m_device);
  aaLogInfo("handleConnectionEstablished",
            "connection state transitioned to CONNECTED and signal emitted");
  Logger::instance().info("Android Auto connection established");
}

void RealAndroidAutoService::armNonControlReceivesAfterControlReady() {
  const bool deferInitialChannelReceiveUntilServiceDiscovery =
      shouldDeferInitialChannelReceiveUntilServiceDiscovery();
  if (deferInitialChannelReceiveUntilServiceDiscovery) {
    return;
  }

  const bool deferNonControlReceiveUntilControlReady =
      shouldDeferNonControlReceiveUntilControlReady();
  if (!deferNonControlReceiveUntilControlReady || m_nonControlReceivesArmed) {
    return;
  }

  if (m_state != ConnectionState::CONNECTED || !m_controlVersionReceived) {
    return;
  }

  m_nonControlReceivesArmed = true;

  aaLogInfo("control", "Control version established, arming deferred non-control receives");

  if (m_videoChannel && m_videoEventHandler) {
    traceChannelReceiveArm(QStringLiteral("video"), QStringLiteral("control_ready"));
    m_videoChannel->receive(m_videoEventHandler);
  }
  if (m_mediaAudioChannel && m_mediaAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("mediaAudio"), QStringLiteral("control_ready"));
    m_mediaAudioChannel->receive(m_mediaAudioEventHandler);
  }
  if (m_systemAudioChannel && m_systemAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("systemAudio"), QStringLiteral("control_ready"));
    m_systemAudioChannel->receive(m_systemAudioEventHandler);
  }
  if (m_speechAudioChannel && m_speechAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("speechAudio"), QStringLiteral("control_ready"));
    m_speechAudioChannel->receive(m_speechAudioEventHandler);
  }
  if (m_telephonyAudioChannel && m_telephonyAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("telephonyAudio"), QStringLiteral("control_ready"));
    m_telephonyAudioChannel->receive(m_telephonyAudioEventHandler);
  }
  if (m_inputChannel && m_inputEventHandler) {
    traceChannelReceiveArm(QStringLiteral("input"), QStringLiteral("control_ready"));
    m_inputChannel->receive(m_inputEventHandler);
  }
  if (m_sensorChannel && m_sensorEventHandler) {
    traceChannelReceiveArm(QStringLiteral("sensor"), QStringLiteral("control_ready"));
    m_sensorChannel->receive(m_sensorEventHandler);
  }
  if (m_bluetoothChannel && m_bluetoothEventHandler) {
    traceChannelReceiveArm(QStringLiteral("bluetooth"), QStringLiteral("control_ready"));
    m_bluetoothChannel->receive(m_bluetoothEventHandler);
  }
  if (m_wifiProjectionChannel && m_wifiProjectionEventHandler) {
    traceChannelReceiveArm(QStringLiteral("wifiProjection"), QStringLiteral("control_ready"));
    m_wifiProjectionChannel->receive(m_wifiProjectionEventHandler);
  }
  if (m_microphoneChannel && m_microphoneEventHandler) {
    const int initialMicrophoneReceiveDelayMs = getInitialMicrophoneReceiveDelayMs();
    if (initialMicrophoneReceiveDelayMs > 0) {
      QTimer::singleShot(initialMicrophoneReceiveDelayMs, this, [this]() {
        if (m_state == ConnectionState::CONNECTED && m_controlVersionReceived &&
            m_microphoneChannel && m_microphoneEventHandler) {
          traceChannelReceiveArm(QStringLiteral("microphone"),
                                 QStringLiteral("control_ready_delayed"));
          m_microphoneChannel->receive(m_microphoneEventHandler);
        }
      });
    } else {
      traceChannelReceiveArm(QStringLiteral("microphone"), QStringLiteral("control_ready"));
      m_microphoneChannel->receive(m_microphoneEventHandler);
    }
  }
}

void RealAndroidAutoService::armDeferredChannelReceivesAfterServiceDiscovery() {
  const bool deferInitialChannelReceiveUntilServiceDiscovery =
      shouldDeferInitialChannelReceiveUntilServiceDiscovery();
  if (!deferInitialChannelReceiveUntilServiceDiscovery) {
    return;
  }

  m_optionalChannelsArmed = false;

  if (m_videoChannel && m_videoEventHandler) {
    traceChannelReceiveArm(QStringLiteral("video"), QStringLiteral("service_discovery_primary"));
    m_videoChannel->receive(m_videoEventHandler);
  }
  if (m_mediaAudioChannel && m_mediaAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("mediaAudio"),
                           QStringLiteral("service_discovery_primary"));
    m_mediaAudioChannel->receive(m_mediaAudioEventHandler);
  }

  armOptionalChannelReceivesAfterPrimaryStart();

  const int optionalChannelsDelayMs = getBoundedConfigValue(
      "core.android_auto.channels.optional_startup_arm_delay_ms", 0, 0, 15000);
  if (optionalChannelsDelayMs > 0) {
    QTimer::singleShot(optionalChannelsDelayMs, this,
                       [this]() { armOptionalChannelReceivesAfterPrimaryStart(); });
  }
}

void RealAndroidAutoService::scheduleVideoFocusKickAfterServiceDiscovery() {
  if (isCompatOpenAutoProfileEnabled()) {
    aaLogInfo("compat", "compat_openauto_profile enabled: skipping unsolicited video focus kick");
    return;
  }

  const int videoFocusKickDelayMs = getBoundedConfigValue(
      "core.android_auto.video.unsolicited_focus_kick_delay_ms", 350, 0, 5000);
  if (videoFocusKickDelayMs < 0) {
    return;
  }

  QTimer::singleShot(videoFocusKickDelayMs, this, [this]() {
    if (m_state == ConnectionState::CONNECTED && m_serviceDiscoveryCompleted &&
        m_videoEventHandler) {
      m_videoEventHandler->sendProjectedFocusKick();
    }
  });
}

void RealAndroidAutoService::armOptionalChannelReceivesAfterPrimaryStart() {
  if (m_optionalChannelsArmed) {
    return;
  }

  if (m_state != ConnectionState::CONNECTED || !m_serviceDiscoveryCompleted) {
    return;
  }

  const bool allowOptionalWithoutPrimaryStart = shouldAllowOptionalChannelsBeforePrimaryStart();

  if (!allowOptionalWithoutPrimaryStart && !m_videoStarted && !m_mediaAudioStarted) {
    return;
  }

  m_optionalChannelsArmed = true;

  if (m_systemAudioChannel && m_systemAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("systemAudio"), QStringLiteral("optional_channels"));
    m_systemAudioChannel->receive(m_systemAudioEventHandler);
  }
  if (m_speechAudioChannel && m_speechAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("speechAudio"), QStringLiteral("optional_channels"));
    m_speechAudioChannel->receive(m_speechAudioEventHandler);
  }
  if (m_telephonyAudioChannel && m_telephonyAudioEventHandler) {
    traceChannelReceiveArm(QStringLiteral("telephonyAudio"), QStringLiteral("optional_channels"));
    m_telephonyAudioChannel->receive(m_telephonyAudioEventHandler);
  }
  if (m_inputChannel && m_inputEventHandler) {
    traceChannelReceiveArm(QStringLiteral("input"), QStringLiteral("optional_channels"));
    m_inputChannel->receive(m_inputEventHandler);
  }
  if (m_sensorChannel && m_sensorEventHandler) {
    traceChannelReceiveArm(QStringLiteral("sensor"), QStringLiteral("optional_channels"));
    m_sensorChannel->receive(m_sensorEventHandler);
  }
  if (m_bluetoothChannel && m_bluetoothEventHandler) {
    traceChannelReceiveArm(QStringLiteral("bluetooth"), QStringLiteral("optional_channels"));
    m_bluetoothChannel->receive(m_bluetoothEventHandler);
  }
  if (m_wifiProjectionChannel && m_wifiProjectionEventHandler) {
    traceChannelReceiveArm(QStringLiteral("wifiProjection"), QStringLiteral("optional_channels"));
    m_wifiProjectionChannel->receive(m_wifiProjectionEventHandler);
  }
  if (m_microphoneChannel && m_microphoneEventHandler) {
    const int initialMicrophoneReceiveDelayMs = getInitialMicrophoneReceiveDelayMs();
    if (initialMicrophoneReceiveDelayMs > 0) {
      QTimer::singleShot(initialMicrophoneReceiveDelayMs, this, [this]() {
        if (m_state == ConnectionState::CONNECTED && m_serviceDiscoveryCompleted &&
            m_microphoneChannel && m_microphoneEventHandler) {
          traceChannelReceiveArm(QStringLiteral("microphone"),
                                 QStringLiteral("optional_channels_delayed"));
          m_microphoneChannel->receive(m_microphoneEventHandler);
        }
      });
    } else {
      traceChannelReceiveArm(QStringLiteral("microphone"), QStringLiteral("optional_channels"));
      m_microphoneChannel->receive(m_microphoneEventHandler);
    }
  }
}

void RealAndroidAutoService::traceServiceDiscoveryResponse() const {
  QStringList channels;
  QStringList capabilityFlags;

  auto appendChannel = [&channels](const QString& name, const auto& channel) {
    if (!channel) {
      return;
    }

    channels.append(QStringLiteral("%1:%2").arg(name).arg(static_cast<int>(channel->getId())));
  };

  appendChannel(QStringLiteral("video"), m_videoChannel);
  appendChannel(QStringLiteral("mediaAudio"), m_mediaAudioChannel);
  appendChannel(QStringLiteral("systemAudio"), m_systemAudioChannel);
  appendChannel(QStringLiteral("speechAudio"), m_speechAudioChannel);
  appendChannel(QStringLiteral("telephonyAudio"), m_telephonyAudioChannel);
  appendChannel(QStringLiteral("input"), m_inputChannel);
  appendChannel(QStringLiteral("sensor"), m_sensorChannel);
  appendChannel(QStringLiteral("bluetooth"), m_bluetoothChannel);
  appendChannel(QStringLiteral("microphone"), m_microphoneChannel);

  capabilityFlags.append(
      QStringLiteral("video=%1")
          .arg(m_channelConfig.videoEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("media=%1")
          .arg(m_channelConfig.mediaAudioEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("system=%1")
          .arg(m_channelConfig.systemAudioEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("guidance=%1")
          .arg(m_channelConfig.speechAudioEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(QStringLiteral("telephony=%1")
                             .arg(m_channelConfig.telephonyAudioEnabled ? QStringLiteral("on")
                                                                        : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("input=%1")
          .arg(m_channelConfig.inputEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("sensor=%1")
          .arg(m_channelConfig.sensorEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("microphone=%1")
          .arg(m_channelConfig.microphoneEnabled ? QStringLiteral("on") : QStringLiteral("off")));
  capabilityFlags.append(
      QStringLiteral("bluetooth=%1")
          .arg(m_channelConfig.bluetoothEnabled ? QStringLiteral("on") : QStringLiteral("off")));

  aaLogInfo("serviceDiscovery", QString("response profile=%1 channels=[%2] capabilities=[%3]")
                                    .arg(aaStartupProfileToString(resolveAAStartupProfile()))
                                    .arg(channels.join(QStringLiteral(", ")))
                                    .arg(capabilityFlags.join(QStringLiteral(", "))));
}

void RealAndroidAutoService::traceChannelReceiveArm(const QString& channelName,
                                                    const QString& reason) {
  const QString key = QStringLiteral("%1|%2").arg(channelName, reason);
  if (m_channelReceiveArmTraceKeys.contains(key)) {
    return;
  }

  m_channelReceiveArmTraceKeys.insert(key);
  aaLogInfo("receiveArm",
            QString("channel=%1 reason=%2 profile=%3 state=%4 discovery=%5 control=%6")
                .arg(channelName)
                .arg(reason)
                .arg(aaStartupProfileToString(resolveAAStartupProfile()))
                .arg(connectionStateToString(m_state))
                .arg(m_serviceDiscoveryCompleted ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(m_controlVersionReceived ? QStringLiteral("true") : QStringLiteral("false")));
}

void RealAndroidAutoService::handleConnectionLost() {
  resetProjectionStatus(QStringLiteral("connection_lost"));
  if (isConnected()) {
    disconnect();
  }
}

void RealAndroidAutoService::ensureAoapRetryResetTimer() {
  if (m_aoapRetryResetTimer) {
    return;
  }

  m_aoapRetryResetTimer = new QTimer(this);
  m_aoapRetryResetTimer->setSingleShot(true);
  connect(m_aoapRetryResetTimer, &QTimer::timeout, this, [this]() {
    logInfo("[RealAndroidAutoService] AOAP attempt window reset; allowing retries again");
    m_aoapAttempts = 0;
  });
}

void RealAndroidAutoService::armAoapRetryResetWindowIfNeeded() {
  if (m_aoapAttempts < m_aoapMaxAttempts) {
    return;
  }

  const int aoapRetryResetMs =
      getBoundedConfigValue("core.android_auto.usb.aoap_retry_reset_ms", 30000, 2000, 300000);

  ensureAoapRetryResetTimer();
  if (!m_aoapRetryResetTimer->isActive()) {
    logInfo(QString("[RealAndroidAutoService] Reached %1 AOAP attempts, pausing retries for %2 ms")
                .arg(m_aoapMaxAttempts)
                .arg(aoapRetryResetMs)
                .toStdString());
    m_aoapRetryResetTimer->start(aoapRetryResetMs);
  }
}

void RealAndroidAutoService::sendControlVersionRequest() {
  if (!m_controlChannel || !m_strand) {
    aaLogWarning("control", "Cannot send version request: control channel not ready");
    return;
  }

  m_controlVersionRequestMaxAttempts =
      getBoundedConfigValue("core.android_auto.control.version_request_max_attempts", 10, 3, 30);

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (m_controlVersionFirstRequestMs <= 0) {
    m_controlVersionFirstRequestMs = nowMs;
  }

  const int recoveryWindowMs = getBoundedConfigValue(
      "core.android_auto.control.version_request_recovery_ms", 120000, 5000, 180000);
  const qint64 elapsedSinceFirstMs = nowMs - m_controlVersionFirstRequestMs;
  if (elapsedSinceFirstMs > recoveryWindowMs) {
    traceControlEvent(QStringLiteral("version_request_window_exceeded"),
                      QStringLiteral("elapsed_ms=%1 recovery_window_ms=%2 attempts=%3")
                          .arg(elapsedSinceFirstMs)
                          .arg(recoveryWindowMs)
                          .arg(m_controlVersionRequestAttempts));
    aaLogWarning("control", QString("Control version recovery window exceeded (%1 ms > %2 ms), "
                                    "triggering recovery")
                                .arg(elapsedSinceFirstMs)
                                .arg(recoveryWindowMs));
    onChannelError(QStringLiteral("control"),
                   QString("Version request timed out after %1 ms (%2 attempts)")
                       .arg(elapsedSinceFirstMs)
                       .arg(m_controlVersionRequestAttempts));
    return;
  }

  m_controlVersionRequestAttempts++;
  const int attempt = m_controlVersionRequestAttempts;
  m_controlVersionLastRequestMs = nowMs;

  auto versionRequestPromise = aasdk::channel::SendPromise::defer(*m_strand);
  versionRequestPromise->then(
      []() {},
      [this, attempt](const aasdk::error::Error& error) {
        traceControlEvent(
            QStringLiteral("version_request_error"),
            QStringLiteral("attempt=%1 %2").arg(attempt).arg(formatAasdkErrorDetails(error)));
        if (isOperationInProgressError(error)) {
          const int retryDelayMs = 300;
          aaLogInfo("control",
                    QString("Version request op-in-progress, retrying in %1 ms (attempt=%2/%3)")
                        .arg(retryDelayMs)
                        .arg(m_controlVersionRequestAttempts)
                        .arg(m_controlVersionRequestMaxAttempts));

          QTimer::singleShot(retryDelayMs, this, [this]() {
            if (m_state == ConnectionState::CONNECTED && m_controlChannel &&
                m_controlEventHandler && !m_controlVersionReceived) {
              sendControlVersionRequest();
            }
          });
          return;
        }

        if (isRecoverableUsbTransferTimeout(error)) {
          const bool fastReconnectOnSendTimeoutNative2 =
              ConfigService::instance()
                  .get("core.android_auto.control.fast_reconnect_on_send_timeout_native2", true)
                  .toBool();
          const bool isSendTimeoutNative2 =
              error.getCode() == aasdk::error::ErrorCode::USB_TRANSFER &&
              error.getNativeCode() == 2;

          if (isSendTimeoutNative2) {
            m_controlSendNative2ConsecutiveTimeouts++;
          } else {
            m_controlSendNative2ConsecutiveTimeouts = 0;
          }

          aaLogWarning("channelError",
                       QString("channel=control recoverable timeout during version request "
                               "(attempt=%1/%2 code=%3 native=%4)")
                           .arg(attempt)
                           .arg(m_controlVersionRequestMaxAttempts)
                           .arg(static_cast<int>(error.getCode()))
                           .arg(error.getNativeCode()));

          if (fastReconnectOnSendTimeoutNative2 && isSendTimeoutNative2) {
            const int fastReconnectNative2Threshold = getBoundedConfigValue(
                "core.android_auto.control.fast_reconnect_on_send_timeout_native2_threshold", 2, 1,
                10);

            aaLogWarning("control",
                         QString("native=2 send timeout consecutive=%1 threshold=%2 attempt=%3/%4")
                             .arg(m_controlSendNative2ConsecutiveTimeouts)
                             .arg(fastReconnectNative2Threshold)
                             .arg(attempt)
                             .arg(m_controlVersionRequestMaxAttempts));

            if (m_controlSendNative2ConsecutiveTimeouts < fastReconnectNative2Threshold) {
              // Keep retrying until the threshold is reached to avoid
              // over-eager teardown on single transient cancels.
              if (m_state == ConnectionState::CONNECTED &&
                  m_controlVersionRequestAttempts < m_controlVersionRequestMaxAttempts) {
                const int baseRetryDelayMs = getBoundedConfigValue(
                    "core.android_auto.control.version_request_retry_delay_ms", 750, 250, 5000);
                const int backoffMultiplier =
                    std::min(4, std::max(1, m_controlVersionRequestAttempts));
                const int retryDelayMs = std::min(baseRetryDelayMs * backoffMultiplier, 5000);
                aaLogInfo("control",
                          QString("Retrying version request in %1 ms (native=2 below threshold, "
                                  "next attempt=%2/%3)")
                              .arg(retryDelayMs)
                              .arg(m_controlVersionRequestAttempts + 1)
                              .arg(m_controlVersionRequestMaxAttempts));
                QTimer::singleShot(retryDelayMs, this, [this]() {
                  if (m_state == ConnectionState::CONNECTED && m_controlChannel &&
                      m_controlEventHandler && !m_controlVersionReceived) {
                    sendControlVersionRequest();
                  }
                });
                return;
              }

              aaLogWarning(
                  "control",
                  QString("native=2 send timeout consecutive=%1 threshold=%2 attempt=%3/%4")
                      .arg(m_controlSendNative2ConsecutiveTimeouts)
                      .arg(fastReconnectNative2Threshold)
                      .arg(attempt)
                      .arg(m_controlVersionRequestMaxAttempts));

              const bool native2ImmediateResetEnabled =
                  ConfigService::instance()
                      .get("core.android_auto.control.native2_immediate_transport_reset_enabled",
                           true)
                      .toBool();

              if (m_controlSendNative2ConsecutiveTimeouts >= fastReconnectNative2Threshold &&
                  native2ImmediateResetEnabled) {
                aaLogWarning("control", QString("Triggering immediate transport recovery due to "
                                                "native=2 consecutive=%1 threshold=%2")
                                            .arg(m_controlSendNative2ConsecutiveTimeouts)
                                            .arg(fastReconnectNative2Threshold));
                performImmediateTransportRecovery(QStringLiteral("native2_send_timeout_threshold"));
                return;
              }

              // Fallback: use existing fast-reconnect path which goes through
              // normal channel error handling.
              aaLogWarning("control",
                           QString("Fast reconnect path: native=2 send timeout at attempt %1/%2")
                               .arg(attempt)
                               .arg(m_controlVersionRequestMaxAttempts));
              onChannelError(QStringLiteral("control"),
                             QString("Version request timed out after %1 attempts")
                                 .arg(m_controlVersionRequestAttempts));
              return;
            }
          }
        }

        onChannelError(QStringLiteral("control"), QString::fromStdString(error.what()));
      });

  m_controlChannel->sendVersionRequest(std::move(versionRequestPromise));
  traceControlEvent(QStringLiteral("version_request_tx"),
                    QStringLiteral("attempt=%1/%2 elapsed_ms=%3")
                        .arg(attempt)
                        .arg(m_controlVersionRequestMaxAttempts)
                        .arg(elapsedSinceFirstMs));
  aaLogInfo("control", QString("Version request sent (attempt=%1/%2 elapsed_ms=%3)")
                           .arg(attempt)
                           .arg(m_controlVersionRequestMaxAttempts)
                           .arg(elapsedSinceFirstMs));
}

void RealAndroidAutoService::traceControlEvent(const QString& event, const QString& details) {
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (m_controlTraceStartMs <= 0) {
    m_controlTraceStartMs = nowMs;
  }

  const qint64 deltaMs = nowMs - m_controlTraceStartMs;
  ++m_controlTraceSequence;

  QString line = QString("#%1 +%2ms %3").arg(m_controlTraceSequence).arg(deltaMs).arg(event);
  if (!details.trimmed().isEmpty()) {
    line.append(QStringLiteral(" | "));
    line.append(details);
  }

  m_controlTraceWindow.append(line);
  while (m_controlTraceWindow.size() > m_controlTraceWindowSize) {
    m_controlTraceWindow.removeFirst();
  }
}

void RealAndroidAutoService::resetControlTrace(const QString& reason) {
  m_controlTraceWindow.clear();
  m_controlTraceSequence = 0;
  m_controlTraceStartMs = QDateTime::currentMSecsSinceEpoch();
  traceControlEvent(QStringLiteral("trace_reset"), reason);
}

void RealAndroidAutoService::dumpControlTrace(const QString& reason) const {
  if (m_controlTraceWindow.isEmpty()) {
    aaLogWarning("controlTrace",
                 QString("Dump requested (%1) but trace window is empty").arg(reason));
    return;
  }

  aaLogWarning("controlTrace",
               QString("Dump reason=%1 entries=%2").arg(reason).arg(m_controlTraceWindow.size()));
  for (const QString& line : m_controlTraceWindow) {
    aaLogWarning("controlTrace", line);
  }
}

void RealAndroidAutoService::checkForConnectedDevices() {
  aaLogDebug("checkForConnectedDevices",
             QString("enter, state=%1, aoapInProgress=%2, attempts=%3/%4")
                 .arg(connectionStateToString(m_state))
                 .arg(m_aoapInProgress ? "true" : "false")
                 .arg(m_aoapAttempts)
                 .arg(m_aoapMaxAttempts));

  if (!m_usbWrapper || m_state != ConnectionState::SEARCHING) {
    aaLogDebug("checkForConnectedDevices",
               "early return due to missing usbWrapper or non-SEARCHING state");
    return;
  }

  static int checkCount = 0;
  checkCount++;

  try {
    aasdk::usb::DeviceListHandle listHandle;
    auto count = m_usbWrapper->getDeviceList(listHandle);

    if (count < 0 || !listHandle) {
      if (checkCount <= 5 || checkCount % 10 == 0) {  // Log first 5 times, then every 10th
        Logger::instance().debug(
            QString("[RealAndroidAutoService] USB device list error (check #%1)").arg(checkCount));
      }
      return;
    }

    bool foundAndroidCandidate = false;
    bool foundAoapDevice = false;
    int androidCandidateCount = 0;
    int candidateOrdinal = 0;
    int filteredByAllowListCount = 0;
    int filteredByDenyListCount = 0;

    const QSet<uint16_t> allowVendorIds =
        readUsbVendorFilterSet(QStringLiteral("core.android_auto.usb.vendor_allow_list"));
    const QSet<uint16_t> denyVendorIds =
        readUsbVendorFilterSet(QStringLiteral("core.android_auto.usb.vendor_deny_list"));
    const bool allowListEnabled = !allowVendorIds.isEmpty();

    static constexpr uint16_t kGoogleVendorId = 0x18D1;
    static constexpr uint16_t kAoapProductIdAccessory = 0x2D00;
    static constexpr uint16_t kAoapProductIdAccessoryAdb = 0x2D01;
    static constexpr uint16_t kLinuxFoundationVendorId = 0x1D6B;

    for (auto* dev : *listHandle) {
      libusb_device_descriptor desc{};
      if (m_usbWrapper->getDeviceDescriptor(dev, desc) == 0) {
        const bool isAoapModeDevice =
            desc.idVendor == kGoogleVendorId && (desc.idProduct == kAoapProductIdAccessory ||
                                                 desc.idProduct == kAoapProductIdAccessoryAdb);
        const bool hasAndroidLikeInterface = hasAndroidLikeUsbInterface(dev);
        const bool isAndroidUsbCandidate =
            isAoapModeDevice || desc.idVendor == kGoogleVendorId ||
            (allowListEnabled && allowVendorIds.contains(desc.idVendor)) ||
            (desc.idVendor != 0 && desc.idProduct != 0 &&
             desc.idVendor != kLinuxFoundationVendorId && hasAndroidLikeInterface);

        if (!isAndroidUsbCandidate) {
          continue;
        }

        if (denyVendorIds.contains(desc.idVendor)) {
          filteredByDenyListCount++;
          continue;
        }

        if (allowListEnabled && !allowVendorIds.contains(desc.idVendor) && !isAoapModeDevice) {
          filteredByAllowListCount++;
          continue;
        }

        androidCandidateCount++;
        candidateOrdinal++;
        foundAndroidCandidate = true;
        foundAoapDevice = foundAoapDevice || isAoapModeDevice;

        const int nextAoapAttempt = m_aoapAttempts + 1;
        const QString descriptorSummary = formatUsbDescriptorSummary(desc, dev);
        const QString traceToken =
            makeAoapTraceToken(checkCount, candidateOrdinal, nextAoapAttempt, desc, dev);

        aaLogInfo("aoapTrace", QString("%1 stage=candidate-detected aoapMode=%2 descriptor={%3}")
                                   .arg(traceToken)
                                   .arg(isAoapModeDevice ? "true" : "false")
                                   .arg(descriptorSummary));

        if (checkCount <= 5 || checkCount % 5 == 0) {  // Log more frequently initially
          logInfo(QString("[RealAndroidAutoService] Check #%1: Found Android USB candidate: "
                          "vid=0x%2 pid=0x%3")
                      .arg(checkCount)
                      .arg(QString::asprintf("%04x", desc.idVendor))
                      .arg(QString::asprintf("%04x", desc.idProduct))
                      .toStdString());
        }

        // --- INSERT PRE-SESSION USB RESET HERE (OpenAuto parity) ---
        const bool preSessionResetEnabled =
            ConfigService::instance()
            .get("core.android_auto.usb.pre_session_reset_enabled", true)
                .toBool();
        const bool shouldAttemptPreSessionReset =
            preSessionResetEnabled && (isAoapModeDevice || hasAndroidLikeInterface);
        if (shouldAttemptPreSessionReset) {
          QString devNode = findDeviceNodeForLibusb(dev);
          if (!devNode.isEmpty()) {
            static QHash<QString, qint64> s_lastPreSessionResetMsByNode;
            static constexpr qint64 kPreSessionResetIntervalMs = 5000;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 lastResetMs = s_lastPreSessionResetMsByNode.value(devNode, 0);
            const bool throttleReset =
                lastResetMs > 0 && (nowMs - lastResetMs) < kPreSessionResetIntervalMs;

            if (throttleReset) {
              aaLogInfo(
                  "aoapTrace",
                  QString("%1 stage=pre-session-usb-reset-throttled node=%2 elapsed_ms=%3")
                      .arg(traceToken)
                      .arg(devNode)
                      .arg(nowMs - lastResetMs));
            } else {
              QString resetErr;
              bool ok = resetUsbDeviceNode(devNode, &resetErr);
              aaLogInfo("aoapTrace",
                        QString("%1 stage=pre-session-usb-reset node=%2 result=%3 err=%4")
                            .arg(traceToken)
                            .arg(devNode)
                            .arg(ok ? "ok" : "fail")
                            .arg(resetErr));
              logInfo(QString("[RealAndroidAutoService] Pre-session USB reset on %1: %2 %3")
                          .arg(devNode)
                          .arg(ok ? "ok" : "fail")
                          .arg(resetErr)
                          .toStdString());
              s_lastPreSessionResetMsByNode.insert(devNode, nowMs);
              // Sleep 250ms to allow kernel to re-enumerate
              usleep(250000);
            }
          } else {
            aaLogInfo("aoapTrace",
                      QString("%1 stage=pre-session-usb-reset node=not-found").arg(traceToken));
            logInfo("[RealAndroidAutoService] Pre-session USB reset: device node not found");
          }
        }

        // If it's already in AOAP mode, try direct attach as a fallback in case
        // USBHub hotplug/promise handoff is missed.
        if (isAoapModeDevice) {
          logInfo("[RealAndroidAutoService] ✓ Device in AOAP mode detected during scan");

          // Skip if already connected/connecting through another path.
          if (m_state == ConnectionState::CONNECTED || m_state == ConnectionState::CONNECTING) {
            aaLogInfo("aoapTrace", QString("%1 stage=aoap-fallback-skip reason=already-%2")
                                       .arg(traceToken)
                                       .arg(connectionStateToString(m_state)));
            logInfo(
                "[RealAndroidAutoService] Already connected/connecting, skipping AOAP fallback "
                "attach");
            return;
          }

          aasdk::usb::DeviceHandle aoapHandle;
          int aoapOpenResult = m_usbWrapper->open(dev, aoapHandle);
          aaLogInfo("aoapTrace", QString("%1 stage=aoap-fallback-open result=%2 handle=%3")
                                     .arg(traceToken)
                                     .arg(aoapOpenResult)
                                     .arg(aoapHandle ? "valid" : "null"));
          logInfo(QString("[RealAndroidAutoService] AOAP fallback open result: %1")
                      .arg(aoapOpenResult)
                      .toStdString());

          if (aoapOpenResult == 0 && aoapHandle) {
            try {
              m_aoapInProgress = false;
              m_aoapAttempts = 0;
              if (m_aoapRetryResetTimer) {
                m_aoapRetryResetTimer->stop();
              }

              if (m_deviceDetectionTimer) {
                m_deviceDetectionTimer->stop();
              }

              m_aoapDevice = aasdk::usb::AOAPDevice::create(*m_usbWrapper, *m_ioService,
                                                            std::move(aoapHandle));

              aaLogInfo(
                  "aoapTrace",
                  QString("%1 stage=aoap-fallback-create-device result=success").arg(traceToken));

              transitionToState(ConnectionState::CONNECTING);
              setupChannels();
              handleConnectionEstablished();
              logInfo("[RealAndroidAutoService] AOAP fallback attach succeeded via periodic scan");
              return;
            } catch (const aasdk::error::Error& error) {
              const QString errorDetails = formatAasdkErrorDetails(error);
              logError(QString("[RealAndroidAutoService] AOAP fallback attach failed: %1")
                           .arg(errorDetails)
                           .toStdString());
              aaLogWarning("aoapTrace",
                           QString("%1 stage=aoap-fallback-create-device result=aasdk-error %2")
                               .arg(traceToken)
                               .arg(errorDetails));

              const bool cleanupUsbResetEnabled =
                  ConfigService::instance()
                    .get("core.android_auto.usb.cleanup_reset_enabled", true)
                      .toBool();
              const qint32 nativeCode = static_cast<qint32>(error.getNativeCode());

              if (cleanupUsbResetEnabled &&
                  error.getCode() == aasdk::error::ErrorCode::USB_CLAIM_INTERFACE &&
                  nativeCode == LIBUSB_ERROR_BUSY) {
                aasdk::usb::DeviceHandle resetHandle;
                const int reopenResult = m_usbWrapper->open(dev, resetHandle);
                aaLogWarning(
                    "aoapTrace",
                    QString("%1 stage=aoap-fallback-busy-recovery reopen_result=%2 handle=%3")
                        .arg(traceToken)
                        .arg(reopenResult)
                        .arg(resetHandle ? "valid" : "null"));

                if (reopenResult == 0 && resetHandle) {
                  const int resetResult = libusb_reset_device(resetHandle.get());
                  aaLogWarning("aoapTrace",
                               QString("%1 stage=aoap-fallback-busy-recovery reset_result=%2")
                                   .arg(traceToken)
                                   .arg(resetResult));
                  logInfo(
                      QString(
                          "[RealAndroidAutoService] AOAP fallback busy recovery reset result: %1")
                          .arg(resetResult)
                          .toStdString());
                }
              }
            } catch (const std::exception& e) {
              logError(QString("[RealAndroidAutoService] AOAP fallback attach failed: %1")
                           .arg(e.what())
                           .toStdString());
              aaLogWarning("aoapTrace",
                           QString("%1 stage=aoap-fallback-create-device result=exception error=%2")
                               .arg(traceToken)
                               .arg(e.what()));
            }
          } else {
            logInfo(
                "[RealAndroidAutoService] AOAP mode detected but open failed; waiting for USBHub");
            aaLogWarning("aoapTrace",
                         QString("%1 stage=aoap-fallback-open result=failed").arg(traceToken));
          }

          return;
        }

        // Skip if AOAP negotiation already in progress
        if (m_aoapInProgress) {
          logInfo("[RealAndroidAutoService] AOAP already in progress, skipping");
          aaLogInfo("aoapTrace", QString("%1 stage=aoap-skip reason=in-progress currentAttempt=%2")
                                     .arg(traceToken)
                                     .arg(m_aoapAttempts));
          return;
        }

        // Respect maximum attempt limit to avoid tight retry loops
        if (m_aoapAttempts >= m_aoapMaxAttempts) {
          armAoapRetryResetWindowIfNeeded();
          const bool resetWindowActive = m_aoapRetryResetTimer && m_aoapRetryResetTimer->isActive();
          const int resetRemainingMs =
              (m_aoapRetryResetTimer ? m_aoapRetryResetTimer->remainingTime() : -1);
          if (checkCount <= 5 || checkCount % 5 == 0) {
            aaLogWarning("aoapTrace",
                         QString("%1 stage=aoap-skip reason=max-attempts current=%2 max=%3 "
                                 "reset-window-active=%4 reset-remaining-ms=%5")
                             .arg(traceToken)
                             .arg(m_aoapAttempts)
                             .arg(m_aoapMaxAttempts)
                             .arg(resetWindowActive ? "true" : "false")
                             .arg(resetRemainingMs));
          }
          logInfo(
              QString("[RealAndroidAutoService] Skipping AOAP attempt: reached max attempts (%1)")
                  .arg(m_aoapMaxAttempts)
                  .toStdString());
          return;
        }

        // Try to open device and initiate AOAP negotiation
        aasdk::usb::DeviceHandle handle;
        int openResult = m_usbWrapper->open(dev, handle);
        aaLogInfo("aoapTrace", QString("%1 stage=aoap-open result=%2 handle=%3")
                                   .arg(traceToken)
                                   .arg(openResult)
                                   .arg(handle ? "valid" : "null"));
        logInfo(QString("[RealAndroidAutoService] Open device result: %1")
                    .arg(openResult)
                    .toStdString());

        if (openResult == 0 && handle) {
          logInfo("[RealAndroidAutoService] Opened device for AOAP negotiation");

          if (m_queryChainFactory && m_ioService) {
            m_aoapInProgress = true;
            m_aoapAttempts++;
            const int aoapAttemptNumber = m_aoapAttempts;
            const int aoapWatchdogMs = getBoundedConfigValue(
                "core.android_auto.usb.aoap_watchdog_ms", 180000, 15000, 900000);
            logInfo("[RealAndroidAutoService] Creating AccessoryModeQueryChain...");
            m_activeAoapQueryChain = m_queryChainFactory->create();
            if (!m_activeAoapQueryChain) {
              aaLogWarning("aoapTrace",
                           QString("%1 stage=aoap-query-chain-create result=null").arg(traceToken));
              Logger::instance().warning(
                  "[RealAndroidAutoService] Failed to create AccessoryModeQueryChain");
              m_aoapInProgress = false;
              return;
            }
            aaLogInfo("aoapTrace",
                      QString("%1 stage=aoap-query-chain-create result=ok attempt=%2/%3")
                          .arg(traceToken)
                          .arg(aoapAttemptNumber)
                          .arg(m_aoapMaxAttempts));
            // Use io_service directly, not strand, for the promise
            auto aoapPromise = aasdk::usb::IAccessoryModeQueryChain::Promise::defer(*m_ioService);

            auto onSuccess = [this, aoapAttemptNumber,
                              traceToken](aasdk::usb::DeviceHandle devHandle) {
              m_aoapInProgress = false;
              m_aoapAttempts = 0;  // reset attempts on success
              m_activeAoapQueryChain.reset();
              if (m_aoapRetryResetTimer) {
                m_aoapRetryResetTimer->stop();
              }
              logInfo("[RealAndroidAutoService] AOAP query chain completed (success)");
              aaLogInfo("checkForConnectedDevices",
                        QString("AOAP query chain success callback (attempt=%1), devHandle=%2")
                            .arg(aoapAttemptNumber)
                            .arg(devHandle ? "valid" : "null"));
              aaLogInfo("aoapTrace",
                        QString("%1 stage=aoap-query-chain-success attempt=%2 handle=%3")
                            .arg(traceToken)
                            .arg(aoapAttemptNumber)
                            .arg(devHandle ? "valid" : "null"));
              // Device may or may not have switched; restart timer to check
              if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
                QTimer::singleShot(2000, this, [this]() {
                  if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
                    m_deviceDetectionTimer->start();
                  }
                });
              }
            };

            auto onError = [this, aoapAttemptNumber, traceToken](const aasdk::error::Error& error) {
              m_aoapInProgress = false;
              m_activeAoapQueryChain.reset();
              aaLogWarning("checkForConnectedDevices",
                           QString("AOAP query chain error callback on attempt %1: %2")
                               .arg(aoapAttemptNumber)
                               .arg(QString::fromStdString(error.what())));
              logError(QString("[RealAndroidAutoService] AOAP chain error (attempt %1): %2")
                           .arg(aoapAttemptNumber)
                           .arg(QString::fromStdString(error.what()))
                           .toStdString());
              aaLogWarning("aoapTrace", QString("%1 stage=aoap-query-chain-error attempt=%2 %3")
                                            .arg(traceToken)
                                            .arg(aoapAttemptNumber)
                                            .arg(formatAasdkErrorDetails(error)));
              armAoapRetryResetWindowIfNeeded();
              // Restart detection timer to retry
              if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
                QTimer::singleShot(2000, this, [this]() {
                  if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
                    m_deviceDetectionTimer->start();
                  }
                });
              }
            };

            aoapPromise->then(onSuccess, onError);

            logInfo("[RealAndroidAutoService] Starting AOAP query chain...");
            try {
              // Stop detection timer before starting AOAP to avoid overlapping attempts.
              if (m_deviceDetectionTimer) {
                m_deviceDetectionTimer->stop();
              }

              aaLogInfo("aoapTrace",
                        QString("%1 stage=aoap-query-chain-start attempt=%2 watchdogMs=%3")
                            .arg(traceToken)
                            .arg(aoapAttemptNumber)
                            .arg(aoapWatchdogMs));
              m_activeAoapQueryChain->start(std::move(handle), std::move(aoapPromise));
              logInfo("[RealAndroidAutoService] AOAP chain started successfully");
              aaLogInfo("checkForConnectedDevices",
                        QString("AOAP watchdog armed for %1 ms (attempt=%2)")
                            .arg(aoapWatchdogMs)
                            .arg(aoapAttemptNumber));

              // Set a timeout to check if device re-enumerated in AOAP mode
              // The chain may take longer on some devices; give it more time
              if (m_deviceDetectionTimer) {
                QTimer::singleShot(
                    aoapWatchdogMs, this, [this, aoapAttemptNumber, aoapWatchdogMs, traceToken]() {
                      if (m_aoapInProgress && m_aoapAttempts == aoapAttemptNumber) {
                        aaLogWarning("checkForConnectedDevices",
                                     QString("AOAP watchdog timeout fired after %1 ms while "
                                             "aoapInProgress=true (attempt=%2)")
                                         .arg(aoapWatchdogMs)
                                         .arg(aoapAttemptNumber));
                        aaLogWarning(
                            "aoapTrace",
                            QString("%1 stage=aoap-watchdog-timeout attempt=%2 watchdogMs=%3")
                                .arg(traceToken)
                                .arg(aoapAttemptNumber)
                                .arg(aoapWatchdogMs));
                        logInfo(
                            "[RealAndroidAutoService] AOAP timeout - checking if device "
                            "re-enumerated...");
                        // This will trigger another device check which will look for AOAP mode
                        if (m_state == ConnectionState::SEARCHING) {
                          m_aoapInProgress = false;  // Reset flag to allow retry
                          m_activeAoapQueryChain.reset();
                          armAoapRetryResetWindowIfNeeded();
                          m_deviceDetectionTimer->start();
                        }
                      } else {
                        aaLogDebug("checkForConnectedDevices",
                                   QString("Ignoring stale AOAP watchdog for attempt=%1 "
                                           "(inProgress=%2 currentAttempt=%3)")
                                       .arg(aoapAttemptNumber)
                                       .arg(m_aoapInProgress ? "true" : "false")
                                       .arg(m_aoapAttempts));
                        aaLogDebug("aoapTrace",
                                   QString("%1 stage=aoap-watchdog-stale attempt=%2 inProgress=%3 "
                                           "currentAttempt=%4")
                                       .arg(traceToken)
                                       .arg(aoapAttemptNumber)
                                       .arg(m_aoapInProgress ? "true" : "false")
                                       .arg(m_aoapAttempts));
                      }
                    });
              }
            } catch (const std::exception& e) {
              logError(QString("[RealAndroidAutoService] Exception starting AOAP chain: %1")
                           .arg(e.what())
                           .toStdString());
              aaLogWarning("aoapTrace",
                           QString("%1 stage=aoap-query-chain-start result=exception error=%2")
                               .arg(traceToken)
                               .arg(e.what()));
              m_aoapInProgress = false;
              m_activeAoapQueryChain.reset();
              if (m_deviceDetectionTimer && m_state == ConnectionState::SEARCHING) {
                m_deviceDetectionTimer->start();
              }
            }

            // Guard against too many attempts
            armAoapRetryResetWindowIfNeeded();
            aaLogDebug("checkForConnectedDevices",
                       QString("AOAP attempt incremented to %1").arg(aoapAttemptNumber));
            Logger::instance().debug(
                QString("[RealAndroidAutoService] AOAP attempt %1 started").arg(aoapAttemptNumber));
          } else {
            Logger::instance().warning(
                "[RealAndroidAutoService] Query chain factory or strand not available");
            aaLogWarning(
                "aoapTrace",
                QString("%1 stage=aoap-query-chain-skip reason=factory-or-ioservice-missing")
                    .arg(traceToken));
            m_aoapInProgress = false;
          }
        } else {
          Logger::instance().warning(
              QString("[RealAndroidAutoService] Failed to open device for AOAP (result=%1)")
                  .arg(openResult));
          aaLogWarning(
              "aoapTrace",
              QString("%1 stage=aoap-open result=failed code=%2").arg(traceToken).arg(openResult));
        }
      }
    }

    // Log summary at end of check (periodically)
    if (checkCount <= 3 || checkCount % 10 == 0) {
      if (foundAndroidCandidate) {
        logInfo(QString("[RealAndroidAutoService] Check #%1 summary: Found %2 Android USB "
                        "candidate(s) (%3 in AOAP mode), filtered(allow=%4 deny=%5), AOAP in "
                        "progress: %6, attempts: %7/%8")
                    .arg(checkCount)
                    .arg(androidCandidateCount)
                    .arg(foundAoapDevice ? "some" : "none")
                    .arg(filteredByAllowListCount)
                    .arg(filteredByDenyListCount)
                    .arg(m_aoapInProgress ? "yes" : "no")
                    .arg(m_aoapAttempts)
                    .arg(m_aoapMaxAttempts)
                    .toStdString());
      } else {
        logInfo(QString("[RealAndroidAutoService] Check #%1: No Android USB candidates detected "
                        "(filtered allow=%2 deny=%3)")
                    .arg(checkCount)
                    .arg(filteredByAllowListCount)
                    .arg(filteredByDenyListCount)
                    .toStdString());
      }
    }
  } catch (const std::exception& e) {
    Logger::instance().debug(
        QString("[RealAndroidAutoService] Device check error: %1").arg(e.what()));
  }
}

void RealAndroidAutoService::onVideoFrame(const uint8_t* data, int size, int width, int height) {
  if (!isConnected()) {
    return;
  }

  emit videoFrameReady(width, height, data, size);
  updateStats();
}

void RealAndroidAutoService::onAudioData(const QByteArray& data) {
  if (!isConnected() || !m_audioEnabled) {
    return;
  }

  emit audioDataReady(data);
}

void RealAndroidAutoService::startControlPingLoop() {
  aaLogDebug("control",
             "Outbound control ping loop disabled; relying on phone-initiated keepalive");
}

void RealAndroidAutoService::stopControlPingLoop() {
  Q_UNUSED(this)
}

void RealAndroidAutoService::sendControlPingRequest() {
  aaLogDebug("control",
             "Ignoring outbound ping request trigger because host-side pings are disabled");
}

void RealAndroidAutoService::updateStats() {
  // TODO: Implement proper stats tracking
  emit statsUpdated(m_fps, m_latency, m_droppedFrames);
}

auto RealAndroidAutoService::isMediaAudioReady() const -> bool {
  if (!m_channelConfig.mediaAudioEnabled || !m_audioEnabled) {
    return true;
  }

  return m_mediaAudioConfigured && m_mediaAudioStarted && m_mediaAudioFrameReceived;
}

auto RealAndroidAutoService::isProjectionReady() const -> bool {
  const bool videoReady =
      !m_channelConfig.videoEnabled ||
      (m_videoChannelOpened && m_videoConfigured && m_videoStarted && m_videoFrameReceived);

  return m_state == ConnectionState::CONNECTED && m_controlVersionReceived &&
         m_serviceDiscoveryCompleted && videoReady;
}

void RealAndroidAutoService::ensureProjectionIdleWatchdogTimer() {
  if (m_projectionIdleWatchdogTimer) {
    return;
  }

  m_projectionIdleWatchdogTimer = new QTimer(this);
  m_projectionIdleWatchdogTimer->setInterval(3000);
  m_projectionIdleWatchdogTimer->setSingleShot(false);

  connect(m_projectionIdleWatchdogTimer, &QTimer::timeout, this, [this]() {
    if (m_state != ConnectionState::CONNECTED || isProjectionReady() || !m_controlVersionReceived ||
        !m_serviceDiscoveryCompleted) {
      stopProjectionIdleWatchdog(QStringLiteral("watchdog_condition_cleared"));
      return;
    }

    ++m_projectionIdleWatchdogTickCount;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs =
        (m_projectionIdleWatchdogStartedMs > 0) ? (nowMs - m_projectionIdleWatchdogStartedMs) : 0;

    QStringList missingMilestones;
    if (m_channelConfig.videoEnabled) {
      if (!m_videoChannelOpened) {
        missingMilestones << QStringLiteral("video_channel_opened");
      }
      if (!m_videoConfigured) {
        missingMilestones << QStringLiteral("video_configured");
      }
      if (!m_videoStarted) {
        missingMilestones << QStringLiteral("video_started");
      }
      if (!m_videoFrameReceived) {
        missingMilestones << QStringLiteral("video_first_frame");
      }
    }

    if (m_channelConfig.mediaAudioEnabled && m_audioEnabled) {
      if (!m_mediaAudioConfigured) {
        missingMilestones << QStringLiteral("media_audio_configured");
      }
      if (!m_mediaAudioStarted) {
        missingMilestones << QStringLiteral("media_audio_started");
      }
      if (!m_mediaAudioFrameReceived) {
        missingMilestones << QStringLiteral("media_audio_first_frame");
      }
    }

    const QString missingSummary =
        missingMilestones.isEmpty() ? QStringLiteral("none") : missingMilestones.join(',');

    const bool hasAnyChannelProgress =
        m_videoChannelOpened || m_videoConfigured || m_videoStarted || m_videoFrameReceived ||
        m_mediaAudioConfigured || m_mediaAudioStarted || m_mediaAudioFrameReceived;
    const bool hasTerminalProjectionProgress = m_videoFrameReceived || m_mediaAudioFrameReceived;
    const bool hasAnyMediaPayloadSeen = m_videoPayloadSeen || m_audioPayloadSeen;

    aaLogWarning("projectionWatchdog",
                 QString("post-discovery idle (elapsed_ms=%1 tick=%2 missing=%3 last_reason=%4)")
                     .arg(elapsedMs)
                     .arg(m_projectionIdleWatchdogTickCount)
                     .arg(missingSummary)
                     .arg(m_projectionIdleWatchdogLastReason));

    const int recoveryWindowMs =
        getBoundedConfigValue("core.android_auto.projection.idle_recovery_ms", 15000, 5000, 180000);
    const int partialProgressRecoveryWindowMs = getBoundedConfigValue(
        "core.android_auto.projection.idle_recovery_partial_progress_ms", 30000, 5000, 240000);
    const int startedStreamRecoveryWindowMs = getBoundedConfigValue(
        "core.android_auto.projection.idle_recovery_started_stream_ms", 120000, 10000, 300000);

    int effectiveRecoveryWindowMs = recoveryWindowMs;
    if (hasAnyChannelProgress) {
      effectiveRecoveryWindowMs =
          std::max(effectiveRecoveryWindowMs, partialProgressRecoveryWindowMs);
    }

    const bool hasStartedStreamWithoutFirstFrame =
        (m_videoStarted && !m_videoFrameReceived) ||
        (m_mediaAudioStarted && !m_mediaAudioFrameReceived);
    if (hasStartedStreamWithoutFirstFrame) {
      effectiveRecoveryWindowMs =
          std::max(effectiveRecoveryWindowMs, startedStreamRecoveryWindowMs);
    }

    const int streamNudgeIntervalMs = getBoundedConfigValue(
        "core.android_auto.projection.stream_nudge_interval_ms", 15000, 3000, 120000);
    const int maxStreamNudges =
        getBoundedConfigValue("core.android_auto.projection.stream_nudge_max_attempts", 3, 0, 12);

    const int preStartNudgeIntervalMs = getBoundedConfigValue(
        "core.android_auto.projection.pre_start_nudge_interval_ms", 5000, 1000, 120000);
    const int maxPreStartNudges = getBoundedConfigValue(
        "core.android_auto.projection.pre_start_nudge_max_attempts", 4, 0, 20);
    const bool compatOpenAutoProfile = isCompatOpenAutoProfileEnabled();
    const bool idleReconnectEnabled = isProjectionIdleReconnectEnabled();

    if (!compatOpenAutoProfile && !hasAnyChannelProgress &&
        m_projectionStreamNudgeCount < maxPreStartNudges &&
        (m_projectionStreamLastNudgeMs == 0 ||
         (nowMs - m_projectionStreamLastNudgeMs) >= preStartNudgeIntervalMs)) {
      m_projectionStreamNudgeCount++;
      m_projectionStreamLastNudgeMs = nowMs;

      // Bump logical epoch for this pre-start nudge attempt so we only count
      // one transport-timeout per attempt even if multiple sends fail.
      ++m_projectionStreamNudgeEpoch;
      const int currentNudgeEpoch = m_projectionStreamNudgeEpoch;

      aaLogWarning("projectionWatchdog",
                   QString("pre-start nudge attempt=%1/%2 epoch=%3 elapsed_ms=%4")
                       .arg(m_projectionStreamNudgeCount)
                       .arg(maxPreStartNudges)
                       .arg(currentNudgeEpoch)
                       .arg(elapsedMs));

      if (m_videoChannel && m_strand) {
        aap_protobuf::service::media::video::message::VideoFocusNotification indication;
        indication.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
        indication.set_unsolicited(false);

        auto promise = aasdk::channel::SendPromise::defer(*m_strand);
        promise->then(
            []() {},
            [this, epoch = currentNudgeEpoch](const aasdk::error::Error& error) {
              if (m_state != ConnectionState::CONNECTED || m_deviceGoneRecoveryScheduled ||
                  m_projectionIdleRecoveryScheduled) {
                return;
              }

              aaLogWarning("projectionWatchdog",
                           QString("pre-start nudge focus indication failed: %1")
                               .arg(QString::fromStdString(error.what())));

              const bool isRecoverableTransportTimeout = isRecoverableUsbTransferTimeout(error);
              if (isRecoverableTransportTimeout) {
                if (m_preStartNudgeLastCountedEpoch != epoch) {
                  ++m_preStartNudgeNative2ConsecutiveTimeouts;
                  m_preStartNudgeLastCountedEpoch = epoch;
                }
              } else {
                m_preStartNudgeNative2ConsecutiveTimeouts = 0;
              }

              if (isRecoverableTransportTimeout) {
                const int native2ThresholdFallback = getBoundedConfigValue(
                    "core.android_auto.projection.pre_start_nudge_native2_fast_reconnect_threshold",
                    2, 1, 10);
                const int transportTimeoutFastReconnectThreshold = getBoundedConfigValue(
                    "core.android_auto.projection.pre_start_nudge_transport_timeout_fast_reconnect_"
                    "threshold",
                    native2ThresholdFallback, 1, 10);
                aaLogWarning(
                    "projectionWatchdog",
                    QString("pre-start nudge timeout consecutive=%1 threshold=%2 native=%3 (focus)")
                        .arg(m_preStartNudgeNative2ConsecutiveTimeouts)
                        .arg(transportTimeoutFastReconnectThreshold)
                        .arg(error.getNativeCode()));

                if (m_preStartNudgeNative2ConsecutiveTimeouts >=
                        transportTimeoutFastReconnectThreshold &&
                    !m_deviceGoneRecoveryScheduled && !m_projectionIdleRecoveryScheduled) {
                  aaLogWarning(
                      "projectionWatchdog",
                      QString("triggering fast reconnect after repeated pre-start timeout failures "
                              "(focus, consecutive=%1 native=%2)")
                          .arg(m_preStartNudgeNative2ConsecutiveTimeouts)
                          .arg(error.getNativeCode()));
                  onChannelError(QStringLiteral("projectionWatchdog"),
                                 QString::fromStdString(error.what()));
                }
              }
            });
        m_videoChannel->sendVideoFocusIndication(indication, std::move(promise));
      }

      if (m_controlChannel && m_strand) {
        aap_protobuf::service::control::message::AudioFocusNotification response;
        response.set_focus_state(aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);

        auto promise = aasdk::channel::SendPromise::defer(*m_strand);
        promise->then(
            []() {},
            [this, epoch = currentNudgeEpoch](const aasdk::error::Error& error) {
              if (m_state != ConnectionState::CONNECTED || m_deviceGoneRecoveryScheduled ||
                  m_projectionIdleRecoveryScheduled) {
                return;
              }

              aaLogWarning("projectionWatchdog", QString("pre-start nudge audio focus failed: %1")
                                                     .arg(QString::fromStdString(error.what())));

              const bool isRecoverableTransportTimeout = isRecoverableUsbTransferTimeout(error);
              if (isRecoverableTransportTimeout) {
                if (m_preStartNudgeLastCountedEpoch != epoch) {
                  ++m_preStartNudgeNative2ConsecutiveTimeouts;
                  m_preStartNudgeLastCountedEpoch = epoch;
                }
              } else {
                m_preStartNudgeNative2ConsecutiveTimeouts = 0;
              }

              if (isRecoverableTransportTimeout) {
                const int native2ThresholdFallback = getBoundedConfigValue(
                    "core.android_auto.projection.pre_start_nudge_native2_fast_reconnect_threshold",
                    2, 1, 10);
                const int transportTimeoutFastReconnectThreshold = getBoundedConfigValue(
                    "core.android_auto.projection.pre_start_nudge_transport_timeout_fast_reconnect_"
                    "threshold",
                    native2ThresholdFallback, 1, 10);
                aaLogWarning(
                    "projectionWatchdog",
                    QString("pre-start nudge timeout consecutive=%1 threshold=%2 native=%3 (audio)")
                        .arg(m_preStartNudgeNative2ConsecutiveTimeouts)
                        .arg(transportTimeoutFastReconnectThreshold)
                        .arg(error.getNativeCode()));

                if (m_preStartNudgeNative2ConsecutiveTimeouts >=
                        transportTimeoutFastReconnectThreshold &&
                    !m_deviceGoneRecoveryScheduled && !m_projectionIdleRecoveryScheduled) {
                  aaLogWarning(
                      "projectionWatchdog",
                      QString("triggering fast reconnect after repeated pre-start timeout failures "
                              "(audio, consecutive=%1 native=%2)")
                          .arg(m_preStartNudgeNative2ConsecutiveTimeouts)
                          .arg(error.getNativeCode()));
                  onChannelError(QStringLiteral("projectionWatchdog"),
                                 QString::fromStdString(error.what()));
                }
              }
            });
        m_controlChannel->sendAudioFocusResponse(response, std::move(promise));
      }
    }

    if (hasStartedStreamWithoutFirstFrame && !hasAnyMediaPayloadSeen) {
      // Keep receive handlers armed while waiting for the first payload.
      if (m_videoChannel && m_videoEventHandler) {
        m_videoChannel->receive(m_videoEventHandler);
      }
      if (m_mediaAudioChannel && m_mediaAudioEventHandler) {
        m_mediaAudioChannel->receive(m_mediaAudioEventHandler);
      }
      if (m_systemAudioChannel && m_systemAudioEventHandler) {
        m_systemAudioChannel->receive(m_systemAudioEventHandler);
      }
      if (m_speechAudioChannel && m_speechAudioEventHandler) {
        m_speechAudioChannel->receive(m_speechAudioEventHandler);
      }
      if (m_telephonyAudioChannel && m_telephonyAudioEventHandler) {
        m_telephonyAudioChannel->receive(m_telephonyAudioEventHandler);
      }

      if (!compatOpenAutoProfile && m_projectionStreamNudgeCount < maxStreamNudges &&
          (m_projectionStreamLastNudgeMs == 0 ||
           (nowMs - m_projectionStreamLastNudgeMs) >= streamNudgeIntervalMs)) {
        m_projectionStreamNudgeCount++;
        m_projectionStreamLastNudgeMs = nowMs;

        aaLogWarning(
            "projectionWatchdog",
            QString("stream nudge attempt=%1/%2 elapsed_ms=%3 videoStarted=%4 "
                    "audioStarted=%5 payloadSeen=%6")
                .arg(m_projectionStreamNudgeCount)
                .arg(maxStreamNudges)
                .arg(elapsedMs)
                .arg(m_videoStarted ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(m_mediaAudioStarted ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(hasAnyMediaPayloadSeen ? QStringLiteral("true") : QStringLiteral("false")));

        if (m_videoChannel && m_strand) {
          aap_protobuf::service::media::video::message::VideoFocusNotification indication;
          indication.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
          indication.set_unsolicited(false);

          auto promise = aasdk::channel::SendPromise::defer(*m_strand);
          promise->then([]() {},
                        [this](const aasdk::error::Error& error) {
                          aaLogWarning("projectionWatchdog",
                                       QString("stream nudge focus indication failed: %1")
                                           .arg(QString::fromStdString(error.what())));
                        });
          m_videoChannel->sendVideoFocusIndication(indication, std::move(promise));
        }

        if (m_controlChannel && m_strand) {
          aap_protobuf::service::control::message::AudioFocusNotification response;
          response.set_focus_state(aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);

          auto promise = aasdk::channel::SendPromise::defer(*m_strand);
          promise->then([]() {},
                        [this](const aasdk::error::Error& error) {
                          aaLogWarning("projectionWatchdog",
                                       QString("stream nudge audio focus failed: %1")
                                           .arg(QString::fromStdString(error.what())));
                        });
          m_controlChannel->sendAudioFocusResponse(response, std::move(promise));
        }
      }
    }

    const int maxRecoveryAttempts =
        getBoundedConfigValue("core.android_auto.projection.idle_recovery_max_attempts", 2, 0, 5);

    if (!idleReconnectEnabled && elapsedMs >= effectiveRecoveryWindowMs &&
        (elapsedMs - m_projectionIdleWatchdogTimer->interval()) < effectiveRecoveryWindowMs) {
      aaLogInfo("projectionWatchdog",
                QString("idle reconnect suppressed after post-discovery idle "
                        "(elapsed_ms=%1 effective_window_ms=%2 compat_profile=%3)")
                    .arg(elapsedMs)
                    .arg(effectiveRecoveryWindowMs)
                    .arg(compatOpenAutoProfile ? QStringLiteral("true") : QStringLiteral("false")));
    }

    if (idleReconnectEnabled && elapsedMs >= effectiveRecoveryWindowMs &&
        !hasTerminalProjectionProgress && !hasAnyMediaPayloadSeen &&
        !m_projectionIdleRecoveryScheduled && m_projectionIdleRecoveryCount < maxRecoveryAttempts) {
      m_projectionIdleRecoveryScheduled = true;
      ++m_projectionIdleRecoveryCount;

      aaLogWarning(
          "projectionWatchdog",
          QString(
              "forcing reconnect after post-discovery idle (elapsed_ms=%1 effective_window_ms=%2 "
              "recovery=%3/%4 any_progress=%5 terminal_progress=%6 started_stream=%7 "
              "payload_seen=%8)")
              .arg(elapsedMs)
              .arg(effectiveRecoveryWindowMs)
              .arg(m_projectionIdleRecoveryCount)
              .arg(maxRecoveryAttempts)
              .arg(hasAnyChannelProgress ? QStringLiteral("true") : QStringLiteral("false"))
              .arg(hasTerminalProjectionProgress ? QStringLiteral("true") : QStringLiteral("false"))
              .arg(hasStartedStreamWithoutFirstFrame ? QStringLiteral("true")
                                                     : QStringLiteral("false"))
              .arg(hasAnyMediaPayloadSeen ? QStringLiteral("true") : QStringLiteral("false")));

      QTimer::singleShot(250, this, [this]() {
        if (m_state != ConnectionState::CONNECTED) {
          aaLogWarning("projectionWatchdog",
                       QString("recovery skipped: state=%1").arg(connectionStateToString(m_state)));
          m_projectionIdleRecoveryScheduled = false;
          return;
        }

        // Prevent operation-aborted channel error callbacks from starting an
        // overlapping recovery while this watchdog-driven reconnect is in progress.
        m_deviceGoneRecoveryScheduled = true;

        const bool disconnected = disconnect();
        aaLogInfo("projectionWatchdog",
                  QString("recovery disconnect result=%1")
                      .arg(disconnected ? QStringLiteral("success") : QStringLiteral("failed")));

        QTimer::singleShot(500, this, [this]() {
          if (m_state != ConnectionState::DISCONNECTED) {
            aaLogWarning("projectionWatchdog", QString("recovery search restart skipped: state=%1")
                                                   .arg(connectionStateToString(m_state)));
            m_projectionIdleRecoveryScheduled = false;
            m_deviceGoneRecoveryScheduled = false;
            return;
          }

          if (m_deviceDetectionTimer) {
            m_deviceDetectionTimer->stop();
          }

          if (m_usbHub) {
            m_usbHub->cancel();
          }

          m_usbHubDetectionStarted = false;
          m_initialScanTriggered = false;
          m_aoapInProgress = false;
          m_aoapAttempts = 0;
          m_activeAoapQueryChain.reset();
          m_controlVersionRequestAttempts = 0;
          m_controlVersionFirstRequestMs = 0;
          m_controlVersionLastRequestMs = 0;

          cleanupAASDK();

          const int recoveryReinitDelayMs = getBoundedConfigValue(
              "core.android_auto.projection.recovery_reinit_delay_ms", 1500, 200, 15000);
          aaLogInfo("projectionWatchdog",
                    QString("recovery full reinit delay_ms=%1").arg(recoveryReinitDelayMs));

          QTimer::singleShot(recoveryReinitDelayMs, this, [this]() {
            if (m_state == ConnectionState::SEARCHING) {
              m_projectionIdleRecoveryScheduled = false;
              m_deviceGoneRecoveryScheduled = false;
              return;
            }

            try {
              setupAASDK();
            } catch (const std::exception& ex) {
              aaLogWarning("projectionWatchdog",
                           QString("recovery setup failed: %1").arg(ex.what()));
              transitionToState(ConnectionState::ERROR);
              emit errorOccurred(QString("Projection recovery setup failed: %1").arg(ex.what()));
              m_projectionIdleRecoveryScheduled = false;
              m_deviceGoneRecoveryScheduled = false;
              return;
            }

            const bool started = startSearching();
            aaLogInfo("projectionWatchdog",
                      QString("recovery search restart result=%1")
                          .arg(started ? QStringLiteral("success") : QStringLiteral("failed")));
            m_projectionIdleRecoveryScheduled = false;
            m_deviceGoneRecoveryScheduled = false;
          });
        });
      });
    } else if (idleReconnectEnabled && elapsedMs >= effectiveRecoveryWindowMs &&
               hasTerminalProjectionProgress) {
      aaLogInfo("projectionWatchdog",
                QString("reconnect skipped: terminal projection progress observed (elapsed_ms=%1)")
                    .arg(elapsedMs));
    } else if (idleReconnectEnabled && elapsedMs >= effectiveRecoveryWindowMs &&
               hasAnyMediaPayloadSeen) {
      aaLogInfo("projectionWatchdog",
                QString("reconnect skipped: media payload already observed (elapsed_ms=%1)")
                    .arg(elapsedMs));
    }
  });
}

void RealAndroidAutoService::stopProjectionIdleWatchdog(const QString& reason) {
  if (m_projectionIdleWatchdogTimer && m_projectionIdleWatchdogTimer->isActive()) {
    aaLogInfo("projectionWatchdog", QString("stopped reason=%1 elapsed_ms=%2 ticks=%3")
                                        .arg(reason)
                                        .arg((m_projectionIdleWatchdogStartedMs > 0)
                                                 ? (QDateTime::currentMSecsSinceEpoch() -
                                                    m_projectionIdleWatchdogStartedMs)
                                                 : 0)
                                        .arg(m_projectionIdleWatchdogTickCount));
    m_projectionIdleWatchdogTimer->stop();
  }

  m_projectionIdleWatchdogStartedMs = 0;
  m_projectionIdleWatchdogTickCount = 0;
  m_projectionIdleWatchdogLastReason.clear();
  m_projectionIdleRecoveryScheduled = false;
}

void RealAndroidAutoService::updateProjectionIdleWatchdog(const QString& reason) {
  m_projectionIdleWatchdogLastReason = reason;

  const bool shouldWatchdogRun = m_state == ConnectionState::CONNECTED && !isProjectionReady() &&
                                 m_controlVersionReceived && m_serviceDiscoveryCompleted;
  if (!shouldWatchdogRun) {
    stopProjectionIdleWatchdog(QStringLiteral("status_update_not_watchable"));
    return;
  }

  ensureProjectionIdleWatchdogTimer();
  if (!m_projectionIdleWatchdogTimer->isActive()) {
    m_projectionIdleWatchdogStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_projectionIdleWatchdogTickCount = 0;
    aaLogInfo(
        "projectionWatchdog",
        QString("started reason=%1 interval_ms=3000").arg(m_projectionIdleWatchdogLastReason));
    m_projectionIdleWatchdogTimer->start();
  }
}

void RealAndroidAutoService::publishProjectionStatus(const QString& reason) {
  const bool videoReady =
      !m_channelConfig.videoEnabled ||
      (m_videoChannelOpened && m_videoConfigured && m_videoStarted && m_videoFrameReceived);
  const bool audioReady = isMediaAudioReady();
  const bool projectionReady = isProjectionReady();

  QJsonObject status;
  status[QStringLiteral("reason")] = reason;
  status[QStringLiteral("connection_state")] = static_cast<int>(m_state);
  status[QStringLiteral("connection_state_name")] = connectionStateToString(m_state);
  status[QStringLiteral("control_version_received")] = m_controlVersionReceived;
  status[QStringLiteral("service_discovery_completed")] = m_serviceDiscoveryCompleted;
  status[QStringLiteral("video_enabled")] = m_channelConfig.videoEnabled;
  status[QStringLiteral("video_channel_opened")] = m_videoChannelOpened;
  status[QStringLiteral("video_configured")] = m_videoConfigured;
  status[QStringLiteral("video_started")] = m_videoStarted;
  status[QStringLiteral("video_frame_received")] = m_videoFrameReceived;
  status[QStringLiteral("video_ready")] = videoReady;
  status[QStringLiteral("media_audio_enabled")] = m_channelConfig.mediaAudioEnabled;
  status[QStringLiteral("media_audio_configured")] = m_mediaAudioConfigured;
  status[QStringLiteral("media_audio_started")] = m_mediaAudioStarted;
  status[QStringLiteral("media_audio_frame_received")] = m_mediaAudioFrameReceived;
  status[QStringLiteral("media_audio_ready")] = audioReady;
  status[QStringLiteral("system_audio_enabled")] = m_channelConfig.systemAudioEnabled;
  status[QStringLiteral("guidance_audio_enabled")] = m_channelConfig.speechAudioEnabled;
  status[QStringLiteral("telephony_audio_enabled")] = m_channelConfig.telephonyAudioEnabled;
  status[QStringLiteral("input_enabled")] = m_channelConfig.inputEnabled;
  status[QStringLiteral("sensor_enabled")] = m_channelConfig.sensorEnabled;
  status[QStringLiteral("microphone_enabled")] = m_channelConfig.microphoneEnabled;
  status[QStringLiteral("projection_ready")] = projectionReady;
  status[QStringLiteral("timestamp")] = QDateTime::currentSecsSinceEpoch();

  if (projectionReady != m_lastProjectionReady) {
    aaLogInfo("projectionStatus",
              QString("projection_ready=%1 reason=%2")
                  .arg(projectionReady ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(reason));
    m_lastProjectionReady = projectionReady;
  }

  emit projectionStatusChanged(status);
  updateProjectionIdleWatchdog(reason);
}

void RealAndroidAutoService::resetProjectionStatus(const QString& reason) {
  m_controlVersionReceived = false;
  m_serviceDiscoveryCompleted = false;
  m_videoChannelOpened = false;
  m_videoConfigured = false;
  m_videoStarted = false;
  m_videoFrameReceived = false;
  m_mediaAudioConfigured = false;
  m_mediaAudioStarted = false;
  m_mediaAudioFrameReceived = false;
  m_videoPayloadSeen = false;
  m_audioPayloadSeen = false;
  m_projectionIdleRecoveryScheduled = false;
  m_projectionIdleRecoveryCount = 0;
  m_projectionStreamNudgeCount = 0;
  m_projectionStreamLastNudgeMs = 0;
  m_preStartNudgeNative2ConsecutiveTimeouts = 0;
  m_projectionStreamNudgeEpoch = 0;
  m_preStartNudgeLastCountedEpoch = 0;
  m_postDiscoveryNoDeviceGraceRecoveryCount = 0;
  m_controlHandshakeAwaitingActivation = false;
  m_controlHandshakeStartedMs = 0;
  ++m_controlHandshakeEpoch;
  m_controlHandshakeActivationRetryCount = 0;
  m_lastProjectionReady = false;
  m_channelReceiveArmTraceKeys.clear();
  publishProjectionStatus(reason);
}

void RealAndroidAutoService::transitionToState(ConnectionState newState) {
  if (m_state == newState) {
    aaLogDebug(
        "transitionToState",
        QString("state unchanged (%1), transition ignored").arg(connectionStateToString(newState)));
    return;
  }

  const ConnectionState previousState = m_state;
  m_state = newState;
  aaLogInfo("transitionToState",
            QString("%1 -> %2")
                .arg(connectionStateToString(previousState), connectionStateToString(newState)));
  emit connectionStateChanged(newState);
  publishProjectionStatus(QStringLiteral("connection_state_changed"));

  // Map connection state to session state
  switch (newState) {
    case ConnectionState::CONNECTING:
    case ConnectionState::AUTHENTICATING:
    case ConnectionState::SECURING:
      transitionToSessionState(SessionState::NEGOTIATING);
      break;
    case ConnectionState::CONNECTED:
      transitionToSessionState(SessionState::ACTIVE);
      break;
    case ConnectionState::DISCONNECTED:
      if (m_sessionState == SessionState::ACTIVE || m_sessionState == SessionState::NEGOTIATING) {
        transitionToSessionState(SessionState::ENDED);
      }
      break;
    case ConnectionState::ERROR:
      transitionToSessionState(SessionState::ERROR);
      break;
    default:
      break;
  }
}

void RealAndroidAutoService::transitionToSessionState(SessionState newState) {
  if (m_sessionState == newState) {
    return;
  }

  const SessionState oldState = m_sessionState;
  m_sessionState = newState;

  const QString stateStr = sessionStateToString(newState);
  Logger::instance().info(QString("[RealAndroidAutoService] Session state: %1 -> %2")
                              .arg(sessionStateToString(oldState), stateStr));

  // Update session in database
  if (!m_currentSessionId.isEmpty() && m_sessionStore) {
    if (!m_sessionStore->updateSessionState(m_currentSessionId, stateStr.toLower())) {
      Logger::instance().warning(
          QString("[RealAndroidAutoService] Failed to update session state: %1")
              .arg(m_currentSessionId));
    }
  }

  // Emit EventBus event for WebSocket subscribers
  if (m_eventBus) {
    QVariantMap payload;
    payload[QStringLiteral("session_id")] = m_currentSessionId;
    payload[QStringLiteral("state")] = stateStr;
    payload[QStringLiteral("device_id")] = m_currentDeviceId;
    payload[QStringLiteral("timestamp")] = QDateTime::currentSecsSinceEpoch();

    m_eventBus->publish(QStringLiteral("android-auto/status/state-changed"), payload);

    // Emit specific events for important state transitions
    if (newState == SessionState::ACTIVE) {
      m_eventBus->publish(QStringLiteral("android-auto/status/connected"), payload);
      Logger::instance().info(
          "[RealAndroidAutoService] Emitted android-auto/status/connected event");
    } else if (newState == SessionState::ENDED || newState == SessionState::ERROR) {
      m_eventBus->publish(QStringLiteral("android-auto/status/disconnected"), payload);
      Logger::instance().info(
          "[RealAndroidAutoService] Emitted android-auto/status/disconnected event");
    }
  }

  // Emit local signal
  emit sessionStateChanged(m_currentSessionId, stateStr);

  // Manage heartbeat timer
  if (newState == SessionState::ACTIVE && !m_heartbeatTimer->isActive()) {
    m_heartbeatTimer->start();
    updateSessionHeartbeat();
  } else if (newState != SessionState::ACTIVE && m_heartbeatTimer->isActive()) {
    m_heartbeatTimer->stop();
  }

  // End session on terminal states
  if (newState == SessionState::ENDED || newState == SessionState::ERROR) {
    endCurrentSession();
  }
}

QString RealAndroidAutoService::sessionStateToString(SessionState state) const {
  switch (state) {
    case SessionState::NEGOTIATING:
      return QStringLiteral("negotiating");
    case SessionState::ACTIVE:
      return QStringLiteral("active");
    case SessionState::SUSPENDED:
      return QStringLiteral("suspended");
    case SessionState::ENDED:
      return QStringLiteral("ended");
    case SessionState::ERROR:
      return QStringLiteral("error");
    default:
      return QStringLiteral("unknown");
  }
}

void RealAndroidAutoService::createSessionForDevice(const QString& deviceId) {
  if (!m_sessionStore) {
    Logger::instance().error("[RealAndroidAutoService] SessionStore not available");
    return;
  }

  QString resolvedDeviceId = deviceId.trimmed();
  if (resolvedDeviceId.isEmpty()) {
    resolvedDeviceId = m_device.serialNumber.trimmed();
  }
  if (resolvedDeviceId.isEmpty()) {
    resolvedDeviceId = QStringLiteral("AA_DEVICE_REAL");
    Logger::instance().warning(
        "[RealAndroidAutoService] Device ID missing during session creation; using fallback ID");
  }

  if (m_device.serialNumber.trimmed().isEmpty()) {
    m_device.serialNumber = resolvedDeviceId;
  }
  if (m_device.model.trimmed().isEmpty()) {
    m_device.model = QStringLiteral("Android Device");
  }
  if (m_device.androidVersion.trimmed().isEmpty()) {
    m_device.androidVersion = QStringLiteral("Unknown");
  }

  QVariantMap persistedDeviceInfo;
  persistedDeviceInfo["model"] = m_device.model;
  persistedDeviceInfo["android_version"] = m_device.androidVersion;
  persistedDeviceInfo["connection_type"] =
      (m_transportMode == TransportMode::Wireless || m_wirelessEnabled) ? "wireless" : "wired";
  persistedDeviceInfo["paired"] = true;
  persistedDeviceInfo["capabilities"] = QVariantList{"media", "maps"};

  if (!m_sessionStore->createDevice(resolvedDeviceId, persistedDeviceInfo) &&
      !m_sessionStore->updateDeviceLastSeen(resolvedDeviceId)) {
    Logger::instance().warning(
        QString("[RealAndroidAutoService] Failed to upsert device metadata for: %1")
            .arg(resolvedDeviceId));
  }

  // End any existing session
  endCurrentSession();

  // Generate new session ID
  m_currentSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_currentDeviceId = resolvedDeviceId;

  // Create session in database
  const bool created =
      m_sessionStore->createSession(m_currentSessionId, resolvedDeviceId,
                                    sessionStateToString(SessionState::NEGOTIATING).toLower());

  if (created) {
    Logger::instance().info(QString("[RealAndroidAutoService] Created session: %1 for device: %2")
                                .arg(m_currentSessionId, resolvedDeviceId));
  } else {
    Logger::instance().error(
        QString("[RealAndroidAutoService] Failed to create session for device: %1")
            .arg(resolvedDeviceId));
  }
}

void RealAndroidAutoService::endCurrentSession() {
  if (m_currentSessionId.isEmpty() || !m_sessionStore) {
    return;
  }

  m_heartbeatTimer->stop();
  if (!m_sessionStore->endSession(m_currentSessionId)) {
    Logger::instance().warning(
        QString("[RealAndroidAutoService] Failed to end session: %1").arg(m_currentSessionId));
  }

  Logger::instance().info(
      QString("[RealAndroidAutoService] Ended session: %1").arg(m_currentSessionId));

  m_currentSessionId.clear();
  m_currentDeviceId.clear();
}

void RealAndroidAutoService::updateSessionHeartbeat() {
  if (m_currentSessionId.isEmpty() || !m_sessionStore) {
    return;
  }

  if (m_sessionState == SessionState::ACTIVE) {
    if (!m_sessionStore->updateSessionHeartbeat(m_currentSessionId)) {
      Logger::instance().debug(
          QString("[RealAndroidAutoService] Failed to update heartbeat for session: %1")
              .arg(m_currentSessionId));
    }
  }
}

void RealAndroidAutoService::onVideoChannelUpdate(const QByteArray& data, int width, int height) {
  if (!m_channelConfig.videoEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_videoChannelUpdateCount, &m_lastVideoChannelDebugMs)) {
    aaLogDebug("videoChannel",
               QString("sample=%1 bytes=%2 resolution=%3x%4 decoderReady=%5 "
                       "payloadCount=%6 decodeSubmit=%7 decodeReject=%8 decodedFrames=%9")
                   .arg(m_videoChannelUpdateCount)
                   .arg(data.size())
                   .arg(width)
                   .arg(height)
                   .arg((m_videoDecoder && m_videoDecoder->isReady()) ? QStringLiteral("yes")
                                                                      : QStringLiteral("no"))
                   .arg(m_videoPayloadCount)
                   .arg(m_videoDecodeSubmitCount)
                   .arg(m_videoDecodeRejectCount)
                   .arg(m_videoDecodedFrameCount));
  }

  m_videoPayloadCount++;

  // H.264 video data from Android device
  if (m_videoDecoder && m_videoDecoder->isReady()) {
    // Decode H.264 to RGBA using GStreamer
    aasdk::common::Data h264Data;
    h264Data.resize(data.size());
    std::copy(data.begin(), data.end(), h264Data.begin());

    // Convert aasdk::common::Data to QByteArray
    QByteArray frameData(reinterpret_cast<const char*>(h264Data.data()), h264Data.size());
    m_videoDecodeSubmitCount++;
    if (!m_videoDecoder->decodeFrame(frameData)) {
      Logger::instance().warning("Failed to decode video frame");
      aaLogWarning("videoDecoder",
                   QString("decodeFrame rejected bytes=%1 submitCount=%2 rejectCount=%3")
                       .arg(frameData.size())
                       .arg(m_videoDecodeSubmitCount)
                       .arg(m_videoDecodeRejectCount + 1));
      m_videoDecodeRejectCount++;
      m_droppedFrames++;
    }
  } else {
    Logger::instance().warning(
        QString("Dropping video frame because decoder is not ready (bytes=%1)").arg(data.size()));
    m_droppedFrames++;
  }

  if (!m_videoFrameReceived) {
    m_videoFrameReceived = true;
    const qint64 sinceDiscoveryMs =
        m_projectionIdleWatchdogStartedMs > 0
            ? (QDateTime::currentMSecsSinceEpoch() - m_projectionIdleWatchdogStartedMs)
            : -1;
    aaLogInfo("videoChannel",
              QString("First video payload accepted bytes=%1 since_discovery_ms=%2 "
                      "videoStarted=%3 decoderReady=%4 decodeSubmit=%5 decodeReject=%6")
                  .arg(data.size())
                  .arg(sinceDiscoveryMs)
                  .arg(m_videoStarted ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg((m_videoDecoder && m_videoDecoder->isReady()) ? QStringLiteral("true")
                                                                     : QStringLiteral("false"))
                  .arg(m_videoDecodeSubmitCount)
                  .arg(m_videoDecodeRejectCount));
    publishProjectionStatus(QStringLiteral("video_first_frame"));
  }

  updateStats();
}

void RealAndroidAutoService::onMediaAudioChannelUpdate(const QByteArray& data) {
  if (!m_channelConfig.mediaAudioEnabled || !m_audioEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_mediaAudioUpdateCount, &m_lastMediaAudioDebugMs)) {
    aaLogDebug("mediaAudioChannel",
               QString("sample=%1 bytes=%2 mixer=%3 audioEnabled=%4 payloadCount=%5 mixCount=%6")
                   .arg(m_mediaAudioUpdateCount)
                   .arg(data.size())
                   .arg(m_audioMixer ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(m_audioEnabled ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(m_mediaAudioPayloadCount)
                   .arg(m_mediaAudioMixCount));
  }

  m_mediaAudioPayloadCount++;

  // PCM audio data from Android device (music playback)
  // Route to vehicle audio system via AudioRouter
  routeMediaAudioToVehicle(data);

  if (!m_mediaAudioFrameReceived) {
    m_mediaAudioFrameReceived = true;
    const qint64 sinceDiscoveryMs =
        m_projectionIdleWatchdogStartedMs > 0
            ? (QDateTime::currentMSecsSinceEpoch() - m_projectionIdleWatchdogStartedMs)
            : -1;
    aaLogInfo("mediaAudioChannel",
              QString("First media audio payload accepted bytes=%1 since_discovery_ms=%2 "
                      "mediaStarted=%3 mixer=%4")
                  .arg(data.size())
                  .arg(sinceDiscoveryMs)
                  .arg(m_mediaAudioStarted ? QStringLiteral("true") : QStringLiteral("false"))
                  .arg(m_audioMixer ? QStringLiteral("true") : QStringLiteral("false")));
    publishProjectionStatus(QStringLiteral("media_audio_first_frame"));
  }

  if (m_audioMixer) {
    m_mediaAudioMixCount++;
    m_audioMixer->mixAudioData(IAudioMixer::ChannelId::MEDIA, data);
    Logger::instance().debug(QString("Media audio mixed: %1 bytes").arg(data.size()));
  } else {
    // Fallback: emit raw audio
    emit audioDataReady(data);
    Logger::instance().debug(QString("Media audio: %1 bytes").arg(data.size()));
  }
}

void RealAndroidAutoService::onSystemAudioChannelUpdate(const QByteArray& data) {
  if (!m_channelConfig.systemAudioEnabled || !m_audioEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_systemAudioUpdateCount, &m_lastSystemAudioDebugMs)) {
    aaLogDebug("systemAudioChannel",
               QString("sample=%1 bytes=%2 mixer=%3 audioEnabled=%4")
                   .arg(m_systemAudioUpdateCount)
                   .arg(data.size())
                   .arg(m_audioMixer ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(m_audioEnabled ? QStringLiteral("yes") : QStringLiteral("no")));
  }

  // PCM audio data from Android device (system sounds, notifications)
  // Route to vehicle audio system via AudioRouter
  routeSystemAudioToVehicle(data);

  if (m_audioMixer) {
    m_audioMixer->mixAudioData(IAudioMixer::ChannelId::SYSTEM, data);
    Logger::instance().debug(QString("System audio mixed: %1 bytes").arg(data.size()));
  } else {
    // Fallback: emit raw audio
    emit audioDataReady(data);
    Logger::instance().debug(QString("System audio: %1 bytes").arg(data.size()));
  }
}

void RealAndroidAutoService::onSpeechAudioChannelUpdate(const QByteArray& data) {
  if (!m_channelConfig.speechAudioEnabled || !m_audioEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_speechAudioUpdateCount, &m_lastSpeechAudioDebugMs)) {
    aaLogDebug("speechAudioChannel",
               QString("sample=%1 bytes=%2 mixer=%3 audioEnabled=%4")
                   .arg(m_speechAudioUpdateCount)
                   .arg(data.size())
                   .arg(m_audioMixer ? QStringLiteral("yes") : QStringLiteral("no"))
                   .arg(m_audioEnabled ? QStringLiteral("yes") : QStringLiteral("no")));
  }

  // PCM audio data from Android device (navigation guidance, voice assistant)
  // Route to vehicle audio system via AudioRouter with ducking support
  routeGuidanceAudioToVehicle(data);

  if (m_audioMixer) {
    m_audioMixer->mixAudioData(IAudioMixer::ChannelId::SPEECH, data);
    Logger::instance().debug(QString("Speech audio mixed: %1 bytes").arg(data.size()));
  } else {
    // Fallback: emit raw audio
    emit audioDataReady(data);
    Logger::instance().debug(QString("Speech audio: %1 bytes").arg(data.size()));
  }
}

void RealAndroidAutoService::onSensorRequest() {
  if (!m_channelConfig.sensorEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_sensorRequestCount, &m_lastSensorDebugMs)) {
    aaLogDebug("sensorChannel", QString("sample=%1 request received").arg(m_sensorRequestCount));
  }

  // Android device is requesting sensor data (GPS, speed, night mode, etc)
  // TODO: Implement sensor data collection and transmission
  Logger::instance().debug("Sensor data requested by Android device");
}

void RealAndroidAutoService::onBluetoothPairingRequest(const QString& deviceName) {
  if (!m_channelConfig.bluetoothEnabled) {
    return;
  }

  if (shouldEmitChannelDebugSample(&m_bluetoothPairingRequestCount, &m_lastBluetoothDebugMs)) {
    aaLogDebug("bluetoothChannel", QString("sample=%1 pairing request device=%2")
                                       .arg(m_bluetoothPairingRequestCount)
                                       .arg(deviceName));
  }

  // Android device is requesting Bluetooth pairing
  Logger::instance().info(QString("Bluetooth pairing requested: %1").arg(deviceName));
  // TODO: Implement Bluetooth pairing flow
}

void RealAndroidAutoService::onChannelError(const QString& channelName, const QString& error) {
  if (m_aasdkTeardownInProgress) {
    aaLogInfo("channelError", QString("ignoring channel=%1 while teardown is in progress: %2")
                                  .arg(channelName)
                                  .arg(error));
    return;
  }

  if (channelName == QStringLiteral("control")) {
    m_controlHandshakeAwaitingActivation = false;
    m_controlHandshakeStartedMs = 0;
    ++m_controlHandshakeEpoch;
    m_controlHandshakeActivationRetryCount = 0;
    traceControlEvent(QStringLiteral("control_channel_error"), error);
    dumpControlTrace(QStringLiteral("channel_error_control"));
  }

  const bool isTransferTimeout = isUsbTransferTimeoutErrorText(error);
  const bool isNoDevice =
      isUsbTransferNoDeviceErrorText(error) || isTransportNoDeviceErrorText(error);
  const bool isSslWrapperNoDevice = isSslWrapperNoDeviceErrorText(error);
  const bool isOperationAborted = isOperationAbortedErrorText(error);
  const bool isControlVersionTimeout =
      channelName == QStringLiteral("control") &&
      error.startsWith(QStringLiteral("Version request timed out after"));
  const bool isControlHandshakeTimeout =
      channelName == QStringLiteral("control") &&
      error.startsWith(QStringLiteral("Handshake activation timed out after"));

  if (channelName == QStringLiteral("video")) {
    m_videoStarted = false;
  } else if (channelName == QStringLiteral("mediaAudio")) {
    m_mediaAudioStarted = false;
  }

  publishProjectionStatus(QStringLiteral("channel_error_%1").arg(channelName));

  const int noDeviceGraceMaxAttempts = getBoundedConfigValue(
      "core.android_auto.usb.post_discovery_no_device_grace_max_attempts", 1, 0, 3);
  const bool inPostDiscoveryPreStartWindow =
      m_serviceDiscoveryCompleted && !m_videoStarted && !m_mediaAudioStarted;

  if (isNoDevice && inPostDiscoveryPreStartWindow &&
      m_postDiscoveryNoDeviceGraceRecoveryCount < noDeviceGraceMaxAttempts &&
      !m_deviceGoneRecoveryScheduled) {
    m_postDiscoveryNoDeviceGraceRecoveryCount++;

    aaLogWarning(
        "channelError",
        QString("channel=%1 state=%2 details=%3 -> post-discovery no-device grace recovery %4/%5")
            .arg(channelName)
            .arg(connectionStateToString(m_state))
            .arg(error)
            .arg(m_postDiscoveryNoDeviceGraceRecoveryCount)
            .arg(noDeviceGraceMaxAttempts));

    QTimer::singleShot(200, this, [this]() {
      if (m_state != ConnectionState::CONNECTED) {
        return;
      }

      const bool stillInPreStartWindow =
          m_serviceDiscoveryCompleted && !m_videoStarted && !m_mediaAudioStarted;

      if (m_controlChannel && m_controlEventHandler) {
        m_controlChannel->receive(m_controlEventHandler);
      }
      if (m_videoChannel && m_videoEventHandler) {
        m_videoChannel->receive(m_videoEventHandler);
      }
      if (m_mediaAudioChannel && m_mediaAudioEventHandler) {
        m_mediaAudioChannel->receive(m_mediaAudioEventHandler);
      }
      if (m_systemAudioChannel && m_systemAudioEventHandler) {
        m_systemAudioChannel->receive(m_systemAudioEventHandler);
      }
      if (m_speechAudioChannel && m_speechAudioEventHandler) {
        m_speechAudioChannel->receive(m_speechAudioEventHandler);
      }
      if (m_telephonyAudioChannel && m_telephonyAudioEventHandler) {
        m_telephonyAudioChannel->receive(m_telephonyAudioEventHandler);
      }
      if (m_inputChannel && m_inputEventHandler) {
        m_inputChannel->receive(m_inputEventHandler);
      }
      if (m_sensorChannel && m_sensorEventHandler) {
        m_sensorChannel->receive(m_sensorEventHandler);
      }
      if (!stillInPreStartWindow && m_microphoneChannel && m_microphoneEventHandler) {
        m_microphoneChannel->receive(m_microphoneEventHandler);
      }

      aaLogInfo("channelError", "post-discovery no-device grace recovery re-armed active receives");
    });

    return;
  }

  if (isTransferTimeout || isNoDevice || isControlVersionTimeout || isControlHandshakeTimeout ||
      isOperationAborted) {
    if (isSslWrapperNoDevice) {
      aaLogWarning("channelError",
                   QString("channel=%1 state=%2 details=%3 -> immediate SSL no-device recovery")
                       .arg(channelName)
                       .arg(connectionStateToString(m_state))
                       .arg(error));
      performImmediateTransportRecovery(QStringLiteral("ssl_wrapper_no_device"));
      return;
    }

    QString recoveryReason;
    if (isNoDevice) {
      recoveryReason = QStringLiteral("device-gone");
    } else if (isControlVersionTimeout) {
      recoveryReason = QStringLiteral("version-timeout");
    } else if (isControlHandshakeTimeout) {
      recoveryReason = QStringLiteral("handshake-timeout");
    } else if (isOperationAborted) {
      recoveryReason = QStringLiteral("operation-aborted");
    } else {
      recoveryReason = QStringLiteral("transfer-timeout");
    }
    aaLogWarning("channelError", QString("channel=%1 state=%2 details=%3 -> %4 recovery path")
                                     .arg(channelName)
                                     .arg(connectionStateToString(m_state))
                                     .arg(error)
                                     .arg(recoveryReason));

    if (!m_deviceGoneRecoveryScheduled) {
      m_deviceGoneRecoveryScheduled = true;

      if (isConnected() || m_state == ConnectionState::CONNECTING) {
        disconnect();
      }

      const int reconnectBaseDelayMs = getBoundedConfigValue(
          "core.android_auto.control.timeout_reconnect_base_delay_ms", 250, 50, 5000);
      const int reconnectJitterMs = getBoundedConfigValue(
          "core.android_auto.control.timeout_reconnect_jitter_ms", 100, 0, 5000);
      const int reconnectDelayMs =
          reconnectBaseDelayMs +
          (reconnectJitterMs > 0 ? QRandomGenerator::global()->bounded(reconnectJitterMs + 1) : 0);

      aaLogInfo("channelError",
                QString("scheduling recovery restart in %1 ms (base=%2 jitter_max=%3)")
                    .arg(reconnectDelayMs)
                    .arg(reconnectBaseDelayMs)
                    .arg(reconnectJitterMs));

      QTimer::singleShot(
          reconnectDelayMs, this, [this, isControlVersionTimeout, isControlHandshakeTimeout]() {
            m_deviceGoneRecoveryScheduled = false;
            if (m_state == ConnectionState::SEARCHING) {
              return;
            }

            if (m_state == ConnectionState::DISCONNECTED) {
              aaLogInfo("channelError",
                        "service already disconnected; continuing with search restart");
            }

            if (isControlVersionTimeout || isControlHandshakeTimeout) {
              const bool configuredFullReinitOnTimeout =
                  ConfigService::instance()
                      .get("core.android_auto.control.full_reinit_on_timeout", false)
                      .toBool();

              m_controlTimeoutRecoveryCount++;
              const int fullReinitEscalationThreshold = getBoundedConfigValue(
                  "core.android_auto.control.timeout_full_reinit_after", 1, 1, 10);
              const int usbResetEscalationThreshold = getBoundedConfigValue(
                  "core.android_auto.control.timeout_force_usb_reset_after", 1, 1, 10);

              const bool escalatedFullReinit =
                  !configuredFullReinitOnTimeout &&
                  m_controlTimeoutRecoveryCount >= fullReinitEscalationThreshold;
              const bool fullReinitOnTimeout = configuredFullReinitOnTimeout || escalatedFullReinit;

              const bool forceCleanupUsbReset =
                  isControlVersionTimeout && fullReinitOnTimeout &&
                  m_controlTimeoutRecoveryCount >= usbResetEscalationThreshold;

              m_forceCleanupUsbResetOnce = forceCleanupUsbReset;

              aaLogInfo(
                  "channelError",
                  QString("control timeout recovery mode=%1 count=%2 full_reinit_threshold=%3 "
                          "usb_reset_threshold=%4 force_usb_reset=%5")
                      .arg(fullReinitOnTimeout ? QStringLiteral("full-reinit")
                                               : QStringLiteral("soft-restart"))
                      .arg(m_controlTimeoutRecoveryCount)
                      .arg(fullReinitEscalationThreshold)
                      .arg(usbResetEscalationThreshold)
                      .arg(forceCleanupUsbReset ? QStringLiteral("true")
                                                : QStringLiteral("false")));

              if (m_deviceDetectionTimer) {
                m_deviceDetectionTimer->stop();
              }

              if (m_usbHub) {
                m_usbHub->cancel();
              }

              m_usbHubDetectionStarted = false;
              m_initialScanTriggered = false;
              m_aoapInProgress = false;
              m_aoapAttempts = 0;
              m_activeAoapQueryChain.reset();
              m_controlVersionRequestAttempts = 0;
              m_controlVersionFirstRequestMs = 0;
              m_controlVersionLastRequestMs = 0;

              const int recoveryReinitDelayMs = getBoundedConfigValue(
                  "core.android_auto.control.recovery_reinit_delay_ms", 5000, 500, 15000);

              if (fullReinitOnTimeout) {
                // Mark teardown early so late channel callbacks cannot schedule
                // overlapping recovery while timeout handling is active.
                m_aasdkTeardownInProgress = true;

                // Full AASDK stack reset is opt-in to preserve legacy behavior.
                cleanupAASDK();

                aaLogInfo("channelError",
                          QString("control timeout recovery (full reinit): delaying by %1 ms")
                              .arg(recoveryReinitDelayMs));

                QTimer::singleShot(recoveryReinitDelayMs, this, [this]() {
                  if (m_state == ConnectionState::SEARCHING) {
                    return;
                  }

                  try {
                    setupAASDK();
                  } catch (const std::exception& ex) {
                    aaLogWarning(
                        "channelError",
                        QString("control timeout recovery setup failed: %1").arg(ex.what()));
                    transitionToState(ConnectionState::ERROR);
                    emit errorOccurred(QString("AASDK recovery setup failed: %1").arg(ex.what()));
                    return;
                  }

                  const bool started = startSearching();
                  aaLogInfo(
                      "channelError",
                      QString("control-timeout full-reinit: restart search started=%1 state=%2")
                          .arg(started ? "true" : "false")
                          .arg(connectionStateToString(m_state)));
                });
              } else {
                // Soft recovery avoids full io_service/libusb teardown races.
                cleanupChannels();

                aaLogInfo("channelError",
                          QString("control timeout recovery (soft restart): delaying by %1 ms")
                              .arg(recoveryReinitDelayMs));

                QTimer::singleShot(recoveryReinitDelayMs, this, [this]() {
                  if (m_state == ConnectionState::SEARCHING) {
                    return;
                  }

                  const bool started = startSearching();
                  aaLogInfo(
                      "channelError",
                      QString("control-timeout soft-restart: restart search started=%1 state=%2")
                          .arg(started ? "true" : "false")
                          .arg(connectionStateToString(m_state)));
                });
              }

              return;
            }

            if (isConnected() || m_state == ConnectionState::CONNECTING) {
              disconnect();
            }

            const bool started = startSearching();
            aaLogInfo("channelError",
                      QString("usb-transfer recovery: restart search started=%1 state=%2")
                          .arg(started ? "true" : "false")
                          .arg(connectionStateToString(m_state)));
          });
    }

    return;
  }

  aaLogWarning("channelError", QString("channel=%1 state=%2 details=%3")
                                   .arg(channelName)
                                   .arg(connectionStateToString(m_state))
                                   .arg(error));
  Logger::instance().error(QString("Channel error [%1]: %2").arg(channelName, error));
  emit errorOccurred(QString("%1 channel error: %2").arg(channelName, error));
}

void RealAndroidAutoService::routeMediaAudioToVehicle(const QByteArray& audioData) {
  if (!m_audioRouter) {
    Logger::instance().debug(
        "[RealAndroidAutoService] AudioRouter not initialised, skipping media audio routing");
    return;
  }

  if (!m_audioRouter->routeAudioFrame(AAudioStreamRole::MEDIA, audioData)) {
    Logger::instance().warning("[RealAndroidAutoService] Failed to route media audio");
  }
}

void RealAndroidAutoService::routeGuidanceAudioToVehicle(const QByteArray& audioData) {
  if (!m_audioRouter) {
    Logger::instance().debug(
        "[RealAndroidAutoService] AudioRouter not initialised, skipping guidance audio routing");
    return;
  }

  // Enable audio ducking when guidance is active (reduces other streams to 40%)
  m_audioRouter->enableAudioDucking(true);

  if (!m_audioRouter->routeAudioFrame(AAudioStreamRole::GUIDANCE, audioData)) {
    Logger::instance().warning("[RealAndroidAutoService] Failed to route guidance audio");
  }
}

void RealAndroidAutoService::routeSystemAudioToVehicle(const QByteArray& audioData) {
  if (!m_audioRouter) {
    Logger::instance().debug(
        "[RealAndroidAutoService] AudioRouter not initialised, skipping system audio routing");
    return;
  }

  if (!m_audioRouter->routeAudioFrame(AAudioStreamRole::SYSTEM_AUDIO, audioData)) {
    Logger::instance().warning("[RealAndroidAutoService] Failed to route system audio");
  }
}
