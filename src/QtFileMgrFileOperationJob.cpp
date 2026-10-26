// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFileOperationJob.h"

#include "QtFileMgrIO.h"
#include "QtFileMgrLocalIO.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace {

QString joinedOperationPath(const QString &directory, const QString &name)
{
    if (directory.isEmpty())
        return name;
    if (directory == QStringLiteral("/"))
        return directory + name;
    return directory.endsWith(QLatin1Char('/'))
        ? directory + name : directory + QLatin1Char('/') + name;
}

QString normalizedOperationPath(QString path)
{
    path = QDir::cleanPath(path);
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}

QString operationParentPath(const QString &path)
{
    const QString normalized = normalizedOperationPath(path);
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    if (slash == 0)
        return QStringLiteral("/");
    return slash > 0 ? normalized.left(slash) : QString();
}

bool isSameDirectoryMove(const QString &source, const QString &destinationDirectory)
{
    return normalizedOperationPath(operationParentPath(source))
        == normalizedOperationPath(destinationDirectory);
}

bool isSameLocalDirectoryMove(const QString &source, const QString &destinationDirectory)
{
    const QString sourceParent =
        QFileInfo(QFileInfo(source).absolutePath()).canonicalFilePath();
    const QString destination =
        QFileInfo(destinationDirectory).canonicalFilePath();
    if (!sourceParent.isEmpty() && !destination.isEmpty())
        return sourceParent == destination;
    return isSameDirectoryMove(source, destinationDirectory);
}

bool isSameLocalFile(const QString &source, const QString &destination)
{
    const QString canonicalSource = QFileInfo(source).canonicalFilePath();
    const QString canonicalDestination = QFileInfo(destination).canonicalFilePath();
    if (!canonicalSource.isEmpty() && !canonicalDestination.isEmpty())
        return canonicalSource == canonicalDestination;
    return normalizedOperationPath(source) == normalizedOperationPath(destination);
}

QString operationName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    if (!name.isEmpty())
        return name;
    return normalizedOperationPath(path).section(QLatin1Char('/'), -1);
}

bool localPathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool removeLocalPath(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile())
        return QFile::remove(path);
    if (info.isDir())
        return QDir(path).removeRecursively();
    return !info.exists();
}

QString uniqueLocalDestName(const QString &directory, const QString &name)
{
    const QString prefix = directory.isEmpty()
        ? QString() : (directory.endsWith(QLatin1Char('/'))
                           ? directory : directory + QLatin1Char('/'));
    const QFileInfo nameInfo(name);
    const QString base = prefix + nameInfo.completeBaseName();
    const QString extension = nameInfo.suffix();
    QString destination = prefix + name;
    int suffix = 1;
    while (localPathExists(destination)) {
        destination = base + QStringLiteral(" (%1)").arg(suffix++)
            + (extension.isEmpty() ? QString()
                                   : QLatin1Char('.') + extension);
    }
    return destination;
}

QString temporaryLocalSiblingPath(const QString &destination, const QString &kind)
{
    const QString parent = operationParentPath(destination);
    const QString marker = QStringLiteral(".qtfilemgr-%1-%2")
        .arg(kind, QUuid::createUuid().toString().remove(QLatin1Char('{')).remove(QLatin1Char('}')));
    return uniqueLocalDestName(parent, marker);
}

struct ItemResult {
    enum Status { Succeeded, Failed, Skipped, Cancelled } status = Failed;
    QString destination;
    QString error;
};

