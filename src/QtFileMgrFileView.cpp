// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// 参考 DolphinView (views/dolphinview.cpp) + kitemviews/
// 三视图模式 + 委托 + 框选 + 缩放 + 排序
#include "QtFileMgrFileView.h"
#include "QtFileMgrItemDelegate.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrLocalIO.h"
#include "QtFileMgrFileOperationJob.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrPathPicker.h"
#include "QtFileMgrPropertiesDialog.h"
#include "QtFileMgrVersionsDialog.h"
#include "QtFileMgrDebug.h"
#include "QtFileMgrStyle.h"

#include <QStackedWidget>
#include <QSplitter>
#include <QListView>
#include <QTableView>
#include <QHeaderView>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QToolButton>
#include <QRubberBand>
#include <QMouseEvent>
#include <QDropEvent>
#include <QUrl>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QTextStream>
#include <QSet>
#include <QHash>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QMimeData>
#include <QDataStream>
#include <QApplication>
#include <QTimer>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QDebug>
#include <QStyle>
#include <QSettings>
#include <QThread>
#include <QPointer>
#include <QSharedPointer>
#include <functional>
#include <memory>
#include <utility>

static const int kMinIconSize = 16;
static const int kMaxIconSize = 256;
static const int kDefaultIconSize = 48;
static const int kZoomLevelCount = 17;

static QString joinedPath(const QString &dir, const QString &name)
{
    if (dir.isEmpty()) return name;
    if (dir.endsWith(QLatin1Char('/'))) return dir + name;
    return dir + QLatin1Char('/') + name;
}

static QString normalizedPlainPath(QString path)
{
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}

static bool isSameDirectoryPath(const QString &a, const QString &b)
{
    return normalizedPlainPath(a) == normalizedPlainPath(b);
}

static QString operationParentDirectory(QString path)
{
    path = normalizedPlainPath(path);
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash == 0)
        return QStringLiteral("/");
    return slash > 0 ? path.left(slash) : QString();
}

// Consume each request once, including a synchronous result or an abnormal
// disappearance. Queue continuations so synchronous backends cannot recurse
// through an entire batch, and so request deletion cannot reenter a new request.
using SerialTransferCompletion =
    std::function<void(bool, const QString &, const QString &, bool)>;

static void watchSerialTransfer(QObject *context, QtFileMgrTransferRequest *request,
                                QProgressDialog *progress, const QString &phase,
                                const QString &source, const QString &destination,
                                SerialTransferCompletion completion)
{
    const auto delivered = std::make_shared<bool>(false);
    const auto deliver = [context, delivered, completion, phase, source, destination](
        bool success, const QString &path, const QString &error, bool cancelled) {
        if (*delivered) {
            qWarning("[FileView] serial transfer: skipped duplicate completion (phase=\"%s\", source=\"%s\", destination=\"%s\")",
                     qPrintable(phase), qPrintable(source), qPrintable(destination));
            return;
        }
        *delivered = true;
        QTimer::singleShot(0, context, [completion, success, path, error, cancelled]() {
            completion(success, path, error, cancelled);
        });
    };
    if (!request) {
        qWarning("[FileView] serial transfer: backend returned no request (phase=\"%s\", source=\"%s\", destination=\"%s\")",
                 qPrintable(phase), qPrintable(source), qPrintable(destination));
        deliver(false, destination, QStringLiteral("文件后端未创建传输请求"), false);
        return;
    }
    QObject::connect(request, &QtFileMgrTransferRequest::finished, context, deliver);
    QObject::connect(request, &QObject::destroyed, context,
                     [delivered, deliver, phase, source, destination]() {
        // Deletion after a consumed result is normal request ownership cleanup.
        if (*delivered)
            return;
        qWarning("[FileView] serial transfer: request destroyed before completion (phase=\"%s\", source=\"%s\", destination=\"%s\")",
                 qPrintable(phase), qPrintable(source), qPrintable(destination));
        deliver(false, destination, QStringLiteral("传输请求在完成前被销毁"), false);
    });
    if (progress)
        QObject::connect(progress, &QProgressDialog::canceled,
                         request, &QtFileMgrTransferRequest::cancel, Qt::UniqueConnection);
    if (!*delivered && request->isFinished()) {
        qWarning("[FileView] serial transfer: consuming result completed before observation (phase=\"%s\", source=\"%s\", destination=\"%s\")",
                 qPrintable(phase), qPrintable(source), qPrintable(destination));
        deliver(request->succeeded(), request->resultPath(), request->errorString(),
                request->isCancelled());
    }
}

static bool serialViewAlive(const QPointer<QtFileMgrFileView> &view,
                            const QString &destination, const char *phase)
{
    if (view)
        return true;
    qWarning("[FileView] serial transfer: stopped after view destruction (phase=%s, destination=\"%s\")",
             phase, qPrintable(destination));
    return false;
}

QtFileMgrFileView::QtFileMgrFileView(QWidget *parent)
    : QtFileMgrFileView(nullptr, parent) {}

QtFileMgrFileView::QtFileMgrFileView(QtFileMgrIO *io, QWidget *parent)
    : QWidget(parent)
    , m_url(QUrl::fromLocalFile(QDir::homePath()))
    , m_mode(IconsView)
    , m_flatModel(nullptr)
    , m_delegate(nullptr)
    , m_stack(new QStackedWidget(this))
    , m_iconView(nullptr)
    , m_compactView(nullptr)
    , m_detailView(nullptr)
    , m_statusLabel(new QLabel(this))
    , m_zoomLevel(2)
    , m_rubberBand(nullptr)
    , m_rubberActive(false)
    , m_rubberView(nullptr)
    , m_resizeTimer(nullptr)
{
    auto *outerLayout = new QHBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // 不使用 QSplitter, 用 QHBoxLayout — 避免多余的灰色分隔把手
    m_leftContainer = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(m_leftContainer);
    mainLayout->setContentsMargins(10, 8, 10, 0);
    mainLayout->setSpacing(0);

    // --- I/O 层 + FlatModel + Delegate ---
    if (io && io->thread() != thread()) {
        qWarning("[FileView] constructor: rejected backend thread mismatch (backend=%s, backendThread=%p, viewThread=%p)",
                 io->metaObject()->className(),
                 static_cast<void *>(io->thread()),
                 static_cast<void *>(thread()));
        m_io = new QtFileMgrLocalIO(this);
    } else if (io) {
        m_io = io;
    } else {
        m_io = new QtFileMgrLocalIO(this);
    }
    registerRemoteBackend(m_io);
    m_flatModel = new QtFileMgrFlatModel(m_io, this);
    m_delegate = new QtFileMgrItemDelegate(this);
    // 异步重命名失败经消息条呈现 (DLG-P1-5)
    connect(m_delegate, &QtFileMgrItemDelegate::operationFailed,
            this, [this](const QString &message) {
        showMessage(message, QStringLiteral("error"));
    });

    // --- 创建三种视图 ---
    m_iconView = new QListView(m_leftContainer);
    m_compactView = new QListView(m_leftContainer);
    m_detailView = new QTableView(m_leftContainer);

    m_iconView->setModel(m_flatModel);
    m_compactView->setModel(m_flatModel);
    m_detailView->setModel(m_flatModel);

    // setModel 后重新连接 selectionChanged (setModel 可能替换 selectionModel)
    // FlatModel 是扁平模型, 无需 setRootIndex (root 始终是 QModelIndex())

    m_iconView->setViewMode(QListView::IconMode);
    m_compactView->setViewMode(QListView::ListMode);

    // 通用设置
    for (auto *v : {static_cast<QAbstractItemView *>(m_iconView),
                    static_cast<QAbstractItemView *>(m_compactView),
                    static_cast<QAbstractItemView *>(m_detailView)}) {
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 手动触发重命名
        v->setDragEnabled(true);
        v->setDragDropMode(QAbstractItemView::DragDrop);
        v->setDefaultDropAction(Qt::MoveAction);
        v->setDragDropOverwriteMode(false);
        v->setItemDelegate(m_delegate);
        v->setMouseTracking(true);             // 悬停效果
        v->setContextMenuPolicy(Qt::CustomContextMenu);
        v->setAutoScroll(true);
        v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        v->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        v->viewport()->installEventFilter(this);
        v->installEventFilter(this);
    }

    // Icon 模式设置
    m_iconView->setSpacing(6);
    m_iconView->setResizeMode(QListView::Adjust);
    m_iconView->setMovement(QListView::Free);  // Free 允许拖拽启动
    m_iconView->setFrameShape(QFrame::NoFrame);
    m_iconView->setWordWrap(true);
    m_iconView->setUniformItemSizes(true);

    // Compact 模式设置 — 参照 Dolphin: 纵向列布局, 水平滚动
    m_compactView->setSpacing(3);
    m_compactView->setMovement(QListView::Free);
    m_compactView->setFrameShape(QFrame::NoFrame);
    m_compactView->setWrapping(true);
    m_compactView->setFlow(QListView::TopToBottom);           // 先填满视口高度, 再溢出到右侧列
    m_compactView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_compactView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_compactView->setResizeMode(QListView::Adjust);           // 视口大小变化时重排
    m_compactView->setUniformItemSizes(true);                 // 统一行高, 加速布局
    m_compactView->setWordWrap(false);

    // Detail 模式设置
    m_detailView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailView->setShowGrid(false);
    m_detailView->setFrameShape(QFrame::NoFrame);
    m_detailView->verticalHeader()->setVisible(false);
    m_detailView->setAlternatingRowColors(true);

    {
        QHeaderView *header = m_detailView->horizontalHeader();
        header->setSectionsClickable(true);
        header->setSortIndicatorShown(true);
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        header->setFixedHeight(qMax(36, fontMetrics().height() + 16));
        QFont headerFont = header->font();
        headerFont.setWeight(QFont::DemiBold);
        header->setFont(headerFont);
        header->setMinimumSectionSize(20);
        header->setStretchLastSection(true);  // Date 自动填满
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->resizeSection(1, qMax(112, fontMetrics().horizontalAdvance(
                                  QStringLiteral("999.9 GB")) + 28));
        header->resizeSection(2, 100);
        connect(header, &QHeaderView::sectionClicked, this, [this](int section) {
            if (section < 0 || section >= m_flatModel->columnCount())
                return;
            Qt::SortOrder order = Qt::AscendingOrder;
            if (section == m_flatModel->sortColumn()) {
                order = m_flatModel->sortOrder() == Qt::AscendingOrder
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
            }
            setSortColumn(section, order);
        });
        connect(header, &QHeaderView::sectionResized, this,
                [this](int, int, int) {
            if (!m_applyingColumnWidths) {
                m_hasExplicitColumnWidths = true;
                emit detailColumnWidthsChanged(detailColumnWidths());
            }
        });
    }

    // 添加到 stack
    m_stack->addWidget(m_iconView);
    m_stack->addWidget(m_compactView);
    m_stack->addWidget(m_detailView);
    m_stack->setCurrentIndex(0);
    m_stack->installEventFilter(this);

    m_contentStateLabel = new QLabel(m_stack);
    m_contentStateLabel->setAlignment(Qt::AlignCenter);
    m_contentStateLabel->setWordWrap(true);
    m_contentStateLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_contentStateLabel->setStyleSheet(
        QStringLiteral("QLabel{color:palette(mid);background:palette(base);padding:24px;}"));
    m_contentStateLabel->hide();

    // 连接双击
    connect(m_iconView, &QListView::doubleClicked, this, &QtFileMgrFileView::onDoubleClicked);
    connect(m_compactView, &QListView::doubleClicked, this, &QtFileMgrFileView::onDoubleClicked);
    connect(m_detailView, &QTableView::doubleClicked, this, &QtFileMgrFileView::onDoubleClicked);


    // 右键菜单
    connect(m_iconView, &QListView::customContextMenuRequested,
            this, &QtFileMgrFileView::onContextMenu);
    connect(m_compactView, &QListView::customContextMenuRequested,
            this, &QtFileMgrFileView::onContextMenu);
    connect(m_detailView, &QTableView::customContextMenuRequested,
            this, &QtFileMgrFileView::onContextMenu);

    mainLayout->addWidget(m_stack, 1);

    // 内联消息条 (error/warning/info, 默认隐藏)
    m_messageBar = new QFrame(m_leftContainer);
    m_messageBar->setObjectName(QStringLiteral("fileViewMessageBar"));
    m_messageBar->setFrameShape(QFrame::StyledPanel);
    m_messageBar->setFixedHeight(28);
    auto *msgLay = new QHBoxLayout(m_messageBar);
    msgLay->setContentsMargins(6, 2, 6, 2);
    msgLay->setSpacing(4);
    m_messageLabel = new QLabel(m_messageBar);
    msgLay->addWidget(m_messageLabel, 1);
    QToolButton *msgClose = new QToolButton(m_messageBar);
    msgClose->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    msgClose->setAutoRaise(true);
    connect(msgClose, &QToolButton::clicked, m_messageBar, &QFrame::hide);
    msgLay->addWidget(msgClose);
    m_messageBar->hide();
    mainLayout->addWidget(m_messageBar);

    m_statusLabel->hide();

    m_dirWatcher = new QFileSystemWatcher(this);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);
    connect(m_reloadTimer, &QTimer::timeout, this, &QtFileMgrFileView::reload);
    m_messageHideTimer = new QTimer(this);
    m_messageHideTimer->setObjectName(QStringLiteral("fileViewMessageHideTimer"));
    m_messageHideTimer->setSingleShot(true);
    m_messageHideTimer->setInterval(5000);
    connect(m_messageHideTimer, &QTimer::timeout, m_messageBar, &QFrame::hide);
    m_pendingSelectionTimer = new QTimer(this);
    m_pendingSelectionTimer->setSingleShot(true);
    m_pendingSelectionTimer->setInterval(5000);
    connect(m_pendingSelectionTimer, &QTimer::timeout, this, [this]() {
        if (!m_pendingRenameName.isEmpty()) {
            qWarning("[FileView] pending rename: timed out waiting for '%s' to appear",
                     qPrintable(m_pendingRenameName));
            m_pendingRenameName.clear();
        }
        m_pendingSelection.clear();
        m_pendingSelectionUrl = QUrl();
    });
    connect(m_dirWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString &) { scheduleReload(); });
    connect(m_flatModel, &QtFileMgrFlatModel::loadingChanged,
            this, [this](bool loading) {
        m_loaded = !loading;
        updateStatus();
    });
    connect(m_flatModel, &QtFileMgrFlatModel::directoryBatchLoaded,
            this, [this](int, int) {
        updateStatus();
    });
    connect(m_flatModel, &QtFileMgrFlatModel::directoryLoadFinished,
            this, [this](const QString &, const QString &error) {
        m_loaded = true;
        if (!error.isEmpty())
            showMessage(error, QStringLiteral("error"));
        updateDirectoryWatcher();
        optimizeDetailColumns();
        updateStatus();
        applyPendingSelection();
        tryStartPendingRename();
    });

    // 文件视图占满；组合控件的侧栏由 QtFileMgrWidget 唯一管理
    outerLayout->addWidget(m_leftContainer, 1);

    connectSelectionSignals();
    connectBackendSignals();

    const auto applyAppearance = [this](const QPalette &themePalette) {
        const auto colors = QtFileMgrStyle::colors(themePalette);
        setStyleSheet(QStringLiteral(
            "QListView,QTableView{"
            "  background:%1;"
            "  alternate-background-color:%2;"
            "  border:0;"
            "  outline:0;"
            "}"
            "QListView::item,QTableView::item{"
            "  border:0;"
            "}"
            "QListView::item:selected,QTableView::item:selected{"
            "  background:%3;"
            "  color:%4;"
            "}"
            "QHeaderView::section{"
            "  background:%2;"
            "  color:%5;"
            "  border:0;"
            "  border-bottom:1px solid %6;"
            "  padding:0 14px;"
            "}"
            "QHeaderView::section:hover{background:%7;color:%4;}"
            "QLabel{background:palette(window);}"
            "#fileViewMessageBar{border-radius:5px;}")
            .arg(colors.base.name(), colors.surface.name(), colors.selection.name(),
                 colors.text.name(), colors.muted.name(), colors.border.name(),
                 colors.hover.name()));
        m_iconView->viewport()->update();
        m_compactView->viewport()->update();
        m_detailView->viewport()->update();
    };
    applyAppearance(palette());
    QtFileMgrStyle::observeApplicationPalette(this, applyAppearance);

    setUrl(m_url);
    setFocusPolicy(Qt::StrongFocus);
    updateSortIndicator();
    updateZoom();

}

// ---------- URL ----------
QUrl QtFileMgrFileView::url() const { return m_url; }

void QtFileMgrFileView::setUrl(const QUrl &url)
{
    QTMGR_DBG("setUrl: %s", qPrintable(url.toString()));
    const QString scheme = url.scheme().toLower();
    const bool isRemoteUrl = !scheme.isEmpty() && scheme != QStringLiteral("file");
    const bool isLocalUrl = scheme.isEmpty() || scheme == QStringLiteral("file");
    if (!isLocalUrl && !isRemoteUrl) {
        showMessage(QStringLiteral("不支持的路径协议: %1").arg(scheme),
                    QStringLiteral("error"));
        return;
    }
    if (m_io && !m_io->acceptsUrl(url)) {
        showMessage(QStringLiteral("路径协议与当前文件后端不匹配。"),
                    QStringLiteral("warning"));
        return;
    }
    if (m_url == url) {
        reload();
        return;
    }

    QString flatPath = m_io ? m_io->pathFromUrl(url)
                            : (isRemoteUrl ? url.path() : url.toLocalFile());
    // 远端路径标准化: "/"→"" , "/uif"→"uif" , 拒绝 "." (仅 remote:// 做此处理)
    const bool isApplicationRemote = (url.scheme() == QStringLiteral("remote"));
    if (isApplicationRemote) {
        if (flatPath == QStringLiteral(".") || flatPath == QStringLiteral("..")) {
            qWarning("[FileView] setUrl: rejected remote path '%s'", qPrintable(flatPath));
            return;
        }
        if (flatPath == QStringLiteral("/"))
            flatPath.clear();
        else if (flatPath.startsWith(QLatin1Char('/')))
            flatPath = flatPath.mid(1);
    }
    // ssh:// 保留绝对路径 (如 "/home/user/docs")
    m_pendingSelection.clear();
    m_pendingSelectionUrl = QUrl();
    m_pendingRenameName.clear();
    m_pendingSelectionTimer->stop();
    m_url = url;
    m_loaded = false;
    updateContentState(QStringLiteral("正在加载..."));
    m_flatModel->setUrl(flatPath);
    updateDirectoryWatcher();
    emit urlChanged(m_url);
}

