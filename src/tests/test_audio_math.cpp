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

#include <cstdint>
#include <cstring>
#include <vector>

#include "services/audio/AudioMath.h"

namespace {

std::vector<char> toBytes(const std::vector<std::int16_t>& samples) {
  std::vector<char> bytes(samples.size() * sizeof(std::int16_t));
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return bytes;
}

std::vector<std::int16_t> toSamples(const std::vector<char>& bytes) {
  std::vector<std::int16_t> samples(bytes.size() / sizeof(std::int16_t));
  std::memcpy(samples.data(), bytes.data(), bytes.size());
  return samples;
}

}  // namespace

TEST(AudioMathTest, ClampPercentLimitsToRange) {
  EXPECT_EQ(audio_math::clampPercent(-5), 0);
  EXPECT_EQ(audio_math::clampPercent(42), 42);
  EXPECT_EQ(audio_math::clampPercent(250), 100);
}

TEST(AudioMathTest, BalanceCentreLeavesBothChannelsAtUnity) {
  const auto gains = audio_math::balanceGains(0);
  EXPECT_DOUBLE_EQ(gains.left, 1.0);
  EXPECT_DOUBLE_EQ(gains.right, 1.0);
}

TEST(AudioMathTest, BalanceFullLeftSilencesRight) {
  const auto gains = audio_math::balanceGains(-100);
  EXPECT_DOUBLE_EQ(gains.left, 1.0);
  EXPECT_DOUBLE_EQ(gains.right, 0.0);
}

TEST(AudioMathTest, BalanceFullRightSilencesLeft) {
  const auto gains = audio_math::balanceGains(100);
  EXPECT_DOUBLE_EQ(gains.left, 0.0);
  EXPECT_DOUBLE_EQ(gains.right, 1.0);
}

TEST(AudioMathTest, BalanceAttenuatesOppositeChannelLinearly) {
  const auto right = audio_math::balanceGains(50);
  EXPECT_DOUBLE_EQ(right.left, 0.5);
  EXPECT_DOUBLE_EQ(right.right, 1.0);

  const auto left = audio_math::balanceGains(-25);
  EXPECT_DOUBLE_EQ(left.left, 1.0);
  EXPECT_DOUBLE_EQ(left.right, 0.75);
}

TEST(AudioMathTest, BalanceClampsOutOfRangeInput) {
  const auto gains = audio_math::balanceGains(250);
  EXPECT_DOUBLE_EQ(gains.left, 0.0);
  EXPECT_DOUBLE_EQ(gains.right, 1.0);
}

TEST(AudioMathTest, DuckingGainOnlyAppliesWhenActive) {
  EXPECT_DOUBLE_EQ(audio_math::duckingGain(false, 40), 1.0);
  EXPECT_DOUBLE_EQ(audio_math::duckingGain(true, 40), 0.4);
}

TEST(AudioMathTest, DuckingGainClampsPercent) {
  EXPECT_DOUBLE_EQ(audio_math::duckingGain(true, 150), 1.0);
  EXPECT_DOUBLE_EQ(audio_math::duckingGain(true, -10), 0.0);
}

TEST(AudioMathTest, UpmixDuplicatesEachMonoSample) {
  const auto mono = toBytes({1, -2, 300});
  std::vector<char> stereo(mono.size() * 2);

  audio_math::upmixMonoToStereo(mono.data(), mono.size(), stereo.data());

  EXPECT_EQ(toSamples(stereo), (std::vector<std::int16_t>{1, 1, -2, -2, 300, 300}));
}

TEST(AudioMathTest, UnityGainLeavesSamplesUnchanged) {
  const std::vector<std::int16_t> original{32767, -32768, 1000, -1000};
  auto bytes = toBytes(original);

  audio_math::applyStereoGains(bytes.data(), bytes.size(), {1.0, 1.0});

  EXPECT_EQ(toSamples(bytes), original);
}

TEST(AudioMathTest, PerChannelGainsScaleOnlyTheirChannel) {
  auto bytes = toBytes({1000, 1000, -2000, -2000});

  audio_math::applyStereoGains(bytes.data(), bytes.size(), {0.5, 0.0});

  EXPECT_EQ(toSamples(bytes), (std::vector<std::int16_t>{500, 0, -1000, 0}));
}

TEST(AudioMathTest, GainsNeverOverflowFullScaleSamples) {
  auto bytes = toBytes({32767, -32768});

  audio_math::applyStereoGains(bytes.data(), bytes.size(), {1.0, 1.0});

  EXPECT_EQ(toSamples(bytes), (std::vector<std::int16_t>{32767, -32768}));
}
