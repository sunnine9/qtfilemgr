// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrTrashDialog.h"

#include "QtFileMgrIO.h"
#include "QtFileMgrLocalIO.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QString cleanAbsolutePath(const QString &path)
{
    if (path.trimmed().isEmpty())
        return QString();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void appendTrashRoot(QStringList *roots, QSet<QString> *seen,
                     const QString &sourceRoot, const char *source)
{
    const QString root = cleanAbsolutePath(sourceRoot);
    if (root.isEmpty()) {
        qWarning("[TrashDialog] roots: skipped empty trash root (source=%s, value=\"%s\")",
                 source, qPrintable(sourceRoot));
        return;
    }
    if (seen->contains(root)) {
        qWarning("[TrashDialog] roots: skipped duplicate trash root (source=%s, root=\"%s\")",
                 source, qPrintable(root));
        return;
    }
    seen->insert(root);
    roots->append(root);
}

bool validSharedTrashDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    if (!info.isDir() || info.isSymLink()) {
        qWarning("[TrashDialog] roots: rejected unsafe shared trash directory (path=\"%s\", isDir=%d, symlink=%d)",
                 qPrintable(path), info.isDir() ? 1 : 0,
                 info.isSymLink() ? 1 : 0);
        return false;
    }
    struct stat status;
    const QByteArray encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &status) != 0
        || (status.st_mode & S_ISVTX) == 0) {
        qWarning("[TrashDialog] roots: rejected shared trash without sticky bit (path=\"%s\")",
                 qPrintable(path));
        return false;
    }
    return true;
}

bool validUserTrashDirectory(const QString &path, const char *source)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    if (!info.isDir() || info.isSymLink()
        || info.ownerId() != static_cast<uint>(::getuid())) {
        qWarning("[TrashDialog] roots: rejected unsafe user trash directory (source=%s, path=\"%s\", isDir=%d, symlink=%d, owner=%u, uid=%u)",
                 source, qPrintable(path), info.isDir() ? 1 : 0,
                 info.isSymLink() ? 1 : 0, info.ownerId(),
                 static_cast<uint>(::getuid()));
        return false;
    }
    return true;
}

QString relativePathBase(const QString &trashRoot)
{
    const QFileInfo rootInfo(trashRoot);
    const QString parent = rootInfo.absolutePath();
    if (rootInfo.fileName().startsWith(QStringLiteral(".Trash-")))
        return parent;
    const QFileInfo parentInfo(parent);
    if (parentInfo.fileName() == QStringLiteral(".Trash"))
        return parentInfo.absolutePath();
    return parent;
}

QString decodeOriginalPath(const QString &trashRoot,
                           const QString &encodedPath,
                           bool *valid)
{
    *valid = false;
    if (encodedPath.isEmpty())
        return QString();
    QString decoded = QUrl::fromPercentEncoding(encodedPath.toUtf8());
    if (decoded.isEmpty())
        return QString();
    decoded = QDir::fromNativeSeparators(decoded);
    if (!QDir::isAbsolutePath(decoded)) {
        const QStringList parts = decoded.split(QLatin1Char('/'),
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                                                Qt::KeepEmptyParts);
#else
                                                QString::KeepEmptyParts);
#endif
        if (parts.contains(QStringLiteral(".."))) {
            qWarning("[TrashDialog] entries: rejected relative metadata path containing '..' (root=\"%s\", path=\"%s\")",
                     qPrintable(trashRoot), qPrintable(decoded));
            return QString();
        }
        decoded = QDir(relativePathBase(trashRoot)).absoluteFilePath(decoded);
    }
    *valid = true;
    return QDir::cleanPath(decoded);
}

bool removeTrashItem(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile())
        return QFile::remove(path);
    if (info.isDir())
        return QDir(path).removeRecursively();
    qWarning("[TrashDialog] empty: skipped unresolved trash item (path=\"%s\")",
             qPrintable(path));
    return false;
}

} // namespace

QString QtFileMgrTrashDialog::trashRootPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/Trash");
}