void QtFileMgrFileView::preserveSelectionAround(const std::function<void()> &mutator)
{
    const QStringList selectedPaths = selectedFiles();
    const QModelIndex currentIndex = currentView()
        ? currentView()->currentIndex() : QModelIndex();
    const QString currentPath = currentIndex.data(Qt::UserRole).toString();
    mutator();
    restoreSelection(selectedPaths, currentPath);
}

void QtFileMgrFileView::applyNameFilter(const QString &text)
{
    if (!m_flatModel) {
        qWarning("[FileView] applyNameFilter: missing FlatModel for '%s'", qPrintable(text));
        return;
    }
    preserveSelectionAround([this, text]() {
        m_flatModel->setNameFilter(text);
    });
    updateStatus();
}

void QtFileMgrFileView::applyFilter(int filterMode, bool caseSensitive, const QString &text)
{
    if (!m_flatModel) {
        qWarning("[FileView] applyFilter: missing FlatModel for '%s'", qPrintable(text));
        return;
    }
    preserveSelectionAround([this, filterMode, caseSensitive, text]() {
        m_flatModel->setFilterMode(filterMode);
        m_flatModel->setCaseSensitive(caseSensitive);
        m_flatModel->setNameFilter(text);
    });
    updateStatus();
}

void QtFileMgrFileView::showMessage(const QString &msg, const QString &type)
{
    if (!m_messageBar || !m_messageLabel) return;
    m_messageLabel->setText(msg);
    // 直接设置整条消息栏的样式 (widget 级 stylesheet 优先于全局 QSS)
    const char *css = "QFrame#fileViewMessageBar{background-color:%1; border:1px solid #aaa; border-radius:2px;}"
                      "QFrame#fileViewMessageBar QLabel{color:#333;}";
    QString color;
    if (type == QStringLiteral("error"))      color = "#fce4e4";
    else if (type == QStringLiteral("warning")) color = "#fcf4dc";
    else if (type == QStringLiteral("info"))    color = "#dcecf4";
    else                                       color = "#e4ecfc";
    m_messageBar->setStyleSheet(QString(css).arg(color));
    m_messageBar->show();
    m_messageHideTimer->stop();
    if (type != QStringLiteral("error"))
        m_messageHideTimer->start();
}

bool QtFileMgrFileView::startFileOperation(const QStringList &sources,
                                            const QString &destinationDirectory,
                                            bool move,
                                            bool askAboutConflicts,
                                            const QSet<QString> &importSources)
{
    if (m_undoInProgress) {
        qWarning("[FileView] startFileOperation: refused while guarded undo is active (destination=\"%s\", sources=%d)",
                 qPrintable(destinationDirectory), static_cast<int>(sources.size()));
        showMessage(QStringLiteral("安全撤销正在进行，请稍后再开始文件操作。"), QStringLiteral("warning"));
        return false;
    }
    if (!m_io || sources.isEmpty())
        return false;
    // 权限闸: 粘贴/拖放此前在各自入口检查,双栏传输曾绕过 (审查#9);
    // 统一在共享入口校验,复制需 allowCreate,移动另需重命名+删除权限
    const bool allowedByConfig = move
        ? (!m_cfg || (m_cfg->allowCreate && m_cfg->allowRename
                      && m_cfg->allowDelete))
        : (!m_cfg || m_cfg->allowCreate);
    if (!allowedByConfig) {
        qWarning("[FileView] startFileOperation: rejected by configuration (move=%d, allowCreate=%d, allowRename=%d, allowDelete=%d, sources=%d)",
                 move ? 1 : 0, m_cfg ? m_cfg->allowCreate : -1,
                 m_cfg ? m_cfg->allowRename : -1,
                 m_cfg ? m_cfg->allowDelete : -1, sources.size());
        showMessage(QStringLiteral("当前配置不允许此文件操作。"),
                    QStringLiteral("warning"));
        return false;
    }
    // 目标是当前目录时冲突扫描直接用模型快照,远端不再逐文件 RPC (UI-P1-4);
    // 模型加载中快照不全,回退到逐文件后端查询
    const QString currentDirectory = m_io ? m_io->pathFromUrl(m_url)
                                          : m_url.toLocalFile();
    const bool scanFromModel = m_flatModel && !m_flatModel->isLoading()
        && isSameDirectoryPath(destinationDirectory, currentDirectory);
    QSet<QString> existingNames;
    if (scanFromModel) {
        const QList<FileEntry> snapshot = m_flatModel->entriesSnapshot();
        for (const FileEntry &entry : snapshot)
            existingNames.insert(entry.name);
    }
    QStringList uniqueSources;
    QSet<QString> seen;
    QStringList preExistingNames;
    int conflictCount = 0;
    for (const QString &source : sources) {
        if (source.isEmpty() || seen.contains(source)) {
            qWarning("[FileView] startFileOperation: skipped empty/duplicate source (source=\"%s\", move=%d, destination=\"%s\")",
                     qPrintable(source), move ? 1 : 0,
                     qPrintable(destinationDirectory));
            continue;
        }
        seen.insert(source);
        uniqueSources.append(source);

        // import 来源跨后端,拼写同目录不代表同位置 (R2-13)
        if (move && !importSources.contains(source)
            && isSameDirectoryPath(operationParentDirectory(source),
                                   destinationDirectory)) {
            continue;
        }
        const QString name = QFileInfo(source).fileName().isEmpty()
            ? normalizedPlainPath(source).section(QLatin1Char('/'), -1)
            : QFileInfo(source).fileName();
        const QString target = joinedPath(destinationDirectory, name);
        const bool conflict = scanFromModel
            ? existingNames.contains(name)
            : (m_io->exists(target)
               || (!(m_io && m_io->isRemote()) && QFileInfo(target).isSymLink()));
        if (conflict) {
            ++conflictCount;
            preExistingNames.append(name);
        }
    }
    if (uniqueSources.isEmpty())
        return false;

    QtFileMgrFileOperationJob::ConflictPolicy policy =
        QtFileMgrFileOperationJob::AutoRename;
    // 冲突框是嵌套事件循环,期间后端/URL 可能被异步连接切换;
    // 确认前快照,返回后验证 (R2-01; 与删除确认同一模式)
    QtFileMgrIO *const backendBefore = m_io;
    const QUrl urlBefore = m_url;
    if (askAboutConflicts && conflictCount > 0) {
        QMessageBox conflictBox(this);
        conflictBox.setIcon(QMessageBox::Question);
        conflictBox.setWindowTitle(QStringLiteral("处理同名项目"));
        conflictBox.setText(QStringLiteral("目标位置已有 %1 个同名项目。").arg(conflictCount));
        conflictBox.setInformativeText(
            QStringLiteral("所选策略将应用于本次操作的全部冲突。"));
        QPushButton *renameButton =
            conflictBox.addButton(QStringLiteral("自动重命名"), QMessageBox::AcceptRole);
        QPushButton *skipButton =
            conflictBox.addButton(QStringLiteral("跳过已有"), QMessageBox::ActionRole);
        QPushButton *overwriteButton = nullptr;
        if (!m_io->isRemote()) {
            overwriteButton =
                conflictBox.addButton(QStringLiteral("覆盖全部"), QMessageBox::DestructiveRole);
        }
        QPushButton *cancelButton =
            conflictBox.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        conflictBox.setDefaultButton(renameButton);
        conflictBox.exec();

        if (conflictBox.clickedButton() == cancelButton || !conflictBox.clickedButton())
            return false;
        if (conflictBox.clickedButton() == skipButton)
            policy = QtFileMgrFileOperationJob::SkipExisting;
        else if (overwriteButton && conflictBox.clickedButton() == overwriteButton)
            policy = QtFileMgrFileOperationJob::OverwriteExisting;
    }
    if (m_io != backendBefore || m_url != urlBefore) {
        qWarning("[FileView] startFileOperation: aborted because context changed during conflict confirmation (backendChanged=%d, urlBefore=\"%s\", urlNow=\"%s\")",
                 m_io != backendBefore ? 1 : 0,
                 qPrintable(urlBefore.toString(QUrl::RemovePassword)),
                 qPrintable(m_url.toString(QUrl::RemovePassword)));
        showMessage(QStringLiteral("操作已取消：确认期间视图已切换。"),
                    QStringLiteral("warning"));
        return false;
    }

    PendingOperation operation;
    operation.sources = uniqueSources;
    operation.destinationDirectory = destinationDirectory;
    operation.importSources = importSources;
    operation.move = move;
    operation.conflictPolicy = static_cast<int>(policy);
    if (policy == QtFileMgrFileOperationJob::OverwriteExisting)
        operation.overwrittenNames = preExistingNames;
    m_operationQueue.enqueue(operation);
    emit operationQueueChanged(m_operationQueue.size());
    if (m_activeJob && m_activeJob->isRunning()) {
        showMessage(QStringLiteral("文件操作已加入队列，前方还有 %1 项任务。")
                        .arg(m_operationQueue.size()),
                    QStringLiteral("info"));
    } else {
        startNextFileOperation();
    }
    return true;
}

bool QtFileMgrFileView::transferSelectedTo(
    const QString &destinationDirectory, bool move)
{
    const QStringList sources = selectedFiles();
    if (sources.isEmpty() || destinationDirectory.isEmpty()) {
        showMessage(QStringLiteral("请先选择要处理的文件。"),
                    QStringLiteral("warning"));
        return false;
    }
    return startFileOperation(sources, destinationDirectory, move);
}

