// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRDEVICEPROVIDER_H
#define QTFILEMGRDEVICEPROVIDER_H

#include <QObject>
#include <QList>
#include <functional>

struct QtFileMgrDeviceEntry {
    QString id;
    QString name;
    QString path;
    QString toolTip;
    QString iconName;
    qint64 bytesAvailable = -1;
    qint64 bytesTotal = -1;
    bool mounted = false;
    bool canMount = false;
    bool canUnmount = false;
};

class QtFileMgrDeviceProvider : public QObject {
    Q_OBJECT
public:
    explicit QtFileMgrDeviceProvider(QObject *parent = nullptr)
        : QObject(parent) {}

    virtual QList<QtFileMgrDeviceEntry> devices() const = 0;
    virtual bool mount(const QString &id, QString *error = nullptr);
    virtual bool unmount(const QString &id, QString *error = nullptr);

    using OperationCallback = std::function<void(
        bool success, const QString &error)>;
    virtual void mountAsync(const QString &id, QObject *context,
                            OperationCallback callback);
    virtual void unmountAsync(const QString &id, QObject *context,
                              OperationCallback callback);

signals:
    void devicesChanged();
};

class QtFileMgrStorageDeviceProvider : public QtFileMgrDeviceProvider {
    Q_OBJECT
public:
    explicit QtFileMgrStorageDeviceProvider(QObject *parent = nullptr)
        : QtFileMgrDeviceProvider(parent) {}

    QList<QtFileMgrDeviceEntry> devices() const override;
};

#endif
