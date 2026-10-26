// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRSSHIO_H
#define QTFILEMGRSSHIO_H

#include "QtFileMgrFeatures.h"
#if QTFILEMGR_WITH_SSH
#include "QtFileMgrIO.h"

#include <QHash>
#include <QPointer>
#include <QSharedPointer>

class QThread;
class QtFileMgrSSHWorker;
struct QtFileMgrSSHBlockingState;

struct QtFileMgrSSHConnectionConfig {
    QString host;
    int port = 22;
    QString user;
    QString password;
    QString identityFile;
    QString initialPath = QStringLiteral("/");
    int timeoutSeconds = 15;
};

/// 基于 libssh/SFTP 的远端文件后端。
///
/// 此 QObject 是 GUI 线程 facade；libssh session 仅由内部工作线程访问。
/// 查询接口从最近一次目录加载的缓存读取，目录加载和文件传输不会阻塞 GUI。
class QtFileMgrSSHIO final : public QtFileMgrIO {
    Q_OBJECT
public:
    // SFTP servers may return a basename or an absolute path for a direct child.
    // Reject anything that could escape the listed directory.
    static QString safeEntryName(const QString &directory, const QString &serverName);
    explicit QtFileMgrSSHIO(QObject *parent = nullptr);
    ~QtFileMgrSSHIO() override;

    void connectToHost(const QtFileMgrSSHConnectionConfig &config);
    void acceptUnknownHostKey(bool accepted);
    void disconnectFromHost();
    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    QtFileMgrSSHConnectionConfig connectionConfig() const { return m_config; }

    QString rootPath() const override;
    QString displayName(const QString &path) const override;
    bool acceptsUrl(const QUrl &url) const override;
    QUrl urlForPath(const QString &path) const override;
    QString pathFromUrl(const QUrl &url) const override;
    bool exists(const QString &path) const override;
    QList<FileEntry> listDir(const QString &path) const override;
    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const override;
    bool isDir(const QString &path) const override;
    bool isWritable(const QString &path) const override;
    bool mkdir(const QString &path) override;
    bool rename(const QString &oldPath, const QString &newPath) override;
    bool renameNoReplace(const QString &oldPath, const QString &newPath,
                         QString *error = nullptr) override;
    // 异步重命名: worker 线程执行,不嵌套事件循环冻结 GUI (DLG-P1-5)
    void renameAsync(const QString &oldPath, const QString &newPath,
                     QObject *ctx, OpCallback cb) override;
    bool remove(const QString &path) override;
    bool removePermanent(const QString &path) override;
    bool moveToTrash(const QString &path, QString *error = nullptr) override;
    bool copy(const QString &source, const QString &destination) override;
    bool writeFile(const QString &path, const QByteArray &contents) override;
    bool canWrite() const override;

    FileEntry stat(const QString &path) const override;
    qint64 fileSize(const QString &path) const override;
    QString mimeType(const QString &path) const override;
    bool supportsMimeTypes() const override { return true; }
    QtFileMgrPreviewRequest *previewRequest(
        const QString &path, qint64 maxBytes, bool image,
        QObject *parent = nullptr) const override;
    QDateTime modificationTime(const QString &path) const override;
    QFile::Permissions permissions(const QString &path) const override;
    QString owner(const QString &path) const override;
    QString group(const QString &path) const override;
    bool chmod(const QString &path, QFile::Permissions permissions) override;
    QIcon fileIcon(const QString &path) const override;
    bool isRemote() const override { return true; }
    bool supportsPermissions() const override { return true; }
    Capabilities capabilities() const override;

    QtFileMgrTransferRequest *copyRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *moveRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *exportToLocalRequest(
        const QString &source, const QString &localDestination,
        QObject *parent = nullptr) override;
    QtFileMgrTransferRequest *importFromLocalRequest(
        const QString &localSource, const QString &remoteDestination,
        bool move, QObject *parent = nullptr) override;

    // worker 报告传输层失效时回退连接状态 (R2-17);内部接口,
    // 供 worker 经事件循环调用,不是公开 API
    void noteSessionDead();

signals:
    void unknownHostKey(const QString &host, int port,
                        const QString &fingerprint);
    void connectionFinished(bool success, const QString &error);

private:
    enum BlockingOperation { MakeDirectory, RenamePath, RenamePathNoReplace, RemovePath,
                             WriteContents, ChangePermissions };
    bool runBlockingOperation(BlockingOperation operation,
                              const QString &firstPath,
                              const QString &secondPath = QString(),
                              const QByteArray &contents = QByteArray(),
                              QFile::Permissions permissions = {},
                              QString *error = nullptr);
    void startConnection(bool acceptUnknownKey);
    // 断开/析构前置位取消所有在途传输,避免阻塞关闭等传输跑完 (R2-IO-P1-2)
    void cancelActiveTransfers();
    void updateCache(const QString &directory,
                     const QList<FileEntry> &entries) const;
    QString normalizedPath(const QString &path) const;
    void invalidatePath(const QString &path);
    QtFileMgrTransferRequest *startTransfer(
        const QString &source, const QString &destination,
        bool move, bool exportToLocal, QObject *parent,
        bool importFromLocal = false);

    QThread *m_workerThread = nullptr;
    QtFileMgrSSHWorker *m_worker = nullptr;
    QtFileMgrSSHConnectionConfig m_config;
    QByteArray m_confirmedHostKeyHash;
    mutable QHash<QString, FileEntry> m_entryCache;
    mutable QHash<QString, QList<FileEntry>> m_directoryCache;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_waitingForHostKey = false;
    bool m_blockingOperationActive = false;
    quint64 m_sessionGeneration = 0;
    // 仅 facade 线程访问;worker 只读各条目的原子取消位
    QList<QSharedPointer<QAtomicInt>> m_activeTransferCancellations;
    // 进行中的阻塞操作共享状态;析构置 facadeAlive=0 (SSH-P1-2)
    QSharedPointer<QtFileMgrSSHBlockingState> m_activeBlockingState;
};

#endif // QTFILEMGR_WITH_SSH
#endif
