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

#include "BluetoothService.h"

#include "../../hal/wireless/BluetoothManager.h"
#include "../audio/AudioRouter.h"
#include "../config/ConfigService.h"
#include "../logging/Logger.h"
#include "../preferences/PreferencesService.h"

BluetoothService::BluetoothService(BluetoothManager* btManager, AudioRouter* audioRouter,
                                   PreferencesService* prefsService, QObject* parent)
    : QObject(parent),
      m_btManager(btManager),
      m_audioRouter(audioRouter),
      m_prefsService(prefsService),
      m_audioRoutedToBluetooth(false),
      m_discoveryTimeoutMs(60000) {
  m_discoveryTimeoutTimer.setSingleShot(true);
  connect(&m_discoveryTimeoutTimer, &QTimer::timeout, this,
          &BluetoothService::onDiscoveryTimeout);

  if (!m_btManager) {
    Logger::instance().error("[BluetoothService] BluetoothManager is null");
  }
  if (!m_audioRouter) {
    Logger::instance().error("[BluetoothService] AudioRouter is null");
  }
  if (!m_prefsService) {
    Logger::instance().error("[BluetoothService] PreferencesService is null");
  }
}

BluetoothService::~BluetoothService() {
  Logger::instance().debug("[BluetoothService] Destructing BluetoothService");
}

bool BluetoothService::initialize() {
  if (!m_btManager || !m_audioRouter || !m_prefsService) {
    Logger::instance().error("[BluetoothService] Cannot initialize: required services are null");
    return false;
  }

  const int discoveryTimeoutSeconds =
      ConfigService::instance().get("core.bluetooth.discovery_timeout_seconds", 60).toInt();
  m_discoveryTimeoutMs = qMax(0, discoveryTimeoutSeconds) * 1000;
  Logger::instance().info(QString("[BluetoothService] Discovery timeout configured to %1 seconds")
                              .arg(discoveryTimeoutSeconds));

  // Load stored preference for audio device
  m_selectedAudioDevice = m_prefsService->get(QStringLiteral("bluetooth/selectedAudioDevice"),
                                              QString()).toString();

  // Connect to Bluetooth manager signals
  connect(m_btManager, &BluetoothManager::enabledChanged, this,
          &BluetoothService::onBtStateChanged);
  connect(m_btManager, &BluetoothManager::deviceDiscovered, this,
          &BluetoothService::onDeviceDiscovered);
  connect(m_btManager, &BluetoothManager::devicePaired, this,
          &BluetoothService::onDevicePaired);
  connect(m_btManager, &BluetoothManager::deviceConnected, this,
          &BluetoothService::onDeviceConnected);
  connect(m_btManager, &BluetoothManager::deviceDisconnected, this,
          &BluetoothService::onDeviceDisconnected);
  connect(m_btManager, &BluetoothManager::errorOccurred, this,
          [this](const QString& error) {
            Logger::instance().error(QString("[BluetoothService] Error: %1").arg(error));
            emit audioRoutingError(error);
          });

  Logger::instance().info("[BluetoothService] Initialized");
  return true;
}

bool BluetoothService::isBtEnabled() const {
  return m_btManager ? m_btManager->isEnabled() : false;
}

bool BluetoothService::isDiscovering() const {
  return m_btManager ? m_btManager->isDiscovering() : false;
}

QString BluetoothService::selectedAudioDevice() const {
  return m_selectedAudioDevice;
}

bool BluetoothService::isAudioRoutedToBluetooth() const {
  return m_audioRoutedToBluetooth;
}

QVector<BluetoothService::AudioDevice> BluetoothService::getDiscoveredAudioDevices() const {
  QVector<AudioDevice> result;
  if (!m_btManager) {
    return result;
  }

  auto devices = m_btManager->getDiscoveredDevices();
  for (const auto& dev : devices) {
    if (dev.type == BluetoothManager::DeviceType::AUDIO ||
        dev.type == BluetoothManager::DeviceType::AUDIO) {
      AudioDevice audioDevice;
      audioDevice.address = dev.address;
      audioDevice.name = dev.name;
      audioDevice.rssi = dev.rssi;
      audioDevice.connected = dev.connected;
      audioDevice.paired = dev.paired;
      result.append(audioDevice);
    }
  }
  return result;
}

QVector<BluetoothService::AudioDevice> BluetoothService::getPairedAudioDevices() const {
  QVector<AudioDevice> result;
  if (!m_btManager) {
    return result;
  }

  auto devices = m_btManager->getPairedDevices();
  for (const auto& dev : devices) {
    if (dev.type == BluetoothManager::DeviceType::AUDIO) {
      AudioDevice audioDevice;
      audioDevice.address = dev.address;
      audioDevice.name = dev.name;
      audioDevice.rssi = dev.rssi;
      audioDevice.connected = dev.connected;
      audioDevice.paired = dev.paired;
      result.append(audioDevice);
    }
  }
  return result;
}

QVector<BluetoothService::AudioDevice> BluetoothService::getConnectedAudioDevices() const {
  QVector<AudioDevice> result;
  auto paired = getPairedAudioDevices();
  for (const auto& dev : paired) {
    if (dev.connected) {
      result.append(dev);
    }
  }
  return result;
}

void BluetoothService::setBtEnabled(bool enabled) {
  if (!m_btManager) {
    return;
  }

  if (m_btManager->setEnabled(enabled)) {
    Logger::instance().info(QString("[BluetoothService] Bluetooth %1")
                                .arg(enabled ? "enabled" : "disabled"));
  } else {
    Logger::instance().warning(
        QString("[BluetoothService] Failed to %1 Bluetooth")
            .arg(enabled ? "enable" : "disable"));
  }
}

