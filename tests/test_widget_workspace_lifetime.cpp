// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrFileView.h"
#include "QtFileMgrHamburgerMenu.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrPreviewPanel.h"
#include "QtFileMgrProtocolProvider.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrWorkspace.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMenu>
#include <QPointer>
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
        qCritical("[widget workspace lifetime test] FAIL: %s", message);
    }
}

void drain()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    do {
        drain();
        if (condition())
            return true;
        QThread::msleep(1);
    } while (timer.elapsed() < 3000);
    return false;
}

class RemoteIO : public QtFileMgrIO {
public:
    explicit RemoteIO(QObject *parent = nullptr,
                      const QString &scheme = QStringLiteral("lifetime"))
        : QtFileMgrIO(parent), m_scheme(scheme) {}
    QString rootPath() const override { return QStringLiteral("/"); }
    QString displayName(const QString &path) const override { return path; }
    bool acceptsUrl(const QUrl &url) const override
    { return url.scheme() == m_scheme; }
    QUrl urlForPath(const QString &path) const override
    {
        QUrl result;
        result.setScheme(m_scheme);
        result.setPath(path);
        return result;
    }
    QString pathFromUrl(const QUrl &url) const override { return url.path(); }
    bool exists(const QString &) const override { return true; }
    QList<FileEntry> listDir(const QString &) const override { return {}; }
    bool isDir(const QString &) const override { return true; }
    bool isWritable(const QString &) const override { return false; }
    bool canWrite() const override { return false; }
    bool isRemote() const override { return true; }
    bool mkdir(const QString &) override { return false; }
    bool rename(const QString &, const QString &) override { return false; }
    bool remove(const QString &) override { return false; }
    bool copy(const QString &, const QString &) override { return false; }
private:
    QString m_scheme;
};

class ImmediateProvider : public QtFileMgrProtocolProvider {
public:
    ImmediateProvider(QtFileMgrIO *io, bool transferOwnership)
        : m_io(io), m_transferOwnership(transferOwnership) {}
    void connectUrl(const QUrl &url, QWidget *, Completion completed) override
    {
        QtFileMgrConnectionResult result;
        result.io = m_io;
        result.url = url;
        result.transferOwnership = m_transferOwnership;
        result.displayName = QStringLiteral("Lifetime fixture");
        completed(result);
    }
private:
    QtFileMgrIO *m_io;
    bool m_transferOwnership;
};

QAction *previewAction(QtFileMgrWidget &widget)
{
    for (QAction *action : widget.hamburgerMenu()->menu()->actions()) {
        if (action->text() == QStringLiteral("预览面板"))
            return action;
    }
    qWarning("[widget workspace lifetime test] preview action unavailable (widget=%p)",
             static_cast<void *>(&widget));
    return nullptr;
}

void checkSplitSharesBackend(QtFileMgrWorkspace *workspace, QtFileMgrIO *backend)
{
    workspace->splitCurrentTab();
    check(workspace->paneCount() == 2, "remote pane is split");
    check(workspace->currentFileManager()->io() == backend,
          "new pane shares the original remote backend");
}

void testSidePanel()
{
    QtFileMgrConfig config;
    config.rootPath.clear();
    QtFileMgrWidget widget(config);
    widget.show();
    drain();
    QAction *action = previewAction(widget);
    check(action != nullptr, "preview menu action is present");
    if (!action)
        return;

    auto *preview = widget.findChild<QtFileMgrPreviewPanel *>();
    check(preview != nullptr, "built-in preview initially exists");
    action->setChecked(true);
    if (preview) {
        preview->setFiles({QDir::homePath()});
        check(preview->previewMode() == QtFileMgrPreviewPanel::IconPreview,
              "built-in preview displays a directory");
    }
    action->setChecked(false);
    check(preview && preview->previewMode() == QtFileMgrPreviewPanel::EmptyPreview,
          "hiding the built-in preview clears its content");

    QPointer<QWidget> custom = new QWidget;
    widget.setSidePanel(custom);
    drain();
    action->setChecked(true);
    check(custom && custom->isVisible(), "menu opens a host-supplied side panel");
    action->setChecked(false);
    check(custom && !custom->isVisible(), "menu closes a host-supplied side panel safely");
    action->setChecked(true);
    check(custom && custom->isVisible(), "custom side panel can be reopened after closing");
}

void testInjectedBackend()
{
    QObject host;
    QPointer<RemoteIO> backend = new RemoteIO(&host);
    auto *workspace = new QtFileMgrWorkspace(nullptr, false);
    workspace->currentFileManager()->setIO(backend);
    checkSplitSharesBackend(workspace, backend);
    check(backend && backend->parent() == &host,
          "split keeps an injected backend under its host parent");
    delete workspace;
    check(backend && backend->parent() == &host,
          "destroying the workspace leaves the injected host backend alive");

    QPointer<RemoteIO> parentless = new RemoteIO;
    workspace = new QtFileMgrWorkspace(nullptr, false);
    workspace->currentFileManager()->setIO(parentless);
    checkSplitSharesBackend(workspace, parentless);
    check(parentless && parentless->parent() == nullptr,
          "split does not adopt a parentless injected backend");
    delete workspace;
    check(parentless, "workspace destruction leaves a parentless injected backend alive");
    delete parentless.data();

    workspace = new QtFileMgrWorkspace(nullptr, false);
    QtFileMgrWidget *source = workspace->currentFileManager();
    QPointer<RemoteIO> paneChild = new RemoteIO(source);
    source->setIO(paneChild);
    checkSplitSharesBackend(workspace, paneChild);
    check(paneChild && paneChild->parent() == source,
          "an injected backend is not adopted merely because its parent is the source widget");
    delete workspace;
    check(!paneChild, "the caller-selected source parent still controls injected backend destruction");
}

