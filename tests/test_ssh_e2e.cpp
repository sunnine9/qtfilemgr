// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrSSHIO.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrPlacesPanel.h"
#include "QtFileMgrFileOperationJob.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QBuffer>
#include <QPointer>
#include <QTextStream>
#include <QTimer>

#include <functional>

namespace {
int failures = 0;
void check(bool ok, const QString &description)
{
    QTextStream(stdout) << (ok ? "PASS: " : "FAIL: ") << description << '\n';
    if (!ok) ++failures;
}
bool waitFor(const std::function<bool()> &condition, int ms = 15000)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return condition();
}
QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(data) == data.size();
}
struct Preview {
    bool done = false;
    QByteArray bytes;
    bool truncated = false;
    QString error;
};
Preview preview(QtFileMgrPreviewRequest *request, bool cancel = false)
{
    Preview result;
    const auto connection = QObject::connect(request, &QtFileMgrPreviewRequest::finished, request,
                     [&](const QByteArray &bytes, bool truncated, const QString &error) {
        result.done = true; result.bytes = bytes;
        result.truncated = truncated; result.error = error;
    });
    if (cancel) request->cancel();
    if (!waitFor([&]() { return result.done; }))
        result.error = QStringLiteral("preview timed out");
    QObject::disconnect(connection);
    return result;
}
struct Transfer {
    bool done = false;
    bool success = false;
    bool cancelled = false;
    int finishedCount = 0;
    QString error;
    qint64 bytes = 0;
};
Transfer transfer(QtFileMgrTransferRequest *request, bool cancelOnProgress = false,
                  const std::function<void()> &onProgress = {})
{
    Transfer result;
    bool progressActionCalled = false;
    const auto progressConnection = QObject::connect(request, &QtFileMgrTransferRequest::progress, request,
                     [&](qint64 bytes, qint64) {
        result.bytes = bytes;
        if (cancelOnProgress && bytes > 0)
            request->cancel();
        if (onProgress && bytes > 0 && !progressActionCalled) {
            progressActionCalled = true;
            onProgress();
        }
    });
    const auto finishedConnection = QObject::connect(request, &QtFileMgrTransferRequest::finished, request,
                     [&](bool success, const QString &, const QString &error, bool cancelled) {
        ++result.finishedCount;
        result.done = true;
        result.success = success;
        result.error = error;
        result.cancelled = cancelled;
    });
    if (!waitFor([&]() { return result.done; }, 30000))
        result.error = QStringLiteral("transfer timed out");
    QObject::disconnect(progressConnection);
    QObject::disconnect(finishedConnection);
    return result;
}
bool listing(QtFileMgrSSHIO &io, const QString &path, QList<FileEntry> *entries)
{
    bool done = false;
    QString error;
    auto *request = io.listDirAsync(path);
    const auto entriesConnection = QObject::connect(request, &QtFileMgrDirectoryRequest::entriesReady, request,
                     [&](const QList<FileEntry> &batch) { entries->append(batch); });
    const auto finishedConnection = QObject::connect(request, &QtFileMgrDirectoryRequest::finished, request,
                     [&](const QString &value) { done = true; error = value; });
    const bool completed = waitFor([&]() { return done; });
    QObject::disconnect(entriesConnection);
    QObject::disconnect(finishedConnection);
    if (!completed || !error.isEmpty())
        qWarning("[SSH E2E] listing failed (path=\"%s\", finished=%d, error=\"%s\")",
                 qPrintable(path), completed, qPrintable(error));
    return completed && error.isEmpty();
}

