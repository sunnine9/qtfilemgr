// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrIO.h"
#include "QtFileMgrLocalIO.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrFileOperationJob.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QThread>
#include <QDebug>
#include <functional>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
int checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[undo guard test] FAIL: %s", message);
    }
}
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (condition())
            return true;
        QThread::msleep(1);
    } while (timer.elapsed() < 5000);
    return false;
}

class FailOnceIO : public QtFileMgrLocalIO {
public:
    QString failedMoveTarget;
    bool failMove = false;
    bool failRemove = false;
    bool renameNoReplace(const QString &from, const QString &to, QString *error) override
    {
        if (failMove && to == failedMoveTarget) {
            failMove = false;
            qWarning("[undo guard test] injected no-replace failure (source=\"%s\", destination=\"%s\")",
                     qPrintable(from), qPrintable(to));
            if (error) *error = QStringLiteral("Injected commit failure.");
            return false;
        }
        return QtFileMgrLocalIO::renameNoReplace(from, to, error);
    }
    bool remove(const QString &path) override
    {
        if (failRemove) {
            failRemove = false;
            qWarning("[undo guard test] injected guarded remove failure (path=\"%s\")", qPrintable(path));
            return false;
        }
        return QtFileMgrLocalIO::remove(path);
    }
};

class ReentrantIO : public QtFileMgrLocalIO {
public:
    QtFileMgrFileView *view = nullptr;
    QtFileMgrIO *replacement = nullptr;
    int calls = 0;
    bool undoCopyIfUnchanged(const QString &path, const QByteArray &proof,
                              QString *error, QByteArray *refreshed) override
    {
        ++calls;
        view->setIO(replacement);
        view->undoLastOperation();
        return QtFileMgrLocalIO::undoCopyIfUnchanged(path, proof, error, refreshed);
    }
};
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
#ifndef Q_OS_LINUX
    qInfo("[undo guard test] Linux identity/revision contract is intentionally unavailable here.");
    return 0;
