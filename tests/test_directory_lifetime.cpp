// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFlatModel.h"
#include "QtFileMgrIO.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QPointer>
#include <functional>
#include <cstdio>

namespace {
int checks = 0;
int failures = 0;
int prematureDestructionWarnings = 0;
QtMessageHandler previousHandler = nullptr;

void messages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains(QStringLiteral("directory request destroyed before finish")))
        ++prematureDestructionWarnings;
    if (previousHandler)
        previousHandler(type, context, message);
    else
        fprintf(stderr, "%s\n", qPrintable(message));
}

void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[directory lifetime test] FAIL: %s", message);
    }
}

void drain()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

FileEntry entry(const QString &path)
{
    FileEntry result;
    result.path = path;
    result.name = path.section(QLatin1Char('/'), -1);
    return result;
}

class DirectoryIO : public QtFileMgrIO {
public:
    mutable QList<QPointer<QtFileMgrDirectoryRequest>> requests;
    mutable QStringList paths;
    std::function<void(QtFileMgrDirectoryRequest *, const QString &)> beforeReturn;

    QString rootPath() const override { return QStringLiteral("/"); }
    QString displayName(const QString &path) const override { return path; }
    bool exists(const QString &) const override { return true; }
    QList<FileEntry> listDir(const QString &) const override { return {}; }
    QtFileMgrDirectoryRequest *listDirAsync(const QString &path, QObject *parent) const override
    {
        auto *request = new QtFileMgrDirectoryRequest(parent);
        requests.append(request);
        paths.append(path);
        if (beforeReturn)
            beforeReturn(request, path);
        return request;
    }
    bool isDir(const QString &) const override { return true; }
    bool isWritable(const QString &) const override { return false; }
    bool canWrite() const override { return false; }
    bool mkdir(const QString &) override { return false; }
    bool rename(const QString &, const QString &) override { return false; }
    bool remove(const QString &) override { return false; }
    bool copy(const QString &, const QString &) override { return false; }
};

struct Results {
    QStringList paths;
    QStringList errors;
    explicit Results(QtFileMgrFlatModel *model)
    {
        QObject::connect(model, &QtFileMgrFlatModel::directoryLoadFinished, model,
                         [this](const QString &path, const QString &error) {
            paths.append(path);
            errors.append(error);
        });
    }
};

void testPrematureDestruction()
{
    DirectoryIO io;
    auto *model = new QtFileMgrFlatModel(&io);
    Results results(model);
    model->setUrl(QStringLiteral("/first"));
    check(model->isLoading(), "first request enters loading state");
    const int warningsBefore = prematureDestructionWarnings;
    delete io.requests.first().data();
    check(!model->isLoading(), "destroyed unfinished request ends loading");
    check(results.paths == QStringList{QStringLiteral("/first")},
          "destroyed request completes its own URL exactly once");
    check(results.errors.size() == 1 && !results.errors.first().isEmpty(),
          "destroyed unfinished request reports a visible error");
    check(prematureDestructionWarnings == warningsBefore + 1,
          "premature request destruction logs contextual warning once");
    model->setUrl(QStringLiteral("/second"));
    check(model->isLoading(), "navigation remains usable after request destruction");
    io.requests.last()->deliverBatch({entry(QStringLiteral("/second/item"))});
    io.requests.last()->finish();
    check(!model->isLoading() && model->rowCount() == 1,
          "subsequent request can complete normally");
    check(results.paths == QStringList{QStringLiteral("/first"), QStringLiteral("/second")}
              && results.errors.last().isEmpty(),
          "subsequent completion carries its own URL and success");
    delete model;
    drain();
    check(prematureDestructionWarnings == warningsBefore + 1,
          "finished requests do not produce false destruction errors");
}

void testCancellation()
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    const int warningsBefore = prematureDestructionWarnings;
    model.setUrl(QStringLiteral("/cancelled"));
    const QPointer<QtFileMgrDirectoryRequest> cancelled = io.requests.first();
    model.setUrl(QStringLiteral("/current"));
    check(cancelled && cancelled->isCancelled(), "new navigation cancels its predecessor");
    delete cancelled.data();
    check(model.isLoading() && results.paths.isEmpty(),
          "destroying a cancelled request neither ends current loading nor emits an error");
    io.requests.last()->finish();
    check(results.paths == QStringList{QStringLiteral("/current")}
              && results.errors == QStringList{QString()},
          "only the current successful request emits completion");
    drain();
    check(prematureDestructionWarnings == warningsBefore,
          "cancelled and finished request destruction is normal lifecycle");
}