ItemResult replaceLocalItem(QtFileMgrFileOperationJob::Operation operation,
                            const QString &source,
                            const QString &destination,
                            const QtFileMgrLocalIO::CancelCheck &isCancelled,
                            const QtFileMgrLocalIO::ByteProgressCallback &progress)
{
    ItemResult result;
    result.destination = destination;

    if (isSameLocalFile(source, destination)) {
        qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: skipped identical source and destination (source=\"%s\", destination=\"%s\")",
                 qPrintable(source), qPrintable(destination));
        result.status = ItemResult::Skipped;
        return result;
    }

    const QString staged = temporaryLocalSiblingPath(
        destination, QStringLiteral("staged"));
    QString transferError;
    const QtFileMgrLocalIO::TransferStatus stagedStatus =
        QtFileMgrLocalIO::copyCancellable(
            source, staged, isCancelled, progress, &transferError);
    if (stagedStatus == QtFileMgrLocalIO::TransferCancelled) {
        result.status = ItemResult::Cancelled;
        result.error = transferError;
        return result;
    }
    if (stagedStatus != QtFileMgrLocalIO::TransferSucceeded) {
        result.error = QStringLiteral("无法创建覆盖用临时副本: %1 -> %2 (%3)")
                           .arg(source, staged, transferError);
        return result;
    }

    if (isCancelled && isCancelled()) {
        if (!removeLocalPath(staged)) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean staged copy after cancellation (staged=\"%s\", destination=\"%s\")",
                     qPrintable(staged), qPrintable(destination));
        }
        result.status = ItemResult::Cancelled;
        result.error = QStringLiteral("操作已取消");
        return result;
    }

    const QString backup = temporaryLocalSiblingPath(
        destination, QStringLiteral("backup"));
    if (!QFile::rename(destination, backup)) {
        if (!removeLocalPath(staged)) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean staged copy after backup failure (staged=\"%s\", destination=\"%s\")",
                     qPrintable(staged), qPrintable(destination));
        }
        result.error = QStringLiteral("无法备份已有目标: %1").arg(destination);
        return result;
    }

    if (isCancelled && isCancelled()) {
        const bool restored = QFile::rename(backup, destination);
        if (!removeLocalPath(staged)) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean staged copy while rolling back cancellation (staged=\"%s\", destination=\"%s\")",
                     qPrintable(staged), qPrintable(destination));
        }
        if (!restored) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to restore target after cancellation (backup=\"%s\", destination=\"%s\")",
                     qPrintable(backup), qPrintable(destination));
            result.error = QStringLiteral("操作已取消，但原目标恢复失败，备份保留在: %1")
                               .arg(backup);
        } else {
            result.error = QStringLiteral("操作已取消");
        }
        result.status = ItemResult::Cancelled;
        return result;
    }

    if (!QFile::rename(staged, destination)) {
        const bool restored = QFile::rename(backup, destination);
        if (!removeLocalPath(staged)) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean staged copy after install failure (staged=\"%s\", destination=\"%s\")",
                     qPrintable(staged), qPrintable(destination));
        }
        result.error = restored
            ? QStringLiteral("无法安装新目标，原目标已恢复: %1").arg(destination)
            : QStringLiteral("无法安装新目标且自动恢复失败，备份保留在: %1").arg(backup);
        return result;
    }

    if (isCancelled && isCancelled()) {
        const QString cancelledNew = temporaryLocalSiblingPath(
            destination, QStringLiteral("cancelled"));
        const bool movedNewAside = QFile::rename(destination, cancelledNew);
        const bool restored = movedNewAside && QFile::rename(backup, destination);
        if (restored && !removeLocalPath(cancelledNew)) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean cancelled replacement (path=\"%s\", destination=\"%s\")",
                     qPrintable(cancelledNew), qPrintable(destination));
        }
        if (!restored) {
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: incomplete rollback after cancellation (destination=\"%s\", backup=\"%s\", replacement=\"%s\")",
                     qPrintable(destination), qPrintable(backup), qPrintable(cancelledNew));
            result.error = QStringLiteral("操作已取消，但目标回滚不完整，请检查: %1, %2")
                               .arg(backup, cancelledNew);
        } else {
            result.error = QStringLiteral("操作已取消");
        }
        result.status = ItemResult::Cancelled;
        return result;
    }

    if (operation == QtFileMgrFileOperationJob::Move
        && !removeLocalPath(source)) {
        const QString failedNew = temporaryLocalSiblingPath(
            destination, QStringLiteral("rollback"));
        const bool movedNewAside = QFile::rename(destination, failedNew);
        const bool restored = movedNewAside && QFile::rename(backup, destination);
        if (restored) {
            // 源清理失败可能已删除部分内容 (removeRecursively 尽力而为),
            // failedNew 是最后一份完整副本,必须保留并向用户报告 (审查#4)
            qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: source removal failed; keeping complete replacement (path=\"%s\", source=\"%s\", destination=\"%s\")",
                     qPrintable(failedNew), qPrintable(source),
                     qPrintable(destination));
            result.error = QStringLiteral(
                "无法完全删除移动源，原目标已恢复；完整副本保留在: %1")
                               .arg(failedNew);
        } else {
            result.error = QStringLiteral(
                "无法删除移动源且自动恢复不完整，请检查: %1, %2")
                               .arg(source, backup);
        }
        return result;
    }

    if (!removeLocalPath(backup)) {
        qWarning("[QtFileMgrFileOperationJob] replaceLocalItem: failed to clean target backup after success (backup=\"%s\", destination=\"%s\")",
                 qPrintable(backup), qPrintable(destination));
        result.error = QStringLiteral("操作已完成，但无法清理目标备份: %1").arg(backup);
    }
    result.status = ItemResult::Succeeded;
    return result;
}

