/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gui/accountmanager.h"
#include "gui/creds/webflowcredentials.h"
#include "gui/creds/privatecloudcredentialsdialog.h"
#include "privateclouddrive.h"
#include "gui/systray.h"
#include "gui/tray/usermodel.h"
#include "gui/wizard/browserreauthcontroller.h"
#include "syncenginetestutils.h"
#include "theme.h"

#include <QApplication>
#include <QLineEdit>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace OCC;
using namespace Qt::StringLiterals;

class BrowserOpeningHandler : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    void openUrl(const QUrl &url)
    {
        Q_EMIT urlOpened(url);
    }

Q_SIGNALS:
    void urlOpened(const QUrl &url);
};

namespace
{

class TestWebFlowCredentials final : public WebFlowCredentials
{
public:
    explicit TestWebFlowCredentials(QNetworkAccessManager *networkAccessManager)
        : WebFlowCredentials(QStringLiteral("old-user"), QStringLiteral("old-password"))
        , _networkAccessManager(networkAccessManager)
    {
    }

    [[nodiscard]] QNetworkAccessManager *createQNAM() const override
    {
        return _networkAccessManager;
    }

    void persist() override
    {
        ++_persistCount;
        _persistedUser = user();
        _persistedPassword = password();
    }

    [[nodiscard]] int persistCount() const
    {
        return _persistCount;
    }

    [[nodiscard]] QString persistedUser() const
    {
        return _persistedUser;
    }

    [[nodiscard]] QString persistedPassword() const
    {
        return _persistedPassword;
    }

private:
    QNetworkAccessManager *_networkAccessManager = nullptr;
    int _persistCount = 0;
    QString _persistedUser;
    QString _persistedPassword;
};

QQuickWindow *findBrowserReAuthWindow()
{
    const auto windows = QGuiApplication::topLevelWindows();
    const auto window = std::find_if(windows.cbegin(), windows.cend(), [](QWindow *window) {
        return window->property("controller").value<QObject *>() != nullptr;
    });
    return window == windows.cend() ? nullptr : qobject_cast<QQuickWindow *>(*window);
}

void configureSuccessfulLoginFlow(FakeQNAM *networkAccessManager)
{
    const auto serverUrl = Account::enforcedServerUrl().isEmpty() ? QUrl(u"http://cloud.example"_s) : Account::enforcedServerUrl();
    networkAccessManager->setOverride(
        [networkAccessManager, serverUrl](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            if (request.url().path() == QStringLiteral("/index.php/login/v2")) {
                const auto response = QJsonObject{
                    {QStringLiteral("poll"),
                     QJsonObject{
                         {QStringLiteral("token"), QStringLiteral("poll-token")},
                         {QStringLiteral("endpoint"), serverUrl.resolved(QUrl(u"/login/v2/poll"_s)).toString()},
                     }},
                    {QStringLiteral("login"), serverUrl.resolved(QUrl(u"/login/v2/flow"_s)).toString()},
                };
                return new FakeJsonReply(operation, request, networkAccessManager, 200, QJsonDocument(response));
            }

            if (request.url().path() == QStringLiteral("/login/v2/poll")) {
                const auto response = QJsonObject{
                    {QStringLiteral("server"), serverUrl.toString()},
                    {QStringLiteral("loginName"), QStringLiteral("alice")},
                    {QStringLiteral("appPassword"), QStringLiteral("app-password")},
                };
                return new FakeJsonReply(operation, request, networkAccessManager, 200, QJsonDocument(response));
            }

            return nullptr;
        });
}

} // namespace

class TestBrowserReAuthController : public QObject
{
    Q_OBJECT

    std::unique_ptr<FolderMan> _fm;

