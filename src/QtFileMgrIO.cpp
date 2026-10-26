// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// QtFileMgrIO 默认实现 (异步批量操作)
#include "QtFileMgrIO.h"
#include <QTimer>
#include <QFileInfo>
#include <QDebug>
#include <QPointer>
#include <QThread>
#include <QDir>

bool QtFileMgrIO::renameNoReplace(const QString &source, const QString &destination,
                                 QString *error)
{
    qWarning("[QtFileMgrIO] renameNoReplace: refused unsupported atomic operation (backend=%s, source=\"%s\", destination=\"%s\")",
             metaObject()->className(), qPrintable(source), qPrintable(destination));
    if (error)
        *error = QStringLiteral("Backend does not support atomic no-replace rename.");
    return false;
}

QByteArray QtFileMgrIO::undoSnapshot(const QString &path, QString *error) const
{
    qWarning("[QtFileMgrIO] undoSnapshot: refused unprovable object revision (backend=%s, path=\"%s\")",
             metaObject()->className(), qPrintable(path));
    if (error)
        *error = QStringLiteral("Backend cannot prove an unchanged object revision for undo.");
    return {};
}

bool QtFileMgrIO::undoCopyIfUnchanged(const QString &destination, const QByteArray &snapshot,
                                     QString *error, QByteArray *refreshedSnapshot)
{
    Q_UNUSED(refreshedSnapshot);
    qWarning("[QtFileMgrIO] undoCopyIfUnchanged: refused unsupported guarded deletion (backend=%s, destination=\"%s\", snapshotBytes=%d)",
             metaObject()->className(), qPrintable(destination), static_cast<int>(snapshot.size()));
    if (error)
        *error = QStringLiteral("Backend does not support guarded copy undo.");
    return false;
}

bool QtFileMgrIO::undoMoveIfUnchanged(const QString &destination, const QString &source,
                                     const QByteArray &snapshot, QString *error, QByteArray *refreshedSnapshot)
{
    Q_UNUSED(refreshedSnapshot);
    qWarning("[QtFileMgrIO] undoMoveIfUnchanged: refused unsupported guarded move (backend=%s, destination=\"%s\", source=\"%s\", snapshotBytes=%d)",
             metaObject()->className(), qPrintable(destination), qPrintable(source), static_cast<int>(snapshot.size()));
    if (error)
        *error = QStringLiteral("Backend does not support guarded move undo.");
    return false;
}

QtFileMgrPreviewRequest::QtFileMgrPreviewRequest(QObject *parent)
    : QObject(parent), m_cancelled(new QAtomicInt(0))
{
    connect(this, &QtFileMgrPreviewRequest::finished, this, &QObject::deleteLater);
}

QtFileMgrPreviewRequest::~QtFileMgrPreviewRequest() { cancel(); }

void QtFileMgrPreviewRequest::cancel() { m_cancelled->storeRelease(1); }

bool QtFileMgrPreviewRequest::isCancelled() const
{ return m_cancelled->loadAcquire() != 0; }

void QtFileMgrPreviewRequest::finish(const QByteArray &bytes, bool truncated,
                                     const QString &error)
{
    if (m_finished) {
        qWarning("[PreviewRequest] duplicate finish ignored (request=%p, bytes=%d)",
                 static_cast<void *>(this), bytes.size());
        return;
    }
    m_finished = true;
    if (isCancelled()) {
        qWarning("[PreviewRequest] cancelled result discarded (request=%p, bytes=%d, error=\"%s\")",
                 static_cast<void *>(this), bytes.size(), qPrintable(error));
        m_bytes.clear();
        m_truncated = false;
        m_error = QStringLiteral("预览已取消");
    } else {
        m_bytes = bytes;
        m_truncated = truncated;
        m_error = error;
    }
    emit finished(m_bytes, m_truncated, m_error);
}