ItemResult processLocalItem(QtFileMgrFileOperationJob::Operation operation,
                            QtFileMgrFileOperationJob::ConflictPolicy policy,
                            const QString &source,
                            const QString &destinationDirectory,
                            const QtFileMgrLocalIO::CancelCheck &isCancelled,
                            const QtFileMgrLocalIO::ByteProgressCallback &progress)
{
    ItemResult result;
    const QString name = operationName(source);
    if (name.isEmpty()) {
        result.error = QStringLiteral("源路径缺少有效文件名: %1").arg(source);
        return result;
    }

    if (operation == QtFileMgrFileOperationJob::Move
        && isSameLocalDirectoryMove(source, destinationDirectory)) {
        qWarning("[QtFileMgrFileOperationJob] processLocalItem: skipped move already in destination (source=\"%s\", destination=\"%s\")",
                 qPrintable(source), qPrintable(destinationDirectory));
        result.status = ItemResult::Skipped;
        return result;
    }

    QString destination = joinedOperationPath(destinationDirectory, name);
    const bool samePath = isSameLocalFile(source, destination);
    if (localPathExists(destination)) {
        if (samePath && operation == QtFileMgrFileOperationJob::Copy)
            policy = QtFileMgrFileOperationJob::AutoRename;
        if (policy == QtFileMgrFileOperationJob::SkipExisting) {
            qWarning("[QtFileMgrFileOperationJob] processLocalItem: skipped existing destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            result.status = ItemResult::Skipped;
            return result;
        }
        if (policy == QtFileMgrFileOperationJob::AutoRename) {
            qWarning("[QtFileMgrFileOperationJob] processLocalItem: resolved conflict by auto-renaming (source=\"%s\", occupied=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            destination = uniqueLocalDestName(destinationDirectory, name);
        } else {
            return replaceLocalItem(operation, source, destination,
                                    isCancelled, progress);
        }
    }

    result.destination = destination;
    QString transferError;
    const QtFileMgrLocalIO::TransferStatus status =
        operation == QtFileMgrFileOperationJob::Copy
            ? QtFileMgrLocalIO::copyCancellable(
                  source, destination, isCancelled, progress, &transferError)
            : QtFileMgrLocalIO::moveCancellable(
                  source, destination, isCancelled, progress, &transferError);
    if (status == QtFileMgrLocalIO::TransferSucceeded) {
        result.status = ItemResult::Succeeded;
    } else if (status == QtFileMgrLocalIO::TransferCancelled) {
        result.status = ItemResult::Cancelled;
        result.error = transferError;
    } else {
        result.error = QStringLiteral("%1失败: %2 -> %3 (%4)")
            .arg(operation == QtFileMgrFileOperationJob::Copy
                     ? QStringLiteral("复制") : QStringLiteral("移动"),
                 source,
                 destination,
                 transferError);
    }
    return result;
}

} // namespace

// job 与工作线程之间的共享桥:析构超时分离后线程不再触碰 job。
// 所有向 job 的排队投递都在 mutex 内完成,job 析构先持锁清空指针,
// 保证线程不会向已销毁对象投递事件。
class QtFileMgrLocalOperationBridge {
public:
    QMutex mutex;
    QtFileMgrFileOperationJob *job = nullptr;
};

class QtFileMgrLocalOperationThread : public QThread {
public:
    QtFileMgrLocalOperationThread(QtFileMgrFileOperationJob *job,
                                  const QSharedPointer<QtFileMgrLocalOperationBridge> &bridge,
                                  QtFileMgrFileOperationJob::Operation operation,
                                  const QStringList &sources,
                                  const QString &destinationDirectory,
                                  QtFileMgrFileOperationJob::ConflictPolicy policy)
        : QThread(job)
        , m_bridge(bridge)
        , m_operation(operation)
        , m_sources(sources)
        , m_destinationDirectory(destinationDirectory)
        , m_policy(policy)
    {
    }

protected:
    void run() override
    {
        int succeeded = 0;
        int failed = 0;
        int skipped = 0;
        QStringList newNames;
        QStringList errors;
        bool cancelled = false;

        for (int index = 0; index < m_sources.size(); ++index) {
            if (isInterruptionRequested()) {
                cancelled = true;
                break;
            }

            const QString source = m_sources.at(index);
            const QtFileMgrLocalIO::CancelCheck isCancelled = [this]() {
                return isInterruptionRequested();
            };
            const QtFileMgrLocalIO::ByteProgressCallback byteProgress =
                [this, index, source](qint64 transferred, qint64 total) {
                const QSharedPointer<QtFileMgrLocalOperationBridge> bridge = m_bridge;
                QMutexLocker lock(&bridge->mutex);
                QtFileMgrFileOperationJob *job = bridge->job;
                if (!job)
                    return;
                QMetaObject::invokeMethod(
                    job,
                    [job, index, source, transferred, total]() {
                        job->handleLocalTransferProgress(
                            index, source, transferred, total);
                    },
                    Qt::QueuedConnection);
            };
            QByteArray sourceSnapshot;
            if (m_operation == QtFileMgrFileOperationJob::Move) {
                QString sourceSnapshotError;
                sourceSnapshot = QtFileMgrLocalIO::captureUndoSnapshot(source, &sourceSnapshotError);
                if (sourceSnapshot.isEmpty()) {
                    qWarning("[QtFileMgrFileOperationJob] move lineage unavailable before transfer (source=\"%s\", destinationDirectory=\"%s\", error=\"%s\")",
                             qPrintable(source), qPrintable(m_destinationDirectory),
                             qPrintable(sourceSnapshotError));
                }
            }
            const ItemResult result = processLocalItem(
                m_operation, m_policy, source, m_destinationDirectory,
                isCancelled, byteProgress);
            if (result.status == ItemResult::Succeeded) {
                ++succeeded;
                newNames.append(operationName(result.destination));
                if (!result.error.isEmpty())
                    errors.append(result.error);
                QString snapshotError;
                const QByteArray snapshot = QtFileMgrLocalIO::captureUndoSnapshot(
                    result.destination, &snapshotError);
                if (snapshot.isEmpty()) {
                    qWarning("[QtFileMgrFileOperationJob] local undo unavailable after completed transfer (source=\"%s\", destination=\"%s\", error=\"%s\")",
                             qPrintable(source), qPrintable(result.destination),
                             qPrintable(snapshotError));
                }
                const bool linkedMove = m_operation == QtFileMgrFileOperationJob::Move
                    && QtFileMgrLocalIO::undoSnapshotsMatchAfterRename(sourceSnapshot, snapshot);
                if (m_operation == QtFileMgrFileOperationJob::Move && !linkedMove) {
                    qWarning("[QtFileMgrFileOperationJob] move lineage withheld: complete retained-object proofs do not match (source=\"%s\", destination=\"%s\", sourceProofAvailable=%d, destinationProofAvailable=%d; copied move or intervening change)",
                             qPrintable(source), qPrintable(result.destination),
                             !sourceSnapshot.isEmpty(), !snapshot.isEmpty());
                }
                const QSharedPointer<QtFileMgrLocalOperationBridge> bridge = m_bridge;
                QMutexLocker lock(&bridge->mutex);
                if (QtFileMgrFileOperationJob *job = bridge->job) {
                    QMetaObject::invokeMethod(job,
                        [job, source, result, snapshot, sourceSnapshot, linkedMove]() {
                            const QPointer<QtFileMgrFileOperationJob> guardedJob(job);
                            if (linkedMove)
                                emit job->itemMoveLineage(source, result.destination, sourceSnapshot, snapshot);
                            if (guardedJob) {
                                emit guardedJob->itemCompleted(source, result.destination, snapshot);
                            } else {
                                qWarning("[QtFileMgrFileOperationJob] skipped completion notification after move-lineage receiver destroyed job (source=\"%s\", destination=\"%s\")",
                                         qPrintable(source), qPrintable(result.destination));
                            }
                        }, Qt::QueuedConnection);
                }
            } else if (result.status == ItemResult::Skipped) {
                ++skipped;
            } else if (result.status == ItemResult::Cancelled) {
                cancelled = true;
                if (!result.error.isEmpty()
                    && result.error != QStringLiteral("操作已取消")) {
                    errors.append(result.error);
                }
                break;
            } else {
                ++failed;
                errors.append(result.error);
            }

            const int completed = index + 1;
            {
                const QSharedPointer<QtFileMgrLocalOperationBridge> bridge = m_bridge;
                QMutexLocker lock(&bridge->mutex);
                QtFileMgrFileOperationJob *job = bridge->job;
                if (job) {
                    QMetaObject::invokeMethod(
                        job,
                        [job, completed, source]() {
                            job->handleLocalProgress(completed, source);
                        },
                        Qt::QueuedConnection);
                }
            }
        }

        if (isInterruptionRequested())
            cancelled = true;
        {
            const QSharedPointer<QtFileMgrLocalOperationBridge> bridge = m_bridge;
            QMutexLocker lock(&bridge->mutex);
            QtFileMgrFileOperationJob *job = bridge->job;
            if (job) {
                QMetaObject::invokeMethod(
                    job,
                    [job, succeeded, failed, skipped, newNames, cancelled, errors]() {
                        job->handleLocalFinished(
                            succeeded, failed, skipped, newNames, cancelled, errors);
                    },
                    Qt::QueuedConnection);
            }
        }
    }

private:
    QSharedPointer<QtFileMgrLocalOperationBridge> m_bridge;
    QtFileMgrFileOperationJob::Operation m_operation;
    QStringList m_sources;
    QString m_destinationDirectory;
    QtFileMgrFileOperationJob::ConflictPolicy m_policy;
};

struct QtFileMgrRemoteOperationItem {
    QString source;
    QString destination;
    QString target;
    QString staging;
    QString backup;
    QByteArray backupSnapshot;
    QString phase = QStringLiteral("transfer");
    quint64 requestId = 0;
    bool transferDelivered = false;
    bool terminal = false;
    bool installed = false;

    QString recoveryContext() const
    {
        return QStringLiteral("source=%1; target=%2; staging=%3; backup=%4; phase=%5")
            .arg(source, target.isEmpty() ? destination : target, staging, backup, phase);
    }
};

QtFileMgrFileOperationJob::QtFileMgrFileOperationJob(
    QtFileMgrIO *io,
    Operation operation,
    const QStringList &sources,
    const QString &destinationDirectory,
    ConflictPolicy conflictPolicy,
    QObject *parent)
    : QObject(parent)
    , m_io(io)
    , m_operation(operation)
    , m_destinationDirectory(destinationDirectory)
    , m_conflictPolicy(conflictPolicy)
    , m_localThread(nullptr)
    , m_remoteIndex(0)
    , m_succeeded(0)
    , m_failed(0)
    , m_skipped(0)
    , m_started(false)
    , m_finished(false)
    , m_cancelRequested(false)
{
    QSet<QString> seen;
    for (const QString &source : sources) {
        if (source.isEmpty()) {
            qWarning("[QtFileMgrFileOperationJob] constructor: skipped empty source (operation=%d, destination=\"%s\")",
                     static_cast<int>(operation),
                     qPrintable(destinationDirectory));
            continue;
        }
        if (seen.contains(source)) {
            qWarning("[QtFileMgrFileOperationJob] constructor: skipped duplicate source (source=\"%s\", operation=%d, destination=\"%s\")",
                     qPrintable(source), static_cast<int>(operation),
                     qPrintable(destinationDirectory));
            continue;
        }
        seen.insert(source);
        m_sources.append(source);
    }
}

QtFileMgrFileOperationJob::~QtFileMgrFileOperationJob()
{
    if (m_remoteItem && !m_remoteItem->terminal) {
        qWarning("[QtFileMgrFileOperationJob] destructor: interrupted remote operation; recovery paths must be inspected (%s)",
                 qPrintable(m_remoteItem->recoveryContext()));
    }
    if (m_remoteRequest)
        m_remoteRequest->cancel();
    if (m_localThread && m_localThread->isRunning()) {
        m_localThread->requestInterruption();
        if (!m_localThread->wait(5000)) {
            // 线程卡在慢速/挂起文件系统上:分离线程自行收尾,
            // 避免析构无限期冻结 GUI(不可 terminate)。
            qWarning("[QtFileMgrFileOperationJob] destructor: local operation thread did not stop within 5s (sources=%d, destination=\"%s\"); detaching, results will be discarded",
                     m_sources.size(), qPrintable(m_destinationDirectory));
            if (m_bridge) {
                QMutexLocker lock(&m_bridge->mutex);
                m_bridge->job = nullptr;
            }
            disconnect(m_localThread, nullptr, this, nullptr);
            m_localThread->setParent(nullptr);
            connect(m_localThread, &QThread::finished,
                    m_localThread, &QObject::deleteLater);
            m_localThread = nullptr;
        }
    }
}

bool QtFileMgrFileOperationJob::isRunning() const
{
    return m_started && !m_finished;
}

int QtFileMgrFileOperationJob::totalCount() const
{
    return m_sources.size();
}

void QtFileMgrFileOperationJob::start()
{
    if (m_started)
        return;
    m_started = true;

    if (m_cancelRequested) {
        m_finished = true;
        emit finished(0, m_sources.size(), 0, {}, true, {});
        return;
    }

    if (!m_io || m_sources.isEmpty()) {
        m_finished = true;
        emit finished(0, m_sources.isEmpty() ? 0 : m_sources.size(), 0,
                      {}, false,
                      m_io ? QStringList()
                           : QStringList(QStringLiteral("文件操作后端不可用")));
        return;
    }
    // 空目标目录会让 joinedOperationPath 退化成裸文件名(相对 CWD),
    // 把文件写到不可预料的位置;直接判失败 (IO-P2-8)
    if (m_destinationDirectory.isEmpty()) {
        qWarning("[QtFileMgrFileOperationJob] start: rejected empty destination directory (operation=%d, sources=%d, first=\"%s\")",
                 static_cast<int>(m_operation), m_sources.size(),
                 qPrintable(m_sources.first()));
        m_finished = true;
        emit finished(0, m_sources.size(), 0, {}, false,
                      {QStringLiteral("目标目录为空")});
        return;
    }
    if (m_io->thread() != thread()) {
        qWarning("[QtFileMgrFileOperationJob] start: rejected backend thread mismatch (backend=%s, backendThread=%p, jobThread=%p, sources=%d)",
                 m_io->metaObject()->className(),
                 static_cast<void *>(m_io->thread()),
                 static_cast<void *>(thread()), m_sources.size());
        m_finished = true;
        emit finished(0, m_sources.size(), 0, {}, false,
                      {QStringLiteral("文件后端线程与任务线程不一致")});
        return;
    }

    if (qobject_cast<QtFileMgrLocalIO *>(m_io)) {
        m_bridge = QSharedPointer<QtFileMgrLocalOperationBridge>::create();
        m_bridge->job = this;
        m_localThread = new QtFileMgrLocalOperationThread(
            this, m_bridge, m_operation, m_sources, m_destinationDirectory,
            m_conflictPolicy);
        m_localThread->start();
        return;
    }

    connect(m_io, &QObject::destroyed, this, [this]() {
        if (m_finished)
            return;
        const auto item = m_remoteItem;
        if (item && !item->terminal) {
            const QString detail = QStringLiteral("文件后端在操作期间销毁；请检查恢复路径 (%1)")
                .arg(item->recoveryContext());
            completeRemoteItem(item, item->installed,
                               item->installed ? item->target : QString(), detail);
        } else {
            QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
        }
    });
    startRemoteNext();
}

void QtFileMgrFileOperationJob::cancel()
{
    if (!m_started) {
        // start() 前的取消也要记账,启动后即刻按取消收尾 (R2-IO-P2-1)
        if (!m_cancelRequested) {
            qWarning("[QtFileMgrFileOperationJob] cancel: recorded cancel before start (sources=%d, destination=\"%s\")",
                     m_sources.size(), qPrintable(m_destinationDirectory));
            m_cancelRequested = true;
        }
        return;
    }
    if (!isRunning())
        return;
    m_cancelRequested = true;
    if (m_localThread)
        m_localThread->requestInterruption();
    if (m_remoteRequest)
        m_remoteRequest->cancel();
}

void QtFileMgrFileOperationJob::startRemoteNext()
{
    if (m_finished || m_remoteRequestPending)
        return;
    if (!m_io) {
        const int remaining = m_sources.size() - m_remoteIndex;
        qWarning("[QtFileMgrFileOperationJob] startRemoteNext: backend destroyed with pending items (remaining=%d, operation=%d)",
                 remaining, static_cast<int>(m_operation));
        m_failed += qMax(0, remaining);
        m_errors.append(QStringLiteral("文件后端已断开，剩余项目未处理"));
        m_remoteIndex = m_sources.size();
        finishRemote();
        return;
    }
    if (m_cancelRequested || m_remoteIndex >= m_sources.size()) {
        finishRemote();
        return;
    }

    const QString source = m_sources.at(m_remoteIndex);
    const QString name = operationName(source);
    if (name.isEmpty()) {
        qWarning("[QtFileMgrFileOperationJob] startRemoteNext: skipped source without file name (source=\"%s\", destination=\"%s\")",
                 qPrintable(source), qPrintable(m_destinationDirectory));
        ++m_failed;
        ++m_remoteIndex;
        m_errors.append(QStringLiteral("源路径缺少有效文件名: %1").arg(source));
        emit progress(m_remoteIndex, m_sources.size(), source);
        QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
        return;
    }

    // 来源身份先于同目录短路: 本机来源即使拼写同目录也是跨后端
    // 上传,不得跳过 (R2-13)
    const bool import = m_importSources.contains(source);
    if (!import && m_operation == Move
        && isSameDirectoryMove(source, m_destinationDirectory)) {
        qWarning("[QtFileMgrFileOperationJob] startRemoteNext: skipped move already in destination (source=\"%s\", destination=\"%s\")",
                 qPrintable(source), qPrintable(m_destinationDirectory));
        ++m_skipped;
        ++m_remoteIndex;
        emit progress(m_remoteIndex, m_sources.size(), source);
        QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
        return;
    }

    const auto item = QSharedPointer<QtFileMgrRemoteOperationItem>::create();
    item->source = source;
    item->destination = joinedOperationPath(m_destinationDirectory, name);
    const QPointer<QtFileMgrFileOperationJob> existenceJob(this);
    const QPointer<QtFileMgrIO> existenceBackend(m_io);
    const bool destinationExists = existenceBackend->exists(item->destination);
    if (!existenceJob)
        return;
    if (!existenceBackend) {
        QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
        return;
    }
    if (destinationExists) {
        if (m_conflictPolicy == OverwriteExisting) {
            item->target = item->destination;
            const QString nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item->staging = joinedOperationPath(m_destinationDirectory,
                QStringLiteral(".qtfilemgr-overwrite-%1-%2").arg(nonce, name));
            item->backup = joinedOperationPath(m_destinationDirectory,
                QStringLiteral(".qtfilemgr-backup-%1-%2").arg(nonce, name));
            item->destination = item->staging;
            qWarning("[QtFileMgrFileOperationJob] overwrite: staging before backup (%s)",
                     qPrintable(item->recoveryContext()));
        } else if (m_conflictPolicy == SkipExisting) {
            qWarning("[QtFileMgrFileOperationJob] startRemoteNext: skipped existing destination (source=\"%s\", destination=\"%s\", policy=%d)",
                     qPrintable(source), qPrintable(item->destination),
                     static_cast<int>(m_conflictPolicy));
            ++m_skipped;
            ++m_remoteIndex;
            emit progress(m_remoteIndex, m_sources.size(), source);
            QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
            return;
        } else {
            item->destination = QtFileMgrIO::uniqueDestName(m_destinationDirectory, name, m_io);
        }
    }

    item->requestId = ++m_remoteRequestId;
    m_remoteItem = item;
    m_remoteRequestPending = true;
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    const QString backendName = QString::fromLatin1(backend->metaObject()->className());
    QtFileMgrTransferRequest *request =
        import ? backend->importFromLocalRequest(source, item->destination,
                                                 m_operation == Move, this)
        : m_operation == Copy
            ? backend->copyRequest(source, item->destination, this)
            : backend->moveRequest(source, item->destination, this);
    if (!guardedJob || item->terminal)
        return;
    if (!backend) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("传输请求创建期间后端销毁；结果未知 (%1)").arg(item->recoveryContext()));
        return;
    }
    if (!request) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("后端未创建传输请求 (%1)").arg(item->recoveryContext()));
        return;
    }
    m_remoteRequest = request;
    connect(request, &QObject::destroyed, this, [this, item]() {
        if (item->terminal || item->transferDelivered)
            return;
        completeRemoteItem(item, false, QString(),
            QStringLiteral("传输请求未返回结果；保留所有恢复节点 (%1)").arg(item->recoveryContext()));
    });
    connect(request, &QtFileMgrTransferRequest::progress, this,
            [this, item](qint64 transferred, qint64 total) {
        if (item->terminal || item->transferDelivered || m_finished
            || item->requestId != m_remoteRequestId) {
            qWarning("[QtFileMgrFileOperationJob] transferProgress: ignored stale progress (%s)",
                     qPrintable(item->recoveryContext()));
            return;
        }
        emit transferProgress(m_remoteIndex, m_sources.size(), item->source, transferred, total);
    });
    const auto completed = [this, item](bool ok, const QString &newPath,
                                        const QString &error, bool cancelled) {
        if (item->terminal || item->transferDelivered || m_finished
            || item->requestId != m_remoteRequestId) {
            qWarning("[QtFileMgrFileOperationJob] transferFinished: ignored duplicate/stale completion (%s)",
                     qPrintable(item->recoveryContext()));
            return;
        }
        item->transferDelivered = true;
        m_remoteRequest.clear();
        if (cancelled)
            m_cancelRequested = true;
        if (item->staging.isEmpty()) {
            completeRemoteItem(item, ok, newPath, error);
            return;
        }
        if (!ok) {
            completeRemoteItem(item, false, QString(),
                QStringLiteral("临时传输失败或取消；原目标未由任务修改，保留新内容以供检查: %1 (%2)")
                    .arg(error, item->recoveryContext()));
            return;
        }
        if (!newPath.isEmpty() && newPath != item->staging) {
            completeRemoteItem(item, false, QString(),
                QStringLiteral("后端返回了非预期临时路径 %1；不提交覆盖 (%2)")
                    .arg(newPath, item->recoveryContext()));
            return;
        }
        item->phase = QStringLiteral("staged");
        QTimer::singleShot(0, this, [this, item]() { backupRemoteTarget(item); });
    };
    connect(request, &QtFileMgrTransferRequest::finished, this, completed);
    if (request->isFinished()) {
        qWarning("[QtFileMgrFileOperationJob] startRemoteNext: consumed synchronously completed request (backend=%s, source=\"%s\")",
                 qPrintable(backendName), qPrintable(source));
        completed(request->succeeded(), request->resultPath(),
                  request->errorString(), request->isCancelled());
    }
}