void QtFileMgrFileView::startNextFileOperation()
{
    if (m_undoInProgress || m_activeJob || m_operationQueue.isEmpty())
        return;
    // 跨后端串行传输进行中不启动新任务 (审查#8),完成时会重新触发
    if (m_activeSerialTransfers > 0)
        return;
    const PendingOperation operation = m_operationQueue.dequeue();
    emit operationQueueChanged(m_operationQueue.size());
    const bool move = operation.move;
    const QString destinationDirectory = operation.destinationDirectory;
    auto *job = new QtFileMgrFileOperationJob(
        m_io,
        move ? QtFileMgrFileOperationJob::Move : QtFileMgrFileOperationJob::Copy,
        operation.sources,
        destinationDirectory,
        static_cast<QtFileMgrFileOperationJob::ConflictPolicy>(operation.conflictPolicy),
        this);
    job->setImportSources(operation.importSources);
    auto *progress = new QProgressDialog(
        move ? QStringLiteral("正在移动文件...") : QStringLiteral("正在复制文件..."),
        QStringLiteral("取消"),
        0,
        job->totalCount(),
        this);
    progress->setWindowTitle(move ? QStringLiteral("移动") : QStringLiteral("复制"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(300);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);

    m_activeJob = job;
    using CompletedItem = QPair<QString, QByteArray>;
    const auto completedItems = QSharedPointer<QHash<QString, CompletedItem>>::create();
    struct MoveLineage {
        QString source;
        QByteArray sourceSnapshot;
        QByteArray destinationSnapshot;
    };
    const auto moveLineages = QSharedPointer<QHash<QString, MoveLineage>>::create();
    connect(job, &QtFileMgrFileOperationJob::itemMoveLineage, this,
            [moveLineages](const QString &source, const QString &destination,
                           const QByteArray &sourceToken, const QByteArray &destinationToken) {
        if (moveLineages->contains(destination)) {
            qWarning("[FileView] undo: ignored duplicate move lineage (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return;
        }
        moveLineages->insert(destination, {source, sourceToken, destinationToken});
    });
    connect(job, &QtFileMgrFileOperationJob::itemCompleted, this,
            [completedItems](const QString &source, const QString &destination, const QByteArray &token) {
        if (completedItems->contains(destination)) {
            qWarning("[FileView] undo: ignored duplicate completion proof (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return;
        }
        completedItems->insert(destination, qMakePair(source, token));
    });
    connect(progress, &QProgressDialog::canceled, job, &QtFileMgrFileOperationJob::cancel);
    connect(job, &QtFileMgrFileOperationJob::progress, this,
            [progress, move](int completed, int total, const QString &currentPath) {
        progress->setMaximum(total);
        progress->setValue(completed);
        progress->setLabelText(
            QStringLiteral("正在%1 %2\n%3 / %4")
                .arg(move ? QStringLiteral("移动") : QStringLiteral("复制"),
                     QFileInfo(currentPath).fileName())
                .arg(completed)
                .arg(total));
    });
    connect(job, &QtFileMgrFileOperationJob::transferProgress, this,
            [progress, move](int completed, int total,
                             const QString &currentPath,
                             qint64 transferred, qint64 bytesTotal) {
        progress->setMaximum(total);
        progress->setValue(completed);
        const QString amount = bytesTotal >= 0
            ? QStringLiteral("%1 / %2")
                  .arg(formatFileSize(transferred), formatFileSize(bytesTotal))
            : formatFileSize(transferred);
        progress->setLabelText(
            QStringLiteral("正在%1 %2\n%3\n%4 / %5 个项目")
                .arg(move ? QStringLiteral("移动") : QStringLiteral("复制"),
                     QFileInfo(currentPath).fileName(), amount)
                .arg(completed)
                .arg(total));
    });
    connect(job, &QtFileMgrFileOperationJob::finished, this,
            [this, job, progress, operation, completedItems, moveLineages](
                int succeeded,
                int failed,
                int skipped,
                const QStringList &newNames,
                bool cancelled,
                const QStringList &errors) {
        if (m_activeJob == job)
            m_activeJob = nullptr;
        progress->close();
        progress->deleteLater();
        const bool move = operation.move;
        const QString destinationDirectory = operation.destinationDirectory;
        int unguardedUndoItems = 0;

        if (succeeded > 0) {
            UndoOperation undo;
            undo.move = move;
            for (const QString &name : newNames) {
                if (!move && operation.overwrittenNames.contains(name)) {
                    // 覆盖策略下该目标在操作前已存在且原内容已被替换,
                    // 撤销删除会连操作前的文件一起毁掉,不得纳入撤销记录。
                    qWarning("[FileView] undo: excluded overwritten destination \"%s/%s\" from undo record (original content was replaced)",
                             qPrintable(destinationDirectory), qPrintable(name));
                    continue;
                }
                const QString destination = joinedPath(destinationDirectory, name);
                const auto proof = completedItems->constFind(destination);
                if (proof == completedItems->constEnd() || proof->second.isEmpty()) {
                    ++unguardedUndoItems;
                    qWarning("[FileView] undo: excluded object without completion-time identity/revision proof (destination=\"%s\", reason=unsupported backend or safety limit)",
                             qPrintable(destination));
                    continue;
                }
                UndoOperation::Item item;
                item.source = proof->first;
                item.destination = destination;
                item.snapshot = proof->second;
                const auto lineage = moveLineages->constFind(destination);
                if (lineage != moveLineages->constEnd()) {
                    if (move && lineage->source == item.source
                        && lineage->destinationSnapshot == item.snapshot) {
                        item.sourceSnapshot = lineage->sourceSnapshot;
                    } else {
                        qWarning("[FileView] undo: refused move lineage inconsistent with completed item (source=\"%s\", lineageSource=\"%s\", destination=\"%s\", move=%d)",
                                 qPrintable(item.source), qPrintable(lineage->source),
                                 qPrintable(destination), move);
                    }
                }
                undo.items.append(item);
            }
            if (move && !operation.importSources.isEmpty()) {
                // 跨后端剪切(本机删除源+远端上传)没有可逆路径,
                // 不产生撤销记录 (复审 P1-3 澄清: 非畸形记录)
                qWarning("[FileView] undo: import move is not undoable; no record created (sources=%d, destination=\"%s\")",
                         operation.sources.size(),
                         qPrintable(operation.destinationDirectory));
            }
            if (!move || (operation.importSources.isEmpty()
                          && failed == 0 && skipped == 0 && !cancelled
                          && succeeded == operation.sources.size()
                          && newNames.size() == operation.sources.size())) {
                if (!undo.items.isEmpty()) {
                    m_undoOperations.append(undo);
                    while (m_undoOperations.size() > 20) {
                        qWarning("[FileView] undo: discarded oldest history record at capacity (limit=20, items=%d)",
                                 static_cast<int>(m_undoOperations.first().items.size()));
                        m_undoOperations.removeFirst();
                    }
                    emit undoAvailableChanged(true);
                }
            }
            reload();
            const QString currentDirectory = m_io
                ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
            if (isSameDirectoryPath(destinationDirectory, currentDirectory))
                selectNewFiles(newNames);
        }

        QString summary;
        if (cancelled) {
            summary = QStringLiteral("操作已取消：完成 %1，跳过 %2，失败 %3")
                          .arg(succeeded).arg(skipped).arg(failed);
        } else {
            summary = QStringLiteral("已%1 %2 个项目")
                          .arg(move ? QStringLiteral("移动") : QStringLiteral("复制"))
                          .arg(succeeded);
            if (skipped > 0)
                summary += QStringLiteral("，跳过 %1 个").arg(skipped);
            if (failed > 0)
                summary += QStringLiteral("，失败 %1 个").arg(failed);
        }
        showMessage(summary,
                    failed > 0 ? QStringLiteral("error")
                               : (cancelled ? QStringLiteral("warning")
                                            : QStringLiteral("info")));
        if (unguardedUndoItems > 0)
            showMessage(summary + QStringLiteral("；%1 项安全校验超限或后端不能证明未变更，不支持撤销。")
                            .arg(unguardedUndoItems), QStringLiteral("warning"));

        if (!errors.isEmpty()) {
            QMessageBox details(this);
            details.setIcon(failed > 0 ? QMessageBox::Critical : QMessageBox::Warning);
            details.setWindowTitle(QStringLiteral("文件操作详情"));
            details.setText(QStringLiteral("文件操作包含需要处理的问题。"));
            details.setDetailedText(errors.join(QLatin1Char('\n')));
            details.exec();
        }
        job->deleteLater();
        QTimer::singleShot(0, this, [this]() {
            // 队列排空后放行延后的跨后端传输 (审查#8)
            if (!m_activeJob && m_operationQueue.isEmpty()
                && !m_deferredTransfers.isEmpty()) {
                const QList<std::function<void()>> deferred =
                    std::exchange(m_deferredTransfers, {});
                for (const auto &start : deferred)
                    start();
            }
            startNextFileOperation();
        });
    });
    job->start();
}

void QtFileMgrFileView::undoLastOperation()
{
    if (m_undoInProgress || m_activeJob || !m_operationQueue.isEmpty()
        || m_activeSerialTransfers > 0) {
        qWarning("[FileView] undo: refused reentrant or busy request (undoRunning=%d, jobRunning=%d, queued=%d, serialTransfers=%d)",
                 m_undoInProgress, m_activeJob != nullptr,
                 static_cast<int>(m_operationQueue.size()), m_activeSerialTransfers);
        showMessage(QStringLiteral("请等待文件操作队列完成后再撤销。"),
                    QStringLiteral("warning"));
        return;
    }
    if (!m_io || m_undoOperations.isEmpty())
        return;

    // Keep no references into history across virtual backend calls. A custom
    // backend may dispatch a nested event loop, emit signals, or be destroyed.
    UndoOperation operation = m_undoOperations.last();
    const QPointer<QtFileMgrFileView> self(this);
    const QPointer<QtFileMgrIO> backend(m_io);
    m_undoInProgress = true;
    bool ok = true;
    QStringList rejected;
    for (int i = operation.items.size() - 1; i >= 0; --i) {
        UndoOperation::Item &item = operation.items[i];
        if (item.completed) {
            qWarning("[FileView] undo retry: skipped explicitly completed item (source=\"%s\", destination=\"%s\", move=%d)",
                     qPrintable(item.source), qPrintable(item.destination), operation.move);
            continue;
        }
        QString error;
        QByteArray refreshedSnapshot;
        const bool done = operation.move
            ? backend->undoMoveIfUnchanged(item.destination, item.source, item.snapshot, &error, &refreshedSnapshot)
            : backend->undoCopyIfUnchanged(item.destination, item.snapshot, &error, &refreshedSnapshot);
        if (!self)
            return;
        if (!backend || m_io != backend || m_undoOperations.isEmpty()) {
            qWarning("[FileView] undo: discarded result after backend/history context was lost (source=\"%s\", destination=\"%s\")",
                     qPrintable(item.source), qPrintable(item.destination));
            m_undoInProgress = false;
            return;
        }
        if (done) {
            item.completed = true;
            if (operation.move) {
                // Follow only this move's execution-time provenance. Matching
                // paths alone or newly scanning an old record would adopt edits
                // that happened before the move and erase the safety boundary.
                for (int history = 0; history + 1 < m_undoOperations.size(); ++history) {
                    for (UndoOperation::Item &previous : m_undoOperations[history].items) {
                        if (previous.completed || previous.destination != item.source)
                            continue;
                        if (!item.sourceSnapshot.isEmpty() && !refreshedSnapshot.isEmpty()
                            && previous.snapshot == item.sourceSnapshot) {
                            previous.snapshot = refreshedSnapshot;
                        } else {
                            qWarning("[FileView] undo lineage: retained earlier exact proof because restored object cannot be linked (source=\"%s\", destination=\"%s\", previousDestination=\"%s\", sourceProofAvailable=%d, restoredProofAvailable=%d, previousProofMatches=%d)",
                                     qPrintable(item.source), qPrintable(item.destination),
                                     qPrintable(previous.destination), !item.sourceSnapshot.isEmpty(),
                                     !refreshedSnapshot.isEmpty(), previous.snapshot == item.sourceSnapshot);
                        }
                    }
                }
            }
        } else {
            if (!refreshedSnapshot.isEmpty())
                item.snapshot = refreshedSnapshot;
            ok = false;
            rejected.append(QStringLiteral("%1: %2").arg(item.destination, error));
            qWarning("[FileView] undo: guarded action refused (source=\"%s\", destination=\"%s\", move=%d, error=\"%s\")",
                     qPrintable(item.source), qPrintable(item.destination), operation.move, qPrintable(error));
        }
    }
    if (ok) {
        m_undoOperations.removeLast();
        emit undoAvailableChanged(!m_undoOperations.isEmpty());
        if (!self)
            return;
    } else {
        m_undoOperations.last() = operation;
    }
    m_undoInProgress = false;
    reload();
    showMessage(ok ? QStringLiteral("已撤销上一次文件操作。")
                   : QStringLiteral("撤销已安全停止，已修改或替换的目标会保留。%1").arg(rejected.join(QStringLiteral("; "))),
                ok ? QStringLiteral("info") : QStringLiteral("error"));
}

void QtFileMgrFileView::clearCachedSizes()
{
    if (!m_flatModel || m_flatModel->rowCount() <= 0) {
        return;
    }
    // dataChanged 清除视图缓存 + doItemsLayout 强制重布局
    m_flatModel->dataChanged(m_flatModel->index(0, 0),
                             m_flatModel->index(m_flatModel->rowCount() - 1, 0));
    m_iconView->doItemsLayout();
    m_compactView->doItemsLayout();
    m_detailView->doItemsLayout();
    m_iconView->viewport()->update();
    m_compactView->viewport()->update();
    m_detailView->viewport()->update();
}

void QtFileMgrFileView::optimizeDetailColumns()
{
    if (m_hasExplicitColumnWidths)
        return;
    QHeaderView *h = m_detailView->horizontalHeader();
    int vpW = m_detailView->viewport()->width();
    if (vpW <= 0) return;
    const int sizeW = qMax(112, fontMetrics().horizontalAdvance(
                           QStringLiteral("999.9 GB")) + 28);
    const int typeW = 120;
    const int dateW = qBound(150, h->sectionSizeHint(3), 220);
    const int versionsW = m_flatModel->columnCount() > 4 ? 72 : 0;
    int nameW = vpW - sizeW - typeW - dateW - versionsW - 8;
    if (nameW < 200) nameW = 200;
    QSignalBlocker blocker(h);
    m_applyingColumnWidths = true;
    h->setStretchLastSection(false);
    h->resizeSection(0, nameW);
    h->resizeSection(1, sizeW);
    h->resizeSection(2, typeW);
    h->resizeSection(3, dateW);
    if (m_flatModel->columnCount() > 4)
        h->resizeSection(4, versionsW);
    h->setStretchLastSection(true);
    m_applyingColumnWidths = false;
}

void QtFileMgrFileView::scheduleReload()
{
    if (m_reloadTimer)
        m_reloadTimer->start();
}

void QtFileMgrFileView::updateDirectoryWatcher()
{
    if (!m_dirWatcher)
        return;
    const QStringList dirs = m_dirWatcher->directories();
    if (!dirs.isEmpty())
        m_dirWatcher->removePaths(dirs);

    if (m_io && m_io->isRemote())
        return;

    const QString path = m_url.toLocalFile();
    if (!path.isEmpty() && QFileInfo(path).isDir() && QFileInfo(path).isReadable())
        m_dirWatcher->addPath(path);
}

void QtFileMgrFileView::connectBackendSignals()
{
    if (!m_io)
        return;
    // 共享后端(被其他视图/注册表引用)被销毁时回退本地后端,
    // 避免 m_io 悬空 (跨后端复用的配套防护)
    QtFileMgrIO *backendGuardTarget = m_io;
    const bool ownedLocalBackend =
        qobject_cast<QtFileMgrLocalIO *>(backendGuardTarget)
        && backendGuardTarget->parent() == this;
    connect(backendGuardTarget, &QObject::destroyed, this,
            [this, backendGuardTarget, ownedLocalBackend]() {
        if (!ownedLocalBackend && m_io == backendGuardTarget) {
            qWarning("[FileView] backend destroyed while in use; falling back to local backend (url=\"%s\")",
                     qPrintable(m_url.toString(QUrl::RemovePassword)));
            m_io = nullptr;  // 先摘悬空指针, setIO 不再触碰旧后端
            setIO(nullptr);
        }
    });
    connect(m_io, &QtFileMgrIO::pathsChanged, this,
            [this](const QStringList &paths) {
        if (paths.isEmpty()) {
            scheduleReload();
            return;
        }
        QSet<QString> seen;
        for (const QString &sourcePath : paths) {
            const QString path = normalizedPlainPath(sourcePath);
            if (path.isEmpty() && !sourcePath.isEmpty()) {
                qWarning("[FileView] pathsChanged: skipped invalid backend path (backend=%s, source=\"%s\")",
                         m_io ? m_io->metaObject()->className() : "<null>",
                         qPrintable(sourcePath));
                continue;
            }
            if (seen.contains(path)) {
                qWarning("[FileView] pathsChanged: skipped duplicate backend path (backend=%s, path=\"%s\")",
                         m_io ? m_io->metaObject()->className() : "<null>",
                         qPrintable(path));
                continue;
            }
            seen.insert(path);
            if (backendPathAffectsCurrentDirectory(path)) {
                scheduleReload();
                return;
            }
        }
    });
    connect(m_io, &QtFileMgrIO::backendStateChanged, this,
            [this](bool available, const QString &message) {
        if (!available) {
            const QString detail = message.isEmpty()
                ? QStringLiteral("文件后端当前不可用") : message;
            qWarning("[FileView] backendStateChanged: backend unavailable (backend=%s, current=\"%s\", message=\"%s\")",
                     m_io ? m_io->metaObject()->className() : "<null>",
                     qPrintable(currentDirectoryPath()), qPrintable(detail));
            showMessage(detail, QStringLiteral("error"));
            return;
        }
        if (!message.isEmpty())
            showMessage(message, QStringLiteral("info"));
        scheduleReload();
    });
}

bool QtFileMgrFileView::backendPathAffectsCurrentDirectory(
    const QString &path) const
{
    const QString current = normalizedPlainPath(currentDirectoryPath());
    if (path == current)
        return true;
    if (operationParentDirectory(path) == current)
        return true;
    if (path == QStringLiteral("/"))
        return current.startsWith(QLatin1Char('/'));
    if (path.isEmpty())
        return m_io && m_io->isRemote();
    return current.startsWith(path + QLatin1Char('/'));
}

// ---------- 视图模式 ----------
QtFileMgrFileView::ViewMode QtFileMgrFileView::viewMode() const { return m_mode; }

void QtFileMgrFileView::setViewMode(ViewMode mode)
{
    QTMGR_DBG("setViewMode: %d", mode);
    mode = static_cast<ViewMode>(qBound(static_cast<int>(IconsView),
                                       static_cast<int>(mode),
                                       static_cast<int>(DetailsView)));
    if (m_mode == mode) {
        updateZoom();
        return;
    }

    const QStringList selectedPaths = selectedFiles();
    const QModelIndex currentIndex = currentView() ? currentView()->currentIndex() : QModelIndex();
    const QString currentPath = currentIndex.data(Qt::UserRole).toString();
    m_mode = mode;
    m_delegate->setViewMode(static_cast<QtFileMgrItemDelegate::ViewMode>(mode));
    switch (mode) {
    case IconsView:
        m_stack->setCurrentWidget(m_iconView);
        break;
    case CompactView:
        m_stack->setCurrentWidget(m_compactView);
        break;
    case DetailsView:
        m_stack->setCurrentWidget(m_detailView);
        QTimer::singleShot(0, this, [this]() { optimizeDetailColumns(); });
        break;
    }
    restoreSelection(selectedPaths, currentPath);
    updateZoom();
    emit viewModeChanged(static_cast<int>(m_mode));
}

// ---------- 隐藏文件 ----------
bool QtFileMgrFileView::showHiddenFiles() const { return m_flatModel->showHidden(); }
void QtFileMgrFileView::setShowHiddenFiles(bool show)
{
    const bool old = showHiddenFiles();
    preserveSelectionAround([this, show]() {
        m_flatModel->setShowHidden(show);
    });
    if (old != show)
        emit showHiddenFilesChanged(show);
}

// ---------- 文件夹优先 ----------
bool QtFileMgrFileView::foldersFirst() const { return m_flatModel->foldersFirst(); }
void QtFileMgrFileView::setFoldersFirst(bool on)
{
    const bool old = foldersFirst();
    preserveSelectionAround([this, on]() {
        m_flatModel->setFoldersFirst(on);
    });
    if (old != on)
        emit foldersFirstChanged(on);
}

void QtFileMgrFileView::setSortColumn(int column, Qt::SortOrder order)
{
    column = qBound(0, column, m_flatModel->columnCount() - 1);
    const int oldColumn = m_flatModel->sortColumn();
    const Qt::SortOrder oldOrder = m_flatModel->sortOrder();
    preserveSelectionAround([this, column, order]() {
        m_flatModel->setSortColumn(column, order);
    });
    updateSortIndicator();
    updateStatus();
    if (oldColumn != column || oldOrder != order)
        emit sortChanged(column, order);
}

// ---------- 缩放 ----------
int QtFileMgrFileView::iconSizeForLevel(int level) const
{
    level = qBound(0, level, 16);
    // Dolphin ZoomLevelInfo 映射
    static const int sizes[] = {16, 22, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
    return sizes[level];
}

int QtFileMgrFileView::levelForIconSize(int size) const
{
    size = qBound(16, size, 256);
    if (size <= 16) return 0;
    if (size <= 22) return 1;
    if (size <= 32) return 2;
    if (size <= 48) return 3;
    if (size <= 64) return 4;
    return 4 + (size - 64) / 16;
}

int QtFileMgrFileView::zoomLevel() const { return m_zoomLevel; }
int QtFileMgrFileView::zoomLevelCount() const { return kZoomLevelCount; }

void QtFileMgrFileView::setZoomLevel(int level)
{
    int prev = m_zoomLevel;
    m_zoomLevel = qBound(0, level, 16);
    if (m_zoomLevel != prev)
        emit zoomLevelChanged(m_zoomLevel, prev);
    updateZoom();
}

void QtFileMgrFileView::updateZoom()
{
    int px = iconSizeForLevel(m_zoomLevel);
    m_flatModel->setThumbnailSize(QSize(px, px));
    if (m_mode == IconsView) {
        m_iconView->setIconSize(QSize(px, px));
        const QFontMetrics fm = fontMetrics();
        const int minimumLabelWidth = qMax(128, fm.averageCharWidth() * 10 + 28);
        const int gridW = qMax(px + 32, minimumLabelWidth);
        const int gridH = 12 + px + 8 + 2 * fm.lineSpacing() + 12;
        m_iconView->setGridSize(QSize(gridW, gridH));
        m_delegate->setIconSize(px);
        m_delegate->setGridSize(QSize(gridW, gridH));
    } else if (m_mode == CompactView) {
        // 参照 Dolphin: 缩放控制列宽和行高, 图标16-48px, 列宽200-400px
        int cs = qBound(16, px / 3, 48);          // 图标: 16-48px
        int lineH = fontMetrics().height();
        int rowH = qMax(cs + 10, lineH + 10);
        int colW = cs + 220;
        m_compactView->setIconSize(QSize(cs, cs));
        m_compactView->setGridSize(QSize(colW, rowH));
        m_delegate->setIconSize(cs);
        m_delegate->setGridSize(QSize(colW, rowH));
    } else {
        // 详情以文字阅读密度为基准，缩放只逐步增加行距。
        m_detailView->setIconSize(QSize(18, 18));
        m_delegate->setIconSize(18);
        int lineH = fontMetrics().height();
        int rowH = qMax(30, lineH + 10 + (px - 16) / 8);
        m_detailView->verticalHeader()->setMinimumSectionSize(qMax(26, lineH + 8));
        m_detailView->verticalHeader()->setDefaultSectionSize(rowH);
    }
}

// ---------- 选择 ----------
QStringList QtFileMgrFileView::selectedFiles() const
{
    QStringList files;
    QItemSelectionModel *sel = m_stack->currentWidget() == m_iconView ? m_iconView->selectionModel()
                              : m_stack->currentWidget() == m_compactView ? m_compactView->selectionModel()
                              : m_detailView->selectionModel();
    if (!sel) return files;
    QModelIndexList idxs = sel->selectedIndexes();
    for (const QModelIndex &idx : idxs) {
        if (idx.column() != 0) continue;  // 只取第0列(名称)
        QString path = idx.data(Qt::UserRole).toString();
        if (!path.isEmpty()) files << path;
    }
    return files;
}

QAbstractItemView *QtFileMgrFileView::currentView() const
{
    return qobject_cast<QAbstractItemView *>(m_stack->currentWidget());
}

void QtFileMgrFileView::focusCurrentView()
{
    if (QAbstractItemView *view = currentView())
        view->setFocus(Qt::ShortcutFocusReason);
}

void QtFileMgrFileView::handleNavigationKey(int key, Qt::KeyboardModifiers modifiers)
{
    QAbstractItemView *view = currentView();
    if (!view)
        return;
    view->setFocus(Qt::ShortcutFocusReason);
    QKeyEvent keyEvent(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(view, &keyEvent);
}

void QtFileMgrFileView::restoreSelection(const QStringList &selectedPaths,
                                         const QString &currentPath)
{
    QAbstractItemView *view = currentView();
    if (!view || !view->selectionModel())
        return;

    QItemSelectionModel *selectionModel = view->selectionModel();
    QSignalBlocker blocker(selectionModel);
    selectionModel->clearSelection();
    selectionModel->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);

    QModelIndex firstSelected;
    QModelIndex restoredCurrent;
    QSet<QString> selectedSet;
    for (const QString &path : selectedPaths)
        selectedSet.insert(path);
    for (int row = 0; row < m_flatModel->rowCount(); ++row) {
        const QModelIndex index = m_flatModel->index(row, 0);
        const QString path = index.data(Qt::UserRole).toString();
        if (selectedSet.contains(path)) {
            selectionModel->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (!firstSelected.isValid())
                firstSelected = index;
        }
        if (!currentPath.isEmpty() && path == currentPath)
            restoredCurrent = index;
    }

    if (!restoredCurrent.isValid())
        restoredCurrent = firstSelected;
    if (restoredCurrent.isValid()) {
        selectionModel->setCurrentIndex(restoredCurrent, QItemSelectionModel::NoUpdate);
        view->scrollTo(restoredCurrent);
    }
    emit filesSelected(selectedFiles());
}

int QtFileMgrFileView::itemCount() const
{
    QModelIndex root = m_iconView->rootIndex();
    return m_flatModel->rowCount(root);
}

QtFileMgrFlatModel *QtFileMgrFileView::flatModel() const { return m_flatModel; }
QtFileMgrItemDelegate *QtFileMgrFileView::delegate() const { return m_delegate; }

void QtFileMgrFileView::selectAll()
{
    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    v->selectAll();
}

void QtFileMgrFileView::invertSelection()
{
    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    QItemSelectionModel *sel = v->selectionModel();
    QModelIndex root = v->rootIndex();
    QItemSelection toggle;
    int n = m_flatModel->rowCount(root);
    for (int i = 0; i < n; ++i) {
        QModelIndex idx = m_flatModel->index(i, 0, root);
        if (!sel->isSelected(idx))
            toggle.select(idx, idx);
    }
    sel->select(toggle, QItemSelectionModel::Toggle);
}

void QtFileMgrFileView::renameCurrent()
{
    if (m_cfg && !m_cfg->allowRename) {
        showMessage(QStringLiteral("当前配置不允许重命名。"), QStringLiteral("warning"));
        return;
    }
    if (!isDirectoryWritable(currentDirectoryPath())) {
        showMessage(QStringLiteral("当前目录不可写，无法重命名。"), QStringLiteral("warning"));
        return;
    }

    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    QModelIndex idx = v->currentIndex();
    if (!idx.isValid()) return;
    // 详情视图焦点可能在元数据列,重命名固定到名称列 (审查#20)
    if (idx.column() != 0)
        idx = idx.sibling(idx.row(), 0);

    const QString oldName = idx.data(Qt::DisplayRole).toString();
    const QUrl editUrl = m_url;
    // shared_ptr 管理连接句柄: 编辑器未打开时由延迟检查回收,
    // 避免堆 Connection 泄漏与下一次 closeEditor 触发陈旧 lambda (UI-P2-1)
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_delegate, &QAbstractItemDelegate::closeEditor,
                    this, [this, oldName, editUrl, conn](
                              QWidget *editor, QAbstractItemDelegate::EndEditHint) {
        disconnect(*conn);
        QString newName;
        if (auto *lineEdit = qobject_cast<QLineEdit *>(editor))
            newName = lineEdit->text().trimmed();
        if (newName.isEmpty() || newName == oldName)
            return;
        QTimer::singleShot(0, this, [this, newName, oldName, editUrl]() {
            if (m_url != editUrl)
                return;
            // 异步重命名: 新名在 rename 完成+模型刷新后才存在;
            // 不在模型里则登记 pending,等 reload 落地后选中 (R2-16)
            bool newNamePresent = false;
            for (int row = 0; row < m_flatModel->rowCount(); ++row) {
                if (m_flatModel->index(row, 0).data(Qt::DisplayRole).toString()
                    == newName) {
                    newNamePresent = true;
                    break;
                }
            }
            if (newNamePresent) {
                selectNewFiles({newName});
                return;
            }
            m_pendingSelection = QStringList{newName};
            m_pendingSelectionUrl = m_url;
            m_pendingSelectionTimer->start();
        });
    });
    v->edit(idx);
    QTimer::singleShot(0, this, [v, idx, conn]() {
        if (!v->indexWidget(idx))
            disconnect(*conn);
    });
}

#include <cstring>

// ---- 文件操作 ----
static const char CUT_MIME[] = "application/x-kde-cutselection";

// 跨后端移动成功后删除源项目,失败告警不静默
static void deleteSourcesOnBackend(QtFileMgrIO *backend, const QStringList &paths)
{
    if (!backend)
        return;
    for (const QString &path : paths) {
        if (!backend->removePermanent(path)) {
            qWarning("[FileView] cross-backend move: failed to delete source after transfer (backend=%s, path=\"%s\")",
                     backend->metaObject()->className(), qPrintable(path));
        }
    }
}

QList<QPointer<QtFileMgrIO>> QtFileMgrFileView::s_remoteBackends;

void QtFileMgrFileView::registerRemoteBackend(QtFileMgrIO *io)
{
    if (!io || !io->isRemote())
        return;
    for (const QPointer<QtFileMgrIO> &candidate : s_remoteBackends) {
        if (candidate == io)
            return;
    }
    s_remoteBackends.append(io);
}

void QtFileMgrFileView::unregisterRemoteBackend(QtFileMgrIO *io)
{
    s_remoteBackends.removeAll(QPointer<QtFileMgrIO>(io));
}

void QtFileMgrFileView::clearRemoteBackendRegistry()
{
    s_remoteBackends.clear();
}

QtFileMgrIO *QtFileMgrFileView::remoteBackendForUrl(const QUrl &url)
{
    // 多个后端均可服务同一 URL 时取最近注册者(通常是当前活跃连接)
    QtFileMgrIO *found = nullptr;
    for (auto it = s_remoteBackends.begin(); it != s_remoteBackends.end();) {
        if (it->isNull()) {
            it = s_remoteBackends.erase(it);
            continue;
        }
        if ((*it)->isRemote() && (*it)->acceptsUrl(url))
            found = *it;
        ++it;
    }
    return found;
}

void QtFileMgrFileView::deletePermanent()
{
    deleteSelectedInternal(true);
}

void QtFileMgrFileView::deleteSelected()
{
    bool permanent = (QApplication::keyboardModifiers() & Qt::ShiftModifier);
    deleteSelectedInternal(permanent);
}

void QtFileMgrFileView::deleteSelectedInternal(bool permanent)
{
    if (m_cfg && !m_cfg->allowDelete) {
        showMessage(QStringLiteral("当前配置不允许删除文件。"), QStringLiteral("warning"));
        return;
    }
    if (!isDirectoryWritable(currentDirectoryPath())) {
        showMessage(QStringLiteral("当前目录不可写，无法删除。"), QStringLiteral("warning"));
        return;
    }

    if (!m_io) {
        qWarning("[FileView] deleteSelectedInternal: rejected operation without backend (permanent=%d, url=\"%s\")",
                 permanent ? 1 : 0, qPrintable(m_url.toString()));
        showMessage(QStringLiteral("文件后端不可用，无法删除。"),
                    QStringLiteral("error"));
        return;
    }

    QStringList files = selectedFiles();
    if (files.isEmpty()) return;
    // 确认框是嵌套事件循环,期间后端/URL 可能被异步连接切换;
    // 确认前快照,确认后验证上下文不变 (审查#7)
    QtFileMgrIO *const backendBefore = m_io;
    const QUrl urlBefore = m_url;
    if (!permanent
        && !m_io->supportsCapability(QtFileMgrIO::TrashCapability)) {
        qWarning("[FileView] deleteSelectedInternal: falling back to permanent-delete confirmation because backend has no trash (backend=%s, url=\"%s\", count=%d)",
                 m_io->metaObject()->className(), qPrintable(m_url.toString()),
                 files.size());
        permanent = true;
    }
    int count = files.size();

    QString msg;
    if (permanent)
        msg = count == 1
            ? QStringLiteral("永久删除 \"%1\"?").arg(QFileInfo(files.first()).fileName())
            : QStringLiteral("永久删除 %1 个项目?").arg(count);
    else
        msg = count == 1
            ? QStringLiteral("将 \"%1\" 移动到回收站?").arg(QFileInfo(files.first()).fileName())
            : QStringLiteral("将 %1 个项目移动到回收站?").arg(count);

    if (QMessageBox::question(this, permanent ? QStringLiteral("永久删除") : QStringLiteral("回收站"),
                               msg, QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) != QMessageBox::Yes)
        return;
    if (m_io != backendBefore || m_url != urlBefore) {
        // 确认期间上下文已变化,删除不得落在未获确认的后端上 (审查#7)
        qWarning("[FileView] deleteSelectedInternal: aborted because context changed during confirmation (backendChanged=%d, urlBefore=\"%s\", urlNow=\"%s\")",
                 m_io != backendBefore ? 1 : 0,
                 qPrintable(urlBefore.toString(QUrl::RemovePassword)),
                 qPrintable(m_url.toString(QUrl::RemovePassword)));
        showMessage(QStringLiteral("删除已取消：确认期间视图已切换。"),
                    QStringLiteral("warning"));
        return;
    }

    // 记下第一个被删项的位置，用于删后选中下一项。
    // reload 会同步清空模型,事后按行号选中必然落空,须先取出
    // 候选条目的名称,删后走 pending 选择机制 (UI-P1-3)
    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    const int firstRow = v->currentIndex().row();
    const int totalBefore = itemCount();
    QString nextNameAfterDelete;
    if (totalBefore > files.size()) {
        QSet<QString> deletedSet;
        for (const QString &f : files)
            deletedSet.insert(f);
        auto nameOfSurvivor = [&](int startRow, int step) -> QString {
            for (int row = startRow; row >= 0 && row < totalBefore; row += step) {
                const QModelIndex idx = m_flatModel->index(row, 0);
                if (!deletedSet.contains(idx.data(Qt::UserRole).toString()))
                    return idx.data(Qt::DisplayRole).toString();
            }
            return QString();
        };
        nextNameAfterDelete = nameOfSurvivor(qMin(firstRow, totalBefore - 1), 1);
        if (nextNameAfterDelete.isEmpty())
            nextNameAfterDelete = nameOfSurvivor(qMin(firstRow, totalBefore - 1) - 1, -1);
    }

    // 删除循环按事件轮次逐项推进: 远端逐项同步 RPC 不再整批冻结
    // 事件循环 (UI-P1-4);facade 同线程契约保持不变
    struct DeleteState {
        QStringList files;
        bool permanent = false;
        int index = 0;
        int ok = 0;
        int fail = 0;
        QStringList errors;
        std::shared_ptr<std::function<void()>> continuation;
    };
    const std::shared_ptr<DeleteState> state(
        new DeleteState{files, permanent, 0, 0, 0, {}, {}});
    const QPointer<QtFileMgrFileView> self(this);
    const QPointer<QtFileMgrIO> backend(backendBefore);  // 锁定确认时的后端 (审查#7)
    const std::shared_ptr<std::function<void()>> step(
        new std::function<void()>);
    // 弱引用双端断环 (R2-15): state 由定时器回调持强引用,
    // state→step→lambda 只持 weak,链断/早退/销毁都整体释放
    state->continuation = step;
    const std::weak_ptr<DeleteState> weakState(state);
    const std::weak_ptr<std::function<void()>> weakStep(step);
    *step = [this, self, backend, weakState, weakStep,
             nextNameAfterDelete]() {
        const auto state = weakState.lock();
        if (!state)
            return;
        if (!self || !backend) {
            qWarning("[FileView] deleteSelectedInternal: aborted mid-flight, state released (backendGone=%d, processed=%d/%d)",
                     backend.isNull() ? 1 : 0, state->index,
                     state->files.size());
            state->continuation.reset();
            return;
        }
        if (state->index < state->files.size()) {
            const QString f = state->files.at(state->index++);
            if (state->permanent) {
                if (backend->removePermanent(f)) {
                    ++state->ok;
                } else {
                    ++state->fail;
                    state->errors.append(QStringLiteral("永久删除失败: %1").arg(f));
                }
            } else {
                QString error;
                if (backend->moveToTrash(f, &error)) {
                    ++state->ok;
                } else {
                    ++state->fail;
                    if (error.isEmpty()) {
                        error = QStringLiteral("后端未提供错误信息");
                        qWarning("[FileView] deleteSelectedInternal: trash backend failed without error (backend=%s, path=\"%s\")",
                                 backend->metaObject()->className(), qPrintable(f));
                    }
                    state->errors.append(
                        QStringLiteral("移动到回收站失败: %1 (%2)").arg(f, error));
                }
            }
            if (state->files.size() > 3) {
                showMessage(QStringLiteral("正在删除 %1/%2...")
                                .arg(state->index).arg(state->files.size()),
                            QStringLiteral("info"));
            }
            QTimer::singleShot(0, this, [weakStep, state]() {
                if (const auto next = weakStep.lock())
                    (*next)();
            });
            return;
        }

        if (state->ok > 0) {
            reload();
            showMessage(state->permanent
                ? QStringLiteral("已永久删除 %1 个项目").arg(state->ok)
                : QStringLiteral("已将 %1 个项目移动到回收站").arg(state->ok),
                state->permanent ? QStringLiteral("warning")
                                 : QStringLiteral("info"));
        }
        if (state->fail > 0) {
            showMessage(QStringLiteral("删除 %1 个项目失败：%2")
                            .arg(state->fail)
                            .arg(state->errors.value(0)),
                        QStringLiteral("error"));
            QMessageBox details(this);
            details.setIcon(QMessageBox::Critical);
            details.setWindowTitle(QStringLiteral("删除失败详情"));
            details.setText(QStringLiteral("%1 个项目删除失败。").arg(state->fail));
            details.setDetailedText(state->errors.join(QLatin1Char('\n')));
            details.exec();
        }

        if (state->ok > 0 && !nextNameAfterDelete.isEmpty())
            selectNewFiles({nextNameAfterDelete});
        state->continuation.reset();
    };
    (*step)();
}

// ---------- 事件处理 ----------
void QtFileMgrFileView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
        QModelIndex idx = v->currentIndex();
        if (idx.isValid()) navigateTo(idx);
        return;
    }
    case Qt::Key_Backspace:
    case Qt::Key_Back:
        goUp();
        return;
    case Qt::Key_F2:
        renameCurrent();
        return;
    case Qt::Key_Delete:
        deleteSelected();
        return;
    case Qt::Key_F5:
        reload();
        return;
    case Qt::Key_Escape: {
        if (m_messageBar && m_messageBar->isVisible()) {
            m_messageBar->hide();
            return;
        }
        QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
        if (v && v->selectionModel() && v->selectionModel()->hasSelection()) {
            v->clearSelection();
            updateStatus();
            return;
        }
        break;
    }
    case Qt::Key_I:
        // 含 Control 即可,不被 NumLock/Shift 等附加修饰挡住 (R2-FV-P2-9)
        if (event->modifiers() & Qt::ControlModifier) {
            emit requestFilterToggle();
            return;
        }
        break;
    case Qt::Key_Slash:
        // 仅在无修饰键时触发过滤,不吞 Ctrl+/ 等组合 (R2-FV-P2-9)
        if (event->modifiers() == Qt::NoModifier) {
            emit requestFilterToggle();
            return;
        }
        break;
    case Qt::Key_F6:
        emit requestBreadcrumbEdit();
        return;
    case Qt::Key_L:
        if (event->modifiers() == Qt::ControlModifier) {
            emit requestBreadcrumbEdit();
            return;
        }
        break;
    case Qt::Key_H:
        if (event->modifiers() == Qt::ControlModifier) {
            setShowHiddenFiles(!showHiddenFiles());
            return;
        }
        break;
    case Qt::Key_A:
        if (event->modifiers() & Qt::ControlModifier) {
            if (event->modifiers() & Qt::ShiftModifier) invertSelection();
            else selectAll();
            return;
        }
        break;
    }
    QWidget::keyPressEvent(event);
}

