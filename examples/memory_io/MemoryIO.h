// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGR_EXAMPLE_MEMORYIO_H
#define QTFILEMGR_EXAMPLE_MEMORYIO_H

#include <QtFileMgrIO.h>
#include <QMap>

// A deliberately small, immutable virtual filesystem. "Remote" means that its
// paths belong to this backend; it does not imply a network connection.
class MemoryIO final : public QtFileMgrIO {
public:
    explicit MemoryIO(QObject *parent = nullptr);

    QString rootPath() const override;
    QString displayName(const QString &path) const override;
    bool acceptsUrl(const QUrl &url) const override;
    QUrl urlForPath(const QString &path) const override;
    QString pathFromUrl(const QUrl &url) const override;
    bool exists(const QString &path) const override;
    bool isDir(const QString &path) const override;
    bool isWritable(const QString &path) const override;
    bool canWrite() const override;
    bool isRemote() const override;
    bool supportsMimeTypes() const override;
    Capabilities capabilities() const override;
    QList<FileEntry> listDir(const QString &path) const override;
    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const override;
    FileEntry stat(const QString &path) const override;
    qint64 fileSize(const QString &path) const override;
    QDateTime modificationTime(const QString &path) const override;
    QString mimeType(const QString &path) const override;
    QByteArray readPreview(const QString &path, qint64 maxBytes,
                           bool *truncated = nullptr,
                           QString *error = nullptr) const override;
    QtFileMgrPreviewRequest *previewRequest(
        const QString &path, qint64 maxBytes, bool image,
        QObject *parent = nullptr) const override;

    bool mkdir(const QString &path) override;
    bool rename(const QString &oldPath, const QString &newPath) override;
    bool remove(const QString &path) override;
    bool removePermanent(const QString &path) override;
    bool moveToTrash(const QString &path, QString *error = nullptr) override;
    bool copy(const QString &source, const QString &destination) override;
    bool writeFile(const QString &path, const QByteArray &contents) override;
    bool chmod(const QString &path, QFile::Permissions permissions) override;
    bool symlink(const QString &target, const QString &linkPath) override;
    bool restoreVersion(const QString &path, int version) override;
    void restoreVersionAsync(const QString &path, int version, QObject *context,
                             ResultCallback callback) override;
    void renameAsync(const QString &source, const QString &destination,
                     QObject *context, OpCallback callback) override;
    void copyAsync(const QString &source, const QString &destination,
                   QObject *context, OpCallback callback) override;
    void moveAsync(const QString &source, const QString &destination,
                   QObject *context, OpCallback callback) override;
    void batchCopyAsync(const QStringList &sources, const QString &destination,
                        QObject *context, BatchCallback callback) override;
    void batchMoveAsync(const QStringList &sources, const QString &destination,
                        QObject *context, BatchCallback callback) override;
    QtFileMgrTransferRequest *copyRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *moveRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *exportToLocalRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *importFromLocalRequest(
        const QString &source, const QString &destination, bool move,
        QObject *parent = nullptr) override;

private:
    struct Node {
        bool directory = false;
        QByteArray bytes;
        QString mime;
    };
    static bool validatePath(const QString &path, const char *source,
                              QString *error = nullptr);
    static bool validateUrl(const QUrl &url, const char *source);
    const Node *findNode(const QString &path, const char *source,
                         QString *error = nullptr) const;
    bool directoryEntries(const QString &path, const char *source,
                           QList<FileEntry> *entries, QString *error) const;
    static bool rejectWrite(const char *operation, const QString &path,
                             const QString &destination = QString());
    static void rejectOperation(const char *operation, const QString &source,
                                 const QString &destination, OpCallback callback);
    static void rejectBatch(const char *operation, const QStringList &sources,
                             const QString &destination, BatchCallback callback);
    static QtFileMgrTransferRequest *rejectTransfer(
        const char *operation, const QString &source,
        const QString &destination, QObject *parent);
    QMap<QString, Node> m_nodes;
};

#endif