QtFileMgrPreviewRequest *QtFileMgrIO::previewRequest(
    const QString &path, qint64 maxBytes, bool image, QObject *parent) const
{
    auto *request = new QtFileMgrPreviewRequest(parent);
    QPointer<const QtFileMgrIO> io(this);
    QTimer::singleShot(0, request, [io, request, path, maxBytes, image]() {
        if (request->isCancelled()) {
            qWarning("[QtFileMgrIO] previewRequest: cancelled legacy read (path=\"%s\")", qPrintable(path));
            request->finish({}, false, QStringLiteral("预览已取消"));
            return;
        }
        if (!io || io->thread() != QThread::currentThread()) {
            qWarning("[QtFileMgrIO] previewRequest: unavailable backend or wrong thread (path=\"%s\")", qPrintable(path));
            request->finish({}, false, QStringLiteral("文件后端不可用"));
            return;
        }
        if (maxBytes <= 0 || maxBytes > 4 * 1024 * 1024) {
            qWarning("[QtFileMgrIO] previewRequest: invalid limit (path=\"%s\", limit=%lld)",
                     qPrintable(path), static_cast<long long>(maxBytes));
            request->finish({}, false, QStringLiteral("预览大小上限无效"));
            return;
        }
        qWarning("[QtFileMgrIO] previewRequest: synchronous compatibility adapter (backend=%s, path=\"%s\")",
                 io->metaObject()->className(), qPrintable(path));
        bool truncated = false;
        QString error;
        const QByteArray bytes = io->readPreview(path, maxBytes, &truncated, &error);
        if (image && truncated) {
            qWarning("[QtFileMgrIO] previewRequest: image exceeds limit (path=\"%s\", limit=%lld)",
                     qPrintable(path), static_cast<long long>(maxBytes));
            error = QStringLiteral("图片超出预览大小上限");
        }
        request->finish(error.isEmpty() ? bytes : QByteArray(), truncated, error);
    });
    return request;
}

QtFileMgrDirectoryRequest::QtFileMgrDirectoryRequest(QObject *parent)
    : QObject(parent)
    , m_cancelled(new QAtomicInt(0))
{
    connect(this, &QtFileMgrDirectoryRequest::finished,
            this, &QObject::deleteLater);
}

QtFileMgrDirectoryRequest::~QtFileMgrDirectoryRequest()
{
    cancel();
}

void QtFileMgrDirectoryRequest::cancel()
{
    m_cancelled->storeRelease(1);
}

bool QtFileMgrDirectoryRequest::isCancelled() const
{
    return m_cancelled->loadAcquire() != 0;
}

void QtFileMgrDirectoryRequest::deliverBatch(
    const QList<FileEntry> &entries)
{
    if (m_finished) {
        qWarning("[QtFileMgrDirectoryRequest] deliverBatch: ignored batch after finish (request=%p, count=%d)",
                 static_cast<void *>(this), entries.size());
        return;
    }
    if (isCancelled()) {
        qWarning("[QtFileMgrDirectoryRequest] deliverBatch: ignored cancelled request (request=%p, count=%d)",
                 static_cast<void *>(this), entries.size());
        return;
    }
    if (!entries.isEmpty()) {
        m_deliveredEntries.append(entries);
        emit entriesReady(entries);
    }
}

void QtFileMgrDirectoryRequest::finish(const QString &error)
{
    if (m_finished) {
        qWarning("[QtFileMgrDirectoryRequest] finish: ignored duplicate finish (request=%p, error=\"%s\")",
                 static_cast<void *>(this), qPrintable(error));
        return;
    }
    m_error = error;
    m_finished = true;
    emit finished(error);
}

QtFileMgrTransferRequest::QtFileMgrTransferRequest(QObject *parent)
    : QObject(parent)
    , m_cancelled(new QAtomicInt(0))
{
    connect(this, &QtFileMgrTransferRequest::finished,
            this, &QObject::deleteLater);
}

QtFileMgrTransferRequest::~QtFileMgrTransferRequest()
{
    if (!m_finished)
        cancel();
}

void QtFileMgrTransferRequest::cancel()
{
    if (m_cancelled->testAndSetOrdered(0, 1))
        emit cancellationRequested();
}

bool QtFileMgrTransferRequest::isCancelled() const
{
    return m_cancelled->loadAcquire() != 0;
}

