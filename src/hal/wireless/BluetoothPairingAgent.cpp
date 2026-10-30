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

#include "BluetoothPairingAgent.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDebug>
#include <QTimer>

namespace {

constexpr auto kBluezService = "org.bluez";
constexpr auto kAgentManagerPath = "/org/bluez";
constexpr auto kAgentManagerInterface = "org.bluez.AgentManager1";
constexpr auto kErrorRejected = "org.bluez.Error.Rejected";
constexpr auto kErrorCanceled = "org.bluez.Error.Canceled";

QDBusObjectPath agentObjectPath() {
  return QDBusObjectPath(QString::fromLatin1(BluetoothPairingAgent::kAgentPath));
}

// The passkey is six digits on the speaker, including leading zeros.
QString formatPasskey(quint32 passkey) {
  return QString::number(passkey).rightJustified(6, QLatin1Char('0'));
}

}  // namespace

BluetoothAgentAdaptor::BluetoothAgentAdaptor(BluetoothPairingAgent* agent)
    : QDBusAbstractAdaptor(agent), m_agent(agent) {}

void BluetoothAgentAdaptor::Release() {
  m_agent->cancelAllPrompts(QString::fromLatin1(kErrorCanceled),
                            QStringLiteral("Pairing agent released"));
}

void BluetoothAgentAdaptor::RequestPinCode(const QDBusObjectPath& device,
                                           const QDBusMessage& message) {
  m_agent->beginPrompt(PairingPromptKind::Pin, device.path(), QString(), message);
}

void BluetoothAgentAdaptor::DisplayPinCode(const QDBusObjectPath& device,
                                           const QString& pincode) {
  emit m_agent->displayRequested(bluetoothAddressFromDevicePath(device.path()), pincode);
}

void BluetoothAgentAdaptor::RequestPasskey(const QDBusObjectPath& device,
                                           const QDBusMessage& message) {
  m_agent->beginPrompt(PairingPromptKind::Passkey, device.path(), QString(), message);
}

void BluetoothAgentAdaptor::DisplayPasskey(const QDBusObjectPath& device, quint32 passkey,
                                           quint16 /*entered*/) {
  emit m_agent->displayRequested(bluetoothAddressFromDevicePath(device.path()),
                                 formatPasskey(passkey));
}

void BluetoothAgentAdaptor::RequestConfirmation(const QDBusObjectPath& device, quint32 passkey,
                                                const QDBusMessage& message) {
  m_agent->beginPrompt(PairingPromptKind::Confirm, device.path(), formatPasskey(passkey),
                       message);
}

void BluetoothAgentAdaptor::RequestAuthorization(const QDBusObjectPath& device,
                                                 const QDBusMessage& /*message*/) {
  // Reached only after pairing, when the speaker connects. The user chose this device to pair.
  qInfo() << "Bluetooth authorization granted for" << bluetoothAddressFromDevicePath(device.path());
}

void BluetoothAgentAdaptor::AuthorizeService(const QDBusObjectPath& device, const QString& uuid,
                                             const QDBusMessage& /*message*/) {
  qInfo() << "Bluetooth service" << uuid << "authorized for"
          << bluetoothAddressFromDevicePath(device.path());
}

void BluetoothAgentAdaptor::Cancel() {
  m_agent->cancelAllPrompts(QString::fromLatin1(kErrorCanceled),
                            QStringLiteral("Pairing cancelled"));
}

BluetoothPairingAgent::BluetoothPairingAgent(QObject* parent)
    : QObject(parent), m_adaptor(new BluetoothAgentAdaptor(this)) {
  m_expiryTimer = new QTimer(this);
  m_expiryTimer->setInterval(1000);
  connect(m_expiryTimer, &QTimer::timeout, this, &BluetoothPairingAgent::onExpiryTick);

  // BlueZ forgets its agents when it restarts, so register again when it comes back.
  m_bluezWatcher = new QDBusServiceWatcher(QString::fromLatin1(kBluezService),
                                           QDBusConnection::systemBus(),
                                           QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(m_bluezWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString& /*service*/, const QString& /*oldOwner*/,
                 const QString& newOwner) { onBluezOwnerChanged(newOwner); });
}

BluetoothPairingAgent::~BluetoothPairingAgent() {
  cancelAllPrompts(QString::fromLatin1(kErrorCanceled), QStringLiteral("Crankshaft is stopping"));

  if (m_exported) {
    QDBusInterface manager(QString::fromLatin1(kBluezService),
                           QString::fromLatin1(kAgentManagerPath),
                           QString::fromLatin1(kAgentManagerInterface),
                           QDBusConnection::systemBus());
    manager.call(QStringLiteral("UnregisterAgent"), QVariant::fromValue(agentObjectPath()));
    QDBusConnection::systemBus().unregisterObject(QString::fromLatin1(kAgentPath));
  }
}

