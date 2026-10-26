// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "MemoryIO.h"

#include <QtFileMgrWidget.h>
#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QImage>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char *description)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[MemoryIO test] FAIL: %s", description);
    }
}

// Every tested request is scheduled with a zero-delay event-loop callback.
// A later timer also permits deferred deletion before the next assertion.
void drainEvents()
{
    QEventLoop loop;
    QTimer::singleShot(15, &loop, &QEventLoop::quit);
    loop.exec();
}

void checkDirectoryRequest(QtFileMgrDirectoryRequest *request, bool expectError,
                             int expectedEntries, QObject *context,
                             bool cancel = false)
{
    int finished = 0;
    int entries = 0;
    QString error;
    check(!request->isFinished(), "listing must be deferred until the event loop");
    QObject::connect(request, &QtFileMgrDirectoryRequest::entriesReady, context,
        [&](const QList<FileEntry> &batch) { entries += static_cast<int>(batch.size()); });
    QObject::connect(request, &QtFileMgrDirectoryRequest::finished, context,
        [&](const QString &message) { ++finished; error = message; });
    if (cancel)
        request->cancel();
    drainEvents();
    check(finished == 1, "directory request completes exactly once");
    check(error.isEmpty() != expectError, "directory request reports the expected error state");
    check(entries == expectedEntries, "directory request delivers the expected entry count");
    drainEvents();
    check(finished == 1, "directory request has no late second completion");
}