void QtFileMgrFileView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        int delta = event->angleDelta().y();
        if (delta != 0) {
            setZoomLevel(m_zoomLevel + (delta > 0 ? 1 : -1));
        }
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

// ---------- 框选 (RubberBand) ----------
void QtFileMgrFileView::setupRubberBand(QAbstractItemView *view)
{
    // 框选坐标基于 viewport,橡皮筋必须挂在当前视图的 viewport 下,
    // 否则视图切换或滚动后位置整体偏移 (UI-P2-3)
    if (!m_rubberBand) {
        m_rubberBand = new QRubberBand(QRubberBand::Rectangle, view->viewport());
    } else if (m_rubberBand->parentWidget() != view->viewport()) {
        m_rubberBand->setParent(view->viewport());
    }
}

bool QtFileMgrFileView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_stack && event->type() == QEvent::Resize) {
        if (m_contentStateLabel)
            m_contentStateLabel->setGeometry(m_stack->rect());
        return QWidget::eventFilter(obj, event);
    }

    QAbstractItemView *view = qobject_cast<QAbstractItemView *>(obj->parent());
    if (!view || !(obj == view->viewport())) {
        // Also check if obj is the view itself (for key events etc.)
        if (obj == m_iconView || obj == m_compactView || obj == m_detailView) {
            view = qobject_cast<QAbstractItemView *>(obj);
        } else {
            return QWidget::eventFilter(obj, event);
        }
    }

    if ((obj == m_detailView || view == m_detailView) && event->type() == QEvent::Resize) {
        // debounce: 用 singleShot 合并连续的 resize 事件
        if (!m_resizeTimer) {
            m_resizeTimer = new QTimer(this);
            m_resizeTimer->setSingleShot(true);
            m_resizeTimer->setInterval(50);
            connect(m_resizeTimer, &QTimer::timeout, this, &QtFileMgrFileView::optimizeDetailColumns);
        }
        m_resizeTimer->start();
    }

    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        const int key = ke->key();
        const Qt::KeyboardModifiers mods = ke->modifiers();
        const bool handledKey =
            key == Qt::Key_Return || key == Qt::Key_Enter ||
            key == Qt::Key_Backspace || key == Qt::Key_Back ||
            key == Qt::Key_F2 || key == Qt::Key_Delete ||
            key == Qt::Key_F5 || key == Qt::Key_Escape ||
            key == Qt::Key_Slash || key == Qt::Key_F6 ||
            (key == Qt::Key_I && mods == Qt::ControlModifier) ||
            (key == Qt::Key_L && mods == Qt::ControlModifier) ||
            (key == Qt::Key_H && mods == Qt::ControlModifier) ||
            (key == Qt::Key_A && (mods & Qt::ControlModifier));
        if (handledKey) {
            keyPressEvent(ke);
            return true;
        }
    }

    if (event->type() == QEvent::Wheel) {
        QWheelEvent *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::ControlModifier) {
            int delta = we->angleDelta().y();
            if (delta != 0)
                setZoomLevel(m_zoomLevel + (delta > 0 ? 1 : -1));
            event->accept();
            return true;
        }
    }

    // ── 拖放: DragEnter/DragMove/Drop 全处理 ──
    if (m_cfg && !m_cfg->allowDragDrop) return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        QDropEvent *de = static_cast<QDropEvent *>(event);
        // 接受外部URL拖放 和 内部model拖放
        if (de->mimeData()->hasUrls() ||
            de->mimeData()->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist"))) {
            de->acceptProposedAction();
            return true;
        }
    }

    if (event->type() == QEvent::Drop) {
        QDropEvent *de = static_cast<QDropEvent *>(event);

        // 收集源文件路径
        QStringList srcFiles;
        QSet<QString> seenSources;
        QSet<QString> importDropSources;  // 本机来源(拖到远端视图) (审查#1)
        QList<QPair<QtFileMgrIO *, QString>> foreignDropItems;
        const auto appendSource = [&srcFiles, &seenSources](const QString &path) {
            if (path.isEmpty() || seenSources.contains(path))
                return;
            seenSources.insert(path);
            srcFiles.append(path);
        };
        if (de->mimeData()->hasUrls()) {
            // 外部拖放: 从URL提取
            for (const QUrl &url : de->mimeData()->urls()) {
                if (url.isLocalFile()) {
                    const QString localPath = url.toLocalFile();
                    appendSource(localPath);
                    if (m_io && m_io->isRemote())
                        importDropSources.insert(localPath);
                } else if (m_io && m_io->isRemote()
                           && m_io->acceptsUrl(url)) {
                    appendSource(m_io->pathFromUrl(url));
                } else if (QtFileMgrIO *sourceBackend = remoteBackendForUrl(url)) {
                    // 其他远端节点的项目 → 下载/中转 (R2-FV-P1-2 延伸)
                    const QString sourcePath = sourceBackend->pathFromUrl(url);
                    if (sourcePath.isEmpty()) {
                        qWarning("[FileView] drop: source backend rejected its own URL (backend=%s, url=\"%s\")",
                                 sourceBackend->metaObject()->className(),
                                 qPrintable(url.toString(QUrl::RemovePassword)));
                    } else {
                        foreignDropItems.append({sourceBackend, sourcePath});
                    }
                } else {
                    qWarning("[FileView] drop: skipped URL from incompatible backend (backend=%s, url=\"%s\")",
                             m_io ? m_io->metaObject()->className() : "<null>",
                             qPrintable(url.toString(QUrl::RemovePassword)));
                }
            }
        } else if (de->mimeData()->hasFormat(QStringLiteral("application/x-qabstractitemmodeldatalist"))) {
            // 内部拖放: 路径按源后端语义构造,仅在同源后端标识下接受;
            // 跨后端必须走上面的 URL 分支 (R2-FV-P1-2)
            const QByteArray sourceBackend = de->mimeData()->data(
                QString::fromLatin1(QtFileMgrFlatModel::backendDragMimeFormat()));
            const QByteArray currentBackend =
                QtFileMgrFlatModel::backendDragId(m_io);
            if (sourceBackend != currentBackend) {
                qWarning("[FileView] drop: rejected datalist from different or unknown backend (source=\"%s\", current=\"%s\")",
                         qPrintable(QString::fromUtf8(sourceBackend)),
                         qPrintable(QString::fromUtf8(currentBackend)));
            } else {
            QByteArray encoded = de->mimeData()->data(
                QStringLiteral("application/x-qabstractitemmodeldatalist"));
            QDataStream stream(&encoded, QIODevice::ReadOnly);
            while (!stream.atEnd()) {
                int row, col;
                QMap<int, QVariant> roleDataMap;
                stream >> row >> col >> roleDataMap;
                Q_UNUSED(col);
                const QString path = roleDataMap.value(Qt::UserRole).toString();
                if (path.isEmpty()) {
                    qWarning("[FileView] drop: skipped datalist item without path (row=%d)", row);
                    continue;
                }
                appendSource(path);
            }
            }
        }

        if (!srcFiles.isEmpty() || !foreignDropItems.isEmpty()) {
            // 确定目标目录
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QModelIndex targetIdx = view->indexAt(de->position().toPoint());
#else
            QModelIndex targetIdx = view->indexAt(de->pos());
#endif
            QString destDir = m_io ? m_io->pathFromUrl(m_url)
                                   : m_url.toLocalFile();
            if (targetIdx.isValid()) {
                QString targetPath = targetIdx.data(Qt::UserRole).toString();
                bool targetIsDir = targetIdx.data(Qt::UserRole + 1).toBool();
                if (targetIsDir) {
                    destDir = targetPath;
                }
            }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const Qt::KeyboardModifiers modifiers = de->modifiers();
#else
            const Qt::KeyboardModifiers modifiers = de->keyboardModifiers();
#endif
            Qt::DropAction action = de->proposedAction();
            if (modifiers & Qt::ControlModifier)
                action = Qt::CopyAction;
            else if (modifiers & Qt::ShiftModifier)
                action = Qt::MoveAction;
            if (action != Qt::CopyAction && action != Qt::MoveAction) {
                de->ignore();
                return true;
            }
            const bool isCopy = action == Qt::CopyAction;
            const bool canDropCopy = (!m_cfg || m_cfg->allowCreate)
                && isDirectoryWritable(destDir);
            const bool canDropMove = canDropCopy && (!m_cfg || (m_cfg->allowRename && m_cfg->allowDelete));
            if ((isCopy && !canDropCopy) || (!isCopy && !canDropMove)) {
                showMessage(QStringLiteral("当前配置或目录权限不允许拖放到此处。"), QStringLiteral("warning"));
                de->ignore();
                return true;
            }

            bool dropStarted = false;
            if (!srcFiles.isEmpty())
                dropStarted = startFileOperation(srcFiles, destDir, !isCopy,
                                                 true, importDropSources);
            if (!foreignDropItems.isEmpty()) {
                QHash<QtFileMgrIO *, QStringList> grouped;
                for (const auto &item : foreignDropItems)
                    grouped[item.first].append(item.second);
                for (auto it = grouped.begin(); it != grouped.end(); ++it) {
                    if (m_io && m_io->isRemote())
                        startSerialRelay(it.key(), it.value(), destDir, !isCopy);
                    else
                        startSerialDownload(it.key(), it.value(), destDir, !isCopy);
                }
                dropStarted = true;
            }
            if (!dropStarted) {
                de->ignore();
                return true;
            }
            de->setDropAction(action);
            de->accept();
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        const QPoint mousePos =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            me->position().toPoint();
#else
            me->pos();
#endif
        if (me->button() == Qt::LeftButton) {
            // 检查是否点在空白区域
            QModelIndex idx = view->indexAt(mousePos);
            if (!idx.isValid()) {
                // 空白区域开始框选
                setupRubberBand(view);
                beginRubberSelection(view, mousePos);
                return true;
            }
            // 记录锚点 (Shift+Click)
            if (me->modifiers() & Qt::ShiftModifier) {
                // Shift range selection handled by QAbstractItemView
            } else if (!(me->modifiers() & Qt::ControlModifier)) {
                m_anchorIndex = idx;
            }
        }
    } else if (event->type() == QEvent::MouseMove && m_rubberActive) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        const QPoint mousePos =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            me->position().toPoint();
#else
            me->pos();
#endif
        updateRubberSelection(view, mousePos);
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && m_rubberActive) {
        endRubberSelection(view);
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void QtFileMgrFileView::beginRubberSelection(QAbstractItemView *view, const QPoint &pos)
{
    m_rubberOrigin = pos;
    m_rubberActive = true;
    m_rubberView = view;
    if (m_rubberBand) {
        m_rubberBand->setGeometry(QRect(pos, QSize()));
        m_rubberBand->show();
    }
    // Ctrl 增选时保留按下时的选择作为基底,否则清空 (UI-P2-3)
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        m_rubberBaseSelection = view->selectionModel()->selection();
    } else {
        m_rubberBaseSelection.clear();
        view->clearSelection();
    }
}

