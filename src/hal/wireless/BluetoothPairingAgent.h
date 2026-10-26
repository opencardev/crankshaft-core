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

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QString>

#include "BluetoothPairingPrompts.h"

class BluetoothPairingAgent;
class QDBusServiceWatcher;
class QTimer;

/**
 * @brief Exports org.bluez.Agent1 on the system bus. BlueZ calls it during pairing.
 *
 * This adaptor only forwards each call to BluetoothPairingAgent, which owns the state.
 */
class BluetoothAgentAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.bluez.Agent1")

 public:
  explicit BluetoothAgentAdaptor(BluetoothPairingAgent* agent);

 public slots:
  void Release();
  void RequestPinCode(const QDBusObjectPath& device, const QDBusMessage& message);
  void DisplayPinCode(const QDBusObjectPath& device, const QString& pincode);
  void RequestPasskey(const QDBusObjectPath& device, const QDBusMessage& message);
  void DisplayPasskey(const QDBusObjectPath& device, quint32 passkey, quint16 entered);
  void RequestConfirmation(const QDBusObjectPath& device, quint32 passkey,
                           const QDBusMessage& message);
  void RequestAuthorization(const QDBusObjectPath& device, const QDBusMessage& message);
  void AuthorizeService(const QDBusObjectPath& device, const QString& uuid,
                        const QDBusMessage& message);
  void Cancel();

 private:
  BluetoothPairingAgent* m_agent;
};

/**
 * @brief Pairing agent for BlueZ, so a speaker's PIN or confirmation reaches the user.
 *
 * BlueZ calls the agent while a pairing is in progress. For a question that needs the
 * user, the call is held (a delayed DBus reply), a PairingPrompt is emitted, and the
 * call is answered when respond() is called or the prompt times out.
 *
 * The agent registers with the "KeyboardDisplay" capability, so BlueZ can ask for a
 * PIN, a passkey, or a yes/no confirmation.
 */
class BluetoothPairingAgent : public QObject {
  Q_OBJECT

 public:
  static constexpr auto kAgentPath = "/org/crankshaft/bluetooth/agent";
  static constexpr qint64 kPromptTimeoutMs = 60000;

  explicit BluetoothPairingAgent(QObject* parent = nullptr);
  ~BluetoothPairingAgent() override;

  /**
   * @brief Register with BlueZ as the default agent.
   *
   * Also called when BlueZ restarts, because a restarted BlueZ has no agent.
   */
  auto registerAgent() -> bool;

  /**
   * @brief Answer a prompt. An accepted answer must be valid for the prompt's kind.
   *
   * A rejected answer, or an invalid answer, leaves the prompt pending, so the user can
   * correct it. UnknownPrompt means the prompt already timed out, was cancelled, or was
   * answered.
   */
  auto respond(const QString& id, bool accepted, const QString& answer) -> PairingResponse;

 signals:
  /** @brief BlueZ needs an answer from the user. */
  void promptRequested(const PairingPrompt& prompt);

  /** @brief Show a code for the user to type on the speaker. No answer is needed. */
  void displayRequested(const QString& address, const QString& code);

  /** @brief A prompt is no longer pending: answered, cancelled, or timed out. */
  void promptClosed(const QString& id);

 private:
  friend class BluetoothAgentAdaptor;

  auto beginPrompt(PairingPromptKind kind, const QString& devicePath, const QString& passkey,
                   const QDBusMessage& message) -> void;
  auto closePrompt(const QString& id) -> void;
  auto cancelAllPrompts(const QString& errorName, const QString& errorText) -> void;
  auto onExpiryTick() -> void;
  auto onBluezOwnerChanged(const QString& newOwner) -> void;

  BluetoothAgentAdaptor* m_adaptor = nullptr;
  PairingPromptQueue m_queue;
  // BlueZ calls waiting for an answer, keyed by prompt id.
  QHash<QString, QDBusMessage> m_messages;
  QTimer* m_expiryTimer = nullptr;
  QDBusServiceWatcher* m_bluezWatcher = nullptr;
  bool m_exported = false;
};
