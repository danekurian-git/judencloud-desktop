/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;
class QPushButton;

namespace OCC
{
class Account;
class PrivateCloudAuthentication;

class PrivateCloudCredentialsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PrivateCloudCredentialsDialog(Account *account);

Q_SIGNALS:
    void credentialsReady(const QString &user, const QString &password);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setBusy(bool busy);
    QLineEdit *_user = nullptr;
    QLineEdit *_password = nullptr;
    QLabel *_status = nullptr;
    QPushButton *_submit = nullptr;
    QPushButton *_showPassword = nullptr;
    PrivateCloudAuthentication *_authentication = nullptr;
};
}
