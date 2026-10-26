// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRPLACESPANEL_H
#define QTFILEMGRPLACESPANEL_H

#include <QTreeView>
#include <QTimer>
#include <QList>
#include <QPointer>
#include <QHash>
#include <QSet>

class QStandardItemModel;
class QStandardItem;
class QtFileMgrDeviceProvider;

// PlaceEntry 在 QtFileMgrWidget.h 中完整定义; 此处需 include 因为 QList<PlaceEntry> 成员
// 循环 include 由头文件保护解决 (QtFileMgrWidget.h -> QtFileMgrPlacesPanel.h 被跳过)
#include "QtFileMgrWidget.h"

/// Place 匹配结果 (参照 Dolphin PlacesPanel::urlChanged 反向查找)
struct PlaceMatch {
    QString name;       ///< Place 显示名称
    QString rootPath;   ///< Place 根路径
    bool found = false; ///< 是否找到匹配
};

// 参考 Dolphin PlacesPanel (KFilePlacesView)
// 侧边栏快捷入口面板：分组显示常用位置
class QtFileMgrPlacesPanel : public QTreeView {
    Q_OBJECT
public:
    explicit QtFileMgrPlacesPanel(QWidget *parent = nullptr);

    QList<QAction*> customContextMenuActions() const;
    void setCustomContextMenuActions(const QList<QAction*> &actions);
    void buildModel();

    // 配置驱动
    void setShowDevices(bool show);
    void setDeviceRefreshMs(int ms);
    void setConfigPlaces(const QList<PlaceEntry> &places);

private:
    // 历史污染迁移: 早期设置对话框把内置默认项写进了自定义位置,
    // 每次启动刷 dedup 警告;在配置上下文就绪后一次性清除重复者
    void deduplicatePersistedCustomPlaces();
    bool m_migrationDone = false;

public:
    void setDeviceProvider(QtFileMgrDeviceProvider *provider);
    bool mountDevice(const QString &id, bool activateAfterMount = false);
    bool unmountDevice(const QString &id);

    // 外部添加/移除分组
    QStandardItem *addGroup(const QString &name);
    void removeGroup(QStandardItem *group);
    QStandardItem *addPlace(QStandardItem *group, const QString &name,
                            const QString &path, const QString &iconName);

    // 网络位置注册 (宿主集成层调用, 与 SSH 入口并列)
    void registerNetworkPlace(const QString &name, const QString &path,
                              const QString &icon = QStringLiteral("folder-remote"));
    void registerPersistentNetworkPlace(
        const QString &name, const QString &path,
        const QString &icon = QStringLiteral("folder-remote"));
    void unregisterNetworkPlace(const QString &path);

    // URL 同步: 根据当前路径高亮匹配的 Place 项, 返回匹配信息
    PlaceMatch selectPlaceForPath(const QString &path);
    PlaceMatch findPlaceForPath(const QString &path) const;

signals:
    void placeActivated(const QString &path);
    void placeRemoved(const QString &path);
    void trashRequested();
    void errorMessage(const QString &error);
    void deviceOperationStarted(const QString &id, bool mounting);
    void deviceOperationFinished(const QString &id, bool mounting,
                                 bool success, const QString &error);

private slots:
    void onItemClicked(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);

private:
    void refreshDevices();
    bool startDeviceOperation(const QString &id, bool mounting,
                              bool activateAfterMount);
    QStandardItem* createGroupItem(const QString &name);
    QStandardItem* createPlaceItem(const QString &name, const QString &path,
                                    const QString &iconName, const QString &tooltip = QString());

    QStandardItemModel *m_model;
    QStandardItem *m_devicesGroup = nullptr;
    QStandardItem *m_networkGroup = nullptr;
    QList<QAction*> m_customActions;
    QTimer *m_deviceTimer = nullptr;

    // 配置状态
    bool m_showDevices = true;
    int m_deviceRefreshMs = 30000;
    QList<PlaceEntry> m_configPlaces;
    QList<PlaceEntry> m_networkPlaces;
    QSet<QString> m_persistentNetworkPaths;
    QPointer<QtFileMgrDeviceProvider> m_deviceProvider;
    QHash<QString, bool> m_pendingDeviceOperations;
    // 最近一次 selectPlaceForPath 的输入:buildModel 重建后据此回补
    // 当前高亮,否则面板重建即丢选中 (R2-WS-P2-4)
    QString m_lastPlacePath;
};

#endif // QTFILEMGRPLACESPANEL_H