void QtFileMgrTransferRequest::reportProgress(qint64 bytesTransferred,
                                              qint64 bytesTotal)
{
    if (m_finished) {
        qWarning("[QtFileMgrTransferRequest] reportProgress: ignored progress after finish (request=%p, transferred=%lld, total=%lld)",
                 static_cast<void *>(this),
                 static_cast<long long>(bytesTransferred),
                 static_cast<long long>(bytesTotal));
        return;
    }
    if (bytesTransferred < 0 || bytesTotal < -1) {
        qWarning("[QtFileMgrTransferRequest] reportProgress: ignored invalid values (request=%p, transferred=%lld, total=%lld)",
                 static_cast<void *>(this),
                 static_cast<long long>(bytesTransferred),
                 static_cast<long long>(bytesTotal));
        return;
    }
    if (bytesTotal >= 0 && bytesTransferred > bytesTotal) {
        qWarning("[QtFileMgrTransferRequest] reportProgress: clamped transferred bytes (request=%p, transferred=%lld, total=%lld)",
                 static_cast<void *>(this),
                 static_cast<long long>(bytesTransferred),
                 static_cast<long long>(bytesTotal));
        bytesTransferred = bytesTotal;
    }
    emit progress(bytesTransferred, bytesTotal);
}

void QtFileMgrTransferRequest::finish(bool success, const QString &newPath,
                                      const QString &error)
{
    if (m_finished) {
        qWarning("[QtFileMgrTransferRequest] finish: ignored duplicate finish (request=%p, path=\"%s\", error=\"%s\")",
                 static_cast<void *>(this), qPrintable(newPath),
                 qPrintable(error));
        return;
    }
    m_finished = true;
    m_success = success;
    m_resultPath = newPath;
    m_error = error;
    emit finished(success, newPath, error, isCancelled());
}

QtFileMgrDirectoryRequest *QtFileMgrIO::listDirAsync(
    const QString &path, QObject *parent) const
{
    qWarning("[QtFileMgrIO] listDirAsync: using synchronous compatibility adapter (backend=%s, path=\"%s\")",
             metaObject()->className(), qPrintable(path));
    auto *request = new QtFileMgrDirectoryRequest(parent);
    QPointer<const QtFileMgrIO> io(this);
    QTimer::singleShot(0, request, [io, request, path]() {
        if (!io) {
            qWarning("[QtFileMgrIO] listDirAsync: backend destroyed before compatibility listing (path=\"%s\")",
                     qPrintable(path));
            request->finish(QStringLiteral("文件后端已断开"));
            return;
        }
        if (request->isCancelled()) {
            qWarning("[QtFileMgrIO] listDirAsync: skipped cancelled compatibility listing (backend=%s, path=\"%s\")",
                     io->metaObject()->className(), qPrintable(path));
            request->finish();
            return;
        }
        if (io->thread() != QThread::currentThread()) {
            qWarning("[QtFileMgrIO] listDirAsync: rejected cross-thread compatibility listing (backend=%s, backendThread=%p, currentThread=%p, path=\"%s\")",
                     io->metaObject()->className(),
                     static_cast<void *>(io->thread()),
                     static_cast<void *>(QThread::currentThread()),
                     qPrintable(path));
            request->finish(QStringLiteral("文件后端线程与调用线程不一致"));
            return;
        }
        request->deliverBatch(io->listDir(path));
        request->finish();
    });
    return request;
}

bool QtFileMgrIO::acceptsUrl(const QUrl &url) const
{
    const QString scheme = url.scheme().toLower();
    if (isRemote())
        return scheme == QStringLiteral("remote");
    return scheme.isEmpty() || scheme == QStringLiteral("file");
}

QUrl QtFileMgrIO::urlForPath(const QString &path) const
{
    if (!isRemote())
        return QUrl::fromLocalFile(QDir(path).absolutePath());

    QString normalized = QDir::cleanPath(path);
    if (normalized == QStringLiteral(".") || normalized == QStringLiteral("/"))
        normalized.clear();
    while (normalized.startsWith(QLatin1Char('/')))
        normalized.remove(0, 1);
    QUrl url(QStringLiteral("remote:///"));
    if (!normalized.isEmpty())
        url.setPath(QStringLiteral("/") + normalized);
    return url;
}

