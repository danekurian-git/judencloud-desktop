/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudwindow.h"
#include "theme.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace OCC
{
namespace
{
constexpr int windowMinimumWidth = 460;
constexpr int windowInitialWidth = 520;
constexpr int windowInitialHeight = 340;
constexpr int horizontalMargin = 28;
constexpr int verticalMargin = 24;
constexpr int layoutSpacing = 16;
constexpr int titlePointSize = 22;
constexpr int statusPointSize = 13;
constexpr int brandIconSize = 48;
constexpr int primaryActionHeight = 44;
constexpr int diagnosticsHeight = 144;
}

PrivateCloudWindow::PrivateCloudWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("privateCloud"));
    setWindowIcon(Theme::instance()->applicationIcon());
    setObjectName(QStringLiteral("privateCloudWindow"));
    setMinimumWidth(windowMinimumWidth);
    resize(windowInitialWidth, windowInitialHeight);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(horizontalMargin, verticalMargin, horizontalMargin, verticalMargin);
    layout->setSpacing(layoutSpacing);
    auto *heading = new QHBoxLayout;
    auto *brandIcon = new QLabel(this);
    brandIcon->setObjectName(QStringLiteral("privateCloudLogo"));
    brandIcon->setPixmap(windowIcon().pixmap(brandIconSize, brandIconSize));
    heading->addWidget(brandIcon);
    auto *title = new QLabel(QStringLiteral("privateCloud"), this);
    auto titleFont = title->font();
    titleFont.setPointSize(titlePointSize);
    titleFont.setBold(true);
    title->setFont(titleFont);
    heading->addWidget(title);
    heading->addStretch();

    auto *options = new QToolButton(this);
    options->setObjectName(QStringLiteral("privateCloudOptions"));
    options->setText(QString(QChar(0x22ef)));
    options->setAccessibleName(tr("More options"));
    options->setToolTip(tr("More options"));
    options->setAutoRaise(true);
    options->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(options);
    menu->setObjectName(QStringLiteral("privateCloudOptionsMenu"));
    _signOut = menu->addAction(tr("Sign out"));
    _signOut->setObjectName(QStringLiteral("privateCloudSignOut"));
    _changeAccount = menu->addAction(tr("Change account"));
    _changeAccount->setObjectName(QStringLiteral("privateCloudChangeAccount"));
    menu->addSeparator();
    auto *quit = menu->addAction(tr("Quit privateCloud"));
    quit->setObjectName(QStringLiteral("privateCloudQuit"));
    options->setMenu(menu);
    heading->addWidget(options);
    layout->addLayout(heading);

    const auto makeLabel = [this](const QString &name) {
        auto *label = new QLabel(this);
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    _account = makeLabel(QStringLiteral("privateCloudAccount"));
    _drive = makeLabel(QStringLiteral("privateCloudDrive"));
    auto *details = new QFormLayout;
    details->setHorizontalSpacing(layoutSpacing);
    details->addRow(tr("Account"), _account);
    details->addRow(tr("Drive"), _drive);
    layout->addLayout(details);

    _status = makeLabel(QStringLiteral("privateCloudStatus"));
    auto statusFont = _status->font();
    statusFont.setBold(true);
    statusFont.setPointSize(statusPointSize);
    _status->setFont(statusFont);
    layout->addWidget(_status);
    _detail = makeLabel(QStringLiteral("privateCloudDetail"));
    layout->addWidget(_detail);
    layout->addStretch();

    auto *primaryActions = new QHBoxLayout;
    _openDrive = new QPushButton(this);
    _openDrive->setObjectName(QStringLiteral("privateCloudOpenDrive"));
    _openDrive->setMinimumHeight(primaryActionHeight);
    auto actionFont = _openDrive->font();
    actionFont.setBold(true);
    _openDrive->setFont(actionFont);
    _retry = new QPushButton(this);
    _retry->setObjectName(QStringLiteral("privateCloudRetry"));
    _retry->setMinimumHeight(primaryActionHeight);
    primaryActions->addWidget(_openDrive, 1);
    primaryActions->addWidget(_retry);
    layout->addLayout(primaryActions);

    _detailsToggle = new QToolButton(this);
    _detailsToggle->setObjectName(QStringLiteral("privateCloudDetailsToggle"));
    _detailsToggle->setText(tr("Details"));
    _detailsToggle->setCheckable(true);
    _detailsToggle->setAutoRaise(true);
    _detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _detailsToggle->setArrowType(Qt::RightArrow);
    layout->addWidget(_detailsToggle, 0, Qt::AlignLeft);
    _detailsPanel = new QWidget(this);
    _detailsPanel->setObjectName(QStringLiteral("privateCloudDetailsPanel"));
    auto *diagnosticsLayout = new QVBoxLayout(_detailsPanel);
    diagnosticsLayout->setContentsMargins(0, 0, 0, 0);
    _fullDetails = new QPlainTextEdit(_detailsPanel);
    _fullDetails->setObjectName(QStringLiteral("privateCloudFullDetails"));
    _fullDetails->setAccessibleName(tr("Details"));
    _fullDetails->setReadOnly(true);
    _fullDetails->setTabChangesFocus(true);
    _fullDetails->setFixedHeight(diagnosticsHeight);
    diagnosticsLayout->addWidget(_fullDetails);
    _copyDetails = new QToolButton(_detailsPanel);
    _copyDetails->setObjectName(QStringLiteral("privateCloudCopyDetails"));
    _copyDetails->setText(tr("Copy details"));
    diagnosticsLayout->addWidget(_copyDetails, 0, Qt::AlignRight);
    layout->addWidget(_detailsPanel);

    connect(_detailsToggle, &QToolButton::toggled, this, [this](bool expanded) {
        _detailsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        _detailsPanel->setVisible(expanded && !_diagnosticText.isEmpty());
    });
    connect(_copyDetails, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(_diagnosticText);
        _copyDetails->setText(tr("Copied"));
    });
    connect(_openDrive, &QPushButton::clicked, this, &PrivateCloudWindow::openDriveRequested);
    connect(_retry, &QPushButton::clicked, this, [this] {
        if (_currentStatus == Status::Conflicts) {
            Q_EMIT reviewFilesRequested();
        } else {
            Q_EMIT retryRequested();
        }
    });
    connect(_signOut, &QAction::triggered, this, &PrivateCloudWindow::signOutRequested);
    connect(_changeAccount, &QAction::triggered, this, &PrivateCloudWindow::changeAccountRequested);
    connect(quit, &QAction::triggered, this, &PrivateCloudWindow::quitRequested);
    setStatus({}, {}, Status::SignedOut, {}, false);
}

