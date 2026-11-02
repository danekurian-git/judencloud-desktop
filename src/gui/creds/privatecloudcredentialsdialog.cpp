/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "privatecloudcredentialsdialog.h"
#include "account.h"
#include "creds/abstractcredentials.h"
#include "privatecloudauthentication.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

namespace OCC
{
PrivateCloudCredentialsDialog::PrivateCloudCredentialsDialog(Account *account)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Sign in to privateCloud"));
    auto *layout = new QFormLayout(this);
    auto *description = new QLabel(tr("Enter your username and password to reconnect your drive."), this);
    description->setWordWrap(true);
    layout->addRow(description);
    _user = new QLineEdit(account->credentials()->user(), this);
    _user->setObjectName(QStringLiteral("privateCloudUsername"));
    _user->setAccessibleName(tr("Username"));
    _password = new QLineEdit(this);
    _password->setObjectName(QStringLiteral("privateCloudPassword"));
    _password->setAccessibleName(tr("Password"));
    _password->setEchoMode(QLineEdit::Password);
    _user->installEventFilter(this);
    _password->installEventFilter(this);
    layout->addRow(tr("Username"), _user);
    auto *passwordRow = new QHBoxLayout;
    passwordRow->addWidget(_password);
    _showPassword = new QPushButton(tr("Show"), this);
    _showPassword->setObjectName(QStringLiteral("privateCloudShowPassword"));
    _showPassword->setAccessibleName(tr("Show password"));
    _showPassword->setCheckable(true);
    _showPassword->setAutoDefault(false);
    passwordRow->addWidget(_showPassword);
    layout->addRow(tr("Password"), passwordRow);
    auto *helpToggle = new QPushButton(tr("Use two-factor authentication?"), this);
    helpToggle->setObjectName(QStringLiteral("privateCloudPasswordHelpToggle"));
    helpToggle->setCheckable(true);
    helpToggle->setFlat(true);
    helpToggle->setAutoDefault(false);
    layout->addRow(helpToggle);
    auto *help = new QLabel(tr("Use an app password from your account's Security settings instead of your usual password."), this);
    help->setObjectName(QStringLiteral("privateCloudPasswordHelp"));
    help->setWordWrap(true);
    help->setVisible(false);
    layout->addRow(help);
    _status = new QLabel(this);
    _status->setObjectName(QStringLiteral("privateCloudSignInStatus"));
    _status->setWordWrap(true);
    _status->setTextFormat(Qt::PlainText);
    layout->addRow(_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    _submit = buttons->button(QDialogButtonBox::Ok);
    _submit->setText(tr("Sign in"));
    _submit->setObjectName(QStringLiteral("privateCloudSignIn"));
    _submit->setAutoDefault(false);
    _submit->setDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    layout->addRow(buttons);

    connect(_showPassword, &QPushButton::toggled, this, [this](bool visible) {
        _password->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        _showPassword->setText(visible ? tr("Hide") : tr("Show"));
        _showPassword->setAccessibleName(visible ? tr("Hide password") : tr("Show password"));
    });
    connect(helpToggle, &QPushButton::toggled, this, [this, help, helpToggle](bool expanded) {
        help->setVisible(expanded);
        helpToggle->setText(expanded ? tr("Hide sign-in help") : tr("Use two-factor authentication?"));
        adjustSize();
    });

    const auto candidate = Account::create();
    candidate->setUrl(account->url());
    candidate->setSslConfiguration(account->getOrCreateSslConfig());
    _authentication = new PrivateCloudAuthentication(candidate, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, expectedUser = account->davUser()] {
        if (!_submit->isEnabled()) {
            return;
        }
        if (_user->text().trimmed().isEmpty() || _password->text().isEmpty()) {
            _status->setText(tr("Enter both your username and password."));
            return;
        }
        setBusy(true);
        _status->setText(tr("Signing in…"));
        _authentication->start(_user->text(), _password->text(), expectedUser);
    });
    connect(_authentication, &PrivateCloudAuthentication::failed, this, [this](const QString &message) {
        setBusy(false);
        _password->clear();
        _status->setText(message);
        _password->setFocus();
    });
    connect(_authentication, &PrivateCloudAuthentication::succeeded, this, [this, candidate] {
        _password->clear();
        Q_EMIT credentialsReady(candidate->credentials()->user(), candidate->credentials()->password());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::rejected, _authentication, &PrivateCloudAuthentication::cancel);
}

bool PrivateCloudCredentialsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == _user || watched == _password) && event->type() == QEvent::KeyPress) {
        const auto keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && !(keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            // Consume the event before QDialogButtonBox's default button can
            // also submit the username field's Return key.
            if (watched == _user) {
                _password->setFocus();
            } else {
                _submit->click();
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void PrivateCloudCredentialsDialog::setBusy(bool busy)
{
    if (busy) {
        _showPassword->setChecked(false);
    }
    _user->setEnabled(!busy);
    _password->setEnabled(!busy);
    _showPassword->setEnabled(!busy);
    _submit->setEnabled(!busy);
}
}