QString QtFileMgrIO::pathFromUrl(const QUrl &url) const
{
    if (!isRemote())
        return url.toLocalFile();

    QString path = QDir::cleanPath(url.path());
    if (path == QStringLiteral(".") || path == QStringLiteral("/"))
        return QString();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    return path;
}

bool QtFileMgrIO::removePermanent(const QString &path)
{
    qWarning("[QtFileMgrIO] removePermanent: falling back to remove (backend=%s, path=\"%s\")",
             metaObject()->className(), qPrintable(path));
    return remove(path);
}

bool QtFileMgrIO::moveToTrash(const QString &path, QString *error)
{
    if (error)
        error->clear();
    qWarning("[QtFileMgrIO] moveToTrash: rejected unsupported backend (backend=%s, path=\"%s\")",
             metaObject()->className(), qPrintable(path));
    if (error)
        *error = QStringLiteral("当前文件后端不支持回收站");
    return false;
}

bool QtFileMgrIO::writeFile(const QString &path, const QByteArray &contents)
{
    qWarning("[QtFileMgrIO] writeFile: backend '%s' does not support writing '%s' (%lld bytes)",
             metaObject()->className(), qPrintable(path),
             static_cast<long long>(contents.size()));
    return false;
}

QByteArray QtFileMgrIO::readPreview(const QString &path, qint64 maxBytes,
                                    bool *truncated, QString *error) const
{
    if (truncated)
        *truncated = false;
    if (error)
        *error = QStringLiteral("当前文件后端不支持文本预览");
    qWarning("[QtFileMgrIO] readPreview: backend '%s' does not support '%s' (limit=%lld)",
             metaObject()->className(), qPrintable(path),
             static_cast<long long>(maxBytes));
    return {};
}

static QString normalizedPath(QString path)
{
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    while (path.startsWith(QStringLiteral("./")))
        path.remove(0, 2);
    return path;
}

static QString parentDirOf(const QString &path)
{
    QString p = normalizedPath(path);
    int slash = p.lastIndexOf(QLatin1Char('/'));
    return slash > 0 ? p.left(slash) : QString();
}

/// 生成不冲突的目标文件名 (dir/name → dir/name (1), dir/name (2) ...)
QString QtFileMgrIO::uniqueDestName(const QString &destDir, const QString &name, QtFileMgrIO *io)
{
    // 处理根目录 (destDir 为空): 避免构造出 "/name" 这种错误路径
    QString prefix = destDir.isEmpty() ? QString() : destDir + QLatin1Char('/');
    QString base = prefix + QFileInfo(name).completeBaseName();
    QString ext  = QFileInfo(name).suffix();
    QString dest = prefix + name;
    int n = 1;
    while (io && io->exists(dest))
        dest = base + QStringLiteral(" (%1)").arg(n++)
               + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    return dest;
}

void QtFileMgrIO::copyAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb)
{
    bool ok = copy(src, dest);
    QTimer::singleShot(0, ctx, [cb, ok, dest]() { cb(ok, dest); });
}

void QtFileMgrIO::moveAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb)
{
    bool ok = rename(src, dest);
    QTimer::singleShot(0, ctx, [cb, ok, dest]() { cb(ok, dest); });
}

void QtFileMgrIO::renameAsync(const QString &oldPath, const QString &newPath,
                              QObject *ctx, OpCallback cb)
{
    // 默认适配同步 rename,经事件循环投递,调用方立即返回 (DLG-P1-5);
    // 后端必须先于 timer 销毁时安全失败,不得调用悬空对象 (R2-06)
    const QPointer<QtFileMgrIO> io(this);
    QTimer::singleShot(0, ctx, [io, oldPath, newPath, cb]() {
        if (!io) {
            qWarning("[QtFileMgrIO] renameAsync: backend destroyed before execution (oldPath=\"%s\", newPath=\"%s\")",
                     qPrintable(oldPath), qPrintable(newPath));
            cb(false, newPath);
            return;
        }
        cb(io->rename(oldPath, newPath), newPath);
    });
}

