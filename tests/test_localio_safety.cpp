// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrLocalIO.h"
#include "QtFileMgrFileOperationJob.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrFilterBar.h"
#include "QtFileMgrSearchDialog.h"
#include "QtFileMgrWorkspace.h"
#include "QtFileMgrDeviceProvider.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrItemDelegate.h"
#include "QtFileMgrBreadcrumbBar.h"
#include "QtFileMgrPlacesPanel.h"
#include "QtFileMgrPreviewPanel.h"
#include "QtFileMgrPropertiesDialog.h"
#include "QtFileMgrTrashDialog.h"
#include "QtFileMgrVersionsDialog.h"
#include "QtFileMgrProtocolProvider.h"
#include "QtFileMgrPathPicker.h"
#include "QtFileMgrSettingsDialog.h"

#ifndef QTFILEMGR_WITH_SSH
#define QTFILEMGR_WITH_SSH 1
#endif
#if QTFILEMGR_WITH_SSH
#include "QtFileMgrSSHIO.h"
#include "QtFileMgrSSHDialog.h"
#endif

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTableView>
#include <QTextStream>
#include <QTimer>
#include <QLineEdit>
#include <QKeyEvent>
#include <QClipboard>
#include <QMimeData>
#include <QImage>
#include <QTreeWidget>
#include <QCompleter>
#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QSettings>
#include <QFrame>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QStandardItemModel>
#include <QToolButton>
#include <QMenu>
#include <QMessageBox>
#include <QThread>
#include <QTabWidget>
#include <QPointer>
#include <QPlainTextEdit>

#include <functional>

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    QTextStream out(stdout);
    out << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    out.flush();
    if (!condition)
        ++failures;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return condition();
}

struct JobOutcome {
    bool callbackCalled = false;
    int succeeded = -1;
    int failed = -1;
    int skipped = -1;
    bool cancelled = false;
    int finalProgress = 0;
    QStringList newNames;
    QStringList errors;
};

JobOutcome runJob(QtFileMgrFileOperationJob &job, bool cancelAfterStart = false)
{
    JobOutcome outcome;
    QEventLoop loop;
    QObject::connect(&job, &QtFileMgrFileOperationJob::progress, &loop,
                     [&](int completed, int, const QString &) {
        outcome.finalProgress = completed;
    });
    QObject::connect(
        &job, &QtFileMgrFileOperationJob::finished, &loop,
        [&](int succeeded,
            int failed,
            int skipped,
            const QStringList &newNames,
            bool cancelled,
            const QStringList &errors) {
            outcome.callbackCalled = true;
            outcome.succeeded = succeeded;
            outcome.failed = failed;
            outcome.skipped = skipped;
            outcome.cancelled = cancelled;
            outcome.newNames = newNames;
            outcome.errors = errors;
            loop.quit();
        });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    job.start();
    if (cancelAfterStart)
        job.cancel();
    loop.exec();
    return outcome;
}

class DelayedRemoteIO : public QtFileMgrIO {
public:
    explicit DelayedRemoteIO(QObject *parent = nullptr)
        : QtFileMgrIO(parent)
    {
    }

    QString rootPath() const override { return QString(); }
    QString displayName(const QString &path) const override { return QFileInfo(path).fileName(); }
    bool exists(const QString &) const override { return false; }
    QList<FileEntry> listDir(const QString &) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    bool isWritable(const QString &) const override { return true; }
    bool mkdir(const QString &) override { return true; }
    bool rename(const QString &, const QString &) override { return false; }
    bool remove(const QString &) override { return false; }
    bool copy(const QString &, const QString &) override { return false; }
    bool canWrite() const override { return true; }
    bool isRemote() const override { return true; }

    void copyAsync(const QString &, const QString &destination,
                   QObject *context, OpCallback callback) override
    {
        QTimer::singleShot(50, context, [callback, destination]() {
            callback(true, destination);
        });
    }
};

class ManualProtocolProvider : public QtFileMgrProtocolProvider {
public:
    using QtFileMgrProtocolProvider::QtFileMgrProtocolProvider;
    QList<Completion> callbacks;
    void connectUrl(const QUrl &, QWidget *, Completion completion) override
    { callbacks.append(completion); }
};

// A third protocol catches accidental assumptions that every virtual backend
// uses remote:// relative paths or SSH absolute paths.
class CatalogNavigationIO final : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;
    QString rootPath() const override { return QStringLiteral("/"); }
    bool acceptsUrl(const QUrl &url) const override
    {
        return url.scheme() == QStringLiteral("catalog") && url.host().isEmpty()
            && url.userName().isEmpty() && url.port(-1) == -1;
    }
    QUrl urlForPath(const QString &path) const override
    {
        QUrl url(QStringLiteral("catalog:///"));
        url.setPath(QDir::cleanPath(path.isEmpty() ? QStringLiteral("/") : path));
        return url;
    }
    QString pathFromUrl(const QUrl &url) const override
    {
        return acceptsUrl(url) ? QDir::cleanPath(url.path()) : QString();
    }
    bool canWrite() const override { return false; }
    bool isWritable(const QString &) const override { return false; }
    bool exists(const QString &path) const override { return isDir(path); }
    bool isDir(const QString &path) const override
    {
        return directories().contains(path);
    }
    QList<FileEntry> listDir(const QString &path) const override
    {
        QList<FileEntry> entries;
        for (const QString &candidate : directories()) {
            if (candidate == QStringLiteral("/") || candidate == path)
                continue;
            const QString parent = candidate.left(candidate.lastIndexOf(QLatin1Char('/')));
            if ((parent.isEmpty() ? QStringLiteral("/") : parent) != path)
                continue;
            FileEntry entry;
            entry.path = candidate;
            entry.name = candidate.section(QLatin1Char('/'), -1);
            entry.isDir = true;
            entry.isReadOnly = true;
            entries.append(entry);
        }
        return entries;
    }
    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const override
    {
        auto *request = new QtFileMgrDirectoryRequest(parent);
        const QList<FileEntry> entries = listDir(path);
        const bool valid = isDir(path);
        QTimer::singleShot(0, request, [request, entries, valid]() {
            if (!request->isCancelled())
                request->deliverBatch(entries);
            request->finish(valid ? QString() : QStringLiteral("Unknown catalog directory"));
        });
        return request;
    }
private:
    static QStringList directories()
    {
        return {QStringLiteral("/"), QStringLiteral("/documents"),
                QStringLiteral("/documents/guide"), QStringLiteral("/documents/guide/nested"),
                QStringLiteral("/documents2"), QStringLiteral("/other")};
    }
};

class ManualPreviewIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;
    mutable QList<QPointer<QtFileMgrPreviewRequest>> requests;
    QString mimeType(const QString &) const override { return QStringLiteral("text/plain"); }
    FileEntry stat(const QString &path) const override {
        FileEntry entry;
        entry.path = path;
        entry.name = path;
        entry.size = 20;
        return entry;
    }
    QtFileMgrPreviewRequest *previewRequest(const QString &, qint64, bool,
                                            QObject *parent) const override {
        auto *request = new QtFileMgrPreviewRequest(parent);
        requests.append(request);
        return request;
    }
};

class DuplicateCallbackRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    int copyRequests = 0;

    void copyAsync(const QString &, const QString &destination,
                   QObject *context, OpCallback callback) override
    {
        ++copyRequests;
        QTimer::singleShot(0, context, [callback, destination]() {
            callback(true, destination);
            callback(true, destination);
        });
    }
};

class DelayedListingIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    mutable int listRequests = 0;

    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const override
    {
        ++listRequests;
        auto *request = new QtFileMgrDirectoryRequest(parent);
        const int delayMs = path == QStringLiteral("slow") ? 120 : 5;
        QTimer::singleShot(delayMs, request, [request, path]() {
            FileEntry entry;
            entry.name = path + QStringLiteral(".txt");
            entry.path = path + QLatin1Char('/') + entry.name;
            request->deliverBatch({entry});
            request->finish();
        });
        return request;
    }
};

class ProgressVersionRemoteIO : public DelayedListingIO {
public:
    using DelayedListingIO::DelayedListingIO;

    int restoredVersion = -1;

    Capabilities capabilities() const override
    {
        return DelayedRemoteIO::capabilities()
            | NonBlockingListingCapability
            | ChangeNotificationCapability
            | TransferProgressCapability
            | VersionsCapability
            | VersionRestoreCapability;
    }

    QtFileMgrTransferRequest *copyRequest(
        const QString &, const QString &destination,
        QObject *parent = nullptr) override
    {
        auto *request = new QtFileMgrTransferRequest(parent);
        QTimer::singleShot(0, request, [request]() {
            request->reportProgress(25, 100);
        });
        QTimer::singleShot(5, request, [request, destination]() {
            request->reportProgress(100, 100);
            request->finish(true, destination);
        });
        return request;
    }

    void versionsAsync(const QString &, QObject *context,
                       VersionsCallback callback) override
    {
        QTimer::singleShot(0, context, [callback]() {
            VersionInfo first;
            first.version = 7;
            first.timestamp = QDateTime::currentDateTime();
            first.size = 1024;
            first.author = QStringLiteral("Spike");
            first.comment = QStringLiteral("stable");
            VersionInfo second = first;
            second.version = 6;
            second.comment = QStringLiteral("previous");
            callback({first, second}, QString());
        });
    }

    void restoreVersionAsync(const QString &, int version,
                             QObject *context,
                             ResultCallback callback) override
    {
        QTimer::singleShot(0, context, [this, version, callback]() {
            restoredVersion = version;
            callback(true, QString());
        });
    }
};

class SynchronousRequestRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    QtFileMgrTransferRequest *copyRequest(
        const QString &, const QString &destination,
        QObject *parent = nullptr) override
    {
        auto *request = new QtFileMgrTransferRequest(parent);
        request->finish(true, destination);
        return request;
    }
};

class SynchronousListingRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    QtFileMgrDirectoryRequest *listDirAsync(
        const QString &, QObject *parent = nullptr) const override
    {
        auto *request = new QtFileMgrDirectoryRequest(parent);
        FileEntry entry;
        entry.name = QStringLiteral("synchronous.txt");
        entry.path = QStringLiteral("/synchronous.txt");
        request->deliverBatch({entry});
        request->finish();
        return request;
    }
};

class DroppedRequestRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    QtFileMgrTransferRequest *copyRequest(
        const QString &, const QString &,
        QObject *parent = nullptr) override
    {
        auto *request = new QtFileMgrTransferRequest(parent);
        request->deleteLater();
        return request;
    }
};

class LegacyTrashRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;

    bool remove(const QString &path) override
    {
        removedPath = path;
        return true;
    }

    QString removedPath;
};

class TrackingTrashIO : public QtFileMgrIO {
public:
    explicit TrackingTrashIO(const QString &directory,
                             QObject *parent = nullptr)
        : QtFileMgrIO(parent)
        , m_directory(directory)
    {
    }

    QString rootPath() const override { return m_directory; }
    QString displayName(const QString &path) const override
    {
        return QFileInfo(path).fileName();
    }
    bool exists(const QString &path) const override
    {
        return !trashed && path == filePath();
    }
    QList<FileEntry> listDir(const QString &path) const override
    {
        if (path != m_directory || trashed)
            return {};
        FileEntry entry;
        entry.name = QStringLiteral("virtual.txt");
        entry.path = filePath();
        entry.size = 7;
        return {entry};
    }
    bool isDir(const QString &path) const override { return path == m_directory; }
    bool isWritable(const QString &) const override { return true; }
    bool mkdir(const QString &) override { return false; }
    bool rename(const QString &, const QString &) override { return false; }
    bool remove(const QString &) override
    {
        ++removeCalls;
        return false;
    }
    bool copy(const QString &, const QString &) override { return false; }
    bool canWrite() const override { return true; }
    bool moveToTrash(const QString &path, QString *error) override
    {
        if (error)
            error->clear();
        ++trashCalls;
        trashedPath = path;
        trashed = path == filePath();
        return trashed;
    }
    Capabilities capabilities() const override
    {
        return QtFileMgrIO::capabilities() | TrashCapability;
    }

    QString filePath() const
    {
        return QDir(m_directory).filePath(QStringLiteral("virtual.txt"));
    }

    QString m_directory;
    QString trashedPath;
    bool trashed = false;
    int trashCalls = 0;
    int removeCalls = 0;
};

class EmptyDeviceProvider : public QtFileMgrDeviceProvider {
public:
    using QtFileMgrDeviceProvider::QtFileMgrDeviceProvider;
    QList<QtFileMgrDeviceEntry> devices() const override { return {}; }
};

class AsyncDeviceProvider : public QtFileMgrDeviceProvider {
public:
    explicit AsyncDeviceProvider(const QString &mountedPath,
                                 QObject *parent = nullptr)
        : QtFileMgrDeviceProvider(parent)
        , path(mountedPath)
    {
    }

    QList<QtFileMgrDeviceEntry> devices() const override
    {
        QtFileMgrDeviceEntry entry;
        entry.id = QStringLiteral("test-device");
        entry.name = QStringLiteral("测试设备");
        entry.path = mounted ? path : QString();
        entry.toolTip = mounted ? path : QStringLiteral("未挂载");
        entry.iconName = QStringLiteral("drive-removable-media");
        entry.mounted = mounted;
        entry.canMount = !mounted;
        entry.canUnmount = mounted;
        return {entry};
    }

    void mountAsync(const QString &id, QObject *context,
                    OperationCallback callback) override
    {
        ++mountRequests;
        QTimer::singleShot(10, context, [this, id, callback]() {
            mounted = id == QStringLiteral("test-device");
            if (mounted)
                emit devicesChanged();
            callback(mounted, mounted ? QString()
                                      : QStringLiteral("未知测试设备"));
        });
    }

    void unmountAsync(const QString &id, QObject *context,
                      OperationCallback callback) override
    {
        ++unmountRequests;
        QTimer::singleShot(10, context, [this, id, callback]() {
            const bool success = id == QStringLiteral("test-device") && mounted;
            if (success) {
                mounted = false;
                emit devicesChanged();
            }
            callback(success, success ? QString()
                                      : QStringLiteral("设备未挂载"));
        });
    }

    QString path;
    bool mounted = false;
    int mountRequests = 0;
    int unmountRequests = 0;
};

class PendingDeviceProvider : public QtFileMgrDeviceProvider {
public:
    using QtFileMgrDeviceProvider::QtFileMgrDeviceProvider;

    QList<QtFileMgrDeviceEntry> devices() const override
    {
        QtFileMgrDeviceEntry entry;
        entry.id = QStringLiteral("pending-device");
        entry.name = QStringLiteral("待处理设备");
        entry.canMount = true;
        return {entry};
    }

    void mountAsync(const QString &, QObject *, OperationCallback) override
    {
    }
};

// DLG-P1-7 复现: 挂载回调成功时 devices() 尚未反映挂载,稍后补发 devicesChanged
class DeferredListMountProvider : public QtFileMgrDeviceProvider {
public:
    using QtFileMgrDeviceProvider::QtFileMgrDeviceProvider;

    QList<QtFileMgrDeviceEntry> devices() const override
    {
        QtFileMgrDeviceEntry entry;
        entry.id = QStringLiteral("deferred-device");
        entry.name = QStringLiteral("延迟设备");
        entry.path = mounted ? mountedPath : QString();
        entry.mounted = mounted;
        entry.canMount = !mounted;
        entry.canUnmount = mounted;
        return {entry};
    }

    void mountAsync(const QString &, QObject *, OperationCallback callback) override
    {
        ++mountRequests;
        // 先回报成功,此时 devices() 仍未反映挂载 —— 复现列表未刷新竞态
        callback(true, QString());
    }

    void unmountAsync(const QString &, QObject *, OperationCallback callback) override
    {
        callback(true, QString());
    }

    void completeMount(const QString &path)
    {
        mountedPath = path;
        mounted = true;
        emit devicesChanged();
    }

    bool mounted = false;
    QString mountedPath;
    int mountRequests = 0;
};

// DLG-P1-3 复现: previewRequest 在返回前同步 finish
class SynchronousPreviewIO : public ManualPreviewIO {
public:
    using ManualPreviewIO::ManualPreviewIO;

    QtFileMgrPreviewRequest *previewRequest(
        const QString &, qint64, bool, QObject *parent = nullptr) const override
    {
        auto *request = new QtFileMgrPreviewRequest(parent);
        request->finish(QByteArrayLiteral("sync-preview-text"), false, QString());
        return request;
    }
};

// DLG-P1-5 复现: rename 恒失败
class FailingRenameIO : public QtFileMgrLocalIO {
public:
    using QtFileMgrLocalIO::QtFileMgrLocalIO;
    bool rename(const QString &, const QString &) override { return false; }
};

// 同主机复用/归并复现: ssh scheme 的远端后端

// IO-P1-6 复现: 目标已存在且可删的远端后端
class OverwritableRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;
    QString occupiedPath;
    QStringList removedPaths;
    QSet<QString> storedPaths;
    bool exists(const QString &path) const override
    {
        return path == occupiedPath || storedPaths.contains(path);
    }
    bool removePermanent(const QString &path) override
    {
        removedPaths << path;
        if (path == occupiedPath)
            occupiedPath.clear();
        storedPaths.remove(path);
        return true;
    }
    QStringList renamedPairs;
    bool rename(const QString &from, const QString &to) override
    {
        return renameNoReplace(from, to);
    }
    bool renameNoReplace(const QString &from, const QString &to,
                         QString *error = nullptr) override
    {
        if (!exists(from) || exists(to)) {
            if (error) *error = QStringLiteral("Test backend refuses missing source or occupied target");
            qWarning("[TEST] renameNoReplace refused (source=\"%s\", destination=\"%s\")",
                     qPrintable(from), qPrintable(to));
            return false;
        }
        renamedPairs << (from + QStringLiteral(">") + to);
        if (from == occupiedPath) occupiedPath.clear();
        storedPaths.remove(from);
        storedPaths.insert(to);
        return true;
    }
    void copyAsync(const QString &, const QString &destination,
                   QObject *context, OpCallback callback) override
    {
        QTimer::singleShot(50, context, [this, callback, destination]() {
            storedPaths.insert(destination);
            callback(true, destination);
        });
    }
};

// R2-09 复现: 传输恒失败的远端后端
class FailingTransferRemoteIO : public OverwritableRemoteIO {
public:
    using OverwritableRemoteIO::OverwritableRemoteIO;
    QtFileMgrTransferRequest *copyRequest(
        const QString &, const QString &, QObject *parent = nullptr) override
    {
        auto *request = new QtFileMgrTransferRequest(parent);
        QTimer::singleShot(0, request, [request]() {
            request->finish(false, QString(),
                            QStringLiteral("模拟传输失败"));
        });
        return request;
    }
};

// 跨后端粘贴复现: 记录 copyRequest 调用的远端后端
class RecordingTransferRemoteIO : public DelayedRemoteIO {
public:
    using DelayedRemoteIO::DelayedRemoteIO;
    mutable QStringList copySources;
    mutable QStringList copyDestinations;
    mutable int existsCalls = 0;
    QString writableDir = QStringLiteral("uploads");
    QString acceptedPrefix;  // 非空时 acceptsUrl 额外要求路径前缀
    bool exists(const QString &path) const override
    {
        ++existsCalls;
        return DelayedRemoteIO::exists(path);
    }
    QList<FileEntry> listDir(const QString &) const override
    {
        // 非空目录: 避开空目录内容状态的 exists 探测,使 existsCalls 可精确断言
        FileEntry seed;
        seed.name = QStringLiteral("seed.txt");
        seed.path = writableDir + QLatin1Char('/') + seed.name;
        return {seed};
    }
    bool acceptsUrl(const QUrl &url) const override
    {
        return DelayedRemoteIO::acceptsUrl(url)
            && (acceptedPrefix.isEmpty()
                || url.path().startsWith(acceptedPrefix));
    }
    bool isDir(const QString &path) const override
    {
        return path == writableDir || DelayedRemoteIO::isDir(path);
    }
    QtFileMgrTransferRequest *copyRequest(
        const QString &source, const QString &destination,
        QObject *parent = nullptr) override
    {
        copySources << source;
        copyDestinations << destination;
        return DelayedRemoteIO::copyRequest(source, destination, parent);
    }
    mutable QStringList importSourcePaths;
    mutable QStringList importDestinationPaths;
    QtFileMgrTransferRequest *importFromLocalRequest(
        const QString &localSource, const QString &remoteDestination,
        bool move, QObject *parent = nullptr) override
    {
        Q_UNUSED(move);
        importSourcePaths << localSource;
        importDestinationPaths << remoteDestination;
        // 兼容按 copySources 断言的旧用例;直接成功,不走 legacy 适配器
        // (adapter 落到 DelayedRemoteIO::rename 恒 false,移动会失败并
        // 弹模态错误框卡死套件)
        copySources << localSource;
        copyDestinations << remoteDestination;
        auto *request = new QtFileMgrTransferRequest(parent);
        QTimer::singleShot(0, request, [request, remoteDestination]() {
            request->finish(true, remoteDestination);
        });
        return request;
    }
};

// 审查#1 复现: ssh scheme + 绝对路径语义的记录型远端后端
class SshRemoteIO : public RecordingTransferRemoteIO {
public:
    using RecordingTransferRemoteIO::RecordingTransferRemoteIO;
    bool acceptsUrl(const QUrl &url) const override
    {
        return url.scheme().toLower() == QStringLiteral("ssh");
    }
    QString pathFromUrl(const QUrl &url) const override
    {
        return url.path().isEmpty() ? QStringLiteral("/") : url.path();
    }
};

// R2-16 复现: 延迟完成的异步重命名远端后端
class SlowRenameRemoteIO : public RecordingTransferRemoteIO {
public:
    using RecordingTransferRemoteIO::RecordingTransferRemoteIO;
    QString currentName = QStringLiteral("old.txt");
    bool renameAsyncCalled = false;
    QList<FileEntry> listDir(const QString &) const override
    {
        FileEntry entry;
        entry.name = currentName;
        entry.path = writableDir + QLatin1Char('/') + currentName;
        return {entry};
    }
    void renameAsync(const QString &, const QString &newPath,
                     QObject *ctx, OpCallback cb) override
    {
        renameAsyncCalled = true;
        const QString newName = QFileInfo(newPath).fileName();
        QTimer::singleShot(200, ctx, [this, cb, newPath, newName]() {
            currentName = newName;
            cb(true, newPath);
        });
    }
};


// 跨后端复现: 支持导出的远端后端(下载/中转的源)
class ExportingRemoteIO : public RecordingTransferRemoteIO {
public:
    using RecordingTransferRemoteIO::RecordingTransferRemoteIO;
    mutable QStringList exportSources;
    QtFileMgrIO::Capabilities capabilities() const override
    {
        return DelayedRemoteIO::capabilities()
            | QtFileMgrIO::LocalExportCapability;
    }
    QtFileMgrTransferRequest *exportToLocalRequest(
        const QString &source, const QString &localDestination,
        QObject *parent = nullptr) override
    {
        exportSources << source;
        auto *request = new QtFileMgrTransferRequest(parent);
        QTimer::singleShot(0, request, [request, localDestination]() {
            QFile file(localDestination);
            const bool ok = file.open(QIODevice::WriteOnly)
                && file.write(QByteArrayLiteral("exported")) > 0;
            file.close();
            request->finish(ok, localDestination,
                            ok ? QString() : QStringLiteral("写入本地失败"));
        });
        return request;
    }
};

