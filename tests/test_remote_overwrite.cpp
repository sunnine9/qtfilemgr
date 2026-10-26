// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFileOperationJob.h"
#include "QtFileMgrIO.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QSharedPointer>
#include <QTextStream>
#include <QTimer>

#include <functional>

namespace {

const QString source = QStringLiteral("/source/report.txt");
const QString target = QStringLiteral("/dest/report.txt");
const QByteArray oldContent("old target contents");
const QByteArray newContent("new source contents");
const QByteArray rivalContent("later external writer");
int failures = 0;

void check(bool condition, const QString &message)
{
    QTextStream stream(stdout);
    stream << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    stream.flush();
    if (!condition)
        ++failures;
}

struct Node {
    QByteArray bytes;
    int identity = 0;
};

struct Store {
    QHash<QString, Node> nodes;
    QString staging;
    QString backup;
    QStringList phases;
    int nextIdentity = 0;
    int unsafeMutations = 0;
    int cleanups = 0;

    void put(const QString &path, const QByteArray &bytes)
    {
        Node node;
        node.bytes = bytes;
        node.identity = ++nextIdentity;
        nodes.insert(path, node);
    }
    QByteArray bytes(const QString &path) const { return nodes.value(path).bytes; }
    QByteArray token(const QString &path) const
    {
        if (!nodes.contains(path))
            return {};
        const Node node = nodes.value(path);
        return QByteArray::number(node.identity) + '\n' + node.bytes;
    }
};

class FaultBackend final : public QtFileMgrIO {
public:
    explicit FaultBackend(const QSharedPointer<Store> &store) : state(store) {}

    QSharedPointer<Store> state;
    bool synchronous = false;
    bool duplicateFinish = false;
    bool destroyRequest = false;
    bool transferFails = false;
    bool snapshotsSupported = true;
    bool noReplaceSupported = true;
    bool cleanupFails = false;
    QString failPhase;
    bool failAfterEffect = false;
    bool rollbackFails = false;
    std::function<void(const QString &)> beforeRename;
    std::function<void(const QString &)> afterRename;
    std::function<void()> afterTransfer;
    std::function<void()> beforeCleanup;

    QString rootPath() const override { return QStringLiteral("/"); }
    QString displayName(const QString &path) const override { return path.section('/', -1); }
    bool exists(const QString &path) const override { return state->nodes.contains(path); }
    bool isDir(const QString &path) const override { return path == QStringLiteral("/dest"); }
    QList<FileEntry> listDir(const QString &) const override { return {}; }
    bool isWritable(const QString &) const override { return true; }
    bool canWrite() const override { return true; }
    bool isRemote() const override { return true; }
    bool mkdir(const QString &path) override
    {
        qWarning("[FaultBackend] unsupported mkdir path=\"%s\"", qPrintable(path));
        return false;
    }
    bool rename(const QString &from, const QString &to) override
    {
        ++state->unsafeMutations;
        qWarning("[FaultBackend] unsafe rename rejected source=\"%s\" destination=\"%s\"",
                 qPrintable(from), qPrintable(to));
        return false;
    }
    bool remove(const QString &path) override
    {
        ++state->unsafeMutations;
        qWarning("[FaultBackend] unsafe remove rejected path=\"%s\"", qPrintable(path));
        return false;
    }
    bool removePermanent(const QString &path) override { return remove(path); }
    bool copy(const QString &from, const QString &to) override
    {
        qWarning("[FaultBackend] synchronous copy rejected source=\"%s\" destination=\"%s\"",
                 qPrintable(from), qPrintable(to));
        return false;
    }

    QtFileMgrTransferRequest *copyRequest(const QString &from, const QString &to,
                                         QObject *parent = nullptr) override
    {
        return transfer(from, to, false, parent);
    }
    QtFileMgrTransferRequest *moveRequest(const QString &from, const QString &to,
                                         QObject *parent = nullptr) override
    {
        return transfer(from, to, true, parent);
    }
    QtFileMgrTransferRequest *importFromLocalRequest(
        const QString &from, const QString &to, bool move, QObject *parent = nullptr) override
    {
        return transfer(from, to, move, parent);
    }

