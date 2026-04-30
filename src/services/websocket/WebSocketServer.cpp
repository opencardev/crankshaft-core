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

#include "WebSocketServer.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QStringList>
#include <QTimer>

#include "../android_auto/AndroidAutoService.h"
#include "../config/ConfigService.h"
#include "../eventbus/EventBus.h"
#include "../logging/Logger.h"
#include "../service_manager/ServiceManager.h"
#include "../../hal/wireless/BluetoothManager.h"

static auto normaliseVendorFilterList(const QVariant& rawValue) -> QStringList {
  QStringList values;

  if (!rawValue.isValid() || rawValue.isNull()) {
    return values;
  }

  if (rawValue.canConvert<QVariantList>()) {
    const QVariantList list = rawValue.toList();
    values.reserve(list.size());
    for (const QVariant& entry : list) {
      const QString token = entry.toString().trimmed();
      if (!token.isEmpty()) {
        values.append(token);
      }
    }
    return values;
  }

  QString tokenList = rawValue.toString().trimmed();
  if (tokenList.isEmpty()) {
    return values;
  }

  tokenList.replace(';', ',');
  const QStringList tokens = tokenList.split(',', Qt::SkipEmptyParts);
  for (const QString& token : tokens) {
    const QString trimmed = token.trimmed();
    if (!trimmed.isEmpty()) {
      values.append(trimmed);
    }
  }

  return values;
}

WebSocketServer::WebSocketServer(quint16 port, QObject* parent)
    : QObject(parent),
      m_server(new QWebSocketServer("CrankshaftCore", QWebSocketServer::NonSecureMode, this)),
      m_serviceManager(nullptr),
      m_secureModeEnabled(false) {
  Logger::instance().info(QString("Initializing WebSocket server on port %1...").arg(port));

  if (m_server->listen(QHostAddress::Any, port)) {
    Logger::instance().info(QString("WebSocket server listening on port %1 (ws://)").arg(port));
    connect(m_server, &QWebSocketServer::newConnection, this, &WebSocketServer::onNewConnection);
  } else {
    Logger::instance().error(QString("Failed to start WebSocket server on port %1: %2")
                                 .arg(port)
                                 .arg(m_server->errorString()));
  }

  m_scanTimeoutTimer.setSingleShot(true);
  connect(&m_scanTimeoutTimer, &QTimer::timeout, this, &WebSocketServer::onScanTimeout);
}

WebSocketServer::~WebSocketServer() {
  m_server->close();
  qDeleteAll(m_clients);
}

bool WebSocketServer::isListening() const {
  return m_server->isListening();
}

void WebSocketServer::setServiceManager(ServiceManager* serviceManager) {
  m_serviceManager = serviceManager;
  Logger::instance().info("[WebSocketServer] ServiceManager registered");
}

void WebSocketServer::initializeServiceConnections() {
  if (!m_serviceManager) {
    Logger::instance().debug("[WebSocketServer] ServiceManager not available");
    return;
  }

  Logger::instance().info("[WebSocketServer] Initializing service connections...");
  setupAndroidAutoConnections();
}

void WebSocketServer::onNewConnection() {
  QWebSocket* client = m_server->nextPendingConnection();

  Logger::instance().info(
      QString("[WebSocketServer] New WebSocket connection from %1, Total clients: %2")
          .arg(client->peerAddress().toString())
          .arg(m_clients.size() + 1));

  connect(client, &QWebSocket::textMessageReceived, this, &WebSocketServer::onTextMessageReceived);
  connect(client, &QWebSocket::disconnected, this, &WebSocketServer::onClientDisconnected);

  m_clients.append(client);
  m_subscriptions[client] = QStringList();
}

void WebSocketServer::onTextMessageReceived(const QString& message) {
  QWebSocket* client = qobject_cast<QWebSocket*>(sender());
  if (!client) return;

  QJsonParseError parseError;
  QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    Logger::instance().warning(
        QString("[WebSocketServer] Invalid JSON message: %1").arg(parseError.errorString()));
    sendError(client, QStringLiteral("invalid_json"));
    return;
  }

  QJsonObject obj = doc.object();
  QString error;
  if (!validateMessage(obj, error)) {
    Logger::instance().warning(QString("[WebSocketServer] Invalid message: %1").arg(error));
    sendError(client, error);
    return;
  }

  QString type = obj.value("type").toString();

  if (type == "subscribe") {
    QString topic = obj.value("topic").toString();
    handleSubscribe(client, topic);
  } else if (type == "unsubscribe") {
    QString topic = obj.value("topic").toString();
    handleUnsubscribe(client, topic);
  } else if (type == "publish") {
    QString topic = obj.value("topic").toString();
    QVariantMap payload = obj.value("payload").toObject().toVariantMap();
    handlePublish(topic, payload);
  } else if (type == "service_command") {
    QString command = obj.value("command").toString();
    QString commandError;
    if (!validateServiceCommand(command, commandError)) {
      Logger::instance().warning(
          QString("[WebSocketServer] Rejected service command: %1").arg(commandError));
      sendError(client, commandError);
      return;
    }
    QVariantMap params = obj.value("params").toObject().toVariantMap();
    handleServiceCommand(client, command, params);
  } else if (type == "admin_api") {
    handleAdminApiRequest(client, obj);
  }
}

void WebSocketServer::onClientDisconnected() {
  QWebSocket* client = qobject_cast<QWebSocket*>(sender());
  if (client) {
    Logger::instance().info(
        QString("Client disconnected: %1").arg(client->peerAddress().toString()));
    m_clients.removeOne(client);
    m_subscriptions.remove(client);
    client->deleteLater();
  }
}

