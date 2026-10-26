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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * @brief Hardware Abstraction Layer for audio devices
 *
 * Provides low-level audio hardware control and configuration.
 */
class AudioHAL : public QObject {
  Q_OBJECT

 public:
  explicit AudioHAL(QObject* parent = nullptr);
  ~AudioHAL() override;

  auto setVolume(int volume) -> bool;
  auto getVolume() const -> int;

  auto setMute(bool muted) -> bool;
  auto isMuted() const -> bool;

  /**
   * @brief Select the output sink for everything the pipeline plays.
   *
   * An empty name selects the system default sink. Any other name is a PulseAudio
   * sink, which may be direct-attached or Bluetooth, and is opened with pulsesink.
   * If the named sink cannot be used, the pipeline falls back to the system default
   * and this returns false.
   *
   * @return true if the requested sink is now in use
   */
  auto setOutputDevice(const QString& sinkName) -> bool;

  /** @brief Sink in use: the PulseAudio sink name, or empty for the system default. */
  [[nodiscard]] auto outputDevice() const -> QString;

  auto startStream(const QString& streamName, int sampleRate, int channels) -> bool;
  auto stopStream(const QString& streamName) -> bool;

  /**
   * @brief Register a named input to the mixer.
   *
   * Inputs are stereo S16LE at @p sampleRate and are converted to the mixer
   * rate. Safe to call while the pipeline is playing.
   */
  auto addSource(const QString& name, int sampleRate) -> bool;

  /**
   * @brief Push interleaved stereo S16LE PCM into a registered source.
   */
  auto pushAudioData(const QString& source, const QByteArray& data) -> bool;

 signals:
  void errorOccurred(const QString& message);
  void streamStarted(const QString& streamName);
  void streamStopped(const QString& streamName);
  void volumeChanged(int volume);
  void muteChanged(bool muted);

 private:
  auto initializePipeline() -> bool;
  void cleanup();

  class AudioHALPrivate;
  AudioHALPrivate* d;
};