void QtFileMgrIO::versionsAsync(const QString &path, QObject *ctx,
                                VersionsCallback callback)
{
    qWarning("[QtFileMgrIO] versionsAsync: using synchronous compatibility adapter (backend=%s, path=\"%s\")",
             metaObject()->className(), qPrintable(path));
    QPointer<QtFileMgrIO> io(this);
    QTimer::singleShot(0, ctx, [io, path, callback]() {
        if (!io) {
            qWarning("[QtFileMgrIO] versionsAsync: backend destroyed before fallback query (path=\"%s\")",
                     qPrintable(path));
            callback({}, QStringLiteral("文件后端已断开"));
            return;
        }
        callback(io->versions(path), QString());
    });
}

void QtFileMgrIO::restoreVersionAsync(const QString &path, int version,
                                      QObject *ctx, ResultCallback callback)
{
    qWarning("[QtFileMgrIO] restoreVersionAsync: using synchronous compatibility adapter (backend=%s, path=\"%s\", version=%d)",
             metaObject()->className(), qPrintable(path), version);
    QPointer<QtFileMgrIO> io(this);
    QTimer::singleShot(0, ctx, [io, path, version, callback]() {
        if (!io) {
            qWarning("[QtFileMgrIO] restoreVersionAsync: backend destroyed before fallback restore (path=\"%s\", version=%d)",
                     qPrintable(path), version);
            callback(false, QStringLiteral("文件后端已断开"));
            return;
        }
        const bool ok = io->restoreVersion(path, version);
        callback(ok, ok ? QString() : QStringLiteral("恢复版本失败"));
    });
}

QtFileMgrTransferRequest *QtFileMgrIO::copyRequest(
    const QString &src, const QString &dest, QObject *parent)
{
    qWarning("[QtFileMgrIO] copyRequest: using legacy callback adapter (backend=%s, source=\"%s\", destination=\"%s\")",
             metaObject()->className(), qPrintable(src), qPrintable(dest));
    auto *request = new QtFileMgrTransferRequest(parent);
    QPointer<QtFileMgrTransferRequest> guarded(request);
    copyAsync(src, dest, request,
              [guarded, src](bool success, const QString &newPath) {
        if (!guarded) {
            qWarning("[QtFileMgrIO] copyRequest: legacy callback arrived after request destruction (source=\"%s\")",
                     qPrintable(src));
            return;
        }
        guarded->finish(success, newPath,
                        success ? QString() : QStringLiteral("复制失败"));
    });
    return request;
}

QtFileMgrTransferRequest *QtFileMgrIO::moveRequest(
    const QString &src, const QString &dest, QObject *parent)
{
    qWarning("[QtFileMgrIO] moveRequest: using legacy callback adapter (backend=%s, source=\"%s\", destination=\"%s\")",
             metaObject()->className(), qPrintable(src), qPrintable(dest));
    auto *request = new QtFileMgrTransferRequest(parent);
    QPointer<QtFileMgrTransferRequest> guarded(request);
    moveAsync(src, dest, request,
              [guarded, src](bool success, const QString &newPath) {
        if (!guarded) {
            qWarning("[QtFileMgrIO] moveRequest: legacy callback arrived after request destruction (source=\"%s\")",
                     qPrintable(src));
            return;
        }
        guarded->finish(success, newPath,
                        success ? QString() : QStringLiteral("移动失败"));
    });
    return request;
}

QtFileMgrTransferRequest *QtFileMgrIO::exportToLocalRequest(
    const QString &source, const QString &localDestination, QObject *parent)
{
    qWarning("[QtFileMgrIO] exportToLocalRequest: backend does not support local export (backend=%s, source=\"%s\", destination=\"%s\")",
             metaObject()->className(), qPrintable(source),
             qPrintable(localDestination));
    auto *request = new QtFileMgrTransferRequest(parent);
    QTimer::singleShot(0, request, [request, source]() {
        request->finish(false, source,
                        QStringLiteral("当前文件后端不支持下载到本地"));
    });
    return request;
}

