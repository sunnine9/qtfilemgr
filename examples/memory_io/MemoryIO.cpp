// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "MemoryIO.h"

#include <QDebug>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <utility>

namespace {

const qint64 kMaximumPreviewBytes = 4 * 1024 * 1024;
const char kReadOnlyError[] = "The memory filesystem is read-only.";

QString parentPath(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash == 0 ? QStringLiteral("/") : path.left(slash);
}

QString leafName(const QString &path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QDateTime sampleTimestamp()
{
    return QDateTime::fromSecsSinceEpoch(1704067200, Qt::UTC);
}

bool validPreviewLimit(const QString &path, qint64 limit, const char *source,
                       QString *error)
{
    if (limit > 0 && limit <= kMaximumPreviewBytes)
        return true;
    qWarning("[MemoryIO] %s: rejected preview limit (source=memory, path=\"%s\", limit=%lld, maximum=%lld)",
             source, qPrintable(path), static_cast<long long>(limit),
             static_cast<long long>(kMaximumPreviewBytes));
    if (error)
        *error = QStringLiteral("Preview limit must be between 1 byte and 4 MiB.");
    return false;
}

} // namespace

MemoryIO::MemoryIO(QObject *parent) : QtFileMgrIO(parent)
{
    m_nodes.insert(QStringLiteral("/"), {true, {}, QStringLiteral("inode/directory")});
    m_nodes.insert(QStringLiteral("/documents"), {true, {}, QStringLiteral("inode/directory")});
    m_nodes.insert(QStringLiteral("/images"), {true, {}, QStringLiteral("inode/directory")});
    m_nodes.insert(QStringLiteral("/empty"), {true, {}, QStringLiteral("inode/directory")});
    m_nodes.insert(QStringLiteral("/documents/readme.txt"),
        {false, QByteArrayLiteral("Welcome to the MemoryIO example.\n\n"
            "These files are immutable in-memory resources.\n"
            "Their memory:/// URLs never refer to local disk paths.\n"
            "Try selecting a file, opening a preview, and navigating back.\n"),
            QStringLiteral("text/plain")});
    m_nodes.insert(QStringLiteral("/documents/config.json"),
        {false, QByteArrayLiteral("{\n  \"backend\": \"memory\",\n  \"readOnly\": true,\n"
                                 "  \"protocol\": \"memory:///\"\n}\n"),
            QStringLiteral("application/json")});
    m_nodes.insert(QStringLiteral("/documents/large.txt"),
        {false, QByteArray(160 * 1024, 'L'), QStringLiteral("text/plain")});
    // PPM is decoded by Qt Gui itself; the example needs no image-format plugin.
    m_nodes.insert(QStringLiteral("/images/checker.ppm"),
        {false, QByteArrayLiteral("P3\n4 4\n255\n"
            "46 112 190  46 112 190  232 240 250  232 240 250\n"
            "46 112 190  46 112 190  232 240 250  232 240 250\n"
            "232 240 250  232 240 250  46 112 190  46 112 190\n"
            "232 240 250  232 240 250  46 112 190  46 112 190\n"),
            QStringLiteral("image/x-portable-pixmap")});
}

QString MemoryIO::rootPath() const { return QStringLiteral("/"); }
bool MemoryIO::canWrite() const { return false; }
bool MemoryIO::isRemote() const { return true; }
bool MemoryIO::supportsMimeTypes() const { return true; }

QtFileMgrIO::Capabilities MemoryIO::capabilities() const
{
    return ReadCapability | NonBlockingListingCapability
        | NonBlockingPreviewCapability | MimeTypeCapability | RemoteCapability;
}

bool MemoryIO::validatePath(const QString &path, const char *source, QString *error)
{
    bool valid = !path.isEmpty() && path.startsWith(QLatin1Char('/'))
        && !path.contains(QLatin1Char('\\')) && !path.contains(QChar::Null)
        && !path.contains(QStringLiteral("//"))
        && (path == QStringLiteral("/") || !path.endsWith(QLatin1Char('/')));
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments)
        valid = valid && segment != QStringLiteral(".") && segment != QStringLiteral("..");
    if (valid)
        return true;
    qWarning("[MemoryIO] %s: rejected non-canonical path (source=memory, path=\"%s\")",
             source, qPrintable(path));
    if (error)
        *error = QStringLiteral("Expected a canonical absolute memory path: %1").arg(path);
    return false;
}

