#include "desktopvisibility.h"

#include <QCoreApplication>
#include <SDL.h>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <functional>

#ifdef HAS_X11
#include "desktopvisibility-x11.h"
#endif
#endif

// SDL_syswm includes Xlib on X11-capable SDL builds. Load it after Qt headers
// and remove macros which collide with Qt's types and generated metaobjects.
#include <SDL_syswm.h>
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef None
#undef None
#endif

#ifdef Q_OS_LINUX

namespace {
const QString KWinService = QStringLiteral("org.kde.KWin");
const QString ScriptingPath = QStringLiteral("/Scripting");
const QString ScriptingInterface = QStringLiteral("org.kde.kwin.Scripting");
const QString ObserverPath = QStringLiteral("/Moonlight/DesktopVisibility");
const QString ObserverInterface = QStringLiteral("com.moonlight_stream.DesktopVisibility");

// This object, its timers, and its D-Bus registration all live in the monitor
// thread, never the Qt main thread that is blocked inside Session's SDL loop.
class KWinVisibilityObserver : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.moonlight_stream.DesktopVisibility")

public:
    KWinVisibilityObserver(const QDBusConnection& bus,
                           const QString& title, qint64 pid, quint64 windowId,
                           std::function<void(bool)> visible,
                           std::function<void(const QString&)> unavailable)
        : m_Bus(bus),
          m_Title(title),
          m_Pid(pid),
          m_WindowId(windowId),
          m_Visible(std::move(visible)),
          m_Unavailable(std::move(unavailable)),
          m_Token(QUuid::createUuid().toString(QUuid::WithoutBraces)),
          m_ScriptName(QStringLiteral("moonlight-visibility-") + m_Token),
          m_Watcher(KWinService, bus, QDBusServiceWatcher::WatchForOwnerChange, this)
    {
        connect(&m_Watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString&, const QString&, const QString& newOwner) {
            m_Owner = newOwner;
            m_ScriptLoaded = false;
            failOpen(QStringLiteral("The desktop compositor changed; video remains active until visibility is available."));
            if (!newOwner.isEmpty()) {
                // Delay until the new compositor has registered its scripting API.
                QTimer::singleShot(2000, this, [this]() { loadScript(); });
            }
        });
        m_HealthTimer.setInterval(2000);
        connect(&m_HealthTimer, &QTimer::timeout, this, [this]() {
            if (!m_LastReport.isValid() || m_LastReport.elapsed() > 7000) {
                failOpen(QStringLiteral("Desktop visibility updates stopped; video remains active."));
            }
        });
    }

    ~KWinVisibilityObserver() override
    {
        m_HealthTimer.stop();
        if (m_ScriptLoaded) {
            scriptingCall(QStringLiteral("unloadScript"), {m_ScriptName});
        }
        m_Bus.unregisterObject(ObserverPath);
    }

    bool initialize()
    {
        if (!m_Bus.isConnected() || !m_Bus.interface()) {
            failOpen(QStringLiteral("The session message bus is unavailable; virtual desktop detection is disabled."));
            return false;
        }
        const QDBusReply<QString> owner = m_Bus.interface()->serviceOwner(KWinService);
        if (!owner.isValid() || owner.value().isEmpty()) {
            failOpen(QStringLiteral("Virtual desktop detection on Wayland currently requires KDE Plasma/KWin."));
            return false;
        }
        m_Owner = owner.value();
        if (!m_Bus.registerObject(ObserverPath, this, QDBusConnection::ExportScriptableSlots)) {
            failOpen(QStringLiteral("Could not register the temporary desktop visibility observer."));
            return false;
        }
        QFile resource(QStringLiteral(":/streaming/desktopvisibility-kwin.js"));
        if (!resource.open(QIODevice::ReadOnly)) {
            failOpen(QStringLiteral("The desktop visibility script is unavailable in this build."));
            return false;
        }
        QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (directory.isEmpty()) {
            directory = QDir::tempPath();
        }
        m_ScriptFile.setFileTemplate(directory + QStringLiteral("/moonlight-visibility-XXXXXX.js"));
        if (!m_ScriptFile.open()) {
            failOpen(QStringLiteral("Could not create the temporary desktop visibility script."));
            return false;
        }
        const QJsonObject config{
            {QStringLiteral("pid"), static_cast<double>(m_Pid)},
            {QStringLiteral("title"), m_Title},
            {QStringLiteral("windowId"), static_cast<double>(m_WindowId)},
            {QStringLiteral("service"), m_Bus.baseService()},
            {QStringLiteral("path"), ObserverPath},
            {QStringLiteral("interface"), ObserverInterface},
            {QStringLiteral("token"), m_Token},
            {QStringLiteral("scriptName"), m_ScriptName},
        };
        const QByteArray script = "var moonlightVisibilityConfig = " +
                QJsonDocument(config).toJson(QJsonDocument::Compact) + ";\n" + resource.readAll();
        if (m_ScriptFile.write(script) != script.size() || !m_ScriptFile.flush()) {
            failOpen(QStringLiteral("Could not write the temporary desktop visibility script."));
            return false;
        }
        m_ScriptFile.close();
        m_LastReport.start();
        m_HealthTimer.start();
        return loadScript();
    }

