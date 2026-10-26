// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrProtocolProvider.h"
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrSSHDialog.h"
#include "QtFileMgrSSHIO.h"

#include <QMessageBox>
#include <QPointer>
#include <QThread>
#include <QWidget>
#include <memory>

void QtFileMgrSSHProvider::connectUrl(const QUrl &url, QWidget *dialogParent,
                                      Completion completion)
{
    if (thread() != QThread::currentThread() ||
        (dialogParent && dialogParent->thread() != thread())) {
        qWarning("[SSHProvider] connectUrl: rejected thread mismatch (url=\"%s\")",
                 qPrintable(url.toString(QUrl::RemovePassword)));
        QtFileMgrConnectionResult result;
        result.error = QStringLiteral("连接提供者与界面不在同一线程");
        completion(result);
        return;
    }
    QtFileMgrConnectionResult result;
    QtFileMgrSSHConnectionConfig config;
    bool fromSavedPlace = false;
    bool savePasswordOnSuccess = false;
    if (url.isValid() && !url.host().isEmpty() && !url.userName().isEmpty()) {
        // Saved places contain only URL identity/path, never credentials.
        // 用户勾选过"记住密码"时从混淆存储取回 (凭据以身份为键, 不入 URL)
        fromSavedPlace = true;
        config.host = url.host();
        config.port = url.port(22);
        config.user = url.userName();
        config.initialPath = url.path().isEmpty() ? QStringLiteral("/") : url.path();
        config.password = QtFileMgrConfigStore::loadSshCredential(
            config.user, config.host, config.port);
        result.url = url.adjusted(QUrl::RemovePassword);
        result.displayName = QStringLiteral("%1@%2").arg(config.user, config.host);
    } else {
        QtFileMgrSSHDialog dialog(dialogParent);
        dialog.setUrl(url);
        if (dialog.exec() != QDialog::Accepted) {
            result.cancelled = true;
            completion(result);
            return;
        }
        config = dialog.connectionConfig();
        result.url = dialog.sshUrl();
        result.displayName = dialog.displayName();
        result.addToPlaces = dialog.addToPlaces();
        savePasswordOnSuccess = dialog.savePassword();
    }
    startConnection(config, result, std::move(completion),
                    fromSavedPlace, dialogParent, savePasswordOnSuccess);
}

void QtFileMgrSSHProvider::startConnection(
    const QtFileMgrSSHConnectionConfig &config,
    const QtFileMgrConnectionResult &baseResult,
    Completion completion, bool allowPasswordPrompt, QWidget *dialogParent,
    bool savePasswordOnSuccess)
{
    auto *backend = new QtFileMgrSSHIO(this);
    QPointer<QtFileMgrSSHIO> guarded(backend);
    QPointer<QWidget> parentGuard(dialogParent);
    connect(backend, &QtFileMgrSSHIO::unknownHostKey, this,
            [guarded, parentGuard](const QString &host, int port,
                                   const QString &fingerprint) {
        if (!guarded) {
            qWarning("[SSHProvider] host key: backend disappeared (host=\"%s\", port=%d)",
                     qPrintable(host), port);
            return;
        }
        QString shownFingerprint = fingerprint;
        if (shownFingerprint.isEmpty()) {
            qWarning("[SSHProvider] host key: fingerprint unavailable (host=\"%s\", port=%d)",
                     qPrintable(host), port);
            shownFingerprint = QStringLiteral("无法读取指纹");
        }
        const auto answer = QMessageBox::question(
            parentGuard.data(), QStringLiteral("验证服务器身份"),
            QStringLiteral("首次连接 %1:%2。\n\n主机密钥指纹:\n%3\n\n确认信任并写入 known_hosts 吗？")
                .arg(host).arg(port).arg(shownFingerprint),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        // The modal dialog runs an event loop: closing the host window or
        // cancelling the connection can destroy the backend while it is open.
        if (!guarded) {
            qWarning("[SSHProvider] host key: discarded decision after backend disappeared (host=\"%s\", port=%d)",
                     qPrintable(host), port);
            return;
        }
        guarded->acceptUnknownHostKey(answer == QMessageBox::Yes);
    });
    auto completed = std::make_shared<bool>(false);
    connect(backend, &QtFileMgrSSHIO::connectionFinished, this,
            [this, guarded, parentGuard, baseResult, completion, completed,
             allowPasswordPrompt, savePasswordOnSuccess,
             config](bool success, const QString &error) mutable {
        if (*completed) {
            qWarning("[SSHProvider] connectUrl: skipped duplicate completion (url=\"%s\")",
                     qPrintable(baseResult.url.toString(QUrl::RemovePassword)));
            return;
        }
        *completed = true;
        if (success && guarded) {
            if (savePasswordOnSuccess && !config.password.isEmpty()) {
                QtFileMgrConfigStore::saveSshCredential(
                    config.user, config.host, config.port, config.password);
            }
            QtFileMgrConnectionResult result = baseResult;
            result.io = guarded.data();
            result.transferOwnership = true;
            guarded->setParent(nullptr);
            completion(result);
            return;
        }
        // 已保存的位置不存凭据:认证失败回弹密码框补录重试一次,
        // 而不是直接硬失败 (对齐 KIO 的交互语义)
        if (allowPasswordPrompt && guarded
            && error.startsWith(QStringLiteral("SSH 认证失败"))) {
            qWarning("[SSHProvider] connectUrl: authentication failed without stored credentials; prompting for password (url=\"%s\")",
                     qPrintable(baseResult.url.toString(QUrl::RemovePassword)));
            QtFileMgrSSHDialog dialog(parentGuard.data());
            dialog.setUrl(baseResult.url);
            dialog.setReconnectMode(true);
            if (guarded)
                guarded->deleteLater();
            if (dialog.exec() == QDialog::Accepted) {
                QtFileMgrConnectionResult retryResult = baseResult;
                retryResult.url = dialog.sshUrl();
                retryResult.displayName = dialog.displayName();
                // 补密码重连的是已存在的位置,绝不再写入 Places,
                // 否则每次重连都按新路径追加一条同名记录
                retryResult.addToPlaces = false;
                startConnection(dialog.connectionConfig(), retryResult,
                                completion, false, parentGuard.data(),
                                dialog.savePassword());
            } else {
                QtFileMgrConnectionResult result = baseResult;
                result.cancelled = true;
                completion(result);
            }
            return;
        }
        if (guarded)
            guarded->deleteLater();
        QtFileMgrConnectionResult result = baseResult;
        result.error = error.isEmpty() ? QStringLiteral("SSH 连接失败") : error;
        completion(result);
    });
    backend->connectToHost(config);
}
