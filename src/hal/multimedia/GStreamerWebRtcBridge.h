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

#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <memory>

class GStreamerWebRtcBridge : public QObject {
  Q_OBJECT

 public:
  explicit GStreamerWebRtcBridge(QObject* parent = nullptr);
  ~GStreamerWebRtcBridge() override;

  bool initialize(const QSize& streamResolution, int fps);
  void deinitialize();
  bool isInitialized() const;

  bool pushVideoFrame(const QByteArray& encodedFrame);
  bool handleWebRtcSignalingMessage(const QString& topic, const QVariantMap& payload);

 signals:
  void signalingMessageReady(const QString& topic, const QVariantMap& payload);
  void statusChanged(const QJsonObject& status);
  void errorOccurred(const QString& error);

 private:
  struct Private;
  std::unique_ptr<Private> m_private;
};