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

// ---------------------------------------------------------------------------
// Tests for AasdkErrorClassification.h
//
// These tests exercise the string-based AASDK error classifiers that drive
// the video-flicker recovery path in RealAndroidAutoService.  The classifiers
// operate on the textual representation produced by aasdk::error::Error::what()
// which is available at the service layer.
//
// Background: transient USB disconnect/reconnect events on Raspberry Pi 3
// produce errors with native codes 1, 2, 5, or -4.  The recovery path must
// correctly identify them as recoverable (re-arm receives only) vs fatal
// (full disconnect/reconnect cycle).  Misclassifying a recoverable error as
// fatal caused GStreamer video pipeline teardown and visible HDMI flicker.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>
#include <QString>

#include "../services/android_auto/AasdkErrorClassification.h"

using namespace crankshaft::aasdk_error_classification;

// ---------------------------------------------------------------------------
// Helper: build an AASDK error string in the canonical format produced by
//   aasdk::error::Error::what() when formatted by Crankshaft:
//   "AASDK Error: <code>, Native Code: <signed-native>, Additional Information: <detail>"
// ---------------------------------------------------------------------------
static QString makeAasdkErrorString(int code, int nativeCode,
                                    const QString& info = QString()) {
  return QStringLiteral("AASDK Error: %1, Native Code: %2, Additional Information: %3")
      .arg(code)
      .arg(nativeCode)
      .arg(info);
}

// ---------------------------------------------------------------------------
// isUsbTransferErrorText
// ---------------------------------------------------------------------------
class IsUsbTransferErrorTextTest : public ::testing::Test {};

TEST_F(IsUsbTransferErrorTextTest, MatchesExactNativeCode) {
  // AASDK error code 10 = USB_TRANSFER; native code 5 = LIBUSB_TRANSFER_NO_DEVICE
  const QString err = makeAasdkErrorString(10, 5);
  EXPECT_TRUE(isUsbTransferErrorText(err, 5));
}

TEST_F(IsUsbTransferErrorTextTest, DoesNotMatchWrongNativeCode) {
  const QString err = makeAasdkErrorString(10, 5);
  EXPECT_FALSE(isUsbTransferErrorText(err, 2));
}

TEST_F(IsUsbTransferErrorTextTest, DoesNotMatchDifferentAasdkCode) {
  // AASDK code 25 = SSL_WRAPPER; should not match USB_TRANSFER check
  const QString err = makeAasdkErrorString(25, 5);
  EXPECT_FALSE(isUsbTransferErrorText(err, 5));
}

TEST_F(IsUsbTransferErrorTextTest, DoesNotMatchEmptyString) {
  EXPECT_FALSE(isUsbTransferErrorText(QString(), 5));
}

// ---------------------------------------------------------------------------
// isRecoverableUsbReceiveErrorText — the key classifier for flicker suppression
// ---------------------------------------------------------------------------
class IsRecoverableUsbReceiveErrorTextTest : public ::testing::Test {};

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode1_TransferError_IsRecoverable) {
  // LIBUSB_TRANSFER_ERROR — generic transfer error; typically transient on Pi3
  const QString err = makeAasdkErrorString(10, 1);
  EXPECT_TRUE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode2_TimedOut_IsRecoverable) {
  // LIBUSB_TRANSFER_TIMED_OUT — phone slow to respond; safe to retry
  const QString err = makeAasdkErrorString(10, 2);
  EXPECT_TRUE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode5_NoDevice_IsRecoverable) {
  // LIBUSB_TRANSFER_NO_DEVICE — brief USB disconnect; phone may reconnect
  // This is the native code observed in the Pi3 kernel log during flicker events.
  const QString err = makeAasdkErrorString(10, 5);
  EXPECT_TRUE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCodeMinus4_Cancelled_IsRecoverable) {
  // LIBUSB_TRANSFER_CANCELLED — transfer cancelled by concurrent libusb reset;
  // appears as "-4" in the signed decimal log string (uint32_t 0xFFFFFFFC).
  const QString err = makeAasdkErrorString(10, -4);
  EXPECT_TRUE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode3_Stall_IsNotRecoverable) {
  // LIBUSB_TRANSFER_STALL — endpoint stall; requires re-enumeration, not transient
  const QString err = makeAasdkErrorString(10, 3);
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode4_Overflow_IsNotRecoverable) {
  // LIBUSB_TRANSFER_OVERFLOW — data overrun; not a connection hiccup
  const QString err = makeAasdkErrorString(10, 4);
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, SslWrapperCode25Native5_IsNotRecoverable) {
  // SSL_WRAPPER no-device errors should NOT be treated as a simple receive
  // re-arm — they require performImmediateTransportRecovery() instead.
  const QString err = makeAasdkErrorString(25, 5);
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, EmptyString_IsNotRecoverable) {
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(QString()));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, UnrelatedString_IsNotRecoverable) {
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(
      QStringLiteral("Connection failed: bluetooth channel error")));
}

