// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrProtocolProvider.h"
#include "QtFileMgrSSHIO.h"

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <cstdio>
#include <memory>

namespace {
int checks = 0;
int failures = 0;
QMutex warningMutex;
QStringList warnings;
QtMessageHandler previousHandler = nullptr;

void captureWarnings(QtMsgType type, const QMessageLogContext &context,
                     const QString &message)
{
    if (type == QtWarningMsg) {
        QMutexLocker lock(&warningMutex);
        warnings.append(message);
    }
    if (previousHandler)
        previousHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", qPrintable(message));
}

void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical("[SSH provider lifetime test] FAIL: %s", message);
    }
}

void clearWarnings()
{
    QMutexLocker lock(&warningMutex);
    warnings.clear();
}

bool hasDiscardDiagnostic()
{
    QMutexLocker lock(&warningMutex);
    for (const QString &message : warnings) {
        if (message.contains(QStringLiteral(
                "[SSHProvider] host key: discarded decision after backend disappeared"))
            && message.contains(QStringLiteral("host=\"127.0.0.1\""))
            && message.contains(QStringLiteral("port=1")))
            return true;
    }
    return false;
}

QPointer<QtFileMgrSSHIO> createBackend(QtFileMgrSSHProvider *provider)
{
    provider->connectUrl(QUrl(QStringLiteral("ssh://probe@127.0.0.1:1/")),
                         nullptr, [](const QtFileMgrConnectionResult &) {});
    QPointer<QtFileMgrSSHIO> backend = provider->findChild<QtFileMgrSSHIO *>();
    check(!backend.isNull(), "provider created its backend");
    if (backend) {
        // Test the provider's dialog callback without an SSH server. Cancel the
        // actual attempt before manually emitting the host-key signal so its
        // queued result cannot dispose the backend during the modal dialog.
        backend->disconnectFromHost();
    }
    return backend;
}

bool closeQuestion()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *message = qobject_cast<QMessageBox *>(widget)) {
            message->done(QMessageBox::No);
            return true;
        }
    }
    return false;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    previousHandler = qInstallMessageHandler(captureWarnings);

    {
        QtFileMgrSSHProvider provider;
        QPointer<QtFileMgrSSHIO> backend = createBackend(&provider);
        if (backend) {
            clearWarnings();
            bool closedQuestion = false;
            QTimer::singleShot(0, &app, [&]() {
                delete backend.data();
                closedQuestion = closeQuestion();
            });
            backend->unknownHostKey(QStringLiteral("127.0.0.1"), 1,
                                    QStringLiteral("SHA256:test-fixture"));
            check(closedQuestion, "backend destruction occurred inside the modal dialog");
            check(backend.isNull(), "backend was destroyed while confirming the host key");
            check(hasDiscardDiagnostic(), "discarded decision logs host and port");
        }
    }

    {
        auto provider = std::make_unique<QtFileMgrSSHProvider>();
        QPointer<QtFileMgrSSHIO> backend = createBackend(provider.get());
        if (backend) {
            clearWarnings();
            bool closedQuestion = false;
            QTimer::singleShot(0, &app, [&]() {
                provider.reset();
                closedQuestion = closeQuestion();
            });
            backend->unknownHostKey(QStringLiteral("127.0.0.1"), 1,
                                    QStringLiteral("SHA256:test-fixture"));
            check(closedQuestion, "provider destruction occurred inside the modal dialog");
            check(backend.isNull(), "provider destruction disposed its backend");
            check(hasDiscardDiagnostic(), "provider destruction also logs the discarded decision");
        }
    }

    {
        // This synthetic signal checks the live-backend UI path only. Actual
        // host-key rejection and connection results are covered by SSH E2E.
        QtFileMgrSSHProvider provider;
        QPointer<QtFileMgrSSHIO> backend = createBackend(&provider);
        if (backend) {
            clearWarnings();
            bool closedQuestion = false;
            QTimer::singleShot(0, &app, [&]() {
                closedQuestion = closeQuestion();
            });
            backend->unknownHostKey(QStringLiteral("127.0.0.1"), 1,
                                    QStringLiteral("SHA256:test-fixture"));
            check(closedQuestion, "the No action closes the confirmation dialog");
            check(!backend.isNull(), "the normal No path preserves the live backend");
            check(!hasDiscardDiagnostic(), "the live-backend decision is not discarded");
        }
    }

    qInstallMessageHandler(previousHandler);
    qInfo("[SSH provider lifetime test] %d checks, %d failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
