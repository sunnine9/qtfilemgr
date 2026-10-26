// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRCONFIGSTORE_H
#define QTFILEMGRCONFIGSTORE_H

#include "QtFileMgrWidget.h"

class QtFileMgrConfigStore {
public:
    static QString settingsOrganization();
    static QString settingsApplication();
    static QString defaultConfigPath();
    static QtFileMgrConfig loadDefault();
    static bool saveDefault(const QtFileMgrConfig &config, QString *error = nullptr);
    static bool saveFile(const QtFileMgrConfig &config,
                         const QString &filePath,
                         QString *error = nullptr);

    static QList<PlaceEntry> loadCustomPlaces();
    static void saveCustomPlaces(const QList<PlaceEntry> &entries);
    static QList<PlaceEntry> loadNetworkPlaces();
    static void saveNetworkPlaces(const QList<PlaceEntry> &entries);
    static QList<int> loadDetailColumnWidths();
    static void saveDetailColumnWidths(const QList<int> &widths);
    static int loadDirectoryViewMode(const QUrl &url, int fallback);
    static void saveDirectoryViewMode(const QUrl &url, int mode);

#if QTFILEMGR_WITH_SSH
    // SSH 凭据: XOR 混淆 + magic 完整性标记, 存储文件 0600。
    // 注意是混淆而非加密, 仅防明文落盘; 身份键为 user@host:port。
    static void saveSshCredential(const QString &user, const QString &host,
                                  int port, const QString &password);
    static QString loadSshCredential(const QString &user, const QString &host,
                                     int port);
    static void removeSshCredential(const QString &user, const QString &host,
                                    int port);
#endif
};

#endif