QStringList QtFileMgrTrashDialog::trashRootPaths()
{
    QStringList roots;
    QSet<QString> seenRoots;
    const QString homeRoot = trashRootPath();
    if (!QFileInfo::exists(homeRoot)
        || validUserTrashDirectory(homeRoot, "home")) {
        appendTrashRoot(&roots, &seenRoots, homeRoot, "home");
    }

    const QString uid = QString::number(static_cast<qulonglong>(::getuid()));
    QSet<QString> seenMounts;
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isReady()) {
            qWarning("[TrashDialog] roots: skipped unavailable mount (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(storage.rootPath()));
            continue;
        }
        const QString topDirectory = cleanAbsolutePath(storage.rootPath());
        if (topDirectory.isEmpty()) {
            qWarning("[TrashDialog] roots: skipped mount without root (device=\"%s\")",
                     storage.device().constData());
            continue;
        }
        if (seenMounts.contains(topDirectory)) {
            qWarning("[TrashDialog] roots: skipped duplicate mount (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(topDirectory));
            continue;
        }
        seenMounts.insert(topDirectory);

        const QString sharedRoot = QDir(topDirectory).filePath(
            QStringLiteral(".Trash"));
        if (validSharedTrashDirectory(sharedRoot)) {
            const QString userRoot = QDir(sharedRoot).filePath(uid);
            if (validUserTrashDirectory(userRoot, "shared-mount"))
                appendTrashRoot(&roots, &seenRoots, userRoot, "shared-mount");
        }

        const QString privateRoot = QDir(topDirectory).filePath(
            QStringLiteral(".Trash-") + uid);
        const QFileInfo privateInfo(privateRoot);
        if (privateInfo.exists()) {
            if (validUserTrashDirectory(privateRoot, "private-mount")) {
                appendTrashRoot(&roots, &seenRoots, privateRoot,
                                "private-mount");
            }
        }
    }
    return roots;
}

QList<QtFileMgrTrashEntry> QtFileMgrTrashDialog::entries()
{
    return entriesForRoots(trashRootPaths());
}