void QtFileMgrFileView::updateRubberSelection(QAbstractItemView *view, const QPoint &pos)
{
    QRect r = QRect(m_rubberOrigin, pos).normalized();
    if (m_rubberBand)
        m_rubberBand->setGeometry(r);

    // 选择 rubber band 内的 items;Ctrl 增选时合并按下时的基底选择 (UI-P2-3)
    QItemSelectionModel *selModel = view->selectionModel();
    QItemSelection selection = m_rubberBaseSelection;

    for (int row = 0; row < m_flatModel->rowCount(view->rootIndex()); ++row) {
        QModelIndex idx = m_flatModel->index(row, 0, view->rootIndex());
        QRect itemRect = view->visualRect(idx);
        if (r.intersects(itemRect)) {
            selection.select(idx, idx);
        }
    }
    selModel->select(selection, QItemSelectionModel::ClearAndSelect);
}

void QtFileMgrFileView::endRubberSelection(QAbstractItemView *view)
{
    Q_UNUSED(view);
    m_rubberActive = false;
    m_rubberView = nullptr;
    m_rubberBaseSelection.clear();
    if (m_rubberBand)
        m_rubberBand->hide();
}

// ---------- 导航 ----------
void QtFileMgrFileView::navigateTo(const QModelIndex &idx)
{
    QString path = idx.data(Qt::UserRole).toString();
    bool isDir = idx.data(Qt::UserRole + 1).toBool();
    if (isDir) {
        if (m_io && m_io->isRemote()) {
            setUrl(m_io->urlForPath(path));
        } else {
            setUrl(QUrl::fromLocalFile(path));
        }
    } else {
        emit fileActivated(path);
    }
}

void QtFileMgrFileView::onDoubleClicked(const QModelIndex &index)
{
    navigateTo(index);
}

void QtFileMgrFileView::goUp()
{
    if (m_io && m_io->isRemote()) {
        const QString path = QDir::cleanPath(m_io->pathFromUrl(m_url));
        if (path.isEmpty() || path == QStringLiteral(".") || path == QStringLiteral("/"))
            return;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parent = slash > 0 ? path.left(slash)
            : (path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString());
        setUrl(m_io->urlForPath(parent));
    } else {
        QDir dir(m_url.toLocalFile());
        if (dir.cdUp())
            setUrl(QUrl::fromLocalFile(dir.absolutePath()));
    }
}

void QtFileMgrFileView::reload()
{
    QString flatPath = m_io ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
    m_loaded = false;
    updateContentState(QStringLiteral("正在加载..."));
    m_flatModel->setUrl(flatPath);
    updateDirectoryWatcher();
}

void QtFileMgrFileView::applyPendingSelection()
{
    if (!m_pendingSelection.isEmpty()) {
        if (m_pendingSelectionUrl != m_url) {
            m_pendingSelection.clear();
            m_pendingSelectionUrl = QUrl();
            m_pendingSelectionTimer->stop();
        } else {
            QStringList found;
            for (int row = 0; row < m_flatModel->rowCount(); ++row) {
                const QString name =
                    m_flatModel->index(row, 0).data(Qt::DisplayRole).toString();
                if (m_pendingSelection.contains(name))
                    found.append(name);
            }
            if (!found.isEmpty()) {
                selectNewFiles(found);
                for (const QString &name : found)
                    m_pendingSelection.removeAll(name);
            }
            if (m_pendingSelection.isEmpty()) {
                m_pendingSelectionUrl = QUrl();
                m_pendingSelectionTimer->stop();
            }
        }
    }
}

void QtFileMgrFileView::tryStartPendingRename()
{
    if (m_pendingRenameName.isEmpty())
        return;
    QAbstractItemView *v = currentView();
    const QModelIndex current = v ? v->currentIndex() : QModelIndex();
    if (!current.isValid()
        || current.data(Qt::DisplayRole).toString() != m_pendingRenameName)
        return;  // 选中尚未落地,等待 directoryLoadFinished 或超时
    m_pendingRenameName.clear();
    QTimer::singleShot(0, this, [this]() { renameCurrent(); });
}

void QtFileMgrFileView::onSelectionChanged()
{
    QStringList files = selectedFiles();
    if (files.isEmpty()) {
        updateStatus();
    } else {
        // 优先使用模型已有元数据,避免选中变化时对远端后端逐文件
        // 同步 stat 冻结事件循环 (UI-P1-4)
        QSet<QString> selectedSet;
        for (const QString &f : files)
            selectedSet.insert(f);
        QHash<QString, FileEntry> entriesByPath;
        if (m_flatModel) {
            const QList<FileEntry> snapshot = m_flatModel->entriesSnapshot();
            for (const FileEntry &entry : snapshot) {
                if (selectedSet.contains(entry.path))
                    entriesByPath.insert(entry.path, entry);
            }
        }
        qint64 totalSize = 0;
        int dirs = 0, fls = 0;
        for (const QString &f : files) {
            const auto it = entriesByPath.constFind(f);
            if (it != entriesByPath.constEnd()) {
                if (it->isDir) dirs++;
                else {
                    fls++;
                    if (it->size > 0) totalSize += it->size;
                }
                continue;
            }
            if (m_io && m_io->isRemote()) {
                qWarning("[FileView] onSelectionChanged: selected path missing from model snapshot; skipped remote stat (path=\"%s\")",
                         qPrintable(f));
                fls++;
                continue;
            }
            const QFileInfo info(f);
            if (info.isDir()) dirs++;
            else {
                fls++;
                if (info.size() > 0) totalSize += info.size();
            }
        }
        QString msg = QStringLiteral("已选中 %1 项 (%2 个文件夹, %3 个文件")
            .arg(files.size()).arg(dirs).arg(fls);
        if (totalSize > 0)
            msg += QStringLiteral(", %1").arg(formatFileSize(totalSize));
        msg += QLatin1Char(')');
        const QString status = QStringLiteral(" %1").arg(msg);
        m_statusLabel->setText(status);
        emit statusMessage(status);
    }
    emit filesSelected(files);
}

void QtFileMgrFileView::updateStatus()
{
    QModelIndex root = m_iconView->rootIndex();
    int count = m_flatModel->rowCount(root);
    const int dirs = m_flatModel->directoryCount();
    const int files = m_flatModel->fileCount();
    QString msg;
    QString contentState;
    if (count == 0) {
        if (!m_loaded) {
            msg = QStringLiteral(" 加载中...");
            contentState = QStringLiteral("正在加载...");
        } else if (!m_flatModel->nameFilter().isEmpty()) {
            msg = QStringLiteral(" 没有匹配项 | 过滤: \"%1\"")
                  .arg(m_flatModel->nameFilter());
            contentState = QStringLiteral("没有匹配项");
        } else {
            const QString path = m_io
                ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
            if (m_io && !path.isEmpty() && m_io->exists(path) && !m_io->isDir(path)) {
                msg = QStringLiteral(" 当前位置不是文件夹");
                contentState = QStringLiteral("当前位置不是文件夹");
            } else if (m_io && !path.isEmpty() && !m_io->exists(path)) {
                msg = QStringLiteral(" 路径不存在");
                contentState = QStringLiteral("路径不存在");
            } else if (!(m_io && m_io->isRemote()) && !path.isEmpty()
                     && QFileInfo(path).exists() && !QFileInfo(path).isReadable()) {
                msg = QStringLiteral(" 无权限读取此文件夹");
                contentState = QStringLiteral("无权限读取此文件夹");
            } else {
                msg = QStringLiteral(" 文件夹为空");
                contentState = QStringLiteral("此文件夹为空");
            }
        }
    } else {
        msg = QStringLiteral(" %1 项 (%2 个文件夹, %3 个文件)")
              .arg(count).arg(dirs).arg(files);
    }
    m_statusLabel->setText(msg);
    updateContentState(contentState);
    emit statusMessage(msg);
}

void QtFileMgrFileView::updateContentState(const QString &text)
{
    if (!m_contentStateLabel)
        return;
    m_contentStateLabel->setText(text);
    m_contentStateLabel->setVisible(!text.isEmpty());
    if (!text.isEmpty()) {
        m_contentStateLabel->setGeometry(m_stack->rect());
        m_contentStateLabel->raise();
    }
}

