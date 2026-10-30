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

#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

/**
 * @brief What BlueZ is asking the user to do during pairing.
 */
enum class PairingPromptKind {
  Pin,      ///< Type the PIN the speaker expects (legacy pairing)
  Passkey,  ///< Type the numeric passkey shown by the speaker
  Confirm   ///< Confirm that the passkey shown on the head unit matches the speaker
};

/**
 * @brief One pending question from BlueZ, waiting for the user's answer.
 */
struct PairingPrompt {
  QString id;
  PairingPromptKind kind = PairingPromptKind::Pin;
  QString address;
  QString name;
  QString passkey;      ///< Six digits, for Confirm only
  qint64 deadlineMs = 0;  ///< Epoch milliseconds after which the prompt expires
};

/**
 * @brief Outcome of answering a prompt.
 */
enum class PairingResponse {
  Answered,       ///< The answer was sent to BlueZ
  UnknownPrompt,  ///< The prompt timed out, was closed, or was already answered
  InvalidAnswer   ///< The answer is not valid for this kind of prompt; the prompt stays open
};

/**
 * @brief Name of a prompt kind as sent to clients: "pin", "passkey" or "confirm".
 */
QString pairingPromptKindName(PairingPromptKind kind);

/**
 * @brief BlueZ device address from its object path, for example
 *        "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" gives "AA:BB:CC:DD:EE:FF".
 *
 * Returns an empty string for a path that is not a device.
 */
QString bluetoothAddressFromDevicePath(const QString& devicePath);

/**
 * @brief Pending pairing prompts, keyed by id. Pure logic, with no DBus or timers.
 *
 * The agent calls add() when BlueZ asks a question, find() and take() when the user
 * answers, and expire() on a timer. Times are passed in, so tests control them.
 */
class PairingPromptQueue {
 public:
  // BlueZ limits: a PIN is at most 16 characters, a passkey is at most 999999.
  static constexpr int kMaxPinLength = 16;
  static constexpr int kMaxPasskeyDigits = 6;

  /**
   * @brief Add a prompt and return its id.
   */
  auto add(PairingPromptKind kind, const QString& address, const QString& name,
           const QString& passkey, qint64 nowMs, qint64 timeoutMs) -> QString;

  /**
   * @brief The prompt with this id, if it is still pending.
   */
  [[nodiscard]] auto find(const QString& id) const -> std::optional<PairingPrompt>;

  /**
   * @brief Remove and return the prompt with this id, if it is still pending.
   */
  auto take(const QString& id) -> std::optional<PairingPrompt>;

  /**
   * @brief Remove every prompt whose deadline has passed, and return their ids.
   */
  auto expire(qint64 nowMs) -> QStringList;

  /**
   * @brief Whether an answer is well formed for this kind of prompt.
   *
   * Pin: 1 to 16 printable ASCII characters. Passkey: 1 to 6 digits, at most 999999.
   * Confirm takes no answer text, so any text is accepted.
   */
  static auto isValidAnswer(PairingPromptKind kind, const QString& answer) -> bool;

 private:
  QHash<QString, PairingPrompt> m_prompts;
  int m_nextId = 1;
};
