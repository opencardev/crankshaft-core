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

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * @brief One PulseAudio sink that core can send audio to.
 *
 * Direct-attached devices (ALSA, HDMI, USB) and Bluetooth devices both appear as
 * sinks through the PulseAudio layer that PipeWire provides, so one list covers
 * every output the user can choose.
 */
struct AudioOutput {
  QString id;              ///< PulseAudio sink name; passed to pulsesink as its device
  QString name;            ///< Human-readable description
  bool bluetooth = false;  ///< True for BlueZ sinks (bluez_output.*)
};

/**
 * @brief Parse the output of `pactl -f json list sinks`.
 *
 * Entries without a name are skipped. Returns an empty list when the input is not
 * a JSON array, so a failed listing never looks like a list of devices.
 */
QList<AudioOutput> parsePactlSinks(const QByteArray& json);
