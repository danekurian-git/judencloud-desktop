/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudissuesdialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;

class TestPrivateCloudIssuesDialog : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testSelectionRevealsOnlyTheSelectedFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PrivateCloudIssuesDialog dialog(directory.path(), {QStringLiteral("Documents/first.txt"), QStringLiteral("Documents/second.txt")});
        QVERIFY(!dialog.isModal());
        auto *files = dialog.findChild<QListWidget *>(QStringLiteral("privateCloudIssuesFiles"));
        auto *reveal = dialog.findChild<QPushButton *>(QStringLiteral("privateCloudShowIssue"));
        QVERIFY(files && reveal);
        QCOMPARE(files->count(), 2);
        QVERIFY(!reveal->isEnabled());
        QSignalSpy requested(&dialog, &PrivateCloudIssuesDialog::revealRequested);
        reveal->click();
        QCOMPARE(requested.count(), 0);
        files->setCurrentRow(1);
        QVERIFY(reveal->isEnabled());
        reveal->click();
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toString(), directory.filePath(QStringLiteral("Documents/second.txt")));
        files->setCurrentRow(-1);
        QVERIFY(!reveal->isEnabled());
        reveal->click();
        QCOMPARE(requested.count(), 1);
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        buttons->button(QDialogButtonBox::Close)->click();
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }

    void testUnsafePaths_data()
    {
        QTest::addColumn<QString>("relative");
        for (const auto &path : {QString(),
                                 QStringLiteral("../outside.txt"),
                                 QStringLiteral("nested/../../outside.txt"),
                                 QStringLiteral("..\\outside.txt"),
                                 QStringLiteral("/outside.txt"),
                                 QStringLiteral("\\outside.txt"),
                                 QStringLiteral("C:/outside.txt"),
                                 QStringLiteral("C:outside.txt"),
                                 QStringLiteral("//server/share/file"),
                                 QStringLiteral("\\\\server\\share\\file"),
                                 QStringLiteral("file:stream"),
                                 QStringLiteral(".. /outside.txt"),
                                 QStringLiteral("folder./file"),
                                 QStringLiteral(".")}) {
            QTest::newRow(qPrintable(path.isEmpty() ? QStringLiteral("empty") : path)) << path;
        }
        auto embeddedNull = QStringLiteral("axb");
        embeddedNull[1] = QChar::Null;
        QTest::newRow("embedded-null") << embeddedNull;
    }

    void testUnsafePaths()
    {
        QFETCH(QString, relative);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PrivateCloudIssuesDialog dialog(directory.path(), {relative});
        auto *files = dialog.findChild<QListWidget *>(QStringLiteral("privateCloudIssuesFiles"));
        auto *reveal = dialog.findChild<QPushButton *>(QStringLiteral("privateCloudShowIssue"));
        QCOMPARE(files->count(), 0);
        QVERIFY(!reveal->isEnabled());
        QSignalSpy requested(&dialog, &PrivateCloudIssuesDialog::revealRequested);
        reveal->click();
        QCOMPARE(requested.count(), 0);
    }

    void testInvalidRootAndEmptyListHaveNoAction()
    {
        PrivateCloudIssuesDialog relativeRoot(QStringLiteral("relative-root"), {QStringLiteral("file.txt")});
        QCOMPARE(relativeRoot.findChild<QListWidget *>()->count(), 0);
        QVERIFY(!relativeRoot.findChild<QPushButton *>(QStringLiteral("privateCloudShowIssue"))->isEnabled());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PrivateCloudIssuesDialog noFiles(directory.path(), {});
        QCOMPARE(noFiles.findChild<QListWidget *>()->count(), 0);
        QVERIFY(!noFiles.findChild<QPushButton *>(QStringLiteral("privateCloudShowIssue"))->isEnabled());
    }

    void testPlainTextAndDuplicatePaths()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PrivateCloudIssuesDialog dialog(
            directory.path(),
            {QStringLiteral("Reports/<b>literal</b>.txt"), QStringLiteral("Documents/file.txt"), QStringLiteral("Documents\\file.txt")});
        auto *files = dialog.findChild<QListWidget *>();
        QCOMPARE(files->count(), 2);
        QCOMPARE(files->item(0)->text(), QStringLiteral("Reports/<b>literal</b>.txt"));
        files->setCurrentRow(0);
        QSignalSpy requested(&dialog, &PrivateCloudIssuesDialog::revealRequested);
        QVERIFY(QMetaObject::invokeMethod(files, "itemActivated", Qt::DirectConnection, Q_ARG(QListWidgetItem *, files->item(0))));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toString(), directory.filePath(QStringLiteral("Reports/<b>literal</b>.txt")));
    }
};

QTEST_MAIN(TestPrivateCloudIssuesDialog)
#include "testprivatecloudissuesdialog.moc"
