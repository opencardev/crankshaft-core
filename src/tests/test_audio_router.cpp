#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <unistd.h>

#include "../hal/multimedia/MediaPipeline.h"
#include "../services/audio/AudioRouter.h"

namespace {

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const char* name, const QByteArray& value)
      : m_name(name), m_hadPreviousValue(qEnvironmentVariableIsSet(name)),
        m_previousValue(qgetenv(name)) {
    qputenv(m_name, value);
  }

  ~ScopedEnvironmentVariable() {
    if (m_hadPreviousValue) {
      qputenv(m_name, m_previousValue);
    } else {
      qunsetenv(m_name);
    }
  }

 private:
  const char* m_name;
  bool m_hadPreviousValue;
  QByteArray m_previousValue;
};

QString runtimeSocketPath(const char* socketName) {
  return QStringLiteral("/run/user/%1/%2").arg(static_cast<uint>(geteuid())).arg(socketName);
}

}  // namespace

TEST(AudioRouterTest, InitializeReturnsTrueWhenPipeWireSocketExists) {
  QTemporaryDir runtimeDir;
  ASSERT_TRUE(runtimeDir.isValid());

  QFile socketFile(runtimeDir.filePath(QStringLiteral("pipewire-0")));
  ASSERT_TRUE(socketFile.open(QIODevice::WriteOnly));
  socketFile.close();

  ScopedEnvironmentVariable pipewireRuntimeDir("PIPEWIRE_RUNTIME_DIR", runtimeDir.path().toUtf8());
  ScopedEnvironmentVariable xdgRuntimeDir("XDG_RUNTIME_DIR", runtimeDir.path().toUtf8());
  ScopedEnvironmentVariable pulseServer("PULSE_SERVER", QByteArray());

  MediaPipeline mediaPipeline;
  AudioRouter audioRouter(&mediaPipeline);

  EXPECT_TRUE(audioRouter.initialize());
  EXPECT_TRUE(audioRouter.shutdown());
}

TEST(AudioRouterTest, InitializeReturnsFalseAfterBackendDisappearsAndShutdown) {
  const QString pipewireSocket = runtimeSocketPath("pipewire-0");
  const QString pulseSocket = runtimeSocketPath("pulse/native");
  if (QFileInfo::exists(pipewireSocket) || QFileInfo::exists(pulseSocket)) {
    GTEST_SKIP() << "System audio sockets are present; skip the no-backend regression path";
  }

  QTemporaryDir runtimeDir;
  ASSERT_TRUE(runtimeDir.isValid());

  QFile socketFile(runtimeDir.filePath(QStringLiteral("pipewire-0")));
  ASSERT_TRUE(socketFile.open(QIODevice::WriteOnly));
  socketFile.close();

  ScopedEnvironmentVariable pipewireRuntimeDir("PIPEWIRE_RUNTIME_DIR", runtimeDir.path().toUtf8());
  ScopedEnvironmentVariable xdgRuntimeDir("XDG_RUNTIME_DIR", runtimeDir.path().toUtf8());
  ScopedEnvironmentVariable pulseServer("PULSE_SERVER", QByteArray());

  MediaPipeline mediaPipeline;
  AudioRouter audioRouter(&mediaPipeline);

  ASSERT_TRUE(audioRouter.initialize());
  EXPECT_TRUE(audioRouter.shutdown());

  ASSERT_TRUE(QFile::remove(socketFile.fileName()));
  EXPECT_FALSE(audioRouter.initialize());
  EXPECT_TRUE(audioRouter.shutdown());
}