TEST_F(IsRecoverableUsbReceiveErrorTextTest, NativeCode5WithLeadingSpace_IsRecoverable) {
  // Defensive: ensure whitespace-tolerant matching does not silently break
  // if the format ever gains a space before the native code value.
  // The real format has no leading space, so this should return false
  // (i.e. we test exact format, not lenient parsing).
  const QString weird = QStringLiteral("AASDK Error: 10, Native Code:  5, Additional Information: ");
  // This should NOT match because the format is "Native Code: 5" (one space, then digit).
  // Two spaces before the digit would not match QString::arg(5) = "5".
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(weird));
}

// ---------------------------------------------------------------------------
// isUsbTransferTimeoutErrorText
// ---------------------------------------------------------------------------
class IsUsbTransferTimeoutErrorTextTest : public ::testing::Test {};

TEST_F(IsUsbTransferTimeoutErrorTextTest, NativeCode2_IsTimeout) {
  EXPECT_TRUE(isUsbTransferTimeoutErrorText(makeAasdkErrorString(10, 2)));
}

TEST_F(IsUsbTransferTimeoutErrorTextTest, NativeCode5_IsNotTimeout) {
  EXPECT_FALSE(isUsbTransferTimeoutErrorText(makeAasdkErrorString(10, 5)));
}

// ---------------------------------------------------------------------------
// isUsbTransferNoDeviceErrorText
// ---------------------------------------------------------------------------
class IsUsbTransferNoDeviceErrorTextTest : public ::testing::Test {};

TEST_F(IsUsbTransferNoDeviceErrorTextTest, NativeCode5_IsNoDevice) {
  EXPECT_TRUE(isUsbTransferNoDeviceErrorText(makeAasdkErrorString(10, 5)));
}

TEST_F(IsUsbTransferNoDeviceErrorTextTest, NativeCode1_IsNotNoDevice) {
  EXPECT_FALSE(isUsbTransferNoDeviceErrorText(makeAasdkErrorString(10, 1)));
}

// ---------------------------------------------------------------------------
// isTransportNoDeviceErrorText — covers multiple AASDK codes with native 5
// ---------------------------------------------------------------------------
class IsTransportNoDeviceErrorTextTest : public ::testing::Test {};

