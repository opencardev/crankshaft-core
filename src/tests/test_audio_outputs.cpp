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

#include "services/audio/AudioOutputs.h"

namespace {

// Trimmed from `pactl -f json list sinks` on a Pi with one ALSA sink and one BlueZ sink.
const char* kTwoSinks = R"([
  {
    "index": 0,
    "state": "SUSPENDED",
    "name": "alsa_output.platform-bcm2835_audio.stereo-fallback",
    "description": "Built-in Audio Stereo",
    "driver": "PipeWire"
  },
  {
    "index": 1,
    "state": "RUNNING",
    "name": "bluez_output.AA_BB_CC_DD_EE_FF.1",
    "description": "Car Speaker",
    "driver": "PipeWire"
  }
])";

}  // namespace

TEST(AudioOutputsTest, ParsesEverySinkWithItsDescription) {
  const QList<AudioOutput> outputs = parsePactlSinks(kTwoSinks);

  ASSERT_EQ(outputs.size(), 2);
  EXPECT_EQ(outputs[0].id, QStringLiteral("alsa_output.platform-bcm2835_audio.stereo-fallback"));
  EXPECT_EQ(outputs[0].name, QStringLiteral("Built-in Audio Stereo"));
  EXPECT_EQ(outputs[1].id, QStringLiteral("bluez_output.AA_BB_CC_DD_EE_FF.1"));
  EXPECT_EQ(outputs[1].name, QStringLiteral("Car Speaker"));
}

TEST(AudioOutputsTest, MarksBluetoothSinksOnly) {
  const QList<AudioOutput> outputs = parsePactlSinks(kTwoSinks);

  ASSERT_EQ(outputs.size(), 2);
  EXPECT_FALSE(outputs[0].bluetooth);
  EXPECT_TRUE(outputs[1].bluetooth);
}

TEST(AudioOutputsTest, UsesNameWhenDescriptionIsMissing) {
  const QList<AudioOutput> outputs =
      parsePactlSinks(R"([{"name": "alsa_output.usb-DAC.analog-stereo"}])");

  ASSERT_EQ(outputs.size(), 1);
  EXPECT_EQ(outputs[0].name, outputs[0].id);
}

TEST(AudioOutputsTest, SkipsEntriesWithoutAName) {
  const QList<AudioOutput> outputs =
      parsePactlSinks(R"([{"description": "Nameless"}, {"name": "hdmi_output", "description": "HDMI"}])");

  ASSERT_EQ(outputs.size(), 1);
  EXPECT_EQ(outputs[0].id, QStringLiteral("hdmi_output"));
}

TEST(AudioOutputsTest, EmptyArrayGivesNoOutputs) {
  EXPECT_TRUE(parsePactlSinks("[]").isEmpty());
}

TEST(AudioOutputsTest, MalformedOrNonArrayInputGivesNoOutputs) {
  EXPECT_TRUE(parsePactlSinks("").isEmpty());
  EXPECT_TRUE(parsePactlSinks("not json").isEmpty());
  EXPECT_TRUE(parsePactlSinks(R"({"name": "single object, not an array"})").isEmpty());
}