    bool renameNoReplace(const QString &from, const QString &to, QString *error) override
    {
        const auto store = state;
        const QString phase = from == target ? QStringLiteral("backup")
            : from == store->staging ? QStringLiteral("commit") : QStringLiteral("rollback");
        if (phase == QStringLiteral("backup"))
            store->backup = to;
        store->phases.append(phase);
        const QPointer<FaultBackend> guard(this);
        const auto before = beforeRename;
        if (before)
            before(phase);
        if (!guard) {
            if (error) *error = QStringLiteral("backend destroyed in rename");
            return false;
        }
        const bool injectedFailure = !noReplaceSupported || phase == failPhase
            || (phase == QStringLiteral("rollback") && rollbackFails);
        if ((injectedFailure && !failAfterEffect) || store->nodes.contains(to)
            || !store->nodes.contains(from)) {
            if (error) *error = QStringLiteral("injected failure or destination occupied");
            qWarning("[FaultBackend] no-replace rejected phase=%s source=\"%s\" destination=\"%s\"",
                     qPrintable(phase), qPrintable(from), qPrintable(to));
            return false;
        }
        store->nodes.insert(to, store->nodes.take(from));
        const auto after = afterRename;
        if (after)
            after(phase);
        if (!guard) {
            if (error) *error = QStringLiteral("backend destroyed after rename side effect");
            return false;
        }
        if (injectedFailure && error)
            *error = QStringLiteral("reply lost after rename side effect");
        return !injectedFailure;
    }

