// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFileView.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrIO.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <functional>

namespace {
int checks = 0;
int failures = 0;
void check(bool value, const char *message)
{
    ++checks;
    if (!value) { ++failures; qCritical("[serial transfer test] FAIL: %s", message); }
}
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool waitFor(const std::function<bool()> &predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
enum class Mode { Sync, Async, Missing, DestroyRequest, DestroyOwner,
                  CancelAfterExport, DeleteOwnerAfterExport };
class Backend : public QtFileMgrIO {
public:
    explicit Backend(QString scheme) : m_scheme(std::move(scheme)) {}
    Mode exportMode = Mode::Sync;
    Mode importMode = Mode::Sync;
    int exports = 0;
    int imports = 0;
    int removals = 0;
    QMap<QString,QByteArray> files;
    QString rootPath() const override { return QStringLiteral("/target"); }
    QString displayName(const QString &path) const override { return path; }
    bool acceptsUrl(const QUrl &url) const override { return url.scheme() == m_scheme; }
    QUrl urlForPath(const QString &path) const override
    { QUrl result; result.setScheme(m_scheme); result.setPath(path); return result; }
    QString pathFromUrl(const QUrl &url) const override { return url.path(); }
    bool exists(const QString &path) const override { return isDir(path) || files.contains(path); }
    bool isDir(const QString &path) const override { return path == QStringLiteral("/") || path == rootPath(); }
    QList<FileEntry> listDir(const QString &) const override { return {}; }
    bool isWritable(const QString &) const override { return true; }
    bool canWrite() const override { return true; }
    bool isRemote() const override { return true; }
    bool mkdir(const QString &) override { return false; }
    bool rename(const QString &, const QString &) override { return false; }
    bool remove(const QString &path) override { ++removals; return files.remove(path) > 0; }
    bool copy(const QString &, const QString &) override { return false; }
    Capabilities capabilities() const override
    { return ReadCapability | WriteCapability | RemoteCapability | LocalExportCapability; }
    QtFileMgrTransferRequest *run(Mode mode, QObject *parent, const QString &destination,
                                  std::function<bool()> action)
    {
        if (mode == Mode::Missing) return nullptr;
        auto *request = new QtFileMgrTransferRequest(parent);
        if (mode == Mode::DestroyRequest) {
            QTimer::singleShot(0, request, [request]() { delete request; });
        } else if (mode == Mode::DestroyOwner) {
            QTimer::singleShot(0, parent, [parent]() { delete parent; });
        } else {
            const auto finish = [request, action, destination, mode, parent]() {
                const bool ok = action();
                if (mode == Mode::CancelAfterExport || mode == Mode::DeleteOwnerAfterExport) {
                    QTimer::singleShot(0, parent, [parent,mode]() {
                        if (mode == Mode::DeleteOwnerAfterExport) delete parent;
                        else if (auto *progress=qobject_cast<QProgressDialog *>(parent)) progress->cancel();
                    });
                }
                request->finish(ok, destination, ok ? QString() : QStringLiteral("fixture failed"));
            };
            if (mode == Mode::Sync || mode == Mode::CancelAfterExport
                || mode == Mode::DeleteOwnerAfterExport) finish();
            else QTimer::singleShot(0, request, finish);
        }
        return request;
    }
    QtFileMgrTransferRequest *exportToLocalRequest(const QString &source, const QString &destination, QObject *parent) override
    {
        ++exports;
        return run(exportMode, parent, destination, [this,source,destination]() {
            return files.contains(source) && write(destination, files.value(source));
        });
    }
    QtFileMgrTransferRequest *importFromLocalRequest(const QString &source, const QString &destination, bool, QObject *parent) override
    {
        ++imports;
        // The second queued import always succeeds; only the relay leg is faulty.
        const Mode mode = destination.endsWith(QStringLiteral("/after.txt")) ? Mode::Sync : importMode;
        return run(mode, parent, destination, [this,source,destination]() {
            QFile file(source);
            if (!file.open(QIODevice::ReadOnly) || files.contains(destination)) return false;
            files.insert(destination, file.readAll());
            return true;
        });
    }
private:
    QString m_scheme;
};
void paste(QtFileMgrFileView &view, const QList<QUrl> &urls, bool cut = false)
{
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    if (cut) mime->setData(QStringLiteral("application/x-kde-cutselection"), "1");
    QApplication::clipboard()->setMimeData(mime);
    view.pasteFiles();
}
void scenario(bool relay, Mode exportMode, Mode importMode, bool cut)
{
    QtFileMgrFileView::clearRemoteBackendRegistry();
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary fixture is valid");
    const QString target = temporary.path() + QStringLiteral("/target");
    check(QDir().mkpath(target), "local destination is created");
    const QString after = temporary.path() + QStringLiteral("/after.txt");
    check(write(after,"queued bytes"), "queued local source is created");
    Backend source(QStringLiteral("serialsource"));
    Backend destination(QStringLiteral("serialdestination"));
    source.files.insert(QStringLiteral("/remote.txt"),"remote bytes");
    source.exportMode = exportMode;
    destination.importMode = importMode;
    QtFileMgrFileView view;
    if (relay) {
        view.setIO(&destination);
        view.setUrl(destination.urlForPath(destination.rootPath()));
    } else {
        view.setUrl(QUrl::fromLocalFile(target));
    }
    check(waitFor([&]() { return !view.flatModel()->isLoading(); }), "target navigation completes");
    QtFileMgrFileView::registerRemoteBackend(&source);
    paste(view,{source.urlForPath(QStringLiteral("/remote.txt"))},cut);
    paste(view,{QUrl::fromLocalFile(after)});
    const QString afterTarget = target + QStringLiteral("/after.txt");
    const bool continued = waitFor([&]() {
        const bool copied = relay ? destination.files.value(QStringLiteral("/target/after.txt")) == "queued bytes"
                                  : read(afterTarget) == "queued bytes";
        return copied && !view.hasActiveOperation() && view.pendingOperationCount() == 0;
    });
    check(continued, "following queued operation completes after the serial batch");
    const bool expectedSuccess = (exportMode == Mode::Sync || exportMode == Mode::Async)
        && (!relay || importMode == Mode::Sync || importMode == Mode::Async);
    const QByteArray remote = relay ? destination.files.value(QStringLiteral("/target/remote.txt"))
                                   : read(target + QStringLiteral("/remote.txt"));
    check(expectedSuccess ? remote == "remote bytes" : remote.isEmpty(), "serial result bytes match its actual outcome");
    check(source.exports == 1, "export runs once");
    check(source.removals == (cut && expectedSuccess ? 1 : 0), "cut removes the source only after confirmed full success");
    check(source.files.contains(QStringLiteral("/remote.txt")) != (cut && expectedSuccess), "unconfirmed transfer preserves source contents");
}
void batch()
{
    QtFileMgrFileView::clearRemoteBackendRegistry();
    QTemporaryDir temporary;
    Backend source(QStringLiteral("serialbatch"));
    QtFileMgrFileView view;
    view.setUrl(QUrl::fromLocalFile(temporary.path()));
    check(waitFor([&]() { return !view.flatModel()->isLoading(); }), "batch target navigation completes");
    QList<QUrl> urls;
    for (int i=0; i<128; ++i) {
        const QString path=QStringLiteral("/file-%1.txt").arg(i);
        source.files.insert(path,"batch"); urls.append(source.urlForPath(path));
    }
    QtFileMgrFileView::registerRemoteBackend(&source);
    paste(view,urls);
    check(waitFor([&]() { return source.exports==128 && QFile::exists(temporary.path()+QStringLiteral("/file-127.txt")); }), "large synchronous batch drains without recursive request creation");
    check(waitFor([&]() { return view.findChildren<QProgressDialog *>().isEmpty(); }), "completed batch releases its progress owner");
    check(source.exports==128, "batch creates one request per source");
}

void cancelBetweenRelayLegs(bool deleteOwner)
{
    QtFileMgrFileView::clearRemoteBackendRegistry();
    QTemporaryDir temporary;
    const QString after=temporary.path()+QStringLiteral("/after.txt");
    check(write(after,"queued bytes"), "cancel-between-legs queued source is created");
    Backend source(QStringLiteral("cancelrelay"));
    Backend destination(QStringLiteral("cancelrelaydest"));
    source.files.insert(QStringLiteral("/remote.txt"),"remote bytes");
    source.exportMode=deleteOwner ? Mode::DeleteOwnerAfterExport : Mode::CancelAfterExport;
    QtFileMgrFileView view(&destination);
    view.setUrl(destination.urlForPath(destination.rootPath()));
    check(waitFor([&]() { return !view.flatModel()->isLoading(); }), "cancel-between-legs target navigation completes");
    QtFileMgrFileView::registerRemoteBackend(&source);
    paste(view,{source.urlForPath(QStringLiteral("/remote.txt"))},true);
    paste(view,{QUrl::fromLocalFile(after)});
    check(waitFor([&]() {
        return destination.files.value(QStringLiteral("/target/after.txt"))=="queued bytes"
            && !view.hasActiveOperation() && !view.pendingOperationCount();
    }), "cancelling between relay legs releases the queued operation");
    check(destination.imports==1 && !destination.files.contains(QStringLiteral("/target/remote.txt")),
          "cancelled or ownerless relay never starts its upload leg");
    check(source.removals==0 && source.files.value(QStringLiteral("/remote.txt"))=="remote bytes",
          "cancellation between relay legs preserves source contents");
}

void destroyProgressInOverwriteQuestion()
{
    QtFileMgrFileView::clearRemoteBackendRegistry();
    QTemporaryDir temporary;
    const QString existing=temporary.path()+QStringLiteral("/remote.txt");
    check(write(existing,"old destination"), "overwrite fixture has an existing destination");
    Backend source(QStringLiteral("overwriteowner"));
    source.files.insert(QStringLiteral("/remote.txt"),"remote bytes");
    QtFileMgrFileView view;
    view.setUrl(QUrl::fromLocalFile(temporary.path()));
    check(waitFor([&]() { return !view.flatModel()->isLoading(); }), "overwrite fixture navigation completes");
    QtFileMgrFileView::registerRemoteBackend(&source);
    bool questionClosed=false;
    QTimer::singleShot(0,&view,[&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog=qobject_cast<QMessageBox *>(widget)) {
                if (dialog->windowTitle()!=QStringLiteral("覆盖本地项目")) continue;
                const auto owners=view.findChildren<QProgressDialog *>();
                for (auto *owner : owners) delete owner;
                questionClosed=true;
                dialog->done(QMessageBox::Yes);
                // The snapshot also contains the progress windows just deleted.
                return;
            }
        }
    });
    paste(view,{source.urlForPath(QStringLiteral("/remote.txt"))});
    check(questionClosed, "progress owner is destroyed inside the overwrite confirmation");
    check(source.exports==0 && read(existing)=="old destination", "destroyed overwrite owner prevents transfer and preserves old bytes");
    const QString after=temporary.path()+QStringLiteral("/after.txt");
    const QString elsewhere=temporary.path()+QStringLiteral("/elsewhere");
    check(QDir().mkpath(elsewhere), "following operation destination is created");
    check(write(after,"queued bytes"), "following operation source is created");
    view.setUrl(QUrl::fromLocalFile(elsewhere));
    paste(view,{QUrl::fromLocalFile(after)});
    check(waitFor([&]() { return read(elsewhere+QStringLiteral("/after.txt"))=="queued bytes"; }),
          "destroying overwrite progress leaves the following queue runnable");
}
}
int main(int argc, char **argv)
{
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    // Acknowledge only the expected error-details dialogs in failure fixtures.
    QTimer closer;
    QObject::connect(&closer,&QTimer::timeout,&app,[]() {
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *dialog=qobject_cast<QMessageBox *>(widget))
                if (dialog->icon()==QMessageBox::Critical) dialog->accept();
    });
    closer.start(5);
    scenario(false,Mode::Sync,Mode::Sync,false);
    scenario(false,Mode::Async,Mode::Sync,true);
    scenario(false,Mode::Missing,Mode::Sync,true);
    scenario(false,Mode::DestroyRequest,Mode::Sync,true);
    scenario(false,Mode::DestroyOwner,Mode::Sync,true);
    scenario(true,Mode::Sync,Mode::Sync,true);
    scenario(true,Mode::Async,Mode::Sync,false);
    scenario(true,Mode::Sync,Mode::Async,false);
    scenario(true,Mode::Missing,Mode::Sync,true);
    scenario(true,Mode::DestroyRequest,Mode::Sync,true);
    scenario(true,Mode::Sync,Mode::Missing,true);
    scenario(true,Mode::Sync,Mode::DestroyRequest,true);
    scenario(true,Mode::Sync,Mode::DestroyOwner,true);
    cancelBetweenRelayLegs(false);
    cancelBetweenRelayLegs(true);
    destroyProgressInOverwriteQuestion();
    batch();
    qInfo("[serial transfer test] %d checks, %d failures",checks,failures);
    return failures ? 1 : 0;
}