#else
    QTemporaryDir fixture;
    check(fixture.isValid(), "temporary fixture exists");
    if (!fixture.isValid()) return 1;
    QDir root(fixture.path());
    QtFileMgrLocalIO io;
    QString error;

    const QString ordinary = root.filePath(QStringLiteral("ordinary.txt"));
    check(write(ordinary, "original"), "normal undo fixture written");
    QByteArray token = io.undoSnapshot(ordinary, &error);
    check(!token.isEmpty() && io.undoCopyIfUnchanged(ordinary, token, &error)
        && !QFileInfo::exists(ordinary), "unchanged copied file can be safely undone");

    const QString edited = root.filePath(QStringLiteral("edited.txt"));
    check(write(edited, "old-data"), "in-place edit fixture written");
    token = io.undoSnapshot(edited, &error);
    check(!token.isEmpty() && write(edited, "new-data"), "same-inode same-size edit performed");
    check(!io.undoCopyIfUnchanged(edited, token, &error) && read(edited) == "new-data",
          "copy undo preserves an in-place edited object");
    check(io.QtFileMgrIO::undoSnapshot(edited, &error).isEmpty()
        && !io.QtFileMgrIO::undoCopyIfUnchanged(edited, token, &error)
        && !io.QtFileMgrIO::undoMoveIfUnchanged(edited, ordinary, token, &error)
        && read(edited) == "new-data", "the default custom-backend contract refuses unprovable undo without mutating paths");
    const QString replaced = root.filePath(QStringLiteral("replaced.txt"));
    check(write(replaced, "same-data"), "replacement fixture written");
    token = io.undoSnapshot(replaced, &error);
    check(QFile::rename(replaced, replaced + QStringLiteral(".original")) && write(replaced, "same-data"),
          "different object created with exactly the old bytes");
    check(!io.undoCopyIfUnchanged(replaced, token, &error) && read(replaced) == "same-data",
          "copy undo rejects same-path replacement with identical contents");

    for (int change = 0; change < 3; ++change) {
        const QString directory = root.filePath(QStringLiteral("tree-%1").arg(change));
        check(QDir().mkpath(directory + QStringLiteral("/nested")), "nested tree prepared");
        const QString child = directory + QStringLiteral("/nested/child.txt");
        check(write(child, "before"), "nested file prepared");
        token = io.undoSnapshot(directory, &error);
        check(!token.isEmpty(), "small complete tree has a revision proof");
        if (change == 0) check(write(directory + QStringLiteral("/nested/added.txt"), "added"), "nested entry added");
        if (change == 1) check(write(child, "after!"), "nested entry edited in-place");
        if (change == 2) check(QFile::remove(child), "nested entry removed");
        check(!io.undoCopyIfUnchanged(directory, token, &error) && QFileInfo::exists(directory),
              "directory undo preserves a tree with any added, edited, or removed descendant");
    }
    const QString normalTree = root.filePath(QStringLiteral("normal-tree"));
    check(QDir().mkpath(normalTree + QStringLiteral("/nested"))
        && write(normalTree + QStringLiteral("/nested/file"), "unchanged"), "normal directory prepared");
    token = io.undoSnapshot(normalTree, &error);
    check(!token.isEmpty() && io.undoCopyIfUnchanged(normalTree, token, &error)
        && !QFileInfo::exists(normalTree), "unchanged complete directory undo succeeds");

    const QString target = root.filePath(QStringLiteral("link-target"));
    const QString link = root.filePath(QStringLiteral("link"));
    check(write(target, "outside") && ::symlink(QFile::encodeName(target).constData(),
                                               QFile::encodeName(link).constData()) == 0,
          "symbolic-link fixture prepared");
    token = io.undoSnapshot(link, &error);
    check(QFile::rename(link, link + QStringLiteral(".original"))
        && ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0,
          "same-target symlink replaced with another link object");
    check(!io.undoCopyIfUnchanged(link, token, &error) && read(target) == "outside"
        && QFileInfo(link).isSymLink(), "replaced symlink and its target survive undo");
    token = io.undoSnapshot(link, &error);
    check(io.undoCopyIfUnchanged(link, token, &error) && read(target) == "outside",
          "normal symlink undo deletes only the link node");

    const QString moved = root.filePath(QStringLiteral("moved.txt"));
    const QString original = root.filePath(QStringLiteral("source.txt"));
    check(write(moved, "moved-object"), "move undo fixture prepared");
    token = io.undoSnapshot(moved, &error);
    check(write(original, "later-source"), "a later object occupies the original source");
    check(!io.undoMoveIfUnchanged(moved, original, token, &error)
        && read(original) == "later-source" && read(moved) == "moved-object",
          "move undo never overwrites a newly occupied source path");
    check(QFile::remove(original) && io.undoMoveIfUnchanged(moved, original, token, &error)
        && read(original) == "moved-object", "move undo retries successfully after an actual source collision is resolved");

    FailOnceIO failOnce;
    const QString retryCopy = root.filePath(QStringLiteral("retry-copy"));
    check(write(retryCopy, "retry-copy"), "copy retry fixture prepared");
    token = failOnce.undoSnapshot(retryCopy, &error);
    failOnce.failRemove = true;
    check(!failOnce.undoCopyIfUnchanged(retryCopy, token, &error, &token)
        && read(retryCopy) == "retry-copy", "failed guarded removal restores unchanged data and returns verified retry proof");
    check(failOnce.undoCopyIfUnchanged(retryCopy, token, &error, &token), "copy retry succeeds after its own restore changed ctime");
    const QString retryMove = root.filePath(QStringLiteral("retry-move"));
    const QString retrySource = root.filePath(QStringLiteral("retry-source"));
    check(write(retryMove, "retry-move"), "move retry fixture prepared");
    token = failOnce.undoSnapshot(retryMove, &error);
    failOnce.failedMoveTarget = retrySource;
    failOnce.failMove = true;
    check(!failOnce.undoMoveIfUnchanged(retryMove, retrySource, token, &error, &token)
        && read(retryMove) == "retry-move", "failed no-replace commit restores and verifies the same moved object");
    check(failOnce.undoMoveIfUnchanged(retryMove, retrySource, token, &error, &token)
        && read(retrySource) == "retry-move", "move retry succeeds after verified own quarantine/restore");

    const QString large = root.filePath(QStringLiteral("larger-than-limit"));
    QFile largeFile(large);
    check(largeFile.open(QIODevice::WriteOnly) && largeFile.resize(8 * 1024 * 1024 + 1), "oversized file prepared");
    largeFile.close();
    check(io.undoSnapshot(large, &error).isEmpty() && !error.isEmpty()
        && !io.undoCopyIfUnchanged(large, {}, &error) && QFileInfo::exists(large),
          "partial or oversized proofs cannot authorize deletion");
    const QString rawDirectory = root.filePath(QStringLiteral("raw-name-tree"));
    check(QDir().mkpath(rawDirectory), "raw filename directory prepared");
    const QByteArray rawPath = QFile::encodeName(rawDirectory) + '/' + QByteArray(1, char(0xff));
    const int rawFd = ::open(rawPath.constData(), O_CREAT | O_WRONLY | O_EXCL, 0600);
    check(rawFd >= 0, "unrepresentable Unix filename created");
    if (rawFd >= 0) ::close(rawFd);
    check(write(rawDirectory + QLatin1Char('/') + QString(QChar::ReplacementCharacter), "different"),
          "a distinct valid replacement-character filename also exists");
    check(io.undoSnapshot(rawDirectory, &error).isEmpty(), "filename decoding collisions fail closed instead of omitting an object");

    // Freeze the GUI until the worker has finished, then edit the destination
    // before queued itemCompleted/finished delivery. The baseline must come
    // from execution time, not the later GUI callback.
    const QString baselineSource = root.filePath(QStringLiteral("baseline.txt"));
    const QString baselineDirectory = root.filePath(QStringLiteral("baseline-destination"));
    const QString baselineDestination = baselineDirectory + QStringLiteral("/baseline.txt");
    check(write(baselineSource, "before") && QDir().mkpath(baselineDirectory),
          "completion-time proof fixture prepared");
    QtFileMgrFileOperationJob baselineJob(&io, QtFileMgrFileOperationJob::Copy,
        {baselineSource}, baselineDirectory, QtFileMgrFileOperationJob::AutoRename);
    QByteArray baselineProof;
    bool baselineFinished = false;
    QObject::connect(&baselineJob, &QtFileMgrFileOperationJob::itemCompleted, &baselineJob,
        [&](const QString &, const QString &, const QByteArray &proof) { baselineProof = proof; });
    QObject::connect(&baselineJob, &QtFileMgrFileOperationJob::finished, &baselineJob,
        [&](int, int, int, const QStringList &, bool, const QStringList &) { baselineFinished = true; });
    baselineJob.start();
    QThread *executionThread = baselineJob.findChild<QThread *>();
    check(executionThread && executionThread->wait(5000) && baselineProof.isEmpty(),
          "worker captures its proof while GUI completion delivery is pending");
    check(write(baselineDestination, "edited") && waitFor([&]() { return baselineFinished; }),
          "destination changes after execution but before GUI handles completion");
    check(!baselineProof.isEmpty()
        && !io.undoCopyIfUnchanged(baselineDestination, baselineProof, &error)
        && read(baselineDestination) == "edited",
          "delayed GUI delivery cannot adopt a later external edit as the undo baseline");

    // Two successful move items: the second item is restored first, while the
    // first is blocked. Retry must consult the explicit completed flag, even if
    // the user later changes both filesystem paths belonging to the done item.
    const QString sources = root.filePath(QStringLiteral("batch-sources"));
    const QString destinations = root.filePath(QStringLiteral("batch-destinations"));
    check(QDir().mkpath(sources) && QDir().mkpath(destinations)
        && write(sources + QStringLiteral("/a.txt"), "A")
        && write(sources + QStringLiteral("/b.txt"), "B"), "two-item move fixture prepared");
    QtFileMgrFileView view;
    view.setUrl(QUrl::fromLocalFile(destinations));
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(sources + QStringLiteral("/a.txt")),
                   QUrl::fromLocalFile(sources + QStringLiteral("/b.txt"))});
    mime->setData(QStringLiteral("application/x-kde-cutselection"), "1");
    QApplication::clipboard()->setMimeData(mime);
    view.pasteFiles();
    check(waitFor([&]() { return !view.hasActiveOperation() && !view.pendingOperationCount() && view.canUndo(); }),
          "real two-item move produces guarded undo records");
    check(write(sources + QStringLiteral("/a.txt"), "later-A"), "first source collision prepared");
    view.undoLastOperation();
    check(view.canUndo() && read(sources + QStringLiteral("/b.txt")) == "B"
        && read(destinations + QStringLiteral("/a.txt")) == "A", "partial undo remembers the successfully restored item");
    check(QFile::remove(sources + QStringLiteral("/b.txt"))
        && write(destinations + QStringLiteral("/b.txt"), "external-B")
        && QFile::remove(sources + QStringLiteral("/a.txt")), "done-item paths changed independently before retry");
    view.undoLastOperation();
    check(!view.canUndo() && read(sources + QStringLiteral("/a.txt")) == "A"
        && read(destinations + QStringLiteral("/b.txt")) == "external-B"
        && !QFileInfo::exists(sources + QStringLiteral("/b.txt")),
          "retry skips only explicit done items and never infers success or mutates later occupants");

    const QString reentrantSource = root.filePath(QStringLiteral("reentrant-source"));
    const QString reentrantDestination = root.filePath(QStringLiteral("reentrant-destination"));
    check(write(reentrantSource, "reentrant") && QDir().mkpath(reentrantDestination), "reentrancy fixture prepared");
    ReentrantIO reentrantIO;
    QtFileMgrLocalIO replacementIO;
    QtFileMgrFileView reentrantView(&reentrantIO);
    reentrantIO.view = &reentrantView;
    reentrantIO.replacement = &replacementIO;
    reentrantView.setUrl(QUrl::fromLocalFile(reentrantDestination));
    auto *copyMime = new QMimeData;
    copyMime->setUrls({QUrl::fromLocalFile(reentrantSource)});
    QApplication::clipboard()->setMimeData(copyMime);
    reentrantView.pasteFiles();
    check(waitFor([&]() { return !reentrantView.hasActiveOperation() && reentrantView.canUndo(); }),
          "custom local backend obtains a real completed-item proof");
    reentrantView.undoLastOperation();
    check(reentrantIO.calls == 1 && reentrantView.io() == &reentrantIO && !reentrantView.canUndo(),
          "backend switch and reentrant undo are refused during a virtual guarded action");

    qInfo("[undo guard test] checks=%d failures=%d", checks, failures);
    return failures ? 1 : 0;
#endif
}