void WebSocketServer::handleSubscribe(QWebSocket* client, const QString& topic) {
  if (!m_subscriptions[client].contains(topic)) {
    m_subscriptions[client].append(topic);
    Logger::instance().info(QString("[WebSocketServer] Client subscribed to topic: %1").arg(topic));
    Logger::instance().info(QString("[WebSocketServer] Client now has %1 subscriptions")
                                .arg(m_subscriptions[client].size()));
    for (const auto& sub : std::as_const(m_subscriptions[client])) {
      Logger::instance().debug(QString("[WebSocketServer]   - %1").arg(sub));
    }

    // Send current Android Auto state when subscribing to android-auto topics
    if (topic.startsWith("android-auto") && m_serviceManager) {
      AndroidAutoService* aaService = m_serviceManager->getAndroidAutoService();
      if (aaService) {
        Logger::instance().info(
            "[WebSocketServer] Sending current Android Auto state to new subscriber");
        int currentState = static_cast<int>(aaService->getConnectionState());
        onAndroidAutoStateChanged(currentState);

        // If connected, also send device info
        if (aaService->isConnected()) {
          AndroidAutoService::AndroidDevice device = aaService->getConnectedDevice();
          QVariantMap deviceMap;
          deviceMap["serial_number"] = device.serialNumber;
          deviceMap["manufacturer"] = device.manufacturer;
          deviceMap["model"] = device.model;
          deviceMap["android_version"] = device.androidVersion;
          deviceMap["connected"] = device.connected;
          onAndroidAutoConnected(deviceMap);
        }

        if (m_hasProjectionStatus) {
          onAndroidAutoProjectionStatus(m_lastProjectionStatus);
        }
      }
    }
  } else {
    Logger::instance().debug(
        QString("[WebSocketServer] Client already subscribed to: %1").arg(topic));
  }
}