void checkPreviewRequest(QtFileMgrPreviewRequest *request, bool expectError,
                           bool expectTruncation, int expectedBytes, QObject *context,
                           bool cancel = false)
{
    int finished = 0;
    QByteArray bytes;
    QString error;
    bool truncated = false;
    check(!request->isFinished(), "preview must be deferred until the event loop");
    QObject::connect(request, &QtFileMgrPreviewRequest::finished, context,
        [&](const QByteArray &result, bool partial, const QString &message) {
            ++finished; bytes = result; truncated = partial; error = message;
        });
    if (cancel)
        request->cancel();
    drainEvents();
    check(finished == 1, "preview request completes exactly once");
    check(error.isEmpty() != expectError, "preview request reports the expected error state");
    check(truncated == expectTruncation, "preview request reports truncation correctly");
    check(bytes.size() == expectedBytes, "preview returns exactly the permitted bytes");
    drainEvents();
    check(finished == 1, "preview request has no late second completion");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
        qCritical("[MemoryIO test] Cannot isolate temporary settings.");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", settingsDirectory.path().toUtf8());
    qputenv("XDG_DATA_HOME", settingsDirectory.path().toUtf8());
    QApplication app(argc, argv);
    QObject requestOwner;
    MemoryIO io;

    const QString readme = QStringLiteral("/documents/readme.txt");
    const QString imagePath = QStringLiteral("/images/checker.ppm");
    const QtFileMgrIO::Capabilities expected = QtFileMgrIO::ReadCapability
        | QtFileMgrIO::NonBlockingListingCapability | QtFileMgrIO::NonBlockingPreviewCapability
        | QtFileMgrIO::MimeTypeCapability | QtFileMgrIO::RemoteCapability;
    check(io.capabilities() == expected, "backend advertises only its implemented capabilities");
    check(io.isRemote() && !io.canWrite() && !io.isWritable(readme), "backend is non-local and read-only");
    check(io.rootPath() == QStringLiteral("/"), "memory root uses an absolute backend path");
    check(io.acceptsUrl(QUrl(QStringLiteral("memory:///"))), "root URL is accepted");
    check(io.acceptsUrl(io.urlForPath(readme)), "backend path becomes a valid memory URL");
    check(io.pathFromUrl(io.urlForPath(readme)) == readme, "memory URL round trip preserves its path");
    check(io.urlForPath(readme).toString() == QStringLiteral("memory:///documents/readme.txt"),
          "URL conversion preserves the memory triple slash");

    const QStringList invalidUrls = {
        QStringLiteral("file:///documents/readme.txt"), QStringLiteral("remote:///documents"),
        QStringLiteral("memory:/documents"), QStringLiteral("memory://host/documents"),
        QStringLiteral("memory://user@host/documents"), QStringLiteral("memory:///documents?x=1"),
        QStringLiteral("memory:///documents#fragment"), QStringLiteral("memory:///../documents"),
        QStringLiteral("memory:///documents/../images"), QStringLiteral("memory:///documents/./readme.txt"),
        QStringLiteral("memory:////documents"), QStringLiteral("memory:///documents/"),
        QStringLiteral("memory:///documents%2freadme.txt"), QStringLiteral("memory:///%2e%2e/documents"),
        QStringLiteral("memory:///documents/%00.txt")
    };
    for (const QString &value : invalidUrls) {
        const QUrl invalid(value);
        check(!io.acceptsUrl(invalid), qPrintable(QStringLiteral("invalid URL rejected: ") + value));
        check(io.pathFromUrl(invalid).isEmpty(), "invalid URL cannot produce a usable backend path");
    }
    const QStringList invalidPaths = {QString(), QStringLiteral("documents"), QStringLiteral("//documents"),
        QStringLiteral("/documents/.."), QStringLiteral("/documents/"), QStringLiteral("/documents\\readme.txt")};
    for (const QString &path : invalidPaths) {
        check(io.urlForPath(path).isEmpty(), "invalid backend path cannot produce a URL");
        check(!io.exists(path), "invalid backend path never exists");
    }

    check(io.listDir(QStringLiteral("/")).size() == 3, "root listing contains only immediate directories");
    check(io.listDir(QStringLiteral("/documents")).size() == 3, "documents listing contains its three files");
    check(io.listDir(QStringLiteral("/empty")).isEmpty(), "an empty directory has a successful empty listing");
    check(io.listDir(readme).isEmpty(), "listing a file does not expose data as a directory");
    check(io.stat(readme).isReadOnly && io.stat(readme).path == readme, "stat describes a read-only file");
    check(io.stat(QStringLiteral("/documents")).isDir, "stat identifies directories");
    check(io.mimeType(readme) == QStringLiteral("text/plain"), "text MIME is explicit");
    check(io.mimeType(imagePath).startsWith(QStringLiteral("image/")), "image MIME is explicit");
    check(io.mimeType(QStringLiteral("/documents/config.json")) == QStringLiteral("application/json"),
          "JSON MIME is explicit");
    const QString missing = QStringLiteral("/documents/missing.txt");
    check(!io.exists(missing) && io.fileSize(missing) == -1 && io.stat(missing).path.isEmpty(),
          "missing resources are not invented as empty files");
    check(io.mimeType(missing).isEmpty(), "missing resource has no fabricated MIME type");

    bool truncated = false;
    QString error;
    const QByteArray whole = io.readPreview(readme, 4096, &truncated, &error);
    check(error.isEmpty() && !truncated && whole.size() == io.fileSize(readme), "complete text preview matches stat");
    check(io.readPreview(readme, 12, &truncated, &error) == whole.left(12) && truncated && error.isEmpty(),
          "bounded text preview returns exactly the requested prefix");
    check(io.readPreview(readme, 0, &truncated, &error).isEmpty() && !error.isEmpty(), "zero preview limit is rejected");
    check(io.readPreview(readme, 4 * 1024 * 1024 + 1, &truncated, &error).isEmpty() && !error.isEmpty(),
          "excessive preview limit is rejected");
    check(io.readPreview(missing, 100, &truncated, &error).isEmpty() && !error.isEmpty(), "missing preview reports an error");
    check(io.readPreview(QStringLiteral("/documents"), 100, &truncated, &error).isEmpty() && !error.isEmpty(),
          "directory preview reports an error");
    const QByteArray imageBytes = io.readPreview(imagePath, 4096, &truncated, &error);
    check(error.isEmpty() && !QImage::fromData(imageBytes).isNull(), "image fixture is actually decodable by Qt");

    checkDirectoryRequest(io.listDirAsync(QStringLiteral("/documents"), &requestOwner), false, 3, &app);
    checkDirectoryRequest(io.listDirAsync(QStringLiteral("/empty"), &requestOwner), false, 0, &app);
    checkDirectoryRequest(io.listDirAsync(missing, &requestOwner), true, 0, &app);
    checkDirectoryRequest(io.listDirAsync(readme, &requestOwner), true, 0, &app);
    checkDirectoryRequest(io.listDirAsync(QStringLiteral("/documents/../images"), &requestOwner), true, 0, &app);
    checkDirectoryRequest(io.listDirAsync(QStringLiteral("/documents"), &requestOwner), true, 0, &app, true);
    checkPreviewRequest(io.previewRequest(readme, 12, false, &requestOwner), false, true, 12, &app);
    checkPreviewRequest(io.previewRequest(imagePath, 4096, true, &requestOwner), false, false, imageBytes.size(), &app);
    checkPreviewRequest(io.previewRequest(imagePath, 12, true, &requestOwner), true, false, 0, &app);
    checkPreviewRequest(io.previewRequest(readme, 4096, true, &requestOwner), true, false, 0, &app);
    checkPreviewRequest(io.previewRequest(readme, 0, false, &requestOwner), true, false, 0, &app);
    checkPreviewRequest(io.previewRequest(missing, 100, false, &requestOwner), true, false, 0, &app);
    checkPreviewRequest(io.previewRequest(readme, 100, false, &requestOwner), true, false, 0, &app, true);

    auto *destroyedBackend = new MemoryIO;
    auto *orphanListing = destroyedBackend->listDirAsync(QStringLiteral("/documents"), &requestOwner);
    auto *orphanPreview = destroyedBackend->previewRequest(readme, 100, false, &requestOwner);
    delete destroyedBackend;
    // Both timers may run in the first event-loop drain, so capture both first.
    int orphanListFinished = 0, orphanPreviewFinished = 0;
    QObject::connect(orphanListing, &QtFileMgrDirectoryRequest::finished, &app,
        [&](const QString &message) { ++orphanListFinished; check(!message.isEmpty(), "destroyed backend listing fails explicitly"); });
    QObject::connect(orphanPreview, &QtFileMgrPreviewRequest::finished, &app,
        [&](const QByteArray &bytes, bool partial, const QString &message) {
            ++orphanPreviewFinished;
            check(!message.isEmpty() && bytes.isEmpty() && !partial, "destroyed backend preview fails explicitly");
        });
    drainEvents();
    drainEvents();
    check(orphanListFinished == 1 && orphanPreviewFinished == 1, "backend destruction still completes surviving requests once");

    int deletedRequestCallbacks = 0;
    auto *deletedListing = io.listDirAsync(QStringLiteral("/documents"), &requestOwner);
    auto *deletedPreview = io.previewRequest(readme, 100, false, &requestOwner);
    QObject::connect(deletedListing, &QtFileMgrDirectoryRequest::finished, &app,
        [&](const QString &) { ++deletedRequestCallbacks; });
    QObject::connect(deletedPreview, &QtFileMgrPreviewRequest::finished, &app,
        [&](const QByteArray &, bool, const QString &) { ++deletedRequestCallbacks; });
    delete deletedListing;
    delete deletedPreview;
    drainEvents();
    check(deletedRequestCallbacks == 0, "deleted request contexts receive no deferred callbacks");
    auto *deleteDuringBatch = io.listDirAsync(QStringLiteral("/documents"), &requestOwner);
    QPointer<QtFileMgrDirectoryRequest> batchGuard(deleteDuringBatch);
    QObject::connect(deleteDuringBatch, &QtFileMgrDirectoryRequest::entriesReady, &app,
        [deleteDuringBatch](const QList<FileEntry> &) { delete deleteDuringBatch; });
    drainEvents();
    check(batchGuard.isNull(), "deleting the request during batch delivery is safe");

    const QByteArray original = io.readPreview(readme, 4096);
    check(!io.mkdir(QStringLiteral("/created")) && !io.rename(readme, QStringLiteral("/renamed"))
        && !io.remove(readme) && !io.removePermanent(readme) && !io.moveToTrash(readme, &error)
        && !io.copy(readme, QStringLiteral("/copied")) && !io.writeFile(readme, "changed")
        && !io.chmod(readme, QFile::WriteOwner) && !io.symlink(readme, QStringLiteral("/link"))
        && !io.restoreVersion(readme, 1), "every synchronous mutation is denied");
    int deniedCallbacks = 0;
    const auto denied = [&](bool success, const QString &) { check(!success, "asynchronous write is denied"); ++deniedCallbacks; };
    io.renameAsync(readme, QStringLiteral("/renamed"), &app, denied);
    io.copyAsync(readme, QStringLiteral("/copied"), &app, denied);
    io.moveAsync(readme, QStringLiteral("/moved"), &app, denied);
    io.restoreVersionAsync(readme, 1, &app, denied);
    const auto deniedBatch = [&](int ok, int failed, const QStringList &names) {
        check(ok == 0 && failed == 1 && names.isEmpty(), "batch write reports one failure and no success");
        ++deniedCallbacks;
    };
    io.batchCopyAsync({readme}, QStringLiteral("/"), &app, deniedBatch);
    io.batchMoveAsync({readme}, QStringLiteral("/"), &app, deniedBatch);
    const QList<QtFileMgrTransferRequest *> deniedTransfers = {
        io.copyRequest(readme, QStringLiteral("/copy"), &requestOwner),
        io.moveRequest(readme, QStringLiteral("/move"), &requestOwner),
        io.exportToLocalRequest(readme, QStringLiteral("/tmp/memory-export-denied"), &requestOwner),
        io.importFromLocalRequest(QStringLiteral("/tmp/local-input"), readme, false, &requestOwner)
    };
    for (QtFileMgrTransferRequest *request : deniedTransfers)
        QObject::connect(request, &QtFileMgrTransferRequest::finished, &app,
            [&](bool success, const QString &path, const QString &message, bool) {
                check(!success && path.isEmpty() && !message.isEmpty(), "transfer rejects writes with a useful error");
                ++deniedCallbacks;
            });
    drainEvents();
    check(deniedCallbacks == 10, "all denied mutation callbacks complete once");
    check(io.readPreview(readme, 4096) == original && !io.exists(QStringLiteral("/created")),
          "denied writes preserve the original immutable data");

    QtFileMgrConfig config;
    config.rootPath.clear();
    config.allowedPaths = QStringList{QStringLiteral("memory:///documents")};
    config.places = {{QStringLiteral("Documents"), QStringLiteral("memory:///documents"), QStringLiteral("folder")}};
    config.showDevices = false;
    QtFileMgrWidget widget(config, &io);
    const QUrl documents(QStringLiteral("memory:///documents"));
    widget.setUrl(documents);
    drainEvents();
    check(widget.url() == documents && widget.io() == &io, "widget navigates through the injected memory backend");
    widget.setUrl(QUrl(QStringLiteral("memory:///images")));
    check(widget.url() == documents, "allowedPaths prevents navigation outside the allowed memory subtree");
    widget.setUrl(QUrl(QStringLiteral("file:///documents")));
    check(widget.url() == documents && widget.io() == &io, "local URL cannot silently replace memory navigation");
    widget.setUrl(QUrl(QStringLiteral("memory://host/documents")));
    check(widget.url() == documents, "a foreign memory authority cannot enter widget navigation");

    qInfo("RESULT: memory IO %s (%d checks, %d failures)",
          failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
