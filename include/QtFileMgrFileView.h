// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRFILEVIEW_H
#define QTFILEMGRFILEVIEW_H

#include <QWidget>
#include <QUrl>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <functional>

class QStackedWidget;
class QListView;
class QTableView;
class QLabel;
class QRubberBand;
class QFrame;
class QLabel;
class QAbstractItemView;
class QMenu;
class QFileSystemWatcher;
class QtFileMgrItemDelegate;
class QtFileMgrFlatModel;
class QtFileMgrIO;
class QtFileMgrLocalIO;
class QtFileMgrFileOperationJob;
struct QtFileMgrConfig;

#include <QPersistentModelIndex>
#include <QItemSelection>

// 参考 DolphinView 三视图模式 (Icons/Compact/Details)
class QtFileMgrFileView : public QWidget {
    Q_OBJECT
public:
    enum ViewMode { IconsView = 0, CompactView, DetailsView };

    explicit QtFileMgrFileView(QWidget *parent = nullptr);
    explicit QtFileMgrFileView(QtFileMgrIO *io, QWidget *parent = nullptr);

    // IO 注入 (nullptr = 默认 LocalIO)
    void setIO(QtFileMgrIO *io);
    QtFileMgrIO *io() const;

    // 跨后端传输/复用: 进程内已连接远端后端注册表
    // (粘贴/拖放查找源后端; Widget 复用同主机已连接后端)
    static void registerRemoteBackend(QtFileMgrIO *io);
    static void unregisterRemoteBackend(QtFileMgrIO *io);
    /// 清空注册表。供需要隔离连接语义的测试流程使用
    static void clearRemoteBackendRegistry();
    static QtFileMgrIO *remoteBackendForUrl(const QUrl &url);

    QUrl url() const;
    void setUrl(const QUrl &url);

    ViewMode viewMode() const;
    void setViewMode(ViewMode mode);

    void setShowHiddenFiles(bool show);
    bool showHiddenFiles() const;

    void setFoldersFirst(bool on);
    bool foldersFirst() const;
    void setSortColumn(int column, Qt::SortOrder order);

    void setZoomLevel(int level);
    int zoomLevel() const;
    int zoomLevelCount() const;

    QStringList selectedFiles() const;
    int itemCount() const;
    bool canUndo() const { return !m_undoOperations.isEmpty(); }
    int pendingOperationCount() const { return m_operationQueue.size(); }
    bool hasActiveOperation() const { return m_activeJob != nullptr || m_undoInProgress; }
    bool transferSelectedTo(const QString &destinationDirectory, bool move);

    QtFileMgrFlatModel *flatModel() const;
    QtFileMgrItemDelegate *delegate() const;
    void setConfig(const QtFileMgrConfig *cfg) { m_cfg = cfg; }
    const QtFileMgrConfig *config() const { return m_cfg; }

    // 右键菜单扩展 (集成项目注入自定义 QAction)
    void addContextMenuAction(QAction *action);
    // 动态扩展钩子 (集成项目根据 IO 模式/选中文件动态添加菜单项)
    using ContextMenuHook = std::function<void(QMenu &menu, const QStringList &selected, bool isRemote)>;
    void setContextMenuHook(ContextMenuHook hook) { m_contextMenuHook = std::move(hook); }
    using OpenWithHandler = std::function<bool(
        const QStringList &paths, const QString &mimeType, QWidget *parent)>;
    void setOpenWithHandler(OpenWithHandler handler) {
        m_openWithHandler = std::move(handler);
    }
    bool openWith(const QStringList &paths);
    bool createFileFromTemplate(const QString &templatePath,
                                QString *createdPath = nullptr);

