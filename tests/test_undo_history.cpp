// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFileOperationJob.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrLocalIO.h"

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

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[undo history test] FAIL: %s", message);
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
bool transfer(QtFileMgrFileView &view, const QStringList &sources,
              const QString &destination, bool move)
{
    view.setUrl(QUrl::fromLocalFile(destination));
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &source : sources)
        urls.append(QUrl::fromLocalFile(source));
    mime->setUrls(urls);
    if (move)
        mime->setData(QStringLiteral("application/x-kde-cutselection"), "1");
    QApplication::clipboard()->setMimeData(mime);
    view.pasteFiles();
    return waitFor([&]() {
        return !view.hasActiveOperation() && !view.pendingOperationCount() && view.canUndo();
    });
}
bool directories(const QString &base)
{
    for (const char *name : {"a", "b", "c", "d"}) {
        if (!QDir().mkpath(base + QLatin1Char('/') + QString::fromLatin1(name)))
            return false;
    }
    return true;
}
QString object(const QString &base, char directory)
{
    return base + QLatin1Char('/') + QLatin1Char(directory) + QStringLiteral("/item");
}
QString contents(const QString &path, bool directory)
{
    return directory ? path + QStringLiteral("/nested/child.txt") : path;
}

void chain(const QString &base, bool firstMove, bool directory)
{
    check(directories(base), "three-level chain directories prepared");
    const QString a = object(base, 'a');
    const QString b = object(base, 'b');
    const QString c = object(base, 'c');
    const QString d = object(base, 'd');
    check((!directory || QDir().mkpath(a + QStringLiteral("/nested")))
        && write(contents(a, directory), "original"), "three-level chain object prepared");
    QtFileMgrFileView view;
    check(transfer(view, {a}, base + QStringLiteral("/b"), firstMove),
          "first copy or move creates a guarded history record");
    check(transfer(view, {b}, base + QStringLiteral("/c"), true),
          "second move creates a guarded history record");
    check(transfer(view, {c}, base + QStringLiteral("/d"), true),
          "third move creates a guarded history record");
    view.undoLastOperation();
    check(view.canUndo() && !QFileInfo::exists(d) && read(contents(c, directory)) == "original",
          "third operation restores its source and keeps earlier history");
    view.undoLastOperation();
    check(view.canUndo() && !QFileInfo::exists(c) && read(contents(b, directory)) == "original",
          "second operation accepts only the verified own-rename successor proof");
    view.undoLastOperation();
    check(!view.canUndo() && !QFileInfo::exists(b) && read(contents(a, directory)) == "original",
          "all three undo levels work for files and complete directories");
}

void externalChangeBeforeMove(const QString &base, bool directory, int change)
{
    check(directories(base), "external-change directories prepared");
    const QString a = object(base, 'a');
    const QString b = object(base, 'b');
    const QString c = object(base, 'c');
    check((!directory || QDir().mkpath(a + QStringLiteral("/nested")))
        && write(contents(a, directory), "original"), "external-change source prepared");
    QtFileMgrFileView view;
    check(transfer(view, {a}, base + QStringLiteral("/b"), false),
          "copy before external change completed");
    QtFileMgrLocalIO io;
    const QByteArray copiedProof = io.undoSnapshot(b);
    if (change == 0) {
        check(write(contents(b, directory), "external"), "same-size external edit prepared before the later move");
    } else if (change == 1) {
        check(QFile::rename(b, b + QStringLiteral(".original"))
            && write(b, "original"), "same-bytes replacement has a different object identity");
    } else {
        const QFile::Permissions permissions = QFile::permissions(b);
        QThread::msleep(2);
        check(QFile::setPermissions(b, permissions ^ QFileDevice::WriteOwner)
            && QFile::setPermissions(b, permissions), "metadata restored externally but exact ctime revision changed");
    }
    check(!copiedProof.isEmpty() && copiedProof != io.undoSnapshot(b),
          "external change differs from the earlier precise completion proof");
    check(transfer(view, {b}, base + QStringLiteral("/c"), true),
          "later move captures its own new source proof");
    view.undoLastOperation();
    check(view.canUndo() && QFileInfo::exists(b) && !QFileInfo::exists(c),
          "later move safely restores the externally changed object");
    view.undoLastOperation();
    check(view.canUndo() && QFileInfo::exists(b)
        && read(contents(b, directory)) == (change == 0 ? "external" : "original"),
          "earlier undo refuses the external change instead of adopting the new move baseline");
}