public slots:
    Q_SCRIPTABLE bool report(const QString& token, bool known, bool visible)
    {
        // The temporary script talks to a unique D-Bus connection, and only the
        // actual compositor owner with the random per-stream token is accepted.
        if (!calledFromDBus() || message().service() != m_Owner || token != m_Token) {
            return false;
        }
        m_LastReport.restart();
        if (!known) {
            failOpen(QStringLiteral("The stream window's virtual desktop could not be identified; video remains active."));
        }
        else {
            if (!m_HasKnownReport || !m_LastError.isEmpty()) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Desktop visibility monitoring active; stream window is %s",
                            visible ? "visible" : "hidden");
            }
            m_HasKnownReport = true;
            m_LastError.clear();
            m_Visible(visible);
        }
        return true;
    }

private:
    QDBusMessage scriptingCall(const QString& method, const QVariantList& arguments)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(KWinService, ScriptingPath,
                                                          ScriptingInterface, method);
        call.setArguments(arguments);
        return m_Bus.call(call, QDBus::Block, 2000);
    }

    bool loadScript()
    {
        if (m_ScriptLoaded || m_Owner.isEmpty() || m_ScriptFile.fileName().isEmpty()) {
            return m_ScriptLoaded;
        }
        const QDBusReply<int> loaded = scriptingCall(QStringLiteral("loadScript"),
                                                    {m_ScriptFile.fileName(), m_ScriptName});
        if (!loaded.isValid() || loaded.value() < 0) {
            failOpen(QStringLiteral("KWin could not load the temporary desktop visibility observer."));
            return false;
        }
        m_ScriptLoaded = true;
        const QString id = QString::number(loaded.value());
        QDBusMessage run = QDBusMessage::createMethodCall(KWinService,
                QStringLiteral("/Scripting/Script") + id,
                QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run"));
        QDBusMessage result = m_Bus.call(run, QDBus::Block, 2000);
        // KWin 5 exposes scripts at /<id>, while KWin 6 uses /Scripting/Script<id>.
        if (result.type() == QDBusMessage::ErrorMessage &&
                (result.errorName() == QStringLiteral("org.freedesktop.DBus.Error.UnknownObject") ||
                 result.errorName() == QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"))) {
            run = QDBusMessage::createMethodCall(KWinService, QStringLiteral("/") + id,
                    QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run"));
            result = m_Bus.call(run, QDBus::Block, 2000);
        }
        if (result.type() == QDBusMessage::ErrorMessage) {
            failOpen(QStringLiteral("KWin could not start the desktop visibility observer."));
            scriptingCall(QStringLiteral("unloadScript"), {m_ScriptName});
            m_ScriptLoaded = false;
            return false;
        }
        m_LastReport.restart();
        return true;
    }

    void failOpen(const QString& reason)
    {
        m_Visible(true);
        if (reason != m_LastError) {
            m_LastError = reason;
            m_Unavailable(reason);
        }
    }

    QDBusConnection m_Bus;
    QString m_Title;
    qint64 m_Pid;
    quint64 m_WindowId;
    std::function<void(bool)> m_Visible;
    std::function<void(const QString&)> m_Unavailable;
    QString m_Token;
    QString m_ScriptName;
    QString m_Owner;
    QString m_LastError;
    QTemporaryFile m_ScriptFile;
    QDBusServiceWatcher m_Watcher;
    QTimer m_HealthTimer;
    QElapsedTimer m_LastReport;
    bool m_HasKnownReport = false;
    bool m_ScriptLoaded = false;
};

}
#endif

DesktopVisibility::DesktopVisibility(QObject* parent)
    : QThread(parent)
{
}

DesktopVisibility::~DesktopVisibility()
{
    stopMonitoring();
}

void DesktopVisibility::startMonitoring(SDL_Window* window)
{
    Q_ASSERT(!isRunning());
    if (!window || isRunning()) {
        return;
    }
    m_WindowTitle = QString::fromUtf8(SDL_GetWindowTitle(window));
    m_ProcessId = QCoreApplication::applicationPid();
    m_NativeWindowId = 0;
#if defined(Q_OS_LINUX) && defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo info = {};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_X11) {
        m_NativeWindowId = info.info.x11.window;
    }
#endif
    start();
}

