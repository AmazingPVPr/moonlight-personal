#pragma once

#include <QThread>
#include <QString>

struct SDL_Window;

// Runs separately because Session's SDL loop intentionally suspends the Qt
// application's main event loop. Consumers must use a direct connection to a
// thread-safe callback (for example, posting an SDL event).
class DesktopVisibility : public QThread
{
    Q_OBJECT

public:
    explicit DesktopVisibility(QObject* parent = nullptr);
    ~DesktopVisibility() override;

    // Call from the SDL thread while the window is valid. No SDL calls are made
    // by the worker. Stop before renderer/window recreation, then restart with
    // the final SDL window so the monitor captures its current native ID.
    void startMonitoring(SDL_Window* window);
    void stopMonitoring();

signals:
    // Unknown/unavailable desktop information is always treated as visible.
    // Focus and ordinary occlusion are deliberately independent of this signal.
    void desktopVisibleChanged(bool visible);
    void monitoringUnavailable(const QString& reason);

protected:
    void run() override;

private:
    QString m_WindowTitle;
    qint64 m_ProcessId = 0;
    quint64 m_NativeWindowId = 0;
};
