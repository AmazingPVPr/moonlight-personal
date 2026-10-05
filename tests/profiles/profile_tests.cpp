#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

// These two platform probes are all the preferences implementation needs.
// Keep tests deterministic and independent of the machine's display server.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

static void configureStorage(const QString& path)
{
    QCoreApplication::setOrganizationName("MoonlightProfileTests");
    QCoreApplication::setApplicationName("Preferences");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
}

static int runPersistenceFixture(const QString& mode)
{
    auto* prefs = StreamingPreferences::get();
    if (mode == "--write-fixture") {
        prefs->width = 1920;
        prefs->fps = 60;
        prefs->setProfileHost("windows-host-a");
        if (!prefs->createProfile("Gaming / HDR")) return 1;
        prefs->width = 3840;
        prefs->fps = 120;
        prefs->enableHdr = true;
        prefs->bitrateKbps = 70000;
        prefs->videoCodecConfig = StreamingPreferences::VCC_FORCE_HEVC;
        prefs->pauseVideoWhenUnfocused = true;
        prefs->language = StreamingPreferences::LANG_FR;
        prefs->save();
        prefs->setProfileHost("windows-host-b");
        if (prefs->width != 1920 || prefs->fps != 60) return 2;
        if (!prefs->createProfile("Work")) return 3;
        prefs->width = 2560;
        prefs->height = 1440;
        prefs->fps = 30;
        prefs->save();
        QSettings().sync();
        return 0;
    }
    if (mode == "--read-fixture") {
        // This is a genuinely new process; its initial selection must never be
        // the last host's preset or leak that preset into the global fallback.
        if (prefs->currentProfile() != "Default" || prefs->width != 1920 || prefs->fps != 60) return 4;
        if (!prefs->pauseVideoWhenUnfocused || prefs->language != StreamingPreferences::LANG_FR) return 5;
        prefs->setProfileHost("windows-host-a");
        if (prefs->currentProfile() != "Gaming / HDR" || prefs->width != 3840 || prefs->fps != 120 ||
            !prefs->enableHdr || prefs->bitrateKbps != 70000 ||
            prefs->videoCodecConfig != StreamingPreferences::VCC_FORCE_HEVC) return 6;
        prefs->setProfileHost("windows-host-b");
        if (prefs->currentProfile() != "Work" || prefs->width != 2560 || prefs->height != 1440 || prefs->fps != 30) return 7;
        prefs->setProfileHost("new-host");
        if (prefs->currentProfile() != "Default" || prefs->width != 1920 || prefs->fps != 60 || prefs->enableHdr) return 8;
        return 0;
    }
    return 9;
}