TEST_F(IsTransportNoDeviceErrorTextTest, UsbTransferCode10Native5_IsNoDevice) {
  EXPECT_TRUE(isTransportNoDeviceErrorText(makeAasdkErrorString(10, 5)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, SslWrapperCode25Native5_IsNoDevice) {
  EXPECT_TRUE(isTransportNoDeviceErrorText(makeAasdkErrorString(25, 5)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, SendHeaderCode26Native5_IsNoDevice) {
  EXPECT_TRUE(isTransportNoDeviceErrorText(makeAasdkErrorString(26, 5)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, RecvHeaderCode28Native5_IsNoDevice) {
  EXPECT_TRUE(isTransportNoDeviceErrorText(makeAasdkErrorString(28, 5)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, RecvPayloadCode33Native5_IsNoDevice) {
  EXPECT_TRUE(isTransportNoDeviceErrorText(makeAasdkErrorString(33, 5)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, NativeCode1_IsNotNoDevice) {
  // Native code 1 is a transfer error, not a no-device condition
  EXPECT_FALSE(isTransportNoDeviceErrorText(makeAasdkErrorString(10, 1)));
}

TEST_F(IsTransportNoDeviceErrorTextTest, UnknownCode99Native5_IsNotNoDevice) {
  // Unknown AASDK code should not match even if native code is 5
  EXPECT_FALSE(isTransportNoDeviceErrorText(makeAasdkErrorString(99, 5)));
}

// ---------------------------------------------------------------------------
// isSslWrapperNoDeviceErrorText
// ---------------------------------------------------------------------------
class IsSslWrapperNoDeviceErrorTextTest : public ::testing::Test {};

TEST_F(IsSslWrapperNoDeviceErrorTextTest, Code25Native5_IsSslNoDevice) {
  EXPECT_TRUE(isSslWrapperNoDeviceErrorText(makeAasdkErrorString(25, 5)));
}

TEST_F(IsSslWrapperNoDeviceErrorTextTest, Code10Native5_IsNotSslNoDevice) {
  EXPECT_FALSE(isSslWrapperNoDeviceErrorText(makeAasdkErrorString(10, 5)));
}

TEST_F(IsSslWrapperNoDeviceErrorTextTest, Code25Native1_IsNotSslNoDevice) {
  EXPECT_FALSE(isSslWrapperNoDeviceErrorText(makeAasdkErrorString(25, 1)));
}

// ---------------------------------------------------------------------------
// isOperationAbortedErrorText
// ---------------------------------------------------------------------------
class IsOperationAbortedErrorTextTest : public ::testing::Test {};

TEST_F(IsOperationAbortedErrorTextTest, Code30_IsOperationAborted) {
  EXPECT_TRUE(isOperationAbortedErrorText(makeAasdkErrorString(30, 0)));
}

TEST_F(IsOperationAbortedErrorTextTest, Code10_IsNotOperationAborted) {
  EXPECT_FALSE(isOperationAbortedErrorText(makeAasdkErrorString(10, 0)));
}

// ---------------------------------------------------------------------------
// Recovery classification invariants
//
// Verify that the set of recoverable errors and the set of no-device errors
// are correctly disjoint where required.  SSL_WRAPPER (code 25) native 5 must
// NOT be treated as a simple receive re-arm — it needs the immediate recovery
// path via performImmediateTransportRecovery().
// ---------------------------------------------------------------------------
class RecoveryClassificationInvariantsTest : public ::testing::Test {};

TEST_F(RecoveryClassificationInvariantsTest,
       SslWrapperNoDevice_IsNoDeviceButNotRecoverableReceive) {
  const QString err = makeAasdkErrorString(25, 5);
  EXPECT_TRUE(isTransportNoDeviceErrorText(err));
  EXPECT_TRUE(isSslWrapperNoDeviceErrorText(err));
  // Must NOT fall into the receive re-arm path — would bypass the immediate
  // SSL transport recovery and leave the session in a broken state.
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(RecoveryClassificationInvariantsTest,
       UsbTransferNoDevice_IsRecoverableReceiveWhenConnected) {
  // LIBUSB_TRANSFER_NO_DEVICE on USB_TRANSFER (code 10) IS recoverable as a
  // receive re-arm when the session state is still CONNECTED (i.e. the
  // USB disconnect was very brief).  The state guard in onChannelError() handles
  // the CONNECTED constraint; the classifier just reports the error type.
  const QString err = makeAasdkErrorString(10, 5);
  EXPECT_TRUE(isRecoverableUsbReceiveErrorText(err));
}

TEST_F(RecoveryClassificationInvariantsTest,
       OperationAborted_IsNeitherRecoverableNorNoDevice) {
  // OPERATION_ABORTED (code 30) is emitted during normal shutdown and must not
  // trigger any recovery path.
  const QString err = makeAasdkErrorString(30, 0);
  EXPECT_FALSE(isRecoverableUsbReceiveErrorText(err));
  EXPECT_FALSE(isTransportNoDeviceErrorText(err));
  EXPECT_TRUE(isOperationAbortedErrorText(err));
}