bool MemoryIO::validateUrl(const QUrl &url, const char *source)
{
    const QByteArray encoded = url.toEncoded(QUrl::FullyEncoded);
    const QByteArray encodedPath = url.path(QUrl::FullyEncoded).toLatin1().toLower();
    const bool valid = url.isValid() && !url.isRelative()
        && url.scheme() == QStringLiteral("memory")
        && encoded.startsWith("memory:///") && url.authority().isEmpty()
        && !url.hasQuery() && !url.hasFragment()
        && !encodedPath.contains("%2f") && !encodedPath.contains("%5c");
    if (!valid) {
        qWarning("[MemoryIO] %s: rejected URL (source=memory, url=\"%s\")",
                 source, qPrintable(url.toString(QUrl::RemovePassword)));
        return false;
    }
    return validatePath(url.path(), source);
}

bool MemoryIO::acceptsUrl(const QUrl &url) const
{
    return validateUrl(url, "acceptsUrl");
}

QUrl MemoryIO::urlForPath(const QString &path) const
{
    if (!validatePath(path, "urlForPath"))
        return {};
    QUrl url(QStringLiteral("memory:///"));
    url.setPath(path);
    return url;
}

QString MemoryIO::pathFromUrl(const QUrl &url) const
{
    return validateUrl(url, "pathFromUrl") ? url.path() : QString();
}

const MemoryIO::Node *MemoryIO::findNode(const QString &path, const char *source,
                                        QString *error) const
{
    if (!validatePath(path, source, error))
        return nullptr;
    const auto found = m_nodes.constFind(path);
    if (found != m_nodes.constEnd())
        return &found.value();
    qWarning("[MemoryIO] %s: resource does not exist (source=memory, path=\"%s\")",
             source, qPrintable(path));
    if (error)
        *error = QStringLiteral("Memory resource does not exist: %1").arg(path);
    return nullptr;
}

QString MemoryIO::displayName(const QString &path) const
{
    if (!findNode(path, "displayName"))
        return {};
    return path == QStringLiteral("/") ? QStringLiteral("Memory") : leafName(path);
}

bool MemoryIO::exists(const QString &path) const
{
    return findNode(path, "exists") != nullptr;
}

bool MemoryIO::isDir(const QString &path) const
{
    const Node *node = findNode(path, "isDir");
    return node && node->directory;
}

bool MemoryIO::isWritable(const QString &path) const
{
    Q_UNUSED(path);
    return false;
}

FileEntry MemoryIO::stat(const QString &path) const
{
    const Node *node = findNode(path, "stat");
    if (!node)
        return {};
    FileEntry entry;
    entry.name = path == QStringLiteral("/") ? QStringLiteral("Memory") : leafName(path);
    entry.path = path;
    entry.size = node->bytes.size();
    entry.modified = sampleTimestamp();
    entry.isDir = node->directory;
    entry.isReadOnly = true;
    entry.permissionBits = QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther;
    return entry;
}

qint64 MemoryIO::fileSize(const QString &path) const
{
    const Node *node = findNode(path, "fileSize");
    return node ? node->bytes.size() : -1;
}

QDateTime MemoryIO::modificationTime(const QString &path) const
{
    return findNode(path, "modificationTime") ? sampleTimestamp() : QDateTime();
}

QString MemoryIO::mimeType(const QString &path) const
{
    const Node *node = findNode(path, "mimeType");
    return node ? node->mime : QString();
}