    QByteArray undoSnapshot(const QString &path, QString *error) const override
    {
        if (!snapshotsSupported) {
            if (error) *error = QStringLiteral("strong revision unavailable");
            qWarning("[FaultBackend] snapshot unsupported path=\"%s\"", qPrintable(path));
            return {};
        }
        return state->token(path);
    }
    bool undoMoveIfUnchanged(const QString &from, const QString &to,
                             const QByteArray &token, QString *error,
                             QByteArray * = nullptr) override
    {
        if (token.isEmpty() || state->token(from) != token) {
            if (error) *error = QStringLiteral("backup revision changed");
            qWarning("[FaultBackend] restore refused changed backup path=\"%s\"", qPrintable(from));
            return false;
        }
        return renameNoReplace(from, to, error);
    }
    bool undoCopyIfUnchanged(const QString &path, const QByteArray &token,
                             QString *error, QByteArray * = nullptr) override
    {
        const auto store = state;
        const QPointer<FaultBackend> guard(this);
        const auto hook = beforeCleanup;
        if (hook)
            hook();
        if (!guard) {
            if (error) *error = QStringLiteral("backend destroyed during cleanup");
            return false;
        }
        ++store->cleanups;
        if (cleanupFails || token.isEmpty() || store->token(path) != token) {
            if (error) *error = QStringLiteral("backup cleanup denied or revision changed");
            qWarning("[FaultBackend] guarded cleanup refused path=\"%s\"", qPrintable(path));
            return false;
        }
        store->nodes.remove(path);
        return true;
    }

private:
    QtFileMgrTransferRequest *transfer(const QString &from, const QString &to,
                                      bool move, QObject *parent)
    {
        auto *request = new QtFileMgrTransferRequest(parent);
        state->staging = to;
        const auto store = state;
        const bool fail = transferFails;
        const bool duplicate = duplicateFinish;
        const bool destroy = destroyRequest;
        const auto hook = afterTransfer;
        const auto finish = [store, request, from, to, move, fail, duplicate, destroy, hook]() {
            if (destroy) {
                delete request;
                return;
            }
            const bool okay = !fail && store->nodes.contains(from) && !store->nodes.contains(to);
            if (okay) {
                Node value = store->nodes.value(from);
                if (move)
                    store->nodes.remove(from);
                else
                    value.identity = ++store->nextIdentity;
                store->nodes.insert(to, value);
            }
            request->finish(okay, to, okay ? QString() : QStringLiteral("injected transfer failure"));
            if (hook)
                hook();
            if (duplicate)
                emit request->finished(okay, to, QString(), false);
        };
        if (synchronous)
            finish();
        else
            QTimer::singleShot(0, request, finish);
        return request;
    }
};

struct Outcome {
    bool done = false;
    int callbacks = 0;
    int succeeded = 0;
    int failed = 0;
    bool cancelled = false;
    QStringList names;
    QStringList errors;
    int completedItems = 0;
};

using Setup = std::function<void(FaultBackend *, QtFileMgrFileOperationJob *,
                                 const QSharedPointer<Store> &)>;

Outcome run(const QSharedPointer<Store> &store, const Setup &setup = {},
            QtFileMgrFileOperationJob::Operation operation = QtFileMgrFileOperationJob::Copy)
{
    store->put(source, newContent);
    store->put(target, oldContent);
    QPointer<FaultBackend> backend = new FaultBackend(store);
    QPointer<QtFileMgrFileOperationJob> job = new QtFileMgrFileOperationJob(
        backend, operation, {source}, QStringLiteral("/dest"),
        QtFileMgrFileOperationJob::OverwriteExisting);
    Outcome result;
    QObject context;
    QObject::connect(job, &QtFileMgrFileOperationJob::finished, &context,
        [&](int succeeded, int failed, int, const QStringList &names, bool cancelled,
            const QStringList &errors) {
        result.done = true;
        ++result.callbacks;
        result.succeeded = succeeded;
        result.failed = failed;
        result.cancelled = cancelled;
        result.names = names;
        result.errors = errors;
    });
    QObject::connect(job, &QtFileMgrFileOperationJob::itemCompleted, &context,
        [&](const QString &, const QString &, const QByteArray &) { ++result.completedItems; });
    if (setup)
        setup(backend, job, store);
    job->start();
    QElapsedTimer clock;
    clock.start();
    while (!result.done && job && clock.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    delete job;
    delete backend;
    check(result.callbacks <= 1 && store->unsafeMutations == 0,
          QStringLiteral("completion occurs at most once and never invokes unguarded mutation"));
    return result;
}

bool failedOnce(const Outcome &outcome)
{
    return outcome.done && outcome.callbacks == 1 && outcome.failed == 1 && outcome.succeeded == 0;
}
bool recoveryReported(const Outcome &outcome, const QSharedPointer<Store> &store)
{
    const QString errors = outcome.errors.join(QLatin1Char('\n'));
    return errors.contains(source) && errors.contains(target)
        && errors.contains(store->staging) && errors.contains(store->backup);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store);
        check(result.done && result.succeeded == 1 && result.failed == 0
                  && result.errors.isEmpty() && result.names == QStringList{QStringLiteral("report.txt")}
                  && result.completedItems == 1 && store->bytes(target) == newContent
                  && store->bytes(source) == newContent && !store->nodes.contains(store->backup)
                  && !store->nodes.contains(store->staging) && store->cleanups == 1,
              QStringLiteral("successful overwrite commits new bytes and safely cleans its unchanged backup"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->failPhase = QStringLiteral("backup");
        });
        check(failedOnce(result) && store->bytes(target) == oldContent
                  && store->bytes(store->staging) == newContent && store->phases.size() == 1
                  && recoveryReported(result, store),
              QStringLiteral("backup failure leaves old target and new staging intact without retry"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->failPhase = QStringLiteral("commit");
        });
        check(failedOnce(result) && store->bytes(target) == oldContent
                  && store->bytes(store->staging) == newContent && !store->nodes.contains(store->backup)
                  && recoveryReported(result, store),
              QStringLiteral("commit failure restores old target and retains the full new staging content"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->failPhase = QStringLiteral("commit");
            backend->rollbackFails = true;
        });
        check(failedOnce(result) && !store->nodes.contains(target)
                  && store->bytes(store->backup) == oldContent && store->bytes(store->staging) == newContent
                  && recoveryReported(result, store),
              QStringLiteral("failed rollback retains both original backup and complete staging with recovery paths"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->cleanupFails = true;
        });
        check(result.done && result.succeeded == 1 && result.failed == 0 && result.completedItems == 1
                  && store->bytes(target) == newContent && store->bytes(store->backup) == oldContent
                  && recoveryReported(result, store),
              QStringLiteral("backup cleanup failure remains successful with an explicit recovery warning"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &state) {
            backend->beforeRename = [state](const QString &phase) {
                if (phase == QStringLiteral("commit"))
                    state->put(target, rivalContent);
            };
        });
        check(failedOnce(result) && store->bytes(target) == rivalContent
                  && store->bytes(store->backup) == oldContent && store->bytes(store->staging) == newContent
                  && recoveryReported(result, store),
              QStringLiteral("commit and rollback never replace a later competing target"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &state) {
            backend->beforeCleanup = [state]() { state->put(state->backup, rivalContent); };
        });
        check(result.done && result.succeeded == 1 && result.failed == 0
                  && store->bytes(target) == newContent && store->bytes(store->backup) == rivalContent
                  && recoveryReported(result, store),
              QStringLiteral("guarded cleanup preserves a backup replaced by another actor"));
    }
    for (const QString &phase : {QStringLiteral("staged"), QStringLiteral("backup"), QStringLiteral("commit")}) {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [phase](FaultBackend *backend, QtFileMgrFileOperationJob *job, const QSharedPointer<Store> &) {
            if (phase == QStringLiteral("staged"))
                backend->afterTransfer = [job]() { job->cancel(); };
            else
                backend->afterRename = [job, phase](const QString &current) {
                    if (current == phase)
                        job->cancel();
                };
        });
        const bool committed = phase == QStringLiteral("commit");
        check(result.done && result.cancelled && result.succeeded == (committed ? 1 : 0)
                  && result.failed == (committed ? 0 : 1)
                  && store->bytes(target) == (committed ? newContent : oldContent)
                  && (committed ? store->bytes(store->backup) == oldContent
                                : store->bytes(store->staging) == newContent),
              QStringLiteral("cancellation has an explicit preservation boundary at phase %1").arg(phase));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->failPhase = QStringLiteral("commit");
        }, QtFileMgrFileOperationJob::Move);
        check(failedOnce(result) && !store->nodes.contains(source)
                  && store->bytes(target) == oldContent && store->bytes(store->staging) == newContent
                  && recoveryReported(result, store),
              QStringLiteral("failed overwrite move retains the only complete new copy at its reported staging path"));
    }
    for (const bool synchronous : {false, true}) {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [synchronous](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->synchronous = synchronous;
            backend->duplicateFinish = true;
        });
        check(result.done && result.succeeded == 1 && result.callbacks == 1 && result.completedItems == 1
                  && store->phases == QStringList{QStringLiteral("backup"), QStringLiteral("commit")},
              QStringLiteral("synchronous=%1 and duplicate transfer finish commit once").arg(synchronous));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->destroyRequest = true;
        });
        check(failedOnce(result) && store->bytes(target) == oldContent && store->phases.isEmpty(),
              QStringLiteral("request destruction fails explicitly before any target backup"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->afterRename = [backend](const QString &phase) {
                if (phase == QStringLiteral("backup"))
                    delete backend;
            };
        });
        check(failedOnce(result) && store->bytes(store->backup) == oldContent
                  && store->bytes(store->staging) == newContent && recoveryReported(result, store),
              QStringLiteral("backend destruction after backup preserves both recovery nodes without dereferencing it"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *job, const QSharedPointer<Store> &) {
            backend->afterRename = [job](const QString &phase) {
                if (phase == QStringLiteral("backup"))
                    delete job;
            };
        });
        check(!result.done && store->bytes(store->backup) == oldContent
                  && store->bytes(store->staging) == newContent,
              QStringLiteral("job destruction during backup leaves recovery nodes intact"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->snapshotsSupported = false;
        });
        check(result.done && result.succeeded == 1 && result.failed == 0
                  && store->bytes(target) == newContent && store->bytes(store->backup) == oldContent
                  && store->cleanups == 0 && recoveryReported(result, store),
              QStringLiteral("a backend without revision proof retains backup and reports successful commit with warning"));
    }
    for (const QString &phase : {QStringLiteral("backup"), QStringLiteral("commit")}) {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [phase](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->failPhase = phase;
            backend->failAfterEffect = true;
        });
        check(failedOnce(result) && store->bytes(store->backup) == oldContent
                  && (phase == QStringLiteral("backup") ? store->bytes(store->staging) == newContent
                                                        : store->bytes(target) == newContent)
                  && recoveryReported(result, store),
              QStringLiteral("lost %1 reply never causes destructive retry or deletion of the last copy").arg(phase));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->transferFails = true;
        });
        check(failedOnce(result) && store->bytes(target) == oldContent && store->phases.isEmpty(),
              QStringLiteral("failed staging transfer never starts target backup"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *job, const QSharedPointer<Store> &) {
            backend->beforeCleanup = [job]() { job->cancel(); };
        });
        check(result.done && result.succeeded == 1 && result.failed == 0 && result.cancelled
                  && store->bytes(target) == newContent && !result.errors.isEmpty(),
              QStringLiteral("cancellation during post-commit cleanup never reclassifies an installed file as failed"));
    }
    {
        const auto store = QSharedPointer<Store>::create();
        const Outcome result = run(store, [](FaultBackend *backend, QtFileMgrFileOperationJob *, const QSharedPointer<Store> &) {
            backend->noReplaceSupported = false;
        });
        check(failedOnce(result) && store->bytes(target) == oldContent
                  && store->bytes(store->staging) == newContent,
              QStringLiteral("unsupported no-replace primitive fails closed without deleting the original"));
    }

    QTextStream(stdout) << "RESULT: " << (failures ? "FAIL" : "PASS")
                        << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
