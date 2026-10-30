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

#include "AudioRouter.h"

#include <unistd.h>

#include <algorithm>

#include <QDateTime>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "../../hal/multimedia/MediaPipeline.h"
#include "../config/ConfigService.h"
#include "../logging/Logger.h"
#include "AudioMath.h"
#include "AudioOutputs.h"

namespace {

// Source formats are the Android Auto channel layouts. Core chooses them and
// advertises them to the phone in RealAndroidAutoService (service discovery);
// AASDK does not define them. Confirm them on hardware.
struct SourceFormat {
  const char* name;
  int sampleRate;
  int channels;
};

constexpr SourceFormat kMediaFormat{"media", 48000, 2};
constexpr SourceFormat kGuidanceFormat{"guidance", 16000, 1};
constexpr SourceFormat kSystemFormat{"system", 16000, 1};
constexpr SourceFormat kTelephonyFormat{"telephony", 16000, 1};

const SourceFormat* formatForRole(AAudioStreamRole role) {
  switch (role) {
    case AAudioStreamRole::MEDIA:
      return &kMediaFormat;
    case AAudioStreamRole::GUIDANCE:
      return &kGuidanceFormat;
    case AAudioStreamRole::SYSTEM_AUDIO:
      return &kSystemFormat;
    case AAudioStreamRole::PHONE_CALL:
      return &kTelephonyFormat;
    default:
      return nullptr;
  }
}

constexpr auto kStateKeyVolume = "core.audio.volume";
constexpr auto kStateKeyMuted = "core.audio.muted";
constexpr auto kStateKeyBalance = "core.audio.balance";
constexpr auto kStateKeyDucking = "core.audio.ducking_enabled";
constexpr auto kStateKeyOutput = "core.audio.output";
constexpr auto kStateKeyBluetoothAutoSelect = "core.audio.bluetooth_auto_select";
constexpr int kPersistDebounceMs = 500;

QStringList runtimeDirCandidates() {
  QStringList candidates;

  const QString pipewireRuntimeDir = qEnvironmentVariable("PIPEWIRE_RUNTIME_DIR");
  if (!pipewireRuntimeDir.isEmpty()) {
    candidates.append(pipewireRuntimeDir);
  }

  const QString xdgRuntimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!xdgRuntimeDir.isEmpty() && !candidates.contains(xdgRuntimeDir)) {
    candidates.append(xdgRuntimeDir);
  }

  const QString uidRuntimeDir = QStringLiteral("/run/user/%1").arg(static_cast<uint>(geteuid()));
  if (!candidates.contains(uidRuntimeDir)) {
    candidates.append(uidRuntimeDir);
  }

  return candidates;
}

// Environment for pactl, so it reaches the same PulseAudio or PipeWire server as
// the probe in initializePulseAudio().
QProcessEnvironment pactlEnvironment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QStringList runtimeDirs = runtimeDirCandidates();
  if (!runtimeDirs.isEmpty()) {
    const QString runtimeDir = runtimeDirs.first();
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
    if (!env.contains(QStringLiteral("PULSE_SERVER"))) {
      env.insert(QStringLiteral("PULSE_SERVER"),
                 QStringLiteral("unix:%1/pulse/native").arg(runtimeDir));
    }
  }
  return env;
}

bool sameOutputs(const QList<AudioOutput>& a, const QList<AudioOutput>& b) {
  return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(),
                    [](const AudioOutput& x, const AudioOutput& y) {
                      return x.id == y.id && x.name == y.name && x.bluetooth == y.bluetooth;
                    });
}

}  // namespace

AudioRouter::AudioRouter(MediaPipeline* mediaPipeline, QObject* parent)
    : QObject(parent), m_mediaPipeline(mediaPipeline) {
  if (!m_mediaPipeline) {
    Logger::instance().error(QStringLiteral("[AudioRouter] MediaPipeline is null"));
    return;
  }

  m_saveTimer = new QTimer(this);
  m_saveTimer->setSingleShot(true);
  m_saveTimer->setInterval(kPersistDebounceMs);
  connect(m_saveTimer, &QTimer::timeout, this, &AudioRouter::persistState);

  m_outputPollTimer = new QTimer(this);
  m_outputPollTimer->setInterval(kOutputPollMs);
  connect(m_outputPollTimer, &QTimer::timeout, this, &AudioRouter::refreshOutputs);
}

