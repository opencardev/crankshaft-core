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

#include "../wireless/BluetoothManager.h"

class BluetoothManagerBackend : public QObject {
  Q_OBJECT

 public:
  explicit BluetoothManagerBackend(QObject* parent = nullptr) : QObject(parent) {}
  ~BluetoothManagerBackend() override = default;

  virtual auto initialise() -> bool = 0;
  virtual void deinitialise() = 0;
  virtual auto isEnabled() const -> bool = 0;
  virtual auto setEnabled(bool enabled) -> bool = 0;
  virtual auto getConnectionState() const -> BluetoothManager::ConnectionState = 0;
  virtual auto getLocalAddress() const -> QString = 0;
  virtual auto getLocalName() const -> QString = 0;
  virtual auto startDiscovery() -> bool = 0;
  virtual auto stopDiscovery() -> bool = 0;
  virtual auto isDiscovering() const -> bool = 0;
  virtual auto getDiscoveredDevices() const -> QVector<BluetoothManager::BluetoothDevice> = 0;
  virtual auto getPairedDevices() const -> QVector<BluetoothManager::BluetoothDevice> = 0;
  virtual auto pair(const QString& address) -> bool = 0;
  virtual auto unpair(const QString& address) -> bool = 0;
  virtual auto connectDevice(const QString& address) -> bool = 0;
  virtual auto disconnectDevice(const QString& address) -> bool = 0;
  virtual auto getConnectedDevices() const -> QVector<BluetoothManager::BluetoothDevice> = 0;
  virtual auto connectAudio(const QString& address, BluetoothManager::AudioProfile profile)
      -> bool = 0;
  virtual auto disconnectAudio(const QString& address, BluetoothManager::AudioProfile profile)
      -> bool = 0;

 signals:
  void stateChanged(BluetoothManager::ConnectionState state);
  void enabledChanged(bool enabled);
  void devicesDiscovered(const QVector<BluetoothManager::BluetoothDevice>& devices);
  void deviceDiscovered(const BluetoothManager::BluetoothDevice& device);
  void devicePaired(const QString& address);
  void deviceUnpaired(const QString& address);
  void deviceConnected(const QString& address);
  void deviceDisconnected(const QString& address);
  void audioConnected(const QString& address, BluetoothManager::AudioProfile profile);
  void audioDisconnected(const QString& address, BluetoothManager::AudioProfile profile);
  void errorOccurred(const QString& error);
};

class BluetoothManagerImpl : public BluetoothManager {
  Q_OBJECT

 public:
  explicit BluetoothManagerImpl(BluetoothManagerBackend* backend = nullptr,
                                QObject* parent = nullptr);
  ~BluetoothManagerImpl() override;

  auto initialise() -> bool override;
  void deinitialise() override;
  auto isEnabled() const -> bool override;
  auto setEnabled(bool enabled) -> bool override;
  auto getConnectionState() const -> ConnectionState override;
  auto getLocalAddress() const -> QString override;
  auto getLocalName() const -> QString override;
  auto startDiscovery() -> bool override;
  auto stopDiscovery() -> bool override;
  auto isDiscovering() const -> bool override;
  auto getDiscoveredDevices() const -> QVector<BluetoothDevice> override;
  auto getPairedDevices() const -> QVector<BluetoothDevice> override;
  auto pair(const QString& address) -> bool override;
  auto unpair(const QString& address) -> bool override;
  auto connect(const QString& address) -> bool override;
  auto disconnect(const QString& address) -> bool override;
  auto getConnectedDevices() const -> QVector<BluetoothDevice> override;
  auto connectAudio(const QString& address, AudioProfile profile) -> bool override;
  auto disconnectAudio(const QString& address, AudioProfile profile) -> bool override;

 private:
  BluetoothManagerBackend* m_backend;
};