// Fault injection changes only the commit decision; all transfers, backups
// and successful restoration calls still go through the real loopback SFTP.
class CommitFaultSSHIO : public QtFileMgrIO {
public:
    QtFileMgrSSHIO transport;
    QString target;
    QString backup;
    QString staged;
    bool failInstall = false;
    bool failRestore = false;
    QString rootPath() const override { return transport.rootPath(); }
    QString displayName(const QString &path) const override { return transport.displayName(path); }
    bool acceptsUrl(const QUrl &url) const override { return transport.acceptsUrl(url); }
    QUrl urlForPath(const QString &path) const override { return transport.urlForPath(path); }
    QString pathFromUrl(const QUrl &url) const override { return transport.pathFromUrl(url); }
    bool exists(const QString &path) const override { return transport.exists(path); }
    bool isDir(const QString &path) const override { return transport.isDir(path); }
    bool isWritable(const QString &path) const override { return transport.isWritable(path); }
    bool canWrite() const override { return transport.canWrite(); }
    bool isRemote() const override { return true; }
    QList<FileEntry> listDir(const QString &path) const override { return transport.listDir(path); }
    bool mkdir(const QString &path) override { return transport.mkdir(path); }
    bool rename(const QString &from, const QString &to) override { return transport.rename(from, to); }
    bool remove(const QString &path) override { return transport.remove(path); }
    bool copy(const QString &from, const QString &to) override { return transport.copy(from, to); }
    bool writeFile(const QString &path, const QByteArray &bytes) override { return transport.writeFile(path, bytes); }
    QtFileMgrTransferRequest *copyRequest(const QString &from, const QString &to,
                                         QObject *parent = nullptr) override
    { return transport.copyRequest(from, to, parent); }
    bool renameNoReplace(const QString &from, const QString &to,
                         QString *error = nullptr) override
    {
        if (from == target)
            backup = to;
        if (to == target && from != backup)
            staged = from;
        if (to == target && ((from == backup && failRestore)
                            || (from != backup && failInstall))) {
            if (error)
                *error = QStringLiteral("Injected SFTP commit/restore failure");
            qWarning("[SSH E2E] injected rename failure (source=\"%s\", destination=\"%s\", restoring=%d)",
                     qPrintable(from), qPrintable(to), from == backup);
            return false;
        }
        return transport.renameNoReplace(from, to, error);
    }
};

struct JobResult {
    bool done = false;
    int succeeded = 0;
    int failed = 0;
    QStringList errors;
};

JobResult overwrite(QtFileMgrIO &io, const QString &source, const QString &directory)
{
    JobResult result;
    QtFileMgrFileOperationJob job(&io, QtFileMgrFileOperationJob::Copy, {source}, directory,
                                  QtFileMgrFileOperationJob::OverwriteExisting);
    const auto connection = QObject::connect(&job, &QtFileMgrFileOperationJob::finished, &job,
        [&](int succeeded, int failed, int, const QStringList &, bool, const QStringList &errors) {
            result.done = true;
            result.succeeded = succeeded;
            result.failed = failed;
            result.errors = errors;
        });
    job.start();
    if (!waitFor([&]() { return result.done; }, 30000))
        qWarning("[SSH E2E] overwrite job timed out (source=\"%s\", destinationDirectory=\"%s\")",
                 qPrintable(source), qPrintable(directory));
    QObject::disconnect(connection);
    return result;
}

