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

// Dependency-free gain and balance helpers for the audio router.
// Kept free of Qt and GStreamer so the arithmetic can be unit-tested directly.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audio_math {

inline constexpr std::size_t kBytesPerSample = sizeof(std::int16_t);
inline constexpr int kMinPercent = 0;
inline constexpr int kMaxPercent = 100;
inline constexpr int kMinBalance = -100;
inline constexpr int kMaxBalance = 100;

inline auto clampPercent(int value) -> int {
  return value < kMinPercent ? kMinPercent : (value > kMaxPercent ? kMaxPercent : value);
}

inline auto clampBalance(int value) -> int {
  return value < kMinBalance ? kMinBalance : (value > kMaxBalance ? kMaxBalance : value);
}

struct ChannelGains {
  double left = 1.0;
  double right = 1.0;
};

/**
 * @brief Per-channel gains for a balance setting.
 *
 * -100 is full left, 0 is centred, +100 is full right. The opposite channel is
 * attenuated linearly, so neither channel ever exceeds unity gain.
 */
inline auto balanceGains(int balance) -> ChannelGains {
  const int value = clampBalance(balance);
  ChannelGains gains;
  if (value > 0) {
    gains.left = static_cast<double>(kMaxBalance - value) / kMaxBalance;
  } else if (value < 0) {
    gains.right = static_cast<double>(kMaxBalance + value) / kMaxBalance;
  }
  return gains;
}

/**
 * @brief Gain applied to non-guidance streams while guidance is active.
 */
inline auto duckingGain(bool duckingActive, int duckPercent) -> double {
  if (!duckingActive) {
    return 1.0;
  }
  return static_cast<double>(clampPercent(duckPercent)) / kMaxPercent;
}

/**
 * @brief Expand mono S16LE samples to interleaved stereo S16LE by duplicating each sample.
 *
 * @param src Mono input, @p srcBytes long (must be a whole number of samples).
 * @param dst Output of at least 2 * @p srcBytes bytes.
 */
inline auto upmixMonoToStereo(const char* src, std::size_t srcBytes, char* dst) -> void {
  for (std::size_t offset = 0; offset + kBytesPerSample <= srcBytes; offset += kBytesPerSample) {
    std::memcpy(dst + offset * 2, src + offset, kBytesPerSample);
    std::memcpy(dst + offset * 2 + kBytesPerSample, src + offset, kBytesPerSample);
  }
}

/**
 * @brief Scale interleaved stereo S16LE samples in place by per-channel gains.
 *
 * Gains must be in [0, 1]. Because the scale factor never exceeds one, the
 * result always fits in the original sample range and needs no clipping.
 *
 * @param data Interleaved stereo samples (L, R, L, R, ...).
 * @param bytes Byte length, which must be a multiple of four.
 */
inline auto applyStereoGains(char* data, std::size_t bytes, ChannelGains gains) -> void {
  constexpr std::size_t kBytesPerFrame = kBytesPerSample * 2;
  for (std::size_t offset = 0; offset + kBytesPerFrame <= bytes; offset += kBytesPerFrame) {
    std::int16_t left = 0;
    std::int16_t right = 0;
    std::memcpy(&left, data + offset, kBytesPerSample);
    std::memcpy(&right, data + offset + kBytesPerSample, kBytesPerSample);

    const auto scaledLeft = static_cast<std::int16_t>(left * gains.left);
    const auto scaledRight = static_cast<std::int16_t>(right * gains.right);

    std::memcpy(data + offset, &scaledLeft, kBytesPerSample);
    std::memcpy(data + offset + kBytesPerSample, &scaledRight, kBytesPerSample);
  }
}

}  // namespace audio_math