QList<QtFileMgrTrashEntry> QtFileMgrTrashDialog::entriesForRoots(
    const QStringList &trashRoots)
{
    QList<QtFileMgrTrashEntry> result;
    if (trashRoots.isEmpty()) {
        qWarning("[TrashDialog] entries: no trash roots supplied");
        return result;
    }

    QSet<QString> seenRoots;
    for (const QString &sourceRoot : trashRoots) {
        const QString root = cleanAbsolutePath(sourceRoot);
        if (root.isEmpty()) {
            qWarning("[TrashDialog] entries: skipped empty trash root (source=\"%s\")",
                     qPrintable(sourceRoot));
            continue;
        }
        if (seenRoots.contains(root)) {
            qWarning("[TrashDialog] entries: skipped duplicate trash root (root=\"%s\")",
                     qPrintable(root));
            continue;
        }
        seenRoots.insert(root);
        const QFileInfo rootInfo(root);
        if (rootInfo.isSymLink()) {
            qWarning("[TrashDialog] entries: rejected symbolic-link trash root (root=\"%s\")",
                     qPrintable(root));
            continue;
        }
        // files/info 子目录也必须是真实目录: 链接会把枚举引向站外 (审查#6)
        const QFileInfo filesLinkInfo(QDir(root).filePath(QStringLiteral("files")));
        const QFileInfo infoLinkInfo(QDir(root).filePath(QStringLiteral("info")));
        if (filesLinkInfo.isSymLink() || infoLinkInfo.isSymLink()) {
            qWarning("[TrashDialog] entries: rejected trash root with symbolic-link subdirectory (root=\"%s\", filesLink=%d, infoLink=%d)",
                     qPrintable(root), filesLinkInfo.isSymLink() ? 1 : 0,
                     infoLinkInfo.isSymLink() ? 1 : 0);
            continue;
        }

        const QDir filesDir(QDir(root).filePath(QStringLiteral("files")));
        const QDir infoDir(QDir(root).filePath(QStringLiteral("info")));
        if (!filesDir.exists())
            continue;
        const QFileInfoList files = filesDir.entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot,
            QDir::Time);
        QSet<QString> fileNames;
        for (const QFileInfo &file : files) {
            fileNames.insert(file.fileName());
            QtFileMgrTrashEntry entry;
            entry.name = file.fileName();
            entry.trashedPath = file.absoluteFilePath();
            entry.trashRoot = root;
            entry.infoPath = infoDir.filePath(
                file.fileName() + QStringLiteral(".trashinfo"));
            entry.size = file.isDir() ? 0 : file.size();
            entry.isDir = file.isDir();

            const QFileInfo metadata(entry.infoPath);
            if (!metadata.isFile() || !metadata.isReadable()) {
                qWarning("[TrashDialog] entries: trash item has no readable metadata (root=\"%s\", item=\"%s\", info=\"%s\")",
                         qPrintable(root), qPrintable(entry.trashedPath),
                         qPrintable(entry.infoPath));
                result.append(entry);
                continue;
            }

            QSettings info(entry.infoPath, QSettings::IniFormat);
            info.beginGroup(QStringLiteral("Trash Info"));
            const QString encodedPath = info.value(
                QStringLiteral("Path")).toString();
            entry.deletionDate = info.value(
                QStringLiteral("DeletionDate")).toString();
            info.endGroup();
            bool pathValid = false;
            entry.originalPath = decodeOriginalPath(
                root, encodedPath, &pathValid);
            entry.metadataValid = pathValid && !entry.deletionDate.isEmpty();
            if (!pathValid || entry.deletionDate.isEmpty()) {
                qWarning("[TrashDialog] entries: incomplete trash metadata (root=\"%s\", item=\"%s\", encodedPath=\"%s\", deletionDate=\"%s\")",
                         qPrintable(root), qPrintable(entry.trashedPath),
                         qPrintable(encodedPath), qPrintable(entry.deletionDate));
            }
            result.append(entry);
        }

        if (infoDir.exists()) {
            const QFileInfoList metadataFiles = infoDir.entryInfoList(
                QStringList{QStringLiteral("*.trashinfo")},
                QDir::Files | QDir::Hidden | QDir::System);
            for (const QFileInfo &metadata : metadataFiles) {
                QString itemName = metadata.fileName();
                itemName.chop(QStringLiteral(".trashinfo").size());
                if (!fileNames.contains(itemName)) {
                    qWarning("[TrashDialog] entries: orphan metadata has no trash item (root=\"%s\", info=\"%s\")",
                             qPrintable(root),
                             qPrintable(metadata.absoluteFilePath()));
                }
            }
        }
    }

    std::sort(result.begin(), result.end(),
              [](const QtFileMgrTrashEntry &lhs,
                 const QtFileMgrTrashEntry &rhs) {
        if (lhs.deletionDate == rhs.deletionDate)
            return lhs.name < rhs.name;
        return lhs.deletionDate > rhs.deletionDate;
    });
    return result;
}

