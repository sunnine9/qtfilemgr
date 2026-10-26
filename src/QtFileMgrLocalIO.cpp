// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

//
#include "QtFileMgrLocalIO.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSet>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QCoreApplication>
#include <QFile>
#include <QDebug>
#include <QImageReader>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QUuid>
#include <cstring>
#include <functional>
#include <limits>
#include <pwd.h>
#include <grp.h>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#include <dirent.h>
#endif
#endif

bool QtFileMgrLocalIO::renameNoReplace(const QString &source, const QString &destination,
                                      QString *error)
{
#if defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const QByteArray from = QFile::encodeName(source);
    const QByteArray to = QFile::encodeName(destination);
    if (::syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD,
                  to.constData(), 1 /* RENAME_NOREPLACE */) == 0)
        return true;
    const int failure = errno;
    qWarning("[LocalIO] renameNoReplace: refused or failed (source=\"%s\", destination=\"%s\", errno=%d, error=\"%s\")",
             qPrintable(source), qPrintable(destination), failure, std::strerror(failure));
    if (error)
        *error = QString::fromLocal8Bit(std::strerror(failure));
#else
    qWarning("[LocalIO] renameNoReplace: atomic no-replace rename unavailable (source=\"%s\", destination=\"%s\")",
             qPrintable(source), qPrintable(destination));
    if (error)
        *error = QStringLiteral("Atomic no-replace rename is unavailable on this platform.");
#endif
    return false;
}

static QString rawLinkTarget(const QString &path)
{
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(path);
    QByteArray target(256, Qt::Uninitialized);
    for (;;) {
        const ssize_t length = ::readlink(encoded.constData(), target.data(),
                                          static_cast<size_t>(target.size()));
        if (length < 0)
            return {};
        if (length < target.size()) {
            target.truncate(static_cast<int>(length));
            return QFile::decodeName(target);
        }
        target.resize(target.size() * 2);
    }
#else
    return QFile::readLink(path);
#endif
}

static bool createSymbolicLink(const QString &target, const QString &path)
{
#ifdef Q_OS_UNIX
    return ::symlink(QFile::encodeName(target).constData(),
                     QFile::encodeName(path).constData()) == 0;
#else
    return QFile::link(target, path);
#endif
}

QtFileMgrLocalIO::QtFileMgrLocalIO(QObject *parent)
    : QtFileMgrIO(parent)
{
}

namespace {

// This first implementation deliberately refuses large/unsupported objects.
// It never treats a partial hash or a directory's own mtime as a valid proof.
constexpr qint64 kUndoMaximumBytes = 8 * 1024 * 1024;
constexpr int kUndoMaximumNodes = 4096;
constexpr int kUndoMaximumDepth = 64;
constexpr int kUndoMaximumMilliseconds = 50;
const QByteArray kUndoMagic("QFMUNDO2");

#ifdef Q_OS_LINUX
QByteArray undoMetadata(const struct stat &s, bool omitChangeTime)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << quint64(s.st_dev) << quint64(s.st_ino) << quint64(s.st_mode)
        << quint64(s.st_uid) << quint64(s.st_gid) << quint64(s.st_nlink)
        << qint64(s.st_size) << qint64(s.st_mtim.tv_sec) << qint64(s.st_mtim.tv_nsec)
        << qint64(omitChangeTime ? 0 : s.st_ctim.tv_sec)
        << qint64(omitChangeTime ? 0 : s.st_ctim.tv_nsec);
    return bytes;
}

struct UndoScan {
    QCryptographicHash exact{QCryptographicHash::Sha256};
    QCryptographicHash relocated{QCryptographicHash::Sha256};
    QElapsedTimer timer;
    qint64 bytes = 0;
    int nodes = 0;
    QString error;