void testProviderRetainsOwnership()
{
    QObject host;
    QPointer<RemoteIO> backend = new RemoteIO(&host);
    ImmediateProvider provider(backend, false);
    auto *workspace = new QtFileMgrWorkspace(nullptr, false);
    QtFileMgrWidget *source = workspace->currentFileManager();
    source->setProtocolProvider(QStringLiteral("lifetime"), &provider);
    source->setUrl(QUrl(QStringLiteral("lifetime:///documents")));
    check(waitFor([&]() { return source->io() == backend; }),
          "provider without ownership transfer supplies the backend");
    check(backend && backend->parent() == &host,
          "provider ownership=false retains the original parent");
    checkSplitSharesBackend(workspace, backend);
    check(backend && backend->parent() == &host,
          "splitting also preserves provider ownership=false");
    delete workspace;
    check(backend, "host backend returned without ownership transfer outlives workspace");

    workspace = new QtFileMgrWorkspace(nullptr, false);
    source = workspace->currentFileManager();
    QPointer<RemoteIO> sourceChild = new RemoteIO(source);
    ImmediateProvider childProvider(sourceChild, false);
    source->setProtocolProvider(QStringLiteral("lifetime"), &childProvider);
    source->setUrl(QUrl(QStringLiteral("lifetime:///documents")));
    check(waitFor([&]() { return source->io() == sourceChild; }),
          "ownership=false also accepts a backend already parented to the receiver");
    checkSplitSharesBackend(workspace, sourceChild);
    check(sourceChild && sourceChild->parent() == source,
          "ownership=false does not promote a caller-parented source child to the tab");
    delete workspace;
    check(!sourceChild, "ownership=false respects the caller-selected source parent's lifetime");
}

void testBackendReplacement(bool initiallyAdopted)
{
    QObject host;
    QtFileMgrConfig config;
    config.rootPath.clear();
    QtFileMgrWidget widget(config);
    QPointer<RemoteIO> original = new RemoteIO(&widget);
    ImmediateProvider first(original, initiallyAdopted);
    widget.setProtocolProvider(QStringLiteral("lifetime"), &first);
    widget.setUrl(QUrl(QStringLiteral("lifetime:///documents")));
    check(waitFor([&]() { return widget.io() == original; }),
          "first provider backend is installed before replacement");

    QPointer<RemoteIO> replacement = new RemoteIO(&host, QStringLiteral("nextlifetime"));
    ImmediateProvider second(replacement, false);
    widget.setProtocolProvider(QStringLiteral("nextlifetime"), &second);
    widget.setUrl(QUrl(QStringLiteral("nextlifetime:///documents")));
    check(waitFor([&]() { return widget.io() == replacement; }),
          "a different provider can replace the current backend");
    drain();
    check(initiallyAdopted ? original.isNull() : !original.isNull(),
          "backend replacement disposes only an explicitly adopted previous backend");
    check(replacement && replacement->parent() == &host,
          "replacement without ownership transfer keeps its host parent");
}

void testAdoptedBackend(bool closeSource)
{
    QObject providerOwner;
    QPointer<RemoteIO> backend = new RemoteIO(&providerOwner);
    ImmediateProvider provider(backend, true);
    auto *workspace = new QtFileMgrWorkspace(nullptr, false);
    workspace->show();
    QPointer<QtFileMgrWidget> source = workspace->currentFileManager();
    source->setProtocolProvider(QStringLiteral("lifetime"), &provider);
    source->setUrl(QUrl(QStringLiteral("lifetime:///documents")));
    check(waitFor([&]() { return source && source->io() == backend; }),
          "provider ownership=true supplies the backend");
    check(backend && backend->parent() == source,
          "explicitly adopted backend initially belongs to its receiver");
    QObject *const tab = source->parent();
    checkSplitSharesBackend(workspace, backend);
    QPointer<QtFileMgrWidget> sibling = workspace->currentFileManager();
    check(backend && backend->parent() == tab,
          "an explicitly adopted shared backend belongs to the tab");

    if (closeSource) {
        source->fileView()->focusCurrentView();
        check(waitFor([&]() { return workspace->currentFileManager() == source; }),
              "the original pane can become active before closing");
    }
    workspace->closeCurrentSplit();
    drain();
    check(workspace->paneCount() == 1, "closing a split leaves one pane");
    check(closeSource ? source.isNull() : sibling.isNull(),
          "the selected split pane is destroyed");
    check(backend && backend->parent() == tab,
          "the shared adopted backend survives closing either pane");
    check(workspace->currentFileManager()->io() == backend,
          "the remaining pane still uses the adopted backend");
    workspace->currentFileManager()->setUrl(QUrl(QStringLiteral("lifetime:///still-live")));
    drain();
    check(workspace->currentFileManager()->url().path() == QStringLiteral("/still-live"),
          "the remaining pane can navigate using the shared backend");

    // Splitting a surviving pane must retain the existing tab owner.
    checkSplitSharesBackend(workspace, backend);
    check(backend && backend->parent() == tab,
          "splitting an already shared adopted backend preserves its tab owner");
    delete workspace;
    check(!backend, "an adopted backend is released when its workspace is destroyed");
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testSidePanel();
    testInjectedBackend();
    testProviderRetainsOwnership();
    testBackendReplacement(false);
    testBackendReplacement(true);
    testAdoptedBackend(false);
    testAdoptedBackend(true);
    qInfo("[widget workspace lifetime test] %d checks, %d failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
