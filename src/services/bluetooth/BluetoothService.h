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

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <memory>

class BluetoothManager;
class AudioRouter;
class PreferencesService;

/**
 * @brief Bluetooth service for Android Auto audio routing
 *
 * Coordinates:
 * - Device discovery and pairing
 * - Audio routing to Bluetooth speakers
 * - Persistence of selected audio output device
 */
class BluetoothService : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool btEnabled READ isBtEnabled NOTIFY btEnabledChanged)
  Q_PROPERTY(bool discovering READ isDiscovering NOTIFY discoveringChanged)
  Q_PROPERTY(QString selectedAudioDevice READ selectedAudioDevice WRITE setSelectedAudioDevice NOTIFY selectedAudioDeviceChanged)
  Q_PROPERTY(bool audioRoutedToBluetooth READ isAudioRoutedToBluetooth NOTIFY audioRoutingChanged)

 public:
  struct AudioDevice {
    QString address;
    QString name;
    int rssi;
    bool connected;
    bool paired;
  };

  explicit BluetoothService(BluetoothManager* btManager, AudioRouter* audioRouter,
                            PreferencesService* prefsService, QObject* parent = nullptr);
  ~BluetoothService() override;

  // Initialize the service
  auto initialize() -> bool;

  // Getters
  [[nodiscard]] auto isBtEnabled() const -> bool;
  [[nodiscard]] auto isDiscovering() const -> bool;
  [[nodiscard]] auto selectedAudioDevice() const -> QString;
  [[nodiscard]] auto isAudioRoutedToBluetooth() const -> bool;

  // Get discovered audio devices
  [[nodiscard]] auto getDiscoveredAudioDevices() const -> QVector<AudioDevice>;

  // Get paired audio devices
  [[nodiscard]] auto getPairedAudioDevices() const -> QVector<AudioDevice>;

  // Get connected audio devices
  [[nodiscard]] auto getConnectedAudioDevices() const -> QVector<AudioDevice>;

 public slots:
  // Enable/disable Bluetooth
  void setBtEnabled(bool enabled);

  // Start/stop device discovery
  void startDiscovery();
  void stopDiscovery();

  // Pair with a device
  void pairDevice(const QString& address);

  // Connect audio to a device
  void connectAudioDevice(const QString& address);

  // Disconnect audio from current device
  void disconnectAudio();

  // Set selected audio device preference
  void setSelectedAudioDevice(const QString& address);

 signals:
  void btEnabledChanged(bool enabled);
  void discoveringChanged(bool discovering);
  void selectedAudioDeviceChanged(const QString& address);
  void audioRoutingChanged(bool routed);

  // Device discovery signals
  void deviceDiscovered(const AudioDevice& device);
  void devicePaired(const QString& address, const QString& name);
  void deviceConnected(const QString& address);
  void deviceDisconnected(const QString& address);

  // Audio routing signals
  void audioConnectedToDevice(const QString& address, const QString& name);
  void audioDisconnectedFromDevice();
  void audioRoutingError(const QString& error);

 private slots:
  void onBtStateChanged();
  void onDeviceDiscovered();
  void onDevicePaired(const QString& address);
  void onDeviceConnected(const QString& address);
  void onDeviceDisconnected(const QString& address);
  void onDiscoveryTimeout();

 private:
  BluetoothManager* m_btManager;
  AudioRouter* m_audioRouter;
  PreferencesService* m_prefsService;

  QString m_selectedAudioDevice;
  QString m_connectedAudioDevice;
  bool m_audioRoutedToBluetooth;
  int m_discoveryTimeoutMs;
  QTimer m_discoveryTimeoutTimer;
};
