/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2026 OpenCarDev Team
 */

#pragma once

#include <QPointer>

#include "IWirelessNetworkManager.h"

class WiFiManager;

class WiFiManagerWirelessNetworkManager final : public IWirelessNetworkManager {
 public:
  explicit WiFiManagerWirelessNetworkManager(WiFiManager* wifiManager);

  bool startHotspot(const HotspotConfig& config) override;
  void stopHotspot() override;
  HotspotStatus getHotspotStatus() const override;

 private:
  QPointer<WiFiManager> m_wifiManager;
};