// DLG-P1-6: qWarning 捕获
QStringList g_jsonWarnings;
void jsonCaptureHandler(QtMsgType type, const QMessageLogContext &,
                        const QString &message)
{
    if (type == QtWarningMsg)
        g_jsonWarnings.append(message);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temporaryDir;
    check(temporaryDir.isValid(), QStringLiteral("temporary directory is available"));
    if (!temporaryDir.isValid())
        return 1;

    QtFileMgrLocalIO io;
    const QDir root(temporaryDir.path());

    check(io.supportsCapability(QtFileMgrIO::ReadCapability)
              && io.supportsCapability(QtFileMgrIO::WriteCapability)
              && io.supportsCapability(QtFileMgrIO::NonBlockingListingCapability)
              && io.supportsCapability(QtFileMgrIO::TrashCapability)
              && !io.supportsCapability(QtFileMgrIO::RemoteCapability),
          QStringLiteral("local backend declares its compatible capability set"));
    DelayedRemoteIO legacyCapabilityIO;
    check(legacyCapabilityIO.supportsCapability(QtFileMgrIO::RemoteCapability)
              && !legacyCapabilityIO.supportsCapability(QtFileMgrIO::TrashCapability)
              && legacyCapabilityIO.supportsCapability(QtFileMgrIO::WriteCapability)
              && !legacyCapabilityIO.supportsCapability(
                     QtFileMgrIO::TransferProgressCapability),
          QStringLiteral("legacy backend capabilities derive from existing virtual methods"));
    LegacyTrashRemoteIO legacyTrashIO;
    QString legacyTrashError;
    check(!legacyTrashIO.moveToTrash(QStringLiteral("remote/path.txt"),
                                     &legacyTrashError)
              && legacyTrashIO.removedPath.isEmpty()
              && !legacyTrashError.isEmpty(),
          QStringLiteral("remote backend without trash override never permanently deletes"));
    check(!legacyCapabilityIO.moveToTrash(QStringLiteral("remote/path.txt"),
                                          &legacyTrashError)
              && !legacyTrashError.isEmpty(),
          QStringLiteral("remote backend without trash support rejects trash request"));

    QtFileMgrPathPicker nullIOPicker;
    check(nullIOPicker.selectPath(nullptr, root.path()).isEmpty(),
          QStringLiteral("path picker rejects null IO without crashing"));

    QObject disconnectedListingContext;
    auto *disconnectedListingIO = new DelayedRemoteIO;
    QtFileMgrDirectoryRequest *disconnectedListingRequest =
        disconnectedListingIO->listDirAsync(
            QStringLiteral("disconnected-directory"),
            &disconnectedListingContext);
    bool disconnectedListingFinished = false;
    QString disconnectedListingError;
    QObject::connect(
        disconnectedListingRequest, &QtFileMgrDirectoryRequest::finished,
        &disconnectedListingContext, [&](const QString &error) {
            disconnectedListingFinished = true;
            disconnectedListingError = error;
        });
    delete disconnectedListingIO;
    check(waitUntil([&]() { return disconnectedListingFinished; })
              && !disconnectedListingError.isEmpty(),
          QStringLiteral("default directory adapter reports backend destruction as an error"));

    QtFileMgrConfig roundTripConfig = QtFileMgrConfig::defaultConfig();
    roundTripConfig.statusBar.visible = false;
    roundTripConfig.statusBar.showZoomSlider = false;
    roundTripConfig.statusBar.showDiskSpace = false;
    roundTripConfig.statusBar.showItemCount = false;
    roundTripConfig.view.detailColumnWidths = {320, 90, 120, 180};
    const QByteArray roundTripJson = roundTripConfig.toJson();
    const QtFileMgrConfig parsedConfig = QtFileMgrConfig::loadFromJson(roundTripJson);
    check(!parsedConfig.statusBar.visible
              && !parsedConfig.statusBar.showZoomSlider
              && !parsedConfig.statusBar.showDiskSpace
              && !parsedConfig.statusBar.showItemCount
              && parsedConfig.view.detailColumnWidths
                     == QList<int>({320, 90, 120, 180}),
          QStringLiteral("status-bar configuration completes a JSON round trip"));
    check(!roundTripJson.contains("remotePlaces")
              && !roundTripJson.contains("contextMenuItems"),
          QStringLiteral("configuration omits removed unimplemented fields"));

    const QString storedConfigPath =
        root.filePath(QStringLiteral("config/subdir/qtfilemgr.json"));
    QString configError;
    check(QtFileMgrConfigStore::saveFile(roundTripConfig, storedConfigPath, &configError)
              && QFileInfo::exists(storedConfigPath)
              && !QtFileMgrConfig::loadFromFile(storedConfigPath).statusBar.visible,
          QStringLiteral("configuration store creates directories and saves atomically"));

    QtFileMgrFilterBar filterBar;
    QLineEdit *filterEdit = filterBar.findChild<QLineEdit *>();
    int focusRequests = 0;
    int navigationKey = 0;
    int closeRequests = 0;
    QObject::connect(&filterBar, &QtFileMgrFilterBar::focusFileViewRequested,
                     [&]() { ++focusRequests; });
    QObject::connect(&filterBar, &QtFileMgrFilterBar::navigationKeyRequested,
                     [&](int key, Qt::KeyboardModifiers) { navigationKey = key; });
    QObject::connect(&filterBar, &QtFileMgrFilterBar::closeRequest,
                     [&]() { ++closeRequests; });
    QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QKeyEvent downEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QKeyEvent escapeEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    if (filterEdit) {
        QApplication::sendEvent(filterEdit, &enterEvent);
        QApplication::sendEvent(filterEdit, &downEvent);
        QApplication::sendEvent(filterEdit, &escapeEvent);
    }
    check(filterEdit && focusRequests == 2
              && navigationKey == Qt::Key_Down
              && closeRequests == 1,
          QStringLiteral("filter editor explicitly returns focus and forwards navigation"));

    const QString completionDirectory =
        root.filePath(QStringLiteral("completion_probe"));
    const QString hiddenCompletionDirectory =
        root.filePath(QStringLiteral(".hidden_completion_probe"));
    QDir().mkpath(completionDirectory);
    QDir().mkpath(hiddenCompletionDirectory);
    QtFileMgrBreadcrumbBar breadcrumbBar;
    breadcrumbBar.setUrl(QUrl::fromLocalFile(root.absolutePath()));
    breadcrumbBar.show();
    breadcrumbBar.setEditableMode(true);
    QLineEdit *breadcrumbEdit = breadcrumbBar.findChild<QLineEdit *>();
    QCompleter *pathCompleter = breadcrumbEdit ? breadcrumbEdit->completer() : nullptr;
    if (breadcrumbEdit) {
        breadcrumbEdit->setFocus();
        breadcrumbEdit->setText(root.absolutePath() + QLatin1Char('/'));
        breadcrumbEdit->textEdited(breadcrumbEdit->text());
    }
    auto completionRow = [&]() {
        if (!pathCompleter || !pathCompleter->completionModel())
            return QModelIndex();
        QAbstractItemModel *model = pathCompleter->completionModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->index(row, 0).data().toString()
                    == QStringLiteral("completion_probe"))
                return model->index(row, 0);
        }
        return QModelIndex();
    };
    const bool slashCompletionAppeared = waitUntil([&]() {
        return completionRow().isValid();
    });
    bool hiddenShownWithoutPrefix = false;
    if (pathCompleter && pathCompleter->completionModel()) {
        QAbstractItemModel *model = pathCompleter->completionModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            hiddenShownWithoutPrefix |= model->index(row, 0).data().toString()
                == QStringLiteral(".hidden_completion_probe");
        }
    }
    check(breadcrumbEdit && pathCompleter && slashCompletionAppeared
              && !qvariant_cast<QIcon>(
                      completionRow().data(Qt::DecorationRole)).isNull()
              && !hiddenShownWithoutPrefix,
          QStringLiteral("path completion is stable, iconized, and hides dot directories"));

    if (breadcrumbEdit) {
        breadcrumbEdit->setText(root.absolutePath() + QStringLiteral("/."));
        breadcrumbEdit->textEdited(breadcrumbEdit->text());
    }
    const bool explicitHiddenCompletion = waitUntil([&]() {
        if (!pathCompleter || !pathCompleter->completionModel())
            return false;
        QAbstractItemModel *model = pathCompleter->completionModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->index(row, 0).data().toString()
                    == QStringLiteral(".hidden_completion_probe"))
                return true;
        }
        return false;
    });
    check(explicitHiddenCompletion,
          QStringLiteral("typing a dot explicitly enables hidden-directory completion"));
    const QString deepBreadcrumbPath = root.filePath(
        QStringLiteral("deep/breadcrumb/path/with/many/segments"));
    QDir().mkpath(deepBreadcrumbPath);
    breadcrumbBar.setEditableMode(false);
    breadcrumbBar.resize(300, 32);
    breadcrumbBar.setUrl(QUrl::fromLocalFile(deepBreadcrumbPath));
    QApplication::processEvents();
    QToolButton *breadcrumbOverflow = breadcrumbBar.findChild<QToolButton *>(
        QStringLiteral("breadcrumbOverflow"));
    check(breadcrumbOverflow && breadcrumbOverflow->isVisible()
              && breadcrumbOverflow->menu()
              && !breadcrumbOverflow->menu()->actions().isEmpty(),
          QStringLiteral("narrow breadcrumbs collapse parent segments into an overflow menu"));
    breadcrumbBar.hide();

    const QString sourceFile = root.filePath(QStringLiteral("source.txt"));
    const QString copiedFile = root.filePath(QStringLiteral("copied.txt"));
    check(writeFile(sourceFile, QByteArrayLiteral("source-data")),
          QStringLiteral("source file created"));
    check(io.copy(sourceFile, copiedFile)
              && readFile(copiedFile) == QByteArrayLiteral("source-data"),
          QStringLiteral("file copy succeeds when destination is absent"));
    const QString cancellableSource = root.filePath(QStringLiteral("cancellable.bin"));
    const QString cancellableDestination =
        root.filePath(QStringLiteral("cancellable-copy.bin"));
    QFile cancellableFile(cancellableSource);
    const bool cancellableSourceCreated =
        cancellableFile.open(QIODevice::WriteOnly)
        && cancellableFile.resize(128 * 1024 * 1024);
    cancellableFile.close();
    check(cancellableSourceCreated,
          QStringLiteral("cancellable transfer source created"));
    int cancellationChecks = 0;
    qint64 cancelledTransferProgress = 0;
    QString cancellationError;
    const QtFileMgrLocalIO::TransferStatus cancellationStatus =
        QtFileMgrLocalIO::copyCancellable(
            cancellableSource, cancellableDestination,
            [&cancellationChecks]() { return ++cancellationChecks >= 4; },
            [&cancelledTransferProgress](qint64 transferred, qint64) {
                cancelledTransferProgress = transferred;
            },
            &cancellationError);
    check(cancellationStatus == QtFileMgrLocalIO::TransferCancelled
              && cancelledTransferProgress > 0
              && QFileInfo::exists(cancellableSource)
              && !QFileInfo::exists(cancellableDestination)
              && !cancellationError.isEmpty(),
          QStringLiteral("chunked local copy cancels mid-file and removes its partial target"));

    TrackingTrashIO trackingTrashIO(root.absolutePath());
    QtFileMgrFileView trackingTrashView(&trackingTrashIO);
    trackingTrashView.setUrl(QUrl::fromLocalFile(root.absolutePath()));
    const bool trashEntryLoaded = waitUntil([&]() {
        return !trackingTrashView.flatModel()->isLoading()
            && trackingTrashView.itemCount() == 1;
    });
    trackingTrashView.selectNewFiles({QStringLiteral("virtual.txt")});
    const bool trashEntrySelected =
        trackingTrashView.selectedFiles() == QStringList({trackingTrashIO.filePath()});
    check(trashEntryLoaded && trashEntrySelected,
          QStringLiteral("injected trash backend entry loads and can be selected"));
    QTimer::singleShot(0, &trackingTrashView, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (QMessageBox *messageBox = qobject_cast<QMessageBox *>(widget)) {
                if (QAbstractButton *yesButton =
                        messageBox->button(QMessageBox::Yes)) {
                    yesButton->click();
                }
                return;
            }
        }
    });
    trackingTrashView.deleteSelectedInternal(false);
    check(trackingTrashIO.trashCalls == 1
              && trackingTrashIO.removeCalls == 0
              && trackingTrashIO.trashedPath == trackingTrashIO.filePath(),
          QStringLiteral("FileView trash deletion stays inside the injected IO contract"));
    const QString directWriteFile =
        root.filePath(QStringLiteral("direct-write.txt"));
    check(io.writeFile(directWriteFile, QByteArrayLiteral("direct-write"))
              && readFile(directWriteFile) == QByteArrayLiteral("direct-write")
              && !io.writeFile(directWriteFile, QByteArrayLiteral("overwrite")),
          QStringLiteral("local file creation writes once and refuses overwrite"));
    bool previewTruncated = false;
    QString previewError;
    const QByteArray boundedPreview = io.readPreview(
        directWriteFile, 6, &previewTruncated, &previewError);
    check(boundedPreview == QByteArrayLiteral("direct")
              && previewTruncated && previewError.isEmpty(),
          QStringLiteral("local text preview enforces its byte limit"));
    check(!io.fileIcon(sourceFile).isNull(),
          QStringLiteral("local file icons are available without asynchronous model loading"));

    QtFileMgrPropertiesDialog batchProperties(
        &io, {sourceFile, copiedFile, sourceFile});
    QLabel *propertyName =
        batchProperties.findChild<QLabel *>(QStringLiteral("propertiesName"));
    QLabel *propertyType =
        batchProperties.findChild<QLabel *>(QStringLiteral("propertiesType"));
    check(batchProperties.itemCount() == 2
              && propertyName
              && propertyName->text() == QStringLiteral("2 个项目")
              && propertyType && !propertyType->text().isEmpty(),
          QStringLiteral("properties dialog deduplicates and summarizes multiple files"));
    QDialogButtonBox *propertyButtons =
        batchProperties.findChild<QDialogButtonBox *>();
    check(propertyButtons
              && propertyButtons->button(QDialogButtonBox::Apply)->text()
                     == QStringLiteral("应用")
              && propertyButtons->button(QDialogButtonBox::Close)->text()
                     == QStringLiteral("关闭"),
          QStringLiteral("properties dialog uses localized command labels"));

    const QString firstTrashRoot = root.filePath(QStringLiteral("trash-one"));
    const QString secondTrashRoot = root.filePath(
        QStringLiteral("trash-mount/.Trash-test"));
    const QString absoluteOriginal = root.filePath(
        QStringLiteral("restored/absolute.txt"));
    const QString relativeOriginal = root.filePath(
        QStringLiteral("trash-mount/restored/relative.txt"));
    const QString firstFiles = QDir(firstTrashRoot).filePath(QStringLiteral("files"));
    const QString firstInfo = QDir(firstTrashRoot).filePath(QStringLiteral("info"));
    const QString secondFiles = QDir(secondTrashRoot).filePath(QStringLiteral("files"));
    const QString secondInfo = QDir(secondTrashRoot).filePath(QStringLiteral("info"));
    check(QDir().mkpath(firstFiles) && QDir().mkpath(firstInfo)
              && QDir().mkpath(secondFiles) && QDir().mkpath(secondInfo)
              && writeFile(QDir(firstFiles).filePath(QStringLiteral("absolute-item")),
                           QByteArrayLiteral("absolute-trash"))
              && writeFile(QDir(firstFiles).filePath(QStringLiteral("orphan-item")),
                           QByteArrayLiteral("orphan-trash"))
              && writeFile(QDir(secondFiles).filePath(QStringLiteral("relative-item")),
                           QByteArrayLiteral("relative-trash"))
              && writeFile(
                  QDir(firstInfo).filePath(QStringLiteral("absolute-item.trashinfo")),
                  QByteArrayLiteral("[Trash Info]\nPath=")
                      + QUrl::toPercentEncoding(absoluteOriginal)
                      + QByteArrayLiteral("\nDeletionDate=2026-08-18T10:00:00\n"))
              && writeFile(
                  QDir(secondInfo).filePath(QStringLiteral("relative-item.trashinfo")),
                  QByteArrayLiteral("[Trash Info]\nPath=restored/relative.txt\n")
                      + QByteArrayLiteral("DeletionDate=2026-08-18T11:00:00\n"))
              && writeFile(
                  QDir(firstInfo).filePath(QStringLiteral("metadata-only.trashinfo")),
                  QByteArrayLiteral("[Trash Info]\nPath=/unused\n")
                      + QByteArrayLiteral("DeletionDate=2026-08-18T09:00:00\n")),
          QStringLiteral("isolated multi-root trash layout prepared"));
    const QList<QtFileMgrTrashEntry> isolatedTrashEntries =
        QtFileMgrTrashDialog::entriesForRoots(
            {firstTrashRoot, secondTrashRoot, firstTrashRoot});
    QtFileMgrTrashEntry absoluteEntry;
    QtFileMgrTrashEntry relativeEntry;
    QtFileMgrTrashEntry orphanEntry;
    for (const QtFileMgrTrashEntry &entry : isolatedTrashEntries) {
        if (entry.name == QStringLiteral("absolute-item"))
            absoluteEntry = entry;
        else if (entry.name == QStringLiteral("relative-item"))
            relativeEntry = entry;
        else if (entry.name == QStringLiteral("orphan-item"))
            orphanEntry = entry;
    }
    check(isolatedTrashEntries.size() == 3
              && absoluteEntry.originalPath == absoluteOriginal
              && absoluteEntry.infoPath.startsWith(firstTrashRoot)
              && relativeEntry.originalPath == relativeOriginal
              && relativeEntry.infoPath.startsWith(secondTrashRoot)
              && orphanEntry.originalPath.isEmpty()
              && !orphanEntry.metadataValid,
          QStringLiteral("trash listing merges roots and resolves per-root metadata"));
    QString trashRestoreError;
    check(QtFileMgrTrashDialog::restore(absoluteEntry, &trashRestoreError)
              && trashRestoreError.isEmpty()
              && readFile(absoluteOriginal) == QByteArrayLiteral("absolute-trash")
              && !QFileInfo::exists(absoluteEntry.infoPath)
              && QtFileMgrTrashDialog::restore(relativeEntry, &trashRestoreError)
              && readFile(relativeOriginal) == QByteArrayLiteral("relative-trash")
              && !QFileInfo::exists(relativeEntry.infoPath),
          QStringLiteral("trash restore moves data and removes matching metadata"));

    QtFileMgrTrashDialog trashDialog(
        QStringList{firstTrashRoot, secondTrashRoot});
    QDialogButtonBox *trashButtons = trashDialog.findChild<QDialogButtonBox *>();
    check(trashButtons
              && trashButtons->button(QDialogButtonBox::Close)->text()
                     == QStringLiteral("关闭")
              && trashDialog.entryCount() == 1,
          QStringLiteral("trash dialog uses localized controls and isolated roots"));
    QString emptyTrashError;
    check(QtFileMgrTrashDialog::emptyTrashRoots(
              {firstTrashRoot, secondTrashRoot}, &emptyTrashError)
              && emptyTrashError.isEmpty()
              && QDir(firstFiles).entryList(
                     QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()
              && QDir(firstInfo).entryList(
                     QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()
              && QDir(secondFiles).entryList(
                     QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()
              && QDir(secondInfo).entryList(
                     QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
          QStringLiteral("trash emptying removes files and metadata from every root"));

    QtFileMgrPreviewPanel previewPanel;
    previewPanel.setIO(&io);
    previewPanel.setFiles({directWriteFile});
    check(waitUntil([&]() {
              return previewPanel.previewMode() == QtFileMgrPreviewPanel::TextPreview;
          })
              && previewPanel.previewText() == QStringLiteral("direct-write"),
          QStringLiteral("preview panel renders bounded text asynchronously"));

    const QString imagePath = root.filePath(QStringLiteral("preview.png"));
    QImage image(64, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(20, 120, 200));
    check(image.save(imagePath)
              && io.hasThumbnails()
              && !io.thumbnail(imagePath, QSize(24, 24)).isNull()
              && io.thumbnail(imagePath, QSize(24, 24)).size().width() <= 24
              && io.thumbnail(imagePath, QSize(24, 24)).size().height() <= 24,
          QStringLiteral("local IO produces bounded image thumbnails"));
    previewPanel.setFiles({imagePath});
    check(waitUntil([&]() {
              return previewPanel.previewMode() == QtFileMgrPreviewPanel::ImagePreview;
          }), QStringLiteral("preview panel decodes bounded image asynchronously"));
    previewPanel.setFiles({sourceFile, imagePath});
    QLabel *previewDetails =
        previewPanel.findChild<QLabel *>(QStringLiteral("previewDetails"));
    check(previewPanel.previewMode() == QtFileMgrPreviewPanel::EmptyPreview
              && previewDetails
              && previewDetails->text().contains(QStringLiteral("2 个文件")),
          QStringLiteral("preview panel summarizes multiple selections"));

    const QString templatePath =
        root.filePath(QStringLiteral("document-template.txt"));
    check(writeFile(templatePath, QByteArrayLiteral("template-body")),
          QStringLiteral("file template prepared"));
    QtFileMgrFileView desktopWorkflowView(&io);
    desktopWorkflowView.setUrl(QUrl::fromLocalFile(root.absolutePath()));
    waitUntil([&]() { return !desktopWorkflowView.flatModel()->isLoading(); });
    QString createdFromTemplate;
    check(desktopWorkflowView.createFileFromTemplate(
              templatePath, &createdFromTemplate)
              && createdFromTemplate != templatePath
              && readFile(createdFromTemplate) == QByteArrayLiteral("template-body"),
          QStringLiteral("file view creates a collision-safe file from a template"));

    const QString renameRoot = root.filePath(QStringLiteral("inline-rename"));
    const QString renameOldPath = QDir(renameRoot).filePath(
        QStringLiteral("before.txt"));
    const QString renameNewPath = QDir(renameRoot).filePath(
        QStringLiteral("after.txt"));
    check(QDir().mkpath(renameRoot)
              && writeFile(renameOldPath, QByteArrayLiteral("rename-body")),
          QStringLiteral("inline rename fixture prepared"));
    QtFileMgrFileView renameView(&io);
    renameView.setUrl(QUrl::fromLocalFile(renameRoot));
    check(waitUntil([&]() {
              return !renameView.flatModel()->isLoading()
                  && renameView.flatModel()->rowCount() == 1;
          }),
          QStringLiteral("inline rename model loaded"));
    QModelIndex renameIndex;
    for (int row = 0; row < renameView.flatModel()->rowCount(); ++row) {
        const QModelIndex candidate = renameView.flatModel()->index(row, 0);
        if (candidate.data(Qt::DisplayRole).toString()
            == QStringLiteral("before.txt")) {
            renameIndex = candidate;
            break;
        }
    }
    bool insideSetModelData = false;
    bool resetDuringSetModelData = false;
    int renameResetCount = 0;
    QObject::connect(
        renameView.flatModel(), &QAbstractItemModel::modelAboutToBeReset,
        [&]() {
            ++renameResetCount;
            if (insideSetModelData)
                resetDuringSetModelData = true;
        });
    QLineEdit renameEditor;
    renameEditor.setText(QStringLiteral("after.txt"));
    insideSetModelData = true;
    renameView.delegate()->setModelData(
        &renameEditor, renameView.flatModel(), renameIndex);
    insideSetModelData = false;
    // 异步重命名 (DLG-P1-5): 提交返回时操作尚未完成,等下一事件轮次
    check(renameIndex.isValid()
              && waitUntil([&]() {
                  return QFileInfo::exists(renameNewPath);
              }, 3000)
              && !QFileInfo::exists(renameOldPath)
              && !resetDuringSetModelData,
          QStringLiteral("inline rename does not reset the model during commit"));
    check(waitUntil([&]() {
              if (renameView.flatModel()->isLoading()
                  || renameResetCount == 0) {
                  return false;
              }
              for (int row = 0; row < renameView.flatModel()->rowCount(); ++row) {
                  if (renameView.flatModel()->index(row, 0)
                          .data(Qt::DisplayRole).toString()
                      == QStringLiteral("after.txt")) {
                      return true;
                  }
              }
              return false;
          }),
          QStringLiteral("inline rename reloads the model with the new name"));
    QStringList openWithPaths;
    QString openWithMime;
    desktopWorkflowView.setOpenWithHandler(
        [&](const QStringList &paths, const QString &mimeType, QWidget *) {
        openWithPaths = paths;
        openWithMime = mimeType;
        return true;
    });
    check(desktopWorkflowView.openWith({sourceFile})
              && openWithPaths == QStringList{sourceFile}
              && openWithMime == QStringLiteral("text/plain"),
          QStringLiteral("open-with integration receives paths and the MIME hint"));
    QtFileMgrFlatModel versionModel(&io);
    versionModel.setUrl(root.absolutePath());
    check(waitUntil([&]() { return !versionModel.isLoading(); }),
          QStringLiteral("flat model completes asynchronous local listing"));
    versionModel.setShowVersions(true);
    check(versionModel.columnCount() == 5
              && versionModel.headerData(4, Qt::Horizontal, Qt::DisplayRole)
                     == QStringLiteral("版本"),
          QStringLiteral("version display adds a real details column"));

    const QString largeDirectory =
        root.filePath(QStringLiteral("large-async-directory"));
    check(QDir().mkpath(largeDirectory),
          QStringLiteral("large asynchronous directory created"));
    bool largeDirectoryPrepared = true;
    for (int i = 0; i < 700; ++i) {
        largeDirectoryPrepared = writeFile(
            QDir(largeDirectory).filePath(
                QStringLiteral("entry-%1.txt").arg(i, 4, 10, QLatin1Char('0'))),
            QByteArrayLiteral("x")) && largeDirectoryPrepared;
    }
    check(largeDirectoryPrepared,
          QStringLiteral("large asynchronous directory populated"));
    QtFileMgrFlatModel largeDirectoryModel(&io);
    int batchCount = 0;
    QObject::connect(
        &largeDirectoryModel, &QtFileMgrFlatModel::directoryBatchLoaded,
        [&](int, int) { ++batchCount; });
    QElapsedTimer listCallTimer;
    listCallTimer.start();
    largeDirectoryModel.setUrl(largeDirectory);
    const bool returnedWhileLoading =
        listCallTimer.elapsed() < 100 && largeDirectoryModel.isLoading();
    check(returnedWhileLoading,
          QStringLiteral("local directory listing returns without blocking the UI thread"));
    check(waitUntil([&]() {
              return !largeDirectoryModel.isLoading()
                  && largeDirectoryModel.sourceItemCount() == 700;
          }, 5000)
              && batchCount >= 3,
          QStringLiteral("large local directory arrives in bounded batches"));

    DelayedListingIO listingIO;
    QtFileMgrFlatModel navigationModel(&listingIO);
    navigationModel.setUrl(QStringLiteral("slow"));
    navigationModel.setUrl(QStringLiteral("fast"));
    check(waitUntil([&]() { return !navigationModel.isLoading(); })
              && navigationModel.rowCount() == 1
              && navigationModel.index(0, 0).data(Qt::DisplayRole).toString()
                     == QStringLiteral("fast.txt"),
          QStringLiteral("rapid navigation discards stale directory results"));
    const int requestsBeforeFiltering = listingIO.listRequests;
    navigationModel.setNameFilter(QStringLiteral("fast"));
    navigationModel.setSortColumn(3, Qt::DescendingOrder);
    check(listingIO.listRequests == requestsBeforeFiltering
              && navigationModel.rowCount() == 1,
          QStringLiteral("filtering and sorting reuse the loaded source snapshot"));

    ProgressVersionRemoteIO notificationIO;
    QtFileMgrFileView notificationView(&notificationIO);
    // 本替身不参与跨后端复用/传输路由,避免影响后续用例
    QtFileMgrFileView::unregisterRemoteBackend(&notificationIO);
    notificationView.setUrl(QUrl(QStringLiteral("remote:///watched")));
    check(waitUntil([&]() {
              return !notificationView.flatModel()->isLoading();
          }),
          QStringLiteral("notification view completes initial remote listing"));
    const int requestsBeforeNotification = notificationIO.listRequests;
    emit notificationIO.pathsChanged(
        {QStringLiteral("watched/new-file.txt")});
    check(waitUntil([&]() {
              return notificationIO.listRequests > requestsBeforeNotification;
          }),
          QStringLiteral("backend path notification refreshes the affected directory"));

    const QString existingFile = root.filePath(QStringLiteral("existing.txt"));
    check(writeFile(existingFile, QByteArrayLiteral("keep-me")),
          QStringLiteral("existing destination created"));
    check(!io.copy(sourceFile, existingFile)
              && readFile(existingFile) == QByteArrayLiteral("keep-me"),
          QStringLiteral("copy refuses to overwrite existing destination"));

    const QString sourceDir = root.filePath(QStringLiteral("source-dir"));
    const QString sourceChild = QDir(sourceDir).filePath(QStringLiteral("nested/item.txt"));
    check(QDir().mkpath(QFileInfo(sourceChild).absolutePath())
              && writeFile(sourceChild, QByteArrayLiteral("nested-data")),
          QStringLiteral("source directory tree created"));
    const QString copiedDir = root.filePath(QStringLiteral("copied-dir"));
    check(io.copy(sourceDir, copiedDir)
              && readFile(QDir(copiedDir).filePath(QStringLiteral("nested/item.txt")))
                     == QByteArrayLiteral("nested-data"),
          QStringLiteral("directory copy preserves nested files"));

    const QString selfChild = QDir(sourceDir).filePath(QStringLiteral("child-copy"));
    check(!io.copy(sourceDir, selfChild) && !QFileInfo::exists(selfChild),
          QStringLiteral("directory cannot be copied into its own child"));
    const QString selfMoveChild = QDir(sourceDir).filePath(QStringLiteral("child-move"));
    check(!io.rename(sourceDir, selfMoveChild)
              && QFileInfo::exists(sourceDir)
              && !QFileInfo::exists(selfMoveChild),
          QStringLiteral("directory cannot be moved into its own child"));

    const QString sourceAlias = root.filePath(QStringLiteral("source-dir-alias"));
    check(QFile::link(sourceDir, sourceAlias) && QFileInfo(sourceAlias).isSymLink(),
          QStringLiteral("source directory alias created"));
    QtFileMgrPropertiesDialog linkProperties(&io, {sourceAlias});
    QLabel *linkTarget = linkProperties.findChild<QLabel *>(
        QStringLiteral("propertiesLinkTarget"));
    check(linkTarget && linkTarget->text() == sourceDir,
          QStringLiteral("properties dialog exposes a symbolic-link target"));
    const QString aliasedChild = QDir(sourceAlias).filePath(QStringLiteral("aliased-child-copy"));
    check(!io.copy(sourceDir, aliasedChild) && !QFileInfo::exists(aliasedChild),
          QStringLiteral("directory child detection resolves parent symbolic links"));

    const QString targetDir = root.filePath(QStringLiteral("link-target"));
    const QString targetFile = QDir(targetDir).filePath(QStringLiteral("target.txt"));
    check(QDir().mkpath(targetDir)
              && writeFile(targetFile, QByteArrayLiteral("target-data")),
          QStringLiteral("symbolic-link target created"));
    const QString sourceLink = root.filePath(QStringLiteral("source-link"));
    check(QFile::link(targetDir, sourceLink) && QFileInfo(sourceLink).isSymLink(),
          QStringLiteral("directory symbolic link created"));

    const QString copiedLink = root.filePath(QStringLiteral("copied-link"));
    check(io.copy(sourceLink, copiedLink) && QFileInfo(copiedLink).isSymLink(),
          QStringLiteral("copy preserves a directory symbolic link"));
    const QString relativeTarget = QStringLiteral("link-target/target.txt");
    const QString relativeLink = root.filePath(QStringLiteral("relative-link"));
    const QString copiedRelativeLink = root.filePath(QStringLiteral("copied-relative-link"));
    check(io.symlink(relativeTarget, relativeLink)
              && io.copy(relativeLink, copiedRelativeLink)
              && io.readLink(copiedRelativeLink) == relativeTarget,
          QStringLiteral("copy preserves a relative symbolic-link target"));
    check(io.remove(copiedLink)
              && !QFileInfo(copiedLink).isSymLink()
              && readFile(targetFile) == QByteArrayLiteral("target-data"),
          QStringLiteral("removing a symbolic link keeps its target intact"));

    bool callbackCalled = false;
    // R2-IO-P1-5: copyAsync 在工作线程执行,回调回到调用线程
    const QString asyncCopyDest = root.filePath(QStringLiteral("async-copy.txt"));
    bool asyncCopyOk = false;
    bool asyncCallbackOnMainThread = false;
    QEventLoop loop;
    io.copyAsync(sourceFile, asyncCopyDest, &loop,
                 [&](bool success, const QString &) {
        callbackCalled = true;
        asyncCopyOk = success;
        asyncCallbackOnMainThread = QThread::currentThread() == qApp->thread();
        loop.quit();
    });
    const bool notSynchronous = !callbackCalled;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    check(notSynchronous && callbackCalled && asyncCopyOk
              && asyncCallbackOnMainThread
              && readFile(asyncCopyDest) == readFile(sourceFile),
          QStringLiteral("copyAsync runs off the caller thread and delivers on it"));

    callbackCalled = false;
    int moved = -1;
    int failed = -1;
    io.batchMoveAsync(QStringList() << sourceFile, root.absolutePath(), &loop,
                      [&](int okCount, int failCount, const QStringList &) {
        callbackCalled = true;
        moved = okCount;
        failed = failCount;
        loop.quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    check(callbackCalled && moved == 0 && failed == 0 && QFileInfo::exists(sourceFile),
          QStringLiteral("moving within the same directory is a no-op"));

    callbackCalled = false;
    int copied = -1;
    failed = -1;
    io.batchCopyAsync({sourceFile}, QString(), &loop,
                      [&](int okCount, int failCount, const QStringList &) {
        callbackCalled = true;
        copied = okCount;
        failed = failCount;
        loop.quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    check(callbackCalled && copied == 0 && failed == 1
              && QFileInfo::exists(sourceFile),
          QStringLiteral("batch copy rejects an empty destination directory"));

    callbackCalled = false;
    moved = -1;
    failed = -1;
    io.batchMoveAsync({sourceFile}, QString(), &loop,
                      [&](int okCount, int failCount, const QStringList &) {
        callbackCalled = true;
        moved = okCount;
        failed = failCount;
        loop.quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    check(callbackCalled && moved == 0 && failed == 1
              && QFileInfo::exists(sourceFile),
          QStringLiteral("batch move rejects an empty destination directory"));

    QTemporaryDir crossFsDir(QStringLiteral("/dev/shm/qtfilemgr-test-XXXXXX"));
    if (crossFsDir.isValid()) {
        const QString moveSource = root.filePath(QStringLiteral("move-source.txt"));
        const QString moveDestination =
            QDir(crossFsDir.path()).filePath(QStringLiteral("move-destination.txt"));
        check(writeFile(moveSource, QByteArrayLiteral("cross-filesystem-data")),
              QStringLiteral("cross-filesystem move source created"));
        check(io.rename(moveSource, moveDestination)
                   && !QFileInfo::exists(moveSource)
                   && readFile(moveDestination) == QByteArrayLiteral("cross-filesystem-data"),
               QStringLiteral("cross-filesystem move falls back to copy and remove"));

        const QString partialSource = root.filePath(QStringLiteral("partial-remove-source"));
        const QString lockedDir = QDir(partialSource).filePath(QStringLiteral("locked"));
        const QString partialDestination = QDir(crossFsDir.path()).filePath(
            QStringLiteral("partial-remove-copy"));
        const QString lockedFile = QDir(lockedDir).filePath(QStringLiteral("keep.txt"));
        check(QDir().mkpath(lockedDir)
                  && writeFile(lockedFile, QByteArrayLiteral("complete copy"))
                  && writeFile(QDir(partialSource).filePath(QStringLiteral("other.txt")),
                               QByteArrayLiteral("other data")),
              QStringLiteral("prepare source for partial removal failure"));
        check(QFile::setPermissions(lockedDir, QFile::ReadOwner | QFile::ExeOwner),
              QStringLiteral("deny source child removal without blocking copy"));
        QString partialError;
        const auto partialStatus = QtFileMgrLocalIO::moveCancellable(
            partialSource, partialDestination, {}, {}, &partialError);
        check(partialStatus == QtFileMgrLocalIO::TransferFailed
                  && partialError.contains(partialDestination)
                  && readFile(QDir(partialDestination).filePath(
                                  QStringLiteral("locked/keep.txt"))) == QByteArrayLiteral("complete copy")
                  && readFile(QDir(partialDestination).filePath(
                                  QStringLiteral("other.txt"))) == QByteArrayLiteral("other data"),
              QStringLiteral("failed source removal preserves complete cross-filesystem copy"));
        check(QFile::setPermissions(lockedDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)
                  && QFile::setPermissions(QDir(partialDestination).filePath(QStringLiteral("locked")),
                                           QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
              QStringLiteral("restore isolated test directory permissions"));

    } else {
        QTextStream(stdout) << "SKIP: /dev/shm is unavailable for cross-filesystem move"
                            << '\n';
    }

    const QString jobSourceDir = root.filePath(QStringLiteral("job-sources"));
    const QString jobDestinationDir = root.filePath(QStringLiteral("job-destination"));
    check(QDir().mkpath(jobSourceDir) && QDir().mkpath(jobDestinationDir),
          QStringLiteral("file-operation job directories created"));

    const QString autoRenameSource =
        QDir(jobSourceDir).filePath(QStringLiteral("auto.txt"));
    const QString autoRenameExisting =
        QDir(jobDestinationDir).filePath(QStringLiteral("auto.txt"));
    check(writeFile(autoRenameSource, QByteArrayLiteral("auto-new"))
              && writeFile(autoRenameExisting, QByteArrayLiteral("auto-old")),
          QStringLiteral("auto-rename conflict prepared"));
    QtFileMgrFileOperationJob autoRenameJob(
        &io,
        QtFileMgrFileOperationJob::Copy,
        {autoRenameSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::AutoRename);
    autoRenameJob.start();
    check(autoRenameJob.isRunning(),
          QStringLiteral("local file-operation job returns while work is active"));
    JobOutcome autoRenameOutcome;
    {
        QEventLoop loop;
        QObject::connect(
            &autoRenameJob, &QtFileMgrFileOperationJob::finished, &loop,
            [&](int succeeded, int failed, int skipped, const QStringList &newNames,
                bool cancelled, const QStringList &errors) {
                autoRenameOutcome.callbackCalled = true;
                autoRenameOutcome.succeeded = succeeded;
                autoRenameOutcome.failed = failed;
                autoRenameOutcome.skipped = skipped;
                autoRenameOutcome.newNames = newNames;
                autoRenameOutcome.cancelled = cancelled;
                autoRenameOutcome.errors = errors;
                loop.quit();
            });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(autoRenameOutcome.callbackCalled
              && autoRenameOutcome.succeeded == 1
              && readFile(autoRenameExisting) == QByteArrayLiteral("auto-old")
              && readFile(QDir(jobDestinationDir).filePath(QStringLiteral("auto (1).txt")))
                     == QByteArrayLiteral("auto-new"),
          QStringLiteral("job auto-renames conflicts without changing existing target"));

    const QString skipSource = QDir(jobSourceDir).filePath(QStringLiteral("skip.txt"));
    const QString skipDestination =
        QDir(jobDestinationDir).filePath(QStringLiteral("skip.txt"));
    check(writeFile(skipSource, QByteArrayLiteral("skip-new"))
              && writeFile(skipDestination, QByteArrayLiteral("skip-old")),
          QStringLiteral("skip conflict prepared"));
    QtFileMgrFileOperationJob skipJob(
        &io,
        QtFileMgrFileOperationJob::Copy,
        {skipSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::SkipExisting);
    const JobOutcome skipOutcome = runJob(skipJob);
    check(skipOutcome.callbackCalled
              && skipOutcome.succeeded == 0
              && skipOutcome.failed == 0
              && skipOutcome.skipped == 1
              && readFile(skipDestination) == QByteArrayLiteral("skip-old"),
          QStringLiteral("job skips existing targets when requested"));

    const QString overwriteSource =
        QDir(jobSourceDir).filePath(QStringLiteral("overwrite.txt"));
    const QString overwriteDestination =
        QDir(jobDestinationDir).filePath(QStringLiteral("overwrite.txt"));
    check(writeFile(overwriteSource, QByteArrayLiteral("overwrite-new"))
              && writeFile(overwriteDestination, QByteArrayLiteral("overwrite-old")),
          QStringLiteral("overwrite conflict prepared"));
    QtFileMgrFileOperationJob overwriteJob(
        &io,
        QtFileMgrFileOperationJob::Copy,
        {overwriteSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::OverwriteExisting);
    const JobOutcome overwriteOutcome = runJob(overwriteJob);
    const QStringList destinationEntries = QDir(jobDestinationDir).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    bool hasTemporaryArtifact = false;
    for (const QString &entry : destinationEntries) {
        if (entry.startsWith(QStringLiteral(".qtfilemgr-"))) {
            hasTemporaryArtifact = true;
            break;
        }
    }
    check(overwriteOutcome.callbackCalled
              && overwriteOutcome.succeeded == 1
              && readFile(overwriteSource) == QByteArrayLiteral("overwrite-new")
              && readFile(overwriteDestination) == QByteArrayLiteral("overwrite-new")
              && !hasTemporaryArtifact,
          QStringLiteral("job replaces existing target transactionally"));

    // R2-IO-P2-1: start 前的 cancel 必须生效,任务不得照常执行
    const QString preCancelSource =
        QDir(jobSourceDir).filePath(QStringLiteral("pre-cancel.txt"));
    check(writeFile(preCancelSource, QByteArrayLiteral("pre-cancel")),
          QStringLiteral("pre-cancel source created"));
    QtFileMgrFileOperationJob preCancelledJob(
        &io,
        QtFileMgrFileOperationJob::Copy,
        {preCancelSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::AutoRename);
    preCancelledJob.cancel();
    const JobOutcome preCancelledOutcome = runJob(preCancelledJob);
    check(preCancelledOutcome.callbackCalled
              && preCancelledOutcome.cancelled
              && preCancelledOutcome.succeeded == 0
              && !QFileInfo::exists(
                  QDir(jobDestinationDir).filePath(QStringLiteral("pre-cancel.txt"))),
          QStringLiteral("cancel before start short-circuits the job"));

    // Remote overwrite backs up the original instead of deleting it first.
    {
        OverwritableRemoteIO overwriteRemote;
        overwriteRemote.occupiedPath = QStringLiteral("dest/clash.txt");
        QtFileMgrFileOperationJob remoteOverwriteJob(
            &overwriteRemote,
            QtFileMgrFileOperationJob::Copy,
            {QStringLiteral("local/clash.txt")},
            QStringLiteral("dest"),
            QtFileMgrFileOperationJob::OverwriteExisting);
        const JobOutcome overwriteRemoteOutcome = runJob(remoteOverwriteJob);
        check(overwriteRemoteOutcome.callbackCalled
                  && overwriteRemoteOutcome.succeeded == 1
                  && overwriteRemoteOutcome.skipped == 0
                  && overwriteRemote.removedPaths.isEmpty()
                  && overwriteRemote.renamedPairs.size() == 2
                  && overwriteRemote.exists(QStringLiteral("dest/clash.txt")),
              QStringLiteral("remote overwrite preserves the original through backup and guarded commit"));
    }

    const QString moveSource = QDir(jobSourceDir).filePath(QStringLiteral("job-move.txt"));
    check(writeFile(moveSource, QByteArrayLiteral("job-move-data")),
          QStringLiteral("job move source created"));
    QtFileMgrFileOperationJob moveJob(
        &io,
        QtFileMgrFileOperationJob::Move,
        {moveSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome moveOutcome = runJob(moveJob);
    check(moveOutcome.callbackCalled
              && moveOutcome.succeeded == 1
              && !QFileInfo::exists(moveSource)
              && readFile(QDir(jobDestinationDir).filePath(QStringLiteral("job-move.txt")))
                     == QByteArrayLiteral("job-move-data"),
          QStringLiteral("job moves a local file in the worker thread"));

    const QString sameDirectorySource =
        QDir(jobSourceDir).filePath(QStringLiteral("same-directory.txt"));
    const QString jobSourceAlias = root.filePath(QStringLiteral("job-sources-alias"));
    check(writeFile(sameDirectorySource, QByteArrayLiteral("same-directory-data"))
              && QFile::link(jobSourceDir, jobSourceAlias),
          QStringLiteral("job same-directory alias prepared"));
    QtFileMgrFileOperationJob sameDirectoryJob(
        &io,
        QtFileMgrFileOperationJob::Move,
        {sameDirectorySource},
        jobSourceAlias,
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome sameDirectoryOutcome = runJob(sameDirectoryJob);
    check(sameDirectoryOutcome.callbackCalled
              && sameDirectoryOutcome.succeeded == 0
              && sameDirectoryOutcome.skipped == 1
              && QFileInfo::exists(sameDirectorySource)
              && !QFileInfo(QDir(jobSourceAlias).filePath(QStringLiteral("same-directory (1).txt")))
                      .exists(),
          QStringLiteral("job treats a symlink alias of the same directory as a no-op"));

    const QString cancelledLocalDestination =
        QDir(jobDestinationDir).filePath(QStringLiteral("cancellable.bin"));
    QtFileMgrFileOperationJob cancelledLocalJob(
        &io,
        QtFileMgrFileOperationJob::Copy,
        {cancellableSource},
        jobDestinationDir,
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome cancelledLocalOutcome = runJob(cancelledLocalJob, true);
    check(cancelledLocalOutcome.callbackCalled
              && cancelledLocalOutcome.cancelled
              && cancelledLocalOutcome.succeeded == 0
              && cancelledLocalOutcome.failed == 0
              && QFileInfo::exists(cancellableSource)
              && !QFileInfo::exists(cancelledLocalDestination),
          QStringLiteral("job cancellation reaches the local transfer and leaves no target"));

    DelayedRemoteIO delayedRemote;
    QtFileMgrFileOperationJob cancelledJob(
        &delayedRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("remote-one.txt"), QStringLiteral("remote-two.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome cancelledOutcome = runJob(cancelledJob, true);
    check(cancelledOutcome.callbackCalled
              && cancelledOutcome.cancelled
              && cancelledOutcome.succeeded == 1
              && cancelledOutcome.finalProgress == 1,
          QStringLiteral("job cancellation finishes the active remote item and stops the rest"));

    DuplicateCallbackRemoteIO duplicateRemote;
    QtFileMgrFileOperationJob duplicateCallbackJob(
        &duplicateRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("source/first.txt"), QStringLiteral("source/second.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome duplicateCallbackOutcome = runJob(duplicateCallbackJob);
    check(duplicateCallbackOutcome.callbackCalled
              && duplicateCallbackOutcome.succeeded == 2
              && duplicateCallbackOutcome.newNames
                     == QStringList({QStringLiteral("first.txt"),
                                     QStringLiteral("second.txt")})
              && duplicateRemote.copyRequests == 2,
          QStringLiteral("duplicate remote callbacks are consumed only once"));

    ProgressVersionRemoteIO progressRemote;
    QtFileMgrFileOperationJob progressJob(
        &progressRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("source/progress.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    qint64 finalTransferred = -1;
    qint64 finalTotal = -1;
    QObject::connect(
        &progressJob, &QtFileMgrFileOperationJob::transferProgress,
        [&](int, int, const QString &, qint64 transferred, qint64 total) {
            finalTransferred = transferred;
            finalTotal = total;
        });
    const JobOutcome progressOutcome = runJob(progressJob);
    check(progressOutcome.callbackCalled
              && progressOutcome.succeeded == 1
              && finalTransferred == 100 && finalTotal == 100,
          QStringLiteral("remote transfer reports byte progress through the operation job"));

    SynchronousRequestRemoteIO synchronousRemote;
    QtFileMgrFileOperationJob synchronousJob(
        &synchronousRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("source/synchronous.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome synchronousOutcome = runJob(synchronousJob);
    check(synchronousOutcome.callbackCalled
              && synchronousOutcome.succeeded == 1
              && synchronousOutcome.failed == 0,
          QStringLiteral("job consumes a transfer request completed before return"));

    SynchronousListingRemoteIO synchronousListing;
    QtFileMgrFlatModel synchronousListingModel(&synchronousListing);
    bool synchronousListingFinished = false;
    QObject::connect(&synchronousListingModel,
                     &QtFileMgrFlatModel::directoryLoadFinished,
                     [&](const QString &, const QString &error) {
        synchronousListingFinished = error.isEmpty();
    });
    synchronousListingModel.setUrl(QStringLiteral("/"));
    check(synchronousListingFinished && synchronousListingModel.rowCount() == 1
              && synchronousListingModel.data(
                     synchronousListingModel.index(0, 0), Qt::DisplayRole).toString()
                     == QStringLiteral("synchronous.txt"),
          QStringLiteral("model consumes a directory request completed before return"));

    DroppedRequestRemoteIO droppedRemote;
    QtFileMgrFileOperationJob droppedJob(
        &droppedRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("source/dropped.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    const JobOutcome droppedOutcome = runJob(droppedJob);
    check(droppedOutcome.callbackCalled
              && droppedOutcome.succeeded == 0
              && droppedOutcome.failed == 1
              && !droppedOutcome.errors.isEmpty(),
          QStringLiteral("job fails cleanly when a backend drops its transfer request"));

    QThread backendThread;
    DelayedRemoteIO threadedRemote;
    threadedRemote.moveToThread(&backendThread);
    backendThread.start();

    JobOutcome affinityOutcome;
    QtFileMgrFileOperationJob affinityJob(
        &threadedRemote,
        QtFileMgrFileOperationJob::Copy,
        {QStringLiteral("source/threaded.txt")},
        QStringLiteral("destination"),
        QtFileMgrFileOperationJob::AutoRename);
    QObject::connect(
        &affinityJob, &QtFileMgrFileOperationJob::finished,
        [&](int succeeded, int failed, int skipped,
            const QStringList &, bool cancelled, const QStringList &errors) {
            affinityOutcome.callbackCalled = true;
            affinityOutcome.succeeded = succeeded;
            affinityOutcome.failed = failed;
            affinityOutcome.skipped = skipped;
            affinityOutcome.cancelled = cancelled;
            affinityOutcome.errors = errors;
        });
    affinityJob.start();
    QtFileMgrFlatModel affinityModel(&io);
    affinityModel.setIO(&threadedRemote);
    QtFileMgrFileView affinityView(&io);
    affinityView.setIO(&threadedRemote);
    QtFileMgrFlatModel rejectedAffinityModel(&threadedRemote);
    QtFileMgrFileView rejectedAffinityView(&threadedRemote);
    QtFileMgrFileView::unregisterRemoteBackend(&threadedRemote);
    check(affinityOutcome.callbackCalled
              && affinityOutcome.succeeded == 0
              && affinityOutcome.failed == 1
              && !affinityOutcome.errors.isEmpty()
              && affinityModel.io() == &io
              && affinityView.io() == &io
              && rejectedAffinityModel.io() == nullptr
              && rejectedAffinityView.io() != &threadedRemote,
          QStringLiteral("GUI models and jobs reject a backend owned by another thread"));

    QMetaObject::invokeMethod(
        &threadedRemote,
        [&threadedRemote, &app]() {
            threadedRemote.moveToThread(app.thread());
        },
        Qt::BlockingQueuedConnection);
    backendThread.quit();
    backendThread.wait();

    QtFileMgrVersionsDialog versionsDialog(
        &progressRemote, QStringLiteral("source/progress.txt"));
    check(waitUntil([&]() { return versionsDialog.versionCount() == 2; })
              && versionsDialog.selectedVersion() == 7,
          QStringLiteral("version dialog completes asynchronous history listing"));
    bool restoredSignal = false;
    QObject::connect(
        &versionsDialog, &QtFileMgrVersionsDialog::versionRestored,
        [&](const QString &path, int version) {
            restoredSignal = path == QStringLiteral("source/progress.txt")
                && version == 7;
        });
    versionsDialog.restoreVersion(7);
    check(waitUntil([&]() {
              return progressRemote.restoredVersion == 7 && restoredSignal;
          }),
          QStringLiteral("version restore closes the backend-to-dialog workflow"));

    const QString stateRoot = root.filePath(QStringLiteral("widget-state"));
    const QString firstDirectory = QDir(stateRoot).filePath(QStringLiteral("first"));
    const QString secondDirectory = QDir(stateRoot).filePath(QStringLiteral("second"));
    check(QDir().mkpath(firstDirectory)
              && QDir().mkpath(secondDirectory)
              && writeFile(QDir(firstDirectory).filePath(QStringLiteral("one.txt")),
                           QByteArrayLiteral("one"))
              && writeFile(QDir(firstDirectory).filePath(QStringLiteral("two.txt")),
                           QByteArrayLiteral("two")),
          QStringLiteral("widget state test tree created"));

    QtFileMgrConfig stateConfig = QtFileMgrConfig::defaultConfig();
    stateConfig.rootPath = stateRoot;
    stateConfig.allowedPaths = QStringList() << stateRoot;
    QtFileMgrWidget stateWidget(stateConfig);
    check(stateWidget.url() == QUrl::fromLocalFile(stateRoot)
              && stateWidget.io() == stateWidget.fileView()->io(),
          QStringLiteral("widget owns the initial URL and IO state"));

    stateWidget.setUrl(QUrl::fromLocalFile(firstDirectory));
    stateWidget.setUrl(QUrl::fromLocalFile(secondDirectory));
    check(stateWidget.canGoBack() && !stateWidget.canGoForward(),
          QStringLiteral("widget records navigation history"));
    stateWidget.goBack();
    check(stateWidget.url() == QUrl::fromLocalFile(firstDirectory)
              && stateWidget.canGoForward(),
          QStringLiteral("widget back navigation restores the previous URL"));
    stateWidget.goForward();
    check(stateWidget.url() == QUrl::fromLocalFile(secondDirectory),
          QStringLiteral("widget forward navigation restores the next URL"));
    stateWidget.goBack();
    stateWidget.setUrl(QUrl::fromLocalFile(stateRoot));
    check(!stateWidget.canGoForward(),
          QStringLiteral("new navigation drops the stale forward history"));

    const QUrl acceptedLocalUrl = stateWidget.url();
    stateWidget.setUrl(QUrl(QStringLiteral("remote:///uif")));
    check(stateWidget.url() == acceptedLocalUrl,
          QStringLiteral("local IO rejects a remote URL without changing state"));
    stateWidget.setUrl(QUrl::fromLocalFile(root.filePath(QStringLiteral("outside"))));
    check(stateWidget.url() == acceptedLocalUrl,
          QStringLiteral("allowedPaths rejects navigation outside the configured root"));

    stateWidget.setUrl(QUrl::fromLocalFile(firstDirectory));
    check(waitUntil([&]() {
              return !stateWidget.fileView()->flatModel()->isLoading();
          }),
          QStringLiteral("widget completes asynchronous directory navigation"));
    stateWidget.fileView()->selectNewFiles(
        {QStringLiteral("one.txt"), QStringLiteral("two.txt")});
    QSet<QString> selectedBefore;
    for (const QString &path : stateWidget.fileView()->selectedFiles())
        selectedBefore.insert(path);
    stateWidget.fileView()->setViewMode(QtFileMgrFileView::DetailsView);
    const QStringList selectedAfterList = stateWidget.fileView()->selectedFiles();
    QSet<QString> selectedAfter;
    for (const QString &path : selectedAfterList)
        selectedAfter.insert(path);
    check(selectedBefore.size() == 2 && selectedAfter == selectedBefore,
          QStringLiteral("view-mode changes preserve the current multi-selection"));

    bool emptySelectionEmitted = false;
    QObject::connect(stateWidget.fileView(), &QtFileMgrFileView::filesSelected,
                     [&](const QStringList &files) {
        if (files.isEmpty())
            emptySelectionEmitted = true;
    });
    const QList<QTableView *> tables = stateWidget.fileView()->findChildren<QTableView *>();
    if (!tables.isEmpty() && tables.first()->selectionModel())
        tables.first()->selectionModel()->clearSelection();
    check(emptySelectionEmitted,
          QStringLiteral("clearing the selection emits an empty filesSelected list"));

    const QString queuedSources = root.filePath(QStringLiteral("queued-sources"));
    const QString queuedDestination = root.filePath(QStringLiteral("queued-destination"));
    check(QDir().mkpath(queuedSources)
              && QDir().mkpath(queuedDestination)
              && writeFile(QDir(queuedSources).filePath(QStringLiteral("first.txt")),
                           QByteArrayLiteral("first"))
              && writeFile(QDir(queuedSources).filePath(QStringLiteral("second.txt")),
                           QByteArrayLiteral("second")),
          QStringLiteral("queued operation files prepared"));
    QtFileMgrConfig queueConfig = QtFileMgrConfig::defaultConfig();
    queueConfig.rootPath = queuedDestination;
    queueConfig.allowedPaths.clear();
    QtFileMgrWidget queueWidget(queueConfig);
    queueWidget.setUrl(QUrl::fromLocalFile(queuedDestination));
    auto submitCopy = [&](const QString &path) {
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(path)});
        QApplication::clipboard()->setMimeData(mime);
        queueWidget.fileView()->pasteFiles();
    };
    submitCopy(QDir(queuedSources).filePath(QStringLiteral("first.txt")));
    submitCopy(QDir(queuedSources).filePath(QStringLiteral("second.txt")));
    check(queueWidget.fileView()->pendingOperationCount() >= 1,
          QStringLiteral("a second file operation enters the queue"));
    {
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            if (!queueWidget.fileView()->hasActiveOperation()
                && queueWidget.fileView()->pendingOperationCount() == 0)
                loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        poll.start();
        loop.exec();
    }
    check(readFile(QDir(queuedDestination).filePath(QStringLiteral("first.txt")))
                  == QByteArrayLiteral("first")
              && readFile(QDir(queuedDestination).filePath(QStringLiteral("second.txt")))
                  == QByteArrayLiteral("second")
              && queueWidget.fileView()->canUndo(),
          QStringLiteral("queued copy operations complete in order"));
    queueWidget.fileView()->undoLastOperation();
    queueWidget.fileView()->undoLastOperation();
    check(!QFileInfo::exists(
              QDir(queuedDestination).filePath(QStringLiteral("first.txt")))
              && !QFileInfo::exists(
                  QDir(queuedDestination).filePath(QStringLiteral("second.txt"))),
          QStringLiteral("completed copy operations can be undone"));

    // R2-FV-P1-4: 覆盖复制的撤销不得删除操作前已存在的同名文件
    const QString overwriteSources = root.filePath(QStringLiteral("undo-overwrite-sources"));
    const QString overwriteDestinationDir =
        root.filePath(QStringLiteral("undo-overwrite-destination"));
    check(QDir().mkpath(overwriteSources)
              && QDir().mkpath(overwriteDestinationDir)
              && writeFile(QDir(overwriteSources).filePath(QStringLiteral("clash.txt")),
                           QByteArrayLiteral("clash-new"))
              && writeFile(QDir(overwriteSources).filePath(QStringLiteral("fresh.txt")),
                           QByteArrayLiteral("fresh"))
              && writeFile(QDir(overwriteDestinationDir).filePath(QStringLiteral("clash.txt")),
                           QByteArrayLiteral("clash-old")),
          QStringLiteral("overwrite undo files prepared"));
    QtFileMgrConfig overwriteConfig = QtFileMgrConfig::defaultConfig();
    overwriteConfig.rootPath = overwriteDestinationDir;
    overwriteConfig.allowedPaths.clear();
    QtFileMgrWidget overwriteWidget(overwriteConfig);
    overwriteWidget.setUrl(QUrl::fromLocalFile(overwriteDestinationDir));
    check(waitUntil([&]() {
        return !overwriteWidget.fileView()->flatModel()->isLoading()
            && overwriteWidget.fileView()->itemCount() == 1;
    }), QStringLiteral("overwrite destination loaded"));
    {
        auto *mime = new QMimeData;
        mime->setUrls({
            QUrl::fromLocalFile(QDir(overwriteSources).filePath(QStringLiteral("clash.txt"))),
            QUrl::fromLocalFile(QDir(overwriteSources).filePath(QStringLiteral("fresh.txt")))});
        QApplication::clipboard()->setMimeData(mime);
    }
    QTimer::singleShot(0, &overwriteWidget, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (QMessageBox *messageBox = qobject_cast<QMessageBox *>(widget)) {
                for (QAbstractButton *button : messageBox->buttons()) {
                    if (button->text() == QStringLiteral("覆盖全部")) {
                        button->click();
                        return;
                    }
                }
            }
        }
    });
    overwriteWidget.fileView()->pasteFiles();
    {
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            if (!overwriteWidget.fileView()->hasActiveOperation()
                && overwriteWidget.fileView()->pendingOperationCount() == 0)
                loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        poll.start();
        loop.exec();
    }
    check(readFile(QDir(overwriteDestinationDir).filePath(QStringLiteral("clash.txt")))
                  == QByteArrayLiteral("clash-new")
              && readFile(QDir(overwriteDestinationDir).filePath(QStringLiteral("fresh.txt")))
                     == QByteArrayLiteral("fresh")
              && overwriteWidget.fileView()->canUndo(),
          QStringLiteral("overwrite copy completes and keeps an undo record"));
    overwriteWidget.fileView()->undoLastOperation();
    check(!QFileInfo::exists(
              QDir(overwriteDestinationDir).filePath(QStringLiteral("fresh.txt")))
              && readFile(QDir(overwriteDestinationDir).filePath(QStringLiteral("clash.txt")))
                     == QByteArrayLiteral("clash-new"),
          QStringLiteral("undoing an overwrite copy does not delete the replaced destination"));

    // Recreating a removed destination does not prove it is the moved object.
    const QString moveRetrySourceDir = root.filePath(QStringLiteral("undo-move-sources"));
    const QString moveRetryDestDir = root.filePath(QStringLiteral("undo-move-destination"));
    const QString moveRetrySource =
        QDir(moveRetrySourceDir).filePath(QStringLiteral("retry.txt"));
    const QString moveRetryDestination =
        QDir(moveRetryDestDir).filePath(QStringLiteral("retry.txt"));
    check(QDir().mkpath(moveRetrySourceDir)
              && QDir().mkpath(moveRetryDestDir)
              && writeFile(moveRetrySource, QByteArrayLiteral("retry-data")),
          QStringLiteral("move retry files prepared"));
    QtFileMgrConfig moveRetryConfig = QtFileMgrConfig::defaultConfig();
    moveRetryConfig.rootPath = moveRetryDestDir;
    moveRetryConfig.allowedPaths.clear();
    QtFileMgrWidget moveRetryWidget(moveRetryConfig);
    moveRetryWidget.setUrl(QUrl::fromLocalFile(moveRetryDestDir));
    {
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(moveRetrySource)});
        mime->setData(QStringLiteral("application/x-kde-cutselection"),
                      QByteArrayLiteral("1"));
        QApplication::clipboard()->setMimeData(mime);
    }
    moveRetryWidget.fileView()->pasteFiles();
    {
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
            if (!moveRetryWidget.fileView()->hasActiveOperation()
                && moveRetryWidget.fileView()->pendingOperationCount() == 0)
                loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        poll.start();
        loop.exec();
    }
    check(!QFileInfo::exists(moveRetrySource)
              && readFile(moveRetryDestination) == QByteArrayLiteral("retry-data")
              && moveRetryWidget.fileView()->canUndo(),
          QStringLiteral("cut-paste move completes and keeps an undo record"));
    QFile::remove(moveRetryDestination);
    moveRetryWidget.fileView()->undoLastOperation();
    check(!QFileInfo::exists(moveRetrySource)
              && moveRetryWidget.fileView()->canUndo(),
          QStringLiteral("failed move undo keeps the record for retry"));
    check(writeFile(moveRetryDestination, QByteArrayLiteral("retry-data")),
          QStringLiteral("move retry destination restored"));
    moveRetryWidget.fileView()->undoLastOperation();
    check(!QFileInfo::exists(moveRetrySource)
              && readFile(moveRetryDestination) == QByteArrayLiteral("retry-data")
              && moveRetryWidget.fileView()->canUndo(),
          QStringLiteral("move undo refuses a recreated destination even when its contents match"));

    // R2-FV-P1-3: 过滤/排序引发的模型 reset 不清空选中
    const QString selectionDir = root.filePath(QStringLiteral("selection-preserve"));
    check(QDir().mkpath(selectionDir)
              && writeFile(QDir(selectionDir).filePath(QStringLiteral("alpha.txt")),
                           QByteArrayLiteral("a"))
              && writeFile(QDir(selectionDir).filePath(QStringLiteral("beta.txt")),
                           QByteArrayLiteral("b"))
              && writeFile(QDir(selectionDir).filePath(QStringLiteral("gamma.log")),
                           QByteArrayLiteral("g")),
          QStringLiteral("selection preserve files prepared"));
    QtFileMgrConfig selectionConfig = QtFileMgrConfig::defaultConfig();
    selectionConfig.rootPath = selectionDir;
    selectionConfig.allowedPaths.clear();
    QtFileMgrWidget selectionWidget(selectionConfig);
    selectionWidget.setUrl(QUrl::fromLocalFile(selectionDir));
    const bool selectionLoaded = waitUntil([&]() {
        return !selectionWidget.fileView()->flatModel()->isLoading()
            && selectionWidget.fileView()->itemCount() == 3;
    });
    selectionWidget.fileView()->selectNewFiles(
        {QStringLiteral("alpha.txt"), QStringLiteral("beta.txt")});
    check(selectionLoaded
              && selectionWidget.fileView()->selectedFiles().size() == 2,
          QStringLiteral("selection established"));
    selectionWidget.fileView()->setSortColumn(1, Qt::DescendingOrder);
    check(selectionWidget.fileView()->selectedFiles().size() == 2,
          QStringLiteral("sorting preserves the selection"));
    selectionWidget.fileView()->applyNameFilter(QStringLiteral("alpha"));
    const QStringList filteredSelection =
        selectionWidget.fileView()->selectedFiles();
    check(filteredSelection.size() == 1
              && filteredSelection.first().endsWith(QStringLiteral("alpha.txt")),
          QStringLiteral("filtering keeps the still-visible selection"));

    // UI-P1-3: 删除后选中下一项（reload 清空模型后按名称经 pending 落地）
    const QString deleteSelectDir = root.filePath(QStringLiteral("delete-select"));
    check(QDir().mkpath(deleteSelectDir)
              && writeFile(QDir(deleteSelectDir).filePath(QStringLiteral("a.txt")),
                           QByteArrayLiteral("a"))
              && writeFile(QDir(deleteSelectDir).filePath(QStringLiteral("b.txt")),
                           QByteArrayLiteral("b"))
              && writeFile(QDir(deleteSelectDir).filePath(QStringLiteral("c.txt")),
                           QByteArrayLiteral("c")),
          QStringLiteral("delete-select files prepared"));
    QtFileMgrConfig deleteSelectConfig = QtFileMgrConfig::defaultConfig();
    deleteSelectConfig.rootPath = deleteSelectDir;
    deleteSelectConfig.allowedPaths.clear();
    QtFileMgrWidget deleteSelectWidget(deleteSelectConfig);
    deleteSelectWidget.setUrl(QUrl::fromLocalFile(deleteSelectDir));
    const bool deleteSelectLoaded = waitUntil([&]() {
        return !deleteSelectWidget.fileView()->flatModel()->isLoading()
            && deleteSelectWidget.fileView()->itemCount() == 3;
    });
    deleteSelectWidget.fileView()->selectNewFiles({QStringLiteral("b.txt")});
    check(deleteSelectLoaded
              && deleteSelectWidget.fileView()->selectedFiles().size() == 1,
          QStringLiteral("delete-select target selected"));
    QTimer::singleShot(0, &deleteSelectWidget, []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (QMessageBox *messageBox = qobject_cast<QMessageBox *>(widget)) {
                if (QAbstractButton *yesButton =
                        messageBox->button(QMessageBox::Yes)) {
                    yesButton->click();
                    return;
                }
            }
        }
    });
    deleteSelectWidget.fileView()->deleteSelectedInternal(true);
    const bool nextSelected = waitUntil([&]() {
        return deleteSelectWidget.fileView()->selectedFiles()
                   == QStringList({QDir(deleteSelectDir).filePath(QStringLiteral("c.txt"))})
            && deleteSelectWidget.fileView()->itemCount() == 2;
    });
    check(nextSelected
              && !QFileInfo::exists(QDir(deleteSelectDir).filePath(QStringLiteral("b.txt"))),
          QStringLiteral("deleting selects the next item after reload"));

    // UI-P1-3: 新建文件夹后自动进入重命名编辑(编辑器为委托创建的 QLineEdit)
    deleteSelectWidget.fileView()->createNewFolder();
    const bool renameEditing = waitUntil([&]() {
        const QList<QLineEdit *> editors =
            deleteSelectWidget.fileView()->findChildren<QLineEdit *>();
        for (QLineEdit *editor : editors) {
            if (!editor->isHidden()
                && editor->text().startsWith(QStringLiteral("新建文件夹")))
                return true;
        }
        return false;
    });
    if (!renameEditing) {
        const QList<QLineEdit *> editors =
            deleteSelectWidget.fileView()->findChildren<QLineEdit *>();
        QStringList descriptions;
        for (QLineEdit *editor : editors)
            descriptions << QStringLiteral("text=\"%1\" hidden=%2")
                                .arg(editor->text()).arg(editor->isHidden());
        qWarning("[TEST] rename editor not found; editors=[%s], selected=%s",
                 qPrintable(descriptions.join(QStringLiteral("; "))),
                 qPrintable(deleteSelectWidget.fileView()->selectedFiles().join(
                     QStringLiteral(","))));
    }
        check(renameEditing
              && QDir(deleteSelectDir).exists(QStringLiteral("新建文件夹")),
          QStringLiteral("creating a folder selects it and opens rename editing"));

    // 审查#7: 确认框期间后端被切换,删除不得落在未确认的后端上
    {
        check(waitUntil([&]() {
            return !deleteSelectWidget.fileView()->flatModel()->isLoading()
                && deleteSelectWidget.fileView()->itemCount() >= 2;
        }), QStringLiteral("delete guard view loaded"));
        deleteSelectWidget.fileView()->selectNewFiles({QStringLiteral("a.txt")});
        OverwritableRemoteIO switchTarget;  // removePermanent 记录型
        QTimer::singleShot(0, &deleteSelectWidget, [&]() {
            deleteSelectWidget.fileView()->setIO(&switchTarget);
        });
        QTimer::singleShot(50, &deleteSelectWidget, []() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (QMessageBox *box = qobject_cast<QMessageBox *>(widget)) {
                    if (QAbstractButton *yes = box->button(QMessageBox::Yes)) {
                        yes->click();
                        return;
                    }
                }
            }
        });
        deleteSelectWidget.fileView()->deleteSelectedInternal(true);
        check(switchTarget.removedPaths.isEmpty()
                  && QFileInfo::exists(
                      QDir(deleteSelectDir).filePath(QStringLiteral("a.txt"))),
              QStringLiteral("backend switch during delete confirmation aborts the deletion"));
        deleteSelectWidget.fileView()->setIO(nullptr);  // 恢复本地后端
        // 后端切换会把 URL 重置到后端根,显式导航回测试目录
        deleteSelectWidget.fileView()->setUrl(QUrl::fromLocalFile(deleteSelectDir));
        check(waitUntil([&]() {
            return !deleteSelectWidget.fileView()->flatModel()->isLoading()
                && deleteSelectWidget.fileView()->itemCount() == 3;
        }), QStringLiteral("delete guard view restored"));
    }

    // R2-FV-P1-1/P1-2: 拖出携带 text/uri-list 与源后端标识
    {
        QtFileMgrFlatModel *model = deleteSelectWidget.fileView()->flatModel();
        QModelIndexList dragIndexes;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex idx = model->index(row, 0);
            if (idx.data(Qt::DisplayRole).toString() == QStringLiteral("a.txt"))
                dragIndexes.append(idx);
        }
        check(dragIndexes.size() == 1,
              QStringLiteral("drag source index located"));
        QMimeData *dragMime = model->mimeData(dragIndexes);
        const bool mimeHasUrls = dragMime && dragMime->hasUrls()
            && dragMime->urls().size() == 1
            && dragMime->urls().first().toLocalFile()
                   == QDir(deleteSelectDir).filePath(QStringLiteral("a.txt"));
        const QByteArray backendId = dragMime
            ? dragMime->data(QString::fromLatin1(
                  QtFileMgrFlatModel::backendDragMimeFormat()))
            : QByteArray();
        check(mimeHasUrls
                  && backendId == QtFileMgrFlatModel::backendDragId(&io),
              QStringLiteral("drag mime carries text/uri-list and backend identity"));
        DelayedRemoteIO foreignRemote;
        check(QtFileMgrFlatModel::backendDragId(&foreignRemote)
                  != QtFileMgrFlatModel::backendDragId(&io),
              QStringLiteral("backend drag identity distinguishes backends"));
        delete dragMime;
    }

    // ── 审查修复回归: 对话框与支撑模块 (批次 576098e1f 等) ──

    // DLG-P1-1: PathPicker 空 IO 拒绝;重复调用复用布局不泄漏不崩
    QtFileMgrPathPicker picker;
    check(picker.selectPath(nullptr, QString(), QStringLiteral("空后端")).isEmpty(),
          QStringLiteral("path picker rejects a null IO"));
    auto acceptPicker = []() {
        QTimer::singleShot(300, []() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QtFileMgrPathPicker *>(widget);
                if (!dialog)
                    continue;
                for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
                    if (button->text() == QStringLiteral("确定")) {
                        button->click();
                        return;
                    }
                }
            }
        });
    };
    acceptPicker();
    const QString pickedFirst =
        picker.selectPath(&io, deleteSelectDir, QStringLiteral("第一次"));
    acceptPicker();
    const QString pickedSecond =
        picker.selectPath(&io, deleteSelectDir, QStringLiteral("第二次"));
    check(QDir(pickedFirst).absolutePath() == QDir(deleteSelectDir).absolutePath()
              && QDir(pickedSecond).absolutePath() == QDir(deleteSelectDir).absolutePath(),
          QStringLiteral("path picker survives repeated calls and returns the start directory"));

    // DLG-P1-4: SettingsDialog 取消路径统一脏检查
    QtFileMgrSettingsDialog settingsDialog(QtFileMgrConfig::defaultConfig());
    QCheckBox *settingsCheck = settingsDialog.findChild<QCheckBox *>();
    check(settingsCheck != nullptr,
          QStringLiteral("settings dialog exposes options to dirty"));
    auto clickMessageBoxButton = [](QMessageBox::StandardButton standard) {
        QTimer::singleShot(0, [standard]() {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                    if (QAbstractButton *button = box->button(standard)) {
                        button->click();
                        return;
                    }
                }
            }
        });
    };
    settingsCheck->click();  // markDirty
    clickMessageBoxButton(QMessageBox::Discard);
    settingsDialog.reject();
    check(settingsDialog.result() == QDialog::Rejected,
          QStringLiteral("dirty settings dialog closes after discard"));
    settingsDialog.setResult(0);
    settingsCheck->click();  // 再次改脏
    clickMessageBoxButton(QMessageBox::Cancel);
    settingsDialog.reject();
    check(settingsDialog.result() == 0,
          QStringLiteral("dirty settings dialog stays open when the close is aborted"));

    // DLG-P1-2: displayRoot 前缀匹配必须有 '/' 边界。
    // 生产路径上 updateDisplayRootForUrl 会在导航出根时清除 displayRoot,
    // 这里钉住该行为:根内显示 placeName,导航到前缀兄弟目录后清除。
    const QString crumbRoot = root.filePath(QStringLiteral("crumbroot"));
    const QString crumbSibling = root.filePath(QStringLiteral("crumbroot2"));
    check(QDir().mkpath(crumbRoot) && QDir().mkpath(crumbSibling),
          QStringLiteral("breadcrumb sibling directories prepared"));
    QtFileMgrBreadcrumbBar breadcrumb;
    breadcrumb.setUrl(QUrl::fromLocalFile(crumbRoot));
    breadcrumb.setPlaceInfo(QStringLiteral("面包屑根"), crumbRoot);
    auto placeNameVisible = [&breadcrumb]() {
        for (QAbstractButton *button : breadcrumb.findChildren<QAbstractButton *>()) {
            if (button->text() == QStringLiteral("面包屑根"))
                return true;
        }
        for (QLabel *label : breadcrumb.findChildren<QLabel *>()) {
            if (label->text() == QStringLiteral("面包屑根"))
                return true;
        }
        return false;
    };
    check(placeNameVisible(),
          QStringLiteral("breadcrumb honors the display root inside it"));
    breadcrumb.setUrl(QUrl::fromLocalFile(crumbSibling));
    check(!placeNameVisible(),
          QStringLiteral("breadcrumb does not treat a prefix sibling as the display root"));

    // DLG-P1-3: 同步后端预览不再卡"正在加载"
    SynchronousPreviewIO syncPreviewIO;
    QtFileMgrPreviewPanel syncPreviewPanel;
    syncPreviewPanel.setIO(&syncPreviewIO);
    syncPreviewPanel.setFiles({QStringLiteral("/sync/preview.txt")});
    bool syncPreviewShown = false;
    if (QPlainTextEdit *textView = syncPreviewPanel.findChild<QPlainTextEdit *>())
        syncPreviewShown = textView->toPlainText()
            == QStringLiteral("sync-preview-text");
    check(syncPreviewShown,
          QStringLiteral("preview panel consumes a synchronously completed request"));

    // DLG-P1-6: 配置 JSON 解析失败必须告警
    g_jsonWarnings.clear();
    const QtMessageHandler previousHandler = qInstallMessageHandler(jsonCaptureHandler);
    const QString badConfigPath = root.filePath(QStringLiteral("bad-config.json"));
    check(writeFile(badConfigPath, QByteArrayLiteral("{ not json")),
          QStringLiteral("bad config prepared"));
    QtFileMgrConfig::loadFromFile(badConfigPath);
    qInstallMessageHandler(previousHandler);
    bool jsonWarningFound = false;
    for (const QString &warning : g_jsonWarnings) {
        if (warning.contains(QStringLiteral("JSON parse failed")))
            jsonWarningFound = true;
    }
    check(jsonWarningFound,
          QStringLiteral("invalid config JSON produces a parse warning"));

    // DLG-P1-8: Places 判重统一规范化路径
    QtFileMgrPlacesPanel dedupPanel;
    PlaceEntry dedupA;
    dedupA.name = QStringLiteral("规范化甲");
    dedupA.path = QDir::homePath();
    dedupA.iconTheme = QStringLiteral("folder");
    PlaceEntry dedupB;
    dedupB.name = QStringLiteral("规范化乙");
    dedupB.path = QStringLiteral("~");
    dedupB.iconTheme = QStringLiteral("folder");
    dedupPanel.setConfigPlaces({dedupA, dedupB});
    dedupPanel.buildModel();
    int dedupNameACount = 0;
    int dedupNameBCount = 0;
    if (QStandardItemModel *dedupModel =
            dedupPanel.findChild<QStandardItemModel *>()) {
        for (int row = 0; row < dedupModel->rowCount(); ++row) {
            QStandardItem *group = dedupModel->item(row);
            for (int child = 0; group && child < group->rowCount(); ++child) {
                QStandardItem *item = group->child(child);
                if (!item)
                    continue;
                if (item->text() == dedupA.name)
                    ++dedupNameACount;
                if (item->text() == dedupB.name)
                    ++dedupNameBCount;
            }
        }
    }
    check(dedupNameACount == 1 && dedupNameBCount == 0,
          QStringLiteral("places dedup treats ~ and the expanded home as duplicates"));

    // 历史污染迁移: 自定义位置中与内置默认重复者在 setConfigPlaces 时清除
    {
        const QList<PlaceEntry> savedMigrationCustoms =
            QtFileMgrConfigStore::loadCustomPlaces();
        PlaceEntry polluted;
        polluted.name = QStringLiteral("污染项");
        polluted.path = QDir::homePath();  // 与内置"主目录"重复
        polluted.iconTheme = QStringLiteral("folder");
        PlaceEntry legitCustom;
        legitCustom.name = QStringLiteral("真实自定义");
        legitCustom.path = QStringLiteral("/tmp");
        legitCustom.iconTheme = QStringLiteral("folder");
        QtFileMgrConfigStore::saveCustomPlaces({polluted, legitCustom});
        QtFileMgrPlacesPanel migrationPanel;
        migrationPanel.setConfigPlaces({});  // 空配置 → 内置默认集合
        const QList<PlaceEntry> afterMigration =
            QtFileMgrConfigStore::loadCustomPlaces();
        bool pollutionGone = true;
        bool legitKept = false;
        for (const PlaceEntry &entry : afterMigration) {
            if (entry.name == polluted.name)
                pollutionGone = false;
            if (entry.name == legitCustom.name)
                legitKept = true;
        }
        check(pollutionGone && legitKept,
              QStringLiteral("custom places migration drops built-in duplicates once"));
        QtFileMgrConfigStore::saveCustomPlaces(savedMigrationCustoms);
    }

    // DLG-P1-5: 重命名失败弹窗反馈
    FailingRenameIO failingRenameIO;
    QtFileMgrFlatModel failingRenameModel(&failingRenameIO);
    failingRenameModel.setUrl(deleteSelectDir);
    check(waitUntil([&]() {
        return !failingRenameModel.isLoading() && failingRenameModel.rowCount() >= 1;
    }), QStringLiteral("failing rename model loaded"));

    // R2-FV-P2-8 修正: 反斜杠仅 Windows 拒绝;POSIX 下是合法文件名字符
    check(QtFileMgrFlatModel::isValidDisplayName(QStringLiteral("a\\b"))
              && QtFileMgrFlatModel::isValidDisplayName(QStringLiteral("ab")),
          QStringLiteral("display name validation accepts backslash on POSIX"));
    check(!QtFileMgrFlatModel::isValidDisplayName(QStringLiteral("a/b"))
              && !QtFileMgrFlatModel::isValidDisplayName(QStringLiteral("..")),
          QStringLiteral("display name validation still rejects separators"));

    // R2-FV-P2-1/P2-3: 隐藏版本列排序列回落;模型层排序列 clamp
    {
        QtFileMgrFlatModel versionSortModel(&io);
        versionSortModel.setShowVersions(true);
        versionSortModel.setSortColumn(4, Qt::DescendingOrder);
        versionSortModel.setShowVersions(false);
        check(versionSortModel.sortColumn() == 0,
              QStringLiteral("hiding the versions column resets out-of-range sorting"));
        versionSortModel.setShowVersions(true);
        versionSortModel.setSortColumn(99, Qt::AscendingOrder);
        check(versionSortModel.sortColumn()
                  == versionSortModel.columnCount() - 1,
              QStringLiteral("model clamps an out-of-range sort column"));
    }
    QModelIndex failingRenameIndex;
    for (int row = 0; row < failingRenameModel.rowCount(); ++row) {
        if (failingRenameModel.index(row, 0).data(Qt::DisplayRole).toString()
            == QStringLiteral("a.txt")) {
            failingRenameIndex = failingRenameModel.index(row, 0);
            break;
        }
    }
    QtFileMgrItemDelegate renameDelegate;
    QLineEdit failingRenameEditor;
    failingRenameEditor.setText(QStringLiteral("renamed-a.txt"));
    // DLG-P1-5 异步化后: 失败经 operationFailed 信号而非模态框
    bool renameFailureNotified = false;
    QObject::connect(&renameDelegate, &QtFileMgrItemDelegate::operationFailed,
                     [&](const QString &message) {
        if (message.contains(QStringLiteral("renamed-a.txt")))
            renameFailureNotified = true;
    });
    renameDelegate.setModelData(&failingRenameEditor, &failingRenameModel,
                                failingRenameIndex);
    check(waitUntil([&]() { return renameFailureNotified; }, 3000)
              && QFileInfo::exists(QDir(deleteSelectDir).filePath(QStringLiteral("a.txt"))),
          QStringLiteral("rename failure notifies through the operationFailed signal"));

    // DLG-P1-5: 异步重命名成功路径, 模型延迟刷新后显示新名称
    {
        const QString renameOkSource =
            QDir(deleteSelectDir).filePath(QStringLiteral("rename-me.txt"));
        check(writeFile(renameOkSource, QByteArrayLiteral("rename-me")),
              QStringLiteral("rename success fixture prepared"));
        QtFileMgrFlatModel renameOkModel(&io);
        renameOkModel.setUrl(deleteSelectDir);
        check(waitUntil([&]() {
            return !renameOkModel.isLoading() && renameOkModel.rowCount() >= 1;
        }), QStringLiteral("rename success model loaded"));
        QModelIndex renameOkIndex;
        for (int row = 0; row < renameOkModel.rowCount(); ++row) {
            if (renameOkModel.index(row, 0).data(Qt::DisplayRole).toString()
                == QStringLiteral("rename-me.txt")) {
                renameOkIndex = renameOkModel.index(row, 0);
                break;
            }
        }
        QtFileMgrItemDelegate renameOkDelegate;
        QLineEdit renameOkEditor;
        renameOkEditor.setText(QStringLiteral("renamed-ok.txt"));
        renameOkDelegate.setModelData(&renameOkEditor, &renameOkModel,
                                      renameOkIndex);
        check(waitUntil([&]() {
            return !QFileInfo::exists(renameOkSource)
                && QFileInfo::exists(QDir(deleteSelectDir).filePath(
                    QStringLiteral("renamed-ok.txt")));
        }, 3000), QStringLiteral("async rename commits through renameAsync"));
        check(waitUntil([&]() {
            for (int row = 0; row < renameOkModel.rowCount(); ++row) {
                if (renameOkModel.index(row, 0).data(Qt::DisplayRole).toString()
                    == QStringLiteral("renamed-ok.txt"))
                    return true;
            }
            return false;
        }, 3000), QStringLiteral("model reloads and shows the renamed entry"));
    }

    // 跨后端粘贴: 本地文件粘贴到远端视图应经 copyRequest 上传;
    // 当前目录冲突扫描走模型快照,不再逐源 exists() RPC (UI-P1-4)
    {
        RecordingTransferRemoteIO uploadIO;
        QtFileMgrFileView uploadView(&uploadIO);
        uploadView.setUrl(QUrl(QStringLiteral("remote:///uploads")));
        check(waitUntil([&]() { return !uploadView.flatModel()->isLoading(); }),
              QStringLiteral("upload view finished initial listing"));
        const QString uploadSource = root.filePath(QStringLiteral("upload-me.txt"));
        const QString uploadSource2 = root.filePath(QStringLiteral("upload-me-2.txt"));
        check(writeFile(uploadSource, QByteArrayLiteral("upload-data"))
                  && writeFile(uploadSource2, QByteArrayLiteral("upload-data-2")),
              QStringLiteral("upload source prepared"));
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(uploadSource),
                       QUrl::fromLocalFile(uploadSource2)});
        QApplication::clipboard()->setMimeData(mime);
        uploadView.pasteFiles();
        if (!waitUntil([&]() { return uploadIO.copySources.size() >= 2; }, 3000)) {
            qWarning("[TEST] upload paste did not reach copyRequest (url=\"%s\")",
                     qPrintable(uploadView.url().toString()));
        }
        check(uploadIO.copySources.size() >= 2
                  && uploadIO.copySources.first() == uploadSource
                  && uploadIO.copyDestinations.first().endsWith(
                      QStringLiteral("upload-me.txt"))
                  && uploadIO.importSourcePaths.size() == 2
                  && uploadIO.importSourcePaths.contains(uploadSource2),
              QStringLiteral("pasting a local file into a remote view uploads through the explicit import API"));
        // 仅任务层每项一次 exists(2 项),FileView 冲突扫描不得再逐源调用
        if (uploadIO.existsCalls != 2) {
            qWarning("[TEST] existsCalls=%d (expected 2)", uploadIO.existsCalls);
        }
        check(uploadIO.existsCalls == 2,
              QStringLiteral("conflict scan for the current directory uses the model snapshot"));
    }

    // 跨后端粘贴: 远端项目粘贴到本地视图应经下载流落地
    {
        ExportingRemoteIO downloadSourceIO;
        QtFileMgrFileView remoteSourceView(&downloadSourceIO);
        remoteSourceView.setUrl(QUrl(QStringLiteral("remote:///fixture")));
        const QString downloadDir = root.filePath(QStringLiteral("downloads"));
        check(QDir().mkpath(downloadDir),
              QStringLiteral("download destination prepared"));
        QtFileMgrFileView downloadLocalView;
        downloadLocalView.setUrl(QUrl::fromLocalFile(downloadDir));
        auto *mime = new QMimeData;
        mime->setUrls({QUrl(QStringLiteral("remote:///fixture/report.txt"))});
        QApplication::clipboard()->setMimeData(mime);
        downloadLocalView.pasteFiles();
        check(waitUntil([&]() {
                  return readFile(QDir(downloadDir).filePath(QStringLiteral("report.txt")))
                             == QByteArrayLiteral("exported");
              }, 3000)
                  && downloadSourceIO.exportSources.first()
                         == QStringLiteral("fixture/report.txt"),
              QStringLiteral("pasting a remote item into a local view downloads it"));
    }

    // 跨后端粘贴: 两个远端节点之间经本地暂存中转
    {
        ExportingRemoteIO nodeA;
        nodeA.acceptedPrefix = QStringLiteral("/a");
        RecordingTransferRemoteIO nodeB;
        nodeB.acceptedPrefix = QStringLiteral("/b");
        nodeB.writableDir = QStringLiteral("b-dest");
        QtFileMgrFileView nodeAView(&nodeA);
        nodeAView.setUrl(QUrl(QStringLiteral("remote:///a")));
        QtFileMgrFileView nodeBView(&nodeB);
        nodeBView.setUrl(QUrl(QStringLiteral("remote:///b-dest")));
        auto *mime = new QMimeData;
        mime->setUrls({QUrl(QStringLiteral("remote:///a/data.bin"))});
        QApplication::clipboard()->setMimeData(mime);
        nodeBView.pasteFiles();
        // R2-04: 第二跳必须走显式上传 importFromLocalRequest, 而非 copyRequest
        check(waitUntil([&]() { return !nodeB.importSourcePaths.isEmpty(); }, 3000)
                  && nodeA.exportSources == QStringList({QStringLiteral("a/data.bin")})
                  && nodeB.importDestinationPaths.first()
                         == QStringLiteral("b-dest/data.bin"),
              QStringLiteral("pasting across remote nodes relays through local staging"));
        // 暂存副本必须清理: 上传完成后中转文件应被删除
        bool stagingClean = false;
        if (!nodeB.importSourcePaths.isEmpty()) {
            const QString stagingPath = nodeB.importSourcePaths.first();
            QEventLoop stagingLoop;
            QElapsedTimer stagingTimer;
            stagingTimer.start();
            QTimer stagingPoll;
            stagingPoll.setInterval(20);
            QObject::connect(&stagingPoll, &QTimer::timeout, &stagingLoop, [&]() {
                if (!QFileInfo::exists(stagingPath) || stagingTimer.elapsed() > 3000)
                    stagingLoop.quit();
            });
            stagingPoll.start();
            stagingLoop.exec();
            stagingClean = !QFileInfo::exists(stagingPath);
        }
        check(stagingClean,
              QStringLiteral("relay staging copy is cleaned after upload"));
    }

    // 审查#5: 占用竞争失败不得删除其他任务创建的目标
    {
        const QString raceSource = root.filePath(QStringLiteral("race-source.txt"));
        const QString raceDestination =
            root.filePath(QStringLiteral("race-destination.txt"));
        check(writeFile(raceSource, QByteArrayLiteral("ours")),
              QStringLiteral("race copy source prepared"));
        bool racePlanted = false;
        QString raceError;
        const QtFileMgrLocalIO::TransferStatus raceStatus =
            QtFileMgrLocalIO::copyCancellable(
                raceSource, raceDestination,
                [&]() {
                    if (!racePlanted) {
                        racePlanted = true;
                        // 模拟另一任务在窗口期创建目标
                        writeFile(raceDestination, QByteArrayLiteral("theirs"));
                    }
                    return false;  // 不取消,只是抢占
                },
                {}, &raceError);
        check(raceStatus == QtFileMgrLocalIO::TransferFailed
                  && readFile(raceDestination) == QByteArrayLiteral("theirs"),
              QStringLiteral("failed copy does not delete a destination created by a racing task"));
    }

    // 审查#4: 覆盖移动源清理失败时保留完整副本(不删 failedNew)
    {
        const QString rollbackSrcDir = root.filePath(QStringLiteral("rollback-src"));
        const QString rollbackDestDir = root.filePath(QStringLiteral("rollback-dest"));
        const QString rollbackSource =
            QDir(rollbackSrcDir).filePath(QStringLiteral("keep.txt"));
        const QString rollbackTarget =
            QDir(rollbackDestDir).filePath(QStringLiteral("keep.txt"));
        check(QDir().mkpath(rollbackSrcDir) && QDir().mkpath(rollbackDestDir)
                  && writeFile(rollbackSource, QByteArrayLiteral("new-content"))
                  && writeFile(rollbackTarget, QByteArrayLiteral("old-content"))
                  && QFile::setPermissions(rollbackSrcDir,
                                           QFileDevice::ReadOwner
                                               | QFileDevice::ExeOwner),
              QStringLiteral("rollback fixture prepared"));
        {
            QtFileMgrFileOperationJob rollbackJob(
                &io, QtFileMgrFileOperationJob::Move, {rollbackSource},
                rollbackDestDir, QtFileMgrFileOperationJob::OverwriteExisting);
            const JobOutcome rollbackOutcome = runJob(rollbackJob);
            check(rollbackOutcome.callbackCalled
                      && rollbackOutcome.failed == 1
                      && !rollbackOutcome.errors.isEmpty(),
                  QStringLiteral("overwrite move with unremovable source reports failure"));
        }
        QFile::setPermissions(rollbackSrcDir,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner);
        QString keptCopy;
        for (const QString &entry : QDir(rollbackDestDir).entryList(
                 QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
            if (entry.startsWith(QStringLiteral(".qtfilemgr-rollback-")))
                keptCopy = QDir(rollbackDestDir).filePath(entry);
        }
        check(readFile(rollbackTarget) == QByteArrayLiteral("old-content")
                  && !keptCopy.isEmpty()
                  && readFile(keptCopy) == QByteArrayLiteral("new-content")
                  && readFile(rollbackSource) == QByteArrayLiteral("new-content"),
              QStringLiteral("rollback keeps the complete copy when source removal fails"));
    }

    // 审查#1: 本机同名绝对路径不得让远端到远端复制误判为上传
    {
        const QString collisionPath =
            root.filePath(QStringLiteral("collision.txt"));
        check(writeFile(collisionPath, QByteArrayLiteral("local-content")),
              QStringLiteral("collision fixture prepared"));
        SshRemoteIO collisionIO;
        collisionIO.writableDir = root.path();
        QtFileMgrFileView collisionView(&collisionIO);
        collisionView.setUrl(QUrl(QStringLiteral("ssh://tester@example.invalid")
                                  + root.path()));
        check(waitUntil([&]() {
            return !collisionView.flatModel()->isLoading();
        }), QStringLiteral("collision view loaded"));
        auto *mime = new QMimeData;
        mime->setUrls({QUrl(QStringLiteral("ssh://tester@example.invalid")
                            + collisionPath)});
        QApplication::clipboard()->setMimeData(mime);
        collisionView.pasteFiles();
        check(waitUntil([&]() { return !collisionIO.copySources.isEmpty(); }, 3000)
                  && collisionIO.importSourcePaths.isEmpty()
                  && collisionIO.copySources.first() == collisionPath
                  && readFile(collisionPath) == QByteArrayLiteral("local-content"),
              QStringLiteral("remote-to-remote copy is not misclassified as an upload by a local same-path file"));
    }

    // R2-06: 默认 renameAsync 在后端销毁后安全失败,不调悬空对象
    {
        bool renameDeadCallback = false;
        bool renameDeadOk = true;
        QEventLoop renameDeadLoop;
        {
            auto *dyingIO = new QtFileMgrLocalIO;
            dyingIO->renameAsync(QStringLiteral("/tmp/qtfilemgr-dead-a"),
                                 QStringLiteral("/tmp/qtfilemgr-dead-b"),
                                 &renameDeadLoop,
                                 [&](bool ok, const QString &) {
                renameDeadCallback = true;
                renameDeadOk = ok;
                renameDeadLoop.quit();
            });
            delete dyingIO;  // 立即销毁,timer 下一拍才触发
        }
        QTimer::singleShot(2000, &renameDeadLoop, &QEventLoop::quit);
        renameDeadLoop.exec();
        check(renameDeadCallback && !renameDeadOk,
              QStringLiteral("renameAsync fails safely when the backend dies first"));
    }

    // R2-07: 后端销毁兜底不被忙检查拦截,模型不持悬空指针
    {
        auto *dyingBackend = new DelayedRemoteIO;
        QtFileMgrFileView orphanView(dyingBackend);
        orphanView.setUrl(QUrl(QStringLiteral("remote:///fixture")));
        QPointer<QtFileMgrIO> dyingGuard(dyingBackend);
        delete dyingBackend;  // 加载在途时销毁
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool fellBack = dyingGuard.isNull()
            && qobject_cast<QtFileMgrLocalIO *>(orphanView.io()) != nullptr
            && orphanView.flatModel()->io() == orphanView.io();
        // 兜底后视图可继续工作
        orphanView.setUrl(QUrl::fromLocalFile(deleteSelectDir));
        check(fellBack
                  && waitUntil([&]() {
                      return !orphanView.flatModel()->isLoading()
                          && orphanView.flatModel()->rowCount() >= 1;
                  }),
              QStringLiteral("backend destruction falls back and rebinds the model"));
    }

    // R2-03: 创建后被他人替换的目标不得被本任务清理
    {
        const QString replaceSource =
            root.filePath(QStringLiteral("replace-source.bin"));
        const QString replaceDestination =
            root.filePath(QStringLiteral("replace-dest.bin"));
        check(writeFile(replaceSource, QByteArray(4096, 'o')),
              QStringLiteral("replace fixture prepared"));
        int replaceChecks = 0;
        QString replaceError;
        const QtFileMgrLocalIO::TransferStatus replaceStatus =
            QtFileMgrLocalIO::copyCancellable(
                replaceSource, replaceDestination,
                [&]() {
                    ++replaceChecks;
                    if (replaceChecks == 2) {
                        // 模拟另一任务在本任务创建后替换目标节点
                        QFile::remove(replaceDestination);
                        writeFile(replaceDestination, QByteArrayLiteral("theirs"));
                        return true;  // 取消
                    }
                    return false;
                },
                {}, &replaceError);
        check(replaceStatus == QtFileMgrLocalIO::TransferCancelled
                  && readFile(replaceDestination) == QByteArrayLiteral("theirs"),
              QStringLiteral("cancel cleanup keeps a destination replaced by a racing task"));
    }

    // R2-09: 远端覆盖——传输失败旧目标完好;成功才备份式替换
    {
        FailingTransferRemoteIO failingOverwrite;
        failingOverwrite.occupiedPath = QStringLiteral("dest/src.txt");
        QtFileMgrFileOperationJob failingOverwriteJob(
            &failingOverwrite, QtFileMgrFileOperationJob::Copy,
            {QStringLiteral("local/src.txt")}, QStringLiteral("dest"),
            QtFileMgrFileOperationJob::OverwriteExisting);
        const JobOutcome failingOverwriteOutcome = runJob(failingOverwriteJob);
        bool originalRemoved = false;
        for (const QString &removed : failingOverwrite.removedPaths) {
            if (removed == QStringLiteral("dest/src.txt"))
                originalRemoved = true;
        }
        check(failingOverwriteOutcome.callbackCalled
                  && failingOverwriteOutcome.failed == 1
                  && !originalRemoved
                  && failingOverwrite.exists(QStringLiteral("dest/src.txt")),
              QStringLiteral("failed remote overwrite keeps the old destination intact"));
    }
    {
        OverwritableRemoteIO commitOverwrite;
        commitOverwrite.occupiedPath = QStringLiteral("dest/src.txt");
        QtFileMgrFileOperationJob commitJob(
            &commitOverwrite, QtFileMgrFileOperationJob::Copy,
            {QStringLiteral("local/src.txt")}, QStringLiteral("dest"),
            QtFileMgrFileOperationJob::OverwriteExisting);
        const JobOutcome commitOutcome = runJob(commitJob);
        bool committedRename = false;
        for (const QString &pair : commitOverwrite.renamedPairs) {
            if (pair.endsWith(QStringLiteral(">dest/src.txt")))
                committedRename = true;
        }
        if (!(commitOutcome.callbackCalled && commitOutcome.succeeded == 1
              && committedRename)) {
            qWarning("[TEST] commit overwrite debug: called=%d succeeded=%d failed=%d removed=%s renamed=%s newNames=%s errors=%s",
                     commitOutcome.callbackCalled ? 1 : 0,
                     commitOutcome.succeeded, commitOutcome.failed,
                     qPrintable(commitOverwrite.removedPaths.join(QStringLiteral(","))),
                     qPrintable(commitOverwrite.renamedPairs.join(QStringLiteral(","))),
                     qPrintable(commitOutcome.newNames.join(QStringLiteral(","))),
                     qPrintable(commitOutcome.errors.join(QStringLiteral(";"))));
        }
        check(commitOutcome.callbackCalled && commitOutcome.succeeded == 1
                  && !commitOverwrite.removedPaths.contains(
                      QStringLiteral("dest/src.txt"))
                  && commitOverwrite.renamedPairs.size() == 2
                  && committedRename
                  && commitOutcome.newNames
                         == QStringList({QStringLiteral("src.txt")}),
              QStringLiteral("remote overwrite commits via staging node and rename"));
    }

    // R2-13: 同拼写目录的本机剪切不得被同目录短路跳过
    {
        RecordingTransferRemoteIO importMoveIO;
        QtFileMgrFileOperationJob importMoveJob(
            &importMoveIO, QtFileMgrFileOperationJob::Move,
            {QStringLiteral("/tmp/qtfilemgr-import-data/a.txt")},
            QStringLiteral("/tmp/qtfilemgr-import-data"),
            QtFileMgrFileOperationJob::AutoRename);
        importMoveJob.setImportSources(
            {QStringLiteral("/tmp/qtfilemgr-import-data/a.txt")});
        const JobOutcome importMoveOutcome = runJob(importMoveJob);
        check(importMoveOutcome.callbackCalled
                  && importMoveOutcome.skipped == 0
                  && importMoveIO.importSourcePaths
                         == QStringList(
                             {QStringLiteral("/tmp/qtfilemgr-import-data/a.txt")}),
              QStringLiteral("import move is not skipped by a same-spelling directory shortcut"));
    }

    // 复审 P1-3 澄清钉住: import-move 不产生撤销记录(跨后端剪切不可逆)
    {
        RecordingTransferRemoteIO importUndoIO;
        QtFileMgrFileView importUndoView(&importUndoIO);
        importUndoView.setUrl(QUrl(QStringLiteral("remote:///uploads")));
        check(waitUntil([&]() {
            return !importUndoView.flatModel()->isLoading();
        }), QStringLiteral("import undo view loaded"));
        const QString importUndoSource =
            root.filePath(QStringLiteral("import-undo.txt"));
        check(writeFile(importUndoSource, QByteArrayLiteral("import-undo")),
              QStringLiteral("import undo fixture prepared"));
        auto *cutMime = new QMimeData;
        cutMime->setUrls({QUrl::fromLocalFile(importUndoSource)});
        cutMime->setData(QStringLiteral("application/x-kde-cutselection"),
                         QByteArrayLiteral("1"));
        QApplication::clipboard()->setMimeData(cutMime);
        importUndoView.pasteFiles();
        check(waitUntil([&]() {
            return !importUndoIO.importSourcePaths.isEmpty();
        }, 3000), QStringLiteral("import move ran through the import API"));
        check(waitUntil([&]() {
            return !importUndoView.hasActiveOperation()
                && importUndoView.pendingOperationCount() == 0;
        }, 3000) && !importUndoView.canUndo(),
              QStringLiteral("import move produces no undo record"));
    }

    // R2-16: 异步重命名完成后选中新名称(经 pending 等 reload 落地)
    {
        SlowRenameRemoteIO slowRenameIO;
        slowRenameIO.writableDir = QStringLiteral("slowrename");
        QtFileMgrFileView slowRenameView(&slowRenameIO);
        slowRenameView.setUrl(QUrl(QStringLiteral("remote:///slowrename")));
        check(waitUntil([&]() {
            return !slowRenameView.flatModel()->isLoading()
                && slowRenameView.flatModel()->rowCount() == 1;
        }), QStringLiteral("slow rename view loaded"));
        slowRenameView.selectNewFiles({QStringLiteral("old.txt")});
        check(slowRenameView.selectedFiles()
                  == QStringList({QStringLiteral("slowrename/old.txt")}),
              QStringLiteral("slow rename target selected"));
        slowRenameView.renameCurrent();
        QLineEdit *slowEditor = nullptr;
        check(waitUntil([&]() {
            for (QLineEdit *candidate :
                 slowRenameView.findChildren<QLineEdit *>()) {
                if (!candidate->isHidden()) {
                    slowEditor = candidate;
                    return true;
                }
            }
            return false;
        }), QStringLiteral("slow rename editor opened"));
        slowEditor->setText(QStringLiteral("new.txt"));
        QKeyEvent commitKey(QEvent::KeyPress, Qt::Key_Return,
                            Qt::NoModifier);
        QApplication::sendEvent(slowEditor, &commitKey);
        check(waitUntil([&]() {
            return slowRenameView.selectedFiles()
                       == QStringList({QStringLiteral("slowrename/new.txt")});
        }, 4000) && slowRenameIO.renameAsyncCalled,
              QStringLiteral("async rename restores selection to the new name"));
    }

    // R2-02: 多个远端来源组串行执行,不并发竞争
    {
        ExportingRemoteIO nodeC;
        nodeC.acceptedPrefix = QStringLiteral("/c/");
        QtFileMgrFileView nodeCView(&nodeC);
        nodeCView.setUrl(QUrl(QStringLiteral("remote:///c")));
        // 复用前面 relay 测试的 nodeA/nodeB(仍在作用域内? 不在——重建)
        ExportingRemoteIO multiA;
        multiA.acceptedPrefix = QStringLiteral("/ma/");
        RecordingTransferRemoteIO multiB;
        multiB.acceptedPrefix = QStringLiteral("/mb");
        multiB.writableDir = QStringLiteral("mb-dest");
        QtFileMgrFileView multiAView(&multiA);
        multiAView.setUrl(QUrl(QStringLiteral("remote:///ma")));
        QtFileMgrFileView multiBView(&multiB);
        multiBView.setUrl(QUrl(QStringLiteral("remote:///mb-dest")));
        auto *multiMime = new QMimeData;
        multiMime->setUrls({QUrl(QStringLiteral("remote:///ma/same.bin")),
                            QUrl(QStringLiteral("remote:///c/same.bin"))});
        QApplication::clipboard()->setMimeData(multiMime);
        multiBView.pasteFiles();
        // 粘贴返回时两组均未上传: 第二组被延后 (R2-02)
        check(multiB.importSourcePaths.isEmpty(),
              QStringLiteral("second serial transfer group defers while the first runs"));
        const bool bothDone = waitUntil([&]() {
            return multiB.importSourcePaths.size() >= 2;
        }, 5000);
        if (!bothDone) {
            qWarning("[TEST] serial groups debug: importB=%d exportA=%d exportC=%d",
                     multiB.importSourcePaths.size(),
                     multiA.exportSources.size(), nodeC.exportSources.size());
        }
        check(bothDone,
              QStringLiteral("both serial transfer groups complete in sequence"));
    }

    // 审查#6: files/info 子目录是链接时枚举与清空都拒绝整个根
    {
        const QString outsideDir = root.filePath(QStringLiteral("trash-outside"));        const QString linkedTrashRoot = root.filePath(QStringLiteral("trash-linked"));
        check(QDir().mkpath(outsideDir)
                  && QDir().mkpath(linkedTrashRoot)
                  && writeFile(QDir(outsideDir).filePath(QStringLiteral("victim.txt")),
                               QByteArrayLiteral("victim"))
                  && QFile::link(outsideDir,
                                 QDir(linkedTrashRoot).filePath(QStringLiteral("files"))),
              QStringLiteral("symlinked trash root prepared"));
        const QList<QtFileMgrTrashEntry> linkedEntries =
            QtFileMgrTrashDialog::entriesForRoots({linkedTrashRoot});
        QString linkedEmptyError;
        QtFileMgrTrashDialog::emptyTrashRoots({linkedTrashRoot},
                                              &linkedEmptyError);
        check(linkedEntries.isEmpty()
                  && readFile(QDir(outsideDir).filePath(QStringLiteral("victim.txt")))
                         == QByteArrayLiteral("victim"),
              QStringLiteral("trash with symlinked subdirectory is rejected and outside files survive"));
    }

    // R2-12: payload 删除失败保留其元数据; 任一子目录链接则整个根不动
    {
        const QString pairRoot = root.filePath(QStringLiteral("trash-pair"));
        const QString pairFiles = QDir(pairRoot).filePath(QStringLiteral("files"));
        const QString pairInfo = QDir(pairRoot).filePath(QStringLiteral("info"));
        const QString lockedPayloadDir =
            QDir(pairFiles).filePath(QStringLiteral("locked-item"));
        check(QDir().mkpath(pairFiles) && QDir().mkpath(pairInfo)
                  && QDir().mkpath(lockedPayloadDir)
                  && writeFile(QDir(lockedPayloadDir).filePath(QStringLiteral("inner.txt")),
                               QByteArrayLiteral("payload"))
                  && writeFile(QDir(pairFiles).filePath(QStringLiteral("free-item")),
                               QByteArrayLiteral("free"))
                  && writeFile(QDir(pairInfo).filePath(QStringLiteral("locked-item.trashinfo")),
                               QByteArrayLiteral("[Trash Info]\nPath=/x\n"))
                  && writeFile(QDir(pairInfo).filePath(QStringLiteral("free-item.trashinfo")),
                               QByteArrayLiteral("[Trash Info]\nPath=/y\n")),
              QStringLiteral("paired trash fixture prepared"));
        // 只锁定 locked-item 子目录: free-item 仍可删除
        QFile::setPermissions(lockedPayloadDir,
                              QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        QString pairError;
        const bool pairResult =
            QtFileMgrTrashDialog::emptyTrashRoots({pairRoot}, &pairError);
        QFile::setPermissions(lockedPayloadDir,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner);
        check(!pairResult
                  && QFileInfo::exists(QDir(pairFiles).filePath(QStringLiteral("locked-item")))
                  && QFileInfo::exists(QDir(pairInfo).filePath(QStringLiteral("locked-item.trashinfo")))
                  && !QFileInfo::exists(QDir(pairFiles).filePath(QStringLiteral("free-item")))
                  && !QFileInfo::exists(QDir(pairInfo).filePath(QStringLiteral("free-item.trashinfo"))),
              QStringLiteral("failed payload removal keeps its metadata"));
    }
    {
        const QString linkRoot = root.filePath(QStringLiteral("trash-rootlink"));
        const QString outsideB = root.filePath(QStringLiteral("trash-outside-b"));
        check(QDir().mkpath(linkRoot) && QDir().mkpath(outsideB)
                  && writeFile(QDir(outsideB).filePath(QStringLiteral("victim.txt")),
                               QByteArrayLiteral("v"))
                  && QDir().mkpath(QDir(linkRoot).filePath(QStringLiteral("info")))
                  && writeFile(QDir(linkRoot).filePath(QStringLiteral("info/a.trashinfo")),
                               QByteArrayLiteral("[Trash Info]\nPath=/z\n"))
                  && QFile::link(outsideB,
                                 QDir(linkRoot).filePath(QStringLiteral("files"))),
              QStringLiteral("symlinked child trash fixture prepared"));
        QString linkError;
        QtFileMgrTrashDialog::emptyTrashRoots({linkRoot}, &linkError);
        check(readFile(QDir(outsideB).filePath(QStringLiteral("victim.txt")))
                      == QByteArrayLiteral("v")
                  && QFileInfo::exists(
                      QDir(linkRoot).filePath(QStringLiteral("info/a.trashinfo"))),
              QStringLiteral("symlinked child leaves the whole trash root untouched"));
    }

    // 审查#19: allowedPaths 含 "/" 放行全部本地子路径
    {
        QtFileMgrConfig rootCfg = QtFileMgrConfig::defaultConfig();
        rootCfg.rootPath = QStringLiteral("/");
        rootCfg.allowedPaths = QStringList{QStringLiteral("/")};
        QtFileMgrWidget rootWidget(rootCfg);
        rootWidget.setUrl(QUrl::fromLocalFile(QStringLiteral("/tmp")));
        check(rootWidget.url() == QUrl::fromLocalFile(QStringLiteral("/tmp")),
              QStringLiteral("allowedPaths root permits local descendants"));
    }

    // 审查#17: 不可读目录复制报错而非静默当空目录
    {
        const QString unreadableSrc = root.filePath(QStringLiteral("unreadable-src"));
        const QString unreadableDest =
            root.filePath(QStringLiteral("unreadable-dest"));
        const QString unreadableSub =
            QDir(unreadableSrc).filePath(QStringLiteral("sub"));
        check(QDir().mkpath(unreadableSub)
                  && writeFile(QDir(unreadableSub).filePath(QStringLiteral("secret.txt")),
                               QByteArrayLiteral("x"))
                  && QFile::setPermissions(unreadableSub, QFileDevice::ExeOwner),
              QStringLiteral("unreadable fixture prepared"));
        QString unreadableError;
        const QtFileMgrLocalIO::TransferStatus unreadableStatus =
            QtFileMgrLocalIO::copyCancellable(unreadableSrc, unreadableDest,
                                              {}, {}, &unreadableError);
        QFile::setPermissions(unreadableSub,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner);
        check(unreadableStatus == QtFileMgrLocalIO::TransferFailed
                  && !unreadableError.isEmpty()
                  && !QFileInfo::exists(unreadableDest),
              QStringLiteral("unreadable directory fails loudly instead of copying as empty"));
    }

    // 审查#20: 仅名称列可编辑
    check(failingRenameModel.flags(failingRenameModel.index(0, 0))
                  .testFlag(Qt::ItemIsEditable)
              && !failingRenameModel.flags(failingRenameModel.index(0, 1))
                      .testFlag(Qt::ItemIsEditable),
          QStringLiteral("only the name column is editable"));

    // 审查#9: 双栏传输入口也受配置权限约束
    {
        QtFileMgrConfig denyCfg = QtFileMgrConfig::defaultConfig();
        denyCfg.rootPath = deleteSelectDir;
        denyCfg.allowedPaths.clear();
        denyCfg.allowCreate = false;
        QtFileMgrWidget denyWidget(denyCfg);
        denyWidget.setUrl(QUrl::fromLocalFile(deleteSelectDir));
        check(waitUntil([&]() {
            return !denyWidget.fileView()->flatModel()->isLoading();
        }), QStringLiteral("deny widget loaded"));
        denyWidget.fileView()->selectNewFiles({QStringLiteral("a.txt")});
        check(!denyWidget.fileView()->transferSelectedTo(
                  root.filePath(QStringLiteral("anywhere")), false),
              QStringLiteral("permission config gates the dual-pane transfer entry"));
    }

    QtFileMgrSearchDialog searchDialog(
        &io, stateRoot);
    QDialogButtonBox *searchButtons =
        searchDialog.findChild<QDialogButtonBox *>();
    check(searchButtons
              && searchButtons->button(QDialogButtonBox::Close)->text()
                     == QStringLiteral("关闭"),
          QStringLiteral("search dialog uses a localized close command"));
    QTreeWidget *searchResults = searchDialog.findChild<QTreeWidget *>();
    searchDialog.setSearchCriteria(
        QStringLiteral("one.txt"), QtFileMgrSearchDialog::AnyType);
    searchDialog.startSearch();
    waitUntil([&]() { return !searchDialog.isSearching(); }, 5000);
    check(searchResults && searchResults->topLevelItemCount() == 1,
          QStringLiteral("recursive search returns the matching file"));

    // 审查#14: 同步完成后端不再让搜索永久停在搜索中
    {
        SynchronousListingRemoteIO syncSearchIO;
        QtFileMgrSearchDialog syncSearch(&syncSearchIO, QStringLiteral("/"));
        syncSearch.setSearchCriteria(QStringLiteral("synchronous"),
                                     QtFileMgrSearchDialog::AnyType);
        syncSearch.startSearch();
        const bool syncSearchSettled =
            waitUntil([&]() { return !syncSearch.isSearching(); }, 5000);
        QTreeWidget *syncResults = syncSearch.findChild<QTreeWidget *>();
        check(syncSearchSettled && syncResults
                  && syncResults->topLevelItemCount() == 1,
              QStringLiteral("search consumes a synchronously completed directory request"));
    }

    const QString searchSnapshotRoot =
        root.filePath(QStringLiteral("search-snapshot"));
    QDir().mkpath(searchSnapshotRoot);
    writeFile(QDir(searchSnapshotRoot).filePath(QStringLiteral("alpha.txt")),
              QByteArrayLiteral("alpha"));
    writeFile(QDir(searchSnapshotRoot).filePath(QStringLiteral("beta.txt")),
              QByteArrayLiteral("beta"));
    QtFileMgrSearchDialog snapshotSearch(&io, searchSnapshotRoot);
    QTreeWidget *snapshotResults = snapshotSearch.findChild<QTreeWidget *>();
    snapshotSearch.setSearchCriteria(
        QStringLiteral("alpha"), QtFileMgrSearchDialog::AnyType);
    snapshotSearch.startSearch();
    snapshotSearch.setSearchCriteria(
        QStringLiteral("beta"), QtFileMgrSearchDialog::AnyType);
    waitUntil([&]() { return !snapshotSearch.isSearching(); }, 5000);
    check(snapshotResults && snapshotResults->topLevelItemCount() == 1
              && snapshotResults->topLevelItem(0)->text(0)
                     == QStringLiteral("alpha.txt"),
          QStringLiteral("an active search uses an immutable query snapshot"));

    const QString filteredSearchRoot =
        root.filePath(QStringLiteral("search-filters"));
    QDir().mkpath(filteredSearchRoot);
    const QString smallSearchFile =
        QDir(filteredSearchRoot).filePath(QStringLiteral("small.txt"));
    const QString largeSearchFile =
        QDir(filteredSearchRoot).filePath(QStringLiteral("large.txt"));
    writeFile(smallSearchFile, QByteArrayLiteral("small"));
    writeFile(largeSearchFile, QByteArray(2 * 1024 * 1024, 'L'));
    QtFileMgrSearchDialog filteredSearch(&io, filteredSearchRoot);
    filteredSearch.setSearchCriteria(
        QString(), QtFileMgrSearchDialog::Documents,
        1024 * 1024, -1, QDateTime(), QDateTime(), false);
    filteredSearch.startSearch();
    check(waitUntil([&]() { return !filteredSearch.isSearching(); }, 5000)
              && filteredSearch.resultPaths() == QStringList{largeSearchFile},
          QStringLiteral("search filters results by MIME category and size"));
    filteredSearch.setSearchCriteria(
        QStringLiteral(".txt"), QtFileMgrSearchDialog::FilesOnly,
        -1, -1, QDateTime(), QDateTime(), false);
    filteredSearch.startSearch();
    waitUntil([&]() { return !filteredSearch.isSearching(); }, 5000);
    QTreeWidget *filteredResults =
        filteredSearch.findChild<QTreeWidget *>(QStringLiteral("searchResults"));
    if (filteredResults)
        filteredResults->sortItems(3, Qt::AscendingOrder);
    check(filteredResults && filteredResults->topLevelItemCount() == 2
              && filteredResults->topLevelItem(0)->text(0)
                     == QStringLiteral("small.txt")
              && filteredSearch.searchHistory().contains(QStringLiteral(".txt")),
          QStringLiteral("search keeps history and sorts file sizes numerically"));

    DelayedListingIO cancellableSearchIO;
    QtFileMgrSearchDialog cancellableSearch(
        &cancellableSearchIO, QStringLiteral("slow"));
    bool searchCancelled = false;
    QObject::connect(
        &cancellableSearch, &QtFileMgrSearchDialog::searchFinished,
        [&](bool cancelled, int) { searchCancelled = cancelled; });
    cancellableSearch.setSearchCriteria(
        QStringLiteral("slow"), QtFileMgrSearchDialog::AnyType);
    QElapsedTimer searchStartTimer;
    searchStartTimer.start();
    cancellableSearch.startSearch();
    const bool asynchronousSearchStart =
        searchStartTimer.elapsed() < 100 && cancellableSearch.isSearching();
    waitUntil([&]() { return cancellableSearchIO.listRequests > 0; });
    cancellableSearch.stopSearch();
    check(asynchronousSearchStart && searchCancelled
              && !cancellableSearch.isSearching()
              && cancellableSearch.resultCount() == 0,
          QStringLiteral("search starts asynchronously and cancels an active listing"));

    QtFileMgrFileView messageView(&io);
    QTimer *messageTimer =
        messageView.findChild<QTimer *>(QStringLiteral("fileViewMessageHideTimer"));
    QFrame *messageBar =
        messageView.findChild<QFrame *>(QStringLiteral("fileViewMessageBar"));
    messageView.showMessage(QStringLiteral("temporary"), QStringLiteral("info"));
    const bool temporaryMessageScheduled = messageTimer && messageTimer->isActive();
    messageView.showMessage(QStringLiteral("persistent"), QStringLiteral("error"));
    check(temporaryMessageScheduled && messageTimer && !messageTimer->isActive()
              && messageBar && messageBar->isVisibleTo(&messageView),
          QStringLiteral("a stale message timeout cannot hide a newer error"));

    const QString pendingSource =
        root.filePath(QStringLiteral("pending-selection-source"));
    const QString pendingOther =
        root.filePath(QStringLiteral("pending-selection-other"));
    QDir().mkpath(pendingSource);
    QDir().mkpath(pendingOther);
    writeFile(QDir(pendingOther).filePath(QStringLiteral("late.txt")),
              QByteArrayLiteral("other"));
    QtFileMgrFileView pendingView(&io);
    pendingView.setUrl(QUrl::fromLocalFile(pendingSource));
    pendingView.observeCreatedFile(QStringLiteral("late.txt"));
    pendingView.setUrl(QUrl::fromLocalFile(pendingOther));
    waitUntil([&]() {
        return !pendingView.selectedFiles().isEmpty();
    }, 300);
    check(pendingView.selectedFiles().isEmpty(),
          QStringLiteral("pending created-file selection does not cross directories"));

    QtFileMgrWorkspace workspace(nullptr, false);
    workspace.addTab(QUrl::fromLocalFile(stateRoot));
    QTabWidget *workspaceTabs = workspace.findChild<QTabWidget *>();
    check(workspace.tabCount() == 2 && workspaceTabs
              && workspaceTabs->tabsClosable()
              && workspaceTabs->cornerWidget(Qt::TopRightCorner)
              && workspaceTabs->cornerWidget(Qt::TopRightCorner)->objectName()
                     == QStringLiteral("qtFileMgrWorkspaceTools"),
          QStringLiteral("workspace integrates tools into the closable multi-tab row"));
    workspace.splitCurrentTab();
    check(workspace.findChildren<QtFileMgrWidget *>().size() == 3
              && workspace.paneCount() == 2,
          QStringLiteral("workspace creates a second pane in the current tab"));

    // 共享 Places 面板: 全局一份,内嵌面板隐藏,激活路由到当前活动面板
    {
        QtFileMgrWorkspace sharedPanelWorkspace(nullptr, false);
        QtFileMgrPlacesPanel *sharedPanel = sharedPanelWorkspace.placesPanel();
        bool allShared = sharedPanel != nullptr;
        bool embeddedHidden = true;
        for (QtFileMgrWidget *manager :
             sharedPanelWorkspace.findChildren<QtFileMgrWidget *>()) {
            if (manager->activePlacesPanel() != sharedPanel)
                allShared = false;
            QWidget *embedded = manager->placesPanel();
            QWidget *wrapper = embedded ? embedded->parentWidget() : nullptr;
            // 内嵌面板经 leftPane 包装层整体隐藏; isHidden 不向子控件传播
            const bool hidden = (embedded && embedded->isHidden())
                || (wrapper && wrapper->isHidden());
            if (embedded != sharedPanel && !hidden)
                embeddedHidden = false;
        }
        check(allShared && embeddedHidden,
              QStringLiteral("workspace panes share one places panel and hide embedded ones"));
        const QString sharedTarget = root.filePath(QStringLiteral("shared-target"));
        check(QDir().mkpath(sharedTarget),
              QStringLiteral("shared panel target prepared"));
        emit sharedPanel->placeActivated(sharedTarget);
        check(sharedPanelWorkspace.currentFileManager()
                  && sharedPanelWorkspace.currentFileManager()->url()
                         == QUrl::fromLocalFile(sharedTarget),
              QStringLiteral("shared panel activation drives the active pane"));
    }

    const QJsonObject workspaceState = workspace.sessionState();
    QtFileMgrWorkspace restoredWorkspace(nullptr, false);
    check(restoredWorkspace.restoreSessionState(workspaceState)
              && restoredWorkspace.tabCount() == 2
              && restoredWorkspace.paneCount() == 2,
          QStringLiteral("workspace session restores tabs and split panes"));
    restoredWorkspace.closeCurrentSplit();
    waitUntil([&]() { return restoredWorkspace.paneCount() == 1; });
    restoredWorkspace.closeCurrentTab();
    QTabWidget *restoredTabs = restoredWorkspace.findChild<QTabWidget *>();
    check(restoredWorkspace.tabCount() == 1
              && restoredTabs && !restoredTabs->tabsClosable(),
          QStringLiteral("workspace closes the active tab and split"));
    restoredWorkspace.restoreClosedTab();
    check(restoredWorkspace.tabCount() == 2,
          QStringLiteral("workspace restores the most recently closed tab"));

    const QString crossPaneSource =
        root.filePath(QStringLiteral("workspace-cross-pane-source"));
    const QString crossPaneDestination =
        root.filePath(QStringLiteral("workspace-cross-pane-destination"));
    QDir().mkpath(crossPaneSource);
    QDir().mkpath(crossPaneDestination);
    writeFile(QDir(crossPaneSource).filePath(QStringLiteral("cross-pane.txt")),
              QByteArrayLiteral("cross-pane"));
    QtFileMgrWorkspace transferWorkspace(nullptr, false);
    transferWorkspace.splitCurrentTab();
    const QList<QtFileMgrWidget *> transferManagers =
        transferWorkspace.findChildren<QtFileMgrWidget *>();
    bool activePaneReady = false;
    bool crossPaneSelectionReady = false;
    if (transferManagers.size() == 2) {
        QtFileMgrWidget *sourceManager =
            transferWorkspace.currentFileManager();
        QtFileMgrWidget *destinationManager =
            transferManagers.at(0) == sourceManager
                ? transferManagers.at(1) : transferManagers.at(0);
        destinationManager->setUrl(
            QUrl::fromLocalFile(crossPaneDestination));
        sourceManager->setUrl(QUrl::fromLocalFile(crossPaneSource));
        sourceManager->fileView()->selectNewFiles(
            {QStringLiteral("cross-pane.txt")});
        waitUntil([&]() {
            return !sourceManager->fileView()->flatModel()->isLoading()
                && !destinationManager->fileView()->flatModel()->isLoading()
                && sourceManager->fileView()->selectedFiles().size() == 1;
        }, 5000);
        activePaneReady =
            transferWorkspace.currentFileManager() == sourceManager;
        crossPaneSelectionReady =
            sourceManager->fileView()->selectedFiles().size() == 1;
        transferWorkspace.copyToOtherPane();
    }
    check(activePaneReady,
          QStringLiteral("workspace tracks the active split pane"));
    check(crossPaneSelectionReady,
          QStringLiteral("workspace preserves the active pane selection"));
    check(waitUntil([&]() {
              return QFileInfo::exists(
                  QDir(crossPaneDestination).filePath(
                      QStringLiteral("cross-pane.txt")));
          }, 5000),
          QStringLiteral("workspace copies selected files to the other pane"));

    QtFileMgrStorageDeviceProvider devices;
    bool validDeviceEntries = true;
    QSet<QString> defaultDeviceIds;
    const QList<QtFileMgrDeviceEntry> defaultDevices = devices.devices();
    for (const QtFileMgrDeviceEntry &device : defaultDevices) {
        validDeviceEntries = validDeviceEntries
            && !device.id.isEmpty() && !device.path.isEmpty() && device.mounted
            && QFileInfo(device.path).isDir()
            && !defaultDeviceIds.contains(device.id);
        defaultDeviceIds.insert(device.id);
    }
    check(validDeviceEntries
              && defaultDeviceIds.size() == defaultDevices.size(),
          QStringLiteral("default device provider exposes valid unique mounted volumes"));

    QtFileMgrPlacesPanel providerPanel;
    providerPanel.setDeviceRefreshMs(1);
    auto *ephemeralProvider = new EmptyDeviceProvider;
    providerPanel.setDeviceProvider(ephemeralProvider);
    delete ephemeralProvider;
    QEventLoop providerLoop;
    QTimer::singleShot(10, &providerLoop, &QEventLoop::quit);
    providerLoop.exec();
    check(true, QStringLiteral("device refresh tolerates a destroyed injected provider"));

    AsyncDeviceProvider asyncDevices(root.filePath(
        QStringLiteral("mounted-test-device")));
    providerPanel.setDeviceProvider(&asyncDevices);
    QString activatedDevicePath;
    bool deviceOperationFinished = false;
    QObject::connect(&providerPanel, &QtFileMgrPlacesPanel::placeActivated,
                     [&](const QString &path) {
        activatedDevicePath = path;
    });
    QObject::connect(
        &providerPanel, &QtFileMgrPlacesPanel::deviceOperationFinished,
        [&](const QString &, bool, bool, const QString &) {
            deviceOperationFinished = true;
        });
    const bool mountAccepted = providerPanel.mountDevice(
        QStringLiteral("test-device"), true);
    const bool duplicateMountRejected = !providerPanel.mountDevice(
        QStringLiteral("test-device"), true);
    check(mountAccepted && duplicateMountRejected
              && waitUntil([&]() {
                  return deviceOperationFinished && asyncDevices.mounted;
              })
              && asyncDevices.mountRequests == 1
              && activatedDevicePath == asyncDevices.path,
          QStringLiteral("places panel mounts once and opens the mounted device"));
    deviceOperationFinished = false;
    check(providerPanel.unmountDevice(QStringLiteral("test-device"))
              && waitUntil([&]() {
                  return deviceOperationFinished && !asyncDevices.mounted;
              })
              && asyncDevices.unmountRequests == 1,
          QStringLiteral("places panel completes asynchronous device unmount"));

    // DLG-P1-7: 挂载成功但列表未刷新时等 devicesChanged,不误报失败
    DeferredListMountProvider deferredDevices;
    providerPanel.setDeviceProvider(&deferredDevices);
    QString deferredError;
    QString deferredActivated;
    QObject::connect(&providerPanel, &QtFileMgrPlacesPanel::errorMessage,
                     [&](const QString &message) { deferredError = message; });
    QObject::connect(&providerPanel, &QtFileMgrPlacesPanel::placeActivated,
                     [&](const QString &path) { deferredActivated = path; });
    check(providerPanel.mountDevice(QStringLiteral("deferred-device"), true)
              && deferredDevices.mountRequests == 1,
          QStringLiteral("deferred mount accepted"));
    const QString deferredMountPath = root.filePath(QStringLiteral("deferred-mount"));
    QTimer::singleShot(100, [&]() { deferredDevices.completeMount(deferredMountPath); });
    check(waitUntil([&]() { return !deferredActivated.isEmpty(); }, 2000)
              && deferredActivated == deferredMountPath
              && deferredError.isEmpty(),
          QStringLiteral("mount activation waits for devicesChanged instead of misreporting"));

    auto *pendingDevices = new PendingDeviceProvider;
    providerPanel.setDeviceProvider(pendingDevices);
    bool destroyedOperationFinished = false;
    QObject::connect(
        &providerPanel, &QtFileMgrPlacesPanel::deviceOperationFinished,
        [&](const QString &id, bool mounting, bool success,
            const QString &error) {
            if (id == QStringLiteral("pending-device") && mounting
                && !success && !error.isEmpty()) {
                destroyedOperationFinished = true;
            }
        });
    const bool pendingMountAccepted = providerPanel.mountDevice(
        QStringLiteral("pending-device"));
    delete pendingDevices;
    check(pendingMountAccepted && destroyedOperationFinished,
          QStringLiteral("provider destruction completes a pending device operation"));

    bool trashPlaceTriggered = false;
    QObject::connect(&providerPanel, &QtFileMgrPlacesPanel::trashRequested,
                     [&]() { trashPlaceTriggered = true; });
    QModelIndex trashPlaceIndex;
    QAbstractItemModel *placesModel = providerPanel.model();
    for (int groupRow = 0; placesModel && groupRow < placesModel->rowCount(); ++groupRow) {
        const QModelIndex group = placesModel->index(groupRow, 0);
        for (int row = 0; row < placesModel->rowCount(group); ++row) {
            const QModelIndex child = placesModel->index(row, 0, group);
            if (child.data().toString() == QStringLiteral("回收站")) {
                trashPlaceIndex = child;
                break;
            }
        }
    }
    QMetaObject::invokeMethod(
        &providerPanel, "onItemClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, trashPlaceIndex));
    check(trashPlaceIndex.isValid() && trashPlaceTriggered,
          QStringLiteral("places panel exposes a working trash entry"));

    auto hasNetworkGroup = [&providerPanel]() {
        QAbstractItemModel *model = providerPanel.model();
        for (int row = 0; model && row < model->rowCount(); ++row) {
            if (model->index(row, 0).data().toString() == QStringLiteral("网络"))
                return true;
        }
        return false;
    };
    const bool networkInitiallyVisible = hasNetworkGroup();
    bool sshConnectEntryPresent = false;
    QAbstractItemModel *networkModel = providerPanel.model();
    for (int groupRow = 0; networkModel
         && groupRow < networkModel->rowCount(); ++groupRow) {
        const QModelIndex group = networkModel->index(groupRow, 0);
        if (group.data().toString() != QStringLiteral("网络"))
            continue;
        for (int row = 0; row < networkModel->rowCount(group); ++row) {
            const QModelIndex child = networkModel->index(row, 0, group);
            if (child.data().toString() == QStringLiteral("连接服务器..."))
                sshConnectEntryPresent = true;
        }
    }
    providerPanel.registerNetworkPlace(
        QStringLiteral("测试网络"), QStringLiteral("remote://test"));
    const bool networkRemainsVisible = hasNetworkGroup();
    providerPanel.unregisterNetworkPlace(QStringLiteral("remote://test"));
    check(networkInitiallyVisible && networkRemainsVisible && hasNetworkGroup(),
          QStringLiteral("places panel retains custom network groups with either SSH configuration"));
#if QTFILEMGR_WITH_SSH
    check(sshConnectEntryPresent,
          QStringLiteral("SSH-enabled places panel exposes the server connection entry"));
#else
    check(!sshConnectEntryPresent,
          QStringLiteral("SSH-disabled places panel hides the built-in server connection entry"));
#endif

    const QList<PlaceEntry> savedNetworkPlaces =
        QtFileMgrConfigStore::loadNetworkPlaces();
    PlaceEntry persistedSshPlace;
    persistedSshPlace.name = QStringLiteral("测试 SSH");
    persistedSshPlace.path = QStringLiteral(
        "ssh://tester@example.invalid:2222/home/tester");
    persistedSshPlace.iconTheme = QStringLiteral("folder-remote");
    QtFileMgrConfigStore::saveNetworkPlaces({persistedSshPlace});
    const QList<PlaceEntry> loadedNetworkPlaces =
        QtFileMgrConfigStore::loadNetworkPlaces();
    check(loadedNetworkPlaces.size() == 1
              && loadedNetworkPlaces.first().name == persistedSshPlace.name
              && loadedNetworkPlaces.first().path == persistedSshPlace.path,
          QStringLiteral("SSH network places persist without credentials"));
    QtFileMgrConfigStore::saveNetworkPlaces(savedNetworkPlaces);

    // 重连补密码历史污染: 规范化判重只保留首条 (ssh:// 主机大小写/端口/路径归一)
    {
        PlaceEntry dupA;
        dupA.name = QStringLiteral("测试 SSH");
        dupA.path = QStringLiteral("ssh://tester@example.invalid:2222/home/tester");
        dupA.iconTheme = QStringLiteral("folder-remote");
        PlaceEntry dupB;
        dupB.name = QStringLiteral("测试 SSH");
        dupB.path = QStringLiteral("ssh://tester@EXAMPLE.invalid:2222//home/tester/");
        dupB.iconTheme = QStringLiteral("folder-remote");
        QtFileMgrConfigStore::saveNetworkPlaces({dupA, dupB});
        const QList<PlaceEntry> deduped =
            QtFileMgrConfigStore::loadNetworkPlaces();
        check(deduped.size() == 1
                  && deduped.first().path == dupA.path,
              QStringLiteral("network places dedup normalized duplicates on load"));
        QtFileMgrConfigStore::saveNetworkPlaces(savedNetworkPlaces);
    }

#if QTFILEMGR_WITH_SSH
    // SSH 凭据混淆存储: 往返/身份隔离/篡改拒绝/删除/0600 权限
    {
        const QString credUser = QStringLiteral("tester");
        const QString credHost = QStringLiteral("cred-test.invalid");
        QtFileMgrConfigStore::removeSshCredential(credUser, credHost, 22);
        QtFileMgrConfigStore::saveSshCredential(credUser, credHost, 22,
                                                QStringLiteral("s3cret密码"));
        check(QtFileMgrConfigStore::loadSshCredential(credUser, credHost, 22)
                  == QStringLiteral("s3cret密码"),
              QStringLiteral("SSH credential survives an obfuscated round trip"));
        check(QtFileMgrConfigStore::loadSshCredential(credUser, credHost, 2222)
                      .isEmpty()
                  && QtFileMgrConfigStore::loadSshCredential(
                         QStringLiteral("other"), credHost, 22).isEmpty(),
              QStringLiteral("SSH credential is isolated by identity"));
        // 篡改内容必须被拒绝而非送出乱码密码(组内全部键一并污染,
        // 兼容历史运行遗留的其他身份条目)
        {
            QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                               QtFileMgrConfigStore::settingsApplication());
            settings.beginGroup(QStringLiteral("sshCredentials"));
            const QStringList credKeys = settings.allKeys();
            check(!credKeys.isEmpty(),
                  QStringLiteral("SSH credential entry exists in settings"));
            for (const QString &key : credKeys)
                settings.setValue(key, QStringLiteral("garbage-not-obfuscated"));
            settings.endGroup();
        }  // 析构落盘, 结束污染
        check(QtFileMgrConfigStore::loadSshCredential(credUser, credHost, 22)
                  .isEmpty(),
              QStringLiteral("corrupted SSH credential is rejected"));
        QtFileMgrConfigStore::saveSshCredential(credUser, credHost, 22,
                                                QStringLiteral("again"));
        QSettings permProbe(QtFileMgrConfigStore::settingsOrganization(),
                            QtFileMgrConfigStore::settingsApplication());
        const QFileInfo settingsFile(permProbe.fileName());
        const auto perms = settingsFile.permissions();
        check(!(perms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                         | QFileDevice::ReadOther | QFileDevice::WriteOther)),
              QStringLiteral("credential store file is owner-only (0600)"));
        QtFileMgrConfigStore::removeSshCredential(credUser, credHost, 22);
        check(QtFileMgrConfigStore::loadSshCredential(credUser, credHost, 22)
                  .isEmpty(),
              QStringLiteral("removed SSH credential stays gone"));
    }
#endif

    // R2-WS-P1-1 行为钉住: 缩短 customPlaces 数组后删除项不得复活
    const QList<PlaceEntry> savedCustomPlaces =
        QtFileMgrConfigStore::loadCustomPlaces();
    PlaceEntry customA;
    customA.name = QStringLiteral("测试甲");
    customA.path = QStringLiteral("/tmp/qtfilemgr-probe-a");
    customA.iconTheme = QStringLiteral("folder");
    PlaceEntry customB;
    customB.name = QStringLiteral("测试乙");
    customB.path = QStringLiteral("/tmp/qtfilemgr-probe-b");
    customB.iconTheme = QStringLiteral("folder");
    PlaceEntry customC;
    customC.name = QStringLiteral("测试丙");
    customC.path = QStringLiteral("/tmp/qtfilemgr-probe-c");
    customC.iconTheme = QStringLiteral("folder");
    QtFileMgrConfigStore::saveCustomPlaces({customA, customB, customC});
    QtFileMgrConfigStore::saveCustomPlaces({customA});
    const QList<PlaceEntry> shrunkCustomPlaces =
        QtFileMgrConfigStore::loadCustomPlaces();
    check(shrunkCustomPlaces.size() == 1
              && shrunkCustomPlaces.first().name == customA.name,
          QStringLiteral("shrinking custom places does not resurrect removed entries"));
    QtFileMgrConfigStore::saveCustomPlaces(savedCustomPlaces);

#if QTFILEMGR_WITH_SSH
    QtFileMgrSSHIO sshIO;
    QtFileMgrSSHConnectionConfig sshConfig;
    sshConfig.host = QStringLiteral("127.0.0.1");
    sshConfig.port = 1;
    sshConfig.user = QStringLiteral("tester");
    sshConfig.initialPath = QStringLiteral("/srv/data");
    sshConfig.timeoutSeconds = 1;
    bool sshHeartbeat = false;
    bool sshConnectionFinished = false;
    bool sshConnectionSucceeded = true;
    QString sshConnectionError;
    QObject::connect(&sshIO, &QtFileMgrSSHIO::connectionFinished,
                     [&](bool success, const QString &error) {
        sshConnectionFinished = true;
        sshConnectionSucceeded = success;
        sshConnectionError = error;
    });
    QTimer::singleShot(0, &sshIO, [&]() { sshHeartbeat = true; });
    sshIO.connectToHost(sshConfig);
    check(waitUntil([&]() { return sshConnectionFinished; }, 5000)
              && sshHeartbeat && !sshConnectionSucceeded
              && !sshConnectionError.isEmpty(),
          QStringLiteral("SSH connection failure completes asynchronously with an error"));

    const QUrl sshUrl = sshIO.urlForPath(QStringLiteral("/srv/data/logs"));
    check(QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                                       QStringLiteral("project")) == QStringLiteral("project")
              && QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                   QStringLiteral("/home/user/project")) == QStringLiteral("project")
              && QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                   QStringLiteral("project\\files")) == QStringLiteral("project\\files")
              && QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                   QStringLiteral("/home/other/project")).isEmpty()
              && QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                   QStringLiteral("/home/user/../escape")).isEmpty()
              && QtFileMgrSSHIO::safeEntryName(QStringLiteral("/home/user"),
                   QStringLiteral("../escape")).isEmpty(),
          QStringLiteral("SSH listing accepts direct children but rejects escaping paths"));
    QUrl foreignSshUrl = sshUrl;
    foreignSshUrl.setHost(QStringLiteral("other-host"));
    check(sshUrl.scheme() == QStringLiteral("ssh")
              && sshUrl.userName() == sshConfig.user
              && sshUrl.host() == sshConfig.host
              && sshUrl.port() == sshConfig.port
              && sshUrl.path() == QStringLiteral("/srv/data/logs")
              && sshIO.acceptsUrl(sshUrl)
              && !sshIO.acceptsUrl(foreignSshUrl)
              && sshIO.pathFromUrl(sshUrl)
                     == QStringLiteral("/srv/data/logs"),
          QStringLiteral("SSH URL mapping preserves and validates connection identity"));

    bool sshDisconnectedListingFinished = false;
    QString sshDisconnectedListingError;
    QtFileMgrDirectoryRequest *sshDisconnectedListing =
        sshIO.listDirAsync(QStringLiteral("/"), &sshIO);
    QObject::connect(sshDisconnectedListing,
                     &QtFileMgrDirectoryRequest::finished,
                     [&](const QString &error) {
        sshDisconnectedListingFinished = true;
        sshDisconnectedListingError = error;
    });
    check(waitUntil([&]() { return sshDisconnectedListingFinished; })
              && !sshDisconnectedListingError.isEmpty()
              && sshIO.supportsCapability(
                     QtFileMgrIO::LocalExportCapability)
              && !sshIO.supportsCapability(QtFileMgrIO::TrashCapability),
          QStringLiteral("disconnected SSH listing closes with explicit capability state"));
#endif

    QtFileMgrConfig remoteConfig = QtFileMgrConfig::defaultConfig();
    remoteConfig.allowedPaths.clear();
    QtFileMgrWidget remoteWidget(remoteConfig, &delayedRemote);
    check(remoteWidget.io() == &delayedRemote
              && remoteWidget.url().scheme() == QStringLiteral("remote")
              && remoteWidget.fileView()->io() == &delayedRemote,
          QStringLiteral("remote IO injection synchronizes widget, view, and URL state"));
    const QUrl acceptedRemoteUrl = remoteWidget.url();
    remoteWidget.setUrl(QUrl::fromLocalFile(stateRoot));
    check(remoteWidget.url() == acceptedRemoteUrl,
          QStringLiteral("remote IO rejects a local URL without changing state"));

#if QTFILEMGR_WITH_SSH
    QtFileMgrConfig sshScopeConfig = QtFileMgrConfig::defaultConfig();
    sshScopeConfig.allowedPaths = QStringList()
        << QStringLiteral("ssh://tester@127.0.0.1:1/shared");
    QtFileMgrWidget sshScopeWidget(sshScopeConfig, &sshIO);
    const QUrl allowedSshUrl(QStringLiteral("ssh://tester@127.0.0.1:1/shared/sub"));
    sshScopeWidget.setUrl(allowedSshUrl);
    check(sshScopeWidget.url() == allowedSshUrl,
          QStringLiteral("SSH access scope accepts a child on the configured host"));
    sshScopeWidget.setUrl(QUrl(QStringLiteral("ssh://tester@127.0.0.1:1/other")));
    check(sshScopeWidget.url() == allowedSshUrl,
          QStringLiteral("SSH access scope rejects another path"));
    QtFileMgrConfig foreignScopeConfig = sshScopeConfig;
    foreignScopeConfig.allowedPaths = QStringList()
        << QStringLiteral("ssh://tester@host-b:1/shared");
    QtFileMgrWidget foreignScopeWidget(foreignScopeConfig, &sshIO);
    const QUrl foreignInitialUrl = foreignScopeWidget.url();
    foreignScopeWidget.setUrl(allowedSshUrl);
    check(foreignScopeWidget.url() == foreignInitialUrl,
          QStringLiteral("SSH access scope rejects the same path on another host"));
#endif

    QtFileMgrPlacesPanel sshRootPlaces;
    sshRootPlaces.registerNetworkPlace(
        QStringLiteral("SSH root"), QStringLiteral("ssh://tester@host-a:22/"));
    const PlaceMatch rootMatch = sshRootPlaces.findPlaceForPath(
        QStringLiteral("ssh://tester@host-a:22/home"));
    check(rootMatch.found && rootMatch.name == QStringLiteral("SSH root"),
          QStringLiteral("SSH root Place matches descendant paths"));

    {
        QtFileMgrConfig cfg = QtFileMgrConfig::defaultConfig();
        cfg.allowedPaths.clear();
        QtFileMgrWidget widget(cfg);
        ManualProtocolProvider provider;
        widget.setProtocolProvider(QStringLiteral("remote"), &provider);
        // 本组用例断言 provider 调用语义,从空注册表开始隔离复用路径
        QtFileMgrFileView::clearRemoteBackendRegistry();
        const auto activate = [&widget](const QString &path) {
            return QMetaObject::invokeMethod(&widget, "onPlaceActivated", Qt::DirectConnection,
                                             Q_ARG(QString, path));
        };
        const QUrl target(QStringLiteral("remote:///fixture"));
        check(activate(target.toString()) && provider.callbacks.size() == 1,
              QStringLiteral("injected protocol provider receives connection request"));
        // 同 URL 在途去重: 并发触发只保留首个,消除双重密码框竞态
        check(activate(target.toString()) && provider.callbacks.size() == 1,
              QStringLiteral("duplicate in-flight connection to the same URL is deduplicated"));
        // 不同 URL 在途时仍执行取代语义(需在完成前触发,完成后 io 变为
        // 远端后端,同 scheme URL 直接导航不再走 provider)
        const QUrl second(QStringLiteral("remote:///second"));
        check(activate(second.toString()) && provider.callbacks.size() == 2,
              QStringLiteral("new connection supersedes pending provider callback"));
        QPointer<DelayedRemoteIO> stale = new DelayedRemoteIO;
        QtFileMgrConnectionResult result;
        result.url = target;
        result.io = stale;
        result.transferOwnership = true;
        provider.callbacks.at(0)(result);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(stale.isNull() && widget.io() != result.io,
              QStringLiteral("stale owned backend is disposed without changing current IO"));
        QPointer<DelayedRemoteIO> retained = new DelayedRemoteIO;
        result.io = retained;
        result.url = second;
        result.transferOwnership = false;
        provider.callbacks.at(1)(result);
        check(widget.io() == retained && retained->parent() != &widget,
              QStringLiteral("accepted non-owned backend remains caller-owned"));
        QPointer<DelayedRemoteIO> duplicate = new DelayedRemoteIO;
        result.io = duplicate;
        result.transferOwnership = true;
        provider.callbacks.at(1)(result);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(widget.io() == retained && duplicate && duplicate->parent() == nullptr,
              QStringLiteral("duplicate callback cannot dispose unaccepted backend or replace IO"));
        delete duplicate;
        // Changing IO must not delete the provider-owned previous backend.
        widget.setIO(nullptr);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(retained && retained->parent() == nullptr,
              QStringLiteral("backend switch preserves caller-owned backend"));
        delete retained;
    }

    // 审查#12: 在途连接期间导航本地目录不得把连接判过期
    {
        QtFileMgrConfig navCfg = QtFileMgrConfig::defaultConfig();
        navCfg.allowedPaths.clear();
        QtFileMgrWidget navWidget(navCfg);
        ManualProtocolProvider navProvider;
        navWidget.setProtocolProvider(QStringLiteral("remote"), &navProvider);
        QtFileMgrFileView::clearRemoteBackendRegistry();
        const auto activate = [&navWidget](const QString &path) {
            return QMetaObject::invokeMethod(&navWidget, "onPlaceActivated",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, path));
        };
        check(activate(QStringLiteral("remote:///fixture"))
                  && navProvider.callbacks.size() == 1,
              QStringLiteral("in-flight connection registered"));
        // 连接在途期间本地导航:此前会递增连接代次导致完成被丢弃
        navWidget.setUrl(QUrl::fromLocalFile(root.absolutePath()));
        QPointer<DelayedRemoteIO> surviving = new DelayedRemoteIO;
        QtFileMgrConnectionResult navResult;
        navResult.url = QUrl(QStringLiteral("remote:///fixture"));
        navResult.io = surviving;
        navResult.transferOwnership = false;
        navProvider.callbacks.at(0)(navResult);
        check(navWidget.io() == surviving && !surviving.isNull(),
              QStringLiteral("navigation during an in-flight connection keeps the completion valid"));
        navWidget.setIO(nullptr);
        delete surviving;
    }

    // 同主机复用: 已有已连接远端后端时不再发起新连接
    {
        DelayedRemoteIO sharedBackend;
        QPointer<DelayedRemoteIO> sharedGuard(&sharedBackend);
        QtFileMgrFileView sharedView(&sharedBackend);  // 注册进远端注册表
        QtFileMgrConfig reuseCfg = QtFileMgrConfig::defaultConfig();
        reuseCfg.allowedPaths.clear();
        QtFileMgrWidget reuseWidget(reuseCfg);
        ManualProtocolProvider unusedProvider;
        reuseWidget.setProtocolProvider(QStringLiteral("remote"), &unusedProvider);
        const auto activate = [&reuseWidget](const QString &path) {
            return QMetaObject::invokeMethod(&reuseWidget, "onPlaceActivated",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, path));
        };
        check(activate(QStringLiteral("remote:///fixture"))
                  && unusedProvider.callbacks.isEmpty()
                  && reuseWidget.io() == &sharedBackend,
              QStringLiteral("widget reuses a connected backend without reconnecting"));
        // 解除共享,共享后端仍归原视图所有,不被删除
        reuseWidget.setIO(nullptr);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!sharedGuard.isNull(),
              QStringLiteral("shared backend survives widget switch"));
    }

    // ssh 在途连接按主机归并: 同主机不同路径只连一次,连通后导航到最新目标
    {
        QtFileMgrConfig coalesceCfg = QtFileMgrConfig::defaultConfig();
        coalesceCfg.allowedPaths.clear();
        QtFileMgrWidget coalesceWidget(coalesceCfg);
        ManualProtocolProvider sshProvider;
        coalesceWidget.setProtocolProvider(QStringLiteral("ssh"), &sshProvider);
        const auto activate = [&coalesceWidget](const QString &path) {
            return QMetaObject::invokeMethod(&coalesceWidget, "onPlaceActivated",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, path));
        };
        SshRemoteIO *sshBackend = new SshRemoteIO;
        check(activate(QStringLiteral("ssh://tester@example.invalid:22/home/a"))
                  && activate(QStringLiteral("ssh://tester@example.invalid:22/home/b"))
                  && sshProvider.callbacks.size() == 1,
              QStringLiteral("same-authority in-flight connections are coalesced"));
        QtFileMgrConnectionResult result;
        result.url = QUrl(QStringLiteral("ssh://tester@example.invalid:22/home/a"));
        result.io = sshBackend;
        result.transferOwnership = false;
        sshProvider.callbacks.at(0)(result);
        check(coalesceWidget.io() == sshBackend
                  && coalesceWidget.url().path() == QStringLiteral("/home/b"),
              QStringLiteral("deferred navigation lands on the latest same-host target"));
        coalesceWidget.setIO(nullptr);
        delete sshBackend;
    }

#if QTFILEMGR_WITH_SSH
    // 重连补密码框: 隐藏且不勾选"添加到网络位置"
    {
        QtFileMgrSSHDialog reconnectDialog;
        reconnectDialog.setUrl(
            QUrl(QStringLiteral("ssh://tester@example.invalid:22/home/tester")));
        reconnectDialog.setReconnectMode(true);
        bool addCheckVisible = false;
        for (QCheckBox *box : reconnectDialog.findChildren<QCheckBox *>()) {
            if (box->text() == QStringLiteral("添加到网络位置")
                && !box->isHidden())
                addCheckVisible = true;
        }
        check(!reconnectDialog.addToPlaces() && !addCheckVisible,
              QStringLiteral("reconnect dialog hides and unchecks add-to-places"));
    }
#endif

    {
        QtFileMgrPreviewRequest request;
        int calls = 0;
        QByteArray bytes;
        QString error;
        QObject::connect(&request, &QtFileMgrPreviewRequest::finished, &request,
                         [&](const QByteArray &value, bool, const QString &message) {
            ++calls; bytes = value; error = message;
        });
        request.cancel();
        request.finish(QByteArray("stale"));
        request.finish(QByteArray("duplicate"));
        check(calls == 1 && bytes.isEmpty() && !error.isEmpty() && request.isCancelled(),
              QStringLiteral("cancelled preview drops bytes and duplicate finish emits once"));
    }
    {
        ManualPreviewIO manual;
        QtFileMgrPreviewPanel panel;
        panel.setIO(&manual);
        panel.setFiles({QStringLiteral("first.txt")});
        QPointer<QtFileMgrPreviewRequest> first = manual.requests.at(0);
        panel.setFiles({QStringLiteral("second.txt")});
        check(first && first->isCancelled() && manual.requests.size() == 2,
              QStringLiteral("selection change cancels prior preview request"));
        first->finish(QByteArray("obsolete"));
        manual.requests.at(1)->finish(QByteArray("current"));
        check(panel.previewMode() == QtFileMgrPreviewPanel::TextPreview
                  && panel.previewText() == QStringLiteral("current"),
              QStringLiteral("stale completion cannot overwrite current selection"));
        panel.setFiles({QStringLiteral("third.txt")});
        QPointer<QtFileMgrPreviewRequest> third = manual.requests.at(2);
        panel.clear();
        check(third && third->isCancelled(),
              QStringLiteral("clearing preview panel cancels pending request"));
        third->finish(QByteArray("obsolete"));
        check(panel.previewMode() == QtFileMgrPreviewPanel::EmptyPreview
                  && panel.previewText().isEmpty(),
              QStringLiteral("late completion after clear leaves panel empty"));
    }

    {
        CatalogNavigationIO catalog;
        QtFileMgrConfig cfg = QtFileMgrConfig::defaultConfig();
        cfg.rootPath = QStringLiteral("catalog:///documents");
        cfg.allowedPaths = QStringList() << QStringLiteral("catalog:///");
        cfg.places = {{QStringLiteral("Catalog documents"),
                       QStringLiteral("catalog:///documents"), QStringLiteral("folder")}};
        cfg.showDevices = false;
        cfg.allowCreate = cfg.allowDelete = cfg.allowRename = cfg.allowDragDrop = false;
        QtFileMgrWidget browser(cfg, &catalog);
        const QUrl documents(QStringLiteral("catalog:///documents"));
        const QUrl guide(QStringLiteral("catalog:///documents/guide"));
        const QUrl nested(QStringLiteral("catalog:///documents/guide/nested"));
        const QUrl catalogRoot(QStringLiteral("catalog:///"));
        check(browser.url() == documents,
              QStringLiteral("custom protocol configuration navigates through the injected backend"));
        browser.setUrl(nested);
        check(browser.url() == nested && browser.breadcrumbBar()->text() == nested.toString(),
              QStringLiteral("custom protocol URL is displayed without local filesystem conversion"));
        bool hasPlaceSegment = false;
        bool hasGuideSegment = false;
        for (QToolButton *button : browser.breadcrumbBar()->findChildren<QToolButton *>()) {
            hasPlaceSegment |= button->property("path").toString() == documents.toString()
                && button->property("fullLabel").toString() == QStringLiteral("Catalog documents");
            hasGuideSegment |= button->property("path").toString() == guide.toString();
        }
        const PlaceMatch match = browser.placesPanel()->findPlaceForPath(nested.toString());
        check(hasPlaceSegment && hasGuideSegment && match.found
                  && match.rootPath == documents.toString(),
              QStringLiteral("custom protocol breadcrumbs and Places preserve ancestor URLs"));
        const QModelIndexList selectedPlaces = browser.placesPanel()->selectionModel()->selectedIndexes();
        check(!selectedPlaces.isEmpty()
                  && selectedPlaces.first().data(Qt::UserRole).toString() == documents.toString(),
              QStringLiteral("custom protocol navigation selects its configured Place"));
        check(!browser.placesPanel()->findPlaceForPath(
                  QStringLiteral("catalog://other/documents/guide")).found
                  && !browser.placesPanel()->findPlaceForPath(
                      QStringLiteral("another:///documents/guide")).found,
              QStringLiteral("custom Places reject a different authority or protocol"));

        browser.goUp();
        check(browser.url() == guide, QStringLiteral("custom widget navigation goes up one backend level"));
        browser.fileView()->goUp();
        check(browser.url() == documents, QStringLiteral("custom file view navigation preserves the protocol"));
        browser.breadcrumbBar()->goUp();
        check(browser.url() == catalogRoot, QStringLiteral("custom breadcrumb navigation reaches the backend root"));
        browser.goUp();
        browser.fileView()->goUp();
        browser.breadcrumbBar()->goUp();
        check(browser.url() == catalogRoot,
              QStringLiteral("custom root navigation cannot escape into a local directory"));

        const auto editPath = [&browser](const QString &path) {
            QtFileMgrBreadcrumbBar *bar = browser.breadcrumbBar();
            bar->setEditableMode(true);
            QLineEdit *editor = bar->findChild<QLineEdit *>();
            if (!editor)
                return false;
            editor->setText(path);
            return QMetaObject::invokeMethod(bar, "onEditReturnPressed", Qt::DirectConnection);
        };
        check(editPath(guide.toString()) && browser.url() == guide,
              QStringLiteral("custom address editing accepts an explicit backend URL"));
        check(editPath(QStringLiteral("../")) && browser.url() == documents,
              QStringLiteral("custom address editing resolves relative paths inside the backend"));
        check(editPath(QStringLiteral("/")) && browser.url() == catalogRoot,
              QStringLiteral("custom address editing preserves the backend root"));
        check(QMetaObject::invokeMethod(&browser, "onPlaceActivated", Qt::DirectConnection,
                                       Q_ARG(QString, documents.toString()))
                  && browser.url() == documents && browser.io() == &catalog,
              QStringLiteral("custom Place activation retains its injected backend"));

        QtFileMgrConfig scopedCfg = cfg;
        scopedCfg.allowedPaths = QStringList() << documents.toString();
        QtFileMgrWidget scoped(scopedCfg, &catalog);
        scoped.setUrl(guide);
        check(scoped.url() == guide,
              QStringLiteral("custom allowedPaths accepts a child of its protocol root"));
        scoped.setUrl(QUrl(QStringLiteral("catalog:///documents2")));
        check(scoped.url() == guide,
              QStringLiteral("custom allowedPaths rejects a sibling sharing the root prefix"));
        scoped.goUp();
        scoped.goUp();
        check(scoped.url() == documents,
              QStringLiteral("custom allowedPaths prevents navigation above the allowed root"));
        QtFileMgrConfig foreignCfg = scopedCfg;
        foreignCfg.allowedPaths = QStringList() << QStringLiteral("catalog://other/documents");
        QtFileMgrWidget foreign(foreignCfg, &catalog);
        const QUrl foreignInitial = foreign.url();
        foreign.setUrl(guide);
        check(foreign.url() == foreignInitial,
              QStringLiteral("custom allowedPaths does not grant a different authority access"));
    }

    QTextStream(stdout) << "RESULT: " << (failures == 0 ? "PASS" : "FAIL")
                        << " (" << failures << " failures)" << '\n';
    return failures == 0 ? 0 : 1;
}
