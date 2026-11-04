/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "account.h"
#include "common/vfs.h"
#include "configfile.h"
#include "creds/privatecloudcredentialsdialog.h"
#include "creds/webflowcredentials.h"
#include "accountmanager.h"
#include "config.h"
#include "folder.h"
#include "gui/localnetworkpermission.h"
#include "gui/tray/svgimageprovider.h"
#include "gui/tray/usermodel.h"
#include "gui/wizard/accountwizardcontroller.h"
#include "privateclouddrive.h"
#include "syncenginetestutils.h"
#include "testhelper.h"
#include "managedsettingstestutils.h"
#include "theme.h"

#ifdef BUILD_FILE_PROVIDER_MODULE
#include "gui/macOS/fileprovider.h"
#endif

#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QDesktopServices>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace OCC;
using namespace Qt::StringLiterals;

namespace OCC
{

class AccountWizardControllerTestAccess
{
public:
    static void setLocalNetworkPermissionDenied(AccountWizardController &controller, bool denied)
    {
        controller._localNetworkPermissionCheck = [denied](const QUrl &, QObject *, std::function<void(bool)> callback) {
            callback(denied);
        };
    }

    static void setAccountUrl(AccountWizardController &controller, const QUrl &url)
    {
        controller._account = Account::create();
        controller._account->setUrl(url);
    }

    static void preparePrivateCloud(AccountWizardController &controller, std::function<QString(const QString &)> availabilityCheck)
    {
        controller._privateCloudMode = true;
        controller._syncMode = AccountWizardController::VirtualFiles;
        controller._privateCloudAvailabilityCheck = std::move(availabilityCheck);
        const auto url = Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl();
        setAccountUrl(controller, url);
        controller._account->setDavUser(QStringLiteral("alice"));
    }

    static void completeAuthentication(AccountWizardController &controller)
    {
        controller.completeAuthentication();
    }

    static QString remoteFolder(const AccountWizardController &controller)
    {
        return controller._remoteFolder;
    }

    static QString privateCloudIdentity(const AccountWizardController &controller)
    {
        return controller._account->url().toString(QUrl::FullyEncoded) + QLatin1Char('\n') + controller._account->davUser();
    }

    static bool resumePrivateCloudSetup(AccountWizardController &controller, Folder *folder)
    {
        return controller.resumePrivateCloudSetup(folder);
    }

    static void showPrivateCloudSignIn(AccountWizardController &controller)
    {
        controller.setCurrentStep(AccountWizardController::BasicAuthStep);
    }

    static void setBusy(AccountWizardController &controller, bool busy)
    {
        controller.setBusy(busy);
    }

    static void clearError(AccountWizardController &controller)
    {
        controller.setErrorText({});
    }

    static void setFolderDialog(AccountWizardController &controller, std::function<QString(const QString &)> picker)
    {
        controller._localSyncFolderPicker = [picker = std::move(picker)](const QString &, const QString &start, auto completion) {
            completion(picker(start), {});
        };
    }

    static void setLocalFolder(AccountWizardController &controller, const QString &path)
    {
        controller.setLocalSyncFolder(path);
    }

    static void setAdoptionProfiles(AccountWizardController &controller, const QStringList &profiles)
    {
        controller._privateCloudAdoptionProfilePaths = profiles;
    }

    static AccountPtr account(const AccountWizardController &controller)
    {
        return controller._account;
    }

    static void showDriveSetup(AccountWizardController &controller)
    {
        controller.setCurrentStep(AccountWizardController::DriveSetupStep);
    }

    static void completeRemoteFolderCheck(AccountWizardController &controller)
    {
        controller.completeRemoteFolderCheck();
    }

    static void setNetworkManager(AccountWizardController &controller, QNetworkAccessManager *manager)
    {
        controller._account->setCredentials(new FakeCredentials(manager));
    }

    static void setSyncMode(AccountWizardController &controller, AccountWizardController::SyncMode mode)
    {
        controller._syncMode = mode;
    }

    static void chooseSyncModeAfterCapabilities(AccountWizardController &controller)
    {
        controller.chooseSyncModeAfterCapabilities();
    }

    static void setLocalSyncFolderPicker(AccountWizardController &controller, AccountWizardController::LocalSyncFolderPicker picker)
    {
        // privateCloud exposes folder selection only after authentication.
        // Exercise the upstream asynchronous picker in that same setup state.
        if (controller.privateCloudMode()) {
            preparePrivateCloud(controller, [](const QString &) {
                return QString{};
            });
            completeAuthentication(controller);
        }
        controller._localSyncFolderPicker = std::move(picker);
    }

    static FolderDefinition syncFolderDefinition(const AccountWizardController &controller)
    {
        return controller.syncFolderDefinition();
    }

    static void openInitialLocalSyncFolderDialog(AccountWizardController &controller)
    {
        controller.openLocalSyncFolderDialog(true);
    }

    static void setLocalSyncFolderOverride(AccountWizardController &controller, bool localSyncFolderOverride)
    {
        controller._localSyncFolderOverride = localSyncFolderOverride;
    }

    static bool localSyncFolderOverride(const AccountWizardController &controller)
    {
        return controller._localSyncFolderOverride;
    }

    static void setCurrentStep(AccountWizardController &controller, AccountWizardController::Step step)
    {
        controller.setCurrentStep(step);
    }

    static void initialiseLocalSyncFolder(AccountWizardController &controller)
    {
        controller.initialiseLocalSyncFolder();
    }
};

}

class TestAccountWizardController : public QObject
{
    Q_OBJECT

    QTemporaryDir _configuration;
Q_SIGNALS:
    void signupUrlRequested(const QUrl &url);

private:
    static QUrl allowedServerUrl()
    {
        const auto enforced = Account::enforcedServerUrl();
        return enforced.isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : enforced;
    }

    [[nodiscard]] static auto restoreOverrideState()
    {
        ConfigFile().clearOverrideServerUrl();
        ConfigFile().clearOverrideLocalDir();
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto previousStartLoginFlowAutomatically = theme->startLoginFlowAutomatically();
        theme->setOverrideServerUrl({});
        // Builds enable APPLICATION_SERVER_URL_ENFORCE by default.
        theme->setForceOverrideServerUrl(true);
        theme->setStartLoginFlowAutomatically(false);
        return qScopeGuard([theme, previousOverrideServerUrl, previousForceOverrideServerUrl, previousStartLoginFlowAutomatically] {
            ConfigFile().clearOverrideServerUrl();
            ConfigFile().clearOverrideLocalDir();
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
            theme->setStartLoginFlowAutomatically(previousStartLoginFlowAutomatically);
        });
    }

    static void setManagedOverrideServerUrl(SettingSourceType type, EnforcementState enforcement, int priority)
    {
        ConfigFile::setDeviceSourcesFactory([type, enforcement, priority] {
            std::vector<std::unique_ptr<SettingSource>> sources;
            sources.push_back(std::make_unique<MapSource>(type, enforcement, priority, QVariantMap{{u"overrideServerUrl"_s, u"https://cloud.example"_s}}));
            return sources;
        });
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(_configuration.isValid());
        ConfigFile::setConfDir(_configuration.path());
        Q_INIT_RESOURCE(resources);
        Q_INIT_RESOURCE(theme);
        qmlRegisterSingletonType<UserModel>("com.nextcloud.desktopclient", 1, 0, "UserModel", [](QQmlEngine *, QJSEngine *) -> QObject * {
            auto *model = UserModel::instance();
            QQmlEngine::setObjectOwnership(model, QQmlEngine::CppOwnership);
            return model;
        });
        qmlRegisterSingletonType<Theme>("com.nextcloud.desktopclient", 1, 0, "Theme", [](QQmlEngine *, QJSEngine *) -> QObject * {
            auto *theme = Theme::instance();
            QQmlEngine::setObjectOwnership(theme, QQmlEngine::CppOwnership);
            return theme;
        });
        qmlRegisterType<AccountWizardController>("com.nextcloud.desktopclient", 1, 0, "AccountWizardController");
    }

    void privateCloudSignInShowsExplicitUsernameAndPassword()
    {
#ifdef Q_OS_WIN
        if (!qEnvironmentVariable("PRIVATECLOUD_SIGNIN_SCREENSHOT").isEmpty()) {
            QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("Fonts/segoeui.ttf")));
            QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
        }
