// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRFILEOPERATIONJOB_H
#define QTFILEMGRFILEOPERATIONJOB_H

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include <QtGlobal>

class QtFileMgrIO;
class QtFileMgrTransferRequest;
class QtFileMgrLocalOperationThread;
class QtFileMgrLocalOperationBridge;
struct QtFileMgrRemoteOperationItem;

class QtFileMgrFileOperationJob : public QObject {
    Q_OBJECT
public:
    enum Operation {
        Copy,
        Move
    };
    Q_ENUM(Operation)

    enum ConflictPolicy {
        AutoRename,
        SkipExisting,
        OverwriteExisting
    };
    Q_ENUM(ConflictPolicy)

    explicit QtFileMgrFileOperationJob(QtFileMgrIO *io,
                                       Operation operation,
                                       const QStringList &sources,
                                       const QString &destinationDirectory,
                                       ConflictPolicy conflictPolicy,
                                       QObject *parent = nullptr);
    ~QtFileMgrFileOperationJob() override;

    bool isRunning() const;
    int totalCount() const;
    // 远端目标下的本机来源集合: 这些源走 importFromLocalRequest 显式上传,
    // 不得按远端路径处理 (审查#1)
    void setImportSources(const QSet<QString> &sources) { m_importSources = sources; }

public slots:
    void start();
    void cancel();

signals:
    // A completed destination may be offered for undo only with a nonempty
    // backend snapshot. An empty token does not change transfer success.
    void itemCompleted(const QString &source, const QString &destination,
                       const QByteArray &undoToken);
    // Local move provenance, issued only when complete execution-time proofs
    // establish that the move retained the source object and its full revision.
    // A copied cross-filesystem move cannot supply this signal.
    void itemMoveLineage(const QString &source, const QString &destination,
                         const QByteArray &sourceToken, const QByteArray &destinationToken);
    void progress(int completed, int total, const QString &currentPath);
    void transferProgress(int completed, int total, const QString &currentPath,
                          qint64 bytesTransferred, qint64 bytesTotal);
    void finished(int succeeded,
                  int failed,
                  int skipped,
                  const QStringList &newNames,
                  bool cancelled,
                  const QStringList &errors);

private:
    friend class QtFileMgrLocalOperationThread;

    void startRemoteNext();
    void backupRemoteTarget(const QSharedPointer<QtFileMgrRemoteOperationItem> &item);
    void commitRemoteTarget(const QSharedPointer<QtFileMgrRemoteOperationItem> &item);
    void restoreRemoteTarget(const QSharedPointer<QtFileMgrRemoteOperationItem> &item,
                             const QString &reason);
    void cleanupRemoteBackup(const QSharedPointer<QtFileMgrRemoteOperationItem> &item);
    void completeRemoteItem(const QSharedPointer<QtFileMgrRemoteOperationItem> &item,
                            bool succeeded, const QString &finalPath,
                            const QString &detail = QString());
    void finishRemote();
    void handleLocalProgress(int completed, const QString &currentPath);
    void handleLocalTransferProgress(int completed,
                                     const QString &currentPath,
                                     qint64 bytesTransferred,
                                     qint64 bytesTotal);
    void handleLocalFinished(int succeeded,
                             int failed,
                             int skipped,
                             const QStringList &newNames,
                             bool cancelled,
                             const QStringList &errors);

    QPointer<QtFileMgrIO> m_io;
    Operation m_operation;
    QStringList m_sources;
    QSet<QString> m_importSources;
    QString m_destinationDirectory;
    ConflictPolicy m_conflictPolicy;
    QtFileMgrLocalOperationThread *m_localThread;
    QSharedPointer<QtFileMgrLocalOperationBridge> m_bridge;
    int m_remoteIndex;
    int m_succeeded;
    int m_failed;
    int m_skipped;
    quint64 m_remoteRequestId = 0;
    QStringList m_newNames;
    QStringList m_errors;
    bool m_started;
    bool m_finished;
    bool m_cancelRequested;
    bool m_remoteRequestPending = false;
    QPointer<QtFileMgrTransferRequest> m_remoteRequest;
    QSharedPointer<QtFileMgrRemoteOperationItem> m_remoteItem;
};

#endif