class ProfileTests : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_Storage.isValid());
        configureStorage(m_Storage.path());
        QSettings settings;
        settings.clear();
        settings.sync();
        StreamingPreferences::get()->reload();
        StreamingPreferences::get()->setProfileHost("");
    }

    void savedProfilesAndHostChoicesSurviveRestart()
    {
        QTemporaryDir storage;
        QVERIFY(storage.isValid());
        for (const QString& mode : {QString("--write-fixture"), QString("--read-fixture")}) {
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {mode, storage.path()});
            QVERIFY(process.waitForFinished(10000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 0);
        }
    }

    void switchingHostsNeverOverwritesGlobalFallback()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->width = 1280;
        prefs->height = 720;
        prefs->setProfileHost("host-a");
        QVERIFY(prefs->createProfile("Fast"));
        prefs->width = 2560;
        prefs->fps = 144;
        prefs->packetSize = 1200;
        prefs->save();
        prefs->setProfileHost("host-b");
        QCOMPARE(prefs->currentProfile(), QString("Default"));
        QCOMPARE(prefs->width, 1280);
        QCOMPARE(prefs->height, 720);
        QCOMPARE(prefs->fps, 60);
        QCOMPARE(prefs->packetSize, 0);
        QVERIFY(prefs->createProfile("Slow"));
        prefs->fps = 30;
        prefs->setProfileHost("host-a");
        QCOMPARE(prefs->currentProfile(), QString("Fast"));
        QCOMPARE(prefs->width, 2560);
        QCOMPARE(prefs->fps, 144);
        QCOMPARE(prefs->packetSize, 1200);
        QCOMPARE(QSettings().value("width").toInt(), 1280);
        QCOMPARE(QSettings().value("fps").toInt(), 60);
        prefs->setProfileHost("host-b");
        QCOMPARE(prefs->currentProfile(), QString("Slow"));
        QCOMPARE(prefs->fps, 30);
    }

    void duplicateRenameAndDeletePreserveHostReferences()
    {
        auto* prefs = StreamingPreferences::get();
        prefs->setProfileHost("host-a");
        QVERIFY(prefs->createProfile("Gaming"));
        prefs->fps = 120;
        QVERIFY(prefs->duplicateProfile("Gaming", "Gaming copy"));
        QCOMPARE(prefs->fps, 120);
        prefs->fps = 90;
        prefs->setCurrentProfile("Gaming");
        QCOMPARE(prefs->fps, 120);
        prefs->setProfileHost("host-b");
        prefs->setCurrentProfile("Gaming");
        QVERIFY(prefs->renameProfile("Gaming", "TV"));
        QCOMPARE(prefs->currentProfile(), QString("TV"));
        prefs->setProfileHost("host-a");
        QCOMPARE(prefs->currentProfile(), QString("TV"));
        QVERIFY(prefs->deleteProfile("TV"));
        QCOMPARE(prefs->currentProfile(), QString("Default"));
        QCOMPARE(prefs->fps, 60);
        prefs->setProfileHost("host-b");
        QCOMPARE(prefs->currentProfile(), QString("Default"));
        prefs->reload();
        QCOMPARE(prefs->profileNames(), QStringList({"Default", "Gaming copy"}));
        prefs->setCurrentProfile("Gaming copy");
        QCOMPARE(prefs->fps, 90);
    }

    void appSettingsAndPausePolicyStayGlobal()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(prefs->pauseVideoWhenHidden);
        QVERIFY(!prefs->pauseVideoWhenUnfocused);
        QVERIFY(prefs->createProfile("Example"));
        prefs->language = StreamingPreferences::LANG_JA;
        prefs->enableMdns = false;
        prefs->pauseVideoWhenHidden = false;
        prefs->pauseVideoWhenUnfocused = true;
        prefs->setCurrentProfile("Default");
        QCOMPARE(prefs->language, StreamingPreferences::LANG_JA);
        QVERIFY(!prefs->enableMdns);
        QVERIFY(!prefs->pauseVideoWhenHidden);
        QVERIFY(prefs->pauseVideoWhenUnfocused);
        prefs->reload();
        QCOMPARE(prefs->language, StreamingPreferences::LANG_JA);
        QVERIFY(!prefs->enableMdns);
        QVERIFY(!prefs->pauseVideoWhenHidden);
        QVERIFY(prefs->pauseVideoWhenUnfocused);
    }

    void sessionSnapshotDoesNotChangeWithEditedProfiles()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(prefs->createProfile("Session"));
        prefs->width = 3840;
        prefs->fps = 120;
        prefs->pauseVideoWhenUnfocused = true;
        prefs->absoluteMouseMode = true;
        std::unique_ptr<StreamingPreferences> snapshot(prefs->createSessionSnapshot());
        prefs->setCurrentProfile("Default");
        prefs->pauseVideoWhenUnfocused = false;
        QCOMPARE(snapshot->width, 3840);
        QCOMPARE(snapshot->fps, 120);
        QVERIFY(snapshot->absoluteMouseMode);
        QVERIFY(snapshot->pauseVideoWhenUnfocused);
        QCOMPARE(prefs->width, 1920);
        QCOMPARE(prefs->fps, 60);
    }

    void invalidProfileOperationsHaveNoSideEffects()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(!prefs->createProfile(""));
        QVERIFY(!prefs->createProfile(" default "));
        QVERIFY(!prefs->createProfile("default"));
        QVERIFY(!prefs->createProfile(QString(81, 'a')));
        QVERIFY(!prefs->deleteProfile("Default"));
        QVERIFY(!prefs->renameProfile("Default", "Original"));
        QVERIFY(!prefs->duplicateProfile("missing", "New"));
        prefs->setCurrentProfile("missing");
        QCOMPARE(prefs->currentProfile(), QString("Default"));
        QCOMPARE(prefs->profileNames(), QStringList({"Default"}));
    }

    void switchingNotifiesAfterWholeSnapshotIsApplied()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(prefs->createProfile("4K"));
        prefs->width = 3840;
        prefs->height = 2160;
        prefs->fps = 120;
        prefs->bitrateKbps = 60000;
        prefs->setCurrentProfile("Default");
        bool coherent = true;
        const auto connection = connect(prefs, &StreamingPreferences::displayModeChanged, this, [&]() {
            coherent = coherent && prefs->width == 3840 && prefs->height == 2160 && prefs->fps == 120 && prefs->bitrateKbps == 60000;
        });
        QSignalSpy profileSpy(prefs, &StreamingPreferences::currentProfileChanged);
        prefs->setCurrentProfile("4K");
        disconnect(connection);
        QVERIFY(coherent);
        QCOMPARE(profileSpy.count(), 1);
    }

private:
    QTemporaryDir m_Storage;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments().at(1).endsWith("-fixture")) {
        configureStorage(app.arguments().at(2));
        return runPersistenceFixture(app.arguments().at(1));
    }
    ProfileTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "profile_tests.moc"