bool connectTo(QtFileMgrSSHIO &io, const QtFileMgrSSHConnectionConfig &config,
               bool acceptKey, bool *prompted = nullptr)
{
    bool done = false, success = false, sawKey = false;
    QString error;
    const auto hostConnection = QObject::connect(
        &io, &QtFileMgrSSHIO::unknownHostKey, &io,
        [&](const QString &host, int port, const QString &fingerprint) {
        sawKey = host == config.host && port == config.port && !fingerprint.isEmpty();
        io.acceptUnknownHostKey(acceptKey);
    });
    const auto finishConnection = QObject::connect(
        &io, &QtFileMgrSSHIO::connectionFinished, &io,
        [&](bool ok, const QString &detail) { done = true; success = ok; error = detail; });
    io.connectToHost(config);
    const bool completed = waitFor([&]() { return done; }, 20000);
    QObject::disconnect(hostConnection);
    QObject::disconnect(finishConnection);
    if (prompted) *prompted = sawKey;
    if (!completed || !success)
        qWarning("[SSH E2E] connection result (host=\"%s\", port=%d, finished=%d, error=\"%s\")",
                 qPrintable(config.host), config.port, completed, qPrintable(error));
    return completed && success;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto required = [](const char *name) { return qEnvironmentVariable(name); };
    const QString base = required("QTFILEMGR_SSH_E2E_ROOT");
    const QString home = required("HOME");
    const QString user = required("QTFILEMGR_SSH_E2E_USER");
    const QString key = required("QTFILEMGR_SSH_E2E_KEY");
    bool portOk = false;
    const int port = required("QTFILEMGR_SSH_E2E_PORT").toInt(&portOk);
    if (required("QTFILEMGR_SSH_E2E") != QStringLiteral("1")
        || base.isEmpty() || home.isEmpty() || user.isEmpty() || key.isEmpty()
        || !portOk || port < 1 || port > 65535 || !QFileInfo(key).isFile()
        || !QFileInfo(base).isDir() || !QFileInfo(home).isDir()
        || !home.contains(QStringLiteral("qtfilemgr-ssh-e2e-"))
        || base == home || home != QDir::homePath()) {
        qCritical("SSH E2E prerequisites missing: use run_ssh_e2e.py (explicit opt-in; isolated HOME, keys, port, root required)");
        return 2;
    }
    const QString knownHosts = required("QTFILEMGR_SSH_KNOWN_HOSTS");
    const QString scratch = QFileInfo(home).absolutePath();
    if (knownHosts.isEmpty() || !QFileInfo(knownHosts).isAbsolute()
        || !knownHosts.startsWith(scratch + QLatin1Char('/'))
        || knownHosts.startsWith(home + QStringLiteral("/.ssh/"))) {
        qCritical("SSH E2E requires an isolated configured known_hosts path");
        return 2;
    }
    check(!QFileInfo(knownHosts).dir().exists(),
          QStringLiteral("known_hosts directory is initially absent"));
    check(QDir().mkpath(QFileInfo(knownHosts).absolutePath()),
          QStringLiteral("create isolated known_hosts parent directory"));
    if (QFileInfo(knownHosts).exists()) {
        qCritical("SSH E2E requires empty isolated known_hosts, refusing existing file");
        return 2;
    }
    QtFileMgrSSHConnectionConfig config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.user = user;
    config.identityFile = key;
    config.initialPath = base;
    config.timeoutSeconds = 5;
    QtFileMgrSSHIO io;
    const QByteArray configuredKnownHosts = QFile::encodeName(knownHosts);
    const QString missingParent = scratch + QStringLiteral("/missing/known_hosts");
    qputenv("QTFILEMGR_SSH_KNOWN_HOSTS", QFile::encodeName(missingParent));
    check(!connectTo(io, config, false)
              && !QFileInfo(missingParent).exists()
              && !QFileInfo(missingParent).dir().exists(),
          QStringLiteral("unwritable configured known_hosts fails without fallback"));
    qputenv("QTFILEMGR_SSH_KNOWN_HOSTS", configuredKnownHosts);
    bool prompted = false;
    check(connectTo(io, config, false, &prompted) == false && prompted && !io.isConnected(),
          QStringLiteral("unknown host key is presented and rejection blocks authentication"));
    prompted = false;
    check(connectTo(io, config, true, &prompted) && prompted && io.isConnected(),
          QStringLiteral("accepted host key authenticates with fixture-only public key"));
    check(QFileInfo(knownHosts).isFile() && !contents(knownHosts).isEmpty(),
          QStringLiteral("host key persists only in isolated HOME"));
    if (!io.isConnected()) return 1;
    io.disconnectFromHost();
    check(connectTo(io, config, false, &prompted) && !prompted,
          QStringLiteral("known host reconnects without prompt"));
    if (!io.isConnected()) return 1;

    const QDir root(base);
    QList<FileEntry> entries;
    const QString dir = root.filePath(QStringLiteral("remote"));
    check(io.mkdir(dir) && QFileInfo(dir).isDir(), QStringLiteral("SFTP mkdir"));
    const QString source = root.filePath(QStringLiteral("upload-source"));
    const QString uploaded = dir + QStringLiteral("/uploaded");
    const QByteArray payload("fixture-only upload\n");
    check(write(source, payload), QStringLiteral("create isolated upload source"));
    bool rootHasRemote = false;
    const bool rootListed = listing(io, base, &entries);
    for (const auto &entry : entries)
        rootHasRemote |= entry.name == QStringLiteral("remote") && entry.isDir;
    check(rootListed && rootHasRemote,
          QStringLiteral("live SFTP listing of populated fixture root"));
    const QString childDir = dir + QStringLiteral("/project");
    check(io.mkdir(childDir), QStringLiteral("create nested SFTP directory"));
    entries.clear();
    bool foundChild = false;
    const bool parentListed = listing(io, dir, &entries);
    for (const auto &entry : entries)
        foundChild |= entry.name == QStringLiteral("project")
            && entry.path == childDir && entry.isDir;
    check(parentListed && foundChild,
          QStringLiteral("nested SFTP directory remains accessible as direct child"));
    const QString nestedDownload = root.filePath(QStringLiteral("downloaded-project"));
    Transfer nestedOutcome = transfer(io.exportToLocalRequest(childDir, nestedDownload));
    check(nestedOutcome.done && nestedOutcome.success
              && QFileInfo(nestedDownload).isDir(),
          QStringLiteral("nested SFTP directory downloads safely to local target"));

    // A saved network place must open its target without showing the new-server form.
    // The fixture identity is supplied by the isolated test HOME, not saved credentials.
    const QString defaultIdentity = home + QStringLiteral("/.ssh/id_rsa");
    check(QFile::copy(key, defaultIdentity),
          QStringLiteral("provide isolated default key for saved-place reconnect"));
    QtFileMgrConfig widgetConfig = QtFileMgrConfig::defaultConfig();
    widgetConfig.allowedPaths.clear();
    QtFileMgrWidget widget(widgetConfig);
    const QUrl savedPlace = io.urlForPath(childDir);
    widget.placesPanel()->registerNetworkPlace(QStringLiteral("dev24"), savedPlace.toString());
    QAbstractItemModel *places = widget.placesPanel()->model();
    QModelIndex savedIndex;
    for (int group = 0; group < places->rowCount(); ++group) {
        const QModelIndex parent = places->index(group, 0);
        for (int row = 0; row < places->rowCount(parent); ++row) {
            const QModelIndex child = places->index(row, 0, parent);
            if (child.data(Qt::UserRole).toString() == savedPlace.toString())
                savedIndex = child;
        }
    }
    const bool activated = savedIndex.isValid() && QMetaObject::invokeMethod(
        widget.placesPanel(), "onItemClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, savedIndex));
    check(activated && waitFor([&]() {
              return widget.io()->acceptsUrl(savedPlace) && widget.url() == savedPlace;
          }, 15000),
          QStringLiteral("saved SSH place connects and opens nested target without setup dialog"));

    Transfer outcome = transfer(io.importFromLocalRequest(source, uploaded, false));
    check(outcome.done && outcome.success && outcome.finishedCount == 1
              && outcome.bytes == payload.size()
              && contents(uploaded) == payload,
          QStringLiteral("explicit local import copies bytes and retains the local source"));
    check(contents(source) == payload,
          QStringLiteral("import copy leaves its local source intact"));
    {
        const QString destination = dir + QStringLiteral("/owner-destroyed-upload");
        QPointer<QObject> requestOwner(new QObject);
        QPointer<QtFileMgrTransferRequest> guardedRequest;
        bool ownerDestroyedByNotification = false;
        int finishedCount = 0;
        const auto pathsConnection = QObject::connect(
            &io, &QtFileMgrIO::pathsChanged, &app,
            [&](const QStringList &paths) {
                if (!paths.contains(destination))
                    return;
                ownerDestroyedByNotification = true;
                delete requestOwner.data();
            });
        guardedRequest = io.importFromLocalRequest(source, destination, false,
                                                   requestOwner.data());
        const auto finishConnection = QObject::connect(
            guardedRequest.data(), &QtFileMgrTransferRequest::finished, &app,
            [&](bool, const QString &, const QString &, bool) { ++finishedCount; });
        const bool notified = waitFor([&]() { return ownerDestroyedByNotification; });
        QObject::disconnect(pathsConnection);
        QObject::disconnect(finishConnection);
        check(notified && requestOwner.isNull() && guardedRequest.isNull()
                  && finishedCount == 0 && contents(destination) == payload
                  && contents(source) == payload,
              QStringLiteral("successful upload survives pathsChanged destroying the request owner"));
        delete requestOwner.data();
    }
    entries.clear();
    bool found = false;
    const bool listed = listing(io, dir, &entries);
    for (const auto &entry : entries)
        found |= entry.name == QStringLiteral("uploaded") && entry.size == payload.size();
    check(listed && found && io.exists(uploaded), QStringLiteral("remote listing updates stat cache"));
    const QString download = root.filePath(QStringLiteral("downloaded"));
    outcome = transfer(io.exportToLocalRequest(uploaded, download));
    check(outcome.done && outcome.success && outcome.bytes == payload.size()
              && contents(download) == payload, QStringLiteral("real download with progress"));
    const QString copied = dir + QStringLiteral("/copied");
    outcome = transfer(io.copyRequest(uploaded, copied));
    check(outcome.done && outcome.success && contents(copied) == payload,
          QStringLiteral("remote-to-remote copy"));
    const QString moved = dir + QStringLiteral("/moved");
    outcome = transfer(io.moveRequest(copied, moved));
    check(outcome.done && outcome.success && !QFileInfo(copied).exists()
              && contents(moved) == payload, QStringLiteral("remote move"));
    outcome = transfer(io.importFromLocalRequest(source, uploaded, false));
    check(outcome.done && !outcome.success && !outcome.error.isEmpty()
              && contents(uploaded) == payload,
          QStringLiteral("existing remote upload target refuses overwrite"));
    outcome = transfer(io.copyRequest(uploaded, moved));
    check(outcome.done && !outcome.success && contents(moved) == payload,
          QStringLiteral("existing remote copy target refuses overwrite"));

    const QString renameSource = dir + QStringLiteral("/rename-source");
    const QString renameTarget = dir + QStringLiteral("/rename-target");
    const QString renameFree = dir + QStringLiteral("/rename-free");
    check(io.writeFile(renameSource, QByteArrayLiteral("rename-original"))
              && io.writeFile(renameTarget, QByteArrayLiteral("keep-existing")),
          QStringLiteral("prepare real no-clobber rename inputs"));
    QString renameError;
    check(!io.renameNoReplace(renameSource, renameTarget, &renameError)
              && !renameError.isEmpty()
              && contents(renameSource) == QByteArrayLiteral("rename-original")
              && contents(renameTarget) == QByteArrayLiteral("keep-existing"),
          QStringLiteral("SFTP safe rename refuses an occupied destination without altering either file"));
    check(io.renameNoReplace(renameSource, renameFree, &renameError)
              && !QFileInfo::exists(renameSource)
              && contents(renameFree) == QByteArrayLiteral("rename-original"),
          QStringLiteral("SFTP safe rename moves to a free destination"));

    {
        CommitFaultSSHIO guarded;
        check(connectTo(guarded.transport, config, false),
              QStringLiteral("connect real SFTP overwrite fixture"));
        const QString sourceDirectory = dir + QStringLiteral("/overwrite-source");
        const QString targetDirectory = dir + QStringLiteral("/overwrite-target");
        check(guarded.mkdir(sourceDirectory) && guarded.mkdir(targetDirectory),
              QStringLiteral("prepare real overwrite directories"));
        const auto runCase = [&](const QString &name, bool failInstall, bool failRestore) {
            const QString input = sourceDirectory + QLatin1Char('/') + name;
            guarded.target = targetDirectory + QLatin1Char('/') + name;
            guarded.backup.clear(); guarded.staged.clear();
            guarded.failInstall = failInstall; guarded.failRestore = failRestore;
            check(guarded.writeFile(input, QByteArrayLiteral("replacement"))
                      && guarded.writeFile(guarded.target, QByteArrayLiteral("original")),
                  QStringLiteral("seed real overwrite case %1").arg(name));
            QList<FileEntry> targetEntries;
            check(listing(guarded.transport, targetDirectory, &targetEntries),
                  QStringLiteral("refresh real overwrite target cache %1").arg(name));
            const JobResult result = overwrite(guarded, input, targetDirectory);
            if (!failInstall) {
                check(result.done && result.succeeded == 1 && result.failed == 0
                          && contents(guarded.target) == QByteArrayLiteral("replacement")
                          && !guarded.backup.isEmpty()
                          && contents(guarded.backup) == QByteArrayLiteral("original")
                          && result.errors.join(QLatin1Char('\n')).contains(guarded.backup),
                      QStringLiteral("real SFTP overwrite commits and reports the retained original backup"));
            } else if (!failRestore) {
                check(result.done && result.succeeded == 0 && result.failed == 1
                          && contents(guarded.target) == QByteArrayLiteral("original")
                          && contents(guarded.staged) == QByteArrayLiteral("replacement")
                          && result.errors.join(QLatin1Char('\n')).contains(guarded.staged),
                      QStringLiteral("real SFTP failed commit restores the original and retains the new staging file"));
            } else {
                const QString detail = result.errors.join(QLatin1Char('\n'));
                check(result.done && result.succeeded == 0 && result.failed == 1
                          && contents(guarded.backup) == QByteArrayLiteral("original")
                          && contents(guarded.staged) == QByteArrayLiteral("replacement")
                          && detail.contains(guarded.backup) && detail.contains(guarded.staged),
                      QStringLiteral("real SFTP failed restore retains and reports both recoverable copies"));
            }
        };
        runCase(QStringLiteral("success.txt"), false, false);
        runCase(QStringLiteral("commit-failure.txt"), true, false);
        runCase(QStringLiteral("restore-failure.txt"), true, true);
    }
    check(write(download, QByteArray("old content")), QStringLiteral("seed local overwrite target"));
    outcome = transfer(io.exportToLocalRequest(uploaded, download));
    check(outcome.done && outcome.success && contents(download) == payload,
          QStringLiteral("download replaces existing local file atomically"));

    const QString moveSource = root.filePath(QStringLiteral("import-move-source"));
    const QString moveTarget = dir + QStringLiteral("/import-moved");
    check(write(moveSource, payload), QStringLiteral("create explicit import-move source"));
    outcome = transfer(io.importFromLocalRequest(moveSource, moveTarget, true));
    check(outcome.done && outcome.success && !QFileInfo::exists(moveSource)
              && contents(moveTarget) == payload,
          QStringLiteral("explicit import move removes local source only after remote commit"));

    Preview text = preview(io.previewRequest(uploaded, 7, false));
    check(text.done && text.error.isEmpty() && text.truncated
              && text.bytes == payload.left(7),
          QStringLiteral("real SSH text preview returns bounded truncated bytes"));
    text = preview(io.previewRequest(uploaded, 0, false));
    check(text.done && !text.error.isEmpty() && text.bytes.isEmpty(),
          QStringLiteral("SSH preview invalid bound reports an error"));
    text = preview(io.previewRequest(uploaded, 128, false), true);
    check(text.done && !text.error.isEmpty() && text.bytes.isEmpty(),
          QStringLiteral("cancelled SSH preview does not deliver bytes"));
    const QString imagePath = dir + QStringLiteral("/preview.png");
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    check(image.save(&buffer, "PNG") && write(imagePath, png),
          QStringLiteral("create fixture image for SSH preview"));
    Preview picture = preview(io.previewRequest(imagePath, png.size(), true));
    check(picture.done && picture.error.isEmpty() && !picture.truncated
              && picture.bytes == png && QImage::fromData(picture.bytes).size() == image.size(),
          QStringLiteral("real SSH image preview returns complete decodable bytes"));
    picture = preview(io.previewRequest(imagePath, png.size() - 1, true));
    check(picture.done && !picture.error.isEmpty() && picture.bytes.isEmpty(),
          QStringLiteral("oversized SSH image preview rejects partial image"));

    const QString bigSource = root.filePath(QStringLiteral("big-source"));
    QByteArray block(8 * 1024 * 1024, 'x');
    check(write(bigSource, block), QStringLiteral("create cancellation input"));
    const QString partial = dir + QStringLiteral("/cancelled");
    outcome = transfer(io.importFromLocalRequest(bigSource, partial, false), true);
    check(outcome.done && !outcome.success && outcome.cancelled && outcome.bytes > 0
              && !QFileInfo(partial).exists(),
          QStringLiteral("upload cancellation removes partial remote file"));

    check(io.removePermanent(moved) && !QFileInfo(moved).exists(),
          QStringLiteral("remote permanent delete"));

    // Start a real export, then disconnect before its queued worker can finish.
    // This asserts cancellation/cleanup rather than only rejecting new requests.
    const QString disconnectSource = dir + QStringLiteral("/disconnect-source");
    outcome = transfer(io.importFromLocalRequest(bigSource, disconnectSource, false));
    check(outcome.done && outcome.success,
          QStringLiteral("prepare remote source for an in-flight disconnect"));
    const QString disconnectTarget = root.filePath(QStringLiteral("disconnect-target"));
    check(write(disconnectTarget, QByteArray("keep-before-disconnect")),
          QStringLiteral("seed local target for disconnect preservation"));
    bool disconnectedOnProgress = false;
    outcome = transfer(io.exportToLocalRequest(disconnectSource, disconnectTarget), false, [&]() {
        disconnectedOnProgress = true;
        io.disconnectFromHost();
    });
    check(disconnectedOnProgress && outcome.done && !outcome.success
              && outcome.cancelled && !outcome.error.isEmpty() && !io.isConnected()
              && contents(disconnectTarget) == QByteArray("keep-before-disconnect"),
          QStringLiteral("disconnect cancels active export and preserves existing local destination"));
    // Ensure the no-progress failure path also leaves the fixture disconnected.
    io.disconnectFromHost();
    entries.clear();
    bool disconnectedDone = false;
    QString disconnectedError;
    auto *request = io.listDirAsync(dir);
    QObject::connect(request, &QtFileMgrDirectoryRequest::finished, request,
                     [&](const QString &error) { disconnectedDone = true; disconnectedError = error; });
    check(!io.isConnected() && waitFor([&]() { return disconnectedDone; })
              && !disconnectedError.isEmpty(),
          QStringLiteral("disconnect rejects subsequent listing with error"));
    QTextStream(stdout) << (failures ? "RESULT: FAIL (" : "RESULT: PASS (")
                        << failures << " failures)" << '\n';
    return failures ? 1 : 0;
}