void WebSocketServer::handlePublish(const QString& topic, const QVariantMap& payload) {
  if (topic.startsWith(QStringLiteral("android-auto/")) && m_serviceManager) {
    AndroidAutoService* aaService = m_serviceManager->getAndroidAutoService();
    if (aaService) {
      if (topic == QStringLiteral("android-auto/launch")) {
        const QString serialNumber = payload.value(QStringLiteral("serial_number")).toString();
        if (serialNumber.isEmpty()) {
          aaService->startSearching();
        } else {
          aaService->connectToDevice(serialNumber);
        }
      } else if (topic == QStringLiteral("android-auto/renegotiate") ||
                 topic == QStringLiteral("android-auto/reconnect")) {
        const int relaunchDelayMs =
            qBound(1500, payload.value(QStringLiteral("relaunch_delay_ms"), 2500).toInt(), 30000);

        Logger::instance().info(
            QString("[WebSocketServer] Forcing Android Auto renegotiation (relaunch delay %1 ms)")
                .arg(relaunchDelayMs));

        aaService->disconnect();

        QTimer::singleShot(relaunchDelayMs, this, [this]() {
          if (!m_serviceManager) {
            return;
          }

          AndroidAutoService* renegotiateService = m_serviceManager->getAndroidAutoService();
          if (!renegotiateService) {
            return;
          }

          if (!renegotiateService->startSearching()) {
            Logger::instance().warning(
                "[WebSocketServer] Android Auto renegotiation startSearching() returned false");
          }
        });
      } else if (topic == QStringLiteral("android-auto/disconnect") ||
                 topic == QStringLiteral("android-auto/terminate")) {
        aaService->disconnect();
      } else if (topic == QStringLiteral("android-auto/touch")) {
        const QSize displayResolution = aaService->getDisplayResolution();
        const double rawX = payload.value(QStringLiteral("x")).toDouble();
        const double rawY = payload.value(QStringLiteral("y")).toDouble();

        int x = static_cast<int>(rawX);
        int y = static_cast<int>(rawY);

        if (rawX >= 0.0 && rawX <= 1.0) {
          x = static_cast<int>(rawX * static_cast<double>(displayResolution.width()));
        }
        if (rawY >= 0.0 && rawY <= 1.0) {
          y = static_cast<int>(rawY * static_cast<double>(displayResolution.height()));
        }

        x = qBound(0, x, qMax(0, displayResolution.width() - 1));
        y = qBound(0, y, qMax(0, displayResolution.height() - 1));

        const QString actionName = payload.value(QStringLiteral("action")).toString();
        int action = 2;
        if (actionName == QStringLiteral("down") || actionName == QStringLiteral("press")) {
          action = 0;
        } else if (actionName == QStringLiteral("up") || actionName == QStringLiteral("release")) {
          action = 1;
        }

        aaService->sendTouchInput(x, y, action);
      } else if (topic == QStringLiteral("android-auto/key")) {
        const QString keyName = payload.value(QStringLiteral("key")).toString().toUpper();
        const QString keyAction = payload.value(QStringLiteral("action")).toString().toLower();
        int keyCode = payload.value(QStringLiteral("key_code"), -1).toInt();

        if (keyCode < 0) {
          if (keyName == QStringLiteral("BACK")) {
            keyCode = 4;
          } else if (keyName == QStringLiteral("HOME")) {
            keyCode = 3;
          }
        }

        if (keyCode >= 0) {
          if (keyAction == QStringLiteral("down")) {
            aaService->sendKeyInput(keyCode, 0);
          } else if (keyAction == QStringLiteral("up")) {
            aaService->sendKeyInput(keyCode, 1);
          } else {
            aaService->sendKeyInput(keyCode, 0);
            aaService->sendKeyInput(keyCode, 1);
          }
        }
      }
    }
  }

  if (topic.startsWith(QStringLiteral("bluetooth/")) && m_serviceManager) {
    BluetoothManager* bluetoothManager = m_serviceManager->getBluetoothManager();
    if (!bluetoothManager) {
      const bool started = m_serviceManager->startService(QStringLiteral("Bluetooth"));
      if (started) {
        bluetoothManager = m_serviceManager->getBluetoothManager();
      }
    }

    if (!bluetoothManager) {
      broadcastEvent(QStringLiteral("bluetooth/error"),
                     QVariantMap{{QStringLiteral("error"),
                                  QStringLiteral("Bluetooth service unavailable")}});
    } else if (topic == QStringLiteral("bluetooth/enabled")) {
      const bool enabled = payload.value(QStringLiteral("enabled"), false).toBool();
      const bool requestAccepted = bluetoothManager->setEnabled(enabled);
      const bool currentEnabled = bluetoothManager->isEnabled();
      const bool success = requestAccepted || (currentEnabled == enabled);
      broadcastEvent(QStringLiteral("bluetooth/enabled/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("enabled"), currentEnabled},
                                 {QStringLiteral("requested_enabled"), enabled},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to update Bluetooth enabled state")}});
    } else if (topic == QStringLiteral("bluetooth/scan/request")) {
      const bool success = bluetoothManager->startDiscovery();
      QVector<BluetoothManager::BluetoothDevice> discovered =
          bluetoothManager->getDiscoveredDevices();
      QVariantList devices;
      devices.reserve(discovered.size());
      for (const auto& dev : discovered) {
        devices.append(QVariantMap{{QStringLiteral("address"), dev.address},
                                   {QStringLiteral("name"), dev.name},
                                   {QStringLiteral("paired"), dev.paired},
                                   {QStringLiteral("connected"), dev.connected},
                                   {QStringLiteral("rssi"), dev.rssi}});
      }
      broadcastEvent(QStringLiteral("bluetooth/scan/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("discovering"), bluetoothManager->isDiscovering()},
                                 {QStringLiteral("devices"), devices},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to start Bluetooth discovery")}});
      if (success) {
        const int timeoutSecs =
            ConfigService::instance().get("core.bluetooth.discovery_timeout_seconds", 60).toInt();
        m_cachedBluetoothManager = bluetoothManager;
        m_scanTimeoutTimer.start(timeoutSecs * 1000);
        Logger::instance().info(
            QString("[WebSocketServer] Bluetooth scan started; timeout in %1s").arg(timeoutSecs));
      }
    } else if (topic == QStringLiteral("bluetooth/scan/stop")) {
      m_scanTimeoutTimer.stop();
      m_cachedBluetoothManager = nullptr;
      const bool success = bluetoothManager->stopDiscovery();
      broadcastEvent(QStringLiteral("bluetooth/scan/stopped"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("discovering"), bluetoothManager->isDiscovering()},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to stop Bluetooth discovery")}});
    } else if (topic == QStringLiteral("bluetooth/pair")) {
      const QString address = payload.value(QStringLiteral("address"),
                                            payload.value(QStringLiteral("id")))
                                  .toString();
      const QString name = payload.value(QStringLiteral("name"), address).toString();
      const bool success = !address.isEmpty() && bluetoothManager->pair(address);
      broadcastEvent(QStringLiteral("bluetooth/pair/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("id"), address},
                                 {QStringLiteral("address"), address},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to pair Bluetooth device")}});
    } else if (topic == QStringLiteral("bluetooth/connect")) {
      const QString address = payload.value(QStringLiteral("address"),
                                            payload.value(QStringLiteral("id")))
                                  .toString();
      const QString name = payload.value(QStringLiteral("name"), address).toString();
      const bool success = !address.isEmpty() && bluetoothManager->connect(address);
      broadcastEvent(QStringLiteral("bluetooth/connect/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("id"), address},
                                 {QStringLiteral("address"), address},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to connect Bluetooth device")}});
    } else if (topic == QStringLiteral("bluetooth/disconnect")) {
      const QString address = payload.value(QStringLiteral("address"),
                                            payload.value(QStringLiteral("id")))
                                  .toString();
      const QString name = payload.value(QStringLiteral("name"), address).toString();
      const bool success = !address.isEmpty() && bluetoothManager->disconnect(address);
      broadcastEvent(QStringLiteral("bluetooth/disconnect/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("id"), address},
                                 {QStringLiteral("address"), address},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("error"),
                                  success ? QString()
                                          : QStringLiteral("Failed to disconnect Bluetooth device")}});
    } else if (topic == QStringLiteral("bluetooth/unpair")) {
      const QString address = payload.value(QStringLiteral("address"),
                                            payload.value(QStringLiteral("id")))
                                  .toString();
      const QString name = payload.value(QStringLiteral("name"), address).toString();
      const bool success = !address.isEmpty() && bluetoothManager->unpair(address);
      broadcastEvent(QStringLiteral("bluetooth/unpair/result"),
                     QVariantMap{{QStringLiteral("success"), success},
                                 {QStringLiteral("id"), address},
                                 {QStringLiteral("address"), address},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("error"),
                                  success ? QString() : QStringLiteral("Failed to unpair Bluetooth device")}});
    }
  }

  EventBus::instance().publish(topic, payload);
}

void WebSocketServer::handleServiceCommand(QWebSocket* client, const QString& command,
                                           const QVariantMap& params) {
  if (!m_serviceManager) {
    Logger::instance().warning("[WebSocketServer] ServiceManager not available for command: " +
                               command);

    QJsonObject response;
    response["type"] = "service_response";
    response["command"] = command;
    response["success"] = false;
    response["error"] = "ServiceManager not available";
    client->sendTextMessage(QJsonDocument(response).toJson(QJsonDocument::Compact));
    return;
  }

  Logger::instance().info(QString("[WebSocketServer] Handling service command: %1").arg(command));

  QJsonObject response;
  response["type"] = "service_response";
  response["command"] = command;
  bool success = false;
  QString error;

  if (command == "reload_services") {
    m_serviceManager->reloadServices();
    success = true;
    Logger::instance().info("[WebSocketServer] Services reloaded via WebSocket command");
  } else if (command == "start_service") {
    QString serviceName = params.value("service").toString();
    if (!serviceName.isEmpty()) {
      success = m_serviceManager->startService(serviceName);
      Logger::instance().info(QString("[WebSocketServer] Start service '%1': %2")
                                  .arg(serviceName)
                                  .arg(success ? "success" : "failed"));
    } else {
      error = "Missing 'service' parameter";
    }
  } else if (command == "stop_service") {
    QString serviceName = params.value("service").toString();
    if (!serviceName.isEmpty()) {
      success = m_serviceManager->stopService(serviceName);
      Logger::instance().info(QString("[WebSocketServer] Stop service '%1': %2")
                                  .arg(serviceName)
                                  .arg(success ? "success" : "failed"));
    } else {
      error = "Missing 'service' parameter";
    }
  } else if (command == "restart_service") {
    QString serviceName = params.value("service").toString();
    if (!serviceName.isEmpty()) {
      success = m_serviceManager->restartService(serviceName);
      Logger::instance().info(QString("[WebSocketServer] Restart service '%1': %2")
                                  .arg(serviceName)
                                  .arg(success ? "success" : "failed"));
    } else {
      error = "Missing 'service' parameter";
    }
  } else if (command == "get_running_services") {
    QStringList services = m_serviceManager->getRunningServices();
    response["services"] = QJsonArray::fromStringList(services);
    success = true;
    Logger::instance().info(
        QString("[WebSocketServer] Running services query: %1").arg(services.join(", ")));
  } else if (command == "get_android_auto_usb_vendor_filters") {
    const QStringList allowList = normaliseVendorFilterList(
        ConfigService::instance().get("core.android_auto.usb.vendor_allow_list", QVariant()));
    const QStringList denyList = normaliseVendorFilterList(
        ConfigService::instance().get("core.android_auto.usb.vendor_deny_list", QVariant()));

    response["vendor_allow_list"] = QJsonArray::fromStringList(allowList);
    response["vendor_deny_list"] = QJsonArray::fromStringList(denyList);
    response["allow_list_enabled"] = !allowList.isEmpty();
    response["allow_list_count"] = allowList.size();
    response["deny_list_count"] = denyList.size();
    success = true;

    Logger::instance().info(
        QString("[WebSocketServer] Android Auto USB vendor filters queried: allow=%1 deny=%2")
            .arg(allowList.join(", "))
            .arg(denyList.join(", ")));
  } else {
    error = "Unknown command: " + command;
    Logger::instance().warning("[WebSocketServer] " + error);
  }

  response["success"] = success;
  if (!error.isEmpty()) {
    response["error"] = error;
  }
  response["timestamp"] = QDateTime::currentSecsSinceEpoch();

  client->sendTextMessage(QJsonDocument(response).toJson(QJsonDocument::Compact));
}

void WebSocketServer::handleAdminApiRequest(QWebSocket* client, const QJsonObject& request) {
  if (!client) {
    return;
  }

  const QString requestId = request.value(QStringLiteral("id")).toString();
  const QString method = request.value(QStringLiteral("method")).toString().toUpper();
  const QString path = request.value(QStringLiteral("path")).toString();
  const QHostAddress peerAddress = client->peerAddress();

  if (!peerAddress.isLoopback()) {
    Logger::instance().warning(
        QString("[WebSocketServer] Rejected non-local admin_api request from %1")
            .arg(peerAddress.toString()));
    sendAdminApiResponse(client, requestId, false, 403, path, QJsonObject(),
                         QStringLiteral("admin_api_localhost_only"));
    return;
  }

  int statusCode = 500;
  QJsonObject responseBody;
  processAdminRoute(method, path, request.value(QStringLiteral("body")).toObject(), statusCode,
                    responseBody);

  const bool ok = statusCode >= 200 && statusCode < 300;
  sendAdminApiResponse(client, requestId, ok, statusCode, path, responseBody,
                       ok ? QString() : QStringLiteral("admin_api_request_failed"));
}

void WebSocketServer::processAdminRoute(const QString& method, const QString& path,
                                        const QJsonObject& body, int& statusCode,
                                        QJsonObject& responseBody) {
  if (!m_serviceManager) {
    statusCode = 503;
    responseBody[QStringLiteral("error")] = QStringLiteral("service_manager_unavailable");
    return;
  }

  if (method == QStringLiteral("GET") && path == QStringLiteral("/admin/v1/status")) {
    const QStringList runningServices = m_serviceManager->getRunningServices();
    responseBody[QStringLiteral("running_services")] = QJsonArray::fromStringList(runningServices);
    responseBody[QStringLiteral("service_count")] = runningServices.size();
    responseBody[QStringLiteral("websocket_secure_mode")] = m_secureModeEnabled;
    responseBody[QStringLiteral("android_auto_connected")] =
        m_serviceManager->getAndroidAutoService() && m_serviceManager->getAndroidAutoService()->isConnected();
    statusCode = 200;
    return;
  }

  if (method == QStringLiteral("GET") && path == QStringLiteral("/admin/v1/config")) {
    const QVariant coreConfig = ConfigService::instance().get(QStringLiteral("core"), QVariantMap());
    const QVariant uiConfig = ConfigService::instance().get(QStringLiteral("ui"), QVariantMap());
    responseBody[QStringLiteral("core")] = QJsonObject::fromVariantMap(coreConfig.toMap());
    responseBody[QStringLiteral("ui")] = QJsonObject::fromVariantMap(uiConfig.toMap());
    statusCode = 200;
    return;
  }

  if (method == QStringLiteral("POST") && path == QStringLiteral("/admin/v1/config/set")) {
    const QString key = body.value(QStringLiteral("key")).toString().trimmed();
    if (key.isEmpty()) {
      statusCode = 400;
      responseBody[QStringLiteral("error")] = QStringLiteral("missing_key");
      return;
    }

    if (!body.contains(QStringLiteral("value"))) {
      statusCode = 400;
      responseBody[QStringLiteral("error")] = QStringLiteral("missing_value");
      return;
    }

    const QVariant value = body.value(QStringLiteral("value")).toVariant();
    ConfigService::instance().set(key, value);

    bool persisted = false;
    const bool persist = body.value(QStringLiteral("persist")).toBool(false);
    if (persist) {
      const QString configPath = ConfigService::instance().loadedFilePath();
      if (!configPath.isEmpty()) {
        persisted = ConfigService::instance().save(configPath);
      }
    }

    responseBody[QStringLiteral("updated_key")] = key;
    responseBody[QStringLiteral("persist_requested")] = persist;
    responseBody[QStringLiteral("persisted")] = persisted;
    statusCode = 200;
    return;
  }

  if (method == QStringLiteral("POST") && path == QStringLiteral("/admin/v1/services/reload")) {
    m_serviceManager->reloadServices();
    responseBody[QStringLiteral("result")] = QStringLiteral("reloaded");
    statusCode = 200;
    return;
  }

  if (method == QStringLiteral("POST") && path.startsWith(QStringLiteral("/admin/v1/services/"))) {
    const QString serviceSuffix = path.mid(QStringLiteral("/admin/v1/services/").size());
    const QStringList parts = serviceSuffix.split('/', Qt::SkipEmptyParts);
    if (parts.size() != 2) {
      statusCode = 404;
      responseBody[QStringLiteral("error")] = QStringLiteral("unknown_route");
      return;
    }

    const QString serviceName = parts.at(0);
    const QString action = parts.at(1);
    bool success = false;

    if (action == QStringLiteral("start")) {
      success = m_serviceManager->startService(serviceName);
    } else if (action == QStringLiteral("stop")) {
      success = m_serviceManager->stopService(serviceName);
    } else if (action == QStringLiteral("restart")) {
      success = m_serviceManager->restartService(serviceName);
    } else {
      statusCode = 404;
      responseBody[QStringLiteral("error")] = QStringLiteral("unknown_service_action");
      return;
    }

    responseBody[QStringLiteral("service")] = serviceName;
    responseBody[QStringLiteral("action")] = action;
    responseBody[QStringLiteral("success")] = success;
    statusCode = success ? 200 : 409;
    return;
  }

  if (method == QStringLiteral("GET") &&
      path == QStringLiteral("/admin/v1/android-auto/usb-filters")) {
    const QStringList allowList = normaliseVendorFilterList(
        ConfigService::instance().get("core.android_auto.usb.vendor_allow_list", QVariant()));
    const QStringList denyList = normaliseVendorFilterList(
        ConfigService::instance().get("core.android_auto.usb.vendor_deny_list", QVariant()));

    responseBody[QStringLiteral("vendor_allow_list")] = QJsonArray::fromStringList(allowList);
    responseBody[QStringLiteral("vendor_deny_list")] = QJsonArray::fromStringList(denyList);
    responseBody[QStringLiteral("allow_list_enabled")] = !allowList.isEmpty();
    statusCode = 200;
    return;
  }

  statusCode = 404;
  responseBody[QStringLiteral("error")] = QStringLiteral("unknown_route");
}

void WebSocketServer::sendAdminApiResponse(QWebSocket* client, const QString& requestId, bool ok,
                                           int statusCode, const QString& path,
                                           const QJsonObject& body,
                                           const QString& error) const {
  if (!client) {
    return;
  }

  QJsonObject response;
  response[QStringLiteral("type")] = QStringLiteral("admin_api_response");
  response[QStringLiteral("id")] = requestId;
  response[QStringLiteral("ok")] = ok;
  response[QStringLiteral("status")] = statusCode;
  response[QStringLiteral("path")] = path;
  response[QStringLiteral("body")] = body;
  response[QStringLiteral("timestamp")] = QDateTime::currentSecsSinceEpoch();
  if (!error.isEmpty()) {
    response[QStringLiteral("error")] = error;
  }

  client->sendTextMessage(QJsonDocument(response).toJson(QJsonDocument::Compact));
}

void WebSocketServer::broadcastEvent(const QString& topic, const QVariantMap& payload) {
  const bool isMediaTopic = topic.startsWith(QStringLiteral("android-auto/media/"));
  if (isMediaTopic) {
    Logger::instance().debug(QString("[WebSocketServer] broadcastEvent media topic=%1 clients=%2")
                                 .arg(topic)
                                 .arg(m_clients.size()));
  } else {
    Logger::instance().info(
        QString("[WebSocketServer] broadcastEvent called - Topic: %1").arg(topic));
    Logger::instance().debug(
        QString("[WebSocketServer] Payload keys: %1").arg(payload.keys().join(", ")));
  }

  QJsonObject obj;
  obj["type"] = "event";
  obj["topic"] = topic;
  obj["payload"] = QJsonObject::fromVariantMap(payload);
  obj["timestamp"] = QDateTime::currentSecsSinceEpoch();

  QJsonDocument doc(obj);
  QString message = doc.toJson(QJsonDocument::Compact);
  if (isMediaTopic) {
    Logger::instance().debug(
        QString("[WebSocketServer] Media message prepared bytes=%1").arg(message.size()));
  } else {
    Logger::instance().debug(
        QString("[WebSocketServer] Number of connected clients: %1").arg(m_clients.size()));
  }

  for (auto* client : std::as_const(m_clients)) {
    bool shouldSend = false;
    if (!isMediaTopic) {
      Logger::instance().debug(QString("[WebSocketServer] Checking subscriptions for client %1")
                                   .arg(m_clients.indexOf(client)));
    }

    for (const QString& subscription : std::as_const(m_subscriptions[client])) {
      if (!isMediaTopic) {
        Logger::instance().debug(
            QString("[WebSocketServer]   Subscription pattern: %1, Topic: %2, Match: %3")
                .arg(subscription)
                .arg(topic)
                .arg(topicMatches(topic, subscription) ? "YES" : "NO"));
      }
      if (topicMatches(topic, subscription)) {
        shouldSend = true;
        break;
      }
    }

    if (shouldSend) {
      if (!isMediaTopic) {
        Logger::instance().info(
            QString("[WebSocketServer] Sending event to client (matched subscription)"));
      }
      client->sendTextMessage(message);
    } else if (!isMediaTopic) {
      Logger::instance().debug(QString("[WebSocketServer] Client has no matching subscription"));
    }
  }
}

auto WebSocketServer::hasAnySubscriberForTopic(const QString& topic) const -> bool {
  for (auto* client : std::as_const(m_clients)) {
    const QStringList clientSubscriptions = m_subscriptions.value(client);
    for (const QString& subscription : clientSubscriptions) {
      if (topicMatches(topic, subscription)) {
        return true;
      }
    }
  }
  return false;
}

bool WebSocketServer::topicMatches(const QString& topic, const QString& pattern) const {
  // Handle exact match
  if (topic == pattern) {
    return true;
  }

  // Handle wildcard '*' - matches everything
  if (pattern == "*") {
    return true;
  }

  // Handle pattern with wildcard like 'test/*'
  if (pattern.endsWith("/*")) {
    QString prefix = pattern.left(pattern.length() - 2);
    return topic.startsWith(prefix + "/");
  }

  // Handle MQTT-style '#' wildcard - matches everything including nested levels
  if (pattern.endsWith("/#")) {
    QString prefix = pattern.left(pattern.length() - 2);
    return topic.startsWith(prefix + "/");
  }

  return false;
}

void WebSocketServer::onScanTimeout() {
  Logger::instance().info("[WebSocketServer] Bluetooth scan timeout reached, stopping discovery");
  if (m_cachedBluetoothManager) {
    m_cachedBluetoothManager->stopDiscovery();
    m_cachedBluetoothManager = nullptr;
  }
  broadcastEvent(QStringLiteral("bluetooth/scan/stopped"),
                 QVariantMap{{QStringLiteral("success"), true},
                             {QStringLiteral("discovering"), false},
                             {QStringLiteral("timeout"), true},
                             {QStringLiteral("error"), QString()}});
}

void WebSocketServer::setupAndroidAutoConnections() {
  if (!m_serviceManager) {
    Logger::instance().warning(
        "[WebSocketServer] ServiceManager not set, cannot setup Android Auto connections");
    return;
  }

  AndroidAutoService* aaService = m_serviceManager->getAndroidAutoService();
  if (!aaService) {
    Logger::instance().warning(
        "[WebSocketServer] Android Auto service not available - will not broadcast Android Auto "
        "events");
    return;
  }

  Logger::instance().info(
      "[WebSocketServer] Setting up Android Auto service signal connections...");

  // Connect Android Auto service signals to WebSocket broadcast methods
  connect(aaService, &AndroidAutoService::connectionStateChanged, this,
          [this](AndroidAutoService::ConnectionState state) {
            onAndroidAutoStateChanged(static_cast<int>(state));
          });
  connect(aaService, &AndroidAutoService::connected, this,
          [this](const AndroidAutoService::AndroidDevice& device) {
            QVariantMap deviceMap;
            deviceMap["serial_number"] = device.serialNumber;
            deviceMap["manufacturer"] = device.manufacturer;
            deviceMap["model"] = device.model;
            deviceMap["android_version"] = device.androidVersion;
            deviceMap["connected"] = device.connected;
            onAndroidAutoConnected(deviceMap);
          });
  connect(aaService, &AndroidAutoService::deviceFound, this,
          [this](const AndroidAutoService::AndroidDevice& device) {
            QVariantMap deviceMap;
            deviceMap["serial_number"] = device.serialNumber;
            deviceMap["manufacturer"] = device.manufacturer;
            deviceMap["model"] = device.model;
            deviceMap["android_version"] = device.androidVersion;
            deviceMap["connected"] = device.connected;
            onAndroidAutoDeviceFound(deviceMap);
          });
  connect(aaService, &AndroidAutoService::disconnected, this,
          &WebSocketServer::onAndroidAutoDisconnected);
  connect(aaService, &AndroidAutoService::errorOccurred, this,
          &WebSocketServer::onAndroidAutoError);
  connect(aaService, &AndroidAutoService::videoFrameReady, this,
          &WebSocketServer::onAndroidAutoVideoFrameReady);
  connect(aaService, &AndroidAutoService::audioDataReady, this,
          &WebSocketServer::onAndroidAutoAudioDataReady);
  connect(aaService, &AndroidAutoService::projectionStatusChanged, this,
          &WebSocketServer::onAndroidAutoProjectionStatus);

  Logger::instance().info("[WebSocketServer] Android Auto service connections setup");
}

void WebSocketServer::onAndroidAutoStateChanged(int state) {
  Logger::instance().info(QString("[WebSocketServer] Android Auto state changed: %1").arg(state));
  QVariantMap payload;
  payload["state"] = state;

  // Convert state enum to string
  static const QStringList stateNames = {"DISCONNECTED",   "SEARCHING", "CONNECTING",
                                         "AUTHENTICATING", "SECURING",  "CONNECTED",
                                         "DISCONNECTING",  "ERROR"};

  if (state >= 0 && state < stateNames.size()) {
    payload["stateName"] = stateNames[state];
    Logger::instance().info(
        QString("[WebSocketServer] Broadcasting state: %1").arg(stateNames[state]));
  }

  broadcastEvent("android-auto/status/state-changed", payload);
}

void WebSocketServer::onAndroidAutoConnected(const QVariantMap& device) {
  QVariantMap payload;
  payload["device"] = device;
  payload["connected"] = true;
  broadcastEvent("android-auto/status/connected", payload);
}

void WebSocketServer::onAndroidAutoDeviceFound(const QVariantMap& device) {
  QVariantMap payload;
  payload["device"] = device;
  broadcastEvent("android-auto/status/device-found", payload);
}

void WebSocketServer::onAndroidAutoDisconnected() {
  QVariantMap payload;
  payload["connected"] = false;
  broadcastEvent("android-auto/status/disconnected", payload);
}

void WebSocketServer::onAndroidAutoError(const QString& error) {
  QVariantMap payload;
  payload["error"] = error;
  broadcastEvent("android-auto/status/error", payload);
}

void WebSocketServer::onAndroidAutoVideoFrameReady(int width, int height, const uint8_t* data,
                                                   int size) {
  if (width <= 0 || height <= 0 || !data || size <= 0) {
    return;
  }

  const qint64 requiredRgbaBytes = static_cast<qint64>(width) * static_cast<qint64>(height) * 4;
  if (requiredRgbaBytes <= 0 || static_cast<qint64>(size) < requiredRgbaBytes) {
    Logger::instance().warning(
        QString("[WebSocketServer] Dropping invalid RGBA frame: expected>=%1 bytes, got=%2 "
                "(%3x%4)")
            .arg(requiredRgbaBytes)
            .arg(size)
            .arg(width)
            .arg(height));
    return;
  }

  if (!hasAnySubscriberForTopic(QStringLiteral("android-auto/media/video-frame"))) {
    return;
  }

  if (!m_videoFrameTimer.isValid()) {
    m_videoFrameTimer.start();
  }

  const qint64 nowMs = m_videoFrameTimer.elapsed();
  if ((nowMs - m_lastVideoFrameBroadcastMs) < m_videoFrameIntervalMs) {
    return;
  }
  m_lastVideoFrameBroadcastMs = nowMs;

  const QByteArray frameBytes(reinterpret_cast<const char*>(data), size);
  QImage frame(reinterpret_cast<const uchar*>(frameBytes.constData()), width, height,
               QImage::Format_RGBA8888);
  QImage frameCopy = frame.copy();
  if (frameCopy.isNull()) {
    return;
  }

  QByteArray jpegBytes;
  QBuffer buffer(&jpegBytes);
  if (!buffer.open(QIODevice::WriteOnly)) {
    return;
  }
  if (!frameCopy.save(&buffer, "JPG", 70)) {
    return;
  }

  QVariantMap payload;
  payload["width"] = width;
  payload["height"] = height;
  payload["encoding"] = "jpeg-base64";
  payload["sequence"] = static_cast<qulonglong>(++m_videoFrameSequence);
  payload["data"] = QString::fromLatin1(jpegBytes.toBase64());
  broadcastEvent("android-auto/media/video-frame", payload);
}

void WebSocketServer::onAndroidAutoAudioDataReady(const QByteArray& data) {
  if (data.isEmpty()) {
    return;
  }

  if (!hasAnySubscriberForTopic(QStringLiteral("android-auto/media/audio-chunk"))) {
    return;
  }

  QVariantMap payload;
  payload["encoding"] = "pcm_s16le_base64";
  payload["sampleRate"] = 48000;
  payload["channels"] = 2;
  payload["sequence"] = static_cast<qulonglong>(++m_audioChunkSequence);
  payload["data"] = QString::fromLatin1(data.toBase64());
  broadcastEvent("android-auto/media/audio-chunk", payload);
}

void WebSocketServer::onAndroidAutoProjectionStatus(const QJsonObject& status) {
  const bool previousAvailable = m_hasProjectionStatus;
  const bool projectionReady = status.value(QStringLiteral("projection_ready")).toBool(false);
  const bool controlVersionReceived =
      status.value(QStringLiteral("control_version_received")).toBool(false);
  const bool videoReady = status.value(QStringLiteral("video_ready")).toBool(false);
  const bool mediaAudioReady = status.value(QStringLiteral("media_audio_ready")).toBool(false);
  const bool serviceDiscoveryCompleted =
      status.value(QStringLiteral("service_discovery_completed")).toBool(false);
  const QString connectionStateName =
      status.value(QStringLiteral("connection_state_name")).toString();
  const QString reason = status.value(QStringLiteral("reason")).toString();

  bool changed = !previousAvailable;
  if (previousAvailable) {
    auto boolChanged = [this, &status](const QString& key) {
      return m_lastProjectionStatus.value(key) != status.value(key);
    };
    changed = boolChanged(QStringLiteral("projection_ready")) ||
              boolChanged(QStringLiteral("control_version_received")) ||
              boolChanged(QStringLiteral("video_ready")) ||
              boolChanged(QStringLiteral("media_audio_ready")) ||
              boolChanged(QStringLiteral("service_discovery_completed")) ||
              boolChanged(QStringLiteral("connection_state_name")) ||
              boolChanged(QStringLiteral("reason"));
  }

  if (changed) {
    Logger::instance().info(
        QString("[WebSocketServer] channel-status values: state=%1 projection_ready=%2 "
                "control_version_received=%3 video_ready=%4 media_audio_ready=%5 "
                "service_discovery_completed=%6 reason=%7")
            .arg(connectionStateName)
            .arg(projectionReady ? "true" : "false")
            .arg(controlVersionReceived ? "true" : "false")
            .arg(videoReady ? "true" : "false")
            .arg(mediaAudioReady ? "true" : "false")
            .arg(serviceDiscoveryCompleted ? "true" : "false")
            .arg(reason));
  }

  m_lastProjectionStatus = status;
  m_hasProjectionStatus = true;

  broadcastEvent("android-auto/status/channel-status", status.toVariantMap());
}

bool WebSocketServer::validateMessage(const QJsonObject& obj, QString& error) const {
  static const QSet<QString> allowedTypes = {
      QStringLiteral("subscribe"), QStringLiteral("unsubscribe"), QStringLiteral("publish"),
      QStringLiteral("service_command"), QStringLiteral("admin_api")};

  const QString type = obj.value("type").toString();
  if (type.isEmpty() || !allowedTypes.contains(type)) {
    error = QStringLiteral("invalid_type");
    return false;
  }

  // Validate required fields for each type
  if (type == "subscribe" || type == "unsubscribe" || type == "publish") {
    if (!obj.contains("topic") || obj.value("topic").toString().isEmpty()) {
      error = QStringLiteral("missing_topic");
      return false;
    }
  }

  if (type == "publish") {
    if (!obj.contains("payload") || !obj.value("payload").isObject()) {
      error = QStringLiteral("invalid_payload");
      return false;
    }
  }

  if (type == "service_command") {
    if (!obj.contains("command") || obj.value("command").toString().isEmpty()) {
      error = QStringLiteral("missing_command");
      return false;
    }
    if (!obj.contains("params") || !obj.value("params").isObject()) {
      error = QStringLiteral("missing_params");
      return false;
    }
  }

  if (type == "admin_api") {
    const QString method = obj.value(QStringLiteral("method")).toString();
    const QString path = obj.value(QStringLiteral("path")).toString();
    if (method.isEmpty()) {
      error = QStringLiteral("missing_method");
      return false;
    }
    if (path.isEmpty()) {
      error = QStringLiteral("missing_path");
      return false;
    }
    if (obj.contains(QStringLiteral("body")) && !obj.value(QStringLiteral("body")).isObject()) {
      error = QStringLiteral("invalid_body");
      return false;
    }
  }

  return true;
}

bool WebSocketServer::validateServiceCommand(const QString& command, QString& error) const {
  static const QSet<QString> allowedCommands = {
      QStringLiteral("reload_services"),
      QStringLiteral("start_service"),
      QStringLiteral("stop_service"),
      QStringLiteral("restart_service"),
      QStringLiteral("get_running_services"),
      QStringLiteral("get_android_auto_usb_vendor_filters")};

  if (!allowedCommands.contains(command)) {
    error = QStringLiteral("unauthorised_command");
    return false;
  }
  return true;
}

void WebSocketServer::sendError(QWebSocket* client, const QString& message) const {
  if (!client) {
    return;
  }

  QJsonObject errorObj;
  errorObj["type"] = "error";
  errorObj["message"] = message;
  QJsonDocument doc(errorObj);
  client->sendTextMessage(doc.toJson(QJsonDocument::Compact));
  Logger::instance().debug(QString("[WebSocketServer] Sent error to client: %1").arg(message));
}

void WebSocketServer::handleUnsubscribe(QWebSocket* client, const QString& topic) {
  if (!m_subscriptions.contains(client)) {
    Logger::instance().warning("[WebSocketServer] Unsubscribe from unknown client");
    sendError(client, QStringLiteral("client_not_found"));
    return;
  }

  if (!m_subscriptions[client].contains(topic)) {
    Logger::instance().debug(QString("[WebSocketServer] Client not subscribed to: %1").arg(topic));
    sendError(client, QStringLiteral("not_subscribed"));
    return;
  }

  m_subscriptions[client].removeOne(topic);
  Logger::instance().info(
      QString("[WebSocketServer] Client unsubscribed from topic: %1").arg(topic));
}

void WebSocketServer::enableSecureMode(const QString& certificatePath, const QString& keyPath) {
  // Load SSL certificate and key
  QFile certFile(certificatePath);
  QFile keyFile(keyPath);

  if (!certFile.exists()) {
    Logger::instance().error(QString("SSL certificate not found: %1").arg(certificatePath));
    return;
  }

  if (!keyFile.exists()) {
    Logger::instance().error(QString("SSL key not found: %1").arg(keyPath));
    return;
  }

  if (!certFile.open(QIODevice::ReadOnly)) {
    Logger::instance().error(QString("Failed to open SSL certificate: %1").arg(certificatePath));
    return;
  }

  if (!keyFile.open(QIODevice::ReadOnly)) {
    Logger::instance().error(QString("Failed to open SSL key: %1").arg(keyPath));
    certFile.close();
    return;
  }

  QSslCertificate certificate(&certFile, QSsl::Pem);
  QSslKey sslKey(&keyFile, QSsl::Rsa, QSsl::Pem);

  certFile.close();
  keyFile.close();

  if (certificate.isNull()) {
    Logger::instance().error("SSL certificate is invalid");
    return;
  }

  if (sslKey.isNull()) {
    Logger::instance().error("SSL key is invalid");
    return;
  }

  // Close the current non-secure server
  m_server->close();
  delete m_server;

  // Create secure server
  m_server = new QWebSocketServer("CrankshaftCore", QWebSocketServer::SecureMode, this);

  // Configure SSL
  QSslConfiguration sslConfiguration;
  sslConfiguration.setLocalCertificate(certificate);
  sslConfiguration.setPrivateKey(sslKey);
  sslConfiguration.setProtocol(QSsl::TlsV1_3OrLater);
  m_server->setSslConfiguration(sslConfiguration);

  // Listen on the secure port (typically 9003 for wss)
  quint16 securePort = 9003;
  if (m_server->listen(QHostAddress::Any, securePort)) {
    Logger::instance().info(
        QString("WebSocket secure server (wss://) listening on port %1").arg(securePort));
    m_secureModeEnabled = true;
    m_certificatePath = certificatePath;
    m_keyPath = keyPath;
    connect(m_server, &QWebSocketServer::newConnection, this, &WebSocketServer::onNewConnection);
  } else {
    Logger::instance().error(QString("Failed to start secure WebSocket server on port %1: %2")
                                 .arg(securePort)
                                 .arg(m_server->errorString()));
  }
}

bool WebSocketServer::isSecureModeEnabled() const {
  return m_secureModeEnabled;
}
