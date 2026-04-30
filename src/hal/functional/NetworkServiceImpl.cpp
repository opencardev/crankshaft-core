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

#include <QDebug>
#include <QHostInfo>
#include <QNetworkInterface>

#include "../wireless/NetworkService.h"

// Placeholder implementation of NetworkService

class NetworkServiceImpl : public NetworkService {
 public:
  explicit NetworkServiceImpl(QObject* parent = nullptr) : NetworkService(parent) {}

  bool initialise() override {
    qDebug() << "[Network] Initialising NetworkService";
    return true;
  }

  void deinitialise() override {
    qDebug() << "[Network] Deinitialising NetworkService";
  }

  NetworkType getActiveNetworkType() const override {
    const NetworkInterface active = getActiveInterface();
    return active.type;
  }

  bool isConnected() const override {
    const QVector<NetworkInterface> interfaces = getNetworkInterfaces();
    for (const auto& iface : interfaces) {
      if (iface.connected) {
        return true;
      }
    }
    return false;
  }

  QVector<NetworkInterface> getNetworkInterfaces() const override {
    QVector<NetworkInterface> result;
    const auto all = QNetworkInterface::allInterfaces();
    result.reserve(all.size());

    for (const auto& iface : all) {
      const bool isUp = iface.flags().testFlag(QNetworkInterface::IsUp) &&
                        iface.flags().testFlag(QNetworkInterface::IsRunning);
      if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
        continue;
      }

      NetworkType type = NetworkType::UNKNOWN;
      if (iface.type() == QNetworkInterface::Wifi) {
        type = NetworkType::WIFI;
      } else if (iface.type() == QNetworkInterface::Ethernet) {
        type = NetworkType::ETHERNET;
      }

      QString ipv4Address;
      for (const auto& entry : iface.addressEntries()) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
          ipv4Address = entry.ip().toString();
          break;
        }
      }

      result.push_back({iface.humanReadableName(), iface.hardwareAddress(), ipv4Address, QString(),
                        type, isUp, 1500});
    }

    return result;
  }

  NetworkInterface getActiveInterface() const override {
    const QVector<NetworkInterface> interfaces = getNetworkInterfaces();

    for (const auto& iface : interfaces) {
      if (iface.connected && iface.type == NetworkType::WIFI) {
        return iface;
      }
    }
    for (const auto& iface : interfaces) {
      if (iface.connected && iface.type == NetworkType::ETHERNET) {
        return iface;
      }
    }
    if (!interfaces.isEmpty()) {
      return interfaces.first();
    }

    return {QStringLiteral("None"), QString(), QString(), QString(), NetworkType::UNKNOWN, false,
            1500};
  }

  QString getHostname() const override {
    const QString hostname = QHostInfo::localHostName();
    return hostname.isEmpty() ? QStringLiteral("crankshaft") : hostname;
  }

  bool setHostname(const QString& hostname) override {
    Q_UNUSED(hostname)
    qWarning() << "[Network] setHostname is not implemented on this backend";
    return false;
  }

  QVector<QString> getDNSServers() const override {
    return {};
  }

  bool setDNSServers(const QVector<QString>& servers) override {
    Q_UNUSED(servers)
    qWarning() << "[Network] setDNSServers is not implemented on this backend";
    return false;
  }

  QString getGateway() const override {
    return {};
  }

  int ping(const QString& host) override {
    Q_UNUSED(host)
    return -1;
  }

  int getLatency() const override {
    return -1;
  }
};
