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

#include "BluetoothManagerImpl.h"

#include <QDebug>

#include "../wireless/BluetoothHAL.h"

namespace {

auto mapState(BluetoothHAL::State state) -> BluetoothManager::ConnectionState {
  switch (state) {
    case BluetoothHAL::State::Off:
      return BluetoothManager::ConnectionState::OFF;
    case BluetoothHAL::State::TurningOn:
      return BluetoothManager::ConnectionState::TURNING_ON;
    case BluetoothHAL::State::On:
      return BluetoothManager::ConnectionState::ON;
    case BluetoothHAL::State::TurningOff:
      return BluetoothManager::ConnectionState::TURNING_OFF;
    default:
      return BluetoothManager::ConnectionState::UNKNOWN;
  }
}

auto mapDeviceType(BluetoothDevice::DeviceType type) -> BluetoothManager::DeviceType {
  switch (type) {
    case BluetoothDevice::DeviceType::Computer:
      return BluetoothManager::DeviceType::COMPUTER;
    case BluetoothDevice::DeviceType::Phone:
      return BluetoothManager::DeviceType::PHONE;
    case BluetoothDevice::DeviceType::Audio:
    case BluetoothDevice::DeviceType::AudioAndInput:
      return BluetoothManager::DeviceType::AUDIO;
    case BluetoothDevice::DeviceType::Input:
    case BluetoothDevice::DeviceType::Peripheral:
      return BluetoothManager::DeviceType::PERIPHERAL;
    case BluetoothDevice::DeviceType::Imaging:
      return BluetoothManager::DeviceType::IMAGING;
    case BluetoothDevice::DeviceType::Unknown:
    default:
      return BluetoothManager::DeviceType::UNKNOWN;
  }
}

auto mapAudioProfile(BluetoothDevice::AudioProfile profile) -> BluetoothManager::AudioProfile {
  switch (profile) {
    case BluetoothDevice::AudioProfile::A2DP:
      return BluetoothManager::AudioProfile::A2DP;
    case BluetoothDevice::AudioProfile::HFP:
      return BluetoothManager::AudioProfile::HFP;
    case BluetoothDevice::AudioProfile::HSP:
      return BluetoothManager::AudioProfile::HSP;
    case BluetoothDevice::AudioProfile::AVRCP:
      return BluetoothManager::AudioProfile::AVRCP;
    case BluetoothDevice::AudioProfile::None:
    default:
      return BluetoothManager::AudioProfile::NONE;
  }
}

auto mapDevice(const BluetoothDevice& device) -> BluetoothManager::BluetoothDevice {
  QVector<BluetoothManager::AudioProfile> profiles;
  profiles.reserve(device.supportedProfiles.size());
  for (const auto& profile : device.supportedProfiles) {
    profiles.push_back(mapAudioProfile(profile));
  }

  return {device.name, device.address, mapDeviceType(device.type), device.rssi, device.paired,
          device.connected, profiles};
}

class DbusBluetoothManagerBackend : public BluetoothManagerBackend {
  Q_OBJECT