void DesktopVisibility::stopMonitoring()
{
    requestInterruption();
    quit();
    wait();
}

void DesktopVisibility::run()
{
    bool lastVisible = true;
    auto visible = [this, &lastVisible](bool value) {
        if (value != lastVisible) {
            lastVisible = value;
            emit desktopVisibleChanged(value);
        }
    };
    auto unavailable = [this](const QString& reason) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Desktop visibility: %s", qPrintable(reason));
        emit monitoringUnavailable(reason);
    };
#ifdef Q_OS_LINUX
#ifdef HAS_X11
    // EWMH provides this information without compositor scripting on X11.
    // Use a separate X connection so no SDL display is accessed from this thread.
    if (m_NativeWindowId != 0) {
        int screenIndex = 0;
        xcb_connection_t* connection = xcb_connect(nullptr, &screenIndex);
        if (connection && !xcb_connection_has_error(connection)) {
            auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
            for (int i = 0; i < screenIndex && screen.rem; ++i) {
                xcb_screen_next(&screen);
            }
            const xcb_window_t root = screen.rem ? screen.data->root : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
            const xcb_atom_t currentDesktop = DesktopVisibilityX11::findAtom(connection, "_NET_CURRENT_DESKTOP");
            const xcb_atom_t windowDesktop = DesktopVisibilityX11::findAtom(connection, "_NET_WM_DESKTOP");
            bool available = false;
            QTimer poll;
            auto update = [&]() {
                uint32_t current = 0, assigned = 0;
                const bool known = root && currentDesktop && windowDesktop &&
                        DesktopVisibilityX11::readCardinal(connection, root, currentDesktop, current) &&
                        DesktopVisibilityX11::readCardinal(connection, static_cast<xcb_window_t>(m_NativeWindowId), windowDesktop, assigned);
                visible(!known || assigned == 0xffffffffU || assigned == current);
                if (!known && available) {
                    unavailable(QStringLiteral("X11 desktop information became unavailable; video remains active."));
                }
                available = known;
            };
            connect(&poll, &QTimer::timeout, &poll, update);
            poll.setInterval(500);
            poll.start();
            update();
            if (!available) {
                unavailable(QStringLiteral("This X11 window manager does not expose virtual desktop information."));
            }
            if (!isInterruptionRequested()) {
                exec();
            }
            poll.stop();
            visible(true);
            xcb_disconnect(connection);
            return;
        }
        if (connection) {
            xcb_disconnect(connection);
        }
    }
#endif
    const QString connectionName = QStringLiteral("moonlight-desktop-visibility-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        const QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                               connectionName);
        KWinVisibilityObserver observer(bus, m_WindowTitle, m_ProcessId, m_NativeWindowId,
                                        visible, unavailable);
        if (observer.initialize() && !isInterruptionRequested()) {
            exec();
        }
        visible(true);
    }
    QDBusConnection::disconnectFromBus(connectionName);
#else
    Q_UNUSED(visible);
    unavailable(QStringLiteral("Virtual desktop detection is available on Linux; minimize and focus options can still be used."));
#endif
}

#ifdef Q_OS_LINUX
#include "desktopvisibility.moc"
#endif
