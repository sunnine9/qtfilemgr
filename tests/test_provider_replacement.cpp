// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrIO.h"
#include "QtFileMgrProtocolProvider.h"
#include "QtFileMgrWidget.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QVector>
#include <utility>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[provider replacement test] FAIL: %s", message);
    }
}

void drain()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void navigate(QtFileMgrWidget &widget, const QUrl &url)
{
    widget.setUrl(url);
    // Unsupported-scheme navigation invokes the registered provider on the
    // next GUI turn, matching saved-place activation and real host usage.
    drain();
}

QtFileMgrConfig config()
{
    QtFileMgrConfig result;
    result.rootPath.clear();
    return result;
}

class RemoteIO : public QtFileMgrIO {
public:
    explicit RemoteIO(QObject *parent = nullptr,
                      const QString &scheme = QStringLiteral("pending"))
        : QtFileMgrIO(parent), m_scheme(scheme) {}
    QString rootPath() const override { return QStringLiteral("/"); }
    QString displayName(const QString &path) const override { return path; }
    bool acceptsUrl(const QUrl &url) const override { return url.scheme() == m_scheme; }
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

class PendingProvider : public QtFileMgrProtocolProvider {
public:
    QVector<Completion> completions;
    QVector<QUrl> urls;
    void connectUrl(const QUrl &url, QWidget *, Completion completed) override
    {
        urls.append(url);
        completions.append(std::move(completed));
    }
    void finish(int index, QtFileMgrIO *backend, bool owned = false)
    {
        if (index < 0 || index >= completions.size()) {
            qWarning("[provider replacement test] completion unavailable (index=%d, requests=%d)",
                     index, completions.size());
            check(false, "the provider has the requested completion");
            return;
        }
        QtFileMgrConnectionResult result;
        result.io = backend;
        result.url = urls.at(index);
        result.transferOwnership = owned;
        result.displayName = QStringLiteral("Pending provider fixture");
        completions.at(index)(result);
    }
    void cancel(int index)
    {
        if (index < 0 || index >= completions.size()) {
            qWarning("[provider replacement test] cancellation completion unavailable (index=%d, requests=%d)",
                     index, completions.size());
            check(false, "the provider has the requested cancellation completion");
            return;
        }
        QtFileMgrConnectionResult result;
        result.url = urls.at(index);
        result.cancelled = true;
        completions.at(index)(result);
    }
};

const QUrl target(QStringLiteral("pending:///documents"));

void testReplacementAndLateCompletion()
{
    QObject host;
    RemoteIO installed(&host);
    PendingProvider first;
    PendingProvider second;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), &first);
    navigate(widget, target);
    navigate(widget, target);
    check(first.urls.size() == 1, "the current provider's duplicate URL is deduplicated");

    widget.setProtocolProvider(QStringLiteral("pending"), &second);
    navigate(widget, target);
    check(second.urls.size() == 1, "replacement provider receives the same pending URL");
    QPointer<RemoteIO> obsolete = new RemoteIO;
    first.finish(0, obsolete, true);
    drain();
    check(obsolete.isNull(), "a stale newly transferred backend is disposed");
    navigate(widget, target);
    check(second.urls.size() == 1, "old completion does not clear the replacement's pending marker");

    second.finish(0, &installed);
    check(widget.io() == &installed, "the replacement completion installs its backend");
    check(installed.parent() == &host, "ownership=false retains the host's backend parent");
}

void testOldProviderDestruction()
{
    auto *first = new PendingProvider;
    PendingProvider second;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), first);
    navigate(widget, target);
    widget.setProtocolProvider(QStringLiteral("pending"), &second);
    navigate(widget, target);
    delete first;
    navigate(widget, target);
    check(second.urls.size() == 1, "destroying an old provider preserves the new pending request");
    widget.setIO(nullptr);
    navigate(widget, target);
    check(second.urls.size() == 2, "destroying an old provider does not unregister its replacement");
}

