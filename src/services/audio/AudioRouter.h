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

#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <atomic>

class MediaPipeline;
class QTimer;

/**
 * @brief Audio stream roles for Android Auto
 */
enum class AAudioStreamRole {
  MEDIA,         ///< Media/music playback
  SYSTEM_AUDIO,  ///< Navigation and system sounds
  GUIDANCE,      ///< Voice guidance only
  PHONE_CALL,    ///< Phone call audio
  UNKNOWN = -1
};

/**
 * @brief Sample rate and channel count of one Android Auto audio stream.
 */
struct AudioStreamFormat {
  int sampleRate;
  int channels;
};

/**
 * @brief Format of the stream for a role, as advertised to the phone.
 *
 * Service discovery and the router both read this table, so they cannot disagree.
 * Returns {0, 0} for UNKNOWN.
 */
auto audioStreamFormat(AAudioStreamRole role) -> AudioStreamFormat;

/**
 * @brief Core-owned audio output for Android Auto channels and user controls.
 *
 * Core is the only process that plays audio. Each Android Auto channel is pushed
 * into its own source in the media pipeline; the pipeline mixes the sources and
 * applies the master volume, mute and output sink. This router applies the
 * per-channel policy (ducking under guidance, balance) before pushing samples.
 *
 * User controls (master volume, mute, balance, ducking) are owned here and
 * exposed to clients over the WebSocket `audio/` topics. The state is persisted
 * to the core configuration file when one is loaded.
 *
 * Backend detection (PipeWire, then PulseAudio) runs once in initialize() and is
 * reported in stateSnapshot(). It does not gate frame routing: the pipeline sink
 * is the source of truth for whether audio is actually played.
 */
class AudioRouter : public QObject {
  Q_OBJECT

 public:
  static constexpr int kDefaultVolume = 50;
  static constexpr int kDefaultDuckPercent = 40;

  explicit AudioRouter(MediaPipeline* mediaPipeline, QObject* parent = nullptr);
  ~AudioRouter() override;

  /**
   * @brief Detect the audio backend, create the mixer sources and load saved state.
   */
  auto initialize() -> bool;

  /**
   * @brief Route one PCM frame from an Android Auto channel into the mixer.
   *
   * Input is S16LE, mono or stereo, at the sample rate configured for the role.
   * Ducking and balance are applied here; the pipeline applies master volume.
   */
  auto routeAudioFrame(AAudioStreamRole role, const QByteArray& audioData) -> bool;

  /**
   * @brief Record the preferred output device.
   *
   * Output selection currently follows the system default sink, so the device is
   * validated and stored but not yet pinned in the pipeline.
   */
  auto setAudioDevice(AAudioStreamRole role, const QString& deviceId) -> bool;

  /**
   * @brief Get list of available audio output devices
   */
  [[nodiscard]] auto getAvailableAudioDevices() const -> QStringList;

  /** @brief Master volume, 0-100. */
  auto setVolume(int percent) -> bool;
  [[nodiscard]] auto volume() const -> int;

  /** @brief Master mute. */
  auto setMuted(bool muted) -> bool;
  [[nodiscard]] auto isMuted() const -> bool;

  /** @brief Balance, -100 (left) to +100 (right). */
  auto setBalance(int balance) -> bool;
  [[nodiscard]] auto balance() const -> int;

  /** @brief Reduce media and system audio while guidance is active. */
  auto enableAudioDucking(bool enable) -> bool;
  [[nodiscard]] auto isAudioDuckingEnabled() const -> bool;

  /**
   * @brief Snapshot of user-visible audio state for clients.
   */
  [[nodiscard]] auto stateSnapshot() const -> QVariantMap;

  /**
   * @brief Stop the pipeline and release backend state.
   */
  auto shutdown() -> bool;

 signals:
  void routingError(const QString& message);

  /**
   * @brief Emitted after a frame is pushed to the pipeline.
   * @param stream Source name: "media", "guidance", "system" or "telephony"
   * @param sampleRate Sample rate of the frame in Hz, before any resampling
   * @param channels Always 2: the frame is interleaved S16LE after the mono upmix
   * @param pcm The stereo samples as played, after balance and ducking
   */
  void frameRouted(const QString& stream, int sampleRate, int channels, const QByteArray& pcm);

 private:
  auto initializePipeWire() -> bool;
  auto initializePulseAudio() -> bool;
  auto createMixerSources() -> void;
  auto applyStoredState() -> void;
  auto schedulePersist() -> void;
  auto persistState() -> void;

  MediaPipeline* m_mediaPipeline = nullptr;
  QTimer* m_saveTimer = nullptr;
  bool m_initialized = false;
  bool m_pipewireAvailable = false;
  bool m_pulseaudioAvailable = false;
  QAudioDevice m_preferredDevice;

  // Read from the Android Auto transport thread and written from the WebSocket
  // thread, so these are atomic.
  std::atomic<int> m_volume{kDefaultVolume};
  std::atomic<bool> m_muted{false};
  std::atomic<int> m_balance{0};
  std::atomic<bool> m_duckingEnabled{true};
  std::atomic<qint64> m_guidanceActiveUntilMs{0};

  // Media and system audio stay ducked this long after the last guidance frame.
  static constexpr qint64 kGuidanceHoldMs = 1000;
};
