// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrDeviceProvider.h"

#include "QtFileMgrIO.h"

#include <QSet>
#include <QStorageInfo>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QTimer>
#include <QDebug>
#include <algorithm>

bool QtFileMgrDeviceProvider::mount(const QString &id, QString *error)
{
    qWarning("[DeviceProvider] mount: provider '%s' does not support device '%s'",
             metaObject()->className(), qPrintable(id));
    if (error)
        *error = QStringLiteral("当前设备提供器不支持挂载");
    return false;
}

bool QtFileMgrDeviceProvider::unmount(const QString &id, QString *error)
{
    qWarning("[DeviceProvider] unmount: provider '%s' does not support device '%s'",
             metaObject()->className(), qPrintable(id));
    if (error)
        *error = QStringLiteral("当前设备提供器不支持卸载");
    return false;
}

void QtFileMgrDeviceProvider::mountAsync(
    const QString &id, QObject *context, OperationCallback callback)
{
    qWarning("[DeviceProvider] mountAsync: using synchronous compatibility adapter (provider=%s, id=\"%s\")",
             metaObject()->className(), qPrintable(id));
    QPointer<QtFileMgrDeviceProvider> provider(this);
    QTimer::singleShot(0, context, [provider, id, callback]() {
        if (!provider) {
            qWarning("[DeviceProvider] mountAsync: provider destroyed before operation (id=\"%s\")",
                     qPrintable(id));
            callback(false, QStringLiteral("设备提供器已断开"));
            return;
        }
        QString error;
        const bool success = provider->mount(id, &error);
        if (success)
            emit provider->devicesChanged();
        callback(success, error);
    });
}

void QtFileMgrDeviceProvider::unmountAsync(
    const QString &id, QObject *context, OperationCallback callback)
{
    qWarning("[DeviceProvider] unmountAsync: using synchronous compatibility adapter (provider=%s, id=\"%s\")",
             metaObject()->className(), qPrintable(id));
    QPointer<QtFileMgrDeviceProvider> provider(this);
    QTimer::singleShot(0, context, [provider, id, callback]() {
        if (!provider) {
            qWarning("[DeviceProvider] unmountAsync: provider destroyed before operation (id=\"%s\")",
                     qPrintable(id));
            callback(false, QStringLiteral("设备提供器已断开"));
            return;
        }
        QString error;
        const bool success = provider->unmount(id, &error);
        if (success)
            emit provider->devicesChanged();
        callback(success, error);
    });
}

QList<QtFileMgrDeviceEntry> QtFileMgrStorageDeviceProvider::devices() const
{
    QList<QtFileMgrDeviceEntry> result;
    QSet<QString> seenRoots;
    QSet<QString> seenIds;
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isReady()) {
            qWarning("[StorageDeviceProvider] devices: skipped unavailable volume (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(storage.rootPath()));
            continue;
        }
        if (storage.device().startsWith(QByteArrayLiteral("/dev/loop"))) {
            qWarning("[StorageDeviceProvider] devices: skipped loop volume (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(storage.rootPath()));
            continue;
        }
        if (storage.rootPath().trimmed().isEmpty()) {
            qWarning("[StorageDeviceProvider] devices: skipped volume without root (device=\"%s\")",
                     storage.device().constData());
            continue;
        }
        const QString rootPath = QDir::cleanPath(storage.rootPath());
        const QFileInfo rootInfo(rootPath);
        if (!rootInfo.isDir()) {
            qWarning("[StorageDeviceProvider] devices: skipped non-directory mount (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(rootPath));
            continue;
        }
        if (seenRoots.contains(rootPath)) {
            qWarning("[StorageDeviceProvider] devices: skipped duplicate root (device=\"%s\", root=\"%s\")",
                     storage.device().constData(), qPrintable(rootPath));
            continue;
        }
        seenRoots.insert(rootPath);
        QtFileMgrDeviceEntry entry;
        const QString deviceId = QString::fromLocal8Bit(storage.device());
        entry.id = deviceId.isEmpty()
            ? rootPath
            : deviceId + QStringLiteral("::") + rootPath;
        if (deviceId.isEmpty()) {
            qWarning("[StorageDeviceProvider] devices: using root as missing device id (root=\"%s\")",
                     qPrintable(rootPath));
        }
        if (seenIds.contains(entry.id)) {
            qWarning("[StorageDeviceProvider] devices: skipped duplicate stable id (device=\"%s\", root=\"%s\", id=\"%s\")",
                     storage.device().constData(), qPrintable(rootPath),
                     qPrintable(entry.id));
            continue;
        }
        seenIds.insert(entry.id);
        entry.name = storage.displayName();
        if (entry.name.isEmpty() || entry.name == QStringLiteral("/")) {
            qWarning("[StorageDeviceProvider] devices: using fallback display name (device=\"%s\", root=\"%s\", name=\"%s\")",
                     storage.device().constData(), qPrintable(rootPath),
                     qPrintable(entry.name));
            entry.name = QStringLiteral("文件系统");
        }
        entry.path = rootPath;
        entry.iconName = QStringLiteral("drive-harddisk");
        entry.bytesAvailable = storage.bytesAvailable();
        entry.bytesTotal = storage.bytesTotal();
        entry.toolTip = QStringLiteral("%1  (可用 %2)")
            .arg(entry.path, formatFileSize(storage.bytesAvailable()));
        entry.mounted = true;
        result.append(entry);
    }
    std::sort(result.begin(), result.end(),
              [](const QtFileMgrDeviceEntry &lhs,
                 const QtFileMgrDeviceEntry &rhs) {
        const int byName = QString::localeAwareCompare(lhs.name, rhs.name);
        return byName == 0 ? lhs.path < rhs.path : byName < 0;
    });
    return result;
}
