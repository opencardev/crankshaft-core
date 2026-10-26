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

#include "AudioOutputs.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

QList<AudioOutput> parsePactlSinks(const QByteArray& json) {
  QList<AudioOutput> outputs;

  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !document.isArray()) {
    return outputs;
  }

  for (const QJsonValue& value : document.array()) {
    const QJsonObject sink = value.toObject();
    const QString name = sink.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) {
      continue;
    }

    AudioOutput output;
    output.id = name;
    output.name = sink.value(QStringLiteral("description")).toString(name);
    output.bluetooth = name.startsWith(QStringLiteral("bluez_output."));
    outputs.append(output);
  }

  return outputs;
}