AudioRouter::~AudioRouter() {
  if (m_saveTimer && m_saveTimer->isActive()) {
    m_saveTimer->stop();
    persistState();
  }
  shutdown();
}

bool AudioRouter::initialize() {
  if (m_initialized) {
    return true;
  }
  if (!m_mediaPipeline) {
    return false;
  }

  Logger::instance().info(QStringLiteral("[AudioRouter] Initialising audio router"));
  m_pipewireAvailable = false;
  m_pulseaudioAvailable = false;

  // Backend detection is a one-off startup probe. It is reported to clients but
  // does not gate routing, because the pipeline sink decides what is actually played.
  if (initializePipeWire()) {
    m_pipewireAvailable = true;
    Logger::instance().info(QStringLiteral("[AudioRouter] PipeWire backend initialised"));
  } else if (initializePulseAudio()) {
    m_pulseaudioAvailable = true;
    Logger::instance().info(QStringLiteral("[AudioRouter] PulseAudio backend initialised"));
  } else {
    Logger::instance().warning(QStringLiteral("[AudioRouter] No audio backend available"));
  }

  createMixerSources();
  applyStoredState();

  m_initialized = m_pipewireAvailable || m_pulseaudioAvailable;
  if (m_initialized) {
    // The first listing applies the saved output once the sinks are known.
    refreshOutputs();
    m_outputPollTimer->start();
  }
  return m_initialized;
}

void AudioRouter::createMixerSources() {
  AudioHAL* audioHal = m_mediaPipeline->audioHAL();
  for (const SourceFormat& format :
       {kMediaFormat, kGuidanceFormat, kSystemFormat, kTelephonyFormat}) {
    const QString name = QString::fromLatin1(format.name);
    if (!audioHal->addSource(name, format.sampleRate)) {
      Logger::instance().error(
          QStringLiteral("[AudioRouter] Failed to create mixer source %1").arg(name));
      continue;
    }
    Logger::instance().info(QStringLiteral("[AudioRouter] Mixer source %1: %2 Hz, %3 channels")
                                .arg(name)
                                .arg(format.sampleRate)
                                .arg(format.channels));
  }
}

AudioStreamFormat audioStreamFormat(AAudioStreamRole role) {
  const SourceFormat* format = formatForRole(role);
  if (!format) {
    return {0, 0};
  }
  return {format->sampleRate, format->channels};
}

void AudioRouter::applyStoredState() {
  const auto& config = ConfigService::instance();
  m_volume.store(audio_math::clampPercent(
      config.get(QString::fromLatin1(kStateKeyVolume), kDefaultVolume).toInt()));
  m_muted.store(config.get(QString::fromLatin1(kStateKeyMuted), false).toBool());
  m_balance.store(audio_math::clampBalance(
      config.get(QString::fromLatin1(kStateKeyBalance), 0).toInt()));
  m_duckingEnabled.store(config.get(QString::fromLatin1(kStateKeyDucking), true).toBool());
  m_preferredOutput = config.get(QString::fromLatin1(kStateKeyOutput), QString()).toString();
  m_bluetoothAutoSelect =
      config.get(QString::fromLatin1(kStateKeyBluetoothAutoSelect), true).toBool();

  if (m_mediaPipeline) {
    AudioHAL* audioHal = m_mediaPipeline->audioHAL();
    audioHal->setVolume(m_volume.load());
    audioHal->setMute(m_muted.load());
  }
}

void AudioRouter::schedulePersist() {
  if (m_saveTimer) {
    m_saveTimer->start();
  }
}

void AudioRouter::persistState() {
  auto& config = ConfigService::instance();
  config.set(QString::fromLatin1(kStateKeyVolume), m_volume.load());
  config.set(QString::fromLatin1(kStateKeyMuted), m_muted.load());
  config.set(QString::fromLatin1(kStateKeyBalance), m_balance.load());
  config.set(QString::fromLatin1(kStateKeyDucking), m_duckingEnabled.load());
  config.set(QString::fromLatin1(kStateKeyOutput), m_preferredOutput);
  config.set(QString::fromLatin1(kStateKeyBluetoothAutoSelect), m_bluetoothAutoSelect);

  const QString path = config.loadedFilePath();
  if (!path.isEmpty() && !config.save(path)) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Failed to persist audio state to %1")
                                   .arg(path));
  }
}

