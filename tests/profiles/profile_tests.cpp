#include "settings/streamingpreferences.h"
#include "settings/settingsmigration.h"

#include <QCoreApplication>
#include <QMetaProperty>
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

class ApplicationNameGuard
{
public:
    ApplicationNameGuard() : m_Name(QCoreApplication::applicationName()) {}
    ~ApplicationNameGuard() { QCoreApplication::setApplicationName(m_Name); }
private:
    QString m_Name;
};

static QVariantMap settingsValues(const QSettings& settings)
{
    QVariantMap values;
    for (const QString& key : settings.allKeys()) {
        values.insert(key, settings.value(key));
    }
    return values;
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
        prefs->setUnfocusedPauseDelaySeconds(12);
        prefs->muteOnFocusLoss = true;
        prefs->muteAudioWhenHidden = true;
        prefs->setUnfocusedAudioMuteDelaySeconds(75);
        prefs->setHiddenAudioMuteDelaySeconds(5);
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
        if (!prefs->pauseVideoWhenUnfocused || prefs->unfocusedPauseDelaySeconds != 12 ||
            prefs->language != StreamingPreferences::LANG_FR || !prefs->muteAudioWhenHidden ||
            prefs->unfocusedAudioMuteDelaySeconds != 75 || prefs->hiddenAudioMuteDelaySeconds != 5 ||
            prefs->muteOnFocusLoss) return 5;
        prefs->setProfileHost("windows-host-a");
        if (prefs->currentProfile() != "Gaming / HDR" || prefs->width != 3840 || prefs->fps != 120 ||
            !prefs->enableHdr || prefs->bitrateKbps != 70000 ||
            prefs->videoCodecConfig != StreamingPreferences::VCC_FORCE_HEVC || !prefs->muteOnFocusLoss) return 6;
        prefs->setProfileHost("windows-host-b");
        if (prefs->currentProfile() != "Work" || prefs->width != 2560 || prefs->height != 1440 || prefs->fps != 30 ||
            prefs->muteOnFocusLoss || !prefs->muteAudioWhenHidden ||
            prefs->unfocusedAudioMuteDelaySeconds != 75 || prefs->hiddenAudioMuteDelaySeconds != 5) return 7;
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
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 0);
        QVERIFY(prefs->createProfile("Example"));
        prefs->language = StreamingPreferences::LANG_JA;
        prefs->enableMdns = false;
        prefs->pauseVideoWhenHidden = false;
        prefs->pauseVideoWhenUnfocused = true;
        prefs->setUnfocusedPauseDelaySeconds(17);
        prefs->setCurrentProfile("Default");
        QCOMPARE(prefs->language, StreamingPreferences::LANG_JA);
        QVERIFY(!prefs->enableMdns);
        QVERIFY(!prefs->pauseVideoWhenHidden);
        QVERIFY(prefs->pauseVideoWhenUnfocused);
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 17);
        QVERIFY(prefs->createProfile("Another"));
        prefs->setUnfocusedPauseDelaySeconds(33);
        prefs->setCurrentProfile("Example");
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 33);
        prefs->reload();
        QCOMPARE(prefs->language, StreamingPreferences::LANG_JA);
        QVERIFY(!prefs->enableMdns);
        QVERIFY(!prefs->pauseVideoWhenHidden);
        QVERIFY(prefs->pauseVideoWhenUnfocused);
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 33);
    }

    void backgroundAudioDelaysStayGlobalAndIndependent()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(!prefs->muteAudioWhenHidden);
        QVERIFY(!prefs->muteOnFocusLoss);
        QCOMPARE(prefs->unfocusedAudioMuteDelaySeconds, 0);
        QCOMPARE(prefs->hiddenAudioMuteDelaySeconds, 0);
        QVERIFY(prefs->createProfile("Audio"));
        prefs->muteOnFocusLoss = true;
        prefs->muteAudioWhenHidden = true;
        prefs->setUnfocusedAudioMuteDelaySeconds(120);
        prefs->setHiddenAudioMuteDelaySeconds(2);
        prefs->setUnfocusedPauseDelaySeconds(60);
        prefs->setCurrentProfile("Default");
        // Preserve the existing per-preset focus mute choice, while new timers
        // and hidden muting apply globally without changing video's timer.
        QVERIFY(!prefs->muteOnFocusLoss);
        QVERIFY(prefs->muteAudioWhenHidden);
        QCOMPARE(prefs->unfocusedAudioMuteDelaySeconds, 120);
        QCOMPARE(prefs->hiddenAudioMuteDelaySeconds, 2);
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 60);
        prefs->setUnfocusedAudioMuteDelaySeconds(3);
        prefs->setHiddenAudioMuteDelaySeconds(44);
        prefs->muteAudioWhenHidden = false;
        prefs->setCurrentProfile("Audio");
        QVERIFY(prefs->muteOnFocusLoss);
        QVERIFY(!prefs->muteAudioWhenHidden);
        QCOMPARE(prefs->unfocusedAudioMuteDelaySeconds, 3);
        QCOMPARE(prefs->hiddenAudioMuteDelaySeconds, 44);
        QCOMPARE(prefs->unfocusedPauseDelaySeconds, 60);
        prefs->save();
        prefs->reload();
        QCOMPARE(prefs->unfocusedAudioMuteDelaySeconds, 3);
        QCOMPARE(prefs->hiddenAudioMuteDelaySeconds, 44);
        QVERIFY(!prefs->muteAudioWhenHidden);
        prefs->setCurrentProfile("Audio");
        QVERIFY(prefs->muteOnFocusLoss);
    }

    void backgroundDelaysAreClamped_data()
    {
        QTest::addColumn<QByteArray>("propertyName");
        QTest::addColumn<QString>("settingsKey");
        QTest::addColumn<int>("input");
        QTest::addColumn<int>("expected");
        const QList<QPair<QByteArray, QString>> properties = {
            {"unfocusedPauseDelaySeconds", "unfocusedpausedelayseconds"},
            {"unfocusedAudioMuteDelaySeconds", "unfocusedaudiomutedelayseconds"},
            {"hiddenAudioMuteDelaySeconds", "hiddenaudiomutedelayseconds"}
        };
        const QList<QPair<int, int>> limits = {{-1, 0}, {0, 0}, {7, 7}, {3600, 3600}, {3601, 3600}};
        for (const auto& property : properties) {
            for (const auto& limit : limits) {
                const QByteArray name = property.first + '-' + QByteArray::number(limit.first);
                QTest::newRow(name.constData()) << property.first << property.second << limit.first << limit.second;
            }
        }
    }

    void backgroundDelaysAreClamped()
    {
        QFETCH(QByteArray, propertyName);
        QFETCH(QString, settingsKey);
        QFETCH(int, input);
        QFETCH(int, expected);
        auto* prefs = StreamingPreferences::get();
        const int propertyIndex = prefs->metaObject()->indexOfProperty(propertyName.constData());
        QVERIFY(propertyIndex >= 0);
        const QMetaProperty property = prefs->metaObject()->property(propertyIndex);
        QSignalSpy delaySpy(prefs, property.notifySignal());
        QVERIFY(prefs->setProperty(propertyName.constData(), input));
        QCOMPARE(prefs->property(propertyName.constData()).toInt(), expected);
        QCOMPARE(delaySpy.count(), expected == 0 ? 0 : 1);
        QVERIFY(prefs->setProperty(propertyName.constData(), input));
        QCOMPARE(delaySpy.count(), expected == 0 ? 0 : 1);

        // A manually edited configuration must obey the same limits.
        QSettings settings;
        settings.setValue(settingsKey, input);
        settings.sync();
        prefs->reload();
        QCOMPARE(prefs->property(propertyName.constData()).toInt(), expected);

        // C++ launch overrides use public members; saving must also validate.
        int* publicValue = propertyName == "unfocusedPauseDelaySeconds" ? &prefs->unfocusedPauseDelaySeconds :
            propertyName == "unfocusedAudioMuteDelaySeconds" ? &prefs->unfocusedAudioMuteDelaySeconds : &prefs->hiddenAudioMuteDelaySeconds;
        *publicValue = input;
        prefs->save();
        QCOMPARE(*publicValue, expected);
        QCOMPARE(QSettings().value(settingsKey).toInt(), expected);
        *publicValue = input;
        std::unique_ptr<StreamingPreferences> snapshot(prefs->createSessionSnapshot());
        QCOMPARE(snapshot->property(propertyName.constData()).toInt(), expected);
    }

    void sessionSnapshotDoesNotChangeWithEditedProfiles()
    {
        auto* prefs = StreamingPreferences::get();
        QVERIFY(prefs->createProfile("Session"));
        prefs->width = 3840;
        prefs->fps = 120;
        prefs->pauseVideoWhenUnfocused = true;
        prefs->setUnfocusedPauseDelaySeconds(29);
        prefs->muteOnFocusLoss = true;
        prefs->muteAudioWhenHidden = true;
        prefs->setUnfocusedAudioMuteDelaySeconds(91);
        prefs->setHiddenAudioMuteDelaySeconds(4);
        prefs->absoluteMouseMode = true;
        std::unique_ptr<StreamingPreferences> snapshot(prefs->createSessionSnapshot());
        prefs->setCurrentProfile("Default");
        prefs->pauseVideoWhenUnfocused = false;
        prefs->setUnfocusedPauseDelaySeconds(0);
        prefs->muteAudioWhenHidden = false;
        prefs->setUnfocusedAudioMuteDelaySeconds(0);
        prefs->setHiddenAudioMuteDelaySeconds(0);
        QCOMPARE(snapshot->width, 3840);
        QCOMPARE(snapshot->fps, 120);
        QVERIFY(snapshot->absoluteMouseMode);
        QVERIFY(snapshot->pauseVideoWhenUnfocused);
        QCOMPARE(snapshot->unfocusedPauseDelaySeconds, 29);
        QVERIFY(snapshot->muteOnFocusLoss);
        QVERIFY(snapshot->muteAudioWhenHidden);
        QCOMPARE(snapshot->unfocusedAudioMuteDelaySeconds, 91);
        QCOMPARE(snapshot->hiddenAudioMuteDelaySeconds, 4);
        QCOMPARE(prefs->width, 1920);
        QCOMPARE(prefs->fps, 60);
    }

    void legacyMigrationPreservesTypedCredentialsAndHostProfiles()
    {
        ApplicationNameGuard applicationName;
        QCoreApplication::setApplicationName("LegacyPersonal");
        QSettings legacy;
        legacy.clear();
        legacy.sync();
        auto* prefs = StreamingPreferences::get();
        prefs->reload();
        prefs->setProfileHost("migration-host");
        QVERIFY(prefs->createProfile("Legacy TV"));
        prefs->width = 3840;
        prefs->fps = 120;
        prefs->enableHdr = true;
        prefs->setHiddenAudioMuteDelaySeconds(7);
        prefs->save();
        // Synthetic credentials exercise opaque byte arrays and identity
        // strings without reading any real pairing information.
        legacy.setValue("uniqueid", QString("synthetic-client-uuid"));
        legacy.setValue("certificate", QByteArray::fromHex("30040001ff00"));
        legacy.setValue("key", QByteArray::fromHex("010203000405"));
        legacy.setValue("computers/1/uuid", QString("migration-host"));
        legacy.setValue("computers/size", 1);
        legacy.sync();
        const QVariantMap expected = settingsValues(legacy);
        QVERIFY(expected.contains("streamprofiles/1/name"));
        QVERIFY(expected.contains("hoststreamprofiles/1/uuid"));

        QCoreApplication::setApplicationName("MigrationImported");
        QSettings current;
        current.clear();
        current.sync();
        QVERIFY(SettingsMigration::importLegacyPreferencesIfEmpty("LegacyPersonal"));
        QCOMPARE(settingsValues(current), expected);
        for (auto value = expected.cbegin(); value != expected.cend(); ++value) {
            QCOMPARE(current.value(value.key()).userType(), value.value().userType());
        }
        QCOMPARE(settingsValues(legacy), expected);
        prefs->reload();
        prefs->setProfileHost("migration-host");
        QCOMPARE(prefs->currentProfile(), QString("Legacy TV"));
        QCOMPARE(prefs->width, 3840);
        QCOMPARE(prefs->fps, 120);
        QVERIFY(prefs->enableHdr);
        QCOMPARE(prefs->hiddenAudioMuteDelaySeconds, 7);
        QCOMPARE(current.value("certificate").toByteArray(), QByteArray::fromHex("30040001ff00"));
        QCOMPARE(current.value("key").toByteArray(), QByteArray::fromHex("010203000405"));
    }

    void legacyMigrationNeverOverwritesCurrentSettings()
    {
        ApplicationNameGuard applicationName;
        QCoreApplication::setApplicationName("MigrationLegacyExisting");
        QSettings legacy;
        legacy.clear();
        legacy.setValue("width", 3840);
        legacy.setValue("certificate", QByteArray("legacy-certificate"));
        legacy.setValue("legacy-only", true);
        legacy.sync();
        QCoreApplication::setApplicationName("MigrationCurrentExisting");
        QSettings current;
        current.clear();
        current.setValue("width", 1280);
        current.setValue("certificate", QByteArray("current-certificate"));
        current.sync();
        const QVariantMap expected = settingsValues(current);
        QVERIFY(!SettingsMigration::importLegacyPreferencesIfEmpty("MigrationLegacyExisting"));
        QCOMPARE(settingsValues(current), expected);
        QVERIFY(!current.contains("legacy-only"));
    }

    void absentLegacySettingsDoNotCreateCurrentPreferences()
    {
        ApplicationNameGuard applicationName;
        QCoreApplication::setApplicationName("MigrationMissingLegacy");
        QSettings legacy;
        legacy.clear();
        legacy.sync();
        QCoreApplication::setApplicationName("MigrationEmptyCurrent");
        QSettings current;
        current.clear();
        current.sync();
        QVERIFY(!SettingsMigration::importLegacyPreferencesIfEmpty("MigrationMissingLegacy"));
        QVERIFY(current.allKeys().isEmpty());
        QVERIFY(legacy.allKeys().isEmpty());
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
