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

#include "BluetoothPairingPrompts.h"

#include <QRegularExpression>

#include <algorithm>

QString pairingPromptKindName(PairingPromptKind kind) {
  switch (kind) {
    case PairingPromptKind::Pin:
      return QStringLiteral("pin");
    case PairingPromptKind::Passkey:
      return QStringLiteral("passkey");
    case PairingPromptKind::Confirm:
      return QStringLiteral("confirm");
  }
  return QString();
}

QString bluetoothAddressFromDevicePath(const QString& devicePath) {
  // BlueZ names a device object "dev_" followed by its address, with ':' written as '_'.
  static const QRegularExpression kDevicePath(
      QStringLiteral("^/org/bluez/hci\\d+/dev_([0-9A-F]{2}(?:_[0-9A-F]{2}){5})$"));
  const QRegularExpressionMatch match = kDevicePath.match(devicePath);
  if (!match.hasMatch()) {
    return QString();
  }
  return match.captured(1).replace(QLatin1Char('_'), QLatin1Char(':'));
}

auto PairingPromptQueue::add(PairingPromptKind kind, const QString& address, const QString& name,
                             const QString& passkey, qint64 nowMs, qint64 timeoutMs) -> QString {
  PairingPrompt prompt;
  prompt.id = QStringLiteral("pairing-%1").arg(m_nextId++);
  prompt.kind = kind;
  prompt.address = address;
  prompt.name = name;
  prompt.passkey = passkey;
  prompt.deadlineMs = nowMs + timeoutMs;
  m_prompts.insert(prompt.id, prompt);
  return prompt.id;
}

auto PairingPromptQueue::find(const QString& id) const -> std::optional<PairingPrompt> {
  const auto it = m_prompts.constFind(id);
  if (it == m_prompts.constEnd()) {
    return std::nullopt;
  }
  return it.value();
}

auto PairingPromptQueue::take(const QString& id) -> std::optional<PairingPrompt> {
  const auto it = m_prompts.find(id);
  if (it == m_prompts.end()) {
    return std::nullopt;
  }
  PairingPrompt prompt = it.value();
  m_prompts.erase(it);
  return prompt;
}

auto PairingPromptQueue::expire(qint64 nowMs) -> QStringList {
  QStringList expired;
  for (auto it = m_prompts.begin(); it != m_prompts.end();) {
    if (nowMs >= it.value().deadlineMs) {
      expired.append(it.key());
      it = m_prompts.erase(it);
    } else {
      ++it;
    }
  }
  return expired;
}

auto PairingPromptQueue::isValidAnswer(PairingPromptKind kind, const QString& answer) -> bool {
  switch (kind) {
    case PairingPromptKind::Confirm:
      return true;
    case PairingPromptKind::Pin:
      if (answer.isEmpty() || answer.size() > kMaxPinLength) {
        return false;
      }
      return std::all_of(answer.cbegin(), answer.cend(), [](QChar ch) {
        return ch.unicode() >= 0x20 && ch.unicode() <= 0x7E;
      });
    case PairingPromptKind::Passkey: {
      static const QRegularExpression kDigits(
          QStringLiteral("^[0-9]{1,%1}$").arg(kMaxPasskeyDigits));
      return kDigits.match(answer).hasMatch() && answer.toUInt() <= 999999U;
    }
  }
  return false;
}
