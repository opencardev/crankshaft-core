#!/bin/bash
# Project: Crankshaft
# This file is part of Crankshaft project.
# Copyright (C) 2025 OpenCarDev Team
#
#  Crankshaft is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 3 of the License, or
#  (at your option) any later version.
#
#  Crankshaft is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with Crankshaft. If not, see <http://www.gnu.org/licenses/>.

set -e
set -u

ACTION="${1:-add}"
MODEL="${2:-unknown}"
USB_PATH="${3:-unknown}"

LOG_TAG="crankshaft-aa-trigger"
RUNTIME_DIR="/run/crankshaft"
STAMP_FILE="${RUNTIME_DIR}/aa_udev_trigger.stamp"
LOCK_FILE="${RUNTIME_DIR}/aa_udev_trigger.lock"
DEBOUNCE_MS="${CRANKSHAFT_AA_UDEV_DEBOUNCE_MS:-2500}"

mkdir -p "${RUNTIME_DIR}"

log_message() {
  local level="$1"
  shift
  logger -t "${LOG_TAG}" -p "user.${level}" "$*"
}

now_ms() {
  if date +%s%3N >/dev/null 2>&1; then
    date +%s%3N
    return
  fi

  python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
}

exec 9>"${LOCK_FILE}"
if command -v flock >/dev/null 2>&1; then
  flock -n 9 || exit 0
fi

if [ "${ACTION}" = "remove" ]; then
  # Clear debounce state so a fast unplug/replug can retrigger immediately.
  rm -f "${STAMP_FILE}" 2>/dev/null || true
  log_message "debug" "Handled remove event model=${MODEL} path=${USB_PATH}"
  exit 0
fi

if [ "${ACTION}" != "add" ]; then
  log_message "debug" "Ignoring unsupported action=${ACTION} model=${MODEL} path=${USB_PATH}"
  exit 0
fi

CURRENT_MS="$(now_ms)"
LAST_MS="0"

if [ -f "${STAMP_FILE}" ]; then
  LAST_MS="$(cat "${STAMP_FILE}" 2>/dev/null || echo 0)"
fi

if [ "${LAST_MS}" -gt 0 ] 2>/dev/null; then
  ELAPSED_MS="$((CURRENT_MS - LAST_MS))"
  if [ "${ELAPSED_MS}" -lt "${DEBOUNCE_MS}" ]; then
    log_message "debug" "Skipping trigger (debounced) model=${MODEL} path=${USB_PATH} elapsed_ms=${ELAPSED_MS}"
    exit 0
  fi
fi

echo "${CURRENT_MS}" >"${STAMP_FILE}"

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  if ! systemctl is-active --quiet crankshaft-core.service; then
    systemctl start crankshaft-core.service >/dev/null 2>&1 || true
    sleep 1
  fi
fi

if ! command -v python3 >/dev/null 2>&1; then
  log_message "warn" "python3 not available; cannot send websocket trigger"
  exit 0
fi

if python3 - <<'PY'
import base64
import json
import os
import socket
import struct
import sys

host = '127.0.0.1'
port = 8080

payload = json.dumps({
    'type': 'publish',
    'topic': 'android-auto/launch',
    'payload': {}
}, separators=(',', ':')).encode('utf-8')

key = base64.b64encode(os.urandom(16)).decode('ascii')
request = (
    'GET / HTTP/1.1\r\n'
    f'Host: {host}:{port}\r\n'
    'Upgrade: websocket\r\n'
    'Connection: Upgrade\r\n'
    f'Sec-WebSocket-Key: {key}\r\n'
    'Sec-WebSocket-Version: 13\r\n\r\n'
)

sock = socket.create_connection((host, port), timeout=2.5)
sock.settimeout(2.5)
sock.sendall(request.encode('ascii'))

response = sock.recv(4096)
if b' 101 ' not in response:
    sock.close()
    sys.exit(2)

mask = os.urandom(4)
header = bytearray([0x81])
length = len(payload)

if length <= 125:
    header.append(0x80 | length)
elif length <= 65535:
    header.append(0x80 | 126)
    header.extend(struct.pack('!H', length))
else:
    header.append(0x80 | 127)
    header.extend(struct.pack('!Q', length))

header.extend(mask)
masked_payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
sock.sendall(bytes(header) + masked_payload)
sock.close()
sys.exit(0)
PY
then
  log_message "info" "Triggered android-auto/launch via websocket model=${MODEL} path=${USB_PATH}"
else
  log_message "warn" "Failed websocket trigger model=${MODEL} path=${USB_PATH}"
fi

exit 0
