/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QDialog>
#include <QStringList>

namespace OCC
{
class PrivateCloudIssuesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PrivateCloudIssuesDialog(const QString &rootPath, const QStringList &relativePaths, QWidget *parent = nullptr);

Q_SIGNALS:
    void revealRequested(const QString &absolutePath);
};
}