void testCompletionResetNavigation()
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    model.setUrl(QStringLiteral("/first"));
    bool redirected = false;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&]() {
        if (!redirected && model.url() == QStringLiteral("/first")) {
            redirected = true;
            model.setUrl(QStringLiteral("/second"));
        }
    });
    io.requests.first()->deliverBatch({entry(QStringLiteral("/first/item"))});
    io.requests.first()->finish(QStringLiteral("old result"));
    check(redirected && io.requests.size() == 2, "completion reset may navigate to a new request");
    check(model.isLoading() && results.paths.isEmpty(),
          "old completion does not finish or mislabel the newly started request");
    io.requests.last()->deliverBatch({entry(QStringLiteral("/second/item"))});
    io.requests.last()->finish();
    check(!model.isLoading() && model.rowCount() == 1,
          "new request completes with its own entries");
    check(results.paths == QStringList{QStringLiteral("/second")}
              && results.errors == QStringList{QString()},
          "reentrant reset emits exactly one accurate final completion");
    check(model.index(0, 0).data(Qt::UserRole).toString() == QStringLiteral("/second/item"),
          "old reset does not overwrite the new directory contents");
}

void testInitialResetNavigation(bool aboutToReset)
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    bool redirected = false;
    const auto redirect = [&]() {
        if (!redirected && model.url() == QStringLiteral("/first")) {
            redirected = true;
            model.setUrl(QStringLiteral("/second"));
        }
    };
    if (aboutToReset)
        QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, redirect);
    else
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, redirect);
    model.setUrl(QStringLiteral("/first"));
    check(redirected && io.paths == QStringList{QStringLiteral("/second")},
          "initial reset navigation starts only the current URL request");
    check(model.isLoading(), "current request remains loading after initial reset reentry");
    io.requests.last()->finish();
    check(results.paths == QStringList{QStringLiteral("/second")},
          "initial reset reentry produces one accurate completion");
}

void testBatchNavigation()
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    model.setUrl(QStringLiteral("/first"));
    QObject::connect(&model, &QtFileMgrFlatModel::directoryBatchLoaded, &model,
                     [&](int, int) {
        if (model.url() == QStringLiteral("/first"))
            model.setUrl(QStringLiteral("/second"));
    });
    const QPointer<QtFileMgrDirectoryRequest> first = io.requests.first();
    first->deliverBatch({entry(QStringLiteral("/first/item"))});
    check(first->isCancelled() && io.paths.last() == QStringLiteral("/second"),
          "batch notification may replace the current request");
    first->finish(QStringLiteral("cancelled"));
    check(model.isLoading() && results.paths.isEmpty(),
          "cancelled batch predecessor does not complete the new request");
    io.requests.last()->deliverBatch({entry(QStringLiteral("/second/item"))});
    io.requests.last()->finish();
    check(results.paths == QStringList{QStringLiteral("/second")} && model.rowCount() == 1,
          "batch reentry retains only new entries and completion");
}

void testLoadingNavigation(bool onLoading)
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    bool redirected = false;
    QObject::connect(&model, &QtFileMgrFlatModel::loadingChanged, &model, [&](bool loading) {
        if (!redirected && loading == onLoading) {
            redirected = true;
            model.setUrl(QStringLiteral("/second"));
        }
    });
    model.setUrl(QStringLiteral("/first"));
    if (!onLoading)
        io.requests.first()->finish(QStringLiteral("old result"));
    check(redirected && model.isLoading() && io.paths.last() == QStringLiteral("/second"),
          "loading notification navigation leaves the current URL loading");
    if (onLoading)
        check(io.paths.size() == 1, "loading(true) reentry prevents issuing a superseded request");
    check(results.paths.isEmpty(), "superseded loading notification does not finish the new URL");
    io.requests.last()->finish();
    check(results.paths == QStringList{QStringLiteral("/second")}
              && results.errors == QStringList{QString()},
          "loading reentry produces one accurate completion");
}

void testBackendReturnNavigation()
{
    DirectoryIO io;
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    io.beforeReturn = [&](QtFileMgrDirectoryRequest *, const QString &path) {
        if (path == QStringLiteral("/first"))
            model.setUrl(QStringLiteral("/second"));
    };
    model.setUrl(QStringLiteral("/first"));
    check(io.paths.size() == 2 && model.isLoading(),
          "listDirAsync may navigate before returning the old request");
    check(io.requests.first() && io.requests.first()->isCancelled(),
          "request returned after replacement is cancelled safely");
    io.requests.first()->finish(QStringLiteral("old result"));
    check(results.paths.isEmpty() && model.isLoading(),
          "old request returned late does not replace active state");
    io.requests.last()->finish();
    check(results.paths == QStringList{QStringLiteral("/second")},
          "request started inside listDirAsync completes normally");
}