class FailOnceIO : public QtFileMgrLocalIO {
public:
    QString failedTarget;
    bool failNext = false;
    bool renameNoReplace(const QString &source, const QString &destination, QString *error) override
    {
        if (failNext && destination == failedTarget) {
            failNext = false;
            qWarning("[undo history test] injected guarded commit failure (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            if (error)
                *error = QStringLiteral("Injected undo commit failure");
            return false;
        }
        return QtFileMgrLocalIO::renameNoReplace(source, destination, error);
    }
};

void retryChain(const QString &base)
{
    check(directories(base) && write(object(base, 'a'), "original"), "retry-chain fixture prepared");
    FailOnceIO io;
    QtFileMgrFileView view(&io);
    check(transfer(view, {object(base, 'a')}, base + QStringLiteral("/b"), false)
        && transfer(view, {object(base, 'b')}, base + QStringLiteral("/c"), true),
          "retry-chain copy and move completed");
    io.failedTarget = object(base, 'b');
    io.failNext = true;
    view.undoLastOperation();
    check(view.canUndo() && read(object(base, 'c')) == "original"
        && !QFileInfo::exists(object(base, 'b')), "failed reverse commit restores its guarded destination");
    view.undoLastOperation();
    check(view.canUndo() && read(object(base, 'b')) == "original", "reverse move can retry with only its own renewed exact proof");
    view.undoLastOperation();
    check(!view.canUndo() && !QFileInfo::exists(object(base, 'b')),
          "retrying the latest move still reconnects the exact earlier copy proof");
}

void partialBatch(const QString &base)
{
    check(directories(base), "partial batch directories prepared");
    const QString a = base + QStringLiteral("/a");
    const QString b = base + QStringLiteral("/b");
    const QString c = base + QStringLiteral("/c");
    check(write(a + QStringLiteral("/a.txt"), "A") && write(a + QStringLiteral("/b.txt"), "B"),
          "partial batch sources prepared");
    QtFileMgrFileView view;
    check(transfer(view, {a + QStringLiteral("/a.txt"), a + QStringLiteral("/b.txt")}, b, false)
        && transfer(view, {b + QStringLiteral("/a.txt"), b + QStringLiteral("/b.txt")}, c, true),
          "partial batch copy and move completed");
    check(write(b + QStringLiteral("/a.txt"), "occupied"), "one reverse-move source is occupied");
    view.undoLastOperation();
    check(view.canUndo() && read(b + QStringLiteral("/b.txt")) == "B"
        && read(c + QStringLiteral("/a.txt")) == "A", "partial undo remembers its successful item and lineage");
    check(write(c + QStringLiteral("/b.txt"), "external-B") && QFile::remove(b + QStringLiteral("/a.txt")),
          "done-item destination is reused and blocked item becomes available");
    view.undoLastOperation();
    check(view.canUndo() && read(b + QStringLiteral("/a.txt")) == "A"
        && read(c + QStringLiteral("/b.txt")) == "external-B", "retry skips completed item without touching its new destination occupant");
    view.undoLastOperation();
    check(!view.canUndo() && !QFileInfo::exists(b + QStringLiteral("/a.txt"))
        && !QFileInfo::exists(b + QStringLiteral("/b.txt"))
        && read(c + QStringLiteral("/b.txt")) == "external-B",
          "both proven predecessor items undo after partial completion and retry");
}

class EditAfterRestoreIO : public QtFileMgrLocalIO {
public:
    QString target;
    bool inject = false;
    bool renameNoReplace(const QString &source, const QString &destination, QString *error) override
    {
        const bool renamed = QtFileMgrLocalIO::renameNoReplace(source, destination, error);
        if (renamed && inject && destination == target) {
            inject = false;
            qWarning("[undo history test] injected external edit after successful reverse rename (path=\"%s\")",
                     qPrintable(destination));
            check(write(destination, "external"), "external edit races after reverse rename but before final proof");
        }
        return renamed;
    }
};

void changedAfterSuccessfulRestore(const QString &base)
{
    check(directories(base) && write(object(base, 'a'), "original"), "post-restore edit fixture prepared");
    EditAfterRestoreIO io;
    QtFileMgrFileView view(&io);
    check(transfer(view, {object(base, 'a')}, base + QStringLiteral("/b"), false)
        && transfer(view, {object(base, 'b')}, base + QStringLiteral("/c"), true),
          "post-restore edit copy and move completed");
    io.target = object(base, 'b');
    io.inject = true;
    view.undoLastOperation();
    check(view.canUndo() && !QFileInfo::exists(object(base, 'c'))
        && read(object(base, 'b')) == "external", "physical successful undo completes even if its final proof is unavailable");
    view.undoLastOperation();
    check(view.canUndo() && read(object(base, 'b')) == "external"
        && !QFileInfo::exists(object(base, 'c')), "failed final proof cannot renew earlier history or retry the already restored move");
}

void copiedMoveWithholdsLineage(const QString &base)
{
    check(directories(base) && write(object(base, 'a'), "source")
        && write(object(base, 'b'), "old-target"), "copy/remove replacement-move fixture prepared");
    QtFileMgrLocalIO io;
    QtFileMgrFileOperationJob job(&io, QtFileMgrFileOperationJob::Move,
        {object(base, 'a')}, base + QStringLiteral("/b"), QtFileMgrFileOperationJob::OverwriteExisting);
    int lineages = 0;
    int completed = 0;
    bool finished = false;
    QObject::connect(&job, &QtFileMgrFileOperationJob::itemMoveLineage, &job,
        [&](const QString &, const QString &, const QByteArray &, const QByteArray &) { ++lineages; });
    QObject::connect(&job, &QtFileMgrFileOperationJob::itemCompleted, &job,
        [&](const QString &, const QString &, const QByteArray &) { ++completed; });
    QObject::connect(&job, &QtFileMgrFileOperationJob::finished, &job,
        [&](int, int, int, const QStringList &, bool, const QStringList &) { finished = true; });
    job.start();
    check(waitFor([&]() { return finished; }) && completed == 1 && lineages == 0
        && read(object(base, 'b')) == "source" && !QFileInfo::exists(object(base, 'a')),
          "a completed copied move never claims retained-object lineage");
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
#ifndef Q_OS_LINUX
    qWarning("[undo history test] skipped on unsupported platform: guarded exact local proofs require Linux");
    return 0;
#else
    QTemporaryDir fixture;
    check(fixture.isValid(), "isolated undo-history fixture exists");
    if (!fixture.isValid())
        return 1;
    for (bool directory : {false, true}) {
        for (bool firstMove : {false, true}) {
            chain(fixture.path() + QStringLiteral("/chain-%1-%2").arg(directory).arg(firstMove),
                  firstMove, directory);
        }
    }
    externalChangeBeforeMove(fixture.path() + QStringLiteral("/edited-file"), false, 0);
    externalChangeBeforeMove(fixture.path() + QStringLiteral("/edited-tree"), true, 0);
    externalChangeBeforeMove(fixture.path() + QStringLiteral("/replaced-file"), false, 1);
    externalChangeBeforeMove(fixture.path() + QStringLiteral("/ctime-only-change"), false, 2);
    retryChain(fixture.path() + QStringLiteral("/retry-chain"));
    partialBatch(fixture.path() + QStringLiteral("/partial-batch"));
    changedAfterSuccessfulRestore(fixture.path() + QStringLiteral("/post-restore-edit"));
    copiedMoveWithholdsLineage(fixture.path() + QStringLiteral("/copied-move"));
    qInfo("[undo history test] checks=%d failures=%d", checks, failures);
    return failures ? 1 : 0;
#endif
}
