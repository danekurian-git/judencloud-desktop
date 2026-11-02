/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "privatecloudissuesdialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>

namespace OCC
{
PrivateCloudIssuesDialog::PrivateCloudIssuesDialog(const QString &rootPath, const QStringList &relativePaths, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Files needing review"));
    setObjectName(QStringLiteral("privateCloudIssuesDialog"));
    setModal(false);
    constexpr auto initialWidth = 540;
    constexpr auto initialHeight = 320;
    resize(initialWidth, initialHeight);
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(tr("Select a file to review it in your drive."), this);
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *files = new QListWidget(this);
    files->setObjectName(QStringLiteral("privateCloudIssuesFiles"));
    files->setAccessibleName(tr("Files needing review"));
    files->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(files);

    const auto root = QDir::cleanPath(QDir::fromNativeSeparators(rootPath));
    QSet<QString> addedPaths;
    if (QDir::isAbsolutePath(root) && !root.contains(QChar::Null)) {
        for (const auto &file : relativePaths) {
            const auto relative = QString(file).replace(QLatin1Char('\\'), QLatin1Char('/'));
            if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.startsWith(QLatin1Char('/')) || relative.contains(QLatin1Char(':'))
                || relative.contains(QChar::Null)) {
                continue;
            }
            const auto parts = relative.split(QLatin1Char('/'));
            const auto unsafePart = std::any_of(parts.cbegin(), parts.cend(), [](const QString &part) {
                return part == QStringLiteral("..") || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '));
            });
            if (unsafePart) {
                continue;
            }
            const auto cleanRelative = QDir::cleanPath(relative);
            if (cleanRelative == QStringLiteral(".") || addedPaths.contains(cleanRelative)) {
                continue;
            }
            addedPaths.insert(cleanRelative);
            auto *item = new QListWidgetItem(cleanRelative, files);
            item->setData(Qt::UserRole, QDir(root).filePath(cleanRelative));
        }
    }
    if (files->count() == 0) {
        description->setText(tr("No affected files are available to show."));
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *reveal = buttons->addButton(tr("Show in folder"), QDialogButtonBox::ActionRole);
    reveal->setObjectName(QStringLiteral("privateCloudShowIssue"));
    reveal->setEnabled(false);
    layout->addWidget(buttons);
    const auto revealItem = [this](const QListWidgetItem *item) {
        if (item && !item->data(Qt::UserRole).toString().isEmpty()) {
            Q_EMIT revealRequested(item->data(Qt::UserRole).toString());
        }
    };
    connect(files, &QListWidget::currentItemChanged, this, [reveal](const QListWidgetItem *item) {
        reveal->setEnabled(item && !item->data(Qt::UserRole).toString().isEmpty());
    });
    connect(files, &QListWidget::itemActivated, this, revealItem);
    connect(reveal, &QPushButton::clicked, this, [files, revealItem] {
        revealItem(files->currentItem());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
}
