/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QWidget>

class QLabel;
class QAction;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace OCC
{

class PrivateCloudWindow : public QWidget
{
    Q_OBJECT
public:
    enum class Status {
        SignedOut,
        Connecting,
        Ready,
        Syncing,
        Paused,
        NeedsAttention,
        Offline,
        AuthenticationRequired,
        Checking,
        Conflicts,
    };
    Q_ENUM(Status)

    explicit PrivateCloudWindow(QWidget *parent = nullptr);
    void setBusy(bool busy);
    void setStatus(const QString &account, const QString &drive, Status status, const QString &detail, bool driveAvailable);
    [[nodiscard]] QString statusSummary() const;

Q_SIGNALS:
    void openDriveRequested();
    void retryRequested();
    void reviewFilesRequested();
    void signOutRequested();
    void changeAccountRequested();
    void quitRequested();

private:
    void updateActions();

    QLabel *_account;
    QLabel *_drive;
    QLabel *_status;
    QLabel *_detail;
    QPushButton *_openDrive;
    QPushButton *_retry;
    QAction *_signOut;
    QAction *_changeAccount;
    QToolButton *_detailsToggle;
    QWidget *_detailsPanel;
    QPlainTextEdit *_fullDetails;
    QToolButton *_copyDetails;
    Status _currentStatus = Status::SignedOut;
    bool _busy = false;
    bool _driveAvailable = false;
    QString _accountName;
    QString _diagnosticText;
};
}
