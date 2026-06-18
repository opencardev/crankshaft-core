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

#include "AudioRouter.h"

#include <unistd.h>

#include <QAudioDevice>
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#include "../../hal/multimedia/MediaPipeline.h"
#include "../logging/Logger.h"

namespace {

QStringList runtimeDirCandidates() {
  QStringList candidates;

  const QString pipewireRuntimeDir = qEnvironmentVariable("PIPEWIRE_RUNTIME_DIR");
  if (!pipewireRuntimeDir.isEmpty()) {
    candidates.append(pipewireRuntimeDir);
  }

  const QString xdgRuntimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!xdgRuntimeDir.isEmpty() && !candidates.contains(xdgRuntimeDir)) {
    candidates.append(xdgRuntimeDir);
  }

  const QString uidRuntimeDir = QStringLiteral("/run/user/%1").arg(static_cast<uint>(geteuid()));
  if (!candidates.contains(uidRuntimeDir)) {
    candidates.append(uidRuntimeDir);
  }

  return candidates;
}

}  // namespace

AudioRouter::AudioRouter(MediaPipeline* mediaPipeline, QObject* parent)
    : QObject(parent), m_mediaPipeline(mediaPipeline) {
  if (!m_mediaPipeline) {
    Logger::instance().error(QStringLiteral("[AudioRouter] MediaPipeline is null"));
    return;
  }

  // Note: QMediaDevices is a static class, not a singleton instance
  // Connect to audio device changes via qt_audio_engine signal
}

AudioRouter::~AudioRouter() {
  shutdown();
}

bool AudioRouter::initialize() {
  if (m_initialized) {
    return true;
  }

  m_lastInitializeAttemptMs = QDateTime::currentMSecsSinceEpoch();

  Logger::instance().info(QStringLiteral("[AudioRouter] Initialising audio router"));
  m_pipewireAvailable = false;
  m_pulseaudioAvailable = false;

  // Try PipeWire first (modern, preferred backend)
  if (initializePipeWire()) {
    m_pipewireAvailable = true;
    Logger::instance().info(QStringLiteral("[AudioRouter] PipeWire backend initialised"));
  }
  // Fallback to PulseAudio
  else if (initializePulseAudio()) {
    m_pulseaudioAvailable = true;
    Logger::instance().info(QStringLiteral("[AudioRouter] PulseAudio backend initialised"));
  } else {
    Logger::instance().warning(QStringLiteral("[AudioRouter] No audio backend available"));
  }

  // Enumerate available devices
  const auto& devices = QMediaDevices::audioOutputs();
  Logger::instance().info(
      QString::asprintf("[AudioRouter] Found %lld audio output devices",
              static_cast<long long>(devices.size())));

  for (const auto& device : devices) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Device: %1 (%2 channels)")
                                 .arg(device.description())
                                 .arg(device.maximumChannelCount()));
  }

  m_initialized = m_pipewireAvailable || m_pulseaudioAvailable;
  return m_initialized;
}

bool AudioRouter::shouldRetryInitialization() const {
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  return (nowMs - m_lastInitializeAttemptMs) >= kInitializeRetryIntervalMs;
}

bool AudioRouter::ensureAudioBackendReady(const char* callerTag) {
  if (m_initialized) {
    return true;
  }

  if (m_lastInitializeAttemptMs == 0 || shouldRetryInitialization()) {
    Logger::instance().info(
        QStringLiteral("[AudioRouter] Retrying backend initialization (%1)")
            .arg(QString::fromUtf8(callerTag)));
    if (initialize()) {
      Logger::instance().info(
          QStringLiteral("[AudioRouter] Backend recovered (%1)")
              .arg(QString::fromUtf8(callerTag)));
      return true;
    }
  }

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if ((nowMs - m_lastInitializeFailureLogMs) >= kInitializeFailureLogIntervalMs) {
    Logger::instance().warning(
        QStringLiteral("[AudioRouter] Backend unavailable; still waiting (%1)")
            .arg(QString::fromUtf8(callerTag)));
    m_lastInitializeFailureLogMs = nowMs;
  }

  return false;
}

bool AudioRouter::initializePipeWire() {
  // Prefer socket presence checks first; this avoids false negatives when
  // helper binaries are unavailable but PipeWire is already running.
  const QStringList runtimeDirs = runtimeDirCandidates();
  for (const QString& runtimeDir : runtimeDirs) {
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    if (QFileInfo::exists(socketPath)) {
      return true;
    }
  }

  // Check if PipeWire daemon is running via pw-cli
  QProcess pwProcess;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (!runtimeDirs.isEmpty()) {
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDirs.first());
    env.insert(QStringLiteral("PIPEWIRE_RUNTIME_DIR"), runtimeDirs.first());
  }
  pwProcess.setProcessEnvironment(env);
  pwProcess.setProgram(QStringLiteral("pw-cli"));
  pwProcess.setArguments(QStringList() << QStringLiteral("info") << QStringLiteral("0"));
  pwProcess.start();

  if (!pwProcess.waitForStarted(1000)) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PipeWire not available"));
    return false;
  }

  const bool finished = pwProcess.waitForFinished(3000);
  const int exitCode = pwProcess.exitCode();

  if (!finished || exitCode != 0) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PipeWire daemon not running"));
    return false;
  }

  return true;
}

