#include <gtest/gtest.h>

#include "../services/android_auto/AndroidAutoService.h"

TEST(AndroidAutoTransportModeTest, ParsesWebRtcModeFromString) {
  EXPECT_EQ(AndroidAutoService::videoTransportModeFromString(QStringLiteral("webrtc")),
            AndroidAutoService::VideoTransportMode::WEBRTC);
  EXPECT_EQ(AndroidAutoService::videoTransportModeFromString(QStringLiteral("WebRTC")),
            AndroidAutoService::VideoTransportMode::WEBRTC);
}

TEST(AndroidAutoTransportModeTest, DefaultsToWebSocketJpegForUnknownValues) {
  EXPECT_EQ(AndroidAutoService::videoTransportModeFromString(QStringLiteral("unknown")),
            AndroidAutoService::VideoTransportMode::WEBSOCKET_JPEG);
  EXPECT_EQ(AndroidAutoService::videoTransportModeToString(
                AndroidAutoService::VideoTransportMode::WEBSOCKET_JPEG),
            QStringLiteral("websocket-jpeg"));
}
