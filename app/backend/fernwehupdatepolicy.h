#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace FernwehUpdatePolicy {
inline const QString WindowsProduct = QStringLiteral("BDE fernweh");
inline const QString WindowsUpgradeCode = QStringLiteral("{F598C7E9-ECA2-432F-B0C0-41F9E1201F8F}");
inline constexpr qint64 MaximumSize = 2LL * 1024 * 1024 * 1024;

struct Release {
    QString version, url;
    QByteArray sha256;
    qint64 size = 0;
};

inline QString feedPath(bool windows)
{
    return windows ? QStringLiteral("/v1/client-updates/windows-x64") :
                     QStringLiteral("/v1/client-updates/macos-arm64");
}

inline bool downloadPathAllowed(const QString& path)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^/v1/client-updates/(?:macos-arm64/[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}\\.dmg|windows-x64/[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}\\.msi)\\z"));
    return pattern.match(path).hasMatch();
}

inline bool parse(const QByteArray& body, bool windows, Release& release)
{
    static const QRegularExpression versionPattern(QStringLiteral("^[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}\\z"));
    static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}\\z"));
    const auto doc = QJsonDocument::fromJson(body);
    const auto object = doc.object();
    Release parsed;
    parsed.version = object.value(QStringLiteral("version")).toString();
    parsed.url = object.value(QStringLiteral("url")).toString();
    parsed.sha256 = object.value(QStringLiteral("sha256")).toString().toLatin1();
    const auto size = object.value(QStringLiteral("size"));
    // Range-check before conversion: NaN, fractional and oversized JSON numbers
    // must never reach an integer cast or the downloader.
    if (!doc.isObject() || !versionPattern.match(parsed.version).hasMatch() ||
            !shaPattern.match(QString::fromLatin1(parsed.sha256)).hasMatch() ||
            !size.isDouble() || !(size.toDouble() > 0 && size.toDouble() <= MaximumSize)) return false;
    parsed.size = static_cast<qint64>(size.toDouble());
    if (size.toDouble() != parsed.size || parsed.url != feedPath(windows) + QLatin1Char('/') +
            parsed.version + (windows ? QStringLiteral(".msi") : QStringLiteral(".dmg"))) return false;
    if (windows) {
        const auto version = QVersionNumber::fromString(parsed.version);
        if (version.majorVersion() > 255 || version.minorVersion() > 255 || version.microVersion() > 65535 ||
                object.value(QStringLiteral("platform")) != QLatin1String("windows-x64") ||
                object.value(QStringLiteral("product_name")) != WindowsProduct ||
                object.value(QStringLiteral("upgrade_code")) != WindowsUpgradeCode) return false;
    }
    release = parsed;
    return true;
}

inline bool newer(const QString& candidate, const QString& installed)
{
    return QVersionNumber::compare(QVersionNumber::fromString(candidate),
                                   QVersionNumber::fromString(installed)) > 0;
}
}