bool QtFileMgrTrashDialog::restore(
    const QtFileMgrTrashEntry &entry, QString *error)
{
    if (entry.trashedPath.isEmpty() || entry.originalPath.isEmpty()) {
        qWarning("[TrashDialog] restore: rejected incomplete entry (name=\"%s\", trashed=\"%s\", original=\"%s\", root=\"%s\")",
                 qPrintable(entry.name), qPrintable(entry.trashedPath),
                 qPrintable(entry.originalPath), qPrintable(entry.trashRoot));
        if (error)
            *error = QStringLiteral("回收站元数据不完整");
        return false;
    }
    if (!entry.metadataValid) {
        qWarning("[TrashDialog] restore: restoring with incomplete non-path metadata (trashed=\"%s\", original=\"%s\", deletionDate=\"%s\")",
                 qPrintable(entry.trashedPath), qPrintable(entry.originalPath),
                 qPrintable(entry.deletionDate));
    }
    const QFileInfo source(entry.trashedPath);
    if (!source.exists() && !source.isSymLink()) {
        qWarning("[TrashDialog] restore: source no longer exists (trashed=\"%s\", original=\"%s\")",
                 qPrintable(entry.trashedPath), qPrintable(entry.originalPath));
        if (error)
            *error = QStringLiteral("回收站项目不存在: %1").arg(entry.name);
        return false;
    }
    const QFileInfo destination(entry.originalPath);
    if (destination.exists() || destination.isSymLink()) {
        qWarning("[TrashDialog] restore: destination already exists (trashed=\"%s\", original=\"%s\")",
                 qPrintable(entry.trashedPath), qPrintable(entry.originalPath));
        if (error)
            *error = QStringLiteral("原位置已有同名项目: %1").arg(entry.originalPath);
        return false;
    }
    const QString parentPath = QFileInfo(entry.originalPath).absolutePath();
    if (!QDir().mkpath(parentPath)) {
        qWarning("[TrashDialog] restore: cannot create destination parent (trashed=\"%s\", parent=\"%s\")",
                 qPrintable(entry.trashedPath), qPrintable(parentPath));
        if (error)
            *error = QStringLiteral("无法创建原目录: %1").arg(parentPath);
        return false;
    }
    QtFileMgrLocalIO localIO;
    if (!localIO.rename(entry.trashedPath, entry.originalPath)) {
        qWarning("[TrashDialog] restore: move failed (trashed=\"%s\", original=\"%s\")",
                 qPrintable(entry.trashedPath), qPrintable(entry.originalPath));
        if (error)
            *error = QStringLiteral("无法还原到: %1").arg(entry.originalPath);
        return false;
    }

    QString infoPath = entry.infoPath;
    if (infoPath.isEmpty() && !entry.trashRoot.isEmpty()) {
        infoPath = QDir(entry.trashRoot).filePath(
            QStringLiteral("info/") + entry.name
            + QStringLiteral(".trashinfo"));
        qWarning("[TrashDialog] restore: using derived metadata path (root=\"%s\", name=\"%s\", info=\"%s\")",
                 qPrintable(entry.trashRoot), qPrintable(entry.name),
                 qPrintable(infoPath));
    }
    if (!infoPath.isEmpty() && QFileInfo::exists(infoPath)
        && !QFile::remove(infoPath)) {
        qWarning("[TrashDialog] restore: restored item but failed to remove metadata (original=\"%s\", info=\"%s\")",
                 qPrintable(entry.originalPath), qPrintable(infoPath));
    }
    return true;
}

bool QtFileMgrTrashDialog::emptyTrash(QString *error)
{
    return emptyTrashRoots(trashRootPaths(), error);
}