#endif
        AccountWizardController controller;
        if (PrivateCloudDrive::isEnabled()) {
            QCOMPARE(controller.currentStep(), AccountWizardController::BasicAuthStep);
        }
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QStringLiteral("unused");
        });
        AccountWizardControllerTestAccess::showPrivateCloudSignIn(controller);
        QVERIFY(controller.basicAuthUser().isEmpty());
        QVERIFY(controller.basicAuthPassword().isEmpty());
        QVERIFY(!controller.basicAuthValid());
        QVERIFY(!controller.startLoginFlowAutomatically());
        QCOMPARE(controller.appName(), QStringLiteral("privateCloud"));
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral("qrc:/qml/theme"));
        engine.addImageProvider(QStringLiteral("svgimage-custom-color"), new Ui::SvgImageProvider);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/src/gui/wizard/qml/AccountWizardWindow.qml")));
        const auto object = std::unique_ptr<QObject>(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        const auto username = object->findChild<QObject *>(QStringLiteral("privateCloudUsername"));
        const auto password = object->findChild<QObject *>(QStringLiteral("privateCloudPassword"));
        QVERIFY(username);
        QVERIFY(password);
        QVERIFY(username->property("text").toString().isEmpty());
        QVERIFY(password->property("text").toString().isEmpty());
        controller.setBasicAuthUser(QStringLiteral("explicit-user"));
        QVERIFY(!controller.basicAuthValid());
        QCOMPARE(username->property("text").toString(), QStringLiteral("explicit-user"));
        const auto window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QCOMPARE(window->title(), QStringLiteral("privateCloud"));
        window->show();
        QTRY_VERIFY(window->isExposed());
        const auto usernameItem = qobject_cast<QQuickItem *>(username);
        const auto passwordItem = qobject_cast<QQuickItem *>(password);
        QVERIFY(usernameItem);
        QVERIFY(passwordItem);
        usernameItem->forceActiveFocus();
        QTRY_VERIFY(usernameItem->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(passwordItem->hasActiveFocus());
        QVERIFY(controller.errorText().isEmpty());
        QVERIFY(!controller.busy());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(controller.errorText(), QStringLiteral("Enter both your username and password."));
        QVERIFY(!controller.busy());

        const auto showPassword = object->findChild<QQuickItem *>(QStringLiteral("privateCloudShowPassword"));
        const auto helpToggle = object->findChild<QQuickItem *>(QStringLiteral("privateCloudPasswordHelpToggle"));
        const auto help = object->findChild<QQuickItem *>(QStringLiteral("privateCloudPasswordHelp"));
        QVERIFY(showPassword);
        QVERIFY(helpToggle);
        QVERIFY(help);
        const auto click = [window](QQuickItem *item) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        controller.setBasicAuthPassword(QStringLiteral("test-password"));
        QVERIFY(controller.basicAuthValid());
        QCOMPARE(password->property("echoMode").toInt(), static_cast<int>(QLineEdit::Password));
        click(showPassword);
        QTRY_COMPARE(password->property("echoMode").toInt(), static_cast<int>(QLineEdit::Normal));
        QCOMPARE(password->property("text").toString(), QStringLiteral("test-password"));
        click(showPassword);
        QTRY_COMPARE(password->property("echoMode").toInt(), static_cast<int>(QLineEdit::Password));
        QVERIFY(!help->isVisible());
        click(helpToggle);
        QTRY_VERIFY(help->isVisible());
        QVERIFY(help->property("text").toString().contains(QStringLiteral("app password")));
        click(helpToggle);
        QTRY_VERIFY(!help->isVisible());
        click(showPassword);
        QTRY_VERIFY(showPassword->property("checked").toBool());
        AccountWizardControllerTestAccess::setBusy(controller, true);
        QTRY_COMPARE(password->property("echoMode").toInt(), static_cast<int>(QLineEdit::Password));
        QVERIFY(!username->property("enabled").toBool());
        QVERIFY(!password->property("enabled").toBool());
        QVERIFY(!showPassword->isEnabled());
        AccountWizardControllerTestAccess::setBusy(controller, false);
        QVERIFY(showPassword->isEnabled());
        QCOMPARE(username->property("text").toString(), QStringLiteral("explicit-user"));
        const auto screenshotPath = qEnvironmentVariable("PRIVATECLOUD_SIGNIN_SCREENSHOT");
        if (!screenshotPath.isEmpty()) {
            controller.setBasicAuthPassword({});
            AccountWizardControllerTestAccess::clearError(controller);
            window->show();
            QTRY_VERIFY(window->isExposed());
            QImage screenshot;
            QTRY_VERIFY(!(screenshot = window->grabWindow()).isNull());
            QVERIFY(screenshot.save(screenshotPath));
        }
        controller.cancel();
        QVERIFY(controller.basicAuthPassword().isEmpty());
    }

    void privateCloudReauthenticationShowsBothFields()
    {
        const auto account = Account::create();
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        account->setDavUser(QStringLiteral("canonical-user"));
        account->setCredentials(new WebFlowCredentials(QStringLiteral("stored-user"), QStringLiteral("stored-token")));
        auto *dialog = new PrivateCloudCredentialsDialog(account.data());
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Sign in to privateCloud"));
        const auto username = dialog->findChild<QLineEdit *>(QStringLiteral("privateCloudUsername"));
        const auto password = dialog->findChild<QLineEdit *>(QStringLiteral("privateCloudPassword"));
        QVERIFY(username);
        QVERIFY(password);
        QCOMPARE(username->text(), QStringLiteral("stored-user"));
        QVERIFY(!username->isReadOnly());
        QVERIFY(password->text().isEmpty());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QCOMPARE(username->accessibleName(), QStringLiteral("Username"));
        QCOMPARE(password->accessibleName(), QStringLiteral("Password"));
        const auto showPassword = dialog->findChild<QPushButton *>(QStringLiteral("privateCloudShowPassword"));
        const auto helpToggle = dialog->findChild<QPushButton *>(QStringLiteral("privateCloudPasswordHelpToggle"));
        const auto help = dialog->findChild<QLabel *>(QStringLiteral("privateCloudPasswordHelp"));
        const auto status = dialog->findChild<QLabel *>(QStringLiteral("privateCloudSignInStatus"));
        QVERIFY(showPassword);
        QVERIFY(helpToggle);
        QVERIFY(help);
        QVERIFY(status);
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        username->setFocus();
        QTest::keyClick(username, Qt::Key_Return);
        QTRY_VERIFY(password->hasFocus());
        QVERIFY(status->text().isEmpty());
        QTest::keyClick(password, Qt::Key_Return);
        QCOMPARE(status->text(), QStringLiteral("Enter both your username and password."));
        QVERIFY(username->isEnabled());
        password->setText(QStringLiteral("visible-test-password"));
        QTest::mouseClick(showPassword, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QCOMPARE(showPassword->accessibleName(), QStringLiteral("Hide password"));
        QCOMPARE(password->text(), QStringLiteral("visible-test-password"));
        QTest::mouseClick(showPassword, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(!help->isVisible());
        QTest::mouseClick(helpToggle, Qt::LeftButton);
        QVERIFY(help->isVisible());
        QVERIFY(help->text().contains(QStringLiteral("app password")));
        QTest::mouseClick(helpToggle, Qt::LeftButton);
        QVERIFY(!help->isVisible());
        QCOMPARE(username->text(), QStringLiteral("stored-user"));
        username->setText(QStringLiteral("another-user"));
        dialog->reject();
        QCOMPARE(account->credentials()->user(), QStringLiteral("stored-user"));
        QCOMPARE(account->credentials()->password(), QStringLiteral("stored-token"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void privateCloudRequiresAuthenticationAndOnlineOnlySetup()
    {
        AccountWizardController controller;
        auto availabilityChecks = 0;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [&availabilityChecks](const QString &) {
            ++availabilityChecks;
            return QStringLiteral("Drive is occupied.");
        });
        QSignalSpy finishedSpy(&controller, &AccountWizardController::finished);

        QVERIFY(controller.privateCloudMode());
        QVERIFY(!controller.canUseClassicSync());
        QVERIFY(!controller.canSkipFolderConfiguration());
        QVERIFY(!controller.hasAdvancedOptions());
        controller.setSyncMode(AccountWizardController::SyncEverything);
        QCOMPARE(controller.syncMode(), AccountWizardController::VirtualFiles);
        controller.skipFolderConfiguration();
        controller.finish();
        QCOMPARE(availabilityChecks, 0);
        QCOMPARE(finishedSpy.count(), 0);
    }

    void privateCloudOffersFolderBeforeSetupAndKeepsRetryOnError()
    {
        ConfigFile config;
        const auto previousOverride = config.overrideLocalDir();
        const auto restoreOverride = qScopeGuard([&config, previousOverride] {
            config.setOverrideLocalDir(previousOverride);
        });
        config.setOverrideLocalDir(QStringLiteral("C:/ignored-custom-folder"));

        AccountWizardController controller;
        auto availabilityChecks = 0;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [&availabilityChecks](const QString &) {
            ++availabilityChecks;
            return availabilityChecks == 1 ? QStringLiteral("Drive is occupied.") : QStringLiteral("On-demand files are unavailable.");
        });
        QSignalSpy finishedSpy(&controller, &AccountWizardController::finished);
        AccountWizardControllerTestAccess::completeAuthentication(controller);

        QCOMPARE(controller.currentStep(), AccountWizardController::SyncOptionsStep);
        QCOMPARE(controller.syncMode(), AccountWizardController::VirtualFiles);
        QCOMPARE(AccountWizardControllerTestAccess::remoteFolder(controller), QStringLiteral("/"));
        QVERIFY(controller.needsSyncOptions());
        QCOMPARE(controller.localSyncFolder(), QStringLiteral("C:/PPCsyncConfig"));
        QVERIFY(controller.localSyncFolder() != config.overrideLocalDir());
        QCoreApplication::processEvents();
        QCOMPARE(availabilityChecks, 0);
        QVERIFY(controller.errorText().isEmpty());
        controller.finish();
        QCOMPARE(availabilityChecks, 1);
        QCOMPARE(controller.errorText(), QStringLiteral("Drive is occupied."));
        QVERIFY(!controller.busy());
        QCOMPARE(finishedSpy.count(), 0);

        controller.goBack();
        QCOMPARE(controller.currentStep(), AccountWizardController::SyncOptionsStep);
        controller.finish();
        QCOMPARE(availabilityChecks, 2);
        QCOMPARE(controller.errorText(), QStringLiteral("On-demand files are unavailable."));
        QCOMPARE(finishedSpy.count(), 0);
        controller.cancel();
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.constFirst().constFirst().toInt(), static_cast<int>(QDialog::Rejected));
    }

    void privateCloudCancellationStopsQueuedSetup()
    {
        AccountWizardController controller;
        auto availabilityChecks = 0;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [&availabilityChecks](const QString &) {
            ++availabilityChecks;
            return QStringLiteral("Drive is occupied.");
        });
        QSignalSpy finishedSpy(&controller, &AccountWizardController::finished);
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        controller.cancel();
        QCoreApplication::processEvents();
        controller.finish();
        controller.cancel();
        QCOMPARE(availabilityChecks, 0);
        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.constFirst().constFirst().toInt(), static_cast<int>(QDialog::Rejected));
    }

    void privateCloudFolderPickerKeepsSelectionAndRejectsNonemptyFolders()
    {
        QTemporaryDir location;
        QVERIFY(location.isValid());
        const auto emptyPath = location.filePath(QStringLiteral("chosen-folder"));
        QVERIFY(QDir().mkpath(emptyPath));
        AccountWizardController controller;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QStringLiteral("unused");
        });
        auto selections = 0;
        QString selectedFolder = emptyPath;
        QString pickerStart;
        AccountWizardControllerTestAccess::setFolderDialog(controller, [&](const QString &startFolder) {
            ++selections;
            pickerStart = startFolder;
            return selectedFolder;
        });
        controller.chooseLocalSyncFolder();
        QCOMPARE(selections, 0);
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        QCOMPARE(controller.localSyncFolder(), QStringLiteral("C:/PPCsyncConfig"));
        controller.chooseLocalSyncFolder();
        QCOMPARE(selections, 1);
        QCOMPARE(pickerStart, QStringLiteral("C:/PPCsyncConfig"));
        QCOMPARE(controller.localSyncFolder(), emptyPath);
        QVERIFY2(controller.canFinish(), qPrintable(controller.localSyncFolderError()));
        selectedFolder.clear();
        controller.chooseLocalSyncFolder();
        QCOMPARE(controller.localSyncFolder(), emptyPath);
        AccountWizardControllerTestAccess::setBusy(controller, true);
        controller.chooseLocalSyncFolder();
        QCOMPARE(selections, 2);
        AccountWizardControllerTestAccess::setBusy(controller, false);
        QFile existingFile(QDir(emptyPath).filePath(QStringLiteral("previous-account.txt")));
        QVERIFY(existingFile.open(QIODevice::WriteOnly));
        existingFile.write("preserved bytes");
        existingFile.close();
        // The default path is also subject to the empty-folder rule before Browse is used.
        AccountWizardController defaultController;
        AccountWizardControllerTestAccess::preparePrivateCloud(defaultController, [](const QString &) {
            return QStringLiteral("unused");
        });
        AccountWizardControllerTestAccess::completeAuthentication(defaultController);
        AccountWizardControllerTestAccess::setLocalFolder(defaultController, emptyPath);
        QVERIFY(!defaultController.canFinish());
        QVERIFY(defaultController.localSyncFolderError().contains(QStringLiteral("empty")));
        selectedFolder = emptyPath;
        controller.chooseLocalSyncFolder();
        QVERIFY(!controller.canFinish());
        QVERIFY(controller.localSyncFolderError().contains(QStringLiteral("empty")));
        QVERIFY(existingFile.open(QIODevice::ReadOnly));
        QCOMPARE(existingFile.readAll(), QByteArray("preserved bytes"));
        controller.cancel();
        controller.chooseLocalSyncFolder();
        QCOMPARE(selections, 3);
    }

    void privateCloudExistingFolderAdmission_data()
    {
        QTest::addColumn<bool>("sameServer");
        QTest::addColumn<bool>("sameAccount");
        QTest::newRow("matching-server-and-account") << true << true;
        QTest::newRow("different-account") << true << false;
        QTest::newRow("different-server") << false << true;
    }

    void privateCloudExistingFolderAdmission()
    {
#ifdef BUILD_FILE_PROVIDER_MODULE
        QSKIP("Existing local folder adoption uses Windows folder-backed virtual files, not the macOS File Provider.");
#endif
        QFETCH(bool, sameServer);
        QFETCH(bool, sameAccount);
        QTemporaryDir location;
        QTemporaryDir profileDirectory;
        QVERIFY(location.isValid());
        QVERIFY(profileDirectory.isValid());
        AccountWizardController controller;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QString{};
        });
        const auto account = AccountWizardControllerTestAccess::account(controller);
        FolderDefinition definition;
        definition.localPath = FolderDefinition::prepareLocalPath(location.path());
        definition.targetPath = u"/"_s;
        definition.journalPath = u".sync_123456789abc.db"_s;
        definition.virtualFilesMode = Vfs::WindowsCfApi;
        definition.ignoreHiddenFiles = false;
        {
            SyncJournalDb journal(definition.absoluteJournalPath());
            QVERIFY(journal.open());
            journal.keyValueStoreSet(u"last_sync"_s, qint64(123456));
            journal.commit(u"existing account fixture"_s, false);
        }
        QFile existingFile(location.filePath(u"keep.txt"_s));
        QVERIFY(existingFile.open(QIODevice::WriteOnly));
        QCOMPARE(existingFile.write("existing user data"), qint64(18));
        existingFile.close();
        const auto profilePath = profileDirectory.filePath(u"nextcloud.cfg"_s);
        {
            QSettings settings(profilePath, QSettings::IniFormat);
            settings.setValue(u"Accounts/version"_s, 13);
            settings.beginGroup(u"Accounts/0"_s);
            settings.setValue(u"version"_s, 13);
            settings.setValue(u"url"_s, sameServer ? account->url().toString() : u"https://different.example"_s);
            settings.setValue(u"dav_user"_s, sameAccount ? account->davUser() : u"another-user"_s);
            settings.beginGroup(u"FoldersWithPlaceholders/2"_s);
            FolderDefinition::save(settings, definition);
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        AccountWizardControllerTestAccess::setAdoptionProfiles(controller, {profilePath});
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        AccountWizardControllerTestAccess::setLocalFolder(controller, location.path());
        if (sameServer && sameAccount) {
            QVERIFY2(controller.canFinish(), qPrintable(controller.localSyncFolderError()));
            const auto adopted = AccountWizardControllerTestAccess::syncFolderDefinition(controller);
            QCOMPARE(adopted.journalPath, definition.journalPath);
            QCOMPARE(adopted.ignoreHiddenFiles, false);
            QCOMPARE(adopted.virtualFilesMode, Vfs::WindowsCfApi);
            QVERIFY(adopted.paused);
            QVERIFY(adopted.privateCloudSetupPending);
            QVERIFY(adopted.privateCloudPreservePinState);

            QQmlApplicationEngine engine;
            engine.addImportPath(QStringLiteral("qrc:/qml/theme"));
            engine.addImageProvider(QStringLiteral("svgimage-custom-color"), new Ui::SvgImageProvider);
            QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/src/gui/wizard/qml/AccountWizardWindow.qml")));
            const auto object =
                std::unique_ptr<QObject>(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
            QVERIFY2(object, qPrintable(component.errorString()));
            const auto window = qobject_cast<QQuickWindow *>(object.get());
            QVERIFY(window);
            const auto reuseMessage = window->findChild<QObject *>(QStringLiteral("privateCloudFolderReuse"));
            QVERIFY(reuseMessage);
            window->show();
            QTRY_VERIFY(window->isExposed());
            QTRY_VERIFY(reuseMessage->property("visible").toBool());
            QVERIFY(reuseMessage->property("text").toString().contains(QStringLiteral("sync history")));

            // The original profile can change while the server check is outstanding.
            {
                QSettings settings(profilePath, QSettings::IniFormat);
                settings.setValue(u"Accounts/0/dav_user"_s, u"another-user"_s);
                settings.sync();
            }
            AccountWizardControllerTestAccess::showDriveSetup(controller);
            AccountWizardControllerTestAccess::setBusy(controller, true);
            QSignalSpy finishedSpy(&controller, &AccountWizardController::finished);
            AccountWizardControllerTestAccess::completeRemoteFolderCheck(controller);
            QCOMPARE(controller.currentStep(), AccountWizardController::SyncOptionsStep);
            QVERIFY(!controller.busy());
            QVERIFY(!controller.errorText().isEmpty());
            QCOMPARE(finishedSpy.count(), 0);

            // Switching back to a fresh location must discard the adoption candidate.
            const auto freshPath = profileDirectory.filePath(u"fresh-folder"_s);
            AccountWizardControllerTestAccess::setLocalFolder(controller, freshPath);
            QVERIFY2(controller.canFinish(), qPrintable(controller.localSyncFolderError()));
            const auto freshDefinition = AccountWizardControllerTestAccess::syncFolderDefinition(controller);
            QVERIFY(freshDefinition.journalPath.isEmpty());
            QVERIFY(!freshDefinition.privateCloudPreservePinState);
            const auto freshReuseMessage = window->findChild<QObject *>(QStringLiteral("privateCloudFolderReuse"));
            QVERIFY(freshReuseMessage);
            QTRY_VERIFY(!freshReuseMessage->property("visible").toBool());
        } else {
            QVERIFY(!controller.canFinish());
            QVERIFY(!controller.localSyncFolderError().isEmpty());
        }
        QVERIFY(existingFile.open(QIODevice::ReadOnly));
        QCOMPARE(existingFile.readAll(), QByteArray("existing user data"));
        SyncJournalDb preserved(definition.absoluteJournalPath());
        QVERIFY(preserved.open());
        QCOMPARE(preserved.keyValueStoreGetInt(u"last_sync"_s, -1), qint64(123456));
    }

    void privateCloudRechecksEmptyFolderAfterRemoteCheck()
    {
        QTemporaryDir location;
        QVERIFY(location.isValid());
        AccountWizardController controller;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QStringLiteral("unused");
        });
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        AccountWizardControllerTestAccess::setLocalFolder(controller, location.path());
        QVERIFY2(controller.canFinish(), qPrintable(controller.localSyncFolderError()));
        AccountWizardControllerTestAccess::showDriveSetup(controller);
        AccountWizardControllerTestAccess::setBusy(controller, true);
        QFile unexpectedFile(location.filePath(QStringLiteral("do-not-upload.txt")));
        QVERIFY(unexpectedFile.open(QIODevice::WriteOnly));
        unexpectedFile.write("private local data");
        unexpectedFile.close();
        QSignalSpy finishedSpy(&controller, &AccountWizardController::finished);
        AccountWizardControllerTestAccess::completeRemoteFolderCheck(controller);
        QCOMPARE(controller.currentStep(), AccountWizardController::SyncOptionsStep);
        QVERIFY(!controller.busy());
        QVERIFY(controller.errorText().contains(QStringLiteral("empty")));
        QCOMPARE(finishedSpy.count(), 0);
        QVERIFY(unexpectedFile.open(QIODevice::ReadOnly));
        QCOMPARE(unexpectedFile.readAll(), QByteArray("private local data"));
    }

    void privateCloudPreparesEmptyFolderBeforeRemoteCheck()
    {
        QTemporaryDir location;
        QVERIFY(location.isValid());
        const auto selectedFolder = location.filePath(QStringLiteral("new-sync-folder"));
        AccountWizardController controller;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QString();
        });
        auto *manager = new FakeQNAM({});
        QList<QUrl> requests;
        manager->setOverride([&requests, manager](QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *) {
            requests.append(request.url());
            return new FakeHangingReply(operation, request, manager);
        });
        AccountWizardControllerTestAccess::setNetworkManager(controller, manager);
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        AccountWizardControllerTestAccess::setLocalFolder(controller, selectedFolder);
        QVERIFY2(controller.canFinish(), qPrintable(controller.localSyncFolderError()));
        controller.finish();
        QCOMPARE(controller.currentStep(), AccountWizardController::DriveSetupStep);
        QVERIFY(controller.busy());
        QTRY_COMPARE(requests.size(), 1);
        QVERIFY(requests.constFirst().path().contains(QStringLiteral("/remote.php/dav/files/alice/")));
        QVERIFY(QDir(selectedFolder).exists());
        QVERIFY(QDir(selectedFolder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty());
        QSettings settings(ConfigFile().configFile(), QSettings::IniFormat);
        QVERIFY(!settings.contains(QStringLiteral("PrivateCloudBackingRoot/Path")));
        controller.cancel();
    }

    void privateCloudFolderPageOffersBrowseAndConnectDrive()
    {
        QTemporaryDir location;
        QVERIFY(location.isValid());
        AccountWizardController controller;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [](const QString &) {
            return QStringLiteral("Drive is occupied.");
        });
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        AccountWizardControllerTestAccess::setFolderDialog(controller, [&](const QString &) {
            return location.path();
        });
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral("qrc:/qml/theme"));
        engine.addImageProvider(QStringLiteral("svgimage-custom-color"), new Ui::SvgImageProvider);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/src/gui/wizard/qml/AccountWizardWindow.qml")));
        const auto object = std::unique_ptr<QObject>(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        const auto window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QVERIFY(window->findChild<QObject *>(QStringLiteral("privateCloudFolderPage")));
        const auto path = window->findChild<QObject *>(QStringLiteral("privateCloudFolderPath"));
        const auto browse = window->findChild<QObject *>(QStringLiteral("privateCloudChooseFolder"));
        const auto connectDrive = window->findChild<QObject *>(QStringLiteral("wizardFinishButton"));
        const auto skip = window->findChild<QObject *>(QStringLiteral("wizardSkipFolderButton"));
        const auto advanced = window->findChild<QObject *>(QStringLiteral("wizardAdvancedOptionsButton"));
        QVERIFY(path);
        QVERIFY(browse);
        QVERIFY(connectDrive);
        QVERIFY(skip);
        QVERIFY(advanced);
        window->show();
        QTRY_VERIFY(window->isExposed());
        QCOMPARE(path->property("text").toString(), QDir::toNativeSeparators(QStringLiteral("C:/PPCsyncConfig")));
        QVERIFY(!skip->property("visible").toBool());
        QVERIFY(!advanced->property("visible").toBool());
        QCOMPARE(connectDrive->property("text").toString(), QStringLiteral("Connect drive"));
        const auto screenshotPath = qEnvironmentVariable("PRIVATECLOUD_FOLDER_SCREENSHOT");
        if (!screenshotPath.isEmpty()) {
            QImage screenshot;
            QTRY_VERIFY(!(screenshot = window->grabWindow()).isNull());
            QVERIFY(screenshot.save(screenshotPath));
        }
        QVERIFY(QMetaObject::invokeMethod(browse, "clicked"));
        QCOMPARE(controller.localSyncFolder(), location.path());
        QCOMPARE(path->property("text").toString(), QDir::toNativeSeparators(location.path()));
        QVERIFY(connectDrive->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(connectDrive, "clicked"));
        QCOMPARE(controller.errorText(), QStringLiteral("Drive is occupied."));
        QCOMPARE(controller.currentStep(), AccountWizardController::SyncOptionsStep);
        AccountWizardControllerTestAccess::setBusy(controller, true);
        QVERIFY(!browse->property("enabled").toBool());
        QVERIFY(!connectDrive->property("enabled").toBool());
        AccountWizardControllerTestAccess::setBusy(controller, false);
        QCOMPARE(controller.syncMode(), AccountWizardController::VirtualFiles);
        controller.cancel();
    }

    void privateCloudResumesPendingFolderAfterCancellation()
    {
        class TestVfs : public VfsOff
        {
        public:
            Mode mode() const override
            {
                return WindowsCfApi;
            }
        };

        const auto account = Account::create();
        QVERIFY(account->setUrl(Account::enforcedServerUrl().isEmpty() ? QUrl(QStringLiteral("https://cloud.example")) : Account::enforcedServerUrl()));
        account->setDavUser(QUuid::createUuid().toString());
        const auto state = AccountStatePtr(new FakeAccountState(account));
        FolderDefinition definition;
        definition.localPath = PrivateCloudDrive::backingPath(account->url().toString(QUrl::FullyEncoded) + QLatin1Char('\n') + account->davUser());
        definition.targetPath = QStringLiteral("/");
        definition.journalPath = QStringLiteral(".sync-test.db");
        definition.alias = QStringLiteral("pending-setup-test");
        definition.paused = true;
        definition.privateCloudSetupPending = true;
        definition.virtualFilesMode = Vfs::WindowsCfApi;
        Folder folder(definition, state.data(), std::make_unique<TestVfs>());
        QPointer<Folder> retainedFolder(&folder);
        auto availabilityChecks = 0;
        const auto availabilityCheck = [&availabilityChecks](const QString &) {
            ++availabilityChecks;
            return QStringLiteral("Drive is occupied.");
        };
        {
            AccountWizardController controller;
            AccountWizardControllerTestAccess::preparePrivateCloud(controller, availabilityCheck);
            QVERIFY(AccountWizardControllerTestAccess::resumePrivateCloudSetup(controller, &folder));
            QVERIFY(!controller.startLoginFlowAutomatically());
            QTRY_COMPARE(availabilityChecks, 1);
            QCOMPARE(controller.currentStep(), AccountWizardController::DriveSetupStep);
            controller.cancel();
        }
        QVERIFY(retainedFolder);
        QVERIFY(folder.privateCloudSetupPending());
        QVERIFY(folder.syncPaused());
        AccountWizardController resumedController;
        AccountWizardControllerTestAccess::preparePrivateCloud(resumedController, availabilityCheck);
        QVERIFY(AccountWizardControllerTestAccess::resumePrivateCloudSetup(resumedController, &folder));
        QTRY_COMPARE(availabilityChecks, 2);
        QCOMPARE(QDir(resumedController.localSyncFolder()), QDir(definition.localPath));
        QCOMPARE(resumedController.errorText(), QStringLiteral("Drive is occupied."));
        resumedController.cancel();
    }

    void privateCloudSetupPageShowsErrorAndRetry()
    {
#ifdef Q_OS_WIN
        if (!qEnvironmentVariable("PRIVATECLOUD_TEST_SCREENSHOT").isEmpty()) {
            QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("Fonts/segoeui.ttf")));
            QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
        }
