/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2026 OpenCarDev Team
 *
 *  Crankshaft is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 */

#pragma once

#include <QString>

class IWirelessNetworkManager {
 public:
  enum class SecurityMode {
    Open,
    Wpa2Psk,
  };

  struct HotspotConfig {
    QString ssid;
    QString password;
    SecurityMode security{SecurityMode::Wpa2Psk};
    int channel{0};
  };

  struct HotspotStatus {
    bool active{false};
    QString ssid;
    QString bssid;
    QString ipAddress;
    SecurityMode security{SecurityMode::Wpa2Psk};
  };

  virtual ~IWirelessNetworkManager() = default;

  virtual bool startHotspot(const HotspotConfig& config) = 0;
  virtual void stopHotspot() = 0;
  virtual HotspotStatus getHotspotStatus() const = 0;
};
