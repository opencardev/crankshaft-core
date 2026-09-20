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

// ---------------------------------------------------------------------------
// AasdkErrorClassification.h
//
// Pure string-classification helpers for AASDK error messages produced by
// RealAndroidAutoService::onChannelError().
//
// All functions here operate only on the textual description returned by
// aasdk::error::Error::what(), which is the form available at the service
// level once an error has been propagated up through the channel stack.
//
// Keeping the helpers in a header makes them directly unit-testable without
// requiring a full AASDK / Qt / GStreamer stack in the test binary.
//
// AASDK error string format:
//   "AASDK Error: <code>, Native Code: <signed-decimal>, Additional Information: <msg>"
//
// Relevant error codes:
//   10 = USB_TRANSFER     (libusb transfer completion callback error)
//   25 = SSL_WRAPPER      (OpenSSL wrapper error)
//   26 = SEND_HEADER      (messenger send header error)
//   27 = SEND_PAYLOAD     (messenger send payload error)
//   28 = RECV_HEADER      (messenger receive header error)
//   33 = RECV_PAYLOAD     (messenger receive payload error)
//   30 = OPERATION_ABORTED
//
// Relevant libusb transfer status codes (Native Code for USB_TRANSFER errors):
//   1  = LIBUSB_TRANSFER_ERROR
//   2  = LIBUSB_TRANSFER_TIMED_OUT
//   5  = LIBUSB_TRANSFER_NO_DEVICE
//   -4 = LIBUSB_TRANSFER_CANCELLED  (appears as signed "-4" in the string)
// ---------------------------------------------------------------------------

#include <QString>

namespace crankshaft::aasdk_error_classification {

// ---------------------------------------------------------------------------
// Primitive: single-code USB transfer match
// ---------------------------------------------------------------------------

/// Returns true when @p errorText describes a USB_TRANSFER error (AASDK code 10)
/// with exactly the given libusb @p nativeCode.
inline auto isUsbTransferErrorText(const QString& errorText, uint32_t nativeCode) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 10")) &&
         errorText.contains(QStringLiteral("Native Code: %1").arg(nativeCode));
}

// ---------------------------------------------------------------------------
// Recoverable USB receive errors
// ---------------------------------------------------------------------------

/// Returns true when the error string represents a transient USB receive failure
/// that is safe to recover from with a channel receive re-arm rather than a
/// full transport teardown.
///
/// Covered libusb native codes:
///   1  LIBUSB_TRANSFER_ERROR        – generic transfer error; often transient
///   2  LIBUSB_TRANSFER_TIMED_OUT    – phone slow to respond; retry is safe
///   5  LIBUSB_TRANSFER_NO_DEVICE    – phone briefly disconnected; may reconnect
///   -4 LIBUSB_TRANSFER_CANCELLED    – transfer was cancelled by a concurrent
///                                     libusb reset; retry after short delay
inline auto isRecoverableUsbReceiveErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kLibusbTransferError = 1;
  static constexpr uint32_t kLibusbTransferTimedOut = 2;
  static constexpr uint32_t kLibusbTransferNoDevice = 5;

  // Native code -4 appears in the AASDK log string as the signed decimal "-4"
  // because aasdk formats via static_cast<int32_t>(nativeCode).
  // Parenthesise the conjunction to avoid operator-precedence ambiguity with ||.
  const bool isCancelledNative =
      (errorText.contains(QStringLiteral("AASDK Error: 10")) &&
       errorText.contains(QStringLiteral("Native Code: -4")));

  return isUsbTransferErrorText(errorText, kLibusbTransferError) ||
         isUsbTransferErrorText(errorText, kLibusbTransferTimedOut) ||
         isUsbTransferErrorText(errorText, kLibusbTransferNoDevice) ||
         isCancelledNative;
}

// ---------------------------------------------------------------------------
// Timeout / no-device helpers (used by the broader recovery dispatch)
// ---------------------------------------------------------------------------

/// Returns true when the error is a USB transfer timeout (native code 2).
inline auto isUsbTransferTimeoutErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kLibusbTransferTimedOut = 2;
  return isUsbTransferErrorText(errorText, kLibusbTransferTimedOut);
}

/// Returns true when the error is a USB transfer "no device" (native code 5).
inline auto isUsbTransferNoDeviceErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kLibusbTransferNoDevice = 5;
  return isUsbTransferErrorText(errorText, kLibusbTransferNoDevice);
}

/// Returns true when the error string carries native code 5 from any transport
/// layer (USB_TRANSFER, SSL_WRAPPER, send/receive headers, etc.).  Used to
/// detect "phone gone" conditions that warrant a full recovery cycle.
inline auto isTransportNoDeviceErrorText(const QString& errorText) -> bool {
  static constexpr uint32_t kNativeNoDevice = 5;

  if (!errorText.contains(QStringLiteral("Native Code: %1").arg(kNativeNoDevice))) {
    return false;
  }

  return errorText.contains(QStringLiteral("AASDK Error: 10")) ||
         errorText.contains(QStringLiteral("AASDK Error: 25")) ||
         errorText.contains(QStringLiteral("AASDK Error: 26")) ||
         errorText.contains(QStringLiteral("AASDK Error: 27")) ||
         errorText.contains(QStringLiteral("AASDK Error: 28")) ||
         errorText.contains(QStringLiteral("AASDK Error: 33"));
}

/// Returns true when the error is an SSL wrapper no-device condition.
inline auto isSslWrapperNoDeviceErrorText(const QString& errorText) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 25")) &&
         errorText.contains(QStringLiteral("Native Code: 5"));
}

/// Returns true when the error represents an OPERATION_ABORTED (AASDK code 30).
/// These are expected during normal messenger shutdown.
inline auto isOperationAbortedErrorText(const QString& errorText) -> bool {
  return errorText.contains(QStringLiteral("AASDK Error: 30"));
}

}  // namespace crankshaft::aasdk_error_classification
