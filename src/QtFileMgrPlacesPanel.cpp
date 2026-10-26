// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// 参考 Dolphin PlacesPanel (panels/places/placespanel.cpp)
// KFilePlacesView 的纯 Qt5 替代实现
#include "QtFileMgrPlacesPanel.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrWidget.h"  // for PlaceEntry full definition
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrDeviceProvider.h"
#include "QtFileMgrStyle.h"
#include <QStandardItemModel>
#include <QStandardItem>
#include <QHeaderView>
#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QSet>
#include <functional>
#include <QtDebug>
#include <QStyle>
#include <QStorageInfo>
#include <QTimer>
#include <QApplication>
#include <QClipboard>
#include <QStyledItemDelegate>
#include <QPainter>

namespace {

class PlacesDelegate final : public QStyledItemDelegate {
public:
    explicit PlacesDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        const bool group = !index.parent().isValid();
        const int contentHeight = qMax(option.fontMetrics.height(),
                                       option.decorationSize.height());
        return QSize(QStyledItemDelegate::sizeHint(option, index).width(),
                     group ? (index.row() == 0 ? 32 : 42)
                           : qMax(36, contentHeight + 12));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const auto c = QtFileMgrStyle::colors(opt.palette);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (!index.parent().isValid()) {
            QFont groupFont = opt.font;
            groupFont.setWeight(QFont::DemiBold);
            if (groupFont.pointSizeF() > 0)
                groupFont.setPointSizeF(groupFont.pointSizeF() * 0.87);
            painter->setFont(groupFont);
            painter->setPen(c.muted);
            const QRect textRect = opt.rect.adjusted(16, index.row() == 0 ? 3 : 12, -14, 0);
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(groupFont).elidedText(
                                  opt.text, Qt::ElideRight, textRect.width()));
            painter->restore();
            return;
        }

        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        const bool hovered = opt.state.testFlag(QStyle::State_MouseOver);
        const QRect row = opt.rect.adjusted(8, 2, -8, -2);
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? c.selection : c.hover);
            painter->drawRoundedRect(row, 6, 6);
        }

        const int iconSize = qMin(opt.decorationSize.width(), row.height() - 8);
        const QRect iconRect(row.left() + 10,
                             row.center().y() - iconSize / 2,
                             iconSize, iconSize);
        opt.icon.paint(painter, iconRect, Qt::AlignCenter,
                       opt.state.testFlag(QStyle::State_Enabled)
                           ? QIcon::Normal : QIcon::Disabled);
        QFont textFont = opt.font;
        if (selected)
            textFont.setWeight(QFont::DemiBold);
        painter->setFont(textFont);
        painter->setPen(opt.state.testFlag(QStyle::State_Enabled) ? c.text : c.muted);
        const QRect textRect = row.adjusted(10 + iconSize + 10, 0, -10, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(textFont).elidedText(
                              opt.text, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

} // namespace

static bool isRemotePlacePath(const QString &path)
{
    const QString scheme = QUrl(path).scheme().toLower();
    return !scheme.isEmpty() && scheme != QStringLiteral("file");
}

static QString normalizedPlacePath(QString path)
{
    if (isRemotePlacePath(path)) {
        QUrl url(path);
        url.setScheme(url.scheme().toLower());
        url.setHost(url.host().toLower());
        url.setPassword(QString());
        // 默认端口归一: 显式 :22 与缺省等价,统一去掉 (DLG-P2-12)
        if (url.scheme() == QStringLiteral("ssh") && url.port(-1) == 22)
            url.setPort(-1);
        QString remotePath = QDir::cleanPath(url.path());
        if (remotePath.isEmpty() || remotePath == QStringLiteral("."))
            remotePath = QStringLiteral("/");
        url.setPath(remotePath);
        return url.toString(QUrl::RemovePassword);
    }
    {
        if (QUrl(path).isLocalFile())
            path = QUrl(path).toLocalFile();
        path = QDir::fromNativeSeparators(path.trimmed());
        if (path == QLatin1String("~"))
            path = QDir::homePath();
        else if (path.startsWith(QLatin1String("~/")))
            path = QDir::homePath() + path.mid(1);
    }

    path = QDir::cleanPath(path);
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}

static bool samePlacePath(const QString &lhs, const QString &rhs)
{
    if (isRemotePlacePath(lhs) != isRemotePlacePath(rhs))
        return false;
    return normalizedPlacePath(lhs) == normalizedPlacePath(rhs);
}

static bool pathBelongsToPlace(const QString &path, const QString &place)
{
    if (isRemotePlacePath(path) != isRemotePlacePath(place))
        return false;

    const QString normPath = normalizedPlacePath(path);
    const QString normPlace = normalizedPlacePath(place);
    if (isRemotePlacePath(normPath)) {
        const QUrl pathUrl(normPath);
        const QUrl placeUrl(normPlace);
        const int defaultPort = pathUrl.scheme() == QStringLiteral("ssh") ? 22 : -1;
        if (pathUrl.scheme() != placeUrl.scheme()
            || pathUrl.host().compare(placeUrl.host(), Qt::CaseInsensitive) != 0
            || pathUrl.port(defaultPort) != placeUrl.port(defaultPort)
            || pathUrl.userName() != placeUrl.userName()) {
            return false;
        }
        const QString pathPart = QDir::cleanPath(pathUrl.path());
        const QString placePart = QDir::cleanPath(placeUrl.path());
        return pathPart == placePart
            || (placePart == QLatin1String("/")
                && pathPart.startsWith(QLatin1Char('/')))
            || pathPart.startsWith(placePart + QLatin1Char('/'));
    }
    if (normPlace == QLatin1String("/"))
        return normPath.startsWith(QLatin1Char('/'));
    return normPath == normPlace || normPath.startsWith(normPlace + QLatin1Char('/'));
}

// 菜单/对话框嵌套事件循环期间模型可能重建 (R2-08): 按路径重新定位
static QStandardItem *findPlaceItemByPath(QStandardItemModel *model,
                                          const QString &path)
{
    if (!model)
        return nullptr;
    for (int r = 0; r < model->rowCount(); ++r) {
        QStandardItem *group = model->item(r);
        if (!group)
            continue;
        for (int cr = 0; cr < group->rowCount(); ++cr) {
            QStandardItem *item = group->child(cr);
            if (item && item->data(Qt::UserRole).toString() == path)
                return item;
        }
    }
    return nullptr;
}

static bool modelContainsPlacePath(const QStandardItemModel *model, const QString &path)
{
    if (!model)
        return false;
    for (int r = 0; r < model->rowCount(); ++r) {
        QStandardItem *group = model->item(r);
        if (!group)
            continue;
        for (int cr = 0; cr < group->rowCount(); ++cr) {
            QStandardItem *item = group->child(cr);
            if (item && samePlacePath(item->data(Qt::UserRole).toString(), path))
                return true;
        }
    }
    return false;
}

QtFileMgrPlacesPanel::QtFileMgrPlacesPanel(QWidget *parent)
    : QTreeView(parent)
    , m_model(new QStandardItemModel(this))
    , m_devicesGroup(nullptr)
    , m_deviceProvider(new QtFileMgrStorageDeviceProvider(this))
{
#if QTFILEMGR_WITH_SSH
    m_networkPlaces = QtFileMgrConfigStore::loadNetworkPlaces();
    for (const PlaceEntry &entry : qAsConst(m_networkPlaces))
        m_persistentNetworkPaths.insert(normalizedPlacePath(entry.path));
#endif

    setHeaderHidden(true);
    setIndentation(0);
    setExpandsOnDoubleClick(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setIconSize(QSize(22, 22));
    setMouseTracking(true);
    setItemDelegate(new PlacesDelegate(this));
    setTextElideMode(Qt::ElideRight);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(QStringLiteral(
        "QTreeView{background:palette(window);border:0;padding:6px 0;}"
        "QTreeView::branch{background:transparent;}"));
    setFrameShape(QFrame::NoFrame);

    buildModel();
    setModel(m_model);
    expandAll();

    connect(this, &QTreeView::clicked, this, &QtFileMgrPlacesPanel::onItemClicked);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTreeView::customContextMenuRequested, this, &QtFileMgrPlacesPanel::onContextMenu);

    // 设备列表定时刷新
    m_deviceTimer = new QTimer(this);
    m_deviceTimer->setInterval(m_deviceRefreshMs);
    connect(m_deviceTimer, &QTimer::timeout, this, &QtFileMgrPlacesPanel::refreshDevices);
    m_deviceTimer->start();
    connect(m_deviceProvider, &QtFileMgrDeviceProvider::devicesChanged,
            this, &QtFileMgrPlacesPanel::refreshDevices);
}

void QtFileMgrPlacesPanel::buildModel()
{
    m_model->clear();

    // Places 分组
    QStandardItem *placesGroup = createGroupItem(QStringLiteral("位置"));
    m_model->appendRow(placesGroup);

    // 用 QSet 彻底去重 — 防止 JSON + QSettings 任何来源的重复
    QSet<QString> seenPaths;

    // 配置驱动的 places 或硬编码默认值
    if (!m_configPlaces.isEmpty()) {
        for (const auto &e : m_configPlaces) {
            const QString key = normalizedPlacePath(e.path);
            if (!seenPaths.contains(key)) {
                seenPaths.insert(key);
                QStandardItem *item = createPlaceItem(e.name, e.path, e.iconTheme);
                item->setData(false, Qt::UserRole + 2);
                placesGroup->appendRow(item);
            } else {
                qWarning("[PlacesPanel] dedup: skipped duplicate place \"%s\" (%s)",
                         qPrintable(e.name), qPrintable(e.path));
            }
        }
    } else {
        // 无配置 → 硬编码默认值 (用 seenPaths 跟 QSettings 去重)
        auto addIfNew = [&](const QString &name, const QString &path, const QString &icon) {
            // 判重统一用规范化路径,避免 "~/Documents" 与绝对路径并存 (DLG-P1-8)
            const QString key = normalizedPlacePath(path);
            if (seenPaths.contains(key)) {
                qWarning("[PlacesPanel] dedup: skipped hardcoded duplicate \"%s\" (%s)",
                         qPrintable(name), qPrintable(path));
                return;
            }
            bool remote = isRemotePlacePath(path);
            if (remote || QFileInfo::exists(path)) {
                seenPaths.insert(key);
                QStandardItem *item = createPlaceItem(name, path, icon);
                item->setData(false, Qt::UserRole + 2);
                placesGroup->appendRow(item);
            }
        };
        addIfNew(QStringLiteral("主目录"), QDir::homePath(), QStringLiteral("user-home"));
        addIfNew(QStringLiteral("桌面"),   QDir::homePath() + QStringLiteral("/Desktop"), QStringLiteral("user-desktop"));
        addIfNew(QStringLiteral("文档"),   QDir::homePath() + QStringLiteral("/Documents"), QStringLiteral("folder-documents"));
        addIfNew(QStringLiteral("下载"),   QDir::homePath() + QStringLiteral("/Downloads"), QStringLiteral("folder-downloads"));
        addIfNew(QStringLiteral("根目录"), QStringLiteral("/"), QStringLiteral("folder-root"));
    }

    // 自定义 places — 与配置/内置位置去重后叠加
    const QList<PlaceEntry> customPlaces = QtFileMgrConfigStore::loadCustomPlaces();
    for (const PlaceEntry &entry : customPlaces) {
        const QString name = entry.name;
        const QString path = entry.path;
        if (!name.isEmpty() && !path.isEmpty()) {
            const QString key = normalizedPlacePath(path);
            if (!seenPaths.contains(key) && (isRemotePlacePath(path) || QFileInfo::exists(path))) {
                seenPaths.insert(key);
                QStandardItem *item = createPlaceItem(
                    name, path,
                    entry.iconTheme.isEmpty() ? QStringLiteral("folder") : entry.iconTheme);
                item->setData(true, Qt::UserRole + 2);
                placesGroup->appendRow(item);
            } else if (seenPaths.contains(key)) {
                qWarning("[PlacesPanel] dedup: skipped QSettings duplicate \"%s\" (%s)",
                         qPrintable(name), qPrintable(path));
            }
        }
    }

    // 回收站是命令入口，不作为普通文件系统路径参与 URL 匹配。
    QStandardItem *trashItem = createPlaceItem(
        QStringLiteral("回收站"), QString(), QStringLiteral("user-trash"),
        QStringLiteral("查看和还原已删除的项目"));
    trashItem->setData(true, Qt::UserRole + 20);
    placesGroup->appendRow(trashItem);

    // Devices 分组 (动态) — 受 showDevices 控制
    if (m_showDevices) {
        m_devicesGroup = createGroupItem(QStringLiteral("设备"));
        m_model->appendRow(m_devicesGroup);
        refreshDevices();
    } else {
        m_devicesGroup = nullptr;
    }

    // 网络分组保留一个连接命令，行为与 Dolphin 的“添加网络位置”入口一致。
    m_networkGroup = createGroupItem(QStringLiteral("网络"));
    m_model->appendRow(m_networkGroup);
#if QTFILEMGR_WITH_SSH
    QStandardItem *connectItem = createPlaceItem(
        QStringLiteral("连接服务器..."), QStringLiteral("ssh://"),
        QStringLiteral("network-connect"), QStringLiteral("连接 SSH/SFTP 服务器"));
    connectItem->setData(true, Qt::UserRole + 21);
    m_networkGroup->appendRow(connectItem);
#endif
    QSet<QString> seenNetworkPaths;
    for (const PlaceEntry &entry : qAsConst(m_networkPlaces)) {
        if (entry.name.isEmpty() || entry.path.isEmpty()) {
            qWarning("[PlacesPanel] network: skipped invalid place name='%s' path='%s'",
                     qPrintable(entry.name), qPrintable(entry.path));
            continue;
        }
        const QString normalizedPath = normalizedPlacePath(entry.path);
        if (seenNetworkPaths.contains(normalizedPath)) {
            qWarning("[PlacesPanel] network dedup: skipped duplicate place '%s' (%s)",
                     qPrintable(entry.name), qPrintable(entry.path));
            continue;
        }
        seenNetworkPaths.insert(normalizedPath);
        QStandardItem *networkItem = createPlaceItem(
            entry.name, entry.path, entry.iconTheme);
        networkItem->setData(
            m_persistentNetworkPaths.contains(normalizedPath),
            Qt::UserRole + 22);
        m_networkGroup->appendRow(networkItem);
    }

    // 重建后回补当前高亮:模型 clear 已把选中清掉,不回补的话
    // 位置面板在注册/移除位置后丢失"正在这里"的指示 (R2-WS-P2-4)
    if (!m_lastPlacePath.isEmpty())
        selectPlaceForPath(m_lastPlacePath);
}

static void saveCustomPlaces(const QStandardItemModel *model)
{
    QList<PlaceEntry> entries;
    for (int r = 0; r < model->rowCount(); ++r) {
        QStandardItem *group = model->item(r);
        if (!group) continue;
        for (int cr = 0; cr < group->rowCount(); ++cr) {
            QStandardItem *item = group->child(cr);
            if (item && item->data(Qt::UserRole + 2).toBool()) {
                entries.append({
                    item->text(),
                    item->data(Qt::UserRole).toString(),
                    item->data(Qt::UserRole + 3).toString()
                });
            }
        }
    }
    QtFileMgrConfigStore::saveCustomPlaces(entries);
}

void QtFileMgrPlacesPanel::refreshDevices()
{
    if (!m_devicesGroup)
        return;
    const QString selectedDeviceId = currentIndex().data(
        Qt::UserRole + 10).toString();
    m_devicesGroup->removeRows(0, m_devicesGroup->rowCount());

    if (!m_deviceProvider)
        return;
    QSet<QString> seenIds;
    QSet<QString> seenMountedPaths;
    for (QtFileMgrDeviceEntry device : m_deviceProvider->devices()) {
        if (device.id.isEmpty()) {
            qWarning("[PlacesPanel] devices: skipped device without id (name=\"%s\", path=\"%s\")",
                     qPrintable(device.name), qPrintable(device.path));
            continue;
        }
        if (seenIds.contains(device.id)) {
            qWarning("[PlacesPanel] devices: skipped duplicate id (id=\"%s\", name=\"%s\", path=\"%s\")",
                     qPrintable(device.id), qPrintable(device.name),
                     qPrintable(device.path));
            continue;
        }
        if (device.mounted && device.path.isEmpty()) {
            qWarning("[PlacesPanel] devices: skipped mounted device without path (id=\"%s\", name=\"%s\")",
                     qPrintable(device.id), qPrintable(device.name));
            continue;
        }
        const QString normalizedPath = device.mounted
            ? normalizedPlacePath(device.path) : QString();
        if (!normalizedPath.isEmpty()
            && seenMountedPaths.contains(normalizedPath)) {
            qWarning("[PlacesPanel] devices: skipped duplicate mounted path (id=\"%s\", path=\"%s\")",
                     qPrintable(device.id), qPrintable(device.path));
            continue;
        }
        seenIds.insert(device.id);
        if (!normalizedPath.isEmpty())
            seenMountedPaths.insert(normalizedPath);
        if (device.name.isEmpty()) {
            qWarning("[PlacesPanel] devices: using id as missing display name (id=\"%s\", path=\"%s\")",
                     qPrintable(device.id), qPrintable(device.path));
            device.name = device.id;
        }
        if (device.iconName.isEmpty()) {
            qWarning("[PlacesPanel] devices: using default icon (id=\"%s\", name=\"%s\")",
                     qPrintable(device.id), qPrintable(device.name));
            device.iconName = QStringLiteral("drive-harddisk");
        }
        QString toolTip = device.toolTip;
        if (toolTip.isEmpty()) {
            toolTip = device.mounted
                ? device.path : QStringLiteral("未挂载");
            qWarning("[PlacesPanel] devices: using fallback tooltip (id=\"%s\", mounted=%d, path=\"%s\")",
                     qPrintable(device.id), device.mounted ? 1 : 0,
                     qPrintable(device.path));
        }
        const bool pending = m_pendingDeviceOperations.contains(device.id);
        if (pending)
            toolTip += QStringLiteral("\n设备操作进行中");
        QStandardItem *item = createPlaceItem(
            device.name, device.path, device.iconName, toolTip);
        item->setData(device.id, Qt::UserRole + 10);
        item->setData(device.mounted, Qt::UserRole + 11);
        item->setData(device.canMount, Qt::UserRole + 12);
        item->setData(device.canUnmount, Qt::UserRole + 13);
        item->setData(device.bytesAvailable, Qt::UserRole + 14);
        item->setData(device.bytesTotal, Qt::UserRole + 15);
        if (pending)
            item->setEnabled(false);
        m_devicesGroup->appendRow(item);
        if (device.id == selectedDeviceId)
            setCurrentIndex(item->index());
    }
}

void QtFileMgrPlacesPanel::setDeviceProvider(QtFileMgrDeviceProvider *provider)
{
    if (provider == m_deviceProvider)
        return;
    if (m_deviceProvider)
        disconnect(m_deviceProvider, nullptr, this, nullptr);
    if (!m_pendingDeviceOperations.isEmpty()) {
        const QString error = QStringLiteral("设备提供器已切换");
        const QHash<QString, bool> cancelled = m_pendingDeviceOperations;
        m_pendingDeviceOperations.clear();
        for (auto it = cancelled.constBegin();
             it != cancelled.constEnd(); ++it) {
            qWarning("[PlacesPanel] setDeviceProvider: cancelled pending device operation (id=\"%s\", mounting=%d)",
                     qPrintable(it.key()), it.value() ? 1 : 0);
            emit deviceOperationFinished(
                it.key(), it.value(), false, error);
        }
        emit errorMessage(error);
    }
    m_deviceProvider = provider;
    if (!provider) {
        qWarning("[PlacesPanel] setDeviceProvider: cleared device provider");
        refreshDevices();
        return;
    }
    connect(provider, &QtFileMgrDeviceProvider::devicesChanged,
            this, &QtFileMgrPlacesPanel::refreshDevices);
    connect(provider, &QObject::destroyed, this, [this]() {
        if (m_pendingDeviceOperations.isEmpty()) {
            qWarning("[PlacesPanel] device provider destroyed; clearing device entries (pending=0)");
        } else {
            const QString error = QStringLiteral("设备提供器已断开");
            const QHash<QString, bool> cancelled = m_pendingDeviceOperations;
            m_pendingDeviceOperations.clear();
            for (auto it = cancelled.constBegin();
                 it != cancelled.constEnd(); ++it) {
                qWarning("[PlacesPanel] device provider destroyed; cancelled pending operation (id=\"%s\", mounting=%d)",
                         qPrintable(it.key()), it.value() ? 1 : 0);
                emit deviceOperationFinished(
                    it.key(), it.value(), false, error);
            }
            emit errorMessage(error);
        }
        m_pendingDeviceOperations.clear();
        refreshDevices();
    });
    refreshDevices();
}

bool QtFileMgrPlacesPanel::mountDevice(const QString &id,
                                      bool activateAfterMount)
{
    return startDeviceOperation(id, true, activateAfterMount);
}

bool QtFileMgrPlacesPanel::unmountDevice(const QString &id)
{
    return startDeviceOperation(id, false, false);
}

bool QtFileMgrPlacesPanel::startDeviceOperation(
    const QString &id, bool mounting, bool activateAfterMount)
{
    if (!m_deviceProvider || id.isEmpty()) {
        qWarning("[PlacesPanel] device operation: rejected invalid request (provider=%s, id=\"%s\", mounting=%d)",
                 m_deviceProvider ? m_deviceProvider->metaObject()->className() : "<null>",
                 qPrintable(id), mounting ? 1 : 0);
        emit errorMessage(QStringLiteral("设备操作请求无效"));
        return false;
    }
    if (m_pendingDeviceOperations.contains(id)) {
        qWarning("[PlacesPanel] device operation: skipped duplicate request (id=\"%s\", mounting=%d)",
                 qPrintable(id), mounting ? 1 : 0);
        return false;
    }

    QtFileMgrDeviceEntry target;
    bool found = false;
    for (const QtFileMgrDeviceEntry &device : m_deviceProvider->devices()) {
        if (device.id != id)
            continue;
        if (found) {
            qWarning("[PlacesPanel] device operation: ignored duplicate provider entry (id=\"%s\", path=\"%s\")",
                     qPrintable(id), qPrintable(device.path));
            continue;
        }
        target = device;
        found = true;
    }
    if (!found || (mounting ? !target.canMount : !target.canUnmount)) {
        qWarning("[PlacesPanel] device operation: unsupported operation (id=\"%s\", found=%d, mounting=%d, canMount=%d, canUnmount=%d)",
                 qPrintable(id), found ? 1 : 0, mounting ? 1 : 0,
                 target.canMount ? 1 : 0, target.canUnmount ? 1 : 0);
        emit errorMessage(mounting ? QStringLiteral("该设备不能挂载")
                                   : QStringLiteral("该设备不能卸载"));
        return false;
    }

    m_pendingDeviceOperations.insert(id, mounting);
    refreshDevices();
    emit deviceOperationStarted(id, mounting);
    const QPointer<QtFileMgrDeviceProvider> requestedProvider = m_deviceProvider;
    const auto completed = [this, requestedProvider, id, mounting,
                            activateAfterMount](bool success,
                                                const QString &sourceError) {
        if (!m_pendingDeviceOperations.contains(id)) {
            qWarning("[PlacesPanel] device operation: ignored duplicate or stale callback (id=\"%s\", mounting=%d, success=%d)",
                     qPrintable(id), mounting ? 1 : 0, success ? 1 : 0);
            return;
        }
        m_pendingDeviceOperations.remove(id);
        QString error = sourceError;
        if (!requestedProvider || requestedProvider != m_deviceProvider) {
            qWarning("[PlacesPanel] device operation: provider changed before completion (id=\"%s\", mounting=%d)",
                     qPrintable(id), mounting ? 1 : 0);
            success = false;
            error = QStringLiteral("设备提供器已切换");
        } else if (!success && error.isEmpty()) {
            error = mounting ? QStringLiteral("挂载设备失败")
                             : QStringLiteral("卸载设备失败");
            qWarning("[PlacesPanel] device operation: backend failed without error (provider=%s, id=\"%s\", mounting=%d)",
                     requestedProvider->metaObject()->className(),
                     qPrintable(id), mounting ? 1 : 0);
        }
        refreshDevices();
        if (!success) {
            emit errorMessage(error);
        } else if (mounting && activateAfterMount) {
            QString mountedPath;
            for (const QtFileMgrDeviceEntry &device : requestedProvider->devices()) {
                if (device.id == id && device.mounted && !device.path.isEmpty()) {
                    mountedPath = device.path;
                    break;
                }
            }
            if (mountedPath.isEmpty()) {
                // provider 列表可能尚未刷新,等 devicesChanged 再判定,
                // 避免把挂载成功误报为失败 (DLG-P1-7)
                qWarning("[PlacesPanel] device operation: mount succeeded but path not yet visible; waiting for devicesChanged (provider=%s, id=\"%s\")",
                         requestedProvider->metaObject()->className(),
                         qPrintable(id));
                auto *waitTimer = new QTimer(this);
                waitTimer->setSingleShot(true);
                waitTimer->setInterval(3000);
                auto *conn = new QMetaObject::Connection;
                *conn = connect(requestedProvider,
                                &QtFileMgrDeviceProvider::devicesChanged, this,
                                [this, requestedProvider, id, waitTimer, conn]() {
                    for (const QtFileMgrDeviceEntry &device : requestedProvider->devices()) {
                        if (device.id == id && device.mounted && !device.path.isEmpty()) {
                            disconnect(*conn);
                            delete conn;
                            waitTimer->stop();
                            waitTimer->deleteLater();
                            emit placeActivated(device.path);
                            return;
                        }
                    }
                });
                connect(waitTimer, &QTimer::timeout, this,
                        [this, id, waitTimer, conn]() {
                    disconnect(*conn);
                    delete conn;
                    waitTimer->deleteLater();
                    qWarning("[PlacesPanel] device operation: mount path wait timed out (id=\"%s\")",
                             qPrintable(id));
                    emit errorMessage(QStringLiteral("设备已挂载，但未提供挂载路径"));
                });
                waitTimer->start();
            } else {
                emit placeActivated(mountedPath);
            }
        }
        emit deviceOperationFinished(id, mounting, success, error);
    };
    if (mounting)
        m_deviceProvider->mountAsync(id, this, completed);
    else
        m_deviceProvider->unmountAsync(id, this, completed);
    return true;
}

QStandardItem *QtFileMgrPlacesPanel::createGroupItem(const QString &name)
{
    auto *item = new QStandardItem(name);
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    // enabled=true 但不可选, 这样右键菜单可以响应
    QFont f = item->font();
    f.setBold(true);
    item->setFont(f);
    return item;
}

QStandardItem *QtFileMgrPlacesPanel::createPlaceItem(const QString &name, const QString &path,
                                                      const QString &iconName, const QString &tooltip)
{
    auto *item = new QStandardItem(name);
    item->setData(path, Qt::UserRole);
    item->setData(iconName, Qt::UserRole + 3);
    item->setToolTip(tooltip.isEmpty() ? path : tooltip);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    QStyle::StandardPixmap fallback = QStyle::SP_DirIcon;
    if (iconName == QStringLiteral("user-home"))
        fallback = QStyle::SP_DirHomeIcon;
    else if (iconName == QStringLiteral("user-desktop"))
        fallback = QStyle::SP_DesktopIcon;
    else if (iconName == QStringLiteral("folder-root"))
        fallback = QStyle::SP_ComputerIcon;
    else if (iconName == QStringLiteral("drive-harddisk"))
        fallback = QStyle::SP_DriveHDIcon;
    else if (iconName == QStringLiteral("user-trash")
             || iconName == QStringLiteral("user-trash-full"))
        fallback = QStyle::SP_TrashIcon;
    item->setIcon(QIcon::fromTheme(iconName,
                                   QApplication::style()->standardIcon(fallback)));
    return item;
}

QStandardItem *QtFileMgrPlacesPanel::addGroup(const QString &name)
{
    QStandardItem *group = createGroupItem(name);
    m_model->appendRow(group);
    return group;
}

void QtFileMgrPlacesPanel::removeGroup(QStandardItem *group)
{
    if (!group) return;
    m_model->removeRow(group->row());
}

void QtFileMgrPlacesPanel::registerNetworkPlace(const QString &name, const QString &path,
                                                  const QString &icon)
{
    if (name.isEmpty() || path.isEmpty()) {
        qWarning("[PlacesPanel] network register: rejected invalid place name='%s' path='%s'",
                 qPrintable(name), qPrintable(path));
        return;
    }

    for (PlaceEntry &entry : m_networkPlaces) {
        if (samePlacePath(entry.path, path)) {
            qWarning("[PlacesPanel] network dedup: updating registered place '%s' (%s)",
                     qPrintable(name), qPrintable(path));
            entry.name = name;
            entry.path = path;
            entry.iconTheme = icon;
            buildModel();
            expandAll();
            return;
        }
    }

    PlaceEntry entry;
    entry.name = name;
    entry.path = path;
    entry.iconTheme = icon;
    m_networkPlaces.append(entry);
    QStandardItem *item = createPlaceItem(name, path, icon);
    item->setData(m_persistentNetworkPaths.contains(normalizedPlacePath(path)),
                  Qt::UserRole + 22);
    m_networkGroup->appendRow(item);
    expand(m_networkGroup->index());
}

void QtFileMgrPlacesPanel::registerPersistentNetworkPlace(
    const QString &name, const QString &path, const QString &icon)
{
    const QUrl url(path);
    if (!url.isValid() || url.scheme().toLower() != QStringLiteral("ssh")
        || url.host().isEmpty()) {
        qWarning("[PlacesPanel] persistent network register: rejected invalid SSH place name='%s' path='%s'",
                 qPrintable(name), qPrintable(path));
        emit errorMessage(QStringLiteral("SSH 网络位置无效"));
        return;
    }
    m_persistentNetworkPaths.insert(normalizedPlacePath(path));
    registerNetworkPlace(name, path, icon);

    QList<PlaceEntry> persisted;
    for (const PlaceEntry &entry : qAsConst(m_networkPlaces)) {
        if (m_persistentNetworkPaths.contains(
                normalizedPlacePath(entry.path))) {
            persisted.append(entry);
        }
    }
    QtFileMgrConfigStore::saveNetworkPlaces(persisted);
    buildModel();
    expandAll();
}

void QtFileMgrPlacesPanel::unregisterNetworkPlace(const QString &path)
{
    const QString normalizedTarget = normalizedPlacePath(path);
    // 先确认位置存在再移除持久化标记/凭据:旧顺序下删除一个
    // 未注册的路径也会把同名 SSH 凭据抹掉 (DLG-P2-13)
    bool exists = false;
    for (const PlaceEntry &entry : qAsConst(m_networkPlaces)) {
        if (samePlacePath(entry.path, path)) {
            exists = true;
            break;
        }
    }
    if (!exists && m_networkGroup) {
        for (int i = 0; i < m_networkGroup->rowCount(); ++i) {
            QStandardItem *child = m_networkGroup->child(i);
            if (child && samePlacePath(child->data(Qt::UserRole).toString(), path)) {
                exists = true;
                break;
            }
        }
    }
    if (!exists) {
        qWarning("[PlacesPanel] network unregister: no registered place for '%s', persistent marker and credential kept",
                 qPrintable(path));
        return;
    }

    const bool persistent = m_persistentNetworkPaths.remove(normalizedTarget) > 0;
    // 位置删除时同步清除保存的 SSH 凭据
    const QUrl url(path);
    if (url.scheme().toLower() == QStringLiteral("ssh") && !url.host().isEmpty()
        && !url.userName().isEmpty()) {
#if QTFILEMGR_WITH_SSH
        QtFileMgrConfigStore::removeSshCredential(
            url.userName(), url.host(), url.port(22));
#endif
    }
    bool removed = false;
    for (int i = 0; i < m_networkPlaces.size(); ++i) {
        if (samePlacePath(m_networkPlaces.at(i).path, path)) {
            m_networkPlaces.removeAt(i);
            removed = true;
            break;
        }
    }
    if (!m_networkGroup) {
        if (!removed) {
            qWarning("[PlacesPanel] network unregister: no registered place for '%s'",
                     qPrintable(path));
        }
        return;
    }
    for (int i = 0; i < m_networkGroup->rowCount(); i++) {
        QStandardItem *child = m_networkGroup->child(i);
        if (child && samePlacePath(child->data(Qt::UserRole).toString(), path)) {
            m_networkGroup->removeRow(i);
            removed = true;
            break;
        }
    }
    if (!removed) {
        qWarning("[PlacesPanel] network unregister: no registered place for '%s'",
                 qPrintable(path));
    }
    if (persistent) {
        QList<PlaceEntry> persisted;
        for (const PlaceEntry &entry : qAsConst(m_networkPlaces)) {
            if (m_persistentNetworkPaths.contains(
                    normalizedPlacePath(entry.path))) {
                persisted.append(entry);
            }
        }
        QtFileMgrConfigStore::saveNetworkPlaces(persisted);
    }
}

QStandardItem *QtFileMgrPlacesPanel::addPlace(QStandardItem *group, const QString &name,
                                                const QString &path, const QString &iconName)
{
    if (!group) return nullptr;
    QStandardItem *item = createPlaceItem(name, path, iconName);
    group->appendRow(item);
    return item;
}

QList<QAction *> QtFileMgrPlacesPanel::customContextMenuActions() const
{
    return m_customActions;
}

void QtFileMgrPlacesPanel::setCustomContextMenuActions(const QList<QAction *> &actions)
{
    m_customActions = actions;
}

void QtFileMgrPlacesPanel::onContextMenu(const QPoint &pos)
{
    QModelIndex idx = indexAt(pos);
    if (!idx.isValid())
        return;

    QStandardItem *item = m_model->itemFromIndex(idx);
    if (!item)
        return;
    const QString path = item->data(Qt::UserRole).toString();
    const QString deviceId = item->data(Qt::UserRole + 10).toString();
    const bool isTrash = item->data(Qt::UserRole + 20).toBool();

    QMenu menu;
    if (isTrash) {
        menu.addAction(QStringLiteral("打开回收站"), this, [this]() {
            emit trashRequested();
        });
    } else if (path.isEmpty() && deviceId.isEmpty()) {
        if (item->text() != QStringLiteral("位置"))
            return;

        // 菜单/输入框嵌套事件循环期间模型可能重建,
        // 只保存稳定的组名,执行时重新定位 (R2-08)
        const QString groupName = item->text();
        menu.addAction(QStringLiteral("添加位置..."), this, [this, groupName]() {
            QStandardItem *group = nullptr;
            for (int i = 0; i < m_model->rowCount(); ++i) {
                QStandardItem *candidate = m_model->item(i);
                if (candidate && candidate->text() == groupName) {
                    group = candidate;
                    break;
                }
            }
            if (!group) {
                qWarning("[PlacesPanel] add place: group \"%s\" no longer exists after model rebuild",
                         qPrintable(groupName));
                return;
            }
            bool ok;
            QString name = QInputDialog::getText(this, QStringLiteral("添加位置"),
                QStringLiteral("名称:"), QLineEdit::Normal, QString(), &ok);
            if (!ok || name.isEmpty()) return;
            QString p = QInputDialog::getText(this, QStringLiteral("添加位置"),
                QStringLiteral("路径:"), QLineEdit::Normal, QDir::homePath(), &ok);
            if (!ok || p.isEmpty()) return;
            if (!isRemotePlacePath(p)) {
                p = normalizedPlacePath(p);
                if (!QFileInfo::exists(p)) {
                    emit errorMessage(QStringLiteral("位置不存在: %1").arg(p));
                    return;
                }
            }
            if (modelContainsPlacePath(m_model, p)) {
                emit errorMessage(QStringLiteral("位置已存在: %1").arg(p));
                return;
            }
            QStandardItem *newItem = createPlaceItem(name, p, QStringLiteral("folder"));
            newItem->setData(true, Qt::UserRole + 2);
            group->appendRow(newItem);
            saveCustomPlaces(m_model);
        });
    } else {
        const bool customPlace = item->data(Qt::UserRole + 2).toBool();
        const bool persistentNetwork = item->data(Qt::UserRole + 22).toBool();
        const bool isRemovable = customPlace || persistentNetwork;
        if (!deviceId.isEmpty() && m_deviceProvider) {
            if (item->data(Qt::UserRole + 12).toBool()) {
                menu.addAction(QStringLiteral("挂载"), this, [this, deviceId]() {
                    mountDevice(deviceId, false);
                });
            }
            if (item->data(Qt::UserRole + 13).toBool()) {
                menu.addAction(QStringLiteral("卸载"), this, [this, deviceId]() {
                    unmountDevice(deviceId);
                });
            }
        }
        if (!path.isEmpty()) {
            menu.addAction(QStringLiteral("打开"), this, [this, path]() {
                emit placeActivated(path);
            });
            menu.addAction(QStringLiteral("复制路径"), this, [path]() {
                QApplication::clipboard()->setText(path);
            });
        }
        menu.addSeparator();
        if (isRemovable) {
            // 只捕获稳定值;模型项在执行时重新定位 (R2-08)
            const QString editPath = path;
            const QString editName = item->text();
            menu.addAction(QStringLiteral("编辑位置..."), this,
                           [this, editPath, editName, persistentNetwork]() {
                if (!findPlaceItemByPath(m_model, editPath)) {
                    qWarning("[PlacesPanel] edit place: entry \"%s\" no longer exists after model rebuild",
                             qPrintable(editPath));
                    return;
                }
                bool ok = false;
                const QString oldPath = editPath;
                QString name = QInputDialog::getText(
                    this, QStringLiteral("编辑位置"), QStringLiteral("名称:"),
                    QLineEdit::Normal, editName, &ok).trimmed();
                if (!ok || name.isEmpty())
                    return;
                QString path = QInputDialog::getText(
                    this, QStringLiteral("编辑位置"), QStringLiteral("路径:"),
                    QLineEdit::Normal, oldPath, &ok).trimmed();
                if (!ok || path.isEmpty())
                    return;
                if (!isRemotePlacePath(path)) {
                    path = normalizedPlacePath(path);
                    if (!QFileInfo::exists(path)) {
                        emit errorMessage(QStringLiteral("位置不存在: %1").arg(path));
                        return;
                    }
                }
                if (persistentNetwork) {
                    const QUrl editedUrl(path);
                    if (editedUrl.scheme().toLower() != QStringLiteral("ssh")
                        || editedUrl.host().isEmpty()) {
                        qWarning("[PlacesPanel] edit persistent network place: rejected non-SSH URL (old=\"%s\", new=\"%s\")",
                                 qPrintable(oldPath), qPrintable(path));
                        emit errorMessage(QStringLiteral(
                            "持久化网络位置必须是有效的 SSH URL"));
                        return;
                    }
                }
                if (!samePlacePath(path, oldPath)
                    && modelContainsPlacePath(m_model, path)) {
                    emit errorMessage(QStringLiteral("位置已存在: %1").arg(path));
                    return;
                }
                // 对话框期间模型可能再次重建,再次定位 (R2-08)
                QStandardItem *item = findPlaceItemByPath(m_model, oldPath);
                if (!item) {
                    qWarning("[PlacesPanel] edit place: entry \"%s\" disappeared during the edit dialog",
                             qPrintable(oldPath));
                    return;
                }
                item->setText(name);
                item->setData(path, Qt::UserRole);
                item->setToolTip(path);
                if (persistentNetwork) {
                    for (PlaceEntry &entry : m_networkPlaces) {
                        if (samePlacePath(entry.path, oldPath)) {
                            entry.name = name;
                            entry.path = path;
                            break;
                        }
                    }
                    m_persistentNetworkPaths.remove(
                        normalizedPlacePath(oldPath));
                    m_persistentNetworkPaths.insert(
                        normalizedPlacePath(path));
                    QList<PlaceEntry> persisted;
                    for (const PlaceEntry &entry : qAsConst(m_networkPlaces)) {
                        if (m_persistentNetworkPaths.contains(
                                normalizedPlacePath(entry.path))) {
                            persisted.append(entry);
                        }
                    }
                    QtFileMgrConfigStore::saveNetworkPlaces(persisted);
                } else {
                    saveCustomPlaces(m_model);
                }
            });
            menu.addAction(QStringLiteral("从位置中移除"), this,
                           [this, path, persistentNetwork]() {
                if (persistentNetwork) {
                    unregisterNetworkPlace(path);
                } else {
                    // 按路径重新定位,不使用可能已失效的 item (R2-08)
                    QStandardItem *item = findPlaceItemByPath(m_model, path);
                    if (!item) {
                        qWarning("[PlacesPanel] remove place: entry \"%s\" no longer exists after model rebuild",
                                 qPrintable(path));
                        return;
                    }
                    m_model->removeRow(item->row(), item->index().parent());
                    saveCustomPlaces(m_model);
                }
                emit placeRemoved(path);
            });
        }
        const bool deviceMounted = item->data(Qt::UserRole + 11).toBool();
        const qint64 deviceAvailable = item->data(Qt::UserRole + 14).toLongLong();
        const qint64 deviceTotal = item->data(Qt::UserRole + 15).toLongLong();
        const QString itemName = item->text();
        menu.addAction(QStringLiteral("属性"), this,
                       [path, deviceId, deviceMounted, deviceAvailable,
                        deviceTotal, itemName]() {
            QString info;
            if (!deviceId.isEmpty()) {
                info = QStringLiteral("设备: %1\n标识: %2\n状态: %3")
                    .arg(itemName, deviceId,
                         deviceMounted ? QStringLiteral("已挂载")
                                       : QStringLiteral("未挂载"));
                if (!path.isEmpty())
                    info += QStringLiteral("\n路径: %1").arg(path);
                if (deviceTotal >= 0)
                    info += QStringLiteral("\n容量: %1").arg(formatFileSize(deviceTotal));
                if (deviceAvailable >= 0)
                    info += QStringLiteral("\n可用: %1").arg(formatFileSize(deviceAvailable));
            } else if (isRemotePlacePath(path)) {
                info = QStringLiteral("路径: %1\n类型: 网络位置").arg(path);
            } else {
                QFileInfo fi(path);
                QStorageInfo storage(fi.absoluteFilePath());
                info = QStringLiteral("路径: %1\n类型: %2")
                    .arg(fi.absoluteFilePath(), fi.isDir() ? QStringLiteral("文件夹") : QStringLiteral("文件"));
                if (storage.isValid() && storage.isReady()) {
                    info += QStringLiteral("\n可用: %1").arg(formatFileSize(storage.bytesAvailable()));
                }
            }
            QMessageBox::information(nullptr, QStringLiteral("位置属性"), info);
        });
    }
    if (!m_customActions.isEmpty()) {
        menu.addSeparator();
        for (QAction *action : qAsConst(m_customActions)) {
            if (action)
                menu.addAction(action);
        }
    }
    menu.exec(viewport()->mapToGlobal(pos));
}

void QtFileMgrPlacesPanel::onItemClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    QStandardItem *item = m_model->itemFromIndex(index);
    if (!item)
        return;

    QString path = item->data(Qt::UserRole).toString();
    if (item->data(Qt::UserRole + 20).toBool()) {
        emit trashRequested();
        return;
    }
    const QString deviceId = item->data(Qt::UserRole + 10).toString();
    if (!deviceId.isEmpty()) {
        if (m_pendingDeviceOperations.contains(deviceId)) {
            qWarning("[PlacesPanel] device click: ignored pending device (id=\"%s\")",
                     qPrintable(deviceId));
            return;
        }
        if (item->data(Qt::UserRole + 11).toBool() && !path.isEmpty()) {
            emit placeActivated(path);
        } else if (item->data(Qt::UserRole + 12).toBool()) {
            mountDevice(deviceId, true);
        } else {
            qWarning("[PlacesPanel] device click: device is not accessible (id=\"%s\", mounted=%d, path=\"%s\")",
                     qPrintable(deviceId),
                     item->data(Qt::UserRole + 11).toBool() ? 1 : 0,
                     qPrintable(path));
            emit errorMessage(QStringLiteral("设备尚未挂载"));
        }
        return;
    }
    if (!path.isEmpty() && (item->flags() & Qt::ItemIsSelectable)) {
        emit placeActivated(path);
    }
}