void PrivateCloudWindow::setBusy(bool busy)
{
    _busy = busy;
    updateActions();
}

QString PrivateCloudWindow::statusSummary() const
{
    return _status->text();
}

void PrivateCloudWindow::updateActions()
{
    const auto signedOut = _currentStatus == Status::SignedOut;
    const auto openEnabled = _driveAvailable && !_busy;
    _openDrive->setVisible(!signedOut || _driveAvailable);
    _openDrive->setEnabled(openEnabled);
    _openDrive->setDefault(openEnabled);

    QString action;
    switch (_currentStatus) {
    case Status::SignedOut:
    case Status::AuthenticationRequired:
        action = tr("Sign in");
        break;
    case Status::Offline:
        action = tr("Reconnect");
        break;
    case Status::Paused:
        action = tr("Resume");
        break;
    case Status::Conflicts:
        action = tr("Review files");
        break;
    case Status::NeedsAttention:
        action = tr("Retry");
        break;
    case Status::Connecting:
    case Status::Ready:
    case Status::Syncing:
    case Status::Checking:
        break;
    }
    const auto actionEnabled = !action.isEmpty() && !_busy && (_currentStatus != Status::Conflicts || _driveAvailable);
    _retry->setText(action);
    _retry->setVisible(!action.isEmpty());
    _retry->setEnabled(actionEnabled);
    _retry->setDefault(actionEnabled && !openEnabled);
    _signOut->setEnabled(!_accountName.isEmpty() && !signedOut && !_busy);
    _changeAccount->setEnabled(!_accountName.isEmpty() && !_busy);
}

void PrivateCloudWindow::setStatus(const QString &account, const QString &drive, Status status, const QString &detail, bool driveAvailable)
{
    const auto accountChanged = _accountName != account;
    _accountName = account;
    _currentStatus = status;
    _driveAvailable = driveAvailable;
    _account->setText(account.isEmpty() ? tr("Not signed in") : account);
    //: %1 is a Windows drive letter followed by a colon, for example J:.
    _drive->setText(drive.isEmpty() ? tr("Not connected") : tr("privateCloud (%1)").arg(drive.first(2)));
    //: %1 is a Windows drive letter followed by a colon, for example J:.
    _openDrive->setText(drive.isEmpty() ? tr("Open privateCloud") : tr("Open privateCloud (%1)").arg(drive.first(2)));
    QString description;
    switch (status) {
    case Status::SignedOut:
        _status->setText(tr("Signed out"));
        description = tr("Sign in to connect your privateCloud drive.");
        break;
    case Status::Connecting:
        _status->setText(tr("Connecting"));
        description = tr("Connecting to your account and preparing the drive.");
        break;
    case Status::Ready:
        _status->setText(tr("Ready"));
        description = tr("Your files are available in the drive. Files download when you open them.");
        break;
    case Status::Syncing:
        _status->setText(tr("Syncing"));
        description = tr("Updating your files. You can keep using the drive.");
        break;
    case Status::Paused:
        _status->setText(tr("Paused"));
        description = tr("Updates are paused. Select Resume to continue.");
        break;
    case Status::NeedsAttention:
        _status->setText(tr("Needs attention"));
        description = tr("The drive could not finish updating. Check the details below, then try again.");
        break;
    case Status::Offline:
        _status->setText(tr("Offline"));
        description = tr("Unable to connect. Files already on this computer can still be opened.");
        break;
    case Status::AuthenticationRequired:
        _status->setText(tr("Sign in required"));
        description = tr("Sign in again to reconnect your drive.");
        break;
    case Status::Checking:
        _status->setText(tr("Checking for changes"));
        description = tr("Looking for changes. You can keep using the drive.");
        break;
    case Status::Conflicts:
        _status->setText(tr("Files need review"));
        description = tr("Some files have conflicting changes. Both versions were kept in your drive.");
        break;
    }
    constexpr qsizetype maximumDetailLength = 320;
    auto conciseDetail = detail.section(u'\n', 0, 0).simplified();
    if (conciseDetail.size() > maximumDetailLength) {
        conciseDetail = conciseDetail.first(maximumDetailLength - 1) + QChar(0x2026);
    }
    _detail->setText(conciseDetail.isEmpty() ? description : conciseDetail);
    if (_diagnosticText != detail) {
        _diagnosticText = detail;
        _fullDetails->setPlainText(detail);
        _copyDetails->setText(tr("Copy details"));
    }
    const auto hasDetails = !detail.trimmed().isEmpty();
    if (!hasDetails || accountChanged) {
        _detailsToggle->setChecked(false);
    }
    _detailsToggle->setVisible(hasDetails);
    _detailsPanel->setVisible(hasDetails && _detailsToggle->isChecked());
    _copyDetails->setEnabled(hasDetails);
    updateActions();
}
}