void BluetoothService::startDiscovery() {
  if (!m_btManager) {
    return;
  }

  if (m_btManager->startDiscovery()) {
    emit discoveringChanged(true);

    if (m_discoveryTimeoutMs > 0) {
      m_discoveryTimeoutTimer.start(m_discoveryTimeoutMs);
      Logger::instance().info(
          QString("[BluetoothService] Discovery started (timeout: %1 seconds)")
              .arg(m_discoveryTimeoutMs / 1000));
    } else {
      Logger::instance().info("[BluetoothService] Discovery started (timeout disabled)");
    }
  } else {
    Logger::instance().warning("[BluetoothService] Failed to start discovery");
  }
}

void BluetoothService::stopDiscovery() {
  if (!m_btManager) {
    return;
  }

  m_discoveryTimeoutTimer.stop();

  if (m_btManager->stopDiscovery()) {
    Logger::instance().info("[BluetoothService] Discovery stopped");
    emit discoveringChanged(false);
  }
}

void BluetoothService::onDiscoveryTimeout() {
  if (!m_btManager || !m_btManager->isDiscovering()) {
    return;
  }

  Logger::instance().info("[BluetoothService] Discovery timeout reached, stopping scan");
  if (m_btManager->stopDiscovery()) {
    emit discoveringChanged(false);
  }
}

void BluetoothService::pairDevice(const QString& address) {
  if (!m_btManager) {
    return;
  }

  if (m_btManager->pair(address)) {
    Logger::instance().info(QString("[BluetoothService] Pairing initiated with %1")
                                .arg(address));
  } else {
    Logger::instance().warning(QString("[BluetoothService] Failed to pair with %1")
                                   .arg(address));
  }
}

void BluetoothService::connectAudioDevice(const QString& address) {
  if (!m_btManager || !m_audioRouter) {
    Logger::instance().error("[BluetoothService] Required services not available");
    emit audioRoutingError("Services not available");
    return;
  }

  // Disconnect from any previous device
  if (!m_connectedAudioDevice.isEmpty() && m_connectedAudioDevice != address) {
    disconnectAudio();
  }

  // Connect to device
  if (!m_btManager->connect(address)) {
    Logger::instance().warning(QString("[BluetoothService] Failed to connect to device %1")
                                   .arg(address));
    emit audioRoutingError("Failed to connect device");
    return;
  }

  // Route audio to device
  if (!m_audioRouter->setAudioDevice(AAudioStreamRole::MEDIA, address)) {
    Logger::instance().warning(QString("[BluetoothService] Failed to route audio to %1")
                                   .arg(address));
    emit audioRoutingError("Failed to route audio");
    m_btManager->disconnect(address);
    return;
  }

  m_connectedAudioDevice = address;
  m_audioRoutedToBluetooth = true;

  // Get device name
  auto devices = m_btManager->getPairedDevices();
  QString deviceName;
  for (const auto& dev : devices) {
    if (dev.address == address) {
      deviceName = dev.name;
      break;
    }
  }

  Logger::instance().info(QString("[BluetoothService] Audio routed to Bluetooth device: %1")
                              .arg(deviceName));
  emit audioConnectedToDevice(address, deviceName);
}

void BluetoothService::disconnectAudio() {
  if (m_connectedAudioDevice.isEmpty()) {
    return;
  }

  if (m_btManager) {
    m_btManager->disconnect(m_connectedAudioDevice);
  }

  m_connectedAudioDevice.clear();
  m_audioRoutedToBluetooth = false;

  Logger::instance().info("[BluetoothService] Audio disconnected from Bluetooth");
  emit audioDisconnectedFromDevice();
}

void BluetoothService::setSelectedAudioDevice(const QString& address) {
  if (m_selectedAudioDevice == address) {
    return;
  }

  m_selectedAudioDevice = address;
  if (m_prefsService) {
    if (!m_prefsService->set(QStringLiteral("bluetooth/selectedAudioDevice"), address)) {
      Logger::instance().warning("[BluetoothService] Failed to save audio device preference");
    }
  }

  emit selectedAudioDeviceChanged(address);

  // Auto-connect if address is not empty and device is available
  if (!address.isEmpty()) {
    auto devices = getPairedAudioDevices();
    for (const auto& dev : devices) {
      if (dev.address == address && dev.paired) {
        connectAudioDevice(address);
        break;
      }
    }
  }
}

void BluetoothService::onBtStateChanged() {
  emit btEnabledChanged(isBtEnabled());
}

void BluetoothService::onDeviceDiscovered() {
  Logger::instance().debug("[BluetoothService] Device discovered/updated");
}

void BluetoothService::onDevicePaired(const QString& address) {
  auto devices = m_btManager->getPairedDevices();
  for (const auto& dev : devices) {
    if (dev.address == address) {
      Logger::instance().info(QString("[BluetoothService] Device paired: %1").arg(dev.name));
      emit devicePaired(address, dev.name);
      break;
    }
  }
}

void BluetoothService::onDeviceConnected(const QString& address) {
  Logger::instance().debug(QString("[BluetoothService] Device connected: %1").arg(address));
  emit deviceConnected(address);
}

void BluetoothService::onDeviceDisconnected(const QString& address) {
  Logger::instance().debug(QString("[BluetoothService] Device disconnected: %1").arg(address));
  emit deviceDisconnected(address);

  // If this was our audio device, mark audio as disconnected
  if (m_connectedAudioDevice == address) {
    m_connectedAudioDevice.clear();
    m_audioRoutedToBluetooth = false;
    emit audioDisconnectedFromDevice();
  }
}