auto BluetoothPairingAgent::registerAgent() -> bool {
  QDBusConnection bus = QDBusConnection::systemBus();
  if (!bus.isConnected()) {
    qWarning() << "Bluetooth pairing agent: no system bus connection";
    return false;
  }

  if (!m_exported) {
    if (!bus.registerObject(QString::fromLatin1(kAgentPath), this,
                            QDBusConnection::ExportAdaptors)) {
      qWarning() << "Bluetooth pairing agent: cannot export" << kAgentPath;
      return false;
    }
    m_exported = true;
  }

  QDBusInterface manager(QString::fromLatin1(kBluezService),
                         QString::fromLatin1(kAgentManagerPath),
                         QString::fromLatin1(kAgentManagerInterface), bus);
  if (!manager.isValid()) {
    qDebug() << "Bluetooth pairing agent: BlueZ agent manager not available yet";
    return false;
  }

  // "KeyboardDisplay" tells BlueZ that the head unit can show a code and take typed input,
  // so it asks for PINs, passkeys and yes/no confirmations rather than refusing.
  const QDBusMessage registered = manager.call(
      QStringLiteral("RegisterAgent"), QVariant::fromValue(agentObjectPath()),
      QStringLiteral("KeyboardDisplay"));
  if (registered.type() == QDBusMessage::ErrorMessage &&
      registered.errorName() != QLatin1String("org.bluez.Error.AlreadyExists")) {
    qWarning() << "Bluetooth pairing agent: RegisterAgent failed:" << registered.errorMessage();
    return false;
  }

  const QDBusMessage defaulted =
      manager.call(QStringLiteral("RequestDefaultAgent"), QVariant::fromValue(agentObjectPath()));
  if (defaulted.type() == QDBusMessage::ErrorMessage) {
    qWarning() << "Bluetooth pairing agent: RequestDefaultAgent failed:"
               << defaulted.errorMessage();
    return false;
  }

  qInfo() << "Bluetooth pairing agent registered with BlueZ";
  return true;
}

auto BluetoothPairingAgent::respond(const QString& id, bool accepted, const QString& answer)
    -> PairingResponse {
  const std::optional<PairingPrompt> prompt = m_queue.find(id);
  if (!prompt || !m_messages.contains(id)) {
    return PairingResponse::UnknownPrompt;
  }
  if (accepted && !PairingPromptQueue::isValidAnswer(prompt->kind, answer)) {
    return PairingResponse::InvalidAnswer;
  }

  m_queue.take(id);
  const QDBusMessage message = m_messages.take(id);
  QDBusConnection bus = QDBusConnection::systemBus();

  if (!accepted) {
    bus.send(message.createErrorReply(QString::fromLatin1(kErrorRejected),
                                      QStringLiteral("Pairing was declined")));
  } else if (prompt->kind == PairingPromptKind::Pin) {
    bus.send(message.createReply(QVariant(answer)));
  } else if (prompt->kind == PairingPromptKind::Passkey) {
    bus.send(message.createReply(QVariant::fromValue(answer.toUInt())));
  } else {
    bus.send(message.createReply());
  }

  qInfo() << "Bluetooth pairing prompt" << id << (accepted ? "answered" : "declined");
  closePrompt(id);
  return PairingResponse::Answered;
}

auto BluetoothPairingAgent::beginPrompt(PairingPromptKind kind, const QString& devicePath,
                                        const QString& passkey, const QDBusMessage& message)
    -> void {
  const QString address = bluetoothAddressFromDevicePath(devicePath);
  if (address.isEmpty()) {
    QDBusConnection::systemBus().send(message.createErrorReply(
        QString::fromLatin1(kErrorRejected), QStringLiteral("Unknown Bluetooth device")));
    return;
  }

  // The reply is sent later, from respond() or onExpiryTick(), so BlueZ waits for the user.
  message.setDelayedReply(true);

  const QString id = m_queue.add(kind, address, QString(), passkey,
                                 QDateTime::currentMSecsSinceEpoch(), kPromptTimeoutMs);
  m_messages.insert(id, message);
  if (!m_expiryTimer->isActive()) {
    m_expiryTimer->start();
  }

  // Never log the answer or the passkey. The address identifies the device.
  qInfo() << "Bluetooth pairing prompt" << id << pairingPromptKindName(kind) << "for" << address;
  if (const std::optional<PairingPrompt> prompt = m_queue.find(id)) {
    emit promptRequested(*prompt);
  }
}

auto BluetoothPairingAgent::closePrompt(const QString& id) -> void {
  if (m_messages.isEmpty()) {
    m_expiryTimer->stop();
  }
  emit promptClosed(id);
}

auto BluetoothPairingAgent::cancelAllPrompts(const QString& errorName, const QString& errorText)
    -> void {
  const QStringList ids = m_messages.keys();
  for (const QString& id : ids) {
    m_queue.take(id);
    const QDBusMessage message = m_messages.take(id);
    QDBusConnection::systemBus().send(message.createErrorReply(errorName, errorText));
    closePrompt(id);
  }
}

auto BluetoothPairingAgent::onExpiryTick() -> void {
  const QStringList expired = m_queue.expire(QDateTime::currentMSecsSinceEpoch());
  for (const QString& id : expired) {
    if (!m_messages.contains(id)) {
      continue;
    }
    const QDBusMessage message = m_messages.take(id);
    QDBusConnection::systemBus().send(
        message.createErrorReply(QString::fromLatin1(kErrorCanceled),
                                 QStringLiteral("Pairing prompt timed out")));
    qInfo() << "Bluetooth pairing prompt" << id << "timed out";
    closePrompt(id);
  }
}

auto BluetoothPairingAgent::onBluezOwnerChanged(const QString& newOwner) -> void {
  if (newOwner.isEmpty()) {
    // BlueZ stopped, so its pending calls are gone. Close the prompts without replying.
    const QStringList ids = m_messages.keys();
    for (const QString& id : ids) {
      m_queue.take(id);
      m_messages.remove(id);
      closePrompt(id);
    }
    return;
  }
  registerAgent();
}