bool MemoryIO::directoryEntries(const QString &path, const char *source,
                                 QList<FileEntry> *entries, QString *error) const
{
    const Node *node = findNode(path, source, error);
    if (!node)
        return false;
    if (!node->directory) {
        qWarning("[MemoryIO] %s: rejected file as directory (source=memory, path=\"%s\")",
                 source, qPrintable(path));
        if (error)
            *error = QStringLiteral("Memory resource is not a directory: %1").arg(path);
        return false;
    }
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        if (it.key() != path && parentPath(it.key()) == path)
            entries->append(stat(it.key()));
    }
    return true;
}

QList<FileEntry> MemoryIO::listDir(const QString &path) const
{
    QList<FileEntry> entries;
    QString error;
    directoryEntries(path, "listDir", &entries, &error);
    return entries;
}

QtFileMgrDirectoryRequest *MemoryIO::listDirAsync(const QString &path, QObject *parent) const
{
    auto *request = new QtFileMgrDirectoryRequest(parent);
    const QPointer<const MemoryIO> backend(this);
    const auto completed = QSharedPointer<bool>::create(false);
    QObject::connect(request, &QObject::destroyed, [path, completed]() {
        if (!*completed)
            qWarning("[MemoryIO] listDirAsync: request destroyed before completion (source=memory, path=\"%s\")",
                     qPrintable(path));
    });
    QTimer::singleShot(0, request, [backend, request, path, completed]() {
        if (request->isFinished()) {
            *completed = true;
            qWarning("[MemoryIO] listDirAsync: discarded work for completed request (source=memory, path=\"%s\")",
                     qPrintable(path));
            return;
        }
        QString error;
        QList<FileEntry> entries;
        if (request->isCancelled()) {
            qWarning("[MemoryIO] listDirAsync: cancelled listing discarded (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory directory request was cancelled.");
        } else if (!backend || backend->thread() != QThread::currentThread()) {
            qWarning("[MemoryIO] listDirAsync: backend unavailable or thread mismatch (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory backend is no longer available on this thread.");
        } else {
            backend->directoryEntries(path, "listDirAsync", &entries, &error);
        }
        const QPointer<QtFileMgrDirectoryRequest> liveRequest(request);
        if (error.isEmpty())
            request->deliverBatch(entries);
        if (!liveRequest) {
            qWarning("[MemoryIO] listDirAsync: request removed while delivering batch (source=memory, path=\"%s\", count=%d)",
                     qPrintable(path), static_cast<int>(entries.size()));
            return;
        }
        if (request->isCancelled() && error.isEmpty()) {
            qWarning("[MemoryIO] listDirAsync: cancelled after batch delivery (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory directory request was cancelled.");
        }
        if (!backend && error.isEmpty()) {
            qWarning("[MemoryIO] listDirAsync: backend destroyed during batch delivery (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory backend was destroyed during directory delivery.");
        }
        *completed = true;
        request->finish(error);
    });
    return request;
}

QByteArray MemoryIO::readPreview(const QString &path, qint64 maxBytes,
                                 bool *truncated, QString *error) const
{
    if (truncated)
        *truncated = false;
    if (error)
        error->clear();
    if (!validPreviewLimit(path, maxBytes, "readPreview", error))
        return {};
    const Node *node = findNode(path, "readPreview", error);
    if (!node)
        return {};
    if (node->directory) {
        qWarning("[MemoryIO] readPreview: rejected directory (source=memory, path=\"%s\")",
                 qPrintable(path));
        if (error)
            *error = QStringLiteral("Cannot preview a memory directory: %1").arg(path);
        return {};
    }
    if (node->bytes.size() > maxBytes) {
        qWarning("[MemoryIO] readPreview: truncated resource (source=memory, path=\"%s\", bytes=%lld, limit=%lld)",
                 qPrintable(path), static_cast<long long>(node->bytes.size()),
                 static_cast<long long>(maxBytes));
        if (truncated)
            *truncated = true;
    }
    return node->bytes.left(static_cast<int>(maxBytes));
}

