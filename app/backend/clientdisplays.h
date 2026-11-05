#pragma once

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStringList>
#include <QVariantList>
#include <QWindow>
#include <algorithm>
#include <tuple>

// Client-local output policy. Never sent to the Host as a monitor identifier.
// Connector names survive enumeration changes and distinguish identical panels.
namespace ClientDisplays {
struct Output {
    QString name;
    QString model;
    QRect bounds;
    QSize pixels;
    bool primary = false;
};

inline bool validChoice(const QString& choice)
{
    if (choice == QStringLiteral("current") || choice == QStringLiteral("span")) return true;
    if (!choice.startsWith(QStringLiteral("screen:")) || choice.size() <= 7 || choice.size() > 256) return false;
    for (const QChar ch : choice) if (ch.isNull() || ch.category() == QChar::Other_Control) return false;
    return true;
}

inline QString load(QSettings& settings)
{
    // Preserve the previous all-monitor behavior for existing bookmarks.
    const auto value = settings.value(QStringLiteral("plank-client-displays"), QStringLiteral("span")).toString();
    return validChoice(value) ? value : QStringLiteral("current");
}

inline void save(QSettings& settings, const QString& choice)
{
    settings.setValue(QStringLiteral("plank-client-displays"), choice);
}

inline QVector<Output> snapshot()
{
    QVector<Output> outputs;
    for (auto* screen : QGuiApplication::screens()) {
        const auto bounds = screen->geometry();
        const auto ratio = screen->devicePixelRatio();
        outputs.append({screen->name(), screen->model(), bounds,
                        QSize(qRound(bounds.width() * ratio), qRound(bounds.height() * ratio)),
                        screen == QGuiApplication::primaryScreen()});
    }
    std::sort(outputs.begin(), outputs.end(), [](const auto& a, const auto& b) {
        return std::make_tuple(a.bounds.x(), a.bounds.y(), a.name) <
               std::make_tuple(b.bounds.x(), b.bounds.y(), b.name);
    });
    return outputs;
}

inline bool canSpan(const QVector<Output>& outputs)
{
    if (outputs.size() != 2) return false;
    const auto& a = outputs[0].bounds;
    const auto& b = outputs[1].bounds;
    return a.isValid() && b.isValid() && (a.x() + a.width() <= b.x() || b.x() + b.width() <= a.x()) &&
           a.y() < b.y() + b.height() && b.y() < a.y() + a.height();
}

struct Selection {
    QStringList names;
    QString target;
    QString notice;
};

inline Selection resolve(const QString& choice, const QVector<Output>& outputs,
                         const QString& current)
{
    Selection result;
    if (outputs.isEmpty()) return result;
    auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& o) { return o.name == current; });
    if (found == outputs.end()) found = std::find_if(outputs.begin(), outputs.end(), [](const auto& o) { return o.primary; });
    if (found == outputs.end()) found = outputs.begin();
    result.target = found->name;
    if (choice == QStringLiteral("span") && canSpan(outputs)) {
        for (const auto& output : outputs) result.names.append(output.name);
        return result;
    }
    if (choice.startsWith(QStringLiteral("screen:"))) {
        const QString name = choice.mid(7);
        const auto selected = std::find_if(outputs.begin(), outputs.end(), [&](const auto& o) { return o.name == name; });
        if (selected != outputs.end()) result.target = name;
        else result.notice = QCoreApplication::translate("ClientDisplays", "The saved Client display is unavailable. Using the current monitor.");
    } else if (choice == QStringLiteral("span")) {
        result.notice = QCoreApplication::translate("ClientDisplays", "Spanning requires two monitors arranged side by side. Using the current monitor.");
    }
    result.names.append(result.target);
    return result;
}

inline QString currentScreenName()
{
    auto* window = QGuiApplication::focusWindow();
    auto* screen = window ? window->screen() : QGuiApplication::primaryScreen();
    return screen ? screen->name() : QString();
}

inline QVariantList choices(const QString& saved = QString())
{
    const auto outputs = snapshot();
    QVariantList result;
    const auto add = [&](QString value, QString text, bool enabled = true) {
        result.append(QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("text"), text},
                                  {QStringLiteral("enabled"), enabled}});
    };
    add(QStringLiteral("current"), QCoreApplication::translate("ClientDisplays", "Current monitor"));
    bool found = saved == QStringLiteral("current") || saved == QStringLiteral("span") || saved.isEmpty();
    for (int index = 0; index < outputs.size(); ++index) {
        const auto& output = outputs[index];
        const auto value = QStringLiteral("screen:") + output.name;
        QString label = output.model.isEmpty() ? output.name : output.model + QStringLiteral(" — ") + output.name;
        if (canSpan(outputs)) label += index == 0 ? QCoreApplication::translate("ClientDisplays", " (left)") :
                                                  QCoreApplication::translate("ClientDisplays", " (right)");
        label += QStringLiteral(" — %1 × %2").arg(output.pixels.width()).arg(output.pixels.height());
        add(value, label);
        found |= saved == value;
    }
    add(QStringLiteral("span"), QCoreApplication::translate("ClientDisplays", "Span both monitors"), canSpan(outputs));
    if (!found) add(saved, QCoreApplication::translate("ClientDisplays", "Unavailable display — %1").arg(saved.mid(7)));
    return result;
}
}