// ---------- 右键菜单 (参考 DolphinContextMenu) ----------
void QtFileMgrFileView::onContextMenu(const QPoint &pos)
{
    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    QModelIndex idx = v->indexAt(pos);
    // 持久索引供各菜单 lambda 捕获: 菜单开着时模型可能 reset,
    // 裸 QModelIndex 悬空 (UI-P2-7)
    const QPersistentModelIndex persistentIdx(idx);

    // 右键非活动项 → 清除旧选择并选中该项 (参照 Dolphin)
    if (idx.isValid()) {
        QString p = filePathAtIndex(idx);
        if (!selectedFiles().contains(p)) {
            v->selectionModel()->select(idx,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
    }

    bool onItem = idx.isValid() && !selectedFiles().isEmpty();

    QString itemPath = onItem ? idx.data(Qt::UserRole).toString() : QString();
    bool itemIsDir = onItem && idx.data(Qt::UserRole + 1).toBool();  // FlatModel stores isDir
    bool single = onItem && (selectedFiles().size() == 1);

    QMenu menu;
    menu.addAction(QStringLiteral("刷新"), this, &QtFileMgrFileView::reload);
    QAction *undoAction = menu.addAction(
        style()->standardIcon(QStyle::SP_ArrowBack),
        QStringLiteral("撤销上一次文件操作"),
        this,
        &QtFileMgrFileView::undoLastOperation);
    undoAction->setEnabled(canUndo() && !m_activeJob && m_operationQueue.isEmpty());
    QAction *hiddenAction = menu.addAction(QStringLiteral("显示隐藏文件"));
    hiddenAction->setCheckable(true);
    hiddenAction->setChecked(showHiddenFiles());
    connect(hiddenAction, &QAction::triggered, this, [this](bool checked) {
        setShowHiddenFiles(checked);
    });
    QAction *foldersFirstAction = menu.addAction(QStringLiteral("文件夹优先"));
    foldersFirstAction->setCheckable(true);
    foldersFirstAction->setChecked(foldersFirst());
    connect(foldersFirstAction, &QAction::triggered, this, [this](bool checked) {
        setFoldersFirst(checked);
    });
    menu.addSeparator();

    const QMimeData *md = QApplication::clipboard()->mimeData();
    bool canPaste = md && md->hasUrls();
    const bool backendWritable = isDirectoryWritable(currentDirectoryPath());
    const bool canCreateHere = backendWritable && (!m_cfg || m_cfg->allowCreate);
    const bool canRenameHere = backendWritable && (!m_cfg || m_cfg->allowRename);
    const bool canDeleteHere = backendWritable && (!m_cfg || m_cfg->allowDelete);
    const bool canMoveHere = canCreateHere && canRenameHere && canDeleteHere;

    // ── 公共顶部操作（参考 Dolphin 的 KHamburgerMenu 前置动作）──
    // Create New — 目录上和有空白时都可用
    if (canCreateHere && (!onItem || itemIsDir)) {
        QMenu *newMenu = menu.addMenu(QStringLiteral("新建"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        newMenu->addAction(QStringLiteral("文件夹..."),
                           QKeySequence(QStringLiteral("Ctrl+Shift+N")), this,
                           &QtFileMgrFileView::createNewFolder);
#else
        newMenu->addAction(QStringLiteral("文件夹..."), this,
                           &QtFileMgrFileView::createNewFolder,
                           QKeySequence(QStringLiteral("Ctrl+Shift+N")));
#endif
        newMenu->addAction(QStringLiteral("空白文本文件"), this, [this]() {
            createFileFromTemplate(QString());
        });
        const QStringList templates = availableTemplateFiles();
        if (!templates.isEmpty()) {
            QMenu *templateMenu = newMenu->addMenu(QStringLiteral("从模板"));
            for (const QString &templatePath : templates) {
                templateMenu->addAction(
                    QFileInfo(templatePath).completeBaseName(), this,
                    [this, templatePath]() {
                    createFileFromTemplate(templatePath);
                });
            }
        }
    }
    // Paste — 始终可见。选中唯一目录时: "Paste Into Folder"
    if (canCreateHere && canPaste) {
        QStringList sel = selectedFiles();
        bool intoFolder = (sel.size() == 1 && m_io && m_io->isDir(sel.first()));
        bool isCut = md->hasFormat(QLatin1String(CUT_MIME));
        // 与 pasteFiles 的 canPasteMove 同源:配置禁改名/删除时剪切
        // 粘贴必失败,菜单纯灰化,不再先显示"移动到此处" (R2-FV-P2-7)
        const bool moveAllowed = !m_cfg
            || (m_cfg->allowRename && m_cfg->allowDelete);
        QString pasteText;
        if (intoFolder)
            pasteText = isCut ? QStringLiteral("移动到此处") : QStringLiteral("复制到此处");
        else if (isCut)
            pasteText = QStringLiteral("粘贴 %1 个项目").arg(md->urls().size());
        else
            pasteText = md->urls().size() > 1
                ? QStringLiteral("粘贴 %1 个项目").arg(md->urls().size())
                : QStringLiteral("粘贴");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QAction *pasteAction = menu.addAction(pasteText, QKeySequence::Paste, this, &QtFileMgrFileView::pasteFiles);
#else
        QAction *pasteAction = menu.addAction(pasteText, this, &QtFileMgrFileView::pasteFiles, QKeySequence::Paste);
#endif
        if (isCut && !moveAllowed) {
            qWarning("[FileView] context menu: clipboard is cut but move is disabled by config, paste grayed out");
            pasteAction->setEnabled(false);
        }
    }
    if (!onItem || itemIsDir) menu.addSeparator();

    // ── 文件/目录操作 ──
    if (onItem) {
        menu.addAction(QStringLiteral("打开"), this, [this, persistentIdx]() {
            if (persistentIdx.isValid())
                navigateTo(persistentIdx);
        });
        bool allFiles = true;
        const QStringList openPaths = selectedFiles();
        for (const QString &path : openPaths) {
            if (m_io && m_io->isDir(path)) {
                allFiles = false;
                break;
            }
        }
        if (allFiles && !openPaths.isEmpty()) {
            menu.addAction(QStringLiteral("打开方式..."), this,
                           [this, openPaths]() { openWith(openPaths); });
        }
        if (single && !itemIsDir) {
            menu.addAction(QStringLiteral("打开所在位置"), this, [this, itemPath]() {
                QString dirPath = itemPath.left(itemPath.lastIndexOf(QLatin1Char('/')));
                if (dirPath.isEmpty())
                    dirPath = QStringLiteral("/");
                if (m_io && m_io->isRemote()) {
                    QUrl url = m_url;
                    url.setPath(dirPath.startsWith(QLatin1Char('/')) ? dirPath : QLatin1Char('/') + dirPath);
                    setUrl(url);
                } else {
                    setUrl(QUrl::fromLocalFile(dirPath));
                }
                selectNewFiles({QFileInfo(itemPath).fileName()});
            });
            if (m_io && m_io->supportsCapability(QtFileMgrIO::VersionsCapability)) {
                menu.addAction(QStringLiteral("版本历史..."), this,
                               [this, itemPath]() {
                    showVersionHistory(itemPath);
                });
            }
        }
        menu.addSeparator();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        menu.addAction(QStringLiteral("剪切"),  QKeySequence::Cut, this, &QtFileMgrFileView::cutSelected);
        menu.addAction(QStringLiteral("复制"), QKeySequence::Copy, this, &QtFileMgrFileView::copySelected);
#else
        menu.addAction(QStringLiteral("剪切"),  this, &QtFileMgrFileView::cutSelected,
                       QKeySequence::Cut);
        menu.addAction(QStringLiteral("复制"), this, &QtFileMgrFileView::copySelected,
                       QKeySequence::Copy);
#endif
        if (single)
            menu.addAction(QStringLiteral("复制路径"), this, [this]() {
                QApplication::clipboard()->setText(selectedFiles().first());
            });
        if (m_io && m_io->supportsCapability(
                QtFileMgrIO::LocalExportCapability)) {
            menu.addAction(QStringLiteral("下载到..."), this,
                           &QtFileMgrFileView::downloadSelected);
        }

        // Copy To / Move To (参照 Dolphin 同名子菜单)
        QMenu *copyToMenu = canCreateHere ? menu.addMenu(QStringLiteral("复制到")) : nullptr;
        QMenu *moveToMenu = canMoveHere ? menu.addMenu(QStringLiteral("移动到")) : nullptr;
        auto doCopyOrMove = [this](bool move) {
            QString dest;
            bool remote = m_io && m_io->isRemote();
            if (remote) {
                // 远程模式: 使用 PathPicker + 当前 NetworkIO 选择目标目录
                QtFileMgrPathPicker picker;
                dest = picker.selectPath(m_io, QString(),
                    move ? QStringLiteral("选择远端目标目录 (移动)") : QStringLiteral("选择远端目标目录 (复制)"));
            } else {
                dest = QFileDialog::getExistingDirectory(this,
                    move ? QStringLiteral("移动到...") : QStringLiteral("复制到..."),
                    QDir::homePath());
            }
            if (dest.isEmpty()) return;
            startFileOperation(selectedFiles(), dest, move);
        };
        if (copyToMenu)
            copyToMenu->addAction(QStringLiteral("选择目标..."), this,
                                  [doCopyOrMove]() { doCopyOrMove(false); });
        if (moveToMenu)
            moveToMenu->addAction(QStringLiteral("选择目标..."), this,
                                  [doCopyOrMove]() { doCopyOrMove(true); });

        if (canCreateHere && !itemIsDir) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("复制到此处"), this, [this, itemPath]() {
                startFileOperation({itemPath}, operationParentDirectory(itemPath), false, false);
            });
        }

        menu.addSeparator();
        if (canRenameHere && single)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            menu.addAction(QStringLiteral("重命名"), QKeySequence(Qt::Key_F2), this, &QtFileMgrFileView::renameCurrent);
#else
            menu.addAction(QStringLiteral("重命名"), this, &QtFileMgrFileView::renameCurrent,
                           QKeySequence(Qt::Key_F2));
#endif
        if (canDeleteHere) {
            // 无回收站能力的后端不显示"移动到回收站":执行期虽会降级为
            // 永久删除,但标签先骗了用户 (R2-FV-P2-6)
            const bool trashSupported = m_io
                && m_io->supportsCapability(QtFileMgrIO::TrashCapability);
            if (trashSupported) {
                QAction *delAction = menu.addAction(QStringLiteral("移动到回收站"));
                delAction->setShortcut(QKeySequence::Delete);
                connect(delAction, &QAction::triggered, this, &QtFileMgrFileView::deleteSelected);
                QAction *permAction = menu.addAction(QStringLiteral("删除"));
                permAction->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_Delete));
                connect(permAction, &QAction::triggered, this, [this]() { deletePermanent(); });
            } else {
                qWarning("[FileView] context menu: backend has no trash, offering permanent delete only (backend=%s)",
                         m_io ? m_io->metaObject()->className() : "<null>");
                QAction *permAction = menu.addAction(QStringLiteral("删除"));
                permAction->setShortcut(QKeySequence::Delete);
                connect(permAction, &QAction::triggered, this, [this]() { deletePermanent(); });
            }
        }
    }

    // ── 排序 / 视图 / 属性 ──
    menu.addSeparator();
    QMenu *sortMenu = menu.addMenu(QStringLiteral("排序方式"));
    addSortActions(sortMenu);

    QMenu *viewMenu = menu.addMenu(QStringLiteral("视图模式"));
    viewMenu->addAction(QStringLiteral("图标"),   this, [this](){ setViewMode(IconsView); });
    viewMenu->addAction(QStringLiteral("紧凑"), this, [this](){ setViewMode(CompactView); });
    viewMenu->addAction(QStringLiteral("列表"), this, [this](){ setViewMode(DetailsView); });

    menu.addSeparator();
    if (onItem) {
        menu.addAction(QStringLiteral("属性"), this, [this, persistentIdx]() {
            if (persistentIdx.isValid())
                showProperties(persistentIdx);
        });
    } else {
        menu.addAction(QStringLiteral("属性"), this, [this]() { showProperties(QModelIndex()); });
    }

    // 扩展菜单项 (集成项目通过 addContextMenuAction 注入)
    if (!m_extraContextActions.isEmpty()) {
        menu.addSeparator();
        for (auto *a : m_extraContextActions)
            menu.addAction(a);
    }

    // 动态扩展钩子 (集成项目根据 IO 模式注入菜单项)
    if (m_contextMenuHook) {
        menu.addSeparator();
        m_contextMenuHook(menu, selectedFiles(), m_io && m_io->isRemote());
    }

    menu.exec(v->viewport()->mapToGlobal(pos));
}

QString QtFileMgrFileView::filePathAtIndex(const QModelIndex &proxyIdx) const
{
    return proxyIdx.data(Qt::UserRole).toString();
}

QString QtFileMgrFileView::currentDirectoryPath() const
{
    return m_io ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
}

bool QtFileMgrFileView::isDirectoryWritable(const QString &path) const
{
    const bool validPath = !path.isEmpty() || (m_io && m_io->isRemote());
    return m_io && m_io->canWrite() && validPath && m_io->isDir(path)
        && m_io->isWritable(path);
}

void QtFileMgrFileView::addSortActions(QMenu *menu)
{
    struct { const char *label; int col; } sorts[] = {
        {"名称", 0}, {"大小", 1}, {"类型", 2}, {"修改日期", 3}
    };
    QActionGroup *grp = new QActionGroup(menu);
    for (auto &s : sorts) {
        QAction *a = menu->addAction(QString::fromUtf8(s.label));
        a->setCheckable(true);
        a->setActionGroup(grp);
        if (s.col == m_flatModel->sortColumn()) a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, s]() {
            Qt::SortOrder order = Qt::AscendingOrder;
            if (s.col == m_flatModel->sortColumn()) {
                order = m_flatModel->sortOrder() == Qt::AscendingOrder
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
            }
            setSortColumn(s.col, order);
        });
    }

    menu->addSeparator();
    QActionGroup *orderGrp = new QActionGroup(menu);
    QAction *asc = menu->addAction(QStringLiteral("升序"));
    asc->setCheckable(true);
    asc->setActionGroup(orderGrp);
    asc->setChecked(m_flatModel->sortOrder() == Qt::AscendingOrder);
    connect(asc, &QAction::triggered, this, [this]() {
        setSortColumn(m_flatModel->sortColumn(), Qt::AscendingOrder);
    });

    QAction *desc = menu->addAction(QStringLiteral("降序"));
    desc->setCheckable(true);
    desc->setActionGroup(orderGrp);
    desc->setChecked(m_flatModel->sortOrder() == Qt::DescendingOrder);
    connect(desc, &QAction::triggered, this, [this]() {
        setSortColumn(m_flatModel->sortColumn(), Qt::DescendingOrder);
    });
}

void QtFileMgrFileView::updateSortIndicator()
{
    if (!m_detailView)
        return;
    QHeaderView *header = m_detailView->horizontalHeader();
    if (!header)
        return;
    header->setSortIndicator(m_flatModel->sortColumn(), m_flatModel->sortOrder());
}

// 参考 Dolphin KNewFileMenu: 创建后立即选中并进入重命名
void QtFileMgrFileView::createNewFolder()
{
    if (m_cfg && !m_cfg->allowCreate) {
        showMessage(QStringLiteral("当前配置不允许新建文件夹。"), QStringLiteral("warning"));
        return;
    }
    if (!isDirectoryWritable(currentDirectoryPath())) {
        showMessage(QStringLiteral("当前目录不可写，无法新建文件夹。"), QStringLiteral("warning"));
        return;
    }

    QString name = QStringLiteral("新建文件夹");
    QString parentPath = m_io ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
    int n = 1;
    auto exists = [&](const QString &p) { return m_io ? m_io->exists(p) : QDir(p).exists(); };
    while (exists(joinedPath(parentPath, name))) name = QStringLiteral("新建文件夹 %1").arg(++n);

    QString fullPath = joinedPath(parentPath, name);
    if (!m_io->mkdir(fullPath)) {
        showMessage(QStringLiteral("无法创建文件夹: %1").arg(fullPath), QStringLiteral("error"));
        return;
    }

    // reload 会同步清空模型,选中与重命名都必须经 pending 机制在
    // directoryLoadFinished 后落地 (UI-P1-3)
    m_pendingRenameName = name;
    reload();
    selectNewFiles({name});
    if (!m_flatModel->isLoading())
        tryStartPendingRename();  // 同步后端:目录已就绪,直接判定
}

QStringList QtFileMgrFileView::availableTemplateFiles() const
{
    QStringList result;
    QSet<QString> seen;
    QStringList directories;
    const QString home = QDir::homePath();
    const QByteArray configHomeValue = qgetenv("XDG_CONFIG_HOME");
    const QString configHome = configHomeValue.isEmpty()
        ? QDir(home).filePath(QStringLiteral(".config"))
        : QString::fromLocal8Bit(configHomeValue);
    const QString userDirsPath =
        QDir(configHome).filePath(QStringLiteral("user-dirs.dirs"));
    bool templatesConfigured = false;
    bool templatesDisabled = false;
    if (QFileInfo(userDirsPath).isReadable()) {
        QSettings userDirs(userDirsPath, QSettings::IniFormat);
        QString configured =
            userDirs.value(QStringLiteral("XDG_TEMPLATES_DIR")).toString();
        if (configured.size() >= 2 && configured.startsWith(QLatin1Char('"'))
            && configured.endsWith(QLatin1Char('"'))) {
            configured = configured.mid(1, configured.size() - 2);
        }
        configured.replace(QStringLiteral("$HOME"), home);
        if (!configured.isEmpty()) {
            const QString cleanConfigured = QDir::cleanPath(configured);
            if (cleanConfigured == QDir::cleanPath(home)) {
                templatesDisabled = true;
                qWarning("[FileView] templates: XDG_TEMPLATES_DIR disables templates by pointing to HOME ('%s')",
                         qPrintable(cleanConfigured));
            } else {
                templatesConfigured = true;
                directories << cleanConfigured;
            }
        } else {
            qWarning("[FileView] templates: '%s' has no XDG_TEMPLATES_DIR value",
                     qPrintable(userDirsPath));
        }
    } else {
        qWarning("[FileView] templates: user directory config is not readable: '%s'",
                 qPrintable(userDirsPath));
    }
    if (!templatesConfigured && !templatesDisabled) {
        directories << QDir(home).filePath(QStringLiteral("Templates"))
                    << QDir(home).filePath(QStringLiteral("模板"));
    }
    QSet<QString> seenDirectories;
    for (const QString &directory : directories) {
        const QString cleanDirectory = QDir::cleanPath(directory);
        if (seenDirectories.contains(cleanDirectory)) {
            qWarning("[FileView] templates: skipped duplicate directory '%s'",
                     qPrintable(cleanDirectory));
            continue;
        }
        seenDirectories.insert(cleanDirectory);
        const QFileInfo directoryInfo(directory);
        if (!directoryInfo.exists() || !directoryInfo.isDir()) {
            qWarning("[FileView] templates: skipped invalid template directory '%s'",
                     qPrintable(directory));
            continue;
        }
        const QFileInfoList entries = QDir(directory).entryInfoList(
            QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
            QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &entry : entries) {
            const QString canonical = entry.canonicalFilePath();
            if (canonical.isEmpty()) {
                qWarning("[FileView] templates: skipped unresolved template '%s'",
                         qPrintable(entry.absoluteFilePath()));
                continue;
            }
            if (seen.contains(canonical)) {
                qWarning("[FileView] templates: skipped duplicate template '%s' from '%s'",
                         qPrintable(canonical), qPrintable(directory));
                continue;
            }
            if (result.size() >= 30) {
                qWarning("[FileView] templates: skipped '%s' because the menu limit is 30",
                         qPrintable(canonical));
                continue;
            }
            seen.insert(canonical);
            result.append(canonical);
        }
    }
    return result;
}

bool QtFileMgrFileView::createFileFromTemplate(
    const QString &templatePath, QString *createdPath)
{
    if (!m_io || (m_cfg && !m_cfg->allowCreate)
        || !isDirectoryWritable(currentDirectoryPath())) {
        qWarning("[FileView] createTemplate: rejected template '%s' in '%s' (io=%p, allowCreate=%d)",
                 qPrintable(templatePath), qPrintable(currentDirectoryPath()),
                 static_cast<void *>(m_io), m_cfg ? m_cfg->allowCreate : 1);
        showMessage(QStringLiteral("当前目录不可写，无法创建文件。"),
                    QStringLiteral("warning"));
        return false;
    }

    QByteArray contents;
    QString name = QStringLiteral("新建文本文件.txt");
    if (!templatePath.isEmpty()) {
        QFileInfo templateInfo(templatePath);
        if (!templateInfo.isFile() || !templateInfo.isReadable()) {
            qWarning("[FileView] createTemplate: invalid template '%s'",
                     qPrintable(templatePath));
            showMessage(QStringLiteral("模板不可读取。"), QStringLiteral("error"));
            return false;
        }
        static const qint64 kMaximumTemplateSize = 16 * 1024 * 1024;
        if (templateInfo.size() > kMaximumTemplateSize) {
            qWarning("[FileView] createTemplate: skipped oversized template '%s' (%lld bytes)",
                     qPrintable(templatePath),
                     static_cast<long long>(templateInfo.size()));
            showMessage(QStringLiteral("模板超过 16 MB，无法创建。"),
                        QStringLiteral("error"));
            return false;
        }
        QFile source(templatePath);
        if (!source.open(QIODevice::ReadOnly)) {
            qWarning("[FileView] createTemplate: cannot open '%s': %s",
                     qPrintable(templatePath), qPrintable(source.errorString()));
            showMessage(QStringLiteral("模板读取失败。"), QStringLiteral("error"));
            return false;
        }
        contents = source.readAll();
        name = templateInfo.fileName();
    }

    const QString parentPath = currentDirectoryPath();
    const QString destination =
        QtFileMgrIO::uniqueDestName(parentPath, name, m_io);
    if (!m_io->writeFile(destination, contents)) {
        qWarning("[FileView] createTemplate: backend failed creating '%s' from '%s'",
                 qPrintable(destination), qPrintable(templatePath));
        showMessage(QStringLiteral("无法从模板创建文件: %1").arg(destination),
                    QStringLiteral("error"));
        return false;
    }
    if (createdPath)
        *createdPath = destination;
    observeCreatedFile(QFileInfo(destination).fileName());
    return true;
}