QtFileMgrTransferRequest *QtFileMgrIO::importFromLocalRequest(
    const QString &localSource, const QString &remoteDestination,
    bool move, QObject *parent)
{
    // 旧后端适配: 保持原 copyRequest/moveRequest 行为 (审查#1)
    qWarning("[QtFileMgrIO] importFromLocalRequest: using legacy adapter (backend=%s, source=\"%s\", destination=\"%s\", move=%d)",
             metaObject()->className(), qPrintable(localSource),
             qPrintable(remoteDestination), move ? 1 : 0);
    return move ? moveRequest(localSource, remoteDestination, parent)
                : copyRequest(localSource, remoteDestination, parent);
}

void QtFileMgrIO::batchCopyAsync(const QStringList &srcs, const QString &destDir,
                                  QObject *ctx, BatchCallback cb)
{
    int ok = 0, fail = 0;
    QStringList newNames;
    // 处理根目录 (destDir 为空): 避免构造出 "/name"
    QString prefix = destDir.isEmpty() ? QString() : destDir + QLatin1Char('/');
    for (const QString &src : srcs) {
        if (src.isEmpty()) {
            qWarning("[QtFileMgrIO] batchCopyAsync: skipped empty source (destination=\"%s\")",
                     qPrintable(destDir));
            continue;
        }
        QString name = QFileInfo(src).fileName();
        if (name.isEmpty()) {
            qWarning("[QtFileMgrIO] batchCopyAsync: skipped source without file name (source=\"%s\", destination=\"%s\")",
                     qPrintable(src), qPrintable(destDir));
            continue;
        }
        QString dest = prefix + name;
        if (src == dest) dest = uniqueDestName(destDir, name, this);
        else if (exists(dest)) dest = uniqueDestName(destDir, name, this);
        if (copy(src, dest)) { ok++; newNames << QFileInfo(dest).fileName(); }
        else fail++;
    }
    QTimer::singleShot(0, ctx, [cb, ok, fail, newNames]() { cb(ok, fail, newNames); });
}

void QtFileMgrIO::batchMoveAsync(const QStringList &srcs, const QString &destDir,
                                  QObject *ctx, BatchCallback cb)
{
    int ok = 0, fail = 0;
    QStringList newNames;
    for (const QString &src : srcs) {
        if (src.isEmpty()) {
            qWarning("[QtFileMgrIO] batchMoveAsync: skipped empty source (destination=\"%s\")",
                     qPrintable(destDir));
            continue;
        }
        QString name = QFileInfo(src).fileName();
        if (name.isEmpty()) {
            qWarning("[QtFileMgrIO] batchMoveAsync: skipped source without file name (source=\"%s\", destination=\"%s\")",
                     qPrintable(src), qPrintable(destDir));
            continue;
        }
        if (parentDirOf(src) == normalizedPath(destDir)) {
            qWarning("[QtFileMgrIO] batchMoveAsync: skipped source already in destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(src), qPrintable(destDir));
            continue;
        }
        QString dest = uniqueDestName(destDir, name, this);
        if (rename(src, dest)) { ok++; newNames << QFileInfo(dest).fileName(); }
        else fail++;
    }
    QTimer::singleShot(0, ctx, [cb, ok, fail, newNames]() { cb(ok, fail, newNames); });
}

QtFileMgrIO::Capabilities QtFileMgrIO::capabilities() const
{
    Capabilities result = ReadCapability;
    if (canWrite())
        result |= WriteCapability;
    if (hasVersions())
        result |= VersionsCapability;
    if (supportsVersionRestore())
        result |= VersionsCapability | VersionRestoreCapability;
    if (hasThumbnails())
        result |= ThumbnailCapability;
    if (supportsSymlinks())
        result |= SymlinkCapability;
    if (supportsPermissions())
        result |= PermissionCapability;
    if (supportsMimeTypes())
        result |= MimeTypeCapability;
    if (isRemote())
        result |= RemoteCapability;
    return result;
}