void QtFileMgrFileOperationJob::backupRemoteTarget(
    const QSharedPointer<QtFileMgrRemoteOperationItem> &item)
{
    if (item->terminal)
        return;
    if (m_cancelRequested) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("备份前已取消；原目标未修改，新内容保留在临时路径 (%1)")
                .arg(item->recoveryContext()));
        return;
    }
    if (!m_io) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("备份前后端已销毁；保留恢复节点 (%1)").arg(item->recoveryContext()));
        return;
    }
    item->phase = QStringLiteral("backup");
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    QString error;
    const bool backedUp = backend->renameNoReplace(item->target, item->backup, &error);
    if (!guardedJob || item->terminal)
        return;
    if (!backedUp || !backend) {
        // A failed RPC may already have moved the node. Do not repeat it, delete
        // staging, or treat an arbitrary node now at backup as our own.
        completeRemoteItem(item, false, QString(),
            QStringLiteral("原目标备份未确认成功，未继续提交；结果可能不确定，请检查所有恢复路径: %1 (%2)")
                .arg(error, item->recoveryContext()));
        return;
    }
    item->phase = QStringLiteral("backed-up");
    QString snapshotError;
    item->backupSnapshot = backend->undoSnapshot(item->backup, &snapshotError);
    if (!guardedJob || item->terminal)
        return;
    if (item->backupSnapshot.isEmpty()) {
        qWarning("[QtFileMgrFileOperationJob] overwrite: backup has no verifiable cleanup token; it will be retained after commit (error=\"%s\", %s)",
                 qPrintable(snapshotError), qPrintable(item->recoveryContext()));
    }
    QTimer::singleShot(0, this, [this, item]() { commitRemoteTarget(item); });
}

