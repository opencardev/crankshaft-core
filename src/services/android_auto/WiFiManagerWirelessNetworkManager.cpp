/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2026 OpenCarDev Team
 */

#include "WiFiManagerWirelessNetworkManager.h"

#include "../../hal/wireless/WiFiManager.h"

namespace {

auto toWiFiManagerSecurity(IWirelessNetworkManager::SecurityMode mode) -> WiFiManager::Security {
  switch (mode) {
    case IWirelessNetworkManager::SecurityMode::Open:
      return WiFiManager::Security::NONE;
    case IWirelessNetworkManager::SecurityMode::Wpa2Psk:
    default:
      return WiFiManager::Security::WPA2;
  }
}

auto fromWiFiManagerSecurity(WiFiManager::Security mode) -> IWirelessNetworkManager::SecurityMode {
  switch (mode) {
    case WiFiManager::Security::NONE:
      return IWirelessNetworkManager::SecurityMode::Open;
    case WiFiManager::Security::WEP:
    case WiFiManager::Security::WPA:
    case WiFiManager::Security::WPA2:
    case WiFiManager::Security::WPA3:
    default:
      return IWirelessNetworkManager::SecurityMode::Wpa2Psk;
  }
}

}  // namespace

WiFiManagerWirelessNetworkManager::WiFiManagerWirelessNetworkManager(WiFiManager* wifiManager)
    : m_wifiManager(wifiManager) {}

bool WiFiManagerWirelessNetworkManager::startHotspot(const HotspotConfig& config) {
  if (!m_wifiManager) {
    return false;
  }

  WiFiManager::HotspotConfig hotspotConfig;
  hotspotConfig.ssid = config.ssid;
  hotspotConfig.password = config.password;
  hotspotConfig.security = toWiFiManagerSecurity(config.security);
  hotspotConfig.channel = config.channel;

  return m_wifiManager->startHotspot(hotspotConfig);
}

void WiFiManagerWirelessNetworkManager::stopHotspot() {
  if (!m_wifiManager) {
    return;
  }

  m_wifiManager->stopHotspot();
}

IWirelessNetworkManager::HotspotStatus WiFiManagerWirelessNetworkManager::getHotspotStatus() const {
  HotspotStatus status;
  if (!m_wifiManager) {
    return status;
  }

  const WiFiManager::HotspotStatus wifiStatus = m_wifiManager->getHotspotStatus();
  status.active = wifiStatus.active;
  status.ssid = wifiStatus.ssid;
  status.bssid = wifiStatus.bssid;
  status.ipAddress = wifiStatus.ipAddress;
  status.security = fromWiFiManagerSecurity(wifiStatus.security);
  return status;
}
