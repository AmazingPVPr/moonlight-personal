#pragma once

#include <QCoreApplication>
#include <QSettings>

namespace SettingsMigration {

// Import the previous fork's preferences only on the new identity's first run.
// Typed values, including the pairing credentials, are copied unchanged.
inline bool importLegacyPreferencesIfEmpty(const QString& legacyApplication)
{
    QSettings current;
    if (!current.allKeys().isEmpty()) {
        return false;
    }
    QSettings legacy(QSettings::defaultFormat(), QSettings::UserScope,
                     QCoreApplication::organizationName(), legacyApplication);
    const auto keys = legacy.allKeys();
    if (keys.isEmpty()) {
        return false;
    }
    for (const auto& key : keys) {
        current.setValue(key, legacy.value(key));
    }
    current.sync();
    return current.status() == QSettings::NoError;
}

}
