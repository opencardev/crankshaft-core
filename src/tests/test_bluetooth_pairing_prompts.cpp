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

#include <gtest/gtest.h>

#include "hal/wireless/BluetoothPairingPrompts.h"

TEST(BluetoothPairingPromptsTest, AddressIsReadFromDevicePath) {
  EXPECT_EQ(bluetoothAddressFromDevicePath(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF")),
            QStringLiteral("AA:BB:CC:DD:EE:FF"));
  EXPECT_EQ(bluetoothAddressFromDevicePath(QStringLiteral("/org/bluez/hci1/dev_00_11_22_33_44_55")),
            QStringLiteral("00:11:22:33:44:55"));
}

TEST(BluetoothPairingPromptsTest, NonDevicePathGivesNoAddress) {
  EXPECT_TRUE(bluetoothAddressFromDevicePath(QStringLiteral("/org/bluez/hci0")).isEmpty());
  EXPECT_TRUE(bluetoothAddressFromDevicePath(QStringLiteral("/org/bluez/hci0/dev_AA_BB")).isEmpty());
  EXPECT_TRUE(bluetoothAddressFromDevicePath(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/x"))
                  .isEmpty());
  EXPECT_TRUE(bluetoothAddressFromDevicePath(QString()).isEmpty());
}

TEST(BluetoothPairingPromptsTest, KindNamesMatchTheProtocol) {
  EXPECT_EQ(pairingPromptKindName(PairingPromptKind::Pin), QStringLiteral("pin"));
  EXPECT_EQ(pairingPromptKindName(PairingPromptKind::Passkey), QStringLiteral("passkey"));
  EXPECT_EQ(pairingPromptKindName(PairingPromptKind::Confirm), QStringLiteral("confirm"));
}

TEST(BluetoothPairingPromptsTest, PinMustBeOneToSixteenPrintableCharacters) {
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin, QStringLiteral("0000")));
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin, QStringLiteral("a")));
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin,
                                                QString(PairingPromptQueue::kMaxPinLength, QLatin1Char('x'))));

  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin, QString()));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin,
                                                 QString(PairingPromptQueue::kMaxPinLength + 1,
                                                         QLatin1Char('x'))));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin,
                                                 QString::fromUtf8("p\xc3\xadn")));  // non-ASCII
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Pin,
                                                 QString(QChar(0x07))));  // control character
}

TEST(BluetoothPairingPromptsTest, PasskeyIsOneToSixDigitsNoGreaterThan999999) {
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("0")));
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("000000")));
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("999999")));

  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QString()));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("1234567")));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("12a4")));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral("-1")));
  EXPECT_FALSE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Passkey, QStringLiteral(" 12")));
}

TEST(BluetoothPairingPromptsTest, ConfirmTakesNoAnswerText) {
  EXPECT_TRUE(PairingPromptQueue::isValidAnswer(PairingPromptKind::Confirm, QString()));
}

TEST(BluetoothPairingPromptsTest, AddedPromptIsFoundUntilTaken) {
  PairingPromptQueue queue;
  const QString id = queue.add(PairingPromptKind::Pin, QStringLiteral("AA:BB:CC:DD:EE:FF"),
                               QString(), QString(), 1000, 60000);

  const auto found = queue.find(id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->address, QStringLiteral("AA:BB:CC:DD:EE:FF"));
  EXPECT_EQ(found->deadlineMs, 61000);

  const auto taken = queue.take(id);
  EXPECT_TRUE(taken.has_value());
  EXPECT_FALSE(queue.find(id).has_value());
  EXPECT_FALSE(queue.take(id).has_value());  // a second answer finds nothing
}

TEST(BluetoothPairingPromptsTest, IdsAreUnique) {
  PairingPromptQueue queue;
  const QString first = queue.add(PairingPromptKind::Pin, QStringLiteral("AA:BB:CC:DD:EE:FF"),
                                  QString(), QString(), 0, 1000);
  const QString second = queue.add(PairingPromptKind::Pin, QStringLiteral("AA:BB:CC:DD:EE:FF"),
                                   QString(), QString(), 0, 1000);
  EXPECT_NE(first, second);
}

TEST(BluetoothPairingPromptsTest, ExpiredPromptsAreRemovedAndReported) {
  PairingPromptQueue queue;
  const QString early = queue.add(PairingPromptKind::Pin, QStringLiteral("AA:BB:CC:DD:EE:01"),
                                  QString(), QString(), 0, 1000);
  const QString late = queue.add(PairingPromptKind::Passkey, QStringLiteral("AA:BB:CC:DD:EE:02"),
                                 QString(), QString(), 0, 5000);

  EXPECT_TRUE(queue.expire(999).isEmpty());

  // The deadline itself counts as expired.
  const QStringList expired = queue.expire(1000);
  ASSERT_EQ(expired.size(), 1);
  EXPECT_EQ(expired.first(), early);
  EXPECT_FALSE(queue.find(early).has_value());
  EXPECT_TRUE(queue.find(late).has_value());
}

TEST(BluetoothPairingPromptsTest, ConfirmPromptKeepsItsPasskeyForDisplay) {
  PairingPromptQueue queue;
  const QString id = queue.add(PairingPromptKind::Confirm, QStringLiteral("AA:BB:CC:DD:EE:FF"),
                               QString(), QStringLiteral("012345"), 0, 1000);
  const auto found = queue.find(id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->passkey, QStringLiteral("012345"));
}