 public:
  explicit DbusBluetoothManagerBackend(QObject* parent = nullptr)
      : BluetoothManagerBackend(parent), m_hal(new BluetoothHAL(this)) {
    connect(m_hal, &BluetoothHAL::stateChanged, this,
            [this](BluetoothHAL::State state) { emit this->stateChanged(mapState(state)); });
    connect(m_hal, &BluetoothHAL::enabledChanged, this, &BluetoothManagerBackend::enabledChanged);
    connect(m_hal, &BluetoothHAL::devicesUpdated, this,
            [this](const QList<BluetoothDevice>& devices) {
              QVector<BluetoothManager::BluetoothDevice> converted;
              converted.reserve(devices.size());
              for (const auto& device : devices) {
                converted.push_back(mapDevice(device));
              }
              emit devicesDiscovered(converted);
              for (const auto& device : converted) {
                emit deviceDiscovered(device);
              }
            });
    connect(m_hal, &BluetoothHAL::devicePaired, this, &BluetoothManagerBackend::devicePaired);
    connect(m_hal, &BluetoothHAL::deviceConnected, this,
            &BluetoothManagerBackend::deviceConnected);
    connect(m_hal, &BluetoothHAL::deviceDisconnected, this,
            &BluetoothManagerBackend::deviceDisconnected);
    connect(m_hal, &BluetoothHAL::errorOccurred, this, &BluetoothManagerBackend::errorOccurred);
    connect(m_hal, &BluetoothHAL::pairingFailed, this,
            [this](const QString& address, const QString& error) {
              emit errorOccurred(QString("Pairing failed for %1: %2").arg(address, error));
            });
    connect(m_hal, &BluetoothHAL::connectionFailed, this,
            [this](const QString& address, const QString& error) {
              emit errorOccurred(QString("Connection failed for %1: %2").arg(address, error));
            });
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

  auto getConnectionState() const -> BluetoothManager::ConnectionState override {
    return mapState(m_hal->getState());
  }

  auto getLocalAddress() const -> QString override {
    return m_hal->getLocalAddress();
  }

  auto getLocalName() const -> QString override {
    return m_hal->getLocalName();
  }

  auto startDiscovery() -> bool override {
    return m_hal->startDiscovery();
  }

  auto stopDiscovery() -> bool override {
    return m_hal->stopDiscovery();
  }

  auto isDiscovering() const -> bool override {
    return m_hal->isDiscovering();
  }

  auto getDiscoveredDevices() const -> QVector<BluetoothManager::BluetoothDevice> override {
    QVector<BluetoothManager::BluetoothDevice> converted;
    const auto devices = m_hal->getDevices();
    converted.reserve(devices.size());
    for (const auto& device : devices) {
      converted.push_back(mapDevice(device));
    }
    return converted;
  }

  auto getPairedDevices() const -> QVector<BluetoothManager::BluetoothDevice> override {
    QVector<BluetoothManager::BluetoothDevice> converted;
    const auto devices = m_hal->getPairedDevices();
    converted.reserve(devices.size());
    for (const auto& device : devices) {
      converted.push_back(mapDevice(device));
    }
    return converted;
  }

  auto pair(const QString& address) -> bool override {
    return m_hal->pairDevice(address);
  }

  auto unpair(const QString& address) -> bool override {
    return m_hal->removeDevice(address);
  }

  auto connectDevice(const QString& address) -> bool override {
    return m_hal->connectDevice(address);
  }

  auto disconnectDevice(const QString& address) -> bool override {
    return m_hal->disconnectDevice(address);
  }

  auto getConnectedDevices() const -> QVector<BluetoothManager::BluetoothDevice> override {
    QVector<BluetoothManager::BluetoothDevice> connected;
    for (const auto& device : getDiscoveredDevices()) {
      if (device.connected) {
        connected.push_back(device);
      }
    }
    return connected;
  }

  auto connectAudio(const QString& address, BluetoothManager::AudioProfile profile)
      -> bool override {
    Q_UNUSED(profile)
    const bool connected = m_hal->connectDevice(address);
    if (connected) {
      emit audioConnected(address, profile);
    }
    return connected;
  }

  auto disconnectAudio(const QString& address, BluetoothManager::AudioProfile profile)
      -> bool override {
    Q_UNUSED(profile)
    const bool disconnected = m_hal->disconnectDevice(address);
    if (disconnected) {
      emit audioDisconnected(address, profile);
    }
    return disconnected;
  }

 private:
  BluetoothHAL* m_hal;
};

}  // namespace

BluetoothManagerImpl::BluetoothManagerImpl(BluetoothManagerBackend* backend, QObject* parent)
    : BluetoothManager(parent),
      m_backend(backend ? backend : new DbusBluetoothManagerBackend(this)) {
  if (m_backend->parent() != this) {
    m_backend->setParent(this);
  }

  QObject::connect(m_backend, &BluetoothManagerBackend::stateChanged, this,
       &BluetoothManager::stateChanged);
  QObject::connect(m_backend, &BluetoothManagerBackend::enabledChanged, this,
       &BluetoothManager::enabledChanged);
  QObject::connect(m_backend, &BluetoothManagerBackend::devicesDiscovered, this,
       &BluetoothManager::devicesDiscovered);
  QObject::connect(m_backend, &BluetoothManagerBackend::deviceDiscovered, this,
       &BluetoothManager::deviceDiscovered);
  QObject::connect(m_backend, &BluetoothManagerBackend::devicePaired, this,
       &BluetoothManager::devicePaired);
  QObject::connect(m_backend, &BluetoothManagerBackend::deviceUnpaired, this,
       &BluetoothManager::deviceUnpaired);
  QObject::connect(m_backend, &BluetoothManagerBackend::deviceConnected, this,
       &BluetoothManager::deviceConnected);
  QObject::connect(m_backend, &BluetoothManagerBackend::deviceDisconnected, this,
       &BluetoothManager::deviceDisconnected);
  QObject::connect(m_backend, &BluetoothManagerBackend::audioConnected, this,
       &BluetoothManager::audioConnected);
  QObject::connect(m_backend, &BluetoothManagerBackend::audioDisconnected, this,
       &BluetoothManager::audioDisconnected);
  QObject::connect(m_backend, &BluetoothManagerBackend::errorOccurred, this,
       &BluetoothManager::errorOccurred);
}

BluetoothManagerImpl::~BluetoothManagerImpl() = default;

auto BluetoothManagerImpl::initialise() -> bool {
  return m_backend->initialise();
}

void BluetoothManagerImpl::deinitialise() {
  m_backend->deinitialise();
}

auto BluetoothManagerImpl::isEnabled() const -> bool {
  return m_backend->isEnabled();
}

auto BluetoothManagerImpl::setEnabled(bool enabled) -> bool {
  return m_backend->setEnabled(enabled);
}

auto BluetoothManagerImpl::getConnectionState() const -> ConnectionState {
  return m_backend->getConnectionState();
}

auto BluetoothManagerImpl::getLocalAddress() const -> QString {
  return m_backend->getLocalAddress();
}

auto BluetoothManagerImpl::getLocalName() const -> QString {
  return m_backend->getLocalName();
}

auto BluetoothManagerImpl::startDiscovery() -> bool {
  return m_backend->startDiscovery();
}

auto BluetoothManagerImpl::stopDiscovery() -> bool {
  return m_backend->stopDiscovery();
}

auto BluetoothManagerImpl::isDiscovering() const -> bool {
  return m_backend->isDiscovering();
}

auto BluetoothManagerImpl::getDiscoveredDevices() const -> QVector<BluetoothDevice> {
  return m_backend->getDiscoveredDevices();
}

auto BluetoothManagerImpl::getPairedDevices() const -> QVector<BluetoothDevice> {
  return m_backend->getPairedDevices();
}

auto BluetoothManagerImpl::pair(const QString& address) -> bool {
  return m_backend->pair(address);
}

auto BluetoothManagerImpl::unpair(const QString& address) -> bool {
  return m_backend->unpair(address);
}

auto BluetoothManagerImpl::connect(const QString& address) -> bool {
  return m_backend->connectDevice(address);
}

auto BluetoothManagerImpl::disconnect(const QString& address) -> bool {
  return m_backend->disconnectDevice(address);
}

auto BluetoothManagerImpl::getConnectedDevices() const -> QVector<BluetoothDevice> {
  return m_backend->getConnectedDevices();
}

auto BluetoothManagerImpl::connectAudio(const QString& address, AudioProfile profile) -> bool {
  return m_backend->connectAudio(address, profile);
}

auto BluetoothManagerImpl::disconnectAudio(const QString& address, AudioProfile profile)
    -> bool {
  return m_backend->disconnectAudio(address, profile);
}

#include "BluetoothManagerImpl.moc"