// ─── 配置驱动方法 ──────────────────────────────────────────

void QtFileMgrPlacesPanel::setShowDevices(bool show)
{
    if (m_showDevices != show) {
        m_showDevices = show;
        buildModel();
        expandAll();
    }
}

void QtFileMgrPlacesPanel::setDeviceRefreshMs(int ms)
{
    m_deviceRefreshMs = ms;
    if (m_deviceTimer) {
        m_deviceTimer->setInterval(ms);
    }
}

void QtFileMgrPlacesPanel::setConfigPlaces(const QList<PlaceEntry> &places)
{
    m_configPlaces = places;
    deduplicatePersistedCustomPlaces();
    // 配置驱动位置变化须即时生效,不再依赖调用方补 buildModel (R2-WS-P2-11)
    buildModel();
    expandAll();
}

void QtFileMgrPlacesPanel::deduplicatePersistedCustomPlaces()
{
    if (m_migrationDone)
        return;
    m_migrationDone = true;
    // 有效默认集合: 配置为空时 buildModel 用硬编码默认项
    QStringList defaults;
    if (m_configPlaces.isEmpty()) {
        const QString homePath = QDir::homePath();
        defaults = QStringList{homePath,
                    homePath + QStringLiteral("/Desktop"),
                    homePath + QStringLiteral("/Documents"),
                    homePath + QStringLiteral("/Downloads"),
                    QStringLiteral("/")};
    } else {
        for (const PlaceEntry &entry : m_configPlaces)
            defaults.append(entry.path);
    }
    QSet<QString> defaultKeys;
    for (const QString &path : defaults)
        defaultKeys.insert(normalizedPlacePath(path));
    const QList<PlaceEntry> customs =
        QtFileMgrConfigStore::loadCustomPlaces();
    QList<PlaceEntry> kept;
    bool droppedAny = false;
    for (const PlaceEntry &entry : customs) {
        if (defaultKeys.contains(normalizedPlacePath(entry.path))) {
            qWarning("[PlacesPanel] migration: dropped custom place duplicating a built-in default (name=\"%s\", path=\"%s\")",
                     qPrintable(entry.name), qPrintable(entry.path));
            droppedAny = true;
        } else {
            kept.append(entry);
        }
    }
    if (droppedAny)
        QtFileMgrConfigStore::saveCustomPlaces(kept);
}