#endif
        AccountWizardController controller;
        auto availabilityChecks = 0;
        AccountWizardControllerTestAccess::preparePrivateCloud(controller, [&availabilityChecks](const QString &) {
            ++availabilityChecks;
            return QStringLiteral("Drive X: is already in use. Free the drive letter and try again.");
        });
        AccountWizardControllerTestAccess::completeAuthentication(controller);
        AccountWizardControllerTestAccess::showDriveSetup(controller);
        controller.finish();
        QCOMPARE(availabilityChecks, 1);

        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral("qrc:/qml/theme"));
        engine.addImageProvider(QStringLiteral("svgimage-custom-color"), new Ui::SvgImageProvider);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/src/gui/wizard/qml/AccountWizardWindow.qml")));
        const auto object = std::unique_ptr<QObject>(component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        const auto window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        const auto setupPage = window->findChild<QObject *>(QStringLiteral("privateCloudSetupPage"));
        QVERIFY(setupPage);
        const auto retry = window->findChild<QObject *>(QStringLiteral("wizardFinishButton"));
        QVERIFY(retry);
        window->show();
        QTRY_VERIFY(retry->property("visible").toBool());
        QVERIFY(retry->property("enabled").toBool());
        QCOMPARE(retry->property("text").toString(), QStringLiteral("Retry"));
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QCOMPARE(availabilityChecks, 2);
        QCOMPARE(controller.currentStep(), AccountWizardController::DriveSetupStep);
        AccountWizardControllerTestAccess::setBusy(controller, true);
        QVERIFY(!retry->property("enabled").toBool());
        AccountWizardControllerTestAccess::setBusy(controller, false);

        const auto screenshotPath = qEnvironmentVariable("PRIVATECLOUD_TEST_SCREENSHOT");
        if (!screenshotPath.isEmpty()) {
            QTRY_VERIFY(window->isExposed());
            QImage screenshot;
            QTRY_VERIFY(!(screenshot = window->grabWindow()).isNull());
            QVERIFY(screenshot.save(screenshotPath));
        }
    }

    // Keeps tests off the device policy of the machine running them.
    void init()
    {
        ConfigFile::setDeviceSourcesFactory([] {
            return std::vector<std::unique_ptr<SettingSource>>{};
        });
    }

    void cleanup()
    {
        ConfigFile::setDeviceSourcesFactory({});
    }

    void choosingLocalSyncFolderKeepsItsBookmarkForTheSyncFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto bookmarkData = "bookmark"_ba;

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
            completion(dir.path(), bookmarkData);
        });
        controller.chooseLocalSyncFolder();

        QCOMPARE(controller.localSyncFolder(), QDir::fromNativeSeparators(dir.path()));
        const auto definition = AccountWizardControllerTestAccess::syncFolderDefinition(controller);
        QCOMPARE(definition.localPath, FolderDefinition::prepareLocalPath(dir.path()));
        QCOMPARE(definition.securityScopedBookmarkData, bookmarkData);
    }

    void cancellingLocalSyncFolderPickerKeepsTheChosenFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto bookmarkData = "bookmark"_ba;
        auto cancel = false;

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
            completion(cancel ? QString{} : dir.path(), cancel ? QByteArray{} : bookmarkData);
        });
        controller.chooseLocalSyncFolder();
        cancel = true;
        controller.chooseLocalSyncFolder();

        QCOMPARE(controller.localSyncFolder(), QDir::fromNativeSeparators(dir.path()));
        QCOMPARE(AccountWizardControllerTestAccess::syncFolderDefinition(controller).securityScopedBookmarkData, bookmarkData);
    }

    void choosingLocalSyncFolderWithoutBookmarkDropsThePreviousBookmark()
    {
        QTemporaryDir firstDir;
        QTemporaryDir secondDir;
        QVERIFY(firstDir.isValid());
        QVERIFY(secondDir.isValid());
        auto chosenDir = firstDir.path();
        auto chosenBookmarkData = "bookmark"_ba;

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
            completion(chosenDir, chosenBookmarkData);
        });
        controller.chooseLocalSyncFolder();
        chosenDir = secondDir.path();
        chosenBookmarkData.clear();
        controller.chooseLocalSyncFolder();

        QCOMPARE(controller.localSyncFolder(), QDir::fromNativeSeparators(secondDir.path()));
        QVERIFY(AccountWizardControllerTestAccess::syncFolderDefinition(controller).securityScopedBookmarkData.isEmpty());
    }

    void closingReportsAcceptedOnlyAfterSetupCompleted()
    {
        AccountWizardController controller;
        QCOMPARE(controller.resultOnClose(), static_cast<int>(QDialog::Rejected));

        AccountWizardControllerTestAccess::setCurrentStep(controller, AccountWizardController::SyncOptionsStep);
        QCOMPARE(controller.resultOnClose(), static_cast<int>(QDialog::Rejected));

        AccountWizardControllerTestAccess::setCurrentStep(controller, AccountWizardController::CompletedStep);
        QCOMPARE(controller.resultOnClose(), static_cast<int>(QDialog::Accepted));
    }

    void localSyncFolderPickerOpensOnceWhilePending()
    {
        auto pickerCalls = 0;
        std::function<void(const QString &, const QByteArray &)> pendingCompletion;

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
            ++pickerCalls;
            pendingCompletion = completion;
        });
        controller.chooseLocalSyncFolder();
        controller.chooseLocalSyncFolder();
        QCOMPARE(pickerCalls, 1);

        pendingCompletion({}, {});
        controller.chooseLocalSyncFolder();
        QCOMPARE(pickerCalls, 2);
    }

    void localSyncFolderPickerCompletionAfterControllerIsGoneIsIgnored()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::function<void(const QString &, const QByteArray &)> pendingCompletion;

        {
            AccountWizardController controller;
            AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
                pendingCompletion = completion;
            });
            controller.chooseLocalSyncFolder();
        }

        QVERIFY(pendingCompletion);
        pendingCompletion(dir.path(), "bookmark"_ba);
    }

    void initialLocalSyncFolderSelectionKeepsTheOverride()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalSyncFolderPicker(controller, [&](const QString &, const QString &, const auto &completion) {
            completion(dir.path(), {});
        });
        AccountWizardControllerTestAccess::setLocalSyncFolderOverride(controller, true);
        AccountWizardControllerTestAccess::openInitialLocalSyncFolderDialog(controller);
        QVERIFY(AccountWizardControllerTestAccess::localSyncFolderOverride(controller));

        controller.chooseLocalSyncFolder();
        QVERIFY(!AccountWizardControllerTestAccess::localSyncFolderOverride(controller));
    }

    void switchesAwayFromVirtualFilesWhenEnforcedOff()
    {
        ConfigFile::setDeviceSourcesFactory([] {
            std::vector<std::unique_ptr<SettingSource>> sources;
            sources.push_back(std::make_unique<MapSource>(SettingSourceType::PlatformPolicy,
                                                          EnforcementState::Enforced,
                                                          200,
                                                          QVariantMap{{QStringLiteral("virtualFilesMode"), QStringLiteral("off")}}));
            return sources;
        });

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setAccountUrl(controller, allowedServerUrl());
        AccountWizardControllerTestAccess::setSyncMode(controller, AccountWizardController::VirtualFiles);
        QCOMPARE(controller.syncMode(), AccountWizardController::VirtualFiles);

        AccountWizardControllerTestAccess::chooseSyncModeAfterCapabilities(controller);

        if (controller.privateCloudMode()) {
            QCOMPARE(controller.syncMode(), AccountWizardController::VirtualFiles);
            QVERIFY(!controller.canUseClassicSync());
            QVERIFY(!controller.canUseVirtualFiles());
        } else {
            QVERIFY(controller.syncMode() != AccountWizardController::VirtualFiles);
        }
        QVERIFY(controller.needsSyncOptions());
        QVERIFY(controller.localSyncFolderRequired());
    }

    void normalizesCommonServerUrlSuffixes()
    {
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QStringLiteral(" https://cloud.example/index.php ")),
                 QStringLiteral("https://cloud.example/"));
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QStringLiteral("https://cloud.example/remote.php/dav/"), QStringLiteral("remote.php/dav/")),
                 QStringLiteral("https://cloud.example/"));
    }

    void rejectsLoginFlowOutsideCompiledServerRestriction()
    {
        const auto enforcedUrl = Account::enforcedServerUrl();
        if (enforcedUrl.isEmpty()) {
            QSKIP("This test requires a compiled server restriction.");
        }
        AccountWizardController controller;
        QVERIFY(!controller.setServerUrlForLoginFlow(QUrl(QStringLiteral("https://unconfigured.example.com"))));
        QCOMPARE(controller.serverUrl(), enforcedUrl.toString());
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY(controller.setServerUrlForLoginFlow(enforcedUrl));
        QCOMPARE(controller.serverUrl(), enforcedUrl.toString());
    }

    void defaultsToHttpsWhenSchemeIsMissing()
    {
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QString()), QString());
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QStringLiteral("cloud.example")), QStringLiteral("https://cloud.example"));
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QStringLiteral("cloud.example:8080")), QStringLiteral("https://cloud.example:8080"));
        QCOMPARE(AccountWizardController::normalizeServerUrlInput(QStringLiteral("http://cloud.example")), QStringLiteral("http://cloud.example"));
    }

    void localNetworkPermissionCheckIgnoresInvalidUrls()
    {
        auto callbackCalled = false;
        auto denied = true;

        LocalNetworkPermission::checkDeniedForConnection({}, this, [&callbackCalled, &denied](bool result) {
            callbackCalled = true;
            denied = result;
        });

        QTRY_VERIFY(callbackCalled);
        QVERIFY(!denied);
    }

    void localNetworkPermissionFailureSkipsSecureConnectionRecovery()
    {
        AccountWizardController controller;
        AccountWizardControllerTestAccess::setLocalNetworkPermissionDenied(controller, true);
        AccountWizardControllerTestAccess::setAccountUrl(controller, allowedServerUrl());
        QSignalSpy recoverySpy(&controller, &AccountWizardController::secureConnectionFailed);

        QVERIFY(QMetaObject::invokeMethod(&controller, "slotNoServerFoundTimeout", Qt::DirectConnection, Q_ARG(QUrl, allowedServerUrl())));

        QCOMPARE(recoverySpy.count(), 0);
        QCOMPARE(controller.errorText(), LocalNetworkPermission::deniedError());
    }

    void otherServerFailureOffersSecureConnectionRecovery()
    {
        AccountWizardController controller;
        const auto serverUrl = allowedServerUrl();
        AccountWizardControllerTestAccess::setLocalNetworkPermissionDenied(controller, false);
        AccountWizardControllerTestAccess::setAccountUrl(controller, serverUrl);
        QSignalSpy recoverySpy(&controller, &AccountWizardController::secureConnectionFailed);

        QVERIFY(QMetaObject::invokeMethod(&controller, "slotNoServerFoundTimeout", Qt::DirectConnection, Q_ARG(QUrl, serverUrl)));

        QCOMPARE(recoverySpy.count(), 1);
        const auto arguments = recoverySpy.takeFirst();
        QCOMPARE(arguments.at(0).toString(), serverUrl.host());
        QCOMPARE(arguments.at(1).toBool(), false);
    }
    void invalidServerUrlKeepsCurrentStep()
    {
        QFETCH(QString, serverUrl);

        AccountWizardController controller;
        const auto initialStep = controller.currentStep();
        QSignalSpy errorSpy(&controller, &AccountWizardController::errorTextChanged);

        controller.setServerUrl(serverUrl);
        controller.submitServerUrl();

        QCOMPARE(controller.currentStep(), initialStep);
        QVERIFY(!controller.errorText().isEmpty());
        QCOMPARE(errorSpy.count(), 1);
    }

    void invalidServerUrlKeepsCurrentStep_data()
    {
        QTest::addColumn<QString>("serverUrl");

        QTest::newRow("empty") << QString();
        QTest::newRow("plain text with spaces") << QStringLiteral("not a url");
        QTest::newRow("missing host") << QStringLiteral("https://");
        QTest::newRow("missing host with path") << QStringLiteral("https:///remote.php/dav");
        QTest::newRow("invalid scheme marker") << QStringLiteral("://cloud.example");
    }

    void rejectsIncompleteManualProxyBeforeServerCheck()
    {
        AccountWizardController controller;
        const auto initialStep = controller.currentStep();
        QSignalSpy errorSpy(&controller, &AccountWizardController::errorTextChanged);

        controller.setServerUrl(QStringLiteral("https://cloud.example"));
        controller.setProxyMode(2);
        controller.submitServerUrl();

        QCOMPARE(controller.currentStep(), initialStep);
        QCOMPARE(controller.errorText(), QStringLiteral("Proxy settings are incomplete."));
        QCOMPARE(errorSpy.count(), 1);
    }

    void validatesManualProxyConfiguration()
    {
        AccountWizardController controller;
        QSignalSpy proxySpy(&controller, &AccountWizardController::proxySettingsChanged);

        QVERIFY(controller.proxySettingsValid());
        QCOMPARE(controller.proxyMode(), 0);

        controller.setProxyMode(2);
        QCOMPARE(controller.proxyMode(), 2);
        QCOMPARE(controller.manualProxyType(), 0);
        QVERIFY(!controller.proxySettingsValid());

        controller.setProxyHost(QStringLiteral("proxy.example"));
        QVERIFY(controller.proxySettingsValid());

        controller.setProxyAuthenticationRequired(true);
        QVERIFY(!controller.proxySettingsValid());

        controller.setProxyUser(QStringLiteral("alice"));
        QVERIFY(!controller.proxySettingsValid());

        controller.setProxyPassword(QStringLiteral("secret"));
        QVERIFY(controller.proxySettingsValid());

        controller.setManualProxyType(1);
        QCOMPARE(controller.manualProxyType(), 1);
        QCOMPARE(controller.proxyMode(), 2);
        QVERIFY(controller.proxySettingsValid());

        QVERIFY(proxySpy.count() >= 6);
    }

    void boundsManualProxyPort()
    {
        AccountWizardController controller;

        controller.setProxyPort(0);
        QCOMPARE(controller.proxyPort(), 1);

        controller.setProxyPort(70000);
        QCOMPARE(controller.proxyPort(), 65535);

        controller.setProxyPort(3128);
        QCOMPARE(controller.proxyPort(), 3128);
    }

    void seedsProxySettingsFromManagedDefault()
    {
        ConfigFile::setDeviceSourcesFactory([] {
            std::vector<std::unique_ptr<SettingSource>> sources;
            sources.push_back(std::make_unique<MapSource>(
                SettingSourceType::PlatformDefault,
                EnforcementState::NotEnforced,
                20,
                QVariantMap{{u"proxyType"_s, int(QNetworkProxy::HttpProxy)}, {u"proxyHost"_s, u"proxy.example.com"_s}, {u"proxyPort"_s, 8080}}));
            return sources;
        });

        AccountWizardController controller;

        QCOMPARE(controller.proxyMode(), 2);
        QCOMPARE(controller.manualProxyType(), 0);
        QCOMPARE(controller.proxyHost(), u"proxy.example.com"_s);
        QCOMPARE(controller.proxyPort(), 8080);
    }

    void warnsWhenManualProxyTargetsLocalhost_data()
    {
        QTest::addColumn<QString>("serverUrl");
        QTest::addColumn<bool>("warns");

        QTest::newRow("localhost") << QStringLiteral("http://localhost") << true;
        QTest::newRow("localhost without scheme") << QStringLiteral("localhost") << true;
        QTest::newRow("ipv4 loopback") << QStringLiteral("http://127.0.0.1") << true;
        QTest::newRow("ipv4 loopback subnet") << QStringLiteral("http://127.12.34.56") << true;
        QTest::newRow("ipv6 loopback") << QStringLiteral("http://[::1]") << true;
        QTest::newRow("remote host") << QStringLiteral("https://cloud.example") << false;
    }

    void warnsWhenManualProxyTargetsLocalhost()
    {
        QFETCH(QString, serverUrl);
        QFETCH(bool, warns);

        AccountWizardController controller;

        controller.setServerUrl(serverUrl);
        QVERIFY(!controller.showProxyLocalhostWarning());

        controller.setProxyMode(2);
        controller.setProxyHost(QStringLiteral("proxy.example"));

        QCOMPARE(controller.showProxyLocalhostWarning(), warns);
    }

    void systemProxyAndNoProxyDoNotRequireManualSettings()
    {
        AccountWizardController controller;

        controller.setProxyMode(2);
        QVERIFY(!controller.proxySettingsValid());

        controller.setProxyMode(1);
        QCOMPARE(controller.proxyMode(), 1);
        QVERIFY(controller.proxySettingsValid());
        QVERIFY(!controller.showProxyLocalhostWarning());

        controller.setProxyMode(0);
        QCOMPARE(controller.proxyMode(), 0);
        QVERIFY(controller.proxySettingsValid());
    }

    void manualProxyTypeDoesNotSelectManualProxy()
    {
        AccountWizardController controller;

        controller.setProxyMode(0);
        controller.setManualProxyType(0);
        QCOMPARE(controller.proxyMode(), 0);
        QVERIFY(controller.proxySettingsValid());

        controller.setProxyMode(1);
        controller.setManualProxyType(1);
        QCOMPARE(controller.proxyMode(), 1);
        QVERIFY(controller.proxySettingsValid());
    }

    void advancedSafeguardsEmitChanges()
    {
        AccountWizardController controller;
        QSignalSpy askLargeSpy(&controller, &AccountWizardController::askBeforeLargeFoldersChanged);
        QSignalSpy thresholdSpy(&controller, &AccountWizardController::largeFolderThresholdMbChanged);
        QSignalSpy externalSpy(&controller, &AccountWizardController::askBeforeExternalStorageChanged);

        controller.setAskBeforeLargeFolders(!controller.askBeforeLargeFolders());
        controller.setLargeFolderThresholdMb(controller.largeFolderThresholdMb() + 1);
        controller.setAskBeforeExternalStorage(!controller.askBeforeExternalStorage());

        QCOMPARE(askLargeSpy.count(), 1);
        QCOMPARE(thresholdSpy.count(), 1);
        QCOMPARE(externalSpy.count(), 1);
    }

    void opensConfiguredSignupUrlInBrowser()
    {
        QDesktopServices::setUrlHandler(u"http"_s, this, "signupUrlRequested");
        QDesktopServices::setUrlHandler(u"https"_s, this, "signupUrlRequested");
        const auto resetUrlHandlers = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(u"http"_s);
            QDesktopServices::unsetUrlHandler(u"https"_s);
        });
        QSignalSpy browserSpy(this, &TestAccountWizardController::signupUrlRequested);
        QVERIFY(browserSpy.isValid());

        AccountWizardController controller;
        controller.openSignup();

        QCOMPARE(browserSpy.count(), 1);
        QCOMPARE(browserSpy.constFirst().constFirst().toUrl(), Theme::instance()->wizardSignupUrl());
    }

    void exposesProxySettingsAvailability()
    {
        AccountWizardController controller;

        QCOMPARE(controller.proxySettingsAvailable(), !Theme::instance()->doNotUseProxy());
    }

    void preservesForcedOverrideServerChoices()
    {
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto restoreTheme = qScopeGuard([theme, previousOverrideServerUrl, previousForceOverrideServerUrl] {
            theme->setOverrideServerUrl(previousOverrideServerUrl);
            theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
        });

        theme->setOverrideServerUrl(
            QStringLiteral("["
                           R"({"name": "Primary", "url": "https://primary.example"},)"
                           R"({"name": "Secondary", "url": "https://secondary.example"})"
                           "]"));
        theme->setForceOverrideServerUrl(true);

        AccountWizardController controller;

        if (!Account::enforcedServerUrl().isEmpty()) {
            QVERIFY(controller.overrideServerNames().isEmpty());
            QVERIFY(!controller.overrideServerSelectionRequired());
            QCOMPARE(controller.serverUrl(), Account::enforcedServerUrl().toString());
            QVERIFY(!controller.serverUrlEditable());
            controller.setOverrideServerIndex(1);
            QCOMPARE(controller.serverUrl(), Account::enforcedServerUrl().toString());
            return;
        }

        QCOMPARE(controller.overrideServerNames(), QStringList({QStringLiteral("Primary"), QStringLiteral("Secondary")}));
        QVERIFY(controller.overrideServerSelectionRequired());
        QCOMPARE(controller.overrideServerIndex(), 0);
        QCOMPARE(controller.serverUrl(), QStringLiteral("https://primary.example"));
        QVERIFY(!controller.serverUrlEditable());

        controller.setOverrideServerIndex(1);

        QCOMPARE(controller.overrideServerIndex(), 1);
        QCOMPARE(controller.serverUrl(), QStringLiteral("https://secondary.example"));
    }

    void startsLoginFlowAutomaticallyForPersistedOverrideServerUrl()
    {
        ConfigFile cfg;
        const auto previousConfiguredOverrideServerUrl = cfg.overrideServerUrl();
        auto theme = Theme::instance();
        const auto previousOverrideServerUrl = theme->overrideServerUrl();
        const auto previousForceOverrideServerUrl = theme->forceOverrideServerUrl();
        const auto previousStartLoginFlowAutomatically = theme->startLoginFlowAutomatically();
        const auto restoreConfiguration = qScopeGuard(
            [&cfg, theme, previousConfiguredOverrideServerUrl, previousOverrideServerUrl, previousForceOverrideServerUrl, previousStartLoginFlowAutomatically] {
                cfg.setOverrideServerUrl(previousConfiguredOverrideServerUrl);
                theme->setOverrideServerUrl(previousOverrideServerUrl);
                theme->setForceOverrideServerUrl(previousForceOverrideServerUrl);
                theme->setStartLoginFlowAutomatically(previousStartLoginFlowAutomatically);
            });

        QCOMPARE(cfg.setOverrideServerUrl(QStringLiteral("https://cloud.example")), Account::enforcedServerUrl().isEmpty());
        theme->setOverrideServerUrl({});
        theme->setForceOverrideServerUrl(false);
        theme->setStartLoginFlowAutomatically(false);

        AccountWizardController controller;

        QCOMPARE(controller.serverUrl(), allowedServerUrl().toString());
        QVERIFY(!controller.serverUrlEditable());
        QCOMPARE(controller.startLoginFlowAutomatically(), !controller.privateCloudMode());
    }

    void prefillsServerUrlFromManagedDefault()
    {
        const auto restoreConfiguration = restoreOverrideState();
        setManagedOverrideServerUrl(SettingSourceType::PlatformDefault, EnforcementState::NotEnforced, 20);

        AccountWizardController controller;

        QCOMPARE(controller.serverUrl(), allowedServerUrl().toString());
#if defined(APPLICATION_SERVER_URL) && defined(APPLICATION_SERVER_URL_ENFORCE)
        QVERIFY(!controller.serverUrlEditable());
        QVERIFY(Theme::instance()->forceOverrideServerUrl());
#else
        QVERIFY(controller.serverUrlEditable());
        QVERIFY(!Theme::instance()->forceOverrideServerUrl());
#endif
        QCOMPARE(controller.startLoginFlowAutomatically(), !Account::enforcedServerUrl().isEmpty() && !controller.privateCloudMode());
    }

    void locksServerUrlFromManagedPolicy()
    {
        const auto restoreConfiguration = restoreOverrideState();
        setManagedOverrideServerUrl(SettingSourceType::PlatformPolicy, EnforcementState::Enforced, 200);

        AccountWizardController controller;

        QCOMPARE(controller.serverUrl(), allowedServerUrl().toString());
        QVERIFY(!controller.serverUrlEditable());
        QCOMPARE(controller.startLoginFlowAutomatically(), !controller.privateCloudMode());
    }

    void appliesOverrideLocalDirForNewAccount_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<bool>("existingAccount");
        QTest::addColumn<bool>("applies");
        QTest::addColumn<bool>("showsSyncOptions");

        QTest::newRow("policy, first account") << u"policy"_s << false << true << false;
        QTest::newRow("policy, additional account") << u"policy"_s << true << false << true;
        QTest::newRow("device default, first account") << u"default"_s << false << true << true;
        QTest::newRow("device default, additional account") << u"default"_s << true << false << true;
        QTest::newRow("command line, additional account") << u"commandLine"_s << true << true << false;
    }

    void appliesOverrideLocalDirForNewAccount()
    {
        QFETCH(QString, source);
        QFETCH(bool, existingAccount);
        QFETCH(bool, applies);
        QFETCH(bool, showsSyncOptions);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto restoreConfiguration = restoreOverrideState();
        ConfigFile::setDeviceSourcesFactory([source, localDir = dir.path()] {
            auto policyValues = QVariantMap{{u"overrideServerUrl"_s, u"https://cloud.example"_s}};
            if (source == u"policy"_s) {
                policyValues.insert(u"overrideLocalDir"_s, localDir);
            }
            std::vector<std::unique_ptr<SettingSource>> sources;
            sources.push_back(std::make_unique<MapSource>(SettingSourceType::PlatformPolicy, EnforcementState::Enforced, 200, policyValues));
            if (source == u"default"_s) {
                sources.push_back(std::make_unique<MapSource>(SettingSourceType::PlatformDefault,
                                                              EnforcementState::NotEnforced,
                                                              20,
                                                              QVariantMap{{u"overrideLocalDir"_s, localDir}}));
            }
            return sources;
        });
        if (source == u"commandLine"_s) {
            QVERIFY(ConfigFile().setOverrideLocalDir(dir.path()));
        }

        AccountState *accountState = nullptr;
        const auto removeAccount = qScopeGuard([&accountState] {
            if (accountState) {
                AccountManager::instance()->removeAccountState(accountState);
            }
        });
        if (existingAccount) {
            auto account = Account::create();
            QVERIFY(account->setUrl(allowedServerUrl()));
            account->setDavUser(u"first"_s);
            account->setCredentials(new FakeCredentials{new FakeQNAM({})});
            accountState = AccountManager::instance()->addAccount(account);
        }

        AccountWizardController controller;
        AccountWizardControllerTestAccess::setAccountUrl(controller, allowedServerUrl());
        AccountWizardControllerTestAccess::initialiseLocalSyncFolder(controller);

        const auto overrideApplies = applies && !controller.privateCloudMode();
        QCOMPARE(AccountWizardControllerTestAccess::localSyncFolderOverride(controller), overrideApplies);
        QCOMPARE(controller.localSyncFolder() == QDir::fromNativeSeparators(dir.path()), overrideApplies);
        if (controller.privateCloudMode()) {
            QCOMPARE(controller.localSyncFolder(), PrivateCloudDrive::defaultSetupPath());
        }

        if (controller.privateCloudMode()) {
            AccountWizardControllerTestAccess::completeAuthentication(controller);
        } else {
            AccountWizardControllerTestAccess::chooseSyncModeAfterCapabilities(controller);
        }
        if (!controller.needsSyncOptions()) {
            QSKIP("This build chooses no sync options, so the wizard always finishes.");
        }
        QCOMPARE(controller.currentStep() == AccountWizardController::SyncOptionsStep, controller.privateCloudMode() || showsSyncOptions);
    }