void testAlreadyAdoptedResultIsRetained()
{
    PendingProvider first;
    PendingProvider second;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), &first);
    navigate(widget, target);
    widget.setProtocolProvider(QStringLiteral("pending"), &second);
    navigate(widget, target);
    QPointer<RemoteIO> installed = new RemoteIO;
    second.finish(0, installed, true);
    check(widget.io() == installed && installed->parent() == &widget,
          "a current ownership transfer is adopted by the widget");
    first.finish(0, installed, true);
    drain();
    check(installed && widget.io() == installed,
          "a stale result cannot delete a backend already adopted by the new connection");
}

void testUnregisterAndRegister()
{
    PendingProvider provider;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), &provider);
    navigate(widget, target);
    widget.setProtocolProvider(QStringLiteral("pending"), nullptr);
    widget.setProtocolProvider(QStringLiteral("pending"), &provider);
    navigate(widget, target);
    check(provider.urls.size() == 2, "unregistering and registering permits a retry before old completion");
    provider.cancel(0);
    navigate(widget, target);
    check(provider.urls.size() == 2, "an old same-provider generation cannot clear a newer pending request");
    provider.cancel(1);
    navigate(widget, target);
    check(provider.urls.size() == 3, "the current cancellation releases its pending marker");
}

void testProviderDestroyedWhilePending()
{
    auto *first = new PendingProvider;
    PendingProvider second;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), first);
    navigate(widget, target);
    const auto lateCompletion = first->completions.first();
    delete first;
    widget.setProtocolProvider(QStringLiteral("pending"), &second);
    navigate(widget, target);
    check(second.urls.size() == 1, "a destroyed pending provider does not block a new registration");
    QtFileMgrConnectionResult stale;
    stale.url = target;
    stale.cancelled = true;
    lateCompletion(stale);
    navigate(widget, target);
    check(second.urls.size() == 1, "a callback from a destroyed provider does not clear the new pending request");
}

void testWidgetOwnedProviderDestruction()
{
    auto *widget = new QtFileMgrWidget(config());
    QPointer<PendingProvider> provider = new PendingProvider;
    provider->setParent(widget);
    widget->setProtocolProvider(QStringLiteral("pending"), provider);
    navigate(*widget, target);
    check(provider->urls.size() == 1, "a widget-owned provider has an outstanding connection before teardown");
    delete widget;
    check(provider.isNull(), "widget teardown destroys child providers without callbacks into destroyed members");
}

void testBackendSwitchAndSupersedingUrl()
{
    PendingProvider provider;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("pending"), &provider);
    navigate(widget, target);
    widget.setIO(nullptr);
    navigate(widget, target);
    check(provider.urls.size() == 2, "setIO invalidates the pending marker so the same URL can retry");
    provider.cancel(0);
    navigate(widget, target);
    check(provider.urls.size() == 2, "completion before a backend switch cannot clear the retried URL");
    const QUrl next(QStringLiteral("pending:///next"));
    navigate(widget, next);
    check(provider.urls.size() == 3, "a different non-SSH URL supersedes the old attempt");
    provider.cancel(1);
    navigate(widget, next);
    check(provider.urls.size() == 3, "a superseded completion cannot clear the latest attempt");
}

void testSshAuthorityCoalescing()
{
    QObject host;
    RemoteIO backend(&host, QStringLiteral("ssh"));
    PendingProvider provider;
    QtFileMgrWidget widget(config());
    widget.setProtocolProvider(QStringLiteral("ssh"), &provider);
    const QUrl first(QStringLiteral("ssh://user@fixture.invalid/first"));
    const QUrl last(QStringLiteral("ssh://user@fixture.invalid/last"));
    navigate(widget, first);
    navigate(widget, last);
    navigate(widget, first);
    check(provider.urls.size() == 1, "SSH same-authority paths share one pending connection");
    provider.finish(0, &backend);
    check(widget.io() == &backend, "the coalesced SSH connection installs its backend");
    check(widget.url() == last, "SSH completion navigates to the latest coalesced target after setIO");
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testReplacementAndLateCompletion();
    testOldProviderDestruction();
    testAlreadyAdoptedResultIsRetained();
    testUnregisterAndRegister();
    testProviderDestroyedWhilePending();
    testWidgetOwnedProviderDestruction();
    testBackendSwitchAndSupersedingUrl();
    testSshAuthorityCoalescing();
    qInfo("[provider replacement test] %d checks, %d failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