    static PrivateCloudCredentialsDialog *findPrivateCloudDialog()
    {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<PrivateCloudCredentialsDialog *>(widget)) {
                return dialog;
            }
        }
        return nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        OCC::Logger::instance()->setLogFlush(true);
        OCC::Logger::instance()->setLogDebug(true);

        QStandardPaths::setTestModeEnabled(true);

        Q_INIT_RESOURCE(resources);
        Q_INIT_RESOURCE(theme);
        qmlRegisterSingletonInstance("com.nextcloud.desktopclient", 1, 0, "UserModel", UserModel::instance());
        qmlRegisterSingletonInstance("com.nextcloud.desktopclient", 1, 0, "Theme", Theme::instance());

        _fm.reset(new FolderMan{});

        Systray::instance()->createTrayEngine();
    }

    void cleanup()
    {
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        QDesktopServices::unsetUrlHandler(u"https"_s);

        if (auto *window = findBrowserReAuthWindow()) {
            window->close();
        }
        if (auto *dialog = findPrivateCloudDialog()) {
            dialog->reject();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void sharedWizardWindowDoesNotEnableMinimizationByDefault()
    {
        QQmlComponent component(Systray::instance()->trayEngine(), QUrl(QStringLiteral("qrc:/qml/src/gui/WizardStyledWindow.qml")));
        const auto window = std::unique_ptr<QObject>(component.create());

        QVERIFY2(window, qPrintable(component.errorString()));
        const auto quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QVERIFY(!quickWindow->flags().testFlag(Qt::WindowMinimizeButtonHint));
    }

    void replacesAndPersistsCredentialsThroughActiveWindow()
    {
        BrowserOpeningHandler browserOpeningHandler;
        QDesktopServices::setUrlHandler(QStringLiteral("http"), &browserOpeningHandler, "openUrl");
        QDesktopServices::setUrlHandler(u"https"_s, &browserOpeningHandler, "openUrl");
        QSignalSpy browserSpy(&browserOpeningHandler, &BrowserOpeningHandler::urlOpened);

        const auto account = AccountManager::createAccount();
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(u"http://cloud.example"_s) : Account::enforcedServerUrl()));
        auto *const networkAccessManager = new FakeQNAM({});
        configureSuccessfulLoginFlow(networkAccessManager);
        auto *const credentials = new TestWebFlowCredentials(networkAccessManager);
        account->setCredentials(credentials);

        const auto registeredAccountCount = AccountManager::instance()->accounts().size();
        QSignalSpy askedSpy(credentials, &AbstractCredentials::asked);

        credentials->askFromUser();

        if (PrivateCloudDrive::isEnabled()) {
            auto *dialog = findPrivateCloudDialog();
            QVERIFY(dialog);
            auto *username = dialog->findChild<QLineEdit *>(u"privateCloudUsername"_s);
            auto *password = dialog->findChild<QLineEdit *>(u"privateCloudPassword"_s);
            QVERIFY(username);
            QVERIFY(password);
            QCOMPARE(username->text(), u"old-user"_s);
            QVERIFY(password->text().isEmpty());
            QPointer<PrivateCloudCredentialsDialog> guard(dialog);
            // Authentication request/response checks live in PrivateCloudAuthenticationTest.
            // This exercises the real dialog-to-credential persistence connection.
            Q_EMIT dialog->credentialsReady(u"alice"_s, u"app-password"_s);
            dialog->accept();
            QCOMPARE(askedSpy.count(), 1);
            QCOMPARE(credentials->user(), u"alice"_s);
            QCOMPARE(credentials->password(), u"app-password"_s);
            QCOMPARE(credentials->persistCount(), 1);
            QCOMPARE(credentials->persistedUser(), u"alice"_s);
            QCOMPARE(credentials->persistedPassword(), u"app-password"_s);
            QCOMPARE(AccountManager::instance()->accounts().size(), registeredAccountCount);
            QCOMPARE(browserSpy.count(), 0);
            QTRY_VERIFY(guard.isNull());
            return;
        }

        QQuickWindow *window = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((window = findBrowserReAuthWindow()) != nullptr, 5000);
        QVERIFY(window->flags().testFlag(Qt::WindowMinimizeButtonHint));
        QPointer<QQuickWindow> windowGuard(window);
        QTRY_COMPARE_WITH_TIMEOUT(browserSpy.count(), 1, 5000);

        auto *const controller = window->property("controller").value<QObject *>();
        QVERIFY(controller);
        QVERIFY(QMetaObject::invokeMethod(controller, "pollNow"));

        QTRY_COMPARE_WITH_TIMEOUT(askedSpy.count(), 1, 5000);
        QCOMPARE(credentials->user(), QStringLiteral("alice"));
        QCOMPARE(credentials->password(), QStringLiteral("app-password"));
        QCOMPARE(credentials->persistCount(), 1);
        QCOMPARE(credentials->persistedUser(), QStringLiteral("alice"));
        QCOMPARE(credentials->persistedPassword(), QStringLiteral("app-password"));
        QCOMPARE(AccountManager::instance()->accounts().size(), registeredAccountCount);
        QTRY_VERIFY_WITH_TIMEOUT(windowGuard.isNull(), 5000);
    }

    void closingActiveWindowCancelsWithoutReplacingCredentials()
    {
        const auto account = AccountManager::createAccount();
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(u"http://cloud.example"_s) : Account::enforcedServerUrl()));
        auto *const networkAccessManager = new FakeQNAM({});
        networkAccessManager->setOverride(
            [networkAccessManager](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
                return new FakeHangingReply(operation, request, networkAccessManager);
            });
        auto *const credentials = new TestWebFlowCredentials(networkAccessManager);
        account->setCredentials(credentials);
        QSignalSpy askedSpy(credentials, &AbstractCredentials::asked);

        credentials->askFromUser();

        if (PrivateCloudDrive::isEnabled()) {
            auto *dialog = findPrivateCloudDialog();
            QVERIFY(dialog);
            QPointer<PrivateCloudCredentialsDialog> guard(dialog);
            dialog->reject();
            QCOMPARE(askedSpy.count(), 1);
            QCOMPARE(credentials->user(), u"old-user"_s);
            QCOMPARE(credentials->password(), u"old-password"_s);
            QCOMPARE(credentials->persistCount(), 0);
            QTRY_VERIFY(guard.isNull());
            return;
        }

        QQuickWindow *window = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((window = findBrowserReAuthWindow()) != nullptr, 5000);
        QPointer<QQuickWindow> windowGuard(window);

        QVERIFY(window->close());

        QTRY_COMPARE_WITH_TIMEOUT(askedSpy.count(), 1, 5000);
        QCOMPARE(credentials->user(), QStringLiteral("old-user"));
        QCOMPARE(credentials->password(), QStringLiteral("old-password"));
        QCOMPARE(credentials->persistCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(windowGuard.isNull(), 5000);
    }

    void exposesAuthenticationErrorsToQml()
    {
        const auto account = AccountManager::createAccount();
        BrowserReAuthController controller(account.data());
        QSignalSpy errorSpy(&controller, &BrowserReAuthController::errorTextChanged);

        controller.slotAuthResult(Flow2Auth::Error, QStringLiteral("Authentication failed."), {}, {});

        QVERIFY(!controller.finished());
        QCOMPARE(controller.errorText(), QStringLiteral("Authentication failed."));
        QCOMPARE(errorSpy.count(), 1);
    }

    void keepsPollingWhenOpeningTheBrowserFails()
    {
        const auto account = AccountManager::createAccount();
        BrowserReAuthController controller(account.data());

        controller.slotStatusChanged(Flow2Auth::statusPollCountdown, 3);
        controller.slotAuthResult(Flow2Auth::NotSupported, {}, {}, {});

        QVERIFY(controller.authPolling());
        QVERIFY(!controller.busy());
        QVERIFY(!controller.finished());
        QVERIFY(!controller.errorText().isEmpty());
    }

    void cancellationFinishesWithoutCredentials()
    {
        const auto account = AccountManager::createAccount();
        BrowserReAuthController controller(account.data());
        QSignalSpy credentialsSpy(&controller, &BrowserReAuthController::credentialsReady);
        QSignalSpy cancelledSpy(&controller, &BrowserReAuthController::cancelled);

        controller.cancel();

        QVERIFY(controller.finished());
        QCOMPARE(credentialsSpy.count(), 0);
        QCOMPARE(cancelledSpy.count(), 1);
    }
};

QTEST_MAIN(TestBrowserReAuthController)
#include "testbrowserreauthcontroller.moc"