QtFileMgrPreviewRequest *MemoryIO::previewRequest(const QString &path, qint64 maxBytes,
                                                  bool image, QObject *parent) const
{
    auto *request = new QtFileMgrPreviewRequest(parent);
    const QPointer<const MemoryIO> backend(this);
    const auto completed = QSharedPointer<bool>::create(false);
    QObject::connect(request, &QObject::destroyed, [path, completed]() {
        if (!*completed)
            qWarning("[MemoryIO] previewRequest: request destroyed before completion (source=memory, path=\"%s\")",
                     qPrintable(path));
    });
    QTimer::singleShot(0, request, [backend, request, path, maxBytes, image, completed]() {
        if (request->isFinished()) {
            *completed = true;
            qWarning("[MemoryIO] previewRequest: discarded work for completed request (source=memory, path=\"%s\")",
                     qPrintable(path));
            return;
        }
        QByteArray bytes;
        QString error;
        bool truncated = false;
        if (request->isCancelled()) {
            qWarning("[MemoryIO] previewRequest: cancelled read discarded (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory preview request was cancelled.");
        } else if (!backend || backend->thread() != QThread::currentThread()) {
            qWarning("[MemoryIO] previewRequest: backend unavailable or thread mismatch (source=memory, path=\"%s\")",
                     qPrintable(path));
            error = QStringLiteral("Memory backend is no longer available on this thread.");
        } else {
            bytes = backend->readPreview(path, maxBytes, &truncated, &error);
            if (error.isEmpty() && image && !backend->mimeType(path).startsWith(QStringLiteral("image/"))) {
                qWarning("[MemoryIO] previewRequest: rejected non-image resource (source=memory, path=\"%s\")",
                         qPrintable(path));
                error = QStringLiteral("Memory resource is not an image: %1").arg(path);
            } else if (error.isEmpty() && image && truncated) {
                qWarning("[MemoryIO] previewRequest: image exceeds limit (source=memory, path=\"%s\", limit=%lld)",
                         qPrintable(path), static_cast<long long>(maxBytes));
                error = QStringLiteral("Memory image exceeds the preview limit: %1").arg(path);
            }
        }
        *completed = true;
        request->finish(error.isEmpty() ? bytes : QByteArray(),
                        error.isEmpty() && truncated, error);
    });
    return request;
}

bool MemoryIO::rejectWrite(const char *operation, const QString &path,
                            const QString &destination)
{
    qWarning("[MemoryIO] %s: write rejected (source=memory, path=\"%s\", destination=\"%s\")",
             operation, qPrintable(path), qPrintable(destination));
    return false;
}

