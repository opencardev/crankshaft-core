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

#include "WiFiManagerImpl.h"

#include <QDebug>

#include "../wireless/WiFiHAL.h"

namespace {

auto mapConnectionState(WiFiHAL::ConnectionState state) -> WiFiManager::ConnectionState {
  switch (state) {
    case WiFiHAL::ConnectionState::Disconnected:
      return WiFiManager::ConnectionState::DISCONNECTED;
    case WiFiHAL::ConnectionState::Connecting:
      return WiFiManager::ConnectionState::CONNECTING;
    case WiFiHAL::ConnectionState::Connected:
      return WiFiManager::ConnectionState::CONNECTED;
    case WiFiHAL::ConnectionState::Failed:
      return WiFiManager::ConnectionState::FAILED;
    case WiFiHAL::ConnectionState::Unknown:
    default:
      return WiFiManager::ConnectionState::UNKNOWN;
  }
}

auto mapSecurity(WiFiSecurity security) -> WiFiManager::Security {
  switch (security) {
    case WiFiSecurity::Open:
      return WiFiManager::Security::NONE;
    case WiFiSecurity::WEP:
      return WiFiManager::Security::WEP;
    case WiFiSecurity::WPA_PSK:
      return WiFiManager::Security::WPA;
    case WiFiSecurity::WPA2_PSK:
      return WiFiManager::Security::WPA2;
    case WiFiSecurity::WPA3_SAE:
      return WiFiManager::Security::WPA3;
    default:
      return WiFiManager::Security::NONE;
  }
}

auto mapSecurity(WiFiManager::Security security) -> WiFiSecurity {
  switch (security) {
    case WiFiManager::Security::WEP:
      return WiFiSecurity::WEP;
    case WiFiManager::Security::WPA:
      return WiFiSecurity::WPA_PSK;
    case WiFiManager::Security::WPA2:
      return WiFiSecurity::WPA2_PSK;
    case WiFiManager::Security::WPA3:
      return WiFiSecurity::WPA3_SAE;
    case WiFiManager::Security::NONE:
    default:
      return WiFiSecurity::Open;
  }
}

auto mapNetwork(const WiFiNetwork& network) -> WiFiManager::WiFiNetwork {
  return {network.ssid, network.bssid, network.signalStrength, mapSecurity(network.security),
          network.frequency, network.known};
}

auto mapHotspotSecurity(WiFiManager::Security security) -> WiFiSecurity {
  switch (security) {
    case WiFiManager::Security::WEP:
      return WiFiSecurity::WEP;
    case WiFiManager::Security::WPA:
      return WiFiSecurity::WPA_PSK;
    case WiFiManager::Security::WPA3:
      return WiFiSecurity::WPA3_SAE;
    case WiFiManager::Security::NONE:
      return WiFiSecurity::Open;
    case WiFiManager::Security::WPA2:
    default:
      return WiFiSecurity::WPA2_PSK;
  }
}

auto mapHotspotSecurity(WiFiSecurity security) -> WiFiManager::Security {
  switch (security) {
    case WiFiSecurity::Open:
      return WiFiManager::Security::NONE;
    case WiFiSecurity::WEP:
      return WiFiManager::Security::WEP;
    case WiFiSecurity::WPA_PSK:
      return WiFiManager::Security::WPA;
    case WiFiSecurity::WPA3_SAE:
      return WiFiManager::Security::WPA3;
    case WiFiSecurity::WPA2_PSK:
    default:
      return WiFiManager::Security::WPA2;
  }
}

class DbusWiFiManagerBackend : public WiFiManagerBackend {
  Q_OBJECT

 public:
  explicit DbusWiFiManagerBackend(QObject* parent = nullptr)
      : WiFiManagerBackend(parent), m_hal(new WiFiHAL(this)) {
    connect(m_hal, &WiFiHAL::connectionStateChanged, this,
            [this](WiFiHAL::ConnectionState state) { emit connectionStateChanged(mapConnectionState(state)); });
    connect(m_hal, &WiFiHAL::enabledChanged, this, &WiFiManagerBackend::enabledChanged);
    connect(m_hal, &WiFiHAL::networksUpdated, this, [this](const QList<WiFiNetwork>& networks) {
      QVector<WiFiManager::WiFiNetwork> converted;
      converted.reserve(networks.size());
      for (const auto& network : networks) {
        converted.push_back(mapNetwork(network));
      }
      emit networksUpdated(converted);
    });
    connect(m_hal, &WiFiHAL::signalStrengthChanged, this, &WiFiManagerBackend::signalStrengthChanged);
    connect(m_hal, &WiFiHAL::errorOccurred, this, &WiFiManagerBackend::errorOccurred);
  }

  auto initialise() -> bool override {
    return true;
  }

  void deinitialise() override {}

  auto isEnabled() const -> bool override {
    return m_hal->isEnabled();
  }

  auto setEnabled(bool enabled) -> bool override {
    return m_hal->setEnabled(enabled);
  }

  auto getConnectionState() const -> WiFiManager::ConnectionState override {
    return mapConnectionState(m_hal->getConnectionState());
  }

  auto getConnectedSSID() const -> QString override {
    return m_hal->getConnectedSSID();
  }

  auto startScan() -> bool override {
    return m_hal->startScan();
  }

