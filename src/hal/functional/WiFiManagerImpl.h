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

#include "../wireless/WiFiManager.h"

class WiFiManagerBackend : public QObject {
  Q_OBJECT

 public:
  explicit WiFiManagerBackend(QObject* parent = nullptr) : QObject(parent) {}
  ~WiFiManagerBackend() override = default;

  virtual auto initialise() -> bool = 0;
  virtual void deinitialise() = 0;
  virtual auto isEnabled() const -> bool = 0;
  virtual auto setEnabled(bool enabled) -> bool = 0;
  virtual auto getConnectionState() const -> WiFiManager::ConnectionState = 0;
  virtual auto getConnectedSSID() const -> QString = 0;
  virtual auto startScan() -> bool = 0;
  virtual auto getAvailableNetworks() const -> QVector<WiFiManager::WiFiNetwork> = 0;
  virtual auto connectToNetwork(const QString& ssid, const QString& password,
                                WiFiManager::Security security) -> bool = 0;
  virtual auto disconnectNetwork() -> bool = 0;
  virtual auto forgetNetwork(const QString& ssid) -> bool = 0;
  virtual auto getSavedNetworks() const -> QVector<QString> = 0;
  virtual auto getSignalStrength() const -> int = 0;
  virtual auto getIPAddress() const -> QString = 0;
  virtual auto startHotspot(const WiFiManager::HotspotConfig& config) -> bool {
    Q_UNUSED(config)
    return false;
  }
  virtual void stopHotspot() {}
  virtual auto getHotspotStatus() const -> WiFiManager::HotspotStatus {
    return {};
  }

 signals:
  void connectionStateChanged(WiFiManager::ConnectionState state);
  void enabledChanged(bool enabled);
  void networksUpdated(const QVector<WiFiManager::WiFiNetwork>& networks);
  void signalStrengthChanged(int percent);
  void errorOccurred(const QString& error);
};

class WiFiManagerImpl : public WiFiManager {
  Q_OBJECT

 public:
  explicit WiFiManagerImpl(WiFiManagerBackend* backend = nullptr, QObject* parent = nullptr);
  ~WiFiManagerImpl() override;

  auto initialise() -> bool override;
  void deinitialise() override;
  auto isEnabled() const -> bool override;
  auto setEnabled(bool enabled) -> bool override;
  auto getConnectionState() const -> ConnectionState override;
  auto getConnectedSSID() const -> QString override;
  auto startScan() -> bool override;
  auto getAvailableNetworks() const -> QVector<WiFiNetwork> override;
  auto connect(const QString& ssid, const QString& password, Security security) -> bool override;
  auto disconnect() -> bool override;
  auto forgetNetwork(const QString& ssid) -> bool override;
  auto getSavedNetworks() const -> QVector<QString> override;
  auto getSignalStrength() const -> int override;
  auto getIPAddress() const -> QString override;
  auto startHotspot(const HotspotConfig& config) -> bool override;
  void stopHotspot() override;
  auto getHotspotStatus() const -> HotspotStatus override;

 private:
  WiFiManagerBackend* m_backend;
};