// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRWIDGET_H
#define QTFILEMGRWIDGET_H

#include "QtFileMgrFeatures.h"
#include <QWidget>
#include <QUrl>
#include <QDir>
#include <QPointer>
#include <QStringList>
#include <QList>
#include <QKeySequence>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

class QtFileMgrPlacesPanel;
class QtFileMgrBreadcrumbBar;
class QtFileMgrFileView;
class QtFileMgrFilterBar;
class QtFileMgrHamburgerMenu;
class QtFileMgrIO;
class QtFileMgrPreviewPanel;
class QtFileMgrProtocolProvider;
class QSplitter;
class QToolBar;
class QStatusBar;
class QActionGroup;
class QSlider;
class QLabel;
class QAction;
class QToolButton;

// ---- 客制化配置 ----
struct PlaceEntry {
    QString name;
    QString path;
    QString iconTheme;
};

struct QtFileMgrConfig {
    // === 路径 ===
    QString rootPath = QDir::homePath();
    QStringList allowedPaths;

    // === Places 面板 ===
    QList<PlaceEntry> places;
    bool showDevices = true;
    int deviceRefreshMs = 30000;

    // === 视图 ===
    int defaultViewMode = 0;    // 0=Icons, 1=Compact, 2=Details
    bool rememberMode = false;

    // === View 子配置 ===
    struct ViewConfig {
        bool showHiddenFiles = false;
        bool foldersFirst = true;
        int zoomLevel = 2;                       // 0-16, 默认 32px
        int sortColumn = 0;                      // 0=名称, 1=大小, 2=类型, 3=日期
        Qt::SortOrder sortOrder = Qt::AscendingOrder;
        bool alternatingRowColors = true;
        QList<int> detailColumnWidths;
    } view;

    // === 权限 ===
    bool allowCreate = true;
    bool allowDelete = true;
    bool allowRename = true;
    bool allowDragDrop = true;

    // === 外观: 工具栏 ===
    struct ToolbarItemConfig {
        bool viewModeIcons = true;
        bool viewModeCompact = true;
        bool viewModeDetails = true;
        bool showHiddenFiles = true;
        bool showFoldersFirst = true;
        bool breadcrumb = true;
        bool settingsButton = true;
    };
    struct ToolbarConfig {
        int iconSize = 18;
        ToolbarItemConfig items;
    } toolbar;

    // === 外观: 状态栏 ===
    struct StatusBarConfig {
        bool visible = true;
        bool showZoomSlider = true;
        bool showDiskSpace = true;
        bool showItemCount = true;
    } statusBar;

    // === 外观: 面板 ===
    struct PlacesPanelConfig {
        int width = 280;
        int iconSize = 22;
    } placesPanel;
    int splitterHandleWidth = 4;

    // === 功能: 过滤栏 ===
    struct FilterBarConfig {
        bool visible = false;
        int defaultMode = 0;                     // 0=PlainText, 1=Glob, 2=Regex
        bool caseSensitive = false;
    } filterBar;

    // === 版本/缩略图 ===
    bool showVersions = false;
    bool showThumbnails = false;

    // === JSON 加载/保存 ===
    static QtFileMgrConfig defaultConfig();
    static QtFileMgrConfig loadFromFile(const QString &filePath);
    static QtFileMgrConfig loadFromJson(const QByteArray &json);
    QJsonObject toJsonObject() const;
    QByteArray toJson() const;

private:
    void mergeJsonObject(const QJsonObject &obj);
    static QList<PlaceEntry> parsePlaceArray(const QJsonArray &arr);
    static Qt::SortOrder parseSortOrder(const QString &s);
    static QString sortOrderToString(Qt::SortOrder order);
};

// ---- Widget ----
namespace Ui { class QtFileMgrWidget; }

class QtFileMgrWidget : public QWidget {
    Q_OBJECT
public:
    explicit QtFileMgrWidget(QWidget *parent = nullptr);
    explicit QtFileMgrWidget(const QtFileMgrConfig &cfg, QWidget *parent = nullptr);
    explicit QtFileMgrWidget(const QtFileMgrConfig &cfg, QtFileMgrIO *io, QWidget *parent = nullptr);
    explicit QtFileMgrWidget(const QString &configPath, QWidget *parent = nullptr);
    ~QtFileMgrWidget() override;

    QUrl url() const;
    void setUrl(const QUrl &url);
    void setIO(QtFileMgrIO *io);
    // Provider must outlive outstanding callbacks; destroyed providers are
    // ignored. Passing nullptr unregisters the scheme.
    void setProtocolProvider(const QString &scheme, QtFileMgrProtocolProvider *provider);
    QtFileMgrIO *io() const;
    bool canGoBack() const;
    bool canGoForward() const;
    QtFileMgrFileView *fileView() const;
    QtFileMgrPlacesPanel *placesPanel() const;
    QtFileMgrHamburgerMenu *hamburgerMenu() const { return m_hamburgerMenu; }
    QToolButton *settingsButton() const { return m_settingsBtn; }
    QtFileMgrBreadcrumbBar *breadcrumbBar() const { return m_breadcrumb; }