bool MemoryIO::mkdir(const QString &path) { return rejectWrite("mkdir", path); }
bool MemoryIO::rename(const QString &a, const QString &b) { return rejectWrite("rename", a, b); }
bool MemoryIO::remove(const QString &path) { return rejectWrite("remove", path); }
bool MemoryIO::removePermanent(const QString &path) { return rejectWrite("removePermanent", path); }
bool MemoryIO::copy(const QString &a, const QString &b) { return rejectWrite("copy", a, b); }
bool MemoryIO::moveToTrash(const QString &path, QString *error)
{
    if (error)
        *error = QString::fromLatin1(kReadOnlyError);
    return rejectWrite("moveToTrash", path);
}
bool MemoryIO::writeFile(const QString &path, const QByteArray &contents)
{
    Q_UNUSED(contents);
    return rejectWrite("writeFile", path);
}
bool MemoryIO::chmod(const QString &path, QFile::Permissions permissions)
{
    Q_UNUSED(permissions);
    return rejectWrite("chmod", path);
}
bool MemoryIO::symlink(const QString &target, const QString &linkPath)
{
    return rejectWrite("symlink", target, linkPath);
}
bool MemoryIO::restoreVersion(const QString &path, int version)
{
    qWarning("[MemoryIO] restoreVersion: write rejected (source=memory, path=\"%s\", version=%d)",
             qPrintable(path), version);
    return false;
}
void MemoryIO::restoreVersionAsync(const QString &path, int version, QObject *context,
                                    ResultCallback callback)
{
    Q_UNUSED(context);
    restoreVersion(path, version);
    if (callback)
        callback(false, QString::fromLatin1(kReadOnlyError));
    else
        qWarning("[MemoryIO] restoreVersionAsync: missing callback (source=memory, path=\"%s\", version=%d)",
                 qPrintable(path), version);
}
void MemoryIO::rejectOperation(const char *operation, const QString &source,
                                const QString &destination, OpCallback callback)
{
    rejectWrite(operation, source, destination);
    if (callback)
        callback(false, QString());
    else
        qWarning("[MemoryIO] %s: missing callback (source=memory, path=\"%s\", destination=\"%s\")",
                 operation, qPrintable(source), qPrintable(destination));
}
void MemoryIO::renameAsync(const QString &a, const QString &b, QObject *context, OpCallback callback)
{
    Q_UNUSED(context);
    rejectOperation("renameAsync", a, b, std::move(callback));
}
void MemoryIO::copyAsync(const QString &a, const QString &b, QObject *context, OpCallback callback)
{
    Q_UNUSED(context);
    rejectOperation("copyAsync", a, b, std::move(callback));
}
void MemoryIO::moveAsync(const QString &a, const QString &b, QObject *context, OpCallback callback)
{
    Q_UNUSED(context);
    rejectOperation("moveAsync", a, b, std::move(callback));
}
void MemoryIO::rejectBatch(const char *operation, const QStringList &sources,
                            const QString &destination, BatchCallback callback)
{
    qWarning("[MemoryIO] %s: batch write rejected (source=memory, paths=\"%s\", destination=\"%s\")",
             operation, qPrintable(sources.join(QStringLiteral(", "))), qPrintable(destination));
    if (callback)
        callback(0, static_cast<int>(sources.size()), {});
    else
        qWarning("[MemoryIO] %s: missing callback (source=memory, paths=\"%s\", destination=\"%s\")",
                 operation, qPrintable(sources.join(QStringLiteral(", "))), qPrintable(destination));
}
void MemoryIO::batchCopyAsync(const QStringList &sources, const QString &destination,
                               QObject *context, BatchCallback callback)
{
    Q_UNUSED(context);
    rejectBatch("batchCopyAsync", sources, destination, std::move(callback));
}
void MemoryIO::batchMoveAsync(const QStringList &sources, const QString &destination,
                               QObject *context, BatchCallback callback)
{
    Q_UNUSED(context);
    rejectBatch("batchMoveAsync", sources, destination, std::move(callback));
}
QtFileMgrTransferRequest *MemoryIO::rejectTransfer(const char *operation, const QString &source,
                                                    const QString &destination, QObject *parent)
{
    rejectWrite(operation, source, destination);
    auto *request = new QtFileMgrTransferRequest(parent);
    const auto completed = QSharedPointer<bool>::create(false);
    QObject::connect(request, &QObject::destroyed, [source, destination, completed]() {
        if (!*completed)
            qWarning("[MemoryIO] rejected transfer: request destroyed before completion (source=memory, path=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
    });
    QTimer::singleShot(0, request, [request, completed, source, destination]() {
        *completed = true;
        if (request->isFinished()) {
            qWarning("[MemoryIO] rejected transfer: completion already delivered (source=memory, path=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return;
        }
        request->finish(false, {}, QString::fromLatin1(kReadOnlyError));
    });
    return request;
}
QtFileMgrTransferRequest *MemoryIO::copyRequest(const QString &a, const QString &b, QObject *parent)
{
    return rejectTransfer("copyRequest", a, b, parent);
}
QtFileMgrTransferRequest *MemoryIO::moveRequest(const QString &a, const QString &b, QObject *parent)
{
    return rejectTransfer("moveRequest", a, b, parent);
}
QtFileMgrTransferRequest *MemoryIO::exportToLocalRequest(const QString &a, const QString &b,
                                                        QObject *parent)
{
    return rejectTransfer("exportToLocalRequest", a, b, parent);
}
QtFileMgrTransferRequest *MemoryIO::importFromLocalRequest(const QString &a, const QString &b,
                                                          bool move, QObject *parent)
{
    return rejectTransfer(move ? "importFromLocalRequest(move)" : "importFromLocalRequest(copy)",
                           a, b, parent);
}