void testSynchronousResults(bool finishBeforeReturn)
{
    DirectoryIO io;
    io.beforeReturn = [finishBeforeReturn](QtFileMgrDirectoryRequest *request, const QString &path) {
        request->deliverBatch({entry(path + QStringLiteral("/early"))});
        if (finishBeforeReturn)
            request->finish();
    };
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    int batches = 0;
    QObject::connect(&model, &QtFileMgrFlatModel::directoryBatchLoaded, &model,
                     [&](int, int) { ++batches; });
    model.setUrl(QStringLiteral("/sync"));
    check(model.sourceItemCount() == 1 && batches == 1,
          "batches emitted before listDirAsync return are consumed exactly once");
    if (!finishBeforeReturn) {
        check(model.isLoading() && results.paths.isEmpty(),
              "early batches without finish preserve active loading");
        io.requests.last()->deliverBatch({entry(QStringLiteral("/sync/later"))});
        io.requests.last()->finish();
    }
    check(!model.isLoading() && results.paths == QStringList{QStringLiteral("/sync")},
          "synchronous and partial synchronous requests complete once");
    check(model.rowCount() == (finishBeforeReturn ? 1 : 2),
          "synchronous replay does not lose or duplicate directory entries");
}

void testSynchronousReplayRequestDestruction()
{
    DirectoryIO io;
    io.beforeReturn = [](QtFileMgrDirectoryRequest *request, const QString &path) {
        request->deliverBatch({entry(path + QStringLiteral("/early"))});
    };
    QtFileMgrFlatModel model(&io);
    Results results(&model);
    QObject::connect(&model, &QtFileMgrFlatModel::directoryBatchLoaded, &model,
                     [&](int, int) { delete io.requests.last().data(); });
    model.setUrl(QStringLiteral("/sync-destroy"));
    check(!model.isLoading() && results.paths == QStringList{QStringLiteral("/sync-destroy")}
              && !results.errors.first().isEmpty(),
          "destroying a request during synchronous replay produces one terminal error");
}

void testModelDestruction(const QString &signal)
{
    DirectoryIO io;
    QPointer<QtFileMgrFlatModel> model = new QtFileMgrFlatModel(&io);
    const auto destroy = [&]() { delete model.data(); };
    if (signal == QStringLiteral("completion-reset")) {
        model->setUrl(QStringLiteral("/destroy"));
        QObject::connect(model.data(), &QAbstractItemModel::modelReset, &io, destroy);
        io.requests.last()->finish();
        check(!model, "model destruction during completion reset is safe");
        drain();
        return;
    }
    if (signal == QStringLiteral("batch")) {
        QObject::connect(model.data(), &QtFileMgrFlatModel::directoryBatchLoaded,
                         &io, [destroy](int, int) { destroy(); });
    } else if (signal == QStringLiteral("reset")) {
        QObject::connect(model.data(), &QAbstractItemModel::modelReset, &io, destroy);
    } else if (signal == QStringLiteral("about-reset")) {
        QObject::connect(model.data(), &QAbstractItemModel::modelAboutToBeReset, &io, destroy);
    } else if (signal == QStringLiteral("loading-true") || signal == QStringLiteral("loading-false")) {
        QObject::connect(model.data(), &QtFileMgrFlatModel::loadingChanged, &io,
                         [destroy, signal](bool loading) {
            if (loading == (signal == QStringLiteral("loading-true")))
                destroy();
        });
    } else if (signal == QStringLiteral("finish")) {
        QObject::connect(model.data(), &QtFileMgrFlatModel::directoryLoadFinished,
                         &io, [destroy](const QString &, const QString &) { destroy(); });
    } else if (signal == QStringLiteral("backend-return")) {
        io.beforeReturn = [destroy](QtFileMgrDirectoryRequest *, const QString &) { destroy(); };
    }
    model->setUrl(QStringLiteral("/destroy"));
    if (model && signal == QStringLiteral("batch"))
        io.requests.last()->deliverBatch({entry(QStringLiteral("/destroy/item"))});
    else if (model)
        io.requests.last()->finish();
    check(!model, qPrintable(QStringLiteral("model destruction during %1 is safe").arg(signal)));
    drain();
}
} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    previousHandler = qInstallMessageHandler(messages);
    testPrematureDestruction();
    testCancellation();
    testCompletionResetNavigation();
    testInitialResetNavigation(false);
    testInitialResetNavigation(true);
    testBatchNavigation();
    testLoadingNavigation(true);
    testLoadingNavigation(false);
    testBackendReturnNavigation();
    testSynchronousResults(false);
    testSynchronousResults(true);
    testSynchronousReplayRequestDestruction();
    for (const QString &signal : {QStringLiteral("batch"), QStringLiteral("reset"),
                                 QStringLiteral("about-reset"), QStringLiteral("completion-reset"),
                                 QStringLiteral("loading-true"),
                                 QStringLiteral("loading-false"), QStringLiteral("finish"),
                                 QStringLiteral("backend-return")})
        testModelDestruction(signal);
    qInfo("[directory lifetime test] checks=%d failures=%d", checks, failures);
    qInstallMessageHandler(previousHandler);
    return failures == 0 ? 0 : 1;
}
