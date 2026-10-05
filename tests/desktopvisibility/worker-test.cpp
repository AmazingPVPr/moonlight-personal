// Run with dbus-run-session so the test uses a private message bus and never
// connects to or loads scripts in the user's real KWin compositor.
#include "../../app/streaming/desktopvisibility.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <SDL.h>
#include <atomic>
#include <cstdio>

class FakeScript : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")
public:
    FakeScript(const QDBusConnection& bus) : m_Bus(bus) {}
    QJsonObject config;
public slots:
    void run()
    {
        // The production worker is waiting on this run() reply at this point.
        // Send reports later, once it has entered its own event loop.
        QTimer::singleShot(100, this, [this]() { report(m_Bus, false); });
        QTimer::singleShot(300, this, [this]() { report(m_Bus, true); });
        QTimer::singleShot(500, this, [this]() {
            const auto impostor = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                                QStringLiteral("impostor"));
            report(impostor, false);
        });
    }
private:
    void report(const QDBusConnection& connection, bool visible)
    {
        auto call = QDBusMessage::createMethodCall(config.value("service").toString(),
                    config.value("path").toString(), config.value("interface").toString(),
                    QStringLiteral("report"));
        call.setArguments({config.value("token").toString(), true, visible});
        connection.asyncCall(call);
    }
    QDBusConnection m_Bus;
};

class FakeScripting : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")
public:
    FakeScripting(FakeScript& script, std::atomic<int>& unloaded, QString& file)
        : m_Script(script), m_Unloaded(unloaded), m_File(file) {}
public slots:
    int loadScript(const QString& filename, const QString&)
    {
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly)) return -1;
        const QByteArray firstLine = file.readLine();
        const int start = firstLine.indexOf('{');
        const int end = firstLine.lastIndexOf('}');
        m_Script.config = QJsonDocument::fromJson(firstLine.mid(start, end - start + 1)).object();
        m_File = filename;
        return m_Script.config.isEmpty() ? -1 : 42;
    }
    bool unloadScript(const QString&) { ++m_Unloaded; return true; }
private:
    FakeScript& m_Script;
    std::atomic<int>& m_Unloaded;
    QString& m_File;
};

class FakeKWin : public QThread
{
public:
    std::atomic<bool> ready{false};
    std::atomic<bool> available{false};
    std::atomic<int> unloaded{0};
    QString scriptFilename;
    void run() override
    {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                     QStringLiteral("fake-kwin"));
        FakeScript script(bus);
        FakeScripting scripting(script, unloaded, scriptFilename);
        const bool registered = bus.registerService(QStringLiteral("org.kde.KWin")) &&
                bus.registerObject(QStringLiteral("/Scripting"), &scripting, QDBusConnection::ExportAllSlots) &&
                bus.registerObject(QStringLiteral("/Scripting/Script42"), &script, QDBusConnection::ExportAllSlots);
        available = registered;
        ready = true;
        if (registered) exec();
        bus.unregisterService(QStringLiteral("org.kde.KWin"));
        bus.unregisterObject(QStringLiteral("/Scripting"));
        bus.unregisterObject(QStringLiteral("/Scripting/Script42"));
        QDBusConnection::disconnectFromBus(QStringLiteral("impostor"));
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-kwin"));
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("MOONLIGHT_PRIVATE_TEST_BUS") != QStringLiteral("1")) {
        std::fprintf(stderr, "Run under a private bus: dbus-run-session -- env MOONLIGHT_PRIVATE_TEST_BUS=1 SDL_VIDEODRIVER=dummy ./desktopvisibility-worker-test\n");
        return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    SDL_Window* window = SDL_CreateWindow("Moonlight visibility test", 0, 0, 32, 32, SDL_WINDOW_HIDDEN);
    if (!window) return 2;

    FakeKWin compositor;
    compositor.start();
    QElapsedTimer deadline;
    deadline.start();
    while (!compositor.ready && deadline.elapsed() < 2000) SDL_Delay(5);
    if (!compositor.available) {
        compositor.quit(); compositor.wait();
        std::fprintf(stderr, "Could not start private fake compositor.\n");
        return 2;
    }

    std::atomic<int> hidden{0}, shown{0};
    DesktopVisibility monitor;
    QObject::connect(&monitor, &DesktopVisibility::desktopVisibleChanged, &monitor,
                     [&](bool visible) { visible ? ++shown : ++hidden; }, Qt::DirectConnection);
    monitor.startMonitoring(window);
    // Intentionally never process Qt events in the main thread, matching the
    // production Session SDL loop. D-Bus callbacks must still arrive.
    deadline.restart();
    while (deadline.elapsed() < 1000) SDL_Delay(5);
    monitor.stopMonitoring();
    const bool firstFileRemoved = !compositor.scriptFilename.isEmpty() &&
            !QFile::exists(compositor.scriptFilename);
    // Renderer recreation reuses the same monitor after stopMonitoring().
    // QThread must reset its interruption state and deliver callbacks again.
    monitor.startMonitoring(window);
    deadline.restart();
    while (deadline.elapsed() < 1000) SDL_Delay(5);
    monitor.stopMonitoring();
    compositor.quit(); compositor.wait();
    const bool workerOk = hidden == 2 && shown == 2 && compositor.unloaded == 2 && firstFileRemoved &&
            !compositor.scriptFilename.isEmpty() && !QFile::exists(compositor.scriptFilename);

    std::atomic<int> unsupported{0};
    DesktopVisibility unsupportedMonitor;
    QObject::connect(&unsupportedMonitor, &DesktopVisibility::monitoringUnavailable,
                     &unsupportedMonitor, [&](const QString&) { ++unsupported; }, Qt::DirectConnection);
    unsupportedMonitor.startMonitoring(window);
    unsupportedMonitor.wait();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!workerOk || unsupported == 0) {
        std::fprintf(stderr, "Worker test failed: hidden=%d shown=%d unload=%d unsupported=%d\n",
                     hidden.load(), shown.load(), compositor.unloaded.load(), unsupported.load());
        return 1;
    }
    std::puts("Passed private-bus worker delivery/restart, sender validation, unload/file cleanup, and unsupported fallback checks.");
    return 0;
}

#include "worker-test.moc"