bool QtFileMgrFileView::openWith(const QStringList &paths)
{
    if (!m_io || paths.isEmpty()) {
        qWarning("[FileView] openWith: ignored empty request (io=%p, count=%d)",
                 static_cast<void *>(m_io), paths.size());
        return false;
    }
    QString mimeType = m_io->mimeType(paths.first());
    for (int i = 1; i < paths.size(); ++i) {
        const QString candidate = m_io->mimeType(paths.at(i));
        if (candidate != mimeType) {
            qWarning("[FileView] openWith: mixed MIME types '%s' and '%s'; using empty MIME hint",
                     qPrintable(mimeType), qPrintable(candidate));
            mimeType.clear();
            break;
        }
    }
    emit openWithRequested(paths, mimeType);
    if (!m_openWithHandler) {
        qWarning("[FileView] openWith: no handler installed for %d path(s), MIME '%s'",
                 paths.size(), qPrintable(mimeType));
        showMessage(QStringLiteral("调用方未配置“打开方式”处理器。"),
                    QStringLiteral("warning"));
        return false;
    }
    if (!m_openWithHandler(paths, mimeType, this)) {
        qWarning("[FileView] openWith: handler rejected %d path(s), MIME '%s'",
                 paths.size(), qPrintable(mimeType));
        showMessage(QStringLiteral("没有可用的打开方式。"),
                    QStringLiteral("warning"));
        return false;
    }
    return true;
}

void QtFileMgrFileView::showProperties(const QModelIndex &idx)
{
    if (!m_io)
        return;
    QStringList paths = idx.isValid() ? selectedFiles() : QStringList();
    if (paths.isEmpty()) {
        paths << (m_io ? m_io->pathFromUrl(m_url) : m_url.toLocalFile());
    }
    auto *dialog = new QtFileMgrPropertiesDialog(m_io, paths, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QtFileMgrPropertiesDialog::permissionsApplied,
            this, [this]() { reload(); });
    dialog->show();
}

void QtFileMgrFileView::cutSelected()
{
    m_cutFiles = selectedFiles();
    QMimeData *md = new QMimeData;
    QList<QUrl> urls;
    bool remote = m_io && m_io->isRemote();
    for (const QString &f : m_cutFiles)
        urls << (remote ? m_io->urlForPath(f)
                        : QUrl::fromLocalFile(f));
    md->setUrls(urls);
    md->setData(QLatin1String(CUT_MIME), QByteArrayLiteral("1"));
    QApplication::clipboard()->setMimeData(md);
}

void QtFileMgrFileView::copySelected()
{
    m_cutFiles.clear();
    QMimeData *md = new QMimeData;
    QList<QUrl> urls;
    bool remote = m_io && m_io->isRemote();
    for (const QString &f : selectedFiles())
        urls << (remote ? m_io->urlForPath(f)
                        : QUrl::fromLocalFile(f));
    md->setUrls(urls);
    // 不设 CUT_MIME → paste 时识别为 copy
    QApplication::clipboard()->setMimeData(md);
}

// 参照 Dolphin observeCreatedItem: 注册待选文件, 由目录监视刷新后选中
void QtFileMgrFileView::observeCreatedFile(const QString &name)
{
    if (name.isEmpty())
        return;
    if (m_pendingSelectionUrl != m_url) {
        m_pendingSelection.clear();
        m_pendingSelectionUrl = m_url;
    }
    if (!m_pendingSelection.contains(name))
        m_pendingSelection.append(name);
    m_pendingSelectionTimer->start();
    scheduleReload();
}

void QtFileMgrFileView::connectSelectionSignals()
{
    disconnect(m_iconView->selectionModel(), nullptr, this, nullptr);
    disconnect(m_compactView->selectionModel(), nullptr, this, nullptr);
    disconnect(m_detailView->selectionModel(), nullptr, this, nullptr);
    connect(m_iconView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &QtFileMgrFileView::onSelectionChanged);
    connect(m_compactView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &QtFileMgrFileView::onSelectionChanged);
    connect(m_detailView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &QtFileMgrFileView::onSelectionChanged);
}