bool QtFileMgrTrashDialog::emptyTrashRoots(
    const QStringList &trashRoots, QString *error)
{
    if (trashRoots.isEmpty()) {
        qWarning("[TrashDialog] empty: no trash roots supplied");
        if (error)
            *error = QStringLiteral("没有可清空的回收站位置");
        return false;
    }
    QStringList failedPaths;
    QSet<QString> seenRoots;
    for (const QString &sourceRoot : trashRoots) {
        const QString root = cleanAbsolutePath(sourceRoot);
        if (root.isEmpty()) {
            qWarning("[TrashDialog] empty: skipped empty trash root (source=\"%s\")",
                     qPrintable(sourceRoot));
            continue;
        }
        if (seenRoots.contains(root)) {
            qWarning("[TrashDialog] empty: skipped duplicate trash root (root=\"%s\")",
                     qPrintable(root));
            continue;
        }
        seenRoots.insert(root);
        if (QFileInfo(root).isSymLink()) {
            qWarning("[TrashDialog] empty: rejected symbolic-link trash root (root=\"%s\")",
                     qPrintable(root));
            failedPaths.append(root);
            continue;
        }
        // 变更前完整预检两个子目录: 任一是链接则整个根不动 (R2-12)
        const QString filesPath = QDir(root).filePath(QStringLiteral("files"));
        const QString infoPath = QDir(root).filePath(QStringLiteral("info"));
        if (QFileInfo(filesPath).isSymLink() || QFileInfo(infoPath).isSymLink()) {
            qWarning("[TrashDialog] empty: rejected trash root with symbolic-link subdirectory (root=\"%s\", filesLink=%d, infoLink=%d)",
                     qPrintable(root), QFileInfo(filesPath).isSymLink() ? 1 : 0,
                     QFileInfo(infoPath).isSymLink() ? 1 : 0);
            failedPaths.append(root);
            continue;
        }
        const QDir filesDir(filesPath);
        const QDir infoDir(infoPath);
        QSet<QString> pairedPayloads;
        // payload 与元数据按名配对: payload 删不掉则保留其 .trashinfo (R2-12)
        if (infoDir.exists()) {
            const QFileInfoList infos = infoDir.entryInfoList(
                QStringList{QStringLiteral("*.trashinfo")},
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
            for (const QFileInfo &infoItem : infos) {
                const QString payloadPath =
                    filesDir.filePath(infoItem.completeBaseName());
                if (!QFileInfo::exists(payloadPath)) {
                    // 无 payload 的孤儿元数据可安全删除
                    if (!QFile::remove(infoItem.absoluteFilePath())) {
                        qWarning("[TrashDialog] empty: failed to remove orphan metadata (root=\"%s\", path=\"%s\")",
                                 qPrintable(root),
                                 qPrintable(infoItem.absoluteFilePath()));
                        failedPaths.append(infoItem.absoluteFilePath());
                    }
                    continue;
                }
                pairedPayloads.insert(payloadPath);
                if (!removeTrashItem(payloadPath)) {
                    qWarning("[TrashDialog] empty: payload removal failed, metadata kept (root=\"%s\", payload=\"%s\")",
                             qPrintable(root), qPrintable(payloadPath));
                    failedPaths.append(payloadPath);
                    continue;
                }
                if (!QFile::remove(infoItem.absoluteFilePath())) {
                    qWarning("[TrashDialog] empty: failed to remove metadata (root=\"%s\", path=\"%s\")",
                             qPrintable(root),
                             qPrintable(infoItem.absoluteFilePath()));
                    failedPaths.append(infoItem.absoluteFilePath());
                }
            }
        }
        // 无元数据的孤儿 payload 仍要清掉
        if (filesDir.exists()) {
            const QFileInfoList items = filesDir.entryInfoList(
                QDir::AllEntries | QDir::Hidden | QDir::System
                    | QDir::NoDotAndDotDot);
            for (const QFileInfo &item : items) {
                if (pairedPayloads.contains(item.absoluteFilePath()))
                    continue;
                if (!removeTrashItem(item.absoluteFilePath())) {
                    qWarning("[TrashDialog] empty: failed to remove item (root=\"%s\", path=\"%s\")",
                             qPrintable(root),
                             qPrintable(item.absoluteFilePath()));
                    failedPaths.append(item.absoluteFilePath());
                }
            }
        }
    }
    if (!failedPaths.isEmpty()) {
        if (error) {
            *error = QStringLiteral("%1 个回收站项目无法删除")
                         .arg(failedPaths.size());
        }
        return false;
    }
    return true;
}

QtFileMgrTrashDialog::QtFileMgrTrashDialog(QWidget *parent)
    : QtFileMgrTrashDialog(QStringList(), parent)
{
}

QtFileMgrTrashDialog::QtFileMgrTrashDialog(
    const QStringList &trashRoots, QWidget *parent)
    : QDialog(parent)
    , m_items(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_trashRoots(trashRoots)
{
    setObjectName(QStringLiteral("qtFileMgrTrashDialog"));
    setWindowTitle(QStringLiteral("回收站"));
    resize(820, 470);
    auto *layout = new QVBoxLayout(this);
    m_items->setObjectName(QStringLiteral("trashItems"));
    m_items->setHeaderLabels(
        {QStringLiteral("名称"), QStringLiteral("原位置"),
         QStringLiteral("删除时间"), QStringLiteral("大小"),
         QStringLiteral("回收站位置")});
    m_items->setRootIsDecorated(false);
    m_items->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_items->setAlternatingRowColors(true);
    m_items->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_items->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_items->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_items->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_items->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(m_items, 1);
    m_status->setObjectName(QStringLiteral("trashStatus"));
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    m_restoreButton = buttons->addButton(
        QStringLiteral("还原"), QDialogButtonBox::ActionRole);
    m_restoreButton->setObjectName(QStringLiteral("restoreTrashButton"));
    m_emptyButton = buttons->addButton(
        QStringLiteral("清空回收站"), QDialogButtonBox::DestructiveRole);
    m_emptyButton->setObjectName(QStringLiteral("emptyTrashButton"));
    connect(m_restoreButton, &QPushButton::clicked,
            this, &QtFileMgrTrashDialog::restoreSelected);
    connect(m_emptyButton, &QPushButton::clicked,
            this, &QtFileMgrTrashDialog::empty);
    connect(m_items, &QTreeWidget::itemSelectionChanged, this, [this]() {
        m_restoreButton->setEnabled(!m_items->selectedItems().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    reload();
}

int QtFileMgrTrashDialog::entryCount() const
{
    return m_items ? m_items->topLevelItemCount() : 0;
}

void QtFileMgrTrashDialog::reload()
{
    m_items->clear();
    const QList<QtFileMgrTrashEntry> trashEntries = m_trashRoots.isEmpty()
        ? entries() : entriesForRoots(m_trashRoots);
    qint64 totalFileSize = 0;
    for (const QtFileMgrTrashEntry &entry : trashEntries) {
        auto *item = new QTreeWidgetItem(m_items);
        item->setText(0, entry.name);
        item->setText(1, entry.originalPath.isEmpty()
                             ? QStringLiteral("元数据缺失")
                             : entry.originalPath);
        item->setText(2, entry.deletionDate.isEmpty()
                             ? QStringLiteral("-") : entry.deletionDate);
        item->setText(3, entry.isDir ? QStringLiteral("文件夹")
                                    : formatFileSize(entry.size));
        item->setText(4, entry.trashRoot);
        item->setData(0, Qt::UserRole, QVariant::fromValue(entry));
        if (!entry.metadataValid)
            item->setToolTip(0, QStringLiteral("回收站元数据不完整，可能无法还原"));
        if (!entry.isDir && entry.size > 0)
            totalFileSize += entry.size;
    }
    QString status = QStringLiteral("%1 个项目").arg(trashEntries.size());
    if (totalFileSize > 0)
        status += QStringLiteral("，文件合计 %1").arg(formatFileSize(totalFileSize));
    m_status->setText(status);
    m_restoreButton->setEnabled(false);
    m_emptyButton->setEnabled(!trashEntries.isEmpty());
}

void QtFileMgrTrashDialog::restoreSelected()
{
    const QList<QTreeWidgetItem *> selected = m_items->selectedItems();
    if (selected.isEmpty()) {
        qWarning("[TrashDialog] restoreSelected: no selected trash entries");
        return;
    }
    QStringList errors;
    int restored = 0;
    for (QTreeWidgetItem *item : selected) {
        const QtFileMgrTrashEntry entry =
            item->data(0, Qt::UserRole).value<QtFileMgrTrashEntry>();
        QString error;
        if (!restore(entry, &error)) {
            qWarning("[TrashDialog] restoreSelected: restore failed (trashed=\"%s\", original=\"%s\", error=\"%s\")",
                     qPrintable(entry.trashedPath),
                     qPrintable(entry.originalPath), qPrintable(error));
            errors.append(error);
        } else {
            ++restored;
            emit entryRestored(entry.originalPath);
        }
    }
    if (restored > 0)
        emit trashChanged();
    reload();
    if (!errors.isEmpty())
        QMessageBox::warning(this, QStringLiteral("还原失败"),
                             errors.join(QLatin1Char('\n')));
}

void QtFileMgrTrashDialog::empty()
{
    if (QMessageBox::warning(
            this,
            QStringLiteral("清空回收站"),
            QStringLiteral("所有挂载位置回收站中的项目将被永久删除，且无法撤销。"),
            QMessageBox::Cancel | QMessageBox::Ok,
            QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }
    QString error;
    const bool emptied = m_trashRoots.isEmpty()
        ? emptyTrash(&error) : emptyTrashRoots(m_trashRoots, &error);
    if (!emptied) {
        QMessageBox::warning(this, QStringLiteral("清空失败"), error);
    } else {
        emit trashChanged();
    }
    reload();
}