bool AudioRouter::initializePipeWire() {
  // Prefer socket presence checks first; this avoids false negatives when
  // helper binaries are unavailable but PipeWire is already running.
  const QStringList runtimeDirs = runtimeDirCandidates();
  for (const QString& runtimeDir : runtimeDirs) {
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    if (QFileInfo::exists(socketPath)) {
      return true;
    }
  }

  // Check if PipeWire daemon is running via pw-cli
  QProcess pwProcess;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (!runtimeDirs.isEmpty()) {
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDirs.first());
    env.insert(QStringLiteral("PIPEWIRE_RUNTIME_DIR"), runtimeDirs.first());
  }
  pwProcess.setProcessEnvironment(env);
  pwProcess.setProgram(QStringLiteral("pw-cli"));
  pwProcess.setArguments(QStringList() << QStringLiteral("info") << QStringLiteral("0"));
  pwProcess.start();

  if (!pwProcess.waitForStarted(1000)) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PipeWire not available"));
    return false;
  }

  const bool finished = pwProcess.waitForFinished(3000);
  const int exitCode = pwProcess.exitCode();

  if (!finished || exitCode != 0) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PipeWire daemon not running"));
    return false;
  }

  return true;
}

bool AudioRouter::initializePulseAudio() {
  // PipeWire Pulse emulation exposes a native Pulse socket. Prefer checking
  // that socket first, then fall back to pactl probing.
  const QString pulseServer = qEnvironmentVariable("PULSE_SERVER");
  if (pulseServer.startsWith(QStringLiteral("unix:"))) {
    const QString pulseSocket = pulseServer.mid(5);
    if (!pulseSocket.isEmpty() && QFileInfo::exists(pulseSocket)) {
      return true;
    }
  } else {
    const QStringList runtimeDirs = runtimeDirCandidates();
    for (const QString& runtimeDir : runtimeDirs) {
      const QString fallbackSocket = runtimeDir + QStringLiteral("/pulse/native");
      if (QFileInfo::exists(fallbackSocket)) {
        return true;
      }
    }
  }

  // Check if PulseAudio daemon is available via pactl
  QProcess paProcess;
  paProcess.setProcessEnvironment(pactlEnvironment());
  paProcess.setProgram(QStringLiteral("pactl"));
  paProcess.setArguments(QStringList() << QStringLiteral("info"));
  paProcess.start();

  if (!paProcess.waitForStarted(1000)) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PulseAudio not available"));
    return false;
  }

  const bool finished = paProcess.waitForFinished(3000);
  const int exitCode = paProcess.exitCode();

  if (!finished || exitCode != 0) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] PulseAudio daemon not running"));
    return false;
  }

  return true;
}

bool AudioRouter::routeAudioFrame(AAudioStreamRole role, const QByteArray& audioData) {
  if (!m_mediaPipeline) {
    Logger::instance().error(QStringLiteral("[AudioRouter] MediaPipeline is null"));
    return false;
  }

  const SourceFormat* format = formatForRole(role);
  if (!format) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Unknown audio stream role"));
    return false;
  }

  if (audioData.isEmpty()) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Empty audio data"));
    return false;
  }

  const int bytesPerInputFrame = static_cast<int>(audio_math::kBytesPerSample) * format->channels;
  if (audioData.size() % bytesPerInputFrame != 0) {
    Logger::instance().warning(
        QStringLiteral("[AudioRouter] Frame of %1 bytes is not a whole number of %2-byte frames")
            .arg(audioData.size())
            .arg(bytesPerInputFrame));
    return false;
  }

  // Guidance holds ducking open for a short time after its last frame, so the
  // media level does not jump back up between prompt fragments.
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (role == AAudioStreamRole::GUIDANCE) {
    m_guidanceActiveUntilMs.store(nowMs + kGuidanceHoldMs);
  }
  const bool guidanceActive = nowMs < m_guidanceActiveUntilMs.load();
  const bool duckThisStream =
      role != AAudioStreamRole::GUIDANCE && m_duckingEnabled.load() && guidanceActive;
  const double duck = audio_math::duckingGain(duckThisStream, kDefaultDuckPercent);

  // Balance and ducking are both linear, so applying them per source before
  // mixing gives the same result as applying them to the mixed output.
  const audio_math::ChannelGains balance = audio_math::balanceGains(m_balance.load());
  const audio_math::ChannelGains gains{balance.left * duck, balance.right * duck};

  QByteArray stereo;
  if (format->channels == 1) {
    stereo.resize(audioData.size() * 2);
    audio_math::upmixMonoToStereo(audioData.constData(), audioData.size(), stereo.data());
  } else {
    stereo = audioData;
  }
  audio_math::applyStereoGains(stereo.data(), stereo.size(), gains);

  const QString stream = QString::fromLatin1(format->name);
  if (!m_mediaPipeline->pushAudioData(stream, stereo)) {
    Logger::instance().error(QStringLiteral("[AudioRouter] Failed to push audio data to pipeline"));
    return false;
  }

  emit frameRouted(stream, format->sampleRate, 2, stereo);
  return true;
}