void QtFileMgrFileOperationJob::commitRemoteTarget(
    const QSharedPointer<QtFileMgrRemoteOperationItem> &item)
{
    if (item->terminal)
        return;
    if (m_cancelRequested) {
        restoreRemoteTarget(item, QStringLiteral("备份完成后已取消"));
        return;
    }
    if (!m_io) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("提交前后端已销毁；旧目标及新内容分别保留在备份/临时节点 (%1)")
                .arg(item->recoveryContext()));
        return;
    }
    item->phase = QStringLiteral("commit");
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    QString error;
    const bool committed = backend->renameNoReplace(item->staging, item->target, &error);
    if (!guardedJob || item->terminal)
        return;
    if (!committed || !backend) {
        restoreRemoteTarget(item,
            QStringLiteral("临时内容提交未确认成功，可能已有并发目标或连接结果不确定: %1").arg(error));
        return;
    }
    item->installed = true;
    item->phase = QStringLiteral("committed");
    QTimer::singleShot(0, this, [this, item]() { cleanupRemoteBackup(item); });
}

void QtFileMgrFileOperationJob::restoreRemoteTarget(
    const QSharedPointer<QtFileMgrRemoteOperationItem> &item, const QString &reason)
{
    if (item->terminal)
        return;
    item->phase = QStringLiteral("rollback");
    if (!m_io) {
        completeRemoteItem(item, false, QString(),
            QStringLiteral("%1；无法恢复：后端已销毁，请检查备份及新内容 (%2)")
                .arg(reason, item->recoveryContext()));
        return;
    }
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    QString error;
    const bool restored = item->backupSnapshot.isEmpty()
        ? backend->renameNoReplace(item->backup, item->target, &error)
        : backend->undoMoveIfUnchanged(item->backup, item->target,
                                       item->backupSnapshot, &error);
    if (!guardedJob || item->terminal)
        return;
    // The new content is deliberately retained, including for move: its original
    // source may already have been removed by the successful staging transfer.
    const QString detail = restored
        ? QStringLiteral("%1；原目标已恢复，新内容保留在临时路径 (%2)")
              .arg(reason, item->recoveryContext())
        : QStringLiteral("%1；恢复未确认成功，禁止覆盖后来出现的目标；保留备份和新内容以供人工恢复: %2 (%3)")
              .arg(reason, error, item->recoveryContext());
    completeRemoteItem(item, false, QString(), detail);
}