    // 配置驱动方法
    void setAlternatingRowColors(bool on);
    void setDetailColumnWidths(const QList<int> &widths);
    QList<int> detailColumnWidths() const;
    void setShowThumbnails(bool show);
    void setShowVersions(bool show);

signals:
    void urlChanged(const QUrl &url);
    void fileActivated(const QString &filePath);
    void filesSelected(const QStringList &files);
    void statusMessage(const QString &msg);
    void zoomLevelChanged(int current, int previous);
    void showHiddenFilesChanged(bool show);
    void foldersFirstChanged(bool on);
    void sortChanged(int column, Qt::SortOrder order);
    void viewModeChanged(int mode);
    void ioChanged(QtFileMgrIO *io);
    void requestBreadcrumbEdit();
    void requestFilterToggle();
    void detailColumnWidthsChanged(const QList<int> &widths);
    void operationQueueChanged(int pendingCount);
    void undoAvailableChanged(bool available);
    void openWithRequested(const QStringList &paths, const QString &mimeType);

public slots:
    void goUp();
    void reload();
    void selectAll();
    void invertSelection();
    void applyNameFilter(const QString &text);
    // 一次性应用过滤模式/大小写/文本并保持选中 (避免多次 reset 逐次丢选中)
    void applyFilter(int filterMode, bool caseSensitive, const QString &text);
    void clearCachedSizes();
    void connectSelectionSignals();
    void deleteSelectedInternal(bool permanent);
    void showMessage(const QString &msg, const QString &type = QStringLiteral("info"));
    void renameCurrent();
    void deleteSelected();
    void deletePermanent();
    void cutSelected();
    void copySelected();
    void pasteFiles();
    void downloadSelected();
    void createNewFolder();
    void selectNewFiles(const QStringList &names);
    void observeCreatedFile(const QString &name);
    void focusCurrentView();
    void handleNavigationKey(int key, Qt::KeyboardModifiers modifiers);
    void undoLastOperation();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onDoubleClicked(const QModelIndex &index);
    void onSelectionChanged();
    void onContextMenu(const QPoint &pos);

private:
    void navigateTo(const QModelIndex &index);
    void updateStatus();
    void updateZoom();
    void optimizeDetailColumns();
    void updateContentState(const QString &text = QString());
    void updateDirectoryWatcher();
    void connectBackendSignals();
    bool backendPathAffectsCurrentDirectory(const QString &path) const;
    void scheduleReload();
    void applyPendingSelection();
    QString filePathAtIndex(const QModelIndex &proxyIdx) const;
    QString currentDirectoryPath() const;
    bool isDirectoryWritable(const QString &path) const;
    QAbstractItemView *currentView() const;
    void restoreSelection(const QStringList &selectedPaths, const QString &currentPath);
    // 在 mutator 引发的模型 reset 前后保存并按路径恢复选中 (R2-FV-P1-3)
    void preserveSelectionAround(const std::function<void()> &mutator);
    // 新建项选中落地后进入重命名编辑 (UI-P1-3)
    void tryStartPendingRename();
    // 跨后端传输: 进程内已连接远端后端注册表本体(私有, 经上方静态方法访问)
    static QList<QPointer<QtFileMgrIO>> s_remoteBackends;
    // 串行下载 sourceIO 的项目到本地目录;deleteSourcesAfter 用于跨后端移动
    void startSerialDownload(QtFileMgrIO *sourceIO, const QStringList &sources,
                             const QString &destinationDirectory,
                             bool deleteSourcesAfter);
    // 跨远端中转: 先下载到本地暂存目录再上传当前后端目标目录
    void startSerialRelay(QtFileMgrIO *sourceIO, const QStringList &sources,
                          const QString &destinationDirectory,
                          bool deleteSourcesAfter);
    void addSortActions(QMenu *menu);
    void updateSortIndicator();
    void showProperties(const QModelIndex &idx);
    QStringList availableTemplateFiles() const;
    int iconSizeForLevel(int level) const;
    int levelForIconSize(int size) const;
    bool startFileOperation(const QStringList &sources,
                            const QString &destinationDirectory,
                            bool move,
                            bool askAboutConflicts = true,
                            const QSet<QString> &importSources = QSet<QString>());
    void startNextFileOperation();
    void showVersionHistory(const QString &path);

    void setupRubberBand(QAbstractItemView *view);
    void beginRubberSelection(QAbstractItemView *view, const QPoint &pos);
    void updateRubberSelection(QAbstractItemView *view, const QPoint &pos);
    void endRubberSelection(QAbstractItemView *view);

    QUrl m_url;
    ViewMode m_mode;

    QtFileMgrFlatModel *m_flatModel;
    QtFileMgrIO *m_io;
    // 跨后端串行传输与任务队列互斥 (审查#8): 传输进行中不启动新任务,
    // 队列忙时传输延后
    int m_activeSerialTransfers = 0;
    QList<std::function<void()>> m_deferredTransfers;
    QtFileMgrItemDelegate *m_delegate;
    QStackedWidget *m_stack;
    QWidget *m_leftContainer;

    QListView *m_iconView;
    QListView *m_compactView;
    QTableView *m_detailView;

    QLabel *m_statusLabel;
    QLabel *m_contentStateLabel = nullptr;

    // 缩放
    int m_zoomLevel;

    // 框选
    QRubberBand *m_rubberBand;
    QPoint m_rubberOrigin;
    bool m_rubberActive;
    QAbstractItemView *m_rubberView;
    QItemSelection m_rubberBaseSelection;  // Ctrl 增选时按下瞬间的已有选择

    // 锚点 (Shift+Click 范围选择)
    QPersistentModelIndex m_anchorIndex;
    QPersistentModelIndex m_rootSourceIndex;
    QPersistentModelIndex m_savedFilterRoot;  // filterAboutToChange 时保存
    QStringList m_cutFiles;
    QTimer *m_resizeTimer = nullptr;
    QTimer *m_reloadTimer = nullptr;
    QTimer *m_messageHideTimer = nullptr;
    QTimer *m_pendingSelectionTimer = nullptr;
    QFileSystemWatcher *m_dirWatcher = nullptr;
    QFrame *m_messageBar = nullptr;
    QList<QAction*> m_extraContextActions;
    ContextMenuHook m_contextMenuHook;  // 动态右键菜单扩展钩子
    OpenWithHandler m_openWithHandler;
    QLabel *m_messageLabel = nullptr;
    QStringList m_pendingSelection;
    QUrl m_pendingSelectionUrl;
    QString m_pendingRenameName;  // 新建后待重命名的条目名
    QtFileMgrFileOperationJob *m_activeJob = nullptr;
    struct PendingOperation {
        QStringList sources;
        QString destinationDirectory;
        QStringList overwrittenNames;  // 目标位置已存在的同名项，撤销时不得删除
        QSet<QString> importSources;   // 本机来源(粘贴/拖放本地文件到远端),走显式上传
        bool move = false;
        int conflictPolicy = 0;
    };
    struct UndoOperation {
        struct Item {
            QString source;
            QString destination;
            QByteArray snapshot;
            // Exact proof at this move's source, linked to snapshot by the
            // execution worker. Empty means older records cannot be renewed.
            QByteArray sourceSnapshot;
            bool completed = false;
        };
        QList<Item> items;
        bool move = false;
    };
    QQueue<PendingOperation> m_operationQueue;
    QList<UndoOperation> m_undoOperations;
    bool m_undoInProgress = false;
    bool m_loaded = false;
    bool m_applyingColumnWidths = false;
    bool m_hasExplicitColumnWidths = false;
    const QtFileMgrConfig *m_cfg = nullptr;
};

#endif // QTFILEMGRFILEVIEW_H
