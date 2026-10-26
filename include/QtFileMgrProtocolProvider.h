// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRPROTOCOLPROVIDER_H
#define QTFILEMGRPROTOCOLPROVIDER_H

#include "QtFileMgrFeatures.h"
#include <QObject>
#include <QUrl>
#include <functional>

class QWidget;
class QtFileMgrIO;
#if QTFILEMGR_WITH_SSH
struct QtFileMgrSSHConnectionConfig;
#endif

// Providers and their callbacks run on the GUI thread. If transferOwnership is
// true, the receiver adopts the IO (or disposes it on rejection). Otherwise the
// provider/caller retains ownership. Never put credentials in url.
struct QtFileMgrConnectionResult {
    QtFileMgrIO *io = nullptr;
    bool transferOwnership = false;
    QUrl url;
    QString displayName;
    bool addToPlaces = false;
    bool cancelled = false;
    QString error;
};

class QtFileMgrProtocolProvider : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(const QtFileMgrConnectionResult &)>;
    explicit QtFileMgrProtocolProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~QtFileMgrProtocolProvider() override = default;
    virtual void connectUrl(const QUrl &url, QWidget *dialogParent,
                            Completion completion) = 0;
};

#if QTFILEMGR_WITH_SSH
class QtFileMgrSSHProvider final : public QtFileMgrProtocolProvider {
    Q_OBJECT
public:
    explicit QtFileMgrSSHProvider(QObject *parent = nullptr)
        : QtFileMgrProtocolProvider(parent) {}
    void connectUrl(const QUrl &url, QWidget *dialogParent,
                    Completion completion) override;

private:
    // 单次连接尝试。已保存的位置不含凭据,认证失败时回弹密码框
    // 补录并重试一次 (allowPasswordPrompt 仅首轮为 true)。
    // savePasswordOnSuccess: 对话框勾选"记住密码"时连接成功后落盘
    void startConnection(const QtFileMgrSSHConnectionConfig &config,
                         const QtFileMgrConnectionResult &baseResult,
                         Completion completion, bool allowPasswordPrompt,
                         QWidget *dialogParent, bool savePasswordOnSuccess);
};
#endif

#endif