bool AudioRouter::initializePulseAudio() {
  // PipeWire Pulse emulation exposes a native Pulse socket. Prefer checking
  // that socket first, then fall back to pactl probing.
  const QString pulseServer = qEnvironmentVariable("PULSE_SERVER");
  if (pulseServer.startsWith(QStringLiteral("unix:"))) {
    const QString pulseSocket = pulseServer.mid(5);
    if (!pulseSocket.isEmpty() && QFileInfo::exists(pulseSocket)) {
      return true;
    }
  } else {
    const QStringList runtimeDirs = runtimeDirCandidates();
    for (const QString& runtimeDir : runtimeDirs) {
      const QString fallbackSocket = runtimeDir + QStringLiteral("/pulse/native");
      if (QFileInfo::exists(fallbackSocket)) {
        return true;
      }
    }
  }

  // Check if PulseAudio daemon is available via pactl
  QProcess paProcess;
  const QStringList runtimeDirs = runtimeDirCandidates();
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (!runtimeDirs.isEmpty()) {
    const QString runtimeDir = runtimeDirs.first();
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
    if (!env.contains(QStringLiteral("PULSE_SERVER"))) {
      env.insert(QStringLiteral("PULSE_SERVER"),
                 QStringLiteral("unix:%1/pulse/native").arg(runtimeDir));
    }
  }
  paProcess.setProcessEnvironment(env);
  paProcess.setProgram(QStringLiteral("pactl"));
  paProcess.setArguments(QStringList() << QStringLiteral("info"));
  paProcess.start();

  if (!paProcess.waitForStarted(1000)) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PulseAudio not available"));
    return false;
  }

  const bool finished = paProcess.waitForFinished(3000);
  const int exitCode = paProcess.exitCode();

  if (!finished || exitCode != 0) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PulseAudio daemon not running"));
    return false;
  }

  return true;
}

bool AudioRouter::routeAudioFrame(AAudioStreamRole role, const QByteArray& audioData) {
  if (!m_mediaPipeline) {
    Logger::instance().error(QStringLiteral("[AudioRouter] MediaPipeline is null"));
    return false;
  }

  if (!ensureAudioBackendReady("route")) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Audio router not initialised"));
    return false;
  }

  if (audioData.isEmpty()) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Empty audio data"));
    return false;
  }

  // Apply ducking if enabled and role is not guidance
  QByteArray processedData = audioData;
  if (m_duckingEnabled && role != AAudioStreamRole::GUIDANCE) {
    // Reduce audio level by applying amplitude scaling
    // duckingLevel is 0-100, apply as volume multiplier
    float volumeFactor = static_cast<float>(m_duckingLevel) / 100.0F;
    int16_t* samples = reinterpret_cast<int16_t*>(processedData.data());
    size_t numSamples = processedData.size() / sizeof(int16_t);

    for (size_t i = 0; i < numSamples; ++i) {
      samples[i] = static_cast<int16_t>(static_cast<float>(samples[i]) * volumeFactor);
    }
  }

  // Route through MediaPipeline audio HAL
  if (!m_mediaPipeline->pushAudioData(processedData)) {
    Logger::instance().error(QStringLiteral("[AudioRouter] Failed to push audio data to pipeline"));
    return false;
  }

  return true;
}

bool AudioRouter::setAudioDevice(AAudioStreamRole role, const QString& deviceId) {
  if (!ensureAudioBackendReady("set-device")) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Audio router not initialised"));
    return false;
  }

  QAudioDevice device;

  if (deviceId.isEmpty()) {
    // Use default device for role
    device = selectAudioDevice(role);
  } else {
    // Find device by ID
    const auto& devices = QMediaDevices::audioOutputs();
    auto it = std::find_if(devices.begin(), devices.end(), [&deviceId](const QAudioDevice& dev) {
      return dev.id() == deviceId.toLatin1();
    });

    if (it == devices.end()) {
      Logger::instance().error(
          QStringLiteral("[AudioRouter] Audio device not found: %1").arg(deviceId));
      return false;
    }

    device = *it;
  }

  // Update appropriate config
  switch (role) {
    case AAudioStreamRole::MEDIA:
      m_mediaConfig.device = device;
      break;
    case AAudioStreamRole::GUIDANCE:
      m_guidanceConfig.device = device;
      break;
    case AAudioStreamRole::SYSTEM_AUDIO:
      m_systemConfig.device = device;
      break;
    default:
      Logger::instance().warning(QStringLiteral("[AudioRouter] Unknown audio stream role"));
      return false;
  }

  Logger::instance().info(QStringLiteral("[AudioRouter] Set audio device for role %1: %2")
                              .arg(static_cast<int>(role))
                              .arg(device.description()));

  return true;
}

