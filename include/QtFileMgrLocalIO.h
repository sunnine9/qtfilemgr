// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRLOCALIO_H
#define QTFILEMGRLOCALIO_H

#include "QtFileMgrIO.h"
#include <QFileIconProvider>

/// 本地文件系统 I/O 实现 — 所有操作委托给 QFile / QDir
class QtFileMgrLocalIO : public QtFileMgrIO {
    Q_OBJECT
public:
    enum TransferStatus {
        TransferSucceeded,
        TransferFailed,
        TransferCancelled
    };
    using CancelCheck = std::function<bool()>;
    using ByteProgressCallback = std::function<void(qint64, qint64)>;

    explicit QtFileMgrLocalIO(QObject *parent = nullptr);

    /// 线程无关的本地文件系统传输执行器。调用方可在工作线程中使用，
    /// 实现不会访问 QtFileMgrLocalIO QObject 实例。
    static TransferStatus copyCancellable(
        const QString &src, const QString &dest,
        const CancelCheck &isCancelled = CancelCheck(),
        const ByteProgressCallback &progress = ByteProgressCallback(),
        QString *error = nullptr);
    static TransferStatus moveCancellable(
        const QString &src, const QString &dest,
        const CancelCheck &isCancelled = CancelCheck(),
        const ByteProgressCallback &progress = ByteProgressCallback(),
        QString *error = nullptr);

    // [纯虚] 核心操作
    QString rootPath() const override;
    QString displayName(const QString &path) const override;
    bool exists(const QString &path) const override;
    QList<FileEntry> listDir(const QString &path) const override;
    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const override;
    bool isDir(const QString &path) const override;
    bool isWritable(const QString &path) const override;
    bool mkdir(const QString &path) override;
    bool rename(const QString &oldPath, const QString &newPath) override;
    bool renameNoReplace(const QString &source, const QString &destination,
                         QString *error = nullptr) override;
    static QByteArray captureUndoSnapshot(const QString &path, QString *error = nullptr);
    /// Compare complete proofs around a known rename. Only the root ctime may
    /// differ; identity, contents, metadata and all descendant revisions match.
    static bool undoSnapshotsMatchAfterRename(const QByteArray &before,
                                              const QByteArray &after);
    QByteArray undoSnapshot(const QString &path, QString *error = nullptr) const override;
    bool undoCopyIfUnchanged(const QString &destination, const QByteArray &snapshot,
                             QString *error = nullptr, QByteArray *refreshedSnapshot = nullptr) override;
    bool undoMoveIfUnchanged(const QString &destination, const QString &source,
                             const QByteArray &snapshot, QString *error = nullptr,
                             QByteArray *refreshedSnapshot = nullptr) override;
    bool remove(const QString &path) override;
    bool moveToTrash(const QString &path, QString *error = nullptr) override;
    bool copy(const QString &src, const QString &dest) override;
    bool writeFile(const QString &path, const QByteArray &contents) override;
    QByteArray readPreview(const QString &path, qint64 maxBytes,
                           bool *truncated = nullptr,
                           QString *error = nullptr) const override;
    bool canWrite() const override { return true; }

    // [可选] 元数据
    FileEntry stat(const QString &path) const override;
    qint64 fileSize(const QString &path) const override;
    QDateTime modificationTime(const QString &path) const override;
    QString mimeType(const QString &path) const override;
    QFile::Permissions permissions(const QString &path) const override;
    QString owner(const QString &path) const override;
    QString group(const QString &path) const override;
    bool chmod(const QString &path, QFile::Permissions permissions) override;

    // [可选] 异步批量 — LocalIO 加 200ms 延迟等 inotify
    void copyAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb) override;
    void moveAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb) override;
    void batchCopyAsync(const QStringList &srcs, const QString &destDir, QObject *ctx, BatchCallback cb) override;
    void batchMoveAsync(const QStringList &srcs, const QString &destDir, QObject *ctx, BatchCallback cb) override;

    // [可选] 符号链接
    bool symlink(const QString &target, const QString &linkPath) override;
    QString readLink(const QString &path) const override;
    bool supportsSymlinks() const override { return true; }

    // [能力]
    bool supportsPermissions() const override { return true; }
    bool supportsMimeTypes() const override { return true; }
    bool hasThumbnails() const override { return true; }
    Capabilities capabilities() const override;
    QPixmap thumbnail(const QString &path, const QSize &size) override;

    QIcon fileIcon(const QString &path) const override;

private:
    QFileIconProvider m_iconProvider;
};

#endif