    UndoScan() { timer.start(); }
    bool fail(const QString &path, const QString &reason)
    {
        error = reason + QStringLiteral(" [") + path + QLatin1Char(']');
        qWarning("[LocalIO] undo snapshot refused (path=\"%s\", reason=\"%s\", nodes=%d, bytes=%lld, elapsedMs=%lld)",
                 qPrintable(path), qPrintable(reason), nodes, static_cast<long long>(bytes),
                 static_cast<long long>(timer.elapsed()));
        return false;
    }
    bool scan(const QString &path, const QString &relative, int depth)
    {
        if (++nodes > kUndoMaximumNodes || depth > kUndoMaximumDepth
            || timer.elapsed() > kUndoMaximumMilliseconds)
            return fail(path, QStringLiteral("安全校验超限，本项不支持撤销 (4096 nodes / depth 64 / 50 ms)"));
        const QByteArray encoded = QFile::encodeName(path);
        struct stat before{}, after{};
        if (::lstat(encoded.constData(), &before) != 0)
            return fail(path, QStringLiteral("Cannot lstat undo object: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        QByteArray content;
        if (S_ISREG(before.st_mode)) {
            if (before.st_size < 0 || bytes + before.st_size > kUndoMaximumBytes)
                return fail(path, QStringLiteral("安全校验超限，本项不支持撤销 (8 MiB total contents)"));
            const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0)
                return fail(path, QStringLiteral("Cannot open undo file without following links"));
            struct stat opened{};
            if (::fstat(fd, &opened) != 0 || undoMetadata(opened, false) != undoMetadata(before, false)) {
                ::close(fd);
                return fail(path, QStringLiteral("Object changed while opening undo file"));
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            char block[64 * 1024];
            qint64 readBytes = 0;
            for (;;) {
                const ssize_t amount = ::read(fd, block, sizeof(block));
                if (amount < 0) {
                    ::close(fd);
                    return fail(path, QStringLiteral("Cannot read complete undo file"));
                }
                if (!amount)
                    break;
                readBytes += amount;
                bytes += amount;
                if (bytes > kUndoMaximumBytes || timer.elapsed() > kUndoMaximumMilliseconds) {
                    ::close(fd);
                    return fail(path, QStringLiteral("安全校验超限，本项不支持撤销 (8 MiB / 50 ms)"));
                }
                hash.addData(block, static_cast<int>(amount));
            }
            const bool stable = ::fstat(fd, &opened) == 0
                && undoMetadata(opened, false) == undoMetadata(before, false)
                && readBytes == before.st_size;
            ::close(fd);
            if (!stable)
                return fail(path, QStringLiteral("File changed while computing undo revision"));
            content = hash.result();
        } else if (S_ISLNK(before.st_mode)) {
            char target[4097];
            const ssize_t length = ::readlink(encoded.constData(), target, sizeof(target));
            if (length < 0 || length >= static_cast<ssize_t>(sizeof(target)))
                return fail(path, QStringLiteral("Cannot read bounded symbolic-link target"));
            content = QByteArray(target, static_cast<int>(length));
        } else if (S_ISDIR(before.st_mode)) {
            QStringList children;
            DIR *directory = ::opendir(encoded.constData());
            if (!directory)
                return fail(path, QStringLiteral("Cannot open directory for complete undo enumeration"));
            errno = 0;
            while (dirent *entry = ::readdir(directory)) {
                const QByteArray name(entry->d_name);
                if (name == "." || name == "..")
                    continue;
                if (QFile::encodeName(QFile::decodeName(name)) != name) {
                    ::closedir(directory);
                    return fail(path, QStringLiteral("Unrepresentable filename bytes in undo tree: %1")
                        .arg(QString::fromLatin1(name.toHex())));
                }
                children.append(QDir(path).absoluteFilePath(QFile::decodeName(name)));
                if (children.size() + nodes > kUndoMaximumNodes || timer.elapsed() > kUndoMaximumMilliseconds) {
                    ::closedir(directory);
                    return fail(path, QStringLiteral("安全校验超限，本项不支持撤销 (4096 nodes / 50 ms)"));
                }
                errno = 0;
            }
            const int enumerationError = errno;
            ::closedir(directory);
            if (enumerationError)
                return fail(path, QStringLiteral("Incomplete directory enumeration"));
            children.sort();
            for (const QString &child : children) {
                const QString name = QFileInfo(child).fileName();
                if (!scan(child, relative + QLatin1Char('/') + name, depth + 1))
                    return false;
            }
        } else {
            return fail(path, QStringLiteral("Unsupported special node in undo tree"));
        }
        if (::lstat(encoded.constData(), &after) != 0
            || undoMetadata(after, false) != undoMetadata(before, false))
            return fail(path, QStringLiteral("Object changed during undo snapshot"));
        QByteArray prefix;
        QDataStream stream(&prefix, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_12);
        stream << relative << content;
        exact.addData(prefix);
        exact.addData(undoMetadata(before, false));
        relocated.addData(prefix);
        // rename changes only the root node's ctime; child revisions remain exact.
        relocated.addData(undoMetadata(before, depth == 0));
        return true;
    }
};
#endif

} // namespace

QByteArray QtFileMgrLocalIO::captureUndoSnapshot(const QString &path, QString *error)
{
    if (error)
        error->clear();
#ifdef Q_OS_LINUX
    UndoScan scan;
    if (scan.scan(path, QString(), 0)) {
        const QByteArray exact = scan.exact.result();
        const QByteArray relocated = scan.relocated.result();
        // A child may change after its first visit without changing its parent's
        // mtime. A second complete manifest must match before issuing a proof.
        UndoScan verify;
        if (verify.scan(path, QString(), 0) && verify.exact.result() == exact)
            return kUndoMagic + exact + relocated;
        scan.fail(path, verify.error.isEmpty()
            ? QStringLiteral("Tree changed between complete undo manifests") : verify.error);
    }
    if (error)
        *error = scan.error;
#else
    qWarning("[LocalIO] undo snapshot: refused unsupported platform (path=\"%s\")", qPrintable(path));
    if (error)
        *error = QStringLiteral("Guarded local undo is currently supported only on Linux.");
#endif
    return {};
}

QByteArray QtFileMgrLocalIO::undoSnapshot(const QString &path, QString *error) const
{
    return captureUndoSnapshot(path, error);
}

bool QtFileMgrLocalIO::undoSnapshotsMatchAfterRename(const QByteArray &before,
                                                    const QByteArray &after)
{
    return before.size() == kUndoMagic.size() + 64
        && after.size() == kUndoMagic.size() + 64
        && before.startsWith(kUndoMagic) && after.startsWith(kUndoMagic)
        && before.right(32) == after.right(32);
}

static bool matchingUndoSnapshot(const QString &path, const QByteArray &expected,
                                  QString *error, bool relocated = false)
{
    if (expected.size() != kUndoMagic.size() + 64 || !expected.startsWith(kUndoMagic)) {
        qWarning("[LocalIO] undo: refused invalid proof (path=\"%s\", proofBytes=%d)",
                 qPrintable(path), static_cast<int>(expected.size()));
        if (error)
            *error = QStringLiteral("Undo identity/revision proof is unavailable.");
        return false;
    }
    const QByteArray actual = QtFileMgrLocalIO::captureUndoSnapshot(path, error);
    const bool matches = !actual.isEmpty()
        && (relocated ? actual.right(32) == expected.right(32) : actual == expected);
    if (!matches) {
        qWarning("[LocalIO] undo: refused replaced or modified object (path=\"%s\", proofAvailable=%d)",
                 qPrintable(path), !actual.isEmpty());
        if (error && error->isEmpty())
            *error = QStringLiteral("目标已被替换或修改，拒绝撤销: %1").arg(path);
    }
    return matches;
}

static void refreshProofAfterOwnRestore(const QString &path, const QByteArray &expected,
                                        QByteArray *refreshedSnapshot, bool retry = true)
{
    if (!refreshedSnapshot)
        return;
    QString error;
    const QByteArray current = QtFileMgrLocalIO::captureUndoSnapshot(path, &error);
    if (QtFileMgrLocalIO::undoSnapshotsMatchAfterRename(expected, current)) {
        *refreshedSnapshot = current;
        if (retry)
            qWarning("[LocalIO] undo retry: renewed only root ctime after verified own restore (path=\"%s\")",
                     qPrintable(path));
    } else {
        refreshedSnapshot->clear();
        qWarning("[LocalIO] undo retry: refused renewed proof because retained identity/content/tree changed (path=\"%s\", error=\"%s\")",
                 qPrintable(path), qPrintable(error));
    }
}

bool QtFileMgrLocalIO::undoCopyIfUnchanged(const QString &destination,
                                         const QByteArray &snapshot, QString *error, QByteArray *refreshedSnapshot)
{
    if (!matchingUndoSnapshot(destination, snapshot, error))
        return false;
    const QString quarantine = QFileInfo(destination).dir().absoluteFilePath(
        QStringLiteral(".qtfilemgr-undo-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!renameNoReplace(destination, quarantine, error))
        return false;
    if (!matchingUndoSnapshot(quarantine, snapshot, error, true)) {
        QString restoreError;
        if (!renameNoReplace(quarantine, destination, &restoreError)) {
            qWarning("[LocalIO] undo: retained quarantined object after failed restore (original=\"%s\", retained=\"%s\", error=\"%s\")",
                     qPrintable(destination), qPrintable(quarantine), qPrintable(restoreError));
            if (error)
                *error += QStringLiteral("; retained object: %1 (%2)").arg(quarantine, restoreError);
        }
        return false;
    }
    if (remove(quarantine))
        return true;
    qWarning("[LocalIO] undo: deletion failed; retained guarded object (original=\"%s\", retained=\"%s\")",
             qPrintable(destination), qPrintable(quarantine));
    QString restoreError;
    const bool restored = renameNoReplace(quarantine, destination, &restoreError);
    if (!restored)
        qWarning("[LocalIO] undo: could not restore remaining object after removal failure (original=\"%s\", retained=\"%s\", error=\"%s\")",
                 qPrintable(destination), qPrintable(quarantine), qPrintable(restoreError));
    else
        refreshProofAfterOwnRestore(destination, snapshot, refreshedSnapshot);
    if (error)
        *error = QStringLiteral("Could not remove guarded object; remaining data retained at %1")
                     .arg(restored ? destination : quarantine);
    return false;
}

bool QtFileMgrLocalIO::undoMoveIfUnchanged(const QString &destination, const QString &source,
                                         const QByteArray &snapshot, QString *error, QByteArray *refreshedSnapshot)
{
    // Avoid changing the destination's own ctime when a normal retry is blocked
    // by a source that appeared after the original move. The final syscall still
    // performs the authoritative atomic no-replace check against racing writers.
#ifdef Q_OS_LINUX
    struct stat sourceState{};
    if (::lstat(QFile::encodeName(source).constData(), &sourceState) == 0) {
        qWarning("[LocalIO] undo move: refused occupied original source (source=\"%s\", destination=\"%s\")",
                 qPrintable(source), qPrintable(destination));
        if (error)
            *error = QStringLiteral("Original source path is now occupied: %1").arg(source);
        return false;
    }
#endif
    if (!matchingUndoSnapshot(destination, snapshot, error))
        return false;
    const QString quarantine = QFileInfo(destination).dir().absoluteFilePath(
        QStringLiteral(".qtfilemgr-undo-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!renameNoReplace(destination, quarantine, error))
        return false;
    if (matchingUndoSnapshot(quarantine, snapshot, error, true)
        && renameNoReplace(quarantine, source, error)) {
        // A successful reverse rename also changes the root ctime. Return its
        // verified new exact proof so the view can reconnect older history only
        // through an exact pre-move proof captured by the execution worker.
        refreshProofAfterOwnRestore(source, snapshot, refreshedSnapshot, false);
        return true;
    }
    QString restoreError;
    if (!renameNoReplace(quarantine, destination, &restoreError)) {
        qWarning("[LocalIO] undo move: retained object after failed guarded restore (source=\"%s\", destination=\"%s\", retained=\"%s\", error=\"%s\")",
                 qPrintable(source), qPrintable(destination), qPrintable(quarantine), qPrintable(restoreError));
        if (error)
            *error += QStringLiteral("; retained object: %1 (%2)").arg(quarantine, restoreError);
    } else {
        refreshProofAfterOwnRestore(destination, snapshot, refreshedSnapshot);
    }
    return false;
}

QString QtFileMgrLocalIO::rootPath() const     { return QDir::homePath(); }
QString QtFileMgrLocalIO::displayName(const QString &path) const { return QFileInfo(path).fileName(); }

bool QtFileMgrLocalIO::exists(const QString &path) const { return QFileInfo::exists(path); }
bool QtFileMgrLocalIO::isDir(const QString &path) const   { return QFileInfo(path).isDir(); }
bool QtFileMgrLocalIO::isWritable(const QString &path) const { return QFileInfo(path).isWritable(); }

static bool localFileEntry(const QFileInfo &fi, FileEntry *entry)
{
    if (!entry || (!fi.isFile() && !fi.isDir() && !fi.isSymLink()))
        return false;
    entry->name = fi.fileName();
    entry->path = fi.absoluteFilePath();
    entry->size = fi.size();
    entry->modified = fi.lastModified();
    entry->isDir = fi.isDir();
    entry->isHidden = fi.isHidden();
    entry->isReadOnly = !fi.isWritable();
    entry->isSymlink = fi.isSymLink();
    return true;
}

QList<FileEntry> QtFileMgrLocalIO::listDir(const QString &path) const
{
    QList<FileEntry> result;
    QDir dir(path);
    if (!dir.exists()) return result;

    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    result.reserve(entries.size());
    for (const QFileInfo &fi : entries) {
        FileEntry entry;
        if (!localFileEntry(fi, &entry)) {
            qWarning("[LocalIO] listDir(%s): skipping unsupported special node '%s'",
                     qPrintable(path), qPrintable(fi.absoluteFilePath()));
            continue;
        }
        result.append(entry);
    }
    return result;
}

QtFileMgrDirectoryRequest *QtFileMgrLocalIO::listDirAsync(
    const QString &path, QObject *parent) const
{
    auto *request = new QtFileMgrDirectoryRequest(parent);
    const QSharedPointer<QAtomicInt> cancelled =
        request->cancellationState();
    const QPointer<QtFileMgrDirectoryRequest> guardedRequest(request);

    auto postBatch = [guardedRequest](const QList<FileEntry> &batch) {
        if (!guardedRequest)
            return;
        QTimer::singleShot(
            0, guardedRequest,
            [guardedRequest, batch]() {
                if (guardedRequest)
                    guardedRequest->deliverBatch(batch);
            });
    };
    auto postFinished = [guardedRequest](const QString &error) {
        if (!guardedRequest)
            return;
        QTimer::singleShot(
            0, guardedRequest,
            [guardedRequest, error]() {
                if (guardedRequest)
                    guardedRequest->finish(error);
            });
    };

    QThread *thread = QThread::create(
        [path, cancelled, postBatch, postFinished]() {
        const QFileInfo directoryInfo(path);
        if (!directoryInfo.exists() || !directoryInfo.isDir()) {
            postFinished(QStringLiteral("路径不存在或不是文件夹"));
            return;
        }
        if (!directoryInfo.isReadable()) {
            postFinished(QStringLiteral("无权限读取此文件夹"));
            return;
        }

        QList<FileEntry> batch;
        batch.reserve(256);
        QDirIterator iterator(
            path,
            QDir::AllEntries | QDir::Hidden | QDir::System
                | QDir::NoDotAndDotDot,
            QDirIterator::NoIteratorFlags);
        while (!cancelled->loadAcquire() && iterator.hasNext()) {
            iterator.next();
            FileEntry entry;
            if (!localFileEntry(iterator.fileInfo(), &entry))
                continue;
            batch.append(entry);
            if (batch.size() >= 256) {
                postBatch(batch);
                batch.clear();
            }
        }
        if (!cancelled->loadAcquire() && !batch.isEmpty())
            postBatch(batch);
        postFinished(QString());
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    // 直接启动:经 singleShot 推迟时,若应用退出前 timer 未触发,
    // 线程既没 start 也没 deleteLater,对象泄漏 (IO-P2-1)
    thread->start();
    return request;
}

bool QtFileMgrLocalIO::mkdir(const QString &path)   { return QDir().mkpath(path); }

bool QtFileMgrLocalIO::writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        qWarning("[LocalIO] writeFile: cannot create '%s': %s",
                 qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    if (file.write(contents) != contents.size()) {
        qWarning("[LocalIO] writeFile: incomplete write for '%s': %s",
                 qPrintable(path), qPrintable(file.errorString()));
        file.remove();
        return false;
    }
    file.close();
    if (file.error() != QFile::NoError) {
        qWarning("[LocalIO] writeFile: close failed for '%s': %s",
                 qPrintable(path), qPrintable(file.errorString()));
        if (!QFile::remove(path)) {
            qWarning("[LocalIO] writeFile: failed to remove partial file '%s' after close error",
                     qPrintable(path));
        }
        return false;
    }
    return true;
}

QByteArray QtFileMgrLocalIO::readPreview(const QString &path, qint64 maxBytes,
                                         bool *truncated, QString *error) const
{
    if (truncated)
        *truncated = false;
    if (error)
        error->clear();
    if (maxBytes <= 0) {
        if (error)
            *error = QStringLiteral("预览大小上限无效");
        qWarning("[LocalIO] readPreview: invalid limit %lld for '%s'",
                 static_cast<long long>(maxBytes), qPrintable(path));
        return {};
    }
    // 目录直接判错:QFile 可打开目录但读出空,界面会显示"空文件" (IO-P2-4)
    const QFileInfo info(path);
    if (info.isDir()) {
        if (error)
            *error = QStringLiteral("路径是文件夹，无法预览");
        qWarning("[LocalIO] readPreview: rejected directory '%s'", qPrintable(path));
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        qWarning("[LocalIO] readPreview: cannot open '%s': %s",
                 qPrintable(path), qPrintable(file.errorString()));
        return {};
    }
    QByteArray data = file.read(maxBytes + 1);
    if (data.size() > maxBytes) {
        // qint64 → int 截断在 >2G 上限时溢出,钳到 INT_MAX (IO-P2-4)
        const int clampMax = static_cast<int>(
            qMin<qint64>(maxBytes, std::numeric_limits<int>::max()));
        data.truncate(clampMax);
        if (truncated)
            *truncated = true;
    }
    return data;
}

static QString resolvedPathForComparison(const QString &path)
{
    const QString absolutePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (absolutePath == QStringLiteral("/"))
        return absolutePath;

    const QFileInfo pathInfo(absolutePath);
    QString probe = QDir::cleanPath(pathInfo.absolutePath());
    QStringList missingParts;
    missingParts.prepend(pathInfo.fileName());

    while (!QFileInfo(probe).exists() && !QFileInfo(probe).isSymLink()) {
        const QFileInfo info(probe);
        const QString parent = QDir::cleanPath(info.absolutePath());
        if (parent == probe)
            break;
        missingParts.prepend(info.fileName());
        probe = parent;
    }

    QString resolved = QFileInfo(probe).canonicalFilePath();
    if (resolved.isEmpty())
        resolved = probe;
    for (const QString &part : missingParts)
        resolved = QDir(resolved).absoluteFilePath(part);
    return QDir::cleanPath(resolved);
}

static bool isSameOrChildPath(const QString &path, const QString &parent)
{
    const QString cleanPath = resolvedPathForComparison(path);
    const QString cleanParent = resolvedPathForComparison(parent);
    if (cleanPath == cleanParent)
        return true;
    const QString prefix = cleanParent == QStringLiteral("/")
        ? cleanParent : cleanParent + QLatin1Char('/');
    return cleanPath.startsWith(prefix);
}

static bool transferCancelled(const QtFileMgrLocalIO::CancelCheck &isCancelled)
{
    return isCancelled && isCancelled();
}

static bool removeLocalNode(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile())
        return QFile::remove(path);
    if (info.isDir())
        return QDir(path).removeRecursively();
    return !info.exists();
}

static bool cleanPartialTransfer(const QString &path, const char *context)
{
    if (!QFileInfo(path).exists() && !QFileInfo(path).isSymLink())
        return true;
    if (removeLocalNode(path))
        return true;
    qWarning("[LocalIO] %s: failed to remove partial destination (path=\"%s\")",
             context, qPrintable(path));
    return false;
}

// 节点身份 (dev,ino): 区分"本次创建的目标"与"创建后被他人替换的
// 同名节点" (R2-03);按路径删除前必须核对
struct LocalNodeIdentity {
    bool valid = false;
    quint64 dev = 0;
    quint64 ino = 0;
};

static LocalNodeIdentity localNodeIdentity(const QString &path)
{
    LocalNodeIdentity id;
#ifdef Q_OS_UNIX
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0) {
        id.valid = true;
        id.dev = static_cast<quint64>(st.st_dev);
        id.ino = static_cast<quint64>(st.st_ino);
    }
#else
    Q_UNUSED(path);
#endif
    return id;
}

static LocalNodeIdentity localNodeIdentityOfFd(int fd)
{
    LocalNodeIdentity id;
#ifdef Q_OS_UNIX
    struct stat st;
    if (::fstat(fd, &st) == 0) {
        id.valid = true;
        id.dev = static_cast<quint64>(st.st_dev);
        id.ino = static_cast<quint64>(st.st_ino);
    }
#else
    Q_UNUSED(fd);
#endif
    return id;
}

// 只在节点仍是本次创建的那个时才清理;被他人替换则保留并告警 (R2-03)
static bool cleanPartialTransferIfOwned(const QString &path,
                                        const LocalNodeIdentity &created,
                                        const char *context)
{
    if (!created.valid) {
        qWarning("[LocalIO] %s: skipped cleanup, destination was not created by this operation (path=\"%s\")",
                 context, qPrintable(path));
        return false;
    }
    const LocalNodeIdentity current = localNodeIdentity(path);
    if (!current.valid)
        return true;  // 已不存在
    if (current.dev != created.dev || current.ino != created.ino) {
        qWarning("[LocalIO] %s: destination identity changed since creation; keeping external node (path=\"%s\")",
                 context, qPrintable(path));
        return false;
    }
    return cleanPartialTransfer(path, context);
}


static QtFileMgrLocalIO::TransferStatus copyRegularFileCancellable(
    const QString &src,
    const QString &dest,
    const QtFileMgrLocalIO::CancelCheck &isCancelled,
    const QtFileMgrLocalIO::ByteProgressCallback &progress,
    qint64 bytesTotal,
    qint64 *transferred,
    LocalNodeIdentity *createdNode,
    QString *error)
{
    QFile source(src);
    if (!source.open(QIODevice::ReadOnly)) {
        const QString detail = QStringLiteral("无法读取源文件: %1").arg(source.errorString());
        qWarning("[LocalIO] copyCancellable: source open failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                 qPrintable(src), qPrintable(dest), qPrintable(source.errorString()));
        if (error)
            *error = detail;
        return QtFileMgrLocalIO::TransferFailed;
    }

    QFile destination(dest);
    if (!destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const QString detail = QStringLiteral("无法创建目标文件: %1")
                                   .arg(destination.errorString());
        qWarning("[LocalIO] copyCancellable: destination open failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                 qPrintable(src), qPrintable(dest), qPrintable(destination.errorString()));
        if (error)
            *error = detail;
        return QtFileMgrLocalIO::TransferFailed;
    }
    // 数据落盘前先收紧到源权限:umask 默认(如 0644)复制 0600 源时,
    // 整个写入期间文件都偏宽;结尾的 setPermissions 仍保留兜底 (IO-P2-9)
    if (!destination.setPermissions(QFileInfo(src).permissions()))
        qWarning("[LocalIO] copyCancellable: could not pre-tighten file permissions (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
    // 目标由本次操作创建(NewOnly 排他),记录节点身份供清理核对 (审查#5/R2-03)
    const LocalNodeIdentity createdIdentity =
        localNodeIdentityOfFd(destination.handle());
    // 首个创建者胜出: 递归子节点不得覆盖顶层目标的身份 (R2-03)
    if (createdNode && !createdNode->valid)
        *createdNode = createdIdentity;

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (true) {
        if (transferCancelled(isCancelled)) {
            destination.close();
            cleanPartialTransferIfOwned(dest, createdIdentity, "copyCancellable(cancel)");
            if (error)
                *error = QStringLiteral("操作已取消");
            return QtFileMgrLocalIO::TransferCancelled;
        }

        const qint64 bytesRead = source.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            const QString detail = QStringLiteral("读取源文件失败: %1")
                                       .arg(source.errorString());
            qWarning("[LocalIO] copyCancellable: read failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                     qPrintable(src), qPrintable(dest), qPrintable(source.errorString()));
            destination.close();
            cleanPartialTransferIfOwned(dest, createdIdentity, "copyCancellable(read-error)");
            if (error)
                *error = detail;
            return QtFileMgrLocalIO::TransferFailed;
        }
        if (bytesRead == 0)
            break;

        qint64 written = 0;
        while (written < bytesRead) {
            if (transferCancelled(isCancelled)) {
                destination.close();
                cleanPartialTransferIfOwned(dest, createdIdentity, "copyCancellable(write-cancel)");
                if (error)
                    *error = QStringLiteral("操作已取消");
                return QtFileMgrLocalIO::TransferCancelled;
            }
            const qint64 bytesWritten = destination.write(
                buffer.constData() + written, bytesRead - written);
            if (bytesWritten <= 0) {
                const QString detail = QStringLiteral("写入目标文件失败: %1")
                                           .arg(destination.errorString());
                qWarning("[LocalIO] copyCancellable: write failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                         qPrintable(src), qPrintable(dest),
                         qPrintable(destination.errorString()));
                destination.close();
                cleanPartialTransferIfOwned(dest, createdIdentity, "copyCancellable(write-error)");
                if (error)
                    *error = detail;
                return QtFileMgrLocalIO::TransferFailed;
            }
            written += bytesWritten;
            *transferred += bytesWritten;
            if (progress)
                progress(*transferred, bytesTotal);
        }
    }

    destination.close();
    if (destination.error() != QFile::NoError) {
        const QString detail = QStringLiteral("关闭目标文件失败: %1")
                                   .arg(destination.errorString());
        qWarning("[LocalIO] copyCancellable: close failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                 qPrintable(src), qPrintable(dest), qPrintable(destination.errorString()));
        cleanPartialTransferIfOwned(dest, createdIdentity, "copyCancellable(close-error)");
        if (error)
            *error = detail;
        return QtFileMgrLocalIO::TransferFailed;
    }

    const QFileInfo sourceInfo(src);
    QFile destinationMetadata(dest);
    if (!destinationMetadata.open(QIODevice::ReadWrite)
        || !destinationMetadata.setFileTime(sourceInfo.lastModified(),
                                            QFileDevice::FileModificationTime)) {
        qWarning("[LocalIO] copyCancellable: could not preserve modification time (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
    }
    destinationMetadata.close();
    if (!QFile::setPermissions(dest, sourceInfo.permissions())) {
        qWarning("[LocalIO] copyCancellable: could not preserve permissions (source=\"%s\", destination=\"%s\", permissions=%x)",
                 qPrintable(src), qPrintable(dest),
                 static_cast<unsigned int>(sourceInfo.permissions()));
    }
    return QtFileMgrLocalIO::TransferSucceeded;
}

static QtFileMgrLocalIO::TransferStatus copyNodeCancellable(
    const QString &src,
    const QString &dest,
    const QtFileMgrLocalIO::CancelCheck &isCancelled,
    const QtFileMgrLocalIO::ByteProgressCallback &progress,
    QSet<QString> *visitedDirs,
    qint64 *transferred,
    qint64 bytesTotal,
    LocalNodeIdentity *createdNode,
    QString *error)
{
    if (transferCancelled(isCancelled)) {
        if (error)
            *error = QStringLiteral("操作已取消");
        return QtFileMgrLocalIO::TransferCancelled;
    }

    const QFileInfo sourceInfo(src);
    if (sourceInfo.isSymLink()) {
        const QString linkTarget = rawLinkTarget(src);
        if (!linkTarget.isEmpty() && createSymbolicLink(linkTarget, dest)) {
            if (createdNode && !createdNode->valid)
                *createdNode = localNodeIdentity(dest);
            return QtFileMgrLocalIO::TransferSucceeded;
        }
        qWarning("[LocalIO] copyCancellable: symbolic-link copy failed (source=\"%s\", target=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(linkTarget), qPrintable(dest));
        if (error)
            *error = QStringLiteral("无法复制符号链接: %1").arg(src);
        return QtFileMgrLocalIO::TransferFailed;
    }

    if (!sourceInfo.exists()) {
        qWarning("[LocalIO] copyCancellable: source does not exist (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("源路径不存在: %1").arg(src);
        return QtFileMgrLocalIO::TransferFailed;
    }
    if (QFileInfo(dest).exists() || QFileInfo(dest).isSymLink()) {
        qWarning("[LocalIO] copyCancellable: destination already exists (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("目标路径已存在: %1").arg(dest);
        return QtFileMgrLocalIO::TransferFailed;
    }

    if (!sourceInfo.isDir()) {
        if (sourceInfo.isFile()) {
            return copyRegularFileCancellable(
                src, dest, isCancelled, progress, bytesTotal,
                transferred, createdNode, error);
        }
        qWarning("[LocalIO] copyCancellable: unsupported source node (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("不支持的源文件类型: %1").arg(src);
        return QtFileMgrLocalIO::TransferFailed;
    }

    const QString canonicalSource = sourceInfo.canonicalFilePath();
    if (canonicalSource.isEmpty() || visitedDirs->contains(canonicalSource)) {
        qWarning("[LocalIO] copyCancellable: rejected unresolved or recursive directory (source=\"%s\", canonical=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(canonicalSource), qPrintable(dest));
        if (error)
            *error = QStringLiteral("目录路径无法解析或形成递归: %1").arg(src);
        return QtFileMgrLocalIO::TransferFailed;
    }
    visitedDirs->insert(canonicalSource);

    if (!QDir().mkdir(dest)) {
        visitedDirs->remove(canonicalSource);
        qWarning("[LocalIO] copyCancellable: directory creation failed (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("无法创建目标目录: %1").arg(dest);
        return QtFileMgrLocalIO::TransferFailed;
    }
    // 目录创建即收紧到源权限:umask 默认(如 0755)复制 0700 源时,
    // 整个递归复制期间目录都偏宽。仅当源目录自身可写时预收紧——
    // 源目录只读(如测试用 r-x 锁住删除)时目标需要保持可写才能
    // 装入子项,权限交给结尾的 setPermissions (IO-P2-9)
    const QFile::Permissions sourceDirPerms = sourceInfo.permissions();
    if (sourceDirPerms & QFile::WriteOwner) {
        if (!QFile::setPermissions(dest, sourceDirPerms))
            qWarning("[LocalIO] copyCancellable: could not pre-tighten directory permissions (source=\"%s\", destination=\"%s\")",
                     qPrintable(src), qPrintable(dest));
    }
    // 目录由本次操作创建;首个创建者胜出,递归子节点不得覆盖 (R2-03)
    if (createdNode && !createdNode->valid)
        *createdNode = localNodeIdentity(dest);

    const QFileInfoList entries = QDir(src).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (entries.isEmpty() && !sourceInfo.isReadable()) {
        // entryInfoList 对不可读目录也返回空列表,必须区分于真空目录,
        // 否则子文件被漏复制且报告成功 (审查#17)
        qWarning("[LocalIO] copyCancellable: directory is not readable (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("无法读取目录内容: %1").arg(src);
        return QtFileMgrLocalIO::TransferFailed;
    }
    for (const QFileInfo &entry : entries) {
        const QString srcPath = entry.absoluteFilePath();
        const QString destPath = QDir(dest).absoluteFilePath(entry.fileName());
        const QtFileMgrLocalIO::TransferStatus status = copyNodeCancellable(
            srcPath, destPath, isCancelled, progress, visitedDirs,
            transferred, bytesTotal, createdNode, error);
        if (status != QtFileMgrLocalIO::TransferSucceeded) {
            visitedDirs->remove(canonicalSource);
            return status;
        }
    }

    visitedDirs->remove(canonicalSource);
    if (!QFile::setPermissions(dest, sourceInfo.permissions())) {
        qWarning("[LocalIO] copyCancellable: could not preserve directory permissions (source=\"%s\", destination=\"%s\", permissions=%x)",
                 qPrintable(src), qPrintable(dest),
                 static_cast<unsigned int>(sourceInfo.permissions()));
    }
    return QtFileMgrLocalIO::TransferSucceeded;
}

bool QtFileMgrLocalIO::rename(const QString &oldPath, const QString &newPath)
{
    return moveCancellable(oldPath, newPath) == TransferSucceeded;
}

bool QtFileMgrLocalIO::remove(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isSymLink())
        return QFile::remove(path);
    if (fi.isDir())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

bool QtFileMgrLocalIO::moveToTrash(const QString &path, QString *error)
{
    if (error)
        error->clear();
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QString trashedPath;
    if (QFile::moveToTrash(path, &trashedPath))
        return true;
    const QString detail = QStringLiteral("无法将项目移动到系统回收站");
    qWarning("[LocalIO] moveToTrash: failed (path=\"%s\", trashPath=\"%s\")",
             qPrintable(path), qPrintable(trashedPath));
    if (error)
        *error = detail;
    return false;
#else
    const QString detail = QStringLiteral("当前 Qt 版本不支持系统回收站");
    qWarning("[LocalIO] moveToTrash: unsupported Qt version (path=\"%s\", qt=%s)",
             qPrintable(path), qVersion());
    if (error)
        *error = detail;
    return false;
#endif
}

bool QtFileMgrLocalIO::copy(const QString &src, const QString &dest)
{
    return copyCancellable(src, dest) == TransferSucceeded;
}

QtFileMgrLocalIO::TransferStatus QtFileMgrLocalIO::copyCancellable(
    const QString &src,
    const QString &dest,
    const CancelCheck &isCancelled,
    const ByteProgressCallback &progress,
    QString *error)
{
    if (error)
        error->clear();
    const QFileInfo sourceInfo(src);
    if ((!sourceInfo.exists() && !sourceInfo.isSymLink())
        || QFileInfo(dest).exists() || QFileInfo(dest).isSymLink()) {
        qWarning("[LocalIO] copyCancellable: rejected invalid source or occupied destination (source=\"%s\", destination=\"%s\", sourceExists=%d, destinationExists=%d)",
                 qPrintable(src), qPrintable(dest),
                 (sourceInfo.exists() || sourceInfo.isSymLink()) ? 1 : 0,
                 (QFileInfo(dest).exists() || QFileInfo(dest).isSymLink()) ? 1 : 0);
        if (error)
            *error = QStringLiteral("源路径无效或目标路径已存在");
        return TransferFailed;
    }
    if (sourceInfo.isDir() && !sourceInfo.isSymLink()
        && isSameOrChildPath(dest, src)) {
        qWarning("[LocalIO] copyCancellable: rejected directory self-copy (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("不能将目录复制到自身或子目录");
        return TransferFailed;
    }

    QSet<QString> visitedDirs;
    qint64 transferred = 0;
    const qint64 bytesTotal = sourceInfo.isFile() ? sourceInfo.size() : -1;
    LocalNodeIdentity createdNode;
    const TransferStatus status = copyNodeCancellable(
        src, dest, isCancelled, progress, &visitedDirs, &transferred,
        bytesTotal, &createdNode, error);
    if (status == TransferSucceeded)
        return status;

    // 只清理本次创建且未被他人替换的目标 (审查#5/R2-03)
    if (!createdNode.valid) {
        qWarning("[LocalIO] copyCancellable: skipped cleanup of a destination not created by this operation (source=\"%s\", destination=\"%s\", status=%d)",
                 qPrintable(src), qPrintable(dest), static_cast<int>(status));
        return status;
    }
    cleanPartialTransferIfOwned(dest, createdNode,
                                status == TransferCancelled
                                    ? "copyCancellable(cancel-cleanup)"
                                    : "copyCancellable(error-cleanup)");
    return status;
}

QtFileMgrLocalIO::TransferStatus QtFileMgrLocalIO::moveCancellable(
    const QString &src,
    const QString &dest,
    const CancelCheck &isCancelled,
    const ByteProgressCallback &progress,
    QString *error)
{
    if (error)
        error->clear();
    if (resolvedPathForComparison(src) == resolvedPathForComparison(dest))
        return TransferSucceeded;

    const QFileInfo sourceInfo(src);
    if ((!sourceInfo.exists() && !sourceInfo.isSymLink())
        || QFileInfo(dest).exists() || QFileInfo(dest).isSymLink()) {
        qWarning("[LocalIO] moveCancellable: rejected invalid source or occupied destination (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("源路径无效或目标路径已存在");
        return TransferFailed;
    }
    if (sourceInfo.isDir() && !sourceInfo.isSymLink()
        && isSameOrChildPath(dest, src)) {
        qWarning("[LocalIO] moveCancellable: rejected directory self-move (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("不能将目录移动到自身或子目录");
        return TransferFailed;
    }
    if (transferCancelled(isCancelled)) {
        if (error)
            *error = QStringLiteral("操作已取消");
        return TransferCancelled;
    }
    // 用原子 rename(2) 而非 QFile::rename: Qt 5.12 的 QFile::rename
    // 跨文件系统时内部做同步复制+删源,绕过取消与进度 (R2-11);
    // rename(2) 跨盘直接 EXDEV,交给我们自己的可取消执行器
#ifdef Q_OS_UNIX
    if (::rename(QFile::encodeName(src).constData(),
                 QFile::encodeName(dest).constData()) == 0)
        return TransferSucceeded;
#else
    if (QFile::rename(src, dest))
        return TransferSucceeded;
#endif

    qWarning("[LocalIO] moveCancellable: direct rename failed, using copy/remove fallback (source=\"%s\", destination=\"%s\")",
             qPrintable(src), qPrintable(dest));
    const TransferStatus copied = copyCancellable(
        src, dest, isCancelled, progress, error);
    if (copied != TransferSucceeded)
        return copied;
    if (transferCancelled(isCancelled)) {
        // 复制刚刚成功返回,节点就是本次创建的完整副本,直接清理
        cleanPartialTransfer(dest, "moveCancellable(cancel-after-copy)");
        if (error)
            *error = QStringLiteral("操作已取消");
        return TransferCancelled;
    }
    if (!removeLocalNode(src)) {
        qWarning("[LocalIO] moveCancellable: source removal failed after copy; preserving complete destination (source=\"%s\", destination=\"%s\")",
                 qPrintable(src), qPrintable(dest));
        if (error)
            *error = QStringLiteral("移动未完成：源路径可能已部分删除（%1）；完整副本保留在 %2，请核对后手动处理")
                         .arg(src, dest);
        return TransferFailed;
    }
    return TransferSucceeded;
}

QIcon QtFileMgrLocalIO::fileIcon(const QString &path) const
{
    return m_iconProvider.icon(QFileInfo(path));
}

QPixmap QtFileMgrLocalIO::thumbnail(const QString &path, const QSize &size)
{
    QImageReader reader(path);
    if (!reader.canRead())
        return {};
    QSize sourceSize = reader.size();
    if (sourceSize.isValid())
        reader.setScaledSize(sourceSize.scaled(size, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    return image.isNull() ? QPixmap() : QPixmap::fromImage(image);
}

// ─── 可选元数据实现 ────────────────────────────────────────

FileEntry QtFileMgrLocalIO::stat(const QString &path) const
{
    QFileInfo fi(path);
    FileEntry e;
    e.name = fi.fileName();
    e.path = fi.absoluteFilePath();
    e.size = fi.size();
    e.modified = fi.lastModified();
    e.isDir = fi.isDir();
    e.isHidden = fi.isHidden();
    e.isReadOnly = !fi.isWritable();
    e.isSymlink = fi.isSymLink();
    return e;
}

qint64 QtFileMgrLocalIO::fileSize(const QString &path) const
{
    return QFileInfo(path).size();
}

QDateTime QtFileMgrLocalIO::modificationTime(const QString &path) const
{
    return QFileInfo(path).lastModified();
}

QString QtFileMgrLocalIO::mimeType(const QString &path) const
{
    QMimeDatabase db;
    return db.mimeTypeForFile(path).name();
}

QFile::Permissions QtFileMgrLocalIO::permissions(const QString &path) const
{
    return QFileInfo(path).permissions();
}

QString QtFileMgrLocalIO::owner(const QString &path) const
{
    // getpwuid 返回静态缓冲,多线程并发会互踩;改可重入版本 (IO-P2-6)
    const uint uid = static_cast<uint>(QFileInfo(path).ownerId());
    struct passwd pw;
    struct passwd *result = nullptr;
    QByteArray buffer(1024, Qt::Uninitialized);
    int rc = getpwuid_r(uid, &pw, buffer.data(), buffer.size(), &result);
    while (rc == ERANGE) {
        buffer.resize(buffer.size() * 2);
        rc = getpwuid_r(uid, &pw, buffer.data(), buffer.size(), &result);
    }
    if (rc != 0 || !result) {
        qWarning("[LocalIO] owner: no passwd entry (path=\"%s\", uid=%u, errno=%d), falling back to numeric id",
                 qPrintable(path), uid, rc);
        return QString::number(uid);
    }
    return QString::fromLocal8Bit(pw.pw_name);
}

QString QtFileMgrLocalIO::group(const QString &path) const
{
    // getgrgid 同上,改可重入版本 (IO-P2-6)
    const uint gid = static_cast<uint>(QFileInfo(path).groupId());
    struct group gr;
    struct group *result = nullptr;
    QByteArray buffer(1024, Qt::Uninitialized);
    int rc = getgrgid_r(gid, &gr, buffer.data(), buffer.size(), &result);
    while (rc == ERANGE) {
        buffer.resize(buffer.size() * 2);
        rc = getgrgid_r(gid, &gr, buffer.data(), buffer.size(), &result);
    }
    if (rc != 0 || !result) {
        qWarning("[LocalIO] group: no group entry (path=\"%s\", gid=%u, errno=%d), falling back to numeric id",
                 qPrintable(path), gid, rc);
        return QString::number(gid);
    }
    return QString::fromLocal8Bit(gr.gr_name);
}

bool QtFileMgrLocalIO::chmod(const QString &path, QFile::Permissions perms)
{
    return QFile::setPermissions(path, perms);
}

bool QtFileMgrLocalIO::symlink(const QString &target, const QString &linkPath)
{
    return createSymbolicLink(target, linkPath);
}

QString QtFileMgrLocalIO::readLink(const QString &path) const
{
    return rawLinkTarget(path);
}

QtFileMgrIO::Capabilities QtFileMgrLocalIO::capabilities() const
{
    return QtFileMgrIO::capabilities()
        | NonBlockingListingCapability | TrashCapability;
}

// ─── 异步批量 ────────────────────────────────────────────────

namespace {

// 结果统一经 QTimer 投递回 ctx 所属线程;ctx 已销毁时丢弃并告警,
// 避免向悬空上下文投递回调。
void deliverLocalResult(const QPointer<QObject> &ctx,
                        const QString &tag,
                        const std::function<void()> &deliver)
{
    if (!ctx) {
        qWarning("[QtFileMgrLocalIO] %s: context destroyed before result delivery; dropping callback",
                 qPrintable(tag));
        return;
    }
    QTimer::singleShot(0, ctx, [ctx, tag, deliver]() {
        if (!ctx) {
            qWarning("[QtFileMgrLocalIO] %s: context destroyed before result delivery; dropping callback",
                     qPrintable(tag));
            return;
        }
        deliver();
    });
}

void startLocalWorkThread(const std::function<void()> &work)
{
    QThread *thread = QThread::create(work);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

} // namespace

void QtFileMgrLocalIO::copyAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb)
{
    const QPointer<QObject> guardedCtx(ctx);
    startLocalWorkThread([src, dest, guardedCtx, cb]() {
        QString error;
        const TransferStatus status = copyCancellable(src, dest, {}, {}, &error);
        const bool ok = status == TransferSucceeded;
        if (!ok) {
            qWarning("[QtFileMgrLocalIO] copyAsync: failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                     qPrintable(src), qPrintable(dest), qPrintable(error));
        }
        deliverLocalResult(guardedCtx, QStringLiteral("copyAsync"),
                           [cb, ok, dest]() { cb(ok, dest); });
    });
}
void QtFileMgrLocalIO::moveAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb)
{
    const QPointer<QObject> guardedCtx(ctx);
    startLocalWorkThread([src, dest, guardedCtx, cb]() {
        QString error;
        const TransferStatus status = moveCancellable(src, dest, {}, {}, &error);
        const bool ok = status == TransferSucceeded;
        if (!ok) {
            qWarning("[QtFileMgrLocalIO] moveAsync: failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                     qPrintable(src), qPrintable(dest), qPrintable(error));
        }
        deliverLocalResult(guardedCtx, QStringLiteral("moveAsync"),
                           [cb, ok, dest]() { cb(ok, dest); });
    });
}
void QtFileMgrLocalIO::batchCopyAsync(const QStringList &srcs, const QString &destDir,
                                       QObject *ctx, BatchCallback cb)
{
    if (destDir.isEmpty()) {
        qWarning("[QtFileMgrLocalIO] batchCopyAsync: rejected empty destination (sources=%d)",
                 srcs.size());
        QTimer::singleShot(0, ctx, [cb, srcs]() { cb(0, srcs.size(), {}); });
        return;
    }
    const QPointer<QObject> guardedCtx(ctx);
    const QPointer<QtFileMgrLocalIO> guardedSelf(this);
    startLocalWorkThread([guardedSelf, srcs, destDir, guardedCtx, cb]() {
        int ok = 0, fail = 0;
        QStringList newNames;
        for (const QString &src : srcs) {
            if (src.isEmpty()) {
                qWarning("[QtFileMgrLocalIO] batchCopyAsync: skipped empty source (destination=\"%s\")",
                         qPrintable(destDir));
                continue;
            }
            QString name = QFileInfo(src).fileName();
            if (name.isEmpty()) {
                qWarning("[QtFileMgrLocalIO] batchCopyAsync: skipped source without file name (source=\"%s\", destination=\"%s\")",
                         qPrintable(src), qPrintable(destDir));
                continue;
            }
            QString dest = destDir + QLatin1Char('/') + name;
            if (src == dest || QFileInfo::exists(dest))
                dest = uniqueDestName(destDir, name, guardedSelf);
            QString error;
            if (copyCancellable(src, dest, {}, {}, &error) == TransferSucceeded) {
                ok++;
                newNames << QFileInfo(dest).fileName();
            } else {
                qWarning("[QtFileMgrLocalIO] batchCopyAsync: item failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                         qPrintable(src), qPrintable(dest), qPrintable(error));
                fail++;
            }
        }
        deliverLocalResult(guardedCtx, QStringLiteral("batchCopyAsync"),
                           [cb, ok, fail, newNames]() { cb(ok, fail, newNames); });
    });
}
void QtFileMgrLocalIO::batchMoveAsync(const QStringList &srcs, const QString &destDir,
                                       QObject *ctx, BatchCallback cb)
{
    if (destDir.isEmpty()) {
        qWarning("[QtFileMgrLocalIO] batchMoveAsync: rejected empty destination (sources=%d)",
                 srcs.size());
        QTimer::singleShot(0, ctx, [cb, srcs]() { cb(0, srcs.size(), {}); });
        return;
    }
    const QPointer<QObject> guardedCtx(ctx);
    const QPointer<QtFileMgrLocalIO> guardedSelf(this);
    startLocalWorkThread([guardedSelf, srcs, destDir, guardedCtx, cb]() {
        int ok = 0, fail = 0;
        QStringList newNames;
        for (const QString &src : srcs) {
            if (src.isEmpty()) {
                qWarning("[QtFileMgrLocalIO] batchMoveAsync: skipped empty source (destination=\"%s\")",
                         qPrintable(destDir));
                continue;
            }
            QString name = QFileInfo(src).fileName();
            if (name.isEmpty()) {
                qWarning("[QtFileMgrLocalIO] batchMoveAsync: skipped source without file name (source=\"%s\", destination=\"%s\")",
                         qPrintable(src), qPrintable(destDir));
                continue;
            }
            if (resolvedPathForComparison(QFileInfo(src).absolutePath())
                == resolvedPathForComparison(destDir)) {
                qWarning("[QtFileMgrLocalIO] batchMoveAsync: skipped source already in destination (source=\"%s\", destination=\"%s\")",
                         qPrintable(src), qPrintable(destDir));
                continue;
            }
            QString dest = uniqueDestName(destDir, name, guardedSelf);
            QString error;
            if (moveCancellable(src, dest, {}, {}, &error) == TransferSucceeded) {
                ok++;
                newNames << QFileInfo(dest).fileName();
            } else {
                qWarning("[QtFileMgrLocalIO] batchMoveAsync: item failed (source=\"%s\", destination=\"%s\", error=\"%s\")",
                         qPrintable(src), qPrintable(dest), qPrintable(error));
                fail++;
            }
        }
        deliverLocalResult(guardedCtx, QStringLiteral("batchMoveAsync"),
                           [cb, ok, fail, newNames]() { cb(ok, fail, newNames); });
    });
}