QStringList AudioRouter::getAvailableAudioDevices() const {
  QStringList devices;
  const auto& audioOutputs = QMediaDevices::audioOutputs();

  for (const auto& output : audioOutputs) {
    devices.append(output.description());
  }

  return devices;
}

bool AudioRouter::setStreamVolume(AAudioStreamRole role, int volume) {
  if (volume < 0 || volume > 100) {
    Logger::instance().warning(
        QStringLiteral("[AudioRouter] Invalid volume level: %1").arg(volume));
    return false;
  }

  switch (role) {
    case AAudioStreamRole::MEDIA:
      m_mediaConfig.volumeLevel = volume;
      break;
    case AAudioStreamRole::GUIDANCE:
      m_guidanceConfig.volumeLevel = volume;
      break;
    case AAudioStreamRole::SYSTEM_AUDIO:
      m_systemConfig.volumeLevel = volume;
      break;
    default:
      Logger::instance().warning(QStringLiteral("[AudioRouter] Unknown audio stream role"));
      return false;
  }

  // Update MediaPipeline if configured for this role
  if (m_mediaPipeline) {
    m_mediaPipeline->audioHAL()->setVolume(volume);
  }

  Logger::instance().debug(QStringLiteral("[AudioRouter] Set volume for role %1: %2%")
                               .arg(static_cast<int>(role))
                               .arg(volume));

  emit volumeChanged(role, volume);
  return true;
}

bool AudioRouter::setStreamMuted(AAudioStreamRole role, bool muted) {
  switch (role) {
    case AAudioStreamRole::MEDIA:
      m_mediaConfig.muted = muted;
      break;
    case AAudioStreamRole::GUIDANCE:
      m_guidanceConfig.muted = muted;
      break;
    case AAudioStreamRole::SYSTEM_AUDIO:
      m_systemConfig.muted = muted;
      break;
    default:
      Logger::instance().warning(QStringLiteral("[AudioRouter] Unknown audio stream role"));
      return false;
  }

  if (m_mediaPipeline) {
    m_mediaPipeline->audioHAL()->setMute(muted);
  }

  Logger::instance().debug(QStringLiteral("[AudioRouter] %1 audio for role %2")
                               .arg(muted ? QStringLiteral("Muted") : QStringLiteral("Unmuted"))
                               .arg(static_cast<int>(role)));

  return true;
}

bool AudioRouter::enableAudioDucking(bool enable) {
  m_duckingEnabled = enable;
  Logger::instance().info(
      QStringLiteral("[AudioRouter] Audio ducking %1")
          .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
  return true;
}

bool AudioRouter::shutdown() {
  if (!m_initialized) {
    return true;
  }

  Logger::instance().info(QStringLiteral("[AudioRouter] Shutting down audio router"));

  // Stop media pipeline if active
  if (m_mediaPipeline && m_mediaPipeline->isActive()) {
    m_mediaPipeline->stop();
  }

  m_initialized = false;
  m_pipewireAvailable = false;
  m_pulseaudioAvailable = false;
  return true;
}

QAudioDevice AudioRouter::selectAudioDevice(AAudioStreamRole role) const {
  const auto& devices = QMediaDevices::audioOutputs();

  if (devices.isEmpty()) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] No audio devices available"));
    return QAudioDevice();
  }

  // For guidance, prefer primary output device
  if (role == AAudioStreamRole::GUIDANCE) {
    return devices.first();
  }

  // For media and system, try to find headphone or Bluetooth device
  for (const auto& device : devices) {
    const auto& desc = device.description();
    if (desc.contains(QStringLiteral("Headphone"), Qt::CaseInsensitive) ||
        desc.contains(QStringLiteral("Bluetooth"), Qt::CaseInsensitive)) {
      return device;
    }
  }

  // Fallback to first device
  return devices.first();
}

void AudioRouter::onAudioDevicesChanged() {
  Logger::instance().info(QStringLiteral("[AudioRouter] Audio devices changed"));
  emit audioDevicesChanged();
}

void AudioRouter::onPipelineStateChanged(bool isActive) {
  if (isActive) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Media pipeline started"));
  } else {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Media pipeline stopped"));
  }
}

AAudioStreamRole AudioRouter::streamTypeToRole(int streamType) {
  switch (streamType) {
    case 1:  // AUDIO_STREAM_GUIDANCE
      return AAudioStreamRole::GUIDANCE;
    case 2:  // AUDIO_STREAM_SYSTEM_AUDIO
      return AAudioStreamRole::SYSTEM_AUDIO;
    default:
      return AAudioStreamRole::MEDIA;
  }
}