void QtFileMgrFileView::selectNewFiles(const QStringList &names)
{
    if (names.isEmpty()) return;
    if (m_flatModel->isLoading()) {
        if (m_pendingSelectionUrl != m_url) {
            m_pendingSelection.clear();
            m_pendingSelectionUrl = m_url;
        }
        for (const QString &name : names) {
            if (!name.isEmpty() && !m_pendingSelection.contains(name))
                m_pendingSelection.append(name);
        }
        if (!m_pendingSelection.isEmpty())
            m_pendingSelectionTimer->start();
        return;
    }
    QAbstractItemView *v = static_cast<QAbstractItemView *>(m_stack->currentWidget());
    if (v->selectionModel())
        v->selectionModel()->clearSelection();
    bool first = true;
    for (int i = 0; i < m_flatModel->rowCount(); ++i) {
        QModelIndex idx = m_flatModel->index(i, 0);
        if (names.contains(m_flatModel->index(i, 0).data(Qt::DisplayRole).toString())) {
            if (first) { v->setCurrentIndex(idx); first = false; }
            v->selectionModel()->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    if (!first) {
        v->doItemsLayout();
        v->scrollTo(v->currentIndex(), QAbstractItemView::PositionAtCenter);
        v->viewport()->update();
    }
}

void QtFileMgrFileView::pasteFiles()
{
    const QMimeData *md = QApplication::clipboard()->mimeData();
    if (!md || !md->hasUrls()) return;

    bool remote = m_io && m_io->isRemote();
    bool isCut = md->hasFormat(QLatin1String(CUT_MIME));
    QStringList sel = selectedFiles();
    QString destDir = m_io ? m_io->pathFromUrl(m_url) : m_url.toLocalFile();
    if (destDir.isEmpty() && !remote) {
        qWarning("[PASTE] destDir is empty in local mode, invalid target");
        return;
    }
    if (sel.size() == 1 && m_io && m_io->isDir(sel.first()))
        destDir = sel.first();
    const bool canPasteCopy = (!m_cfg || m_cfg->allowCreate)
        && isDirectoryWritable(destDir);
    const bool canPasteMove = canPasteCopy
        && (!m_cfg || (m_cfg->allowRename && m_cfg->allowDelete));
    if ((isCut && !canPasteMove) || (!isCut && !canPasteCopy)) {
        showMessage(QStringLiteral("当前配置或目录权限不允许粘贴。"),
                    QStringLiteral("warning"));
        return;
    }

    QStringList srcs;
    QSet<QString> importSources;  // 远端目标下的本机来源 (审查#1 显式方向)
    // 跨后端项目: 按源后端分组 (源后端, 源路径列表)
    QList<QPair<QtFileMgrIO *, QString>> foreignItems;
    for (const QUrl &url : md->urls()) {
        QString src;
        if (remote) {
            if (m_io->acceptsUrl(url)) {
                src = m_io->pathFromUrl(url);
            } else if (url.isLocalFile()) {
                src = url.toLocalFile();  // 本地文件粘贴到远程 (显式上传)
                importSources.insert(src);
            } else if (QtFileMgrIO *sourceBackend = remoteBackendForUrl(url)) {
                // 其他远端节点 → 本地暂存中转上传
                const QString sourcePath = sourceBackend->pathFromUrl(url);
                if (sourcePath.isEmpty()) {
                    qWarning("[FileView] pasteFiles: source backend rejected its own URL (backend=%s, url=\"%s\")",
                             sourceBackend->metaObject()->className(),
                             qPrintable(url.toString(QUrl::RemovePassword)));
                } else {
                    foreignItems.append({sourceBackend, sourcePath});
                }
            } else {
                qWarning("[FileView] pasteFiles: skipped URL from incompatible backend (current=%s, url=\"%s\")",
                         m_io->metaObject()->className(),
                         qPrintable(url.toString(QUrl::RemovePassword)));
            }
        } else {
            if (url.isLocalFile()) {
                src = url.toLocalFile();
            } else if (QtFileMgrIO *sourceBackend = remoteBackendForUrl(url)) {
                // 远端项目粘贴到本地 → 经下载流
                const QString sourcePath = sourceBackend->pathFromUrl(url);
                if (sourcePath.isEmpty()) {
                    qWarning("[FileView] pasteFiles: source backend rejected its own URL (backend=%s, url=\"%s\")",
                             sourceBackend->metaObject()->className(),
                             qPrintable(url.toString(QUrl::RemovePassword)));
                } else {
                    foreignItems.append({sourceBackend, sourcePath});
                }
            } else {
                qWarning("[FileView] pasteFiles: skipped URL without a connected source backend (url=\"%s\")",
                         qPrintable(url.toString(QUrl::RemovePassword)));
            }
        }
        if (!src.isEmpty() && !(isCut && src == destDir))
            srcs << src;
    }
    bool anyStarted = false;
    if (!srcs.isEmpty())
        anyStarted = startFileOperation(srcs, destDir, isCut,
                                        true, importSources) || anyStarted;
    if (!foreignItems.isEmpty()) {
        // 按源后端分组,分别走下载/中转流;剪切语义 = 成功后删除源
        QHash<QtFileMgrIO *, QStringList> grouped;
        for (const auto &item : foreignItems)
            grouped[item.first].append(item.second);
        for (auto it = grouped.begin(); it != grouped.end(); ++it) {
            if (remote)
                startSerialRelay(it.key(), it.value(), destDir, isCut);
            else
                startSerialDownload(it.key(), it.value(), destDir, isCut);
            anyStarted = true;
        }
    }
    if (anyStarted)
        m_cutFiles.clear();
}

void QtFileMgrFileView::downloadSelected()
{
    if (!m_io || !m_io->supportsCapability(
            QtFileMgrIO::LocalExportCapability)) {
        qWarning("[FileView] downloadSelected: rejected unsupported backend (backend=%s)",
                 m_io ? m_io->metaObject()->className() : "<null>");
        showMessage(QStringLiteral("当前文件后端不支持下载到本地。"),
                    QStringLiteral("warning"));
        return;
    }
    const QStringList sources = selectedFiles();
    if (sources.isEmpty()) {
        qWarning("[FileView] downloadSelected: skipped empty selection (url=\"%s\")",
                 qPrintable(m_url.toString(QUrl::RemovePassword)));
        return;
    }
    const QString destinationDirectory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("下载到..."), QDir::homePath());
    if (destinationDirectory.isEmpty())
        return;
    startSerialDownload(m_io, sources, destinationDirectory, false);
}

void QtFileMgrFileView::startSerialDownload(
    QtFileMgrIO *sourceIO, const QStringList &sources,
    const QString &destinationDirectory, bool deleteSourcesAfter)
{
    if (!sourceIO || !sourceIO->supportsCapability(
            QtFileMgrIO::LocalExportCapability)) {
        qWarning("[FileView] startSerialDownload: rejected unsupported backend (backend=%s)",
                 sourceIO ? sourceIO->metaObject()->className() : "<null>");
        showMessage(QStringLiteral("当前文件后端不支持下载到本地。"),
                    QStringLiteral("warning"));
        return;
    }
    if (sources.isEmpty() || destinationDirectory.isEmpty()) {
        qWarning("[FileView] startSerialDownload: rejected empty sources/destination (sources=%d, destination=\"%s\")",
                 sources.size(), qPrintable(destinationDirectory));
        return;
    }
    // 任务队列忙或已有串行传输时延后,避免对同名目标的并发竞争
    // (审查#8/R2-02)
    if (m_activeJob || !m_operationQueue.isEmpty()
        || m_activeSerialTransfers > 0) {
        qWarning("[FileView] startSerialDownload: deferred while transfers are busy (sources=%d, destination=\"%s\", activeSerial=%d)",
                 sources.size(), qPrintable(destinationDirectory),
                 m_activeSerialTransfers);
        showMessage(QStringLiteral("跨后端传输已排队，等待当前任务完成..."),
                    QStringLiteral("info"));
        const QPointer<QtFileMgrFileView> self(this);
        const QPointer<QtFileMgrIO> guardedSourceIO(sourceIO);
        m_deferredTransfers.append(
            [self, guardedSourceIO, sources, destinationDirectory,
             deleteSourcesAfter]() {
            if (self && guardedSourceIO)
                self->startSerialDownload(guardedSourceIO, sources,
                                          destinationDirectory,
                                          deleteSourcesAfter);
        });
        return;
    }
    ++m_activeSerialTransfers;

    struct DownloadState {
        QStringList sources;
        QString destinationDirectory;
        int index = 0;
        int succeeded = 0;
        int failed = 0;
        bool cancelled = false;
        bool finished = false;
        QStringList errors;
        QStringList succeededSources;
        QPointer<QtFileMgrTransferRequest> request;
        QPointer<QProgressDialog> progress;
        std::shared_ptr<std::function<void()>> continuation;
    };
    const std::shared_ptr<DownloadState> state(new DownloadState);
    state->sources = sources;
    state->destinationDirectory = destinationDirectory;
    state->progress = new QProgressDialog(
        QStringLiteral("正在下载..."), QStringLiteral("取消"),
        0, sources.size(), this);
    state->progress->setWindowTitle(QStringLiteral("下载远端项目"));
    state->progress->setWindowModality(Qt::WindowModal);
    state->progress->setMinimumDuration(0);

    const QPointer<QtFileMgrIO> guardedSource(sourceIO);
    const QPointer<QtFileMgrFileView> self(this);
    const std::shared_ptr<std::function<void()>> startNext(
        new std::function<void()>);
    state->continuation = startNext;
    const std::weak_ptr<DownloadState> weakState(state);
    const std::weak_ptr<std::function<void()>> weakStartNext(startNext);
    *startNext = [this, self, weakState, weakStartNext, guardedSource,
                  deleteSourcesAfter]() {
        const auto state = weakState.lock();
        if (!state)
            return;
        if (!self) {
            qWarning("[FileView] download: view destroyed; abandoned continuation (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            return;
        }
        if (state->finished) {
            qWarning("[FileView] download: skipped continuation after batch completion (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            return;
        }
        if (!state->progress) {
            qWarning("[FileView] download: progress owner destroyed; cancelling batch (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            state->cancelled = true;
        } else if (state->progress->wasCanceled()) {
            state->cancelled = true;
        }
        if (!guardedSource) {
            qWarning("[FileView] download: source backend destroyed; cancelling batch (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            state->cancelled = true;
            state->errors.append(QStringLiteral("源后端已断开"));
        }
        if (state->cancelled || state->index >= state->sources.size()) {
            state->finished = true;
            if (state->progress) {
                state->progress->close();
                if (state->progress)
                    state->progress->deleteLater();
            }
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            if (deleteSourcesAfter && !state->cancelled
                && state->failed == 0 && !state->succeededSources.isEmpty())
                deleteSourcesOnBackend(guardedSource, state->succeededSources);
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            QString summary = state->cancelled
                ? QStringLiteral("下载已取消：完成 %1，失败 %2")
                      .arg(state->succeeded).arg(state->failed)
                : QStringLiteral("已下载 %1 个项目")
                      .arg(state->succeeded);
            if (state->failed > 0)
                summary += QStringLiteral("，失败 %1 个").arg(state->failed);
            showMessage(summary, state->failed > 0
                ? QStringLiteral("error")
                : (state->cancelled ? QStringLiteral("warning")
                                    : QStringLiteral("info")));
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            if (!state->errors.isEmpty()) {
                const QPointer<QMessageBox> details = new QMessageBox(this);
                details->setIcon(QMessageBox::Critical);
                details->setWindowTitle(QStringLiteral("下载详情"));
                details->setText(QStringLiteral("部分远端项目下载失败。"));
                details->setDetailedText(state->errors.join(QLatin1Char('\n')));
                details->exec();
                if (details)
                    details->deleteLater();
                if (!serialViewAlive(self, state->destinationDirectory, "error details dialog"))
                    return;
            }
            state->continuation.reset();
            // 串行传输结束,放行排队的任务/延后传输 (审查#8)
            if (m_activeSerialTransfers > 0)
                --m_activeSerialTransfers;
            if (m_activeSerialTransfers == 0)
                QTimer::singleShot(0, this, [this]() {
                    startNextFileOperation();
                    if (!m_activeJob && m_operationQueue.isEmpty()
                        && !m_deferredTransfers.isEmpty()) {
                        const QList<std::function<void()>> deferred =
                            std::exchange(m_deferredTransfers, {});
                        for (const auto &start : deferred)
                            start();
                    }
                });
            return;
        }

        const QString source = state->sources.at(state->index);
        const QString name = QFileInfo(source).fileName();
        if (name.isEmpty()) {
            qWarning("[FileView] downloadSelected: skipped source without file name (source=\"%s\", destination=\"%s\")",
                     qPrintable(source),
                     qPrintable(state->destinationDirectory));
            state->failed++;
            state->errors.append(QStringLiteral("无法确定下载文件名: %1").arg(source));
            state->index++;
            if (const auto next = weakStartNext.lock())
                (*next)();
            return;
        }
        const QString destination = QDir(state->destinationDirectory)
            .filePath(name);
        if (QFileInfo::exists(destination)) {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                this, QStringLiteral("覆盖本地项目"),
                QStringLiteral("%1 已存在，是否覆盖？").arg(destination),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                QMessageBox::No);
            if (!self) {
                qWarning("[FileView] download: view destroyed during overwrite confirmation (source=\"%s\", destination=\"%s\")",
                         qPrintable(source), qPrintable(destination));
                return;
            }
            if (!state->progress || state->progress->wasCanceled() || !guardedSource) {
                qWarning("[FileView] download: context lost or cancelled during overwrite confirmation (source=\"%s\", destination=\"%s\", progressAvailable=%d, backendAvailable=%d)",
                         qPrintable(source), qPrintable(destination), !state->progress.isNull(),
                         !guardedSource.isNull());
                state->cancelled = true;
                if (const auto next = weakStartNext.lock())
                    (*next)();
                return;
            }
            if (answer == QMessageBox::Cancel) {
                state->cancelled = true;
                if (const auto next = weakStartNext.lock())
                    (*next)();
                return;
            }
            if (answer == QMessageBox::No) {
                qWarning("[FileView] downloadSelected: skipped existing destination by user choice (source=\"%s\", destination=\"%s\")",
                         qPrintable(source), qPrintable(destination));
                state->index++;
                if (const auto next = weakStartNext.lock())
                    (*next)();
                return;
            }
        }

        state->progress->setLabelText(
            QStringLiteral("正在下载 %1").arg(name));
        if (state->progress)
            state->progress->setValue(state->index);
        if (!self || !state->progress || !guardedSource) {
            qWarning("[FileView] download: context disappeared while updating progress (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            if (self) {
                state->cancelled = true;
                if (const auto next = weakStartNext.lock())
                    (*next)();
            }
            return;
        }
        QtFileMgrTransferRequest *request = guardedSource->exportToLocalRequest(
            source, destination, state->progress);
        if (!self) {
            qWarning("[FileView] download: view destroyed while creating request (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return;
        }
        state->request = request;
        watchSerialTransfer(this, request, state->progress, QStringLiteral("download"),
                source, destination,
                [this, state, weakStartNext, source](bool success,
                    const QString &, const QString &error, bool cancelled) {
            state->request = nullptr;
            if (success) {
                state->succeeded++;
                state->succeededSources.append(source);
            } else if (!cancelled) {
                state->failed++;
                state->errors.append(QStringLiteral("%1: %2").arg(
                    source, error.isEmpty() ? QStringLiteral("下载失败") : error));
            }
            state->cancelled = state->cancelled || cancelled;
            state->index++;
            if (const auto next = weakStartNext.lock())
                (*next)();
        });
    };
    (*startNext)();
}

void QtFileMgrFileView::startSerialRelay(
    QtFileMgrIO *sourceIO, const QStringList &sources,
    const QString &destinationDirectory, bool deleteSourcesAfter)
{
    if (!sourceIO || !sourceIO->supportsCapability(
            QtFileMgrIO::LocalExportCapability)) {
        qWarning("[FileView] startSerialRelay: source backend cannot export (backend=%s)",
                 sourceIO ? sourceIO->metaObject()->className() : "<null>");
        showMessage(QStringLiteral("源后端不支持跨节点传输。"),
                    QStringLiteral("warning"));
        return;
    }
    if (!m_io || sources.isEmpty() || destinationDirectory.isEmpty()) {
        qWarning("[FileView] startSerialRelay: rejected invalid arguments (sources=%d, destination=\"%s\")",
                 sources.size(), qPrintable(destinationDirectory));
        return;
    }
    // 任务队列忙或已有串行传输时延后 (审查#8/R2-02)
    if (m_activeJob || !m_operationQueue.isEmpty()
        || m_activeSerialTransfers > 0) {
        qWarning("[FileView] startSerialRelay: deferred while transfers are busy (sources=%d, destination=\"%s\", activeSerial=%d)",
                 sources.size(), qPrintable(destinationDirectory),
                 m_activeSerialTransfers);
        showMessage(QStringLiteral("跨后端传输已排队，等待当前任务完成..."),
                    QStringLiteral("info"));
        const QPointer<QtFileMgrFileView> self(this);
        const QPointer<QtFileMgrIO> guardedSourceIO(sourceIO);
        m_deferredTransfers.append(
            [self, guardedSourceIO, sources, destinationDirectory,
             deleteSourcesAfter]() {
            if (self && guardedSourceIO)
                self->startSerialRelay(guardedSourceIO, sources,
                                       destinationDirectory,
                                       deleteSourcesAfter);
        });
        return;
    }

    struct RelayState {
        QStringList sources;
        QString destinationDirectory;
        int index = 0;
        int succeeded = 0;
        int failed = 0;
        bool cancelled = false;
        bool finished = false;
        QStringList errors;
        QStringList succeededSources;
        QSharedPointer<QTemporaryDir> staging;
        QPointer<QtFileMgrTransferRequest> request;
        QPointer<QProgressDialog> progress;
        std::shared_ptr<std::function<void()>> continuation;
    };
    const std::shared_ptr<RelayState> state(new RelayState);
    state->sources = sources;
    state->destinationDirectory = destinationDirectory;
    // 绝对模板落在系统临时目录,不受进程 CWD 权限影响 (R2-10);
    // 忙计数在暂存创建成功后才增加,失败路径无泄漏
    const QString stagingTemplate = QDir::tempPath()
        + QStringLiteral("/qtfilemgr-relay-XXXXXX");
    state->staging = QSharedPointer<QTemporaryDir>::create(stagingTemplate);
    if (!state->staging->isValid()) {
        qWarning("[FileView] startSerialRelay: cannot create staging directory (template=\"%s\", cwd=\"%s\", error=\"%s\")",
                 qPrintable(stagingTemplate), qPrintable(QDir::currentPath()),
                 qPrintable(state->staging->errorString()));
        showMessage(QStringLiteral("无法创建本地中转目录。"),
                    QStringLiteral("error"));
        return;
    }
    ++m_activeSerialTransfers;
    state->progress = new QProgressDialog(
        QStringLiteral("正在中转传输..."), QStringLiteral("取消"),
        0, sources.size(), this);
    state->progress->setWindowTitle(QStringLiteral("跨节点传输"));
    state->progress->setWindowModality(Qt::WindowModal);
    state->progress->setMinimumDuration(0);

    const QPointer<QtFileMgrIO> guardedSource(sourceIO);
    const QPointer<QtFileMgrIO> guardedDest(m_io);
    const QPointer<QtFileMgrFileView> self(this);
    const std::shared_ptr<std::function<void()>> startNext(
        new std::function<void()>);
    state->continuation = startNext;
    const std::weak_ptr<RelayState> weakState(state);
    const std::weak_ptr<std::function<void()>> weakStartNext(startNext);
    *startNext = [this, self, weakState, weakStartNext, guardedSource, guardedDest,
                  deleteSourcesAfter]() {
        const auto state = weakState.lock();
        if (!state)
            return;
        if (!self) {
            qWarning("[FileView] relay: view destroyed; abandoned continuation (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            return;
        }
        if (state->finished) {
            qWarning("[FileView] relay: skipped continuation after batch completion (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            return;
        }
        if (!state->progress) {
            qWarning("[FileView] relay: progress owner destroyed; cancelling batch (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            state->cancelled = true;
        } else if (state->progress->wasCanceled()) {
            state->cancelled = true;
        }
        if (!guardedSource || !guardedDest) {
            qWarning("[FileView] relay: backend destroyed; cancelling batch (destination=\"%s\", index=%d)",
                     qPrintable(state->destinationDirectory), state->index);
            state->cancelled = true;
            state->errors.append(QStringLiteral("传输后端已断开"));
        }
        if (state->cancelled || state->index >= state->sources.size()) {
            state->finished = true;
            if (state->progress) {
                state->progress->close();
                if (state->progress)
                    state->progress->deleteLater();
            }
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            if (deleteSourcesAfter && !state->cancelled
                && state->failed == 0 && !state->succeededSources.isEmpty())
                deleteSourcesOnBackend(guardedSource, state->succeededSources);
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            QString summary = state->cancelled
                ? QStringLiteral("中转传输已取消：完成 %1，失败 %2")
                      .arg(state->succeeded).arg(state->failed)
                : QStringLiteral("已跨节点传输 %1 个项目").arg(state->succeeded);
            if (state->failed > 0)
                summary += QStringLiteral("，失败 %1 个").arg(state->failed);
            showMessage(summary, state->failed > 0
                ? QStringLiteral("error")
                : (state->cancelled ? QStringLiteral("warning")
                                    : QStringLiteral("info")));
            if (!serialViewAlive(self, state->destinationDirectory, "batch finalization"))
                return;
            if (!state->errors.isEmpty()) {
                const QPointer<QMessageBox> details = new QMessageBox(this);
                details->setIcon(QMessageBox::Critical);
                details->setWindowTitle(QStringLiteral("跨节点传输详情"));
                details->setText(QStringLiteral("部分项目跨节点传输失败。"));
                details->setDetailedText(state->errors.join(QLatin1Char('\n')));
                details->exec();
                if (details)
                    details->deleteLater();
                if (!serialViewAlive(self, state->destinationDirectory, "error details dialog"))
                    return;
            }
            state->continuation.reset();
            // 串行传输结束,放行排队的任务/延后传输 (审查#8)
            if (m_activeSerialTransfers > 0)
                --m_activeSerialTransfers;
            if (m_activeSerialTransfers == 0)
                QTimer::singleShot(0, this, [this]() {
                    startNextFileOperation();
                    if (!m_activeJob && m_operationQueue.isEmpty()
                        && !m_deferredTransfers.isEmpty()) {
                        const QList<std::function<void()>> deferred =
                            std::exchange(m_deferredTransfers, {});
                        for (const auto &start : deferred)
                            start();
                    }
                });
            return;
        }

        const QString source = state->sources.at(state->index);
        const QString name = QFileInfo(source).fileName();
        if (name.isEmpty()) {
            qWarning("[FileView] relay: skipped source without file name (source=\"%s\")",
                     qPrintable(source));
            state->failed++;
            state->errors.append(QStringLiteral("无法确定文件名: %1").arg(source));
            state->index++;
            if (const auto next = weakStartNext.lock())
                (*next)();
            return;
        }
        // 暂存路径加序号前缀避免同名冲突;上传时目标名仍为原始 name
        const QString stagingPath = QDir(state->staging->path()).filePath(
            QStringLiteral("%1-%2").arg(state->index).arg(name));
        state->progress->setLabelText(
            QStringLiteral("正在中转 %1").arg(name));
        if (state->progress)
            state->progress->setValue(state->index);
        if (!self || !state->progress || !guardedSource || !guardedDest) {
            qWarning("[FileView] relay: context disappeared while updating progress (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(stagingPath));
            if (self) {
                state->cancelled = true;
                if (const auto next = weakStartNext.lock())
                    (*next)();
            }
            return;
        }
        QtFileMgrTransferRequest *request = guardedSource->exportToLocalRequest(
            source, stagingPath, state->progress);
        if (!self) {
            qWarning("[FileView] relay: view destroyed while creating export request (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(stagingPath));
            return;
        }
        state->request = request;
        watchSerialTransfer(this, request, state->progress, QStringLiteral("relay-download"),
                source, stagingPath,
                [this, self, state, weakStartNext, guardedSource, guardedDest, source, name,
                 stagingPath](bool success, const QString &,
                              const QString &error, bool cancelled) {
            state->request = nullptr;
            if (!success) {
                if (!cancelled) {
                    state->failed++;
                    state->errors.append(QStringLiteral("%1: %2").arg(
                        source, error.isEmpty() ? QStringLiteral("下载源项目失败")
                                                : error));
                }
                state->cancelled = state->cancelled || cancelled;
                state->index++;
                if (const auto next = weakStartNext.lock())
                    (*next)();
                return;
            }
            // The queued completion leaves a turn for cancellation or owner
            // destruction. Do not start the second leg after losing its context.
            if (!state->progress || state->progress->wasCanceled()
                || !guardedSource || !guardedDest) {
                qWarning("[FileView] relay: skipped upload after download context was lost or cancelled (source=\"%s\", staging=\"%s\", progressAvailable=%d, sourceAvailable=%d, destinationAvailable=%d)",
                         qPrintable(source), qPrintable(stagingPath), !state->progress.isNull(),
                         !guardedSource.isNull(), !guardedDest.isNull());
                state->cancelled = true;
                if (!guardedSource || !guardedDest) {
                    state->failed++;
                    state->errors.append(QStringLiteral("%1: 传输后端已断开").arg(source));
                }
                state->index++;
                if (const auto next = weakStartNext.lock())
                    (*next)();
                return;
            }
            const QString destination =
                joinedPath(state->destinationDirectory, name);
            // 暂存是本机路径,必须走显式上传;普通 copyRequest 在 SSH 后端
            // 已定义为远端到远端,会被当成目标主机上的路径 (R2-04)
            QtFileMgrTransferRequest *upload = guardedDest->importFromLocalRequest(
                stagingPath, destination, false, state->progress);
            if (!self) {
                qWarning("[FileView] relay: view destroyed while creating import request (source=\"%s\", destination=\"%s\")",
                         qPrintable(stagingPath), qPrintable(destination));
                return;
            }
            state->request = upload;
            watchSerialTransfer(this, upload, state->progress, QStringLiteral("relay-upload"),
                    stagingPath, destination,
                    [this, state, weakStartNext, source, stagingPath](
                        bool uploadSuccess, const QString &,
                        const QString &uploadError, bool uploadCancelled) {
                state->request = nullptr;
                // 清理暂存副本(文件或目录)
                const QFileInfo stagingInfo(stagingPath);
                bool stagingRemoved = stagingInfo.isDir()
                    ? QDir(stagingPath).removeRecursively()
                    : QFile::remove(stagingPath);
                if (!stagingRemoved && stagingInfo.exists()) {
                    qWarning("[FileView] relay: failed to clean staging copy (path=\"%s\")",
                             qPrintable(stagingPath));
                }
                if (uploadSuccess) {
                    state->succeeded++;
                    state->succeededSources.append(source);
                } else if (!uploadCancelled) {
                    state->failed++;
                    state->errors.append(QStringLiteral("%1: %2").arg(
                        source, uploadError.isEmpty()
                            ? QStringLiteral("上传到目标失败") : uploadError));
                }
                state->cancelled = state->cancelled || uploadCancelled;
                state->index++;
                if (const auto next = weakStartNext.lock())
                    (*next)();
            });
        });
    };
    (*startNext)();
}

// ─── 配置驱动方法 ──────────────────────────────────────────

void QtFileMgrFileView::addContextMenuAction(QAction *action)
{
    if (action && !m_extraContextActions.contains(action))
        m_extraContextActions.append(action);
}

void QtFileMgrFileView::setAlternatingRowColors(bool on)
{
    m_detailView->setAlternatingRowColors(on);
    m_delegate->setAlternatingRows(on);
    m_detailView->viewport()->update();
}

void QtFileMgrFileView::setDetailColumnWidths(const QList<int> &widths)
{
    if (widths.size() != m_flatModel->columnCount())
        return;
    QHeaderView *header = m_detailView->horizontalHeader();
    QSignalBlocker blocker(header);
    m_applyingColumnWidths = true;
    header->setStretchLastSection(false);
    for (int i = 0; i < widths.size(); ++i)
        header->resizeSection(i, qMax(header->minimumSectionSize(), widths.at(i)));
    header->setStretchLastSection(true);
    m_applyingColumnWidths = false;
    m_hasExplicitColumnWidths = true;
}

QList<int> QtFileMgrFileView::detailColumnWidths() const
{
    QList<int> widths;
    QHeaderView *header = m_detailView->horizontalHeader();
    for (int i = 0; i < m_flatModel->columnCount(); ++i)
        widths.append(header->sectionSize(i));
    return widths;
}

void QtFileMgrFileView::setShowThumbnails(bool show)
{
    m_flatModel->setShowThumbnails(show);
}

void QtFileMgrFileView::setShowVersions(bool show)
{
    m_flatModel->setShowVersions(show);
    m_hasExplicitColumnWidths = false;
    // 模型可能在隐藏版本列后回落排序列,指示需同步 (R2-FV-P2-1)
    updateSortIndicator();
    optimizeDetailColumns();
}

void QtFileMgrFileView::showVersionHistory(const QString &path)
{
    if (!m_io || path.isEmpty()
        || !m_io->supportsCapability(QtFileMgrIO::VersionsCapability)) {
        qWarning("[FileView] showVersionHistory: skipped unsupported request (backend=%s, path=\"%s\")",
                 m_io ? m_io->metaObject()->className() : "<null>",
                 qPrintable(path));
        return;
    }
    auto *dialog = new QtFileMgrVersionsDialog(m_io, path, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QtFileMgrVersionsDialog::versionRestored, this,
            [this](const QString &restoredPath, int version) {
        showMessage(QStringLiteral("已将 %1 恢复到版本 %2")
                        .arg(QFileInfo(restoredPath).fileName())
                        .arg(version),
                    QStringLiteral("info"));
        scheduleReload();
    });
    dialog->open();
}

// ─── IO 注入 ──────────────────────────────────────────────

void QtFileMgrFileView::setIO(QtFileMgrIO *io)
{
    if (m_undoInProgress && io) {
        qWarning("[FileView] setIO: refused backend switch during guarded undo (backend=%s)",
                 io->metaObject()->className());
        return;
    }
    if (io && io->thread() != thread()) {
        qWarning("[FileView] setIO: rejected backend thread mismatch (backend=%s, backendThread=%p, viewThread=%p)",
                 io->metaObject()->className(),
                 static_cast<void *>(io->thread()),
                 static_cast<void *>(thread()));
        showMessage(QStringLiteral("文件后端线程与界面线程不一致。"),
                    QStringLiteral("error"));
        return;
    }
    // nullptr = 回退到默认 LocalIO (先于相等判断: 后端销毁兜底路径里
    // m_io 已悬空, 必须先转换再比较);销毁兜底不适用忙检查
    const bool destructionFallback = (io == nullptr);
    if (!io) {
        auto *oldLocal = qobject_cast<QtFileMgrLocalIO *>(m_io);
        if (oldLocal) return;
        io = new QtFileMgrLocalIO(this);
    }
    if (io == m_io) return;
    if ((m_activeJob && m_activeJob->isRunning()) || !m_operationQueue.isEmpty()) {
        if (!destructionFallback) {
            showMessage(QStringLiteral("文件操作进行中，暂时不能切换文件后端。"),
                        QStringLiteral("warning"));
            return;
        }
        // 先终止任务与队列,让消费者脱离旧后端,
        // 否则 FlatModel 继续持悬空指针 (R2-07)
        qWarning("[FileView] setIO: forcing fallback to local backend while operations are pending (jobRunning=%d, queued=%d)",
                 (m_activeJob && m_activeJob->isRunning()) ? 1 : 0,
                 m_operationQueue.size());
        if (m_activeJob)
            m_activeJob->cancel();
        m_operationQueue.clear();
        emit operationQueueChanged(0);
    }

    const QUrl previousUrl = m_url;
    QUrl targetUrl = m_url;
    if (!io->acceptsUrl(targetUrl))
        targetUrl = io->urlForPath(io->rootPath());
    const QString flatPath = io->pathFromUrl(targetUrl);

    if (m_io)
        disconnect(m_io, nullptr, this, nullptr);
    m_io = io;
    registerRemoteBackend(m_io);
    connectBackendSignals();
    m_pendingSelection.clear();
    m_pendingSelectionUrl = QUrl();
    m_pendingSelectionTimer->stop();
    m_undoOperations.clear();
    emit undoAvailableChanged(false);
    m_url = targetUrl;
    m_loaded = false;
    m_flatModel->setIOAndUrl(m_io, flatPath);
    updateDirectoryWatcher();
    emit ioChanged(m_io);
    if (previousUrl != m_url)
        emit urlChanged(m_url);
}

QtFileMgrIO *QtFileMgrFileView::io() const { return m_io; }