#ifdef Q_OS_LINUX
    void defaultSyncModeIsNotVirtualFilesOnLinux()
    {
        AccountWizardController controller;

        if (controller.canUseVirtualFiles()) {
            QCOMPARE(controller.syncMode(), AccountWizardController::SyncEverything);
        }
    }
#endif

#ifdef BUILD_FILE_PROVIDER_MODULE
    // Keep this test last: it redirects the configuration directory.
    void followsAppLevelFileProviderMode()
    {
        QTemporaryDir dir;
        ConfigFile::setConfDir(dir.path());
        QVERIFY(dir.isValid());

        ConfigFile cfg;
        cfg.setMacFileProviderModeEnabled(false);

        {
            AccountWizardController controller;
            QVERIFY(!controller.canUseVirtualFiles());
            QVERIFY(!controller.isUsingFileProvider());
            QVERIFY(controller.canUseClassicSync());
            QVERIFY(controller.localSyncFolderRequired());
        }

        cfg.setMacFileProviderModeEnabled(true);

        {
            AccountWizardController controller;
            QCOMPARE(controller.canUseVirtualFiles(), Mac::FileProvider::available());
            QCOMPARE(controller.isUsingFileProvider(), Mac::FileProvider::available());
            QCOMPARE(controller.localSyncFolderRequired(), !Mac::FileProvider::available());
        }
    }
#endif
};

QTEST_MAIN(TestAccountWizardController)
#include "testaccountwizardcontroller.moc"