void QtFileMgrFileOperationJob::cleanupRemoteBackup(
    const QSharedPointer<QtFileMgrRemoteOperationItem> &item)
{
    if (item->terminal)
        return;
    item->phase = QStringLiteral("cleanup");
    if (m_cancelRequested || !m_io || item->backupSnapshot.isEmpty()) {
        const QString reason = m_cancelRequested ? QStringLiteral("取消已迟于提交点")
            : (!m_io ? QStringLiteral("后端已销毁") : QStringLiteral("后端无法验证备份身份"));
        completeRemoteItem(item, true, item->target,
            QStringLiteral("警告：新内容已提交；%1，旧目标备份保留 (%2)")
                .arg(reason, item->recoveryContext()));
        return;
    }
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    QString error;
    const bool cleaned = backend->undoCopyIfUnchanged(item->backup,
                                                     item->backupSnapshot, &error);
    if (!guardedJob || item->terminal)
        return;
    completeRemoteItem(item, true, item->target, cleaned ? QString()
        : QStringLiteral("警告：覆盖已完成，但备份清理未确认成功；请检查备份路径: %1 (%2)")
              .arg(error, item->recoveryContext()));
}

void QtFileMgrFileOperationJob::completeRemoteItem(
    const QSharedPointer<QtFileMgrRemoteOperationItem> &item, bool succeeded,
    const QString &finalPath, const QString &detail)
{
    if (item->terminal || m_finished || item->requestId != m_remoteRequestId) {
        qWarning("[QtFileMgrFileOperationJob] completeRemoteItem: ignored stale completion (%s)",
                 qPrintable(item->recoveryContext()));
        return;
    }
    item->terminal = true;
    m_remoteRequest.clear();
    const QString report = detail.isEmpty() && succeeded && item->installed && m_cancelRequested
        ? QStringLiteral("警告：取消发生在新目标提交之后；已完成项目保持成功 (%1)")
              .arg(item->recoveryContext())
        : detail;
    if (!report.isEmpty()) {
        qWarning("[QtFileMgrFileOperationJob] %s (%s)", qPrintable(report),
                 qPrintable(item->recoveryContext()));
        m_errors.append(report);
    }
    const QPointer<QtFileMgrFileOperationJob> guardedJob(this);
    if (succeeded) {
        ++m_succeeded;
        m_newNames.append(operationName(finalPath));
        QString snapshotError;
        const QByteArray snapshot = m_io ? m_io->undoSnapshot(finalPath, &snapshotError) : QByteArray();
        if (!guardedJob)
            return;
        if (snapshot.isEmpty()) {
            qWarning("[QtFileMgrFileOperationJob] remote undo unavailable after completed transfer (error=\"%s\", %s)",
                     qPrintable(snapshotError), qPrintable(item->recoveryContext()));
        }
        emit itemCompleted(item->source, finalPath, snapshot);
        if (!guardedJob)
            return;
    } else {
        ++m_failed;
        if (detail.isEmpty()) {
            qWarning("[QtFileMgrFileOperationJob] backend returned failure without details (%s)",
                     qPrintable(item->recoveryContext()));
            m_errors.append(QStringLiteral("后端未提供失败详情 (%1)").arg(item->recoveryContext()));
        }
    }
    ++m_remoteIndex;
    m_remoteRequestPending = false;
    emit progress(m_remoteIndex, m_sources.size(), item->source);
    if (!guardedJob)
        return;
    QTimer::singleShot(0, this, [this]() { startRemoteNext(); });
}

