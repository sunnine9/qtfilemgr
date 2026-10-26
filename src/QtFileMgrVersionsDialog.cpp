// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrVersionsDialog.h"

#include "QtFileMgrIO.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QDebug>

namespace {
const int VersionRole = Qt::UserRole;
}

QtFileMgrVersionsDialog::QtFileMgrVersionsDialog(
    QtFileMgrIO *io, const QString &path, QWidget *parent)
    : QDialog(parent)
    , m_io(io)
    , m_path(path)
    , m_versions(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_restoreButton(nullptr)
{
    setObjectName(QStringLiteral("qtFileMgrVersionsDialog"));
    setWindowTitle(QStringLiteral("版本历史 - %1")
                       .arg(QFileInfo(path).fileName()));
    resize(720, 430);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_versions->setObjectName(QStringLiteral("versionsTree"));
    m_versions->setColumnCount(5);
    m_versions->setHeaderLabels({QStringLiteral("版本"),
                                  QStringLiteral("时间"),
                                  QStringLiteral("大小"),
                                  QStringLiteral("作者"),
                                  QStringLiteral("说明")});
    m_versions->setRootIsDecorated(false);
    m_versions->setSelectionMode(QAbstractItemView::SingleSelection);
    m_versions->setAlternatingRowColors(true);
    m_versions->header()->setStretchLastSection(true);
    m_versions->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versions->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_versions->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_versions->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(m_versions, 1);

    m_status->setObjectName(QStringLiteral("versionsStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    m_restoreButton = buttons->addButton(QStringLiteral("恢复此版本"),
                                         QDialogButtonBox::ActionRole);
    m_restoreButton->setObjectName(QStringLiteral("restoreVersionButton"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_restoreButton, &QPushButton::clicked,
            this, &QtFileMgrVersionsDialog::restoreSelectedVersion);
    connect(m_versions, &QTreeWidget::itemSelectionChanged,
            this, &QtFileMgrVersionsDialog::updateRestoreButton);
    layout->addWidget(buttons);

    if (!m_io) {
        qWarning("[VersionsDialog] init: missing backend (path=\"%s\")",
                 qPrintable(m_path));
        setBusy(false, QStringLiteral("文件后端不可用"));
        return;
    }
    if (m_path.isEmpty()) {
        qWarning("[VersionsDialog] init: rejected empty file path (backend=%s)",
                 m_io->metaObject()->className());
        setBusy(false, QStringLiteral("文件路径无效"));
        return;
    }
    reloadVersions();
}

int QtFileMgrVersionsDialog::versionCount() const
{
    return m_versions->topLevelItemCount();
}

int QtFileMgrVersionsDialog::selectedVersion() const
{
    QTreeWidgetItem *item = m_versions->currentItem();
    return item ? item->data(0, VersionRole).toInt() : -1;
}

void QtFileMgrVersionsDialog::setBusy(bool busy, const QString &status)
{
    m_busy = busy;
    m_versions->setEnabled(!busy);
    m_status->setText(status);
    updateRestoreButton();
}

void QtFileMgrVersionsDialog::updateRestoreButton()
{
    const bool canRestore = m_io
        && m_io->supportsCapability(QtFileMgrIO::VersionRestoreCapability);
    m_restoreButton->setVisible(canRestore);
    m_restoreButton->setEnabled(canRestore && !m_busy
                                && m_versions->currentItem());
}

void QtFileMgrVersionsDialog::reloadVersions()
{
    reloadVersions(QString());
}

void QtFileMgrVersionsDialog::reloadVersions(const QString &completionStatus)
{
    if (!m_io || m_path.isEmpty()) {
        qWarning("[VersionsDialog] reloadVersions: skipped invalid request (backend=%s, path=\"%s\")",
                 m_io ? m_io->metaObject()->className() : "<null>",
                 qPrintable(m_path));
        return;
    }
    m_versions->clear();
    setBusy(true, QStringLiteral("正在读取版本历史..."));
    const QPointer<QtFileMgrIO> requestedIO = m_io;
    m_io->versionsAsync(m_path, this,
                        [this, requestedIO, completionStatus](const QList<VersionInfo> &versions,
                                            const QString &error) {
        if (!m_io || m_io != requestedIO) {
            qWarning("[VersionsDialog] reloadVersions: ignored stale result (path=\"%s\")",
                     qPrintable(m_path));
            return;
        }
        if (!error.isEmpty()) {
            qWarning("[VersionsDialog] reloadVersions: backend query failed (backend=%s, path=\"%s\", error=\"%s\")",
                     m_io->metaObject()->className(), qPrintable(m_path),
                     qPrintable(error));
            setBusy(false, error);
            return;
        }
        for (const VersionInfo &version : versions) {
            auto *item = new QTreeWidgetItem(m_versions);
            item->setText(0, QString::number(version.version));
            item->setData(0, VersionRole, version.version);
            item->setText(1, version.timestamp.isValid()
                              ? version.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              : QStringLiteral("-"));
            item->setText(2, version.size >= 0
                              ? formatFileSize(version.size)
                              : QStringLiteral("-"));
            item->setText(3, version.author);
            item->setText(4, version.comment);
        }
        if (m_versions->topLevelItemCount() > 0)
            m_versions->setCurrentItem(m_versions->topLevelItem(0));
        setBusy(false, !completionStatus.isEmpty()
                ? completionStatus
                : (versions.isEmpty()
                   ? QStringLiteral("没有可用的历史版本")
                   : QStringLiteral("共 %1 个历史版本").arg(versions.size())));
    });
}

void QtFileMgrVersionsDialog::restoreSelectedVersion()
{
    const int version = selectedVersion();
    if (version < 0) {
        qWarning("[VersionsDialog] restoreSelectedVersion: no selected version (path=\"%s\")",
                 qPrintable(m_path));
        return;
    }
    restoreVersion(version, true);
}

void QtFileMgrVersionsDialog::restoreVersion(int version,
                                             bool askConfirmation)
{
    if (!m_io || m_busy || m_path.isEmpty()) {
        qWarning("[VersionsDialog] restoreVersion: skipped invalid request (backend=%s, path=\"%s\", version=%d, busy=%d)",
                 m_io ? m_io->metaObject()->className() : "<null>",
                 qPrintable(m_path), version, m_busy ? 1 : 0);
        return;
    }
    if (!m_io->supportsCapability(QtFileMgrIO::VersionRestoreCapability)) {
        qWarning("[VersionsDialog] restoreVersion: backend does not declare restore capability (backend=%s, path=\"%s\", version=%d)",
                 m_io->metaObject()->className(), qPrintable(m_path), version);
        return;
    }
    if (askConfirmation
        && QMessageBox::question(this, QStringLiteral("恢复版本"),
             QStringLiteral("确定将当前文件恢复到版本 %1？").arg(version),
             QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes) {
        return;
    }

    setBusy(true, QStringLiteral("正在恢复版本 %1...").arg(version));
    const QPointer<QtFileMgrIO> requestedIO = m_io;
    m_io->restoreVersionAsync(m_path, version, this,
                              [this, requestedIO, version](bool success,
                                                           const QString &error) {
        if (!m_io || m_io != requestedIO) {
            qWarning("[VersionsDialog] restoreVersion: ignored stale result (path=\"%s\", version=%d)",
                     qPrintable(m_path), version);
            return;
        }
        if (!success) {
            const QString detail = error.isEmpty()
                ? QStringLiteral("恢复版本失败") : error;
            qWarning("[VersionsDialog] restoreVersion: backend restore failed (backend=%s, path=\"%s\", version=%d, error=\"%s\")",
                     m_io->metaObject()->className(), qPrintable(m_path), version,
                     qPrintable(detail));
            setBusy(false, detail);
            return;
        }
        emit versionRestored(m_path, version);
        reloadVersions(QStringLiteral("已恢复到版本 %1").arg(version));
    });
}