    // 共享 Places 面板模式 (Workspace 注入一份全局面板; 内嵌面板隐藏,
    // 高亮/设置应用/位置注册等交互一律走共享实例, 对齐 Dolphin 布局)
    void setSharedPlacesPanel(QtFileMgrPlacesPanel *panel);
    QtFileMgrPlacesPanel *activePlacesPanel() const;

    // 右侧面板 (版本信息/缩略图等, 与文件区域通过 splitter 分隔)
    void setSidePanel(QWidget *panel);
    void showSidePanel(bool show);
    bool isSidePanelVisible() const;

signals:
    void urlChanged(const QUrl &url);
    void fileActivated(const QString &filePath);
    void filesSelected(const QStringList &files);

public slots:
    void goBack();
    void goForward();
    void goUp();
    // 共享 Places 面板由 Workspace 路由到当前活动面板
    void onPlaceActivated(const QString &path);
    void showTrashDialog();

private slots:
    void onBreadcrumbUrlChanged(const QUrl &url);
    void onFileViewUrlChanged(const QUrl &url);
    void onZoomChanged(int value);
    void showIconView();
    void showCompactView();
    void showDetailsView();
    void toggleHiddenFiles();
    void toggleFoldersFirst();
    void toggleBreadcrumbEdit();
    void showSearchDialog();

private:
    void setupUi();
    void updateStatusBar();
    void applyConfig(const QtFileMgrConfig &cfg);
    void updateDiskSpace();
    bool navigateToUrl(const QUrl &url, bool addToHistory, bool updateFileView = true);
    void addUrlToHistory(const QUrl &url);
    void updateNavigationState();
    void synchronizeIO(QtFileMgrIO *io);
    void connectToProtocol(const QUrl &url);
    void invalidatePendingConnection(const char *reason);
    void clearPendingConnection();
    bool isUrlCompatibleWithIO(const QUrl &url, QtFileMgrIO *io) const;
    QUrl backendRootUrl(QtFileMgrIO *io) const;
    bool isUrlAllowed(const QUrl &url) const;
    void showPathRejected(const QUrl &url);
    bool isFileViewCommandContext() const;
    static QString formatDiskSize(qint64 bytes);

    Ui::QtFileMgrWidget *ui;
    QtFileMgrConfig m_config;
    QtFileMgrIO *m_io = nullptr;
    QPointer<QtFileMgrPlacesPanel> m_sharedPlacesPanel;  // 共享面板(Workspace 注入, 非拥有)
    QHash<QString, QPointer<QtFileMgrProtocolProvider>> m_protocolProviders;
    QHash<QString, QMetaObject::Connection> m_providerDestroyedConnections;
    quint64 m_connectionGeneration = 0;
    quint64 m_pendingConnectGeneration = 0;
    QPointer<QtFileMgrProtocolProvider> m_pendingConnectProvider;
    QUrl m_pendingConnectUrl;  // 仅对当前 provider/代次的在途连接去重
    QUrl m_appliedRootUrl;     // 上次 applyConfig 应用成功的配置根(审查#15)
    QString m_pendingConnectAuthority;   // ssh 在途连接按 user@host:port 归并
    QUrl m_deferredNavigationUrl;        // 归并期间新到的同主机目标,连通后导航
    QUrl m_url;
    QList<QUrl> m_history;
    int m_historyIndex = -1;

    // 快捷指针 (ui->setupUi 后赋值)
    QSplitter *m_splitter;
    QtFileMgrPlacesPanel *m_placesPanel;
    QtFileMgrFileView *m_fileView;
    QtFileMgrFilterBar *m_filterBar;
    QStatusBar *m_statusBar;

    QtFileMgrBreadcrumbBar *m_breadcrumb = nullptr;
    QSplitter *m_fileViewSplitter = nullptr;
    QWidget *m_fileViewSidePanel = nullptr;
    QPointer<QtFileMgrPreviewPanel> m_previewPanel;  // 替换面板后自动置空 (审查#10)
    QSlider *m_zoomSlider = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_diskSpaceLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;

    QAction *m_hiddenFilesAction = nullptr;
    QAction *m_foldersFirstAction = nullptr;
    QAction *m_iconViewAction = nullptr;
    QAction *m_compactViewAction = nullptr;
    QAction *m_detailViewAction = nullptr;
    QToolButton *m_settingsBtn = nullptr;
    QToolButton *m_backBtn = nullptr;
    QToolButton *m_forwardBtn = nullptr;
    QToolButton *m_upBtn = nullptr;
    QtFileMgrHamburgerMenu *m_hamburgerMenu = nullptr;
    bool m_updatingUrl = false;
};

#endif