void QtFileMgrFileOperationJob::finishRemote()
{
    if (m_finished)
        return;
    m_remoteRequestPending = false;
    m_remoteRequest.clear();
    ++m_remoteRequestId;
    m_finished = true;
    emit finished(m_succeeded, m_failed, m_skipped, m_newNames,
                  m_cancelRequested, m_errors);
}

void QtFileMgrFileOperationJob::handleLocalProgress(
    int completed, const QString &currentPath)
{
    if (!m_finished)
        emit progress(completed, m_sources.size(), currentPath);
}

void QtFileMgrFileOperationJob::handleLocalTransferProgress(
    int completed,
    const QString &currentPath,
    qint64 bytesTransferred,
    qint64 bytesTotal)
{
    if (!m_finished) {
        emit transferProgress(completed, m_sources.size(), currentPath,
                              bytesTransferred, bytesTotal);
    }
}

void QtFileMgrFileOperationJob::handleLocalFinished(
    int succeeded,
    int failed,
    int skipped,
    const QStringList &newNames,
    bool cancelled,
    const QStringList &errors)
{
    if (m_finished)
        return;
    m_finished = true;
    // 收尾窗口内的 cancel() 也要反映到结果里 (R2-IO-P2-8)
    emit finished(succeeded, failed, skipped, newNames,
                  cancelled || m_cancelRequested, errors);
}