// ─── URL 同步: 反向查找 + 高亮 ─────────────────────────────

PlaceMatch QtFileMgrPlacesPanel::findPlaceForPath(const QString &path) const
{
    PlaceMatch best;

    // 递归遍历 model, 找最长前缀匹配
    int bestNormLen = -1;  // 最佳匹配的标准化路径长度
    std::function<void(QStandardItem *, const QString &)> walk =
        [&](QStandardItem *item, const QString &prefix) {
        for (int i = 0; i < item->rowCount(); i++) {
            QStandardItem *child = item->child(i);
            if (!child || !child->isSelectable()) continue;
            QString placePath = child->data(Qt::UserRole).toString();
            if (placePath.isEmpty()) continue;
            QString normPlace = normalizedPlacePath(placePath);

            if (pathBelongsToPlace(path, placePath)) {
                if ((int)normPlace.size() > bestNormLen) {
                    bestNormLen = normPlace.size();
                    best.name = child->text();
                    best.rootPath = placePath;
                    best.found = true;
                }
            }
        }
    };

    // 遍历顶层 groups
    for (int g = 0; g < m_model->rowCount(); g++) {
        QStandardItem *group = m_model->item(g);
        if (group) walk(group, QString());
    }

    return best;
}

PlaceMatch QtFileMgrPlacesPanel::selectPlaceForPath(const QString &path)
{
    m_lastPlacePath = path;
    PlaceMatch match = findPlaceForPath(path);

    selectionModel()->clearSelection();
    if (!match.found) return match;

    // 遍历找匹配项并选中
    std::function<bool(QStandardItem *)> walk = [&](QStandardItem *item) -> bool {
        for (int i = 0; i < item->rowCount(); i++) {
            QStandardItem *child = item->child(i);
            if (!child || !child->isSelectable()) continue;
            QString placePath = child->data(Qt::UserRole).toString();
            if (placePath == match.rootPath) {
                QModelIndex idx = m_model->indexFromItem(child);
                if (idx.isValid()) {
                    selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
                    setCurrentIndex(idx);
                    scrollTo(idx, QAbstractItemView::EnsureVisible);
                    return true;
                }
            }
        }
        return false;
    };

    for (int g = 0; g < m_model->rowCount(); g++) {
        QStandardItem *group = m_model->item(g);
        if (group && walk(group)) break;
    }
    return match;
}
