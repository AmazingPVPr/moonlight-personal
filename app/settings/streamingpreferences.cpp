#include "streamingpreferences.h"
#include "utils.h"

#include <QSettings>
#include <QTranslator>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QtMath>

#include <QtDebug>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_RICHPRESENCE "richpresence"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_DETECTNETBLOCKING "detectnetblocking"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_LANGUAGE "language"
#define SER_RENDERER "renderer"
#define SER_PAUSEHIDDEN "pausevideowhenhidden"
#define SER_PAUSEUNFOCUSED "pausevideowhenunfocused"
#define SER_UNFOCUSEDPAUSEDELAY "unfocusedpausedelayseconds"
#define SER_MUTEAUDIOHIDDEN "muteaudiowhenhidden"
#define SER_UNFOCUSEDAUDIOMUTEDELAY "unfocusedaudiomutedelayseconds"
#define SER_HIDDENAUDIOMUTEDELAY "hiddenaudiomutedelayseconds"

// Only settings that control a stream belong to presets. Discovery, application
// language/window state, diagnostics and the background pause policy stay global.
#define STREAM_PROFILE_FIELDS(X) \
    X(SER_WIDTH, width, int, displayModeChanged) \
    X(SER_HEIGHT, height, int, displayModeChanged) \
    X(SER_FPS, fps, int, displayModeChanged) \
    X(SER_BITRATE, bitrateKbps, int, bitrateChanged) \
    X(SER_UNLOCK_BITRATE, unlockBitrate, bool, unlockBitrateChanged) \
    X(SER_AUTOADJUSTBITRATE, autoAdjustBitrate, bool, autoAdjustBitrateChanged) \
    X(SER_VSYNC, enableVsync, bool, enableVsyncChanged) \
    X(SER_GAMEOPTS, gameOptimizations, bool, gameOptimizationsChanged) \
    X(SER_HOSTAUDIO, playAudioOnHost, bool, playAudioOnHostChanged) \
    X(SER_MULTICONT, multiController, bool, multiControllerChanged) \
    X(SER_QUITAPPAFTER, quitAppAfter, bool, quitAppAfterChanged) \
    X(SER_ABSMOUSEMODE, absoluteMouseMode, bool, absoluteMouseModeChanged) \
    X(SER_ABSTOUCHMODE, absoluteTouchMode, bool, absoluteTouchModeChanged) \
    X(SER_FRAMEPACING, framePacing, bool, framePacingChanged) \
    X(SER_GAMEPADMOUSE, gamepadMouse, bool, gamepadMouseChanged) \
    X(SER_SHOWPERFOVERLAY, showPerformanceOverlay, bool, showPerformanceOverlayChanged) \
    X(SER_SWAPMOUSEBUTTONS, swapMouseButtons, bool, mouseButtonsChanged) \
    X(SER_MUTEONFOCUSLOSS, muteOnFocusLoss, bool, muteOnFocusLossChanged) \
    X(SER_BACKGROUNDGAMEPAD, backgroundGamepad, bool, backgroundGamepadChanged) \
    X(SER_REVERSESCROLL, reverseScrollDirection, bool, reverseScrollDirectionChanged) \
    X(SER_SWAPFACEBUTTONS, swapFaceButtons, bool, swapFaceButtonsChanged) \
    X(SER_KEEPAWAKE, keepAwake, bool, keepAwakeChanged) \
    X(SER_HDR, enableHdr, bool, enableHdrChanged) \
    X(SER_YUV444, enableYUV444, bool, enableYUV444Changed) \
    X(SER_CAPTURESYSKEYS, captureSysKeysMode, int, captureSysKeysModeChanged) \
    X(SER_AUDIOCFG, audioConfig, int, audioConfigChanged) \
    X(SER_VIDEOCFG, videoCodecConfig, int, videoCodecConfigChanged) \
    X(SER_VIDEODEC, videoDecoderSelection, int, videoDecoderSelectionChanged) \
    X(SER_RENDERER, rendererSelection, int, rendererSelectionChanged) \
    X(SER_WINDOWMODE, windowMode, int, windowModeChanged)

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine, bool loadSettings)
    : m_QmlEngine(qmlEngine)
{
    if (loadSettings) {
        reload();
    }
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::reload()
{
    QSettings settings;

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless we have a slow GPU (which can take advantage of wp_viewporter
    // to reduce GPU load with lower resolution video streams).
    if (WMUtils::isRunningWayland() && !WMUtils::isGpuSlow()) {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
    }
    else {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN;
    }
#endif

    width = settings.value(SER_WIDTH, 1920).toInt();
    height = settings.value(SER_HEIGHT, 1080).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    bitrateKbps = settings.value(SER_BITRATE, getDefaultBitrate(width, height, fps, enableYUV444)).toInt();
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    richPresence = settings.value(SER_RICHPRESENCE, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    detectNetworkBlocking = settings.value(SER_DETECTNETBLOCKING, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    pauseVideoWhenHidden = settings.value(SER_PAUSEHIDDEN, true).toBool();
    pauseVideoWhenUnfocused = settings.value(SER_PAUSEUNFOCUSED, false).toBool();
    unfocusedPauseDelaySeconds = qBound(0, settings.value(SER_UNFOCUSEDPAUSEDELAY, 0).toInt(), 3600);
    muteAudioWhenHidden = settings.value(SER_MUTEAUDIOHIDDEN, false).toBool();
    unfocusedAudioMuteDelaySeconds = qBound(0, settings.value(SER_UNFOCUSEDAUDIOMUTEDELAY, 0).toInt(), 3600);
    hiddenAudioMuteDelaySeconds = qBound(0, settings.value(SER_HIDDENAUDIOMUTEDELAY, 0).toInt(), 3600);
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    enableHdr = settings.value(SER_HDR, false).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    rendererSelection = static_cast<RendererSelection>(settings.value(SER_RENDERER,
                                                  static_cast<int>(RendererSelection::RS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        // Try to load from the old preference value too
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());


    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }

    // Legacy top-level values are the global fallback. Never overwrite them
    // with the last host's preset, including when reloading at startup.
    m_DefaultStreamSettings = captureStreamSettings();
    loadProfiles(settings);
    m_CurrentProfile = m_HostProfiles.value(m_ProfileHost, QStringLiteral("Default"));
    if (!m_StreamProfiles.contains(m_CurrentProfile)) {
        m_CurrentProfile = QStringLiteral("Default");
    }
    applyStreamSettings(m_CurrentProfile == QStringLiteral("Default") ?
                            m_DefaultStreamSettings : m_StreamProfiles.value(m_CurrentProfile));
    emit profileNamesChanged();
    emit currentProfileChanged();
}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::save()
{
    setUnfocusedPauseDelaySeconds(unfocusedPauseDelaySeconds);
    setUnfocusedAudioMuteDelaySeconds(unfocusedAudioMuteDelaySeconds);
    setHiddenAudioMuteDelaySeconds(hiddenAudioMuteDelaySeconds);
    rememberCurrentStreamSettings();
    QSettings settings;

    for (auto it = m_DefaultStreamSettings.cbegin(); it != m_DefaultStreamSettings.cend(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_RICHPRESENCE, richPresence);
    settings.setValue(SER_DETECTNETBLOCKING, detectNetworkBlocking);
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_PAUSEHIDDEN, pauseVideoWhenHidden);
    settings.setValue(SER_PAUSEUNFOCUSED, pauseVideoWhenUnfocused);
    settings.setValue(SER_UNFOCUSEDPAUSEDELAY, unfocusedPauseDelaySeconds);
    settings.setValue(SER_MUTEAUDIOHIDDEN, muteAudioWhenHidden);
    settings.setValue(SER_UNFOCUSEDAUDIOMUTEDELAY, unfocusedAudioMuteDelaySeconds);
    settings.setValue(SER_HIDDENAUDIOMUTEDELAY, hiddenAudioMuteDelaySeconds);
    saveProfiles(settings);
}

int StreamingPreferences::getUnfocusedPauseDelaySeconds() const
{
    return unfocusedPauseDelaySeconds;
}

void StreamingPreferences::setUnfocusedPauseDelaySeconds(int seconds)
{
    const int clampedSeconds = qBound(0, seconds, 3600);
    if (unfocusedPauseDelaySeconds != clampedSeconds) {
        unfocusedPauseDelaySeconds = clampedSeconds;
        emit unfocusedPauseDelaySecondsChanged();
    }
}

int StreamingPreferences::getUnfocusedAudioMuteDelaySeconds() const
{
    return unfocusedAudioMuteDelaySeconds;
}

void StreamingPreferences::setUnfocusedAudioMuteDelaySeconds(int seconds)
{
    const int clampedSeconds = qBound(0, seconds, 3600);
    if (unfocusedAudioMuteDelaySeconds != clampedSeconds) {
        unfocusedAudioMuteDelaySeconds = clampedSeconds;
        emit unfocusedAudioMuteDelaySecondsChanged();
    }
}

int StreamingPreferences::getHiddenAudioMuteDelaySeconds() const
{
    return hiddenAudioMuteDelaySeconds;
}

void StreamingPreferences::setHiddenAudioMuteDelaySeconds(int seconds)
{
    const int clampedSeconds = qBound(0, seconds, 3600);
    if (hiddenAudioMuteDelaySeconds != clampedSeconds) {
        hiddenAudioMuteDelaySeconds = clampedSeconds;
        emit hiddenAudioMuteDelaySecondsChanged();
    }
}

QVariantMap StreamingPreferences::captureStreamSettings() const
{
    QVariantMap values;
#define CAPTURE_VALUE(key, member, type, signal) values.insert(key, static_cast<type>(member));
    STREAM_PROFILE_FIELDS(CAPTURE_VALUE)
#undef CAPTURE_VALUE
    values.insert(SER_PACKETSIZE, packetSize);
    return values;
}

void StreamingPreferences::applyStreamSettings(const QVariantMap& values)
{
    // Assign the whole snapshot before notifying QML: dependent bindings (such
    // as automatic bitrate) must always see one coherent resolution/FPS pair.
#define APPLY_VALUE(key, member, type, signal) \
    const auto member##Value = static_cast<decltype(member)>( \
        values.value(key, static_cast<type>(member)).value<type>()); \
    const bool member##WasChanged = member != member##Value; \
    member = member##Value;
    STREAM_PROFILE_FIELDS(APPLY_VALUE)
#undef APPLY_VALUE
    if (values.contains(SER_PACKETSIZE)) {
        packetSize = values.value(SER_PACKETSIZE).toInt();
    }
#define NOTIFY_VALUE(key, member, type, signal) if (member##WasChanged) { emit signal(); }
    STREAM_PROFILE_FIELDS(NOTIFY_VALUE)
#undef NOTIFY_VALUE
}

void StreamingPreferences::rememberCurrentStreamSettings()
{
    if (m_CurrentProfile == QStringLiteral("Default")) {
        m_DefaultStreamSettings = captureStreamSettings();
    }
    else if (m_StreamProfiles.contains(m_CurrentProfile)) {
        m_StreamProfiles[m_CurrentProfile] = captureStreamSettings();
    }
}

QStringList StreamingPreferences::profileNames() const
{
    QStringList names = m_StreamProfiles.keys();
    names.prepend(QStringLiteral("Default"));
    return names;
}

QString StreamingPreferences::currentProfile() const
{
    return m_CurrentProfile;
}

void StreamingPreferences::setCurrentProfile(const QString& name)
{
    if (name == m_CurrentProfile || (name != QStringLiteral("Default") && !m_StreamProfiles.contains(name))) {
        return;
    }
    save();
    m_CurrentProfile = name;
    if (!m_ProfileHost.isEmpty()) {
        m_HostProfiles[m_ProfileHost] = name;
    }
    applyStreamSettings(name == QStringLiteral("Default") ?
                            m_DefaultStreamSettings : m_StreamProfiles.value(name));
    save();
    emit currentProfileChanged();
}

void StreamingPreferences::setProfileHost(const QString& hostUuid)
{
    if (m_ProfileHost == hostUuid) {
        return;
    }
    save();
    m_ProfileHost = hostUuid;
    m_CurrentProfile = m_HostProfiles.value(hostUuid, QStringLiteral("Default"));
    if (!m_StreamProfiles.contains(m_CurrentProfile)) {
        m_CurrentProfile = QStringLiteral("Default");
    }
    applyStreamSettings(m_CurrentProfile == QStringLiteral("Default") ?
                            m_DefaultStreamSettings : m_StreamProfiles.value(m_CurrentProfile));
    emit currentProfileChanged();
}

bool StreamingPreferences::isValidProfileName(const QString& name) const
{
    return !name.isEmpty() && name.size() <= 80 && name == name.trimmed() &&
           name.compare(QStringLiteral("Default"), Qt::CaseInsensitive) != 0;
}

bool StreamingPreferences::createProfile(const QString& name)
{
    return duplicateProfile(m_CurrentProfile, name);
}

bool StreamingPreferences::duplicateProfile(const QString& sourceName, const QString& newName)
{
    if (!isValidProfileName(newName) || m_StreamProfiles.contains(newName) ||
        (sourceName != QStringLiteral("Default") && !m_StreamProfiles.contains(sourceName))) {
        return false;
    }
    rememberCurrentStreamSettings();
    m_StreamProfiles.insert(newName, sourceName == QStringLiteral("Default") ?
                                      m_DefaultStreamSettings : m_StreamProfiles.value(sourceName));
    emit profileNamesChanged();
    setCurrentProfile(newName);
    return true;
}

bool StreamingPreferences::renameProfile(const QString& oldName, const QString& newName)
{
    if (!m_StreamProfiles.contains(oldName) || !isValidProfileName(newName) || m_StreamProfiles.contains(newName)) {
        return false;
    }
    rememberCurrentStreamSettings();
    m_StreamProfiles.insert(newName, m_StreamProfiles.take(oldName));
    for (auto it = m_HostProfiles.begin(); it != m_HostProfiles.end(); ++it) {
        if (it.value() == oldName) {
            it.value() = newName;
        }
    }
    if (m_CurrentProfile == oldName) {
        m_CurrentProfile = newName;
        emit currentProfileChanged();
    }
    save();
    emit profileNamesChanged();
    return true;
}

bool StreamingPreferences::deleteProfile(const QString& name)
{
    if (!m_StreamProfiles.contains(name)) {
        return false;
    }
    if (m_CurrentProfile == name) {
        setCurrentProfile(QStringLiteral("Default"));
    }
    m_StreamProfiles.remove(name);
    for (auto it = m_HostProfiles.begin(); it != m_HostProfiles.end(); ++it) {
        if (it.value() == name) {
            it.value() = QStringLiteral("Default");
        }
    }
    save();
    emit profileNamesChanged();
    return true;
}

void StreamingPreferences::loadProfiles(QSettings& settings)
{
    m_StreamProfiles.clear();
    m_HostProfiles.clear();
    const int profileCount = settings.beginReadArray("streamprofiles");
    for (int i = 0; i < profileCount; i++) {
        settings.setArrayIndex(i);
        const QString name = settings.value("name").toString();
        if (isValidProfileName(name)) {
            // Merging with the fallback handles fields added by future versions.
            QVariantMap values = m_DefaultStreamSettings;
            const QVariantMap savedValues = settings.value("settings").toMap();
            for (auto it = savedValues.cbegin(); it != savedValues.cend(); ++it) {
                if (values.contains(it.key())) {
                    values[it.key()] = it.value();
                }
            }
            m_StreamProfiles.insert(name, values);
        }
    }
    settings.endArray();
    const int hostCount = settings.beginReadArray("hoststreamprofiles");
    for (int i = 0; i < hostCount; i++) {
        settings.setArrayIndex(i);
        const QString hostUuid = settings.value("uuid").toString();
        const QString name = settings.value("profile").toString();
        if (!hostUuid.isEmpty() && (name == QStringLiteral("Default") || m_StreamProfiles.contains(name))) {
            m_HostProfiles.insert(hostUuid, name);
        }
    }
    settings.endArray();
}

void StreamingPreferences::saveProfiles(QSettings& settings) const
{
    // Arrays avoid treating slashes in user names or host identifiers as groups.
    settings.remove("streamprofiles");
    settings.beginWriteArray("streamprofiles", m_StreamProfiles.size());
    int index = 0;
    for (auto it = m_StreamProfiles.cbegin(); it != m_StreamProfiles.cend(); ++it) {
        settings.setArrayIndex(index++);
        settings.setValue("name", it.key());
        settings.setValue("settings", it.value());
    }
    settings.endArray();
    settings.remove("hoststreamprofiles");
    settings.beginWriteArray("hoststreamprofiles", m_HostProfiles.size());
    index = 0;
    for (auto it = m_HostProfiles.cbegin(); it != m_HostProfiles.cend(); ++it) {
        settings.setArrayIndex(index++);
        settings.setValue("uuid", it.key());
        settings.setValue("profile", it.value());
    }
    settings.endArray();
}

StreamingPreferences* StreamingPreferences::createSessionSnapshot() const
{
    auto* snapshot = new StreamingPreferences(nullptr, false);
#define COPY_VALUE(key, member, type, signal) snapshot->member = member;
    STREAM_PROFILE_FIELDS(COPY_VALUE)
#undef COPY_VALUE
    snapshot->packetSize = packetSize;
    snapshot->recommendedFullScreenMode = recommendedFullScreenMode;
    snapshot->enableMdns = enableMdns;
    snapshot->connectionWarnings = connectionWarnings;
    snapshot->configurationWarnings = configurationWarnings;
    snapshot->richPresence = richPresence;
    snapshot->detectNetworkBlocking = detectNetworkBlocking;
    snapshot->uiDisplayMode = uiDisplayMode;
    snapshot->language = language;
    snapshot->pauseVideoWhenHidden = pauseVideoWhenHidden;
    snapshot->pauseVideoWhenUnfocused = pauseVideoWhenUnfocused;
    snapshot->unfocusedPauseDelaySeconds = qBound(0, unfocusedPauseDelaySeconds, 3600);
    snapshot->muteAudioWhenHidden = muteAudioWhenHidden;
    snapshot->unfocusedAudioMuteDelaySeconds = qBound(0, unfocusedAudioMuteDelaySeconds, 3600);
    snapshot->hiddenAudioMuteDelaySeconds = qBound(0, hiddenAudioMuteDelaySeconds, 3600);
    return snapshot;
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444)
{
    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}
