#include <QtTest>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>

class ChangelogTest : public QObject
{
    Q_OBJECT
private slots:
    void bundledNotesAndInteraction()
    {
        QFile file(QStringLiteral(":/res/changelog.md"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString notes = QString::fromUtf8(file.readAll());
        QVERIFY(notes.contains("### Client"));
        QVERIFY(notes.contains("### Host"));
        QVERIFY(notes.contains("## 1.0.143"));

        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            import QtQuick.Controls.Material 2.15
            import QtQuick.Layouts 1.15
            import "qrc:/gui"
            ApplicationWindow {
                width: 900; height: 900; visible: true
                Material.theme: Material.Dark
                property int backgroundClicks: 0
                Item { id: stackView }
                MouseArea { anchors.fill: parent; onClicked: backgroundClicks++ }
                header: ToolBar {
                    height: 56
                    RowLayout {
                        anchors.fill: parent
                        PlankVersionButton { version: "1.0.145-client-changelog" }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        )", QUrl(QStringLiteral("qrc:/changelog-test.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *button = root->findChild<QQuickItem *>("plankVersionButton");
        auto *dialog = root->findChild<QObject *>("changelogDialog");
        auto *text = root->findChild<QQuickItem *>("changelogNotes");
        auto *scroll = root->findChild<QObject *>("changelogScroll");
        QVERIFY(button && dialog && text && scroll);
        button->setProperty("changelog", notes);
        QCOMPARE(button->property("text").toString(), QString("1.0.145-client-changelog"));
        QVERIFY(!dialog->property("visible").toBool());

        const auto click = [window](QQuickItem *item) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        click(button);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(dialog->property("modal").toBool());
        QVERIFY(!dialog->property("dim").toBool());
        QVERIFY(text->property("readOnly").toBool());
        auto *installed = root->findChild<QObject *>("changelogInstalledVersion");
        QVERIFY(installed->property("text").toString().contains("1.0.145-client-changelog"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(10, 850));
        QCOMPARE(root->property("backgroundClicks").toInt(), 0);
        QVERIFY(dialog->property("opened").toBool());

        // Narrow windows wrap notes; long histories scroll vertically.
        window->resize(480, 420);
        QTRY_VERIFY(dialog->property("width").toReal() <= 448);
        QTRY_VERIFY(dialog->property("height").toReal() <= 388);
        auto *flickable = qvariant_cast<QObject *>(scroll->property("contentItem"));
        QVERIFY(flickable);
        QTRY_VERIFY(flickable->property("contentHeight").toReal() > flickable->property("height").toReal());
        QTRY_VERIFY(text->property("contentWidth").toReal() <= text->width());
        flickable->setProperty("contentY", 100);
        QVERIFY(flickable->property("contentY").toReal() > 0);
        QTest::keyClick(window, Qt::Key_X);
        QCOMPARE(text->property("text").toString(), notes);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(flickable->property("contentY").toReal(), 0.0);

        // The footer Close button works independently of Escape.
        auto *footer = qvariant_cast<QObject *>(dialog->property("footer"));
        QVERIFY(footer);
        QQuickItem *closeButton = nullptr;
        for (auto *item : footer->findChildren<QQuickItem *>()) {
            if (item->property("text").toString() == "Close" &&
                    item->metaObject()->indexOfSignal("clicked()") >= 0) {
                closeButton = item;
                break;
            }
        }
        QVERIFY(closeButton);
        click(closeButton);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(button->hasActiveFocus());
        button->setProperty("changelog", QString());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(text->property("text").toString().contains("No release notes"));
        QCOMPARE(warnings.count(), 0);
    }
};

QTEST_MAIN(ChangelogTest)
#include "test_changelog.moc"