QList<AudioOutput> AudioRouter::outputs() const {
  return m_outputs;
}

bool AudioRouter::hasOutput(const QString& id) const {
  return std::any_of(m_outputs.cbegin(), m_outputs.cend(),
                     [&](const AudioOutput& output) { return output.id == id; });
}

bool AudioRouter::setOutputDevice(const QString& id) {
  if (!id.isEmpty() && !hasOutput(id)) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Audio output not found: %1").arg(id));
    return false;
  }

  // The choice is kept even if the sink cannot be opened now, so it is applied again
  // when the output next appears in a listing.
  m_preferredOutput = id;
  schedulePersist();
  const bool applied = applyOutput(id);
  emit outputsChanged();
  return applied;
}

bool AudioRouter::setBluetoothAutoSelect(bool enabled) {
  m_bluetoothAutoSelect = enabled;
  schedulePersist();
  return true;
}

bool AudioRouter::bluetoothAutoSelect() const {
  return m_bluetoothAutoSelect;
}

QString AudioRouter::outputDevice() const {
  return m_preferredOutput;
}

QString AudioRouter::activeOutput() const {
  if (!m_mediaPipeline) {
    return QString();
  }
  return m_mediaPipeline->audioHAL()->outputDevice();
}

bool AudioRouter::applyOutput(const QString& id) {
  if (!m_mediaPipeline) {
    return false;
  }

  if (m_mediaPipeline->audioHAL()->setOutputDevice(id)) {
    Logger::instance().info(QStringLiteral("[AudioRouter] Audio output: %1")
                                .arg(id.isEmpty() ? QStringLiteral("system default") : id));
    return true;
  }

  Logger::instance().warning(QStringLiteral("[AudioRouter] Could not open audio output %1; "
                                            "playing on the system default")
                                 .arg(id));
  return false;
}

void AudioRouter::reconcileOutput() {
  // Use the chosen output while it is present. Otherwise play on the system default,
  // keeping the choice so it applies again when the output returns.
  const bool preferredPresent = m_preferredOutput.isEmpty() || hasOutput(m_preferredOutput);
  const QString target = preferredPresent ? m_preferredOutput : QString();
  if (target == activeOutput()) {
    return;
  }
  applyOutput(target);
}

void AudioRouter::refreshOutputs() {
  if (!m_mediaPipeline || (m_listProcess && m_listProcess->state() != QProcess::NotRunning)) {
    return;
  }

  if (!m_listProcess) {
    m_listProcess = new QProcess(this);
    connect(m_listProcess, &QProcess::finished, this, &AudioRouter::onSinkListFinished);
  }

  m_listProcess->setProcessEnvironment(pactlEnvironment());
  m_listProcess->start(QStringLiteral("pactl"),
                       QStringList() << QStringLiteral("-f") << QStringLiteral("json")
                                     << QStringLiteral("list") << QStringLiteral("sinks"));
}

void AudioRouter::onSinkListFinished() {
  // shutdown() kills a listing that is still running; its finished signal then arrives
  // after the router has stopped, and must not touch the sink.
  if (!m_initialized) {
    return;
  }

  const bool listed =
      m_listProcess->exitStatus() == QProcess::NormalExit && m_listProcess->exitCode() == 0;
  if (!listed) {
    Logger::instance().debug(QStringLiteral("[AudioRouter] Could not list audio outputs: %1")
                                 .arg(QString::fromUtf8(m_listProcess->readAllStandardError())
                                          .trimmed()));
  }

  // A failed listing means no outputs are visible, so the sink falls back to the default.
  const QList<AudioOutput> found =
      listed ? parsePactlSinks(m_listProcess->readAllStandardOutput()) : QList<AudioOutput>();
  // The first listing is the startup state. A speaker already connected then must not
  // override the saved output, so only Bluetooth outputs that appear later count.
  const bool startupListing = listed && !m_outputsListed;
  if (listed) {
    m_outputsListed = true;
  }
  if (sameOutputs(found, m_outputs)) {
    return;
  }

  const QList<AudioOutput> arrived =
      startupListing ? QList<AudioOutput>() : newBluetoothOutputs(found, m_outputs);
  m_outputs = found;
  Logger::instance().info(QStringLiteral("[AudioRouter] Audio outputs changed: %1 found")
                              .arg(static_cast<qlonglong>(m_outputs.size())));
  if (m_bluetoothAutoSelect && !arrived.isEmpty() &&
      m_preferredOutput != arrived.first().id) {
    Logger::instance().info(QStringLiteral("[AudioRouter] Bluetooth output connected, "
                                          "selecting %1")
                                .arg(arrived.first().id));
    m_preferredOutput = arrived.first().id;
    schedulePersist();
  }
  reconcileOutput();
  emit outputsChanged();
}