  auto getAvailableNetworks() const -> QVector<WiFiManager::WiFiNetwork> override {
    QVector<WiFiManager::WiFiNetwork> converted;
    const auto networks = m_hal->getAvailableNetworks();
    converted.reserve(networks.size());
    for (const auto& network : networks) {
      converted.push_back(mapNetwork(network));
    }
    return converted;
  }

  auto connectToNetwork(const QString& ssid, const QString& password,
                        WiFiManager::Security security) -> bool override {
    return m_hal->connectToNetwork(ssid, password, mapSecurity(security));
  }

  auto disconnectNetwork() -> bool override {
    return m_hal->disconnect();
  }

  auto forgetNetwork(const QString& ssid) -> bool override {
    return m_hal->forgetNetwork(ssid);
  }

  auto getSavedNetworks() const -> QVector<QString> override {
    return QVector<QString>(m_hal->getSavedNetworks().begin(), m_hal->getSavedNetworks().end());
  }

  auto getSignalStrength() const -> int override {
    return m_hal->getSignalStrength();
  }

  auto getIPAddress() const -> QString override {
    return m_hal->getIPAddress();
  }

  auto startHotspot(const WiFiManager::HotspotConfig& config) -> bool override {
    WiFiHAL::HotspotConfig halConfig;
    halConfig.ssid = config.ssid;
    halConfig.password = config.password;
    halConfig.security = mapHotspotSecurity(config.security);
    halConfig.channel = config.channel;
    return m_hal->startHotspot(halConfig);
  }

  void stopHotspot() override {
    m_hal->stopHotspot();
  }

  auto getHotspotStatus() const -> WiFiManager::HotspotStatus override {
    const WiFiHAL::HotspotStatus halStatus = m_hal->getHotspotStatus();
    WiFiManager::HotspotStatus status;
    status.active = halStatus.active;
    status.ssid = halStatus.ssid;
    status.bssid = halStatus.bssid;
    status.ipAddress = halStatus.ipAddress;
    status.security = mapHotspotSecurity(halStatus.security);
    return status;
  }

 private:
  WiFiHAL* m_hal;
};

}  // namespace

WiFiManagerImpl::WiFiManagerImpl(WiFiManagerBackend* backend, QObject* parent)
    : WiFiManager(parent), m_backend(backend ? backend : new DbusWiFiManagerBackend(this)) {
  if (m_backend->parent() != this) {
    m_backend->setParent(this);
  }

  QObject::connect(m_backend, &WiFiManagerBackend::connectionStateChanged, this,
                   &WiFiManager::connectionStateChanged);
  QObject::connect(m_backend, &WiFiManagerBackend::enabledChanged, this,
                   &WiFiManager::enabledChanged);
  QObject::connect(m_backend, &WiFiManagerBackend::networksUpdated, this,
                   &WiFiManager::networksUpdated);
  QObject::connect(m_backend, &WiFiManagerBackend::signalStrengthChanged, this,
                   &WiFiManager::signalStrengthChanged);
  QObject::connect(m_backend, &WiFiManagerBackend::errorOccurred, this,
                   &WiFiManager::errorOccurred);
}

WiFiManagerImpl::~WiFiManagerImpl() = default;

auto WiFiManagerImpl::initialise() -> bool {
  return m_backend->initialise();
}

void WiFiManagerImpl::deinitialise() {
  m_backend->deinitialise();
}

auto WiFiManagerImpl::isEnabled() const -> bool {
  return m_backend->isEnabled();
}

auto WiFiManagerImpl::setEnabled(bool enabled) -> bool {
  return m_backend->setEnabled(enabled);
}

auto WiFiManagerImpl::getConnectionState() const -> ConnectionState {
  return m_backend->getConnectionState();
}

auto WiFiManagerImpl::getConnectedSSID() const -> QString {
  return m_backend->getConnectedSSID();
}

auto WiFiManagerImpl::startScan() -> bool {
  return m_backend->startScan();
}

auto WiFiManagerImpl::getAvailableNetworks() const -> QVector<WiFiNetwork> {
  return m_backend->getAvailableNetworks();
}

auto WiFiManagerImpl::connect(const QString& ssid, const QString& password,
                              Security security) -> bool {
  return m_backend->connectToNetwork(ssid, password, security);
}

auto WiFiManagerImpl::disconnect() -> bool {
  return m_backend->disconnectNetwork();
}

auto WiFiManagerImpl::forgetNetwork(const QString& ssid) -> bool {
  return m_backend->forgetNetwork(ssid);
}

auto WiFiManagerImpl::getSavedNetworks() const -> QVector<QString> {
  return m_backend->getSavedNetworks();
}

auto WiFiManagerImpl::getSignalStrength() const -> int {
  return m_backend->getSignalStrength();
}

auto WiFiManagerImpl::getIPAddress() const -> QString {
  return m_backend->getIPAddress();
}

auto WiFiManagerImpl::startHotspot(const HotspotConfig& config) -> bool {
  return m_backend->startHotspot(config);
}

void WiFiManagerImpl::stopHotspot() {
  m_backend->stopHotspot();
}

auto WiFiManagerImpl::getHotspotStatus() const -> HotspotStatus {
  return m_backend->getHotspotStatus();
}

#include "WiFiManagerImpl.moc"