bool AudioRouter::setVolume(int percent) {
  if (percent < audio_math::kMinPercent || percent > audio_math::kMaxPercent) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Invalid volume level: %1").arg(percent));
    return false;
  }

  m_volume.store(percent);
  if (m_mediaPipeline) {
    m_mediaPipeline->audioHAL()->setVolume(percent);
  }
  schedulePersist();
  return true;
}

int AudioRouter::volume() const {
  return m_volume.load();
}

bool AudioRouter::setMuted(bool muted) {
  m_muted.store(muted);
  if (m_mediaPipeline) {
    m_mediaPipeline->audioHAL()->setMute(muted);
  }
  schedulePersist();
  return true;
}

bool AudioRouter::isMuted() const {
  return m_muted.load();
}

bool AudioRouter::setBalance(int balance) {
  if (balance < audio_math::kMinBalance || balance > audio_math::kMaxBalance) {
    Logger::instance().warning(QStringLiteral("[AudioRouter] Invalid balance: %1").arg(balance));
    return false;
  }

  m_balance.store(balance);
  schedulePersist();
  return true;
}

int AudioRouter::balance() const {
  return m_balance.load();
}

bool AudioRouter::enableAudioDucking(bool enable) {
  m_duckingEnabled.store(enable);
  Logger::instance().info(QStringLiteral("[AudioRouter] Audio ducking %1")
                              .arg(enable ? QStringLiteral("enabled") : QStringLiteral("disabled")));
  schedulePersist();
  return true;
}

bool AudioRouter::isAudioDuckingEnabled() const {
  return m_duckingEnabled.load();
}

QVariantMap AudioRouter::stateSnapshot() const {
  QString backend = QStringLiteral("None");
  if (m_pipewireAvailable) {
    backend = QStringLiteral("PipeWire");
  } else if (m_pulseaudioAvailable) {
    backend = QStringLiteral("PulseAudio");
  }

  QVariantMap state;
  state[QStringLiteral("available")] = true;
  state[QStringLiteral("backend")] = backend;
  state[QStringLiteral("backend_available")] = m_initialized;
  state[QStringLiteral("volume")] = m_volume.load();
  state[QStringLiteral("muted")] = m_muted.load();
  state[QStringLiteral("balance")] = m_balance.load();
  state[QStringLiteral("ducking_enabled")] = m_duckingEnabled.load();

  QVariantList outputList;
  for (const AudioOutput& output : m_outputs) {
    QVariantMap entry;
    entry[QStringLiteral("id")] = output.id;
    entry[QStringLiteral("name")] = output.name;
    entry[QStringLiteral("bluetooth")] = output.bluetooth;
    outputList.append(entry);
  }
  state[QStringLiteral("outputs")] = outputList;
  state[QStringLiteral("output")] = m_preferredOutput;
  state[QStringLiteral("bluetooth_auto_select")] = m_bluetoothAutoSelect;
  state[QStringLiteral("active_output")] = activeOutput();
  return state;
}

bool AudioRouter::shutdown() {
  if (!m_initialized) {
    return true;
  }

  Logger::instance().info(QStringLiteral("[AudioRouter] Shutting down audio router"));

  m_outputPollTimer->stop();
  if (m_listProcess && m_listProcess->state() != QProcess::NotRunning) {
    m_listProcess->kill();
  }

  if (m_mediaPipeline && m_mediaPipeline->isActive()) {
    m_mediaPipeline->stop();
  }

  m_initialized = false;
  m_pipewireAvailable = false;
  m_pulseaudioAvailable = false;
  return true;
}
