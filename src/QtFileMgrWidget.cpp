// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// 参考 DolphinViewContainer (dolphinviewcontainer.cpp)
// 完整工具栏 + 缩放 + 排序 + 状态栏 + JSON 配置驱动
#include "QtFileMgrWidget.h"
#include "ui_QtFileMgrWidget.h"
#include "QtFileMgrPlacesPanel.h"
#include "QtFileMgrBreadcrumbBar.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrFilterBar.h"
#include "QtFileMgrHamburgerMenu.h"
#include "QtFileMgrItemDelegate.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrSettingsDialog.h"
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrDebug.h"
#include "QtFileMgrPreviewPanel.h"
#include "QtFileMgrSearchDialog.h"
#include "QtFileMgrTrashDialog.h"
#include "QtFileMgrProtocolProvider.h"
#if QTFILEMGR_WITH_SSH
#include "QtFileMgrSSHIO.h"
#endif
#include "QtFileMgrStyle.h"

#include <QSplitter>
#include <QToolBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QToolButton>
#include <QAction>
#include <QActionGroup>
#include <QSlider>
#include <QLabel>
#include <QDir>
#include <QStyle>
#include <QIcon>
#include <QFileInfo>
#include <QStorageInfo>
#include <QMessageBox>
#include <QTimer>
#include <QPointer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QApplication>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QVariant>
#include <functional>
#include <memory>

namespace {

// Shared with Workspace: only explicitly adopted provider IO may change owner
// when a pane starts sharing it. An injected IO keeps the host's parent.
const char kBackendOwnerProperty[] = "_qtfilemgr_backendOwner";

bool ownsBackend(const QObject *owner, const QtFileMgrIO *backend)
{
    return owner && backend && backend->parent() == owner
        && backend->property(kBackendOwnerProperty).value<QObject *>() == owner;
}

// Status text may shrink in a split pane without imposing a wide minimum window.
class StatusLabel : public QLabel {
public:
    explicit StatusLabel(QWidget *parent = nullptr) : QLabel(parent) {}
    QSize minimumSizeHint() const override
    {
        return QSize(0, QLabel::minimumSizeHint().height());
    }
};

} // namespace

// ─── 构造函数 ────────────────────────────────────────────

QtFileMgrWidget::QtFileMgrWidget(QWidget *parent)
    : QtFileMgrWidget(QtFileMgrConfigStore::loadDefault(), parent) {}

QtFileMgrWidget::QtFileMgrWidget(const QtFileMgrConfig &cfg, QWidget *parent)
    : QtFileMgrWidget(cfg, nullptr, parent) {}

QtFileMgrWidget::QtFileMgrWidget(const QtFileMgrConfig &cfg, QtFileMgrIO *io, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::QtFileMgrWidget)
    , m_config(cfg)
{
    QTMGR_DBG("Widget constructed with config: viewMode=%d rootPath=%s",
              cfg.defaultViewMode, qPrintable(cfg.rootPath));
    setupUi();
#if QTFILEMGR_WITH_SSH
    setProtocolProvider(QStringLiteral("ssh"), new QtFileMgrSSHProvider(this));
#endif
    if (io)
        setIO(io);
    applyConfig(cfg);
    // Initialize only after applying the host's root, so a constrained host
    // does not briefly navigate to (and reject) the user's home directory.
    if (m_url.isEmpty()) {
        if (!cfg.rootPath.isEmpty()) {
            qWarning("[QtFileMgrWidget] initial navigation: configured root was not applied; using the view URL (root=\"%s\", viewUrl=\"%s\")",
                     qPrintable(cfg.rootPath),
                     qPrintable(m_fileView->url().toString(QUrl::RemovePassword)));
        }
        setUrl(m_fileView->url());
    }
    // PlacesPanel 构造与 setConfigPlaces 均已自行 buildModel,
    // 不再重复构建 (R2-WS-P2-11)
}

QtFileMgrWidget::QtFileMgrWidget(const QString &configPath, QWidget *parent)
    : QtFileMgrWidget(QtFileMgrConfig::loadFromFile(configPath), parent) {}

QtFileMgrWidget::~QtFileMgrWidget()
{
    // QWidget destroys child providers after this class's members have been
    // destroyed, before QObject disconnects receiver connections. Remove the
    // registration callbacks now, while their captured state is still alive.
    for (const auto &connection : m_providerDestroyedConnections)
        disconnect(connection);
    m_providerDestroyedConnections.clear();
    delete ui;
}

// ─── 主布局 ──────────────────────────────────────────────

void QtFileMgrWidget::setupUi()
{
    // .ui 定义静态骨架 (topBar/QSplitter/m_placesPanel/m_fileView/m_filterBar/m_statusBar)
    // C++ 填充动态内容 (导航按钮/汉堡/面包屑/设置/信号连接)
    ui->setupUi(this);
    setObjectName(QStringLiteral("qtFileMgrRoot"));

    // 快捷指针
    m_splitter          = ui->m_splitter;
    m_fileViewSplitter  = ui->m_fileViewSplitter;
    m_fileViewSidePanel = ui->m_fileViewSidePanel;
    m_placesPanel       = ui->m_placesPanel;
    m_fileView          = ui->m_fileView;
    m_filterBar         = ui->m_filterBar;
    m_statusBar         = ui->m_statusBar;
    ui->topBar->setObjectName(QStringLiteral("qtFileMgrTopBar"));
    m_placesPanel->setObjectName(QStringLiteral("qtFileMgrPlacesPanel"));
    m_fileView->setObjectName(QStringLiteral("qtFileMgrFileView"));

    m_previewPanel = new QtFileMgrPreviewPanel(m_fileViewSidePanel);
    setSidePanel(m_previewPanel);
    m_fileViewSidePanel->hide();

    // === 顶部工具栏: nav buttons + breadcrumb + hamburger/settings ===
    auto *topLayout = new QHBoxLayout(ui->topBar);
    topLayout->setContentsMargins(10, 5, 10, 5);
    topLayout->setSpacing(4);

    auto mkBtn = [&](const QString &tip, QtFileMgrStyle::Symbol icon) {
        auto *b = new QToolButton(ui->topBar);
        b->setIcon(QtFileMgrStyle::toolIcon(icon, palette()));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        topLayout->addWidget(b);
        return b;
    };
    m_backBtn = mkBtn(QStringLiteral("后退"), QtFileMgrStyle::Symbol::Back);
    m_forwardBtn = mkBtn(QStringLiteral("前进"), QtFileMgrStyle::Symbol::Forward);
    m_upBtn = mkBtn(QStringLiteral("上级"), QtFileMgrStyle::Symbol::Up);

    auto *navSep = new QFrame(ui->topBar);
    navSep->setObjectName(QStringLiteral("qtFileMgrToolbarSeparator"));
    navSep->setFixedSize(1, 18);
    topLayout->addWidget(navSep);

    m_breadcrumb = new QtFileMgrBreadcrumbBar(ui->topBar);
    m_breadcrumb->setMinimumWidth(0);
    m_breadcrumb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_breadcrumb->setNavButtonsVisible(false);
    connect(m_breadcrumb, &QtFileMgrBreadcrumbBar::urlChanged,
            this, &QtFileMgrWidget::onBreadcrumbUrlChanged);
    connect(m_breadcrumb, &QtFileMgrBreadcrumbBar::backRequested,
            this, &QtFileMgrWidget::goBack);
    connect(m_breadcrumb, &QtFileMgrBreadcrumbBar::forwardRequested,
            this, &QtFileMgrWidget::goForward);
    connect(m_breadcrumb, &QtFileMgrBreadcrumbBar::statusMessage,
            this, [this](const QString &msg, const QString &type) {
        if (m_statusLabel)
            m_statusLabel->setText(QStringLiteral(" %1").arg(msg));
        m_fileView->showMessage(msg, type);
    });
    connect(m_breadcrumb, &QtFileMgrBreadcrumbBar::focusFileViewRequested,
            m_fileView, &QtFileMgrFileView::focusCurrentView);
    topLayout->addWidget(m_breadcrumb, 1);

    auto *toolSep = new QFrame(ui->topBar);
    toolSep->setObjectName(QStringLiteral("qtFileMgrToolbarSeparator"));
    toolSep->setFixedSize(1, 18);
    topLayout->addWidget(toolSep);

    m_hamburgerMenu = new QtFileMgrHamburgerMenu(ui->topBar);
    m_hamburgerMenu->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Menu, palette()));
    m_hamburgerMenu->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(m_hamburgerMenu, &QtFileMgrHamburgerMenu::viewModeChanged,
            this, [this](int m) { m==0?showIconView():m==1?showCompactView():showDetailsView(); });
    connect(m_hamburgerMenu, &QtFileMgrHamburgerMenu::showHiddenFilesToggled,
            this, &QtFileMgrWidget::toggleHiddenFiles);
    connect(m_hamburgerMenu, &QtFileMgrHamburgerMenu::foldersFirstToggled,
            this, &QtFileMgrWidget::toggleFoldersFirst);
    QAction *previewAction =
        m_hamburgerMenu->addExtensionAction(QStringLiteral("预览面板"));
    previewAction->setCheckable(true);
    connect(previewAction, &QAction::toggled, this, [this](bool visible) {
        const QPointer<QtFileMgrWidget> self(this);
        showSidePanel(visible);
        if (!self) {
            qWarning("[QtFileMgrWidget] preview toggle: widget destroyed while changing side panel visibility (visible=%d)",
                     visible ? 1 : 0);
            return;
        }
        if (!visible) {
            if (self->m_previewPanel) {
                self->m_previewPanel->clear();
            } else {
                const QLayout *layout = self->m_fileViewSidePanel
                    ? self->m_fileViewSidePanel->layout() : nullptr;
                const QWidget *panel = layout && layout->count() > 0
                    ? layout->itemAt(0)->widget() : nullptr;
                qWarning("[QtFileMgrWidget] preview toggle: skipped built-in preview clear for custom side panel (visible=0, sidePanel=%s @%p)",
                         panel ? panel->metaObject()->className() : "<none>",
                         static_cast<const void *>(panel));
            }
        }
    });
    QAction *searchAction =
        m_hamburgerMenu->addExtensionAction(QStringLiteral("搜索..."));
    searchAction->setShortcut(QKeySequence::Find);
    searchAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(searchAction);
    connect(searchAction, &QAction::triggered,
            this, &QtFileMgrWidget::showSearchDialog);
    QAction *trashAction =
        m_hamburgerMenu->addExtensionAction(QStringLiteral("管理回收站..."));
    connect(trashAction, &QAction::triggered,
            this, &QtFileMgrWidget::showTrashDialog);
    topLayout->addWidget(m_hamburgerMenu);

    m_settingsBtn = new QToolButton(ui->topBar);
    m_settingsBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Settings, palette()));
    m_settingsBtn->setAutoRaise(true);
    m_settingsBtn->setToolTip(QStringLiteral("设置"));
    connect(m_settingsBtn, &QToolButton::clicked, this, [this]() {
        QtFileMgrSettingsDialog dlg(m_config, this);
        connect(&dlg, &QtFileMgrSettingsDialog::settingsApplied, this, [this, &dlg]() {
            m_config = dlg.resultConfig();
            applyConfig(m_config);
            // applyConfig→setConfigPlaces 已触发面板重建,不再重复 (R2-WS-P2-11)

            QString error;
            if (!QtFileMgrConfigStore::saveDefault(m_config, &error)) {
                m_fileView->showMessage(
                    QStringLiteral("设置已应用，但未能持久化：%1").arg(error),
                    QStringLiteral("warning"));
            }
        });
        dlg.exec();
    });
    topLayout->addWidget(m_settingsBtn);

    // === 信号连接 ===
    connect(m_backBtn, &QToolButton::clicked, this, &QtFileMgrWidget::goBack);
    connect(m_forwardBtn, &QToolButton::clicked, this, &QtFileMgrWidget::goForward);
    connect(m_upBtn, &QToolButton::clicked, this, &QtFileMgrWidget::goUp);

    connect(m_placesPanel, &QtFileMgrPlacesPanel::placeActivated,
            this, &QtFileMgrWidget::onPlaceActivated);
    connect(m_placesPanel, &QtFileMgrPlacesPanel::trashRequested,
            this, &QtFileMgrWidget::showTrashDialog);
    connect(m_placesPanel, &QtFileMgrPlacesPanel::errorMessage,
            this, [this](const QString &message) {
        m_fileView->showMessage(message, QStringLiteral("warning"));
        if (m_statusLabel)
            m_statusLabel->setText(message);
    });

    m_fileView->setConfig(&m_config);
    m_io = m_fileView->io();
    m_breadcrumb->setIO(m_io);
    m_previewPanel->setIO(m_io);
    connect(m_fileView, &QtFileMgrFileView::ioChanged,
            this, &QtFileMgrWidget::synchronizeIO);
    connect(m_fileView, &QtFileMgrFileView::urlChanged,
            this, &QtFileMgrWidget::onFileViewUrlChanged);
    connect(m_fileView, &QtFileMgrFileView::fileActivated,
            this, &QtFileMgrWidget::fileActivated);
    connect(m_fileView, &QtFileMgrFileView::filesSelected,
            this, &QtFileMgrWidget::filesSelected);
    connect(m_fileView, &QtFileMgrFileView::filesSelected,
            m_previewPanel, &QtFileMgrPreviewPanel::setFiles);
    connect(m_fileView, &QtFileMgrFileView::statusMessage,
            this, [this](const QString &msg) {
        if (m_statusLabel)
            m_statusLabel->setText(msg);
    });
    connect(m_fileView, &QtFileMgrFileView::zoomLevelChanged, this,
            [this](int l, int) { if (m_zoomSlider) m_zoomSlider->setValue(l); });
    connect(m_fileView, &QtFileMgrFileView::showHiddenFilesChanged, this, [this](bool show) {
        m_config.view.showHiddenFiles = show;
        if (m_hamburgerMenu) m_hamburgerMenu->setShowHiddenFiles(show);
        if (m_hiddenFilesAction) m_hiddenFilesAction->setChecked(show);
    });
    connect(m_fileView, &QtFileMgrFileView::foldersFirstChanged, this, [this](bool on) {
        m_config.view.foldersFirst = on;
        if (m_hamburgerMenu) m_hamburgerMenu->setFoldersFirst(on);
        if (m_foldersFirstAction) m_foldersFirstAction->setChecked(on);
    });
    connect(m_fileView, &QtFileMgrFileView::sortChanged, this, [this](int column, Qt::SortOrder order) {
        m_config.view.sortColumn = column;
        m_config.view.sortOrder = order;
    });
    connect(m_fileView, &QtFileMgrFileView::detailColumnWidthsChanged,
            this, [this](const QList<int> &widths) {
        m_config.view.detailColumnWidths = widths;
        QtFileMgrConfigStore::saveDetailColumnWidths(widths);
    });
    connect(m_fileView, &QtFileMgrFileView::viewModeChanged, this, [this](int mode) {
        if (m_config.rememberMode)
            QtFileMgrConfigStore::saveDirectoryViewMode(m_url, mode);
        else
            m_config.defaultViewMode = mode;
        if (m_hamburgerMenu)
            m_hamburgerMenu->setViewMode(mode);
        if (m_iconViewAction)
            m_iconViewAction->setChecked(mode == QtFileMgrFileView::IconsView);
        if (m_compactViewAction)
            m_compactViewAction->setChecked(mode == QtFileMgrFileView::CompactView);
        if (m_detailViewAction)
            m_detailViewAction->setChecked(mode == QtFileMgrFileView::DetailsView);
    });
    connect(m_fileView, &QtFileMgrFileView::requestBreadcrumbEdit,
            this, &QtFileMgrWidget::toggleBreadcrumbEdit);
    connect(m_fileView, &QtFileMgrFileView::requestFilterToggle,
            this, [this]() {
        m_filterBar->setVisible(true);
        m_filterBar->setFocus();
    });

    auto applyFilter = [this]() {
        const QString t = m_filterBar->text();
        m_fileView->applyFilter(m_filterBar->filterMode(),
                                m_filterBar->isCaseSensitive(), t);
        if (!t.isEmpty() && m_statusLabel) {
            m_statusLabel->setText(QStringLiteral(" 过滤: \"%1\" | 命中 %2 项")
                                   .arg(t).arg(m_fileView->itemCount()));
        }
    };
    connect(m_filterBar, &QtFileMgrFilterBar::filterChanged, this, applyFilter);
    connect(m_filterBar, &QtFileMgrFilterBar::filterOptionsChanged, this, applyFilter);
    connect(m_filterBar, &QtFileMgrFilterBar::closeRequest,
            this, [this]() {
        m_filterBar->clear();
        m_filterBar->hide();
        m_fileView->applyNameFilter(QString());
        m_fileView->focusCurrentView();
    });
    connect(m_filterBar, &QtFileMgrFilterBar::focusFileViewRequested,
            m_fileView, &QtFileMgrFileView::focusCurrentView);
    connect(m_filterBar, &QtFileMgrFilterBar::navigationKeyRequested,
            m_fileView, &QtFileMgrFileView::handleNavigationKey);

    auto addFileShortcut = [this](const QKeySequence &shortcut,
                                  const std::function<void()> &handler) {
        QAction *action = new QAction(this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, handler]() {
            if (isFileViewCommandContext())
                handler();
        });
        return action;
    };

    addFileShortcut(QKeySequence(QStringLiteral("F5")), [this]() { m_fileView->reload(); });
    addFileShortcut(QKeySequence(QStringLiteral("F2")), [this]() { m_fileView->renameCurrent(); });
    addFileShortcut(QKeySequence::Delete, [this]() { m_fileView->deleteSelected(); });
    addFileShortcut(QKeySequence(QStringLiteral("Shift+Delete")),
                    [this]() { m_fileView->deletePermanent(); });
    addFileShortcut(QKeySequence::Cut, [this]() { m_fileView->cutSelected(); });
    addFileShortcut(QKeySequence::Copy, [this]() { m_fileView->copySelected(); });
    addFileShortcut(QKeySequence::Paste, [this]() { m_fileView->pasteFiles(); });
    addFileShortcut(QKeySequence::SelectAll, [this]() { m_fileView->selectAll(); });
    addFileShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")),
                    [this]() { m_fileView->invertSelection(); });
    addFileShortcut(QKeySequence(QStringLiteral("Backspace")), [this]() { m_fileView->goUp(); });
    addFileShortcut(QKeySequence(QStringLiteral("Alt+Up")), [this]() { m_fileView->goUp(); });
    addFileShortcut(QKeySequence::Undo, [this]() { m_fileView->undoLastOperation(); });

    // === 状态栏 ===
    m_statusBar->setObjectName(QStringLiteral("qtFileMgrStatusBar"));
    m_statusBar->setFixedHeight(qMax(30, fontMetrics().height() + 10));
    m_statusBar->setSizeGripEnabled(false);
    QFont statusFont = font();
    if (statusFont.pixelSize() > 0)
        statusFont.setPixelSize(qMax(10, statusFont.pixelSize() - 1));
    else
        statusFont.setPointSizeF(qMax(8.0, statusFont.pointSizeF() - 1.0));
    m_statusBar->setFont(statusFont);

    m_zoomSlider = new QSlider(Qt::Horizontal, m_statusBar);
    m_zoomSlider->setRange(0, m_fileView->zoomLevelCount() - 1);
    m_zoomSlider->setValue(m_fileView->zoomLevel());
    m_zoomSlider->setFixedWidth(88);
    m_zoomSlider->setObjectName(QStringLiteral("qtFileMgrZoomSlider"));
    m_zoomSlider->setToolTip(QStringLiteral("缩放"));
    connect(m_zoomSlider, &QSlider::valueChanged, this, &QtFileMgrWidget::onZoomChanged);

    m_zoomLabel = new QLabel(QStringLiteral("缩放"), m_statusBar);
    m_statusBar->addPermanentWidget(m_zoomLabel);
    m_statusBar->addPermanentWidget(m_zoomSlider);

    m_diskSpaceLabel = new StatusLabel(m_statusBar);
    m_diskSpaceLabel->setMinimumWidth(0);
    m_diskSpaceLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_diskSpaceLabel->setContentsMargins(12, 0, 4, 0);
    m_statusBar->addPermanentWidget(m_diskSpaceLabel);

    m_statusLabel = new StatusLabel(m_statusBar);
    m_statusLabel->setText(QStringLiteral("就绪"));
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_statusLabel->setContentsMargins(8, 0, 0, 0);
    m_statusBar->addWidget(m_statusLabel, 1);

    const auto applyAppearance = [this](const QPalette &themePalette) {
        const auto c = QtFileMgrStyle::colors(themePalette);
        setStyleSheet(QStringLiteral(
        "#qtFileMgrRoot{background:%1;}"
        "#qtFileMgrTopBar{background:%2;border-bottom:1px solid %3;}"
        "#qtFileMgrToolbarSeparator{background:%3;border:0;margin:0 3px;}"
        "QToolButton{border:0;padding:5px;border-radius:5px;}"
        "QToolButton:hover{background:%4;}"
        "QToolButton:pressed,QToolButton:checked{background:%5;color:%6;}"
        "QToolButton:focus{outline:none;border:1px solid %6;padding:4px;}"
        "QToolButton::menu-indicator{image:none;width:0;}"
        "QSplitter::handle{background:%3;}"
        "QSplitter::handle:hover{background:%6;}"
        "#qtFileMgrStatusBar{border-top:1px solid %3;background:%2;color:%7;}"
        "#qtFileMgrStatusBar::item{border:0;}"
        "#qtFileMgrStatusBar QLabel{background:transparent;color:%7;}"
        "#qtFileMgrZoomSlider::groove:horizontal{height:3px;background:%3;border-radius:1px;}"
        "#qtFileMgrZoomSlider::sub-page:horizontal{background:%6;border-radius:1px;}"
        "#qtFileMgrZoomSlider::handle:horizontal{width:10px;margin:-4px 0;"
        "background:%1;border:1px solid %6;border-radius:5px;}"
        "QMenu{background:%1;color:%8;border:1px solid %3;padding:5px;}"
        "QMenu::item{padding:6px 24px 6px 12px;border-radius:4px;}"
        "QMenu::item:selected{background:%5;color:%8;}"
        "QMenu::separator{height:1px;background:%3;margin:5px 8px;}"
        ).arg(c.base.name(), c.surface.name(), c.border.name(), c.hover.name(),
              c.selection.name(), c.accent.name(), c.muted.name(), c.text.name()));
        m_backBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Back, themePalette));
        m_forwardBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Forward, themePalette));
        m_upBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Up, themePalette));
        m_hamburgerMenu->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Menu, themePalette));
        m_settingsBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Settings, themePalette));
    };
    applyAppearance(palette());
    QtFileMgrStyle::observeApplicationPalette(this, applyAppearance);

    // 初始比例 — 延迟到布局完成后, 按配置值
    QTimer::singleShot(0, this, [this]() {
        int total = m_splitter->width();
        int hw = m_splitter->handleWidth();
        if (total > 400)
            m_splitter->setSizes({m_config.placesPanel.width, total - m_config.placesPanel.width - hw});
    });

}

// ─── URL ─────────────────────────────────────────────────

QUrl QtFileMgrWidget::url() const { return m_url; }

QtFileMgrIO *QtFileMgrWidget::io() const { return m_io; }

void QtFileMgrWidget::setIO(QtFileMgrIO *io)
{
    invalidatePendingConnection("backend changed");
    m_fileView->setIO(io);
    if (m_fileView->io() != m_io)
        synchronizeIO(m_fileView->io());
}

void QtFileMgrWidget::setProtocolProvider(const QString &scheme,
                                           QtFileMgrProtocolProvider *provider)
{
    const QString key = scheme.toLower();
    if (key.isEmpty() || key == QStringLiteral("file") ||
        (provider && provider->thread() != thread())) {
        qWarning("[QtFileMgrWidget] provider: rejected registration (scheme=\"%s\", provider=%p)",
                 qPrintable(key), static_cast<void *>(provider));
        return;
    }
    invalidatePendingConnection("provider registration changed");
    disconnect(m_providerDestroyedConnections.take(key));
    if (provider) {
        m_protocolProviders.insert(key, provider);
        m_providerDestroyedConnections.insert(key,
            connect(provider, &QObject::destroyed, this, [this, key]() {
                // The old registration's connection is removed on replacement,
                // so destroying an old provider cannot erase its successor.
                m_protocolProviders.remove(key);
                m_providerDestroyedConnections.remove(key);
                qWarning("[QtFileMgrWidget] provider: registration destroyed (scheme=\"%s\", pendingUrl=\"%s\", generation=%llu)",
                         qPrintable(key),
                         qPrintable(m_pendingConnectUrl.toString(QUrl::RemovePassword)),
                         static_cast<unsigned long long>(m_pendingConnectGeneration));
                if (m_pendingConnectGeneration != 0
                    && m_pendingConnectUrl.scheme().toLower() == key)
                    invalidatePendingConnection("provider destroyed");
            }));
    } else {
        m_protocolProviders.remove(key);
    }
}

void QtFileMgrWidget::clearPendingConnection()
{
    m_pendingConnectGeneration = 0;
    m_pendingConnectProvider.clear();
    m_pendingConnectUrl.clear();
    m_pendingConnectAuthority.clear();
    m_deferredNavigationUrl.clear();
}

void QtFileMgrWidget::invalidatePendingConnection(const char *reason)
{
    if (m_pendingConnectGeneration != 0) {
        qWarning("[QtFileMgrWidget] connect: invalidated in-flight connection (reason=\"%s\", scheme=\"%s\", url=\"%s\", generation=%llu)",
                 reason, qPrintable(m_pendingConnectUrl.scheme()),
                 qPrintable(m_pendingConnectUrl.toString(QUrl::RemovePassword)),
                 static_cast<unsigned long long>(m_pendingConnectGeneration));
    }
    ++m_connectionGeneration;
    clearPendingConnection();
}

bool QtFileMgrWidget::canGoBack() const
{
    return m_historyIndex > 0;
}

bool QtFileMgrWidget::canGoForward() const
{
    return m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size();
}

static QUrl normalizedNavigationUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme.isEmpty()) {
        const QString path = url.path().isEmpty() ? url.toString() : url.path();
        return QUrl::fromLocalFile(QDir(path).absolutePath());
    }
    if (scheme == QStringLiteral("file"))
        return QUrl::fromLocalFile(QDir(url.toLocalFile()).absolutePath());
    if (scheme == QStringLiteral("remote")) {
        QString path = QDir::cleanPath(url.path());
        if (path == QStringLiteral(".") || path == QStringLiteral("/"))
            path.clear();
        while (path.startsWith(QLatin1Char('/')))
            path.remove(0, 1);
        QUrl normalized(QStringLiteral("remote:///"));
        if (!path.isEmpty())
            normalized.setPath(QStringLiteral("/") + path);
        return normalized;
    }
    if (scheme == QStringLiteral("ssh")) {
        QUrl normalized = url;
        QString path = QDir::cleanPath(url.path());
        if (path.isEmpty() || path == QStringLiteral("."))
            path = QStringLiteral("/");
        if (!path.startsWith(QLatin1Char('/')))
            path.prepend(QLatin1Char('/'));
        normalized.setPath(path);
        normalized.setPassword(QString());
        return normalized;
    }
    if (!url.password().isEmpty())
        qWarning("[QtFileMgrWidget] navigation: discarded embedded password (scheme=\"%s\", host=\"%s\")",
                 qPrintable(scheme), qPrintable(url.host()));
    return url.adjusted(QUrl::RemovePassword);
}

bool QtFileMgrWidget::isUrlCompatibleWithIO(const QUrl &url, QtFileMgrIO *io) const
{
    if (!io || !url.isValid())
        return false;
    return io->acceptsUrl(url);
}

QUrl QtFileMgrWidget::backendRootUrl(QtFileMgrIO *io) const
{
    if (!io)
        return {};
    return io->urlForPath(io->rootPath());
}

void QtFileMgrWidget::synchronizeIO(QtFileMgrIO *io)
{
    if (!io)
        return;
    m_io = io;
    m_breadcrumb->setIO(io);
    if (m_previewPanel)
        m_previewPanel->setIO(io);

    if (!m_url.isEmpty() && isUrlCompatibleWithIO(m_url, io))
        return;

    m_history.clear();
    m_historyIndex = -1;
    navigateToUrl(backendRootUrl(io), true);
}

static QString normalizedLocalAccessPath(QString path)
{
    if (path.startsWith(QStringLiteral("~/")))
        path = QDir::homePath() + path.mid(1);
    else if (path == QStringLiteral("~"))
        path = QDir::homePath();

    path = QDir(path).absolutePath();
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    if (!canonicalPath.isEmpty())
        path = canonicalPath;
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}

static QString normalizedRemoteAccessPath(QString path)
{
    const QUrl url(path);
    if (!url.scheme().isEmpty() && !url.isLocalFile()) {
        path = url.path();
    }
    path = QDir::cleanPath(path);
    if (path == QStringLiteral(".") || path == QStringLiteral("/"))
        path.clear();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path;
}

bool QtFileMgrWidget::isUrlAllowed(const QUrl &url) const
{
    if (m_config.allowedPaths.isEmpty())
        return true;

    const bool remote = !url.scheme().isEmpty() && !url.isLocalFile();
    const QString path = remote
        ? normalizedRemoteAccessPath(url.path())
        : normalizedLocalAccessPath(url.toLocalFile());

    for (const QString &allowed : m_config.allowedPaths) {
        const QUrl allowedUrl(allowed);
        const bool explicitlyRemote = !allowedUrl.scheme().isEmpty()
            && !allowedUrl.isLocalFile();
        if (!remote && explicitlyRemote)
            continue;
        if (remote && explicitlyRemote) {
            const int defaultPort = url.scheme() == QStringLiteral("ssh") ? 22 : -1;
            if (allowedUrl.scheme().compare(url.scheme(), Qt::CaseInsensitive) != 0
                || allowedUrl.host().compare(url.host(), Qt::CaseInsensitive) != 0
                || allowedUrl.port(defaultPort) != url.port(defaultPort)
                || allowedUrl.userName() != url.userName())
                continue;
        } else if (remote && url.scheme() != QStringLiteral("remote")) {
            // Legacy remote:// permits raw backend paths. Other protocols must
            // declare their identity, so local allowlists cannot authorize them.
            continue;
        }
        const QString root = remote
            ? normalizedRemoteAccessPath(allowed)
            : normalizedLocalAccessPath(allowed);
        if (root.isEmpty() && !remote)
            continue;
        if (remote && root.isEmpty())
            return true;
        // 根目录 "/" + '/' 会拼成 "//" 永不匹配,单独放行 (审查#19)
        if (root == QLatin1String("/"))
            return true;
        if (path == root || path.startsWith(root + QLatin1Char('/')))
            return true;
    }
    return false;
}

void QtFileMgrWidget::showPathRejected(const QUrl &url)
{
    const QString path = (!url.scheme().isEmpty() && !url.isLocalFile())
        ? url.toString(QUrl::PreferLocalFile)
        : url.toLocalFile();
    QStringList allowedForLog;
    for (const QString &allowed : m_config.allowedPaths) {
        const QUrl allowedUrl(allowed);
        allowedForLog.append(allowedUrl.scheme().isEmpty() ? allowed
                            : allowedUrl.toString(QUrl::RemovePassword));
    }
    qWarning("[QtFileMgrWidget] navigation: rejected URL outside allowedPaths (url=\"%s\", allowed=%s)",
             qPrintable(url.toString(QUrl::RemovePassword)),
             qPrintable(allowedForLog.join(QStringLiteral(", "))));
    m_fileView->showMessage(QStringLiteral("路径不在允许访问范围内: %1").arg(path),
                            QStringLiteral("warning"));
}

bool QtFileMgrWidget::isFileViewCommandContext() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || (focus != m_fileView && !m_fileView->isAncestorOf(focus)))
        return false;
    return !qobject_cast<QLineEdit *>(focus)
        && !qobject_cast<QAbstractSpinBox *>(focus)
        && !qobject_cast<QComboBox *>(focus);
}

void QtFileMgrWidget::setUrl(const QUrl &url)
{
    navigateToUrl(url, true);
}

bool QtFileMgrWidget::navigateToUrl(const QUrl &requestedUrl,
                                    bool addToHistory,
                                    bool updateFileView)
{
    if (m_updatingUrl)
        return false;

    const QUrl url = normalizedNavigationUrl(requestedUrl);
    // 导航不再递增连接代次: 在途连接的完成属于用户已发起的操作,
    // 本地浏览/历史导航不得将其判过期丢弃 (审查#12);代次只在
    // connectToProtocol 新连接/复用时递增
    if (!isUrlCompatibleWithIO(url, m_io)) {
        const auto provider = m_protocolProviders.value(url.scheme().toLower());
        if (provider) {
            QTimer::singleShot(0, this, [this, url]() {
                connectToProtocol(url);
            });
            return false;
        }
        const QString scheme = url.scheme().isEmpty()
            ? QStringLiteral("file") : url.scheme();
        m_fileView->showMessage(
            QStringLiteral("路径协议 %1 与当前文件后端不匹配。").arg(scheme),
            QStringLiteral("warning"));
        return false;
    }
    if (!isUrlAllowed(url)) {
        showPathRejected(url);
        return false;
    }

    const QUrl previousUrl = m_url;
    m_updatingUrl = true;
    if (updateFileView)
        m_fileView->setUrl(url);
    const bool accepted = !updateFileView || m_fileView->url() == url;
    if (accepted) {
        m_url = url;
        m_breadcrumb->setUrl(url);
        if (m_config.rememberMode) {
            const int mode = QtFileMgrConfigStore::loadDirectoryViewMode(
                url, m_config.defaultViewMode);
            m_fileView->setViewMode(
                static_cast<QtFileMgrFileView::ViewMode>(mode));
        }
    }
    m_updatingUrl = false;
    if (!accepted)
        return false;

    if (addToHistory)
        addUrlToHistory(url);

    QString path = (m_io && m_io->isRemote())
        ? url.toString(QUrl::PreferLocalFile) : url.toLocalFile();
    PlaceMatch match = activePlacesPanel()->selectPlaceForPath(path);
    if (match.found)
        m_breadcrumb->setPlaceInfo(match.name, match.rootPath);
    else
        m_breadcrumb->setPlaceInfo(QString(), QString());

    updateStatusBar();
    updateDiskSpace();
    updateNavigationState();
    if (previousUrl != m_url)
        emit urlChanged(m_url);
    return true;
}

void QtFileMgrWidget::addUrlToHistory(const QUrl &url)
{
    if (m_historyIndex >= 0 && m_historyIndex < m_history.size()
        && m_history.at(m_historyIndex) == url) {
        return;
    }
    while (m_history.size() - 1 > m_historyIndex)
        m_history.removeLast();
    m_history.append(url);
    m_historyIndex = m_history.size() - 1;
}

void QtFileMgrWidget::updateNavigationState()
{
    const bool back = canGoBack();
    const bool forward = canGoForward();
    if (m_backBtn)
        m_backBtn->setEnabled(back);
    if (m_forwardBtn)
        m_forwardBtn->setEnabled(forward);
    if (m_breadcrumb)
        m_breadcrumb->setNavigationState(back, forward);
}

void QtFileMgrWidget::goBack()
{
    if (!canGoBack())
        return;
    const int targetIndex = m_historyIndex - 1;
    if (navigateToUrl(m_history.at(targetIndex), false)) {
        m_historyIndex = targetIndex;
        updateNavigationState();
    }
}

void QtFileMgrWidget::goForward()
{
    if (!canGoForward())
        return;
    const int targetIndex = m_historyIndex + 1;
    if (navigateToUrl(m_history.at(targetIndex), false)) {
        m_historyIndex = targetIndex;
        updateNavigationState();
    }
}

void QtFileMgrWidget::goUp()
{
    if (m_io && m_io->isRemote() && m_io->acceptsUrl(m_url)) {
        QString path = QDir::cleanPath(m_io->pathFromUrl(m_url));
        if (path.isEmpty() || path == QStringLiteral(".")
            || path == QStringLiteral("/"))
            return;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parent = slash > 0 ? path.left(slash)
            : (path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString());
        setUrl(m_io->urlForPath(parent));
        return;
    }
    if (m_url.scheme() == QStringLiteral("remote")) {
        QString path = m_url.path();
        while (path.startsWith(QLatin1Char('/')))
            path.remove(0, 1);
        if (path.isEmpty())
            return;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        QUrl parent(QStringLiteral("remote:///"));
        if (slash > 0)
            parent.setPath(QStringLiteral("/") + path.left(slash));
        setUrl(parent);
        return;
    }
    if (m_url.scheme() == QStringLiteral("ssh")) {
        QString path = m_url.path();
        while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
            path.chop(1);
        if (path == QStringLiteral("/"))
            return;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        QUrl parent = m_url;
        parent.setPath(slash > 0 ? path.left(slash) : QStringLiteral("/"));
        setUrl(parent);
        return;
    }
    const QString localPath = m_url.toLocalFile();
    if (localPath.isEmpty()) {
        qWarning("[QtFileMgrWidget] goUp: skipped non-local URL (url=\"%s\")",
                 qPrintable(m_url.toString(QUrl::RemovePassword)));
        return;
    }
    QDir directory(localPath);
    if (directory.cdUp())
        setUrl(QUrl::fromLocalFile(directory.absolutePath()));
}

QtFileMgrFileView *QtFileMgrWidget::fileView() const { return m_fileView; }
QtFileMgrPlacesPanel *QtFileMgrWidget::placesPanel() const { return m_placesPanel; }

void QtFileMgrWidget::setSharedPlacesPanel(QtFileMgrPlacesPanel *panel)
{
    m_sharedPlacesPanel = panel;
    // 共享模式: 内嵌面板所在的 leftPane 包装整体隐藏——只藏 m_placesPanel
    // 会让 splitter 留下一个可拖动的空白区域
    if (ui && ui->leftPane)
        ui->leftPane->setVisible(panel == nullptr);
    else
        m_placesPanel->setVisible(panel == nullptr);
}

QtFileMgrPlacesPanel *QtFileMgrWidget::activePlacesPanel() const
{
    return m_sharedPlacesPanel ? m_sharedPlacesPanel.data() : m_placesPanel;
}

void QtFileMgrWidget::setSidePanel(QWidget *panel)
{
    if (!m_fileViewSidePanel || !panel) return;
    // 清除旧内容
    if (m_fileViewSidePanel->layout()) {
        QLayoutItem *child;
        while ((child = m_fileViewSidePanel->layout()->takeAt(0)) != nullptr) {
            QWidget *oldWidget = child->widget();
            if (oldWidget && oldWidget != panel)
                oldWidget->deleteLater();
            delete child;
        }
        delete m_fileViewSidePanel->layout();
    }
    // 预览面板被替换/下架时同步引用;QPointer 在延迟删除后自动置空 (审查#10)
    if (m_previewPanel && m_previewPanel != panel)
        m_previewPanel = nullptr;
    if (auto *preview = qobject_cast<QtFileMgrPreviewPanel *>(panel))
        m_previewPanel = preview;
    m_fileViewSidePanel->setLayout(new QVBoxLayout(m_fileViewSidePanel));
    m_fileViewSidePanel->layout()->setContentsMargins(0, 0, 0, 0);
    m_fileViewSidePanel->layout()->addWidget(panel);
}

void QtFileMgrWidget::showSidePanel(bool show)
{
    if (!m_fileViewSidePanel || !m_fileViewSplitter) return;
    const QPointer<QtFileMgrWidget> self(this);
    const QPointer<QSplitter> splitter(m_fileViewSplitter);
    m_fileViewSidePanel->setVisible(show);
    if (!self || !splitter) {
        qWarning("[QtFileMgrWidget] side panel: widget or splitter destroyed while changing visibility (visible=%d)",
                 show ? 1 : 0);
        return;
    }
    if (show) {
        int total = splitter->width();
        splitter->setSizes({total * 2 / 3, total / 3});
    }
}

bool QtFileMgrWidget::isSidePanelVisible() const
{
    return m_fileViewSidePanel && m_fileViewSidePanel->isVisible();
}

// ─── Slots ───────────────────────────────────────────────

void QtFileMgrWidget::onPlaceActivated(const QString &path)
{
    const QUrl placeUrl(path);
    if (m_protocolProviders.contains(placeUrl.scheme().toLower()) &&
        m_protocolProviders.value(placeUrl.scheme().toLower())) {
        const QUrl target = normalizedNavigationUrl(placeUrl);
        if (!isUrlAllowed(target)) {
            showPathRejected(target);
            return;
        }
        if (m_io && m_io->acceptsUrl(target)) {
#if QTFILEMGR_WITH_SSH
            auto *ssh = qobject_cast<QtFileMgrSSHIO *>(m_io);
            if (!ssh || ssh->isConnected()) {
#endif
                setUrl(target);
                m_filterBar->clearIfUnlocked();
                return;
#if QTFILEMGR_WITH_SSH
            }
#endif
        }
        connectToProtocol(target);
        return;
    }

    const QUrl url = !placeUrl.scheme().isEmpty()
        ? placeUrl : QUrl::fromLocalFile(path);
    if (!isUrlAllowed(url)) {
        showPathRejected(url);
        return;
    }

    if (m_io && m_io->isRemote() && url.isLocalFile()) {
        QPointer<QtFileMgrIO> oldRemote = m_io;
        setIO(nullptr);
        if (m_io == oldRemote) {
            qWarning("[QtFileMgrWidget] onPlaceActivated: backend switch rejected; retained SSH backend (place=\"%s\")",
                     qPrintable(path));
            return;
        }
        if (ownsBackend(this, oldRemote))
            oldRemote->deleteLater();
    }

    setUrl(url);
    m_filterBar->clearIfUnlocked();
}

void QtFileMgrWidget::connectToProtocol(const QUrl &requestedUrl)
{
    const auto provider = m_protocolProviders.value(requestedUrl.scheme().toLower());
    if (!provider) {
        qWarning("[QtFileMgrWidget] connect: provider unavailable (url=\"%s\")",
                 qPrintable(requestedUrl.toString(QUrl::RemovePassword)));
        return;
    }
    // 同主机已有已连接后端时直接复用,不再发起新连接/弹密码框;
    // 共享后端改由应用托管,避免拥有者销毁导致共享方悬空
    if (QtFileMgrIO *existing = QtFileMgrFileView::remoteBackendForUrl(requestedUrl)) {
        if (existing != m_io && existing->canWrite()) {
            // setIO invalidates any connection replaced by this shared backend.
            qWarning("[QtFileMgrWidget] connect: reusing connected backend (url=\"%s\", backend=%s @%p)",
                     qPrintable(requestedUrl.toString(QUrl::RemovePassword)),
                     existing->metaObject()->className(),
                     static_cast<void *>(existing));
            if (qobject_cast<QtFileMgrWidget *>(existing->parent())
                && ownsBackend(existing->parent(), existing)) {
                existing->setParent(qApp);
                existing->setProperty(kBackendOwnerProperty,
                                     QVariant::fromValue(static_cast<QObject *>(qApp)));
            }
            QPointer<QtFileMgrIO> oldBackend(m_io);
            setIO(existing);
            if (m_io != existing) {
                qWarning("[QtFileMgrWidget] connect: shared backend adoption rejected (url=\"%s\")",
                         qPrintable(requestedUrl.toString(QUrl::RemovePassword)));
                return;
            }
            navigateToUrl(requestedUrl, true);
            if (oldBackend != existing && ownsBackend(this, oldBackend))
                oldBackend->deleteLater();
            return;
        }
    }

    const QUrl connectKey = requestedUrl.adjusted(QUrl::RemovePassword);
    const QString connectAuthority =
        requestedUrl.scheme().toLower() == QStringLiteral("ssh")
            ? QStringLiteral("%1@%2:%3")
                  .arg(requestedUrl.userName(), requestedUrl.host().toLower())
                  .arg(requestedUrl.port(22))
            : QString();
    const bool currentPending = m_pendingConnectGeneration != 0
        && m_pendingConnectGeneration == m_connectionGeneration
        && m_pendingConnectProvider == provider;
    if (currentPending && m_pendingConnectUrl == connectKey) {
        // 启动自动重连与手动点击等并发触发只保留首个,避免双重密码框
        // 与 stale-completion 竞态
        qWarning("[QtFileMgrWidget] connect: skipped duplicate in-flight connection (scheme=\"%s\", url=\"%s\", generation=%llu)",
                 qPrintable(connectKey.scheme()), qPrintable(connectKey.toString()),
                 static_cast<unsigned long long>(m_pendingConnectGeneration));
        return;
    }
    if (currentPending && !connectAuthority.isEmpty()
        && m_pendingConnectAuthority == connectAuthority) {
        // 同主机不同路径: 归并到在途连接,连通后导航到最新目标
        qWarning("[QtFileMgrWidget] connect: coalesced into in-flight connection (scheme=\"%s\", url=\"%s\", authority=\"%s\", generation=%llu)",
                 qPrintable(connectKey.scheme()), qPrintable(connectKey.toString()),
                 qPrintable(connectAuthority),
                 static_cast<unsigned long long>(m_pendingConnectGeneration));
        m_deferredNavigationUrl = connectKey;
        return;
    }
    invalidatePendingConnection("superseded by another connection");
    m_pendingConnectUrl = connectKey;
    m_pendingConnectAuthority = connectAuthority;
    m_pendingConnectProvider = provider;
    const quint64 generation = m_connectionGeneration;
    m_pendingConnectGeneration = generation;
    m_fileView->showMessage(QStringLiteral("正在连接 %1...")
                            .arg(requestedUrl.toString(QUrl::RemovePassword)),
                            QStringLiteral("info"));
    QPointer<QtFileMgrWidget> self(this);
    QPointer<QtFileMgrProtocolProvider> providerGuard(provider);
    auto completed = std::make_shared<bool>(false);
    provider->connectUrl(requestedUrl, this,
        [self, providerGuard, generation, completed, connectKey](const QtFileMgrConnectionResult &result) {
        QPointer<QtFileMgrIO> backend(result.io);
        const QUrl safeUrl = result.url.adjusted(QUrl::RemovePassword);
        auto discard = [&]() {
            if (result.transferOwnership && backend) {
                if ((self && self->m_io == backend)
                    || backend->property(kBackendOwnerProperty).value<QObject *>()) {
                    qWarning("[QtFileMgrWidget] connect: retained an already adopted backend while discarding result (scheme=\"%s\", url=\"%s\", generation=%llu, backend=%p)",
                             qPrintable(connectKey.scheme()), qPrintable(safeUrl.toString()),
                             static_cast<unsigned long long>(generation),
                             static_cast<void *>(backend.data()));
                } else {
                    backend->deleteLater();
                }
            }
        };
        if (*completed) {
            qWarning("[QtFileMgrWidget] connect: skipped duplicate completion (scheme=\"%s\", url=\"%s\", generation=%llu)",
                     qPrintable(connectKey.scheme()), qPrintable(safeUrl.toString()),
                     static_cast<unsigned long long>(generation));
            // Ownership has already been processed for this connection.
            return;
        }
        *completed = true;
        if (!self || !providerGuard || generation != self->m_connectionGeneration
            || self->m_pendingConnectGeneration != generation
            || self->m_pendingConnectProvider != providerGuard
            || self->m_pendingConnectUrl != connectKey
            || self->m_protocolProviders.value(connectKey.scheme().toLower()) != providerGuard) {
            qWarning("[QtFileMgrWidget] connect: ignored stale completion (scheme=\"%s\", url=\"%s\", generation=%llu, currentGeneration=%llu)",
                     qPrintable(connectKey.scheme()), qPrintable(safeUrl.toString()),
                     static_cast<unsigned long long>(generation),
                     static_cast<unsigned long long>(self ? self->m_connectionGeneration : 0));
            discard();
            return;
        }
        const QUrl deferred = self->m_deferredNavigationUrl;
        self->clearPendingConnection();
        if (result.cancelled) {
            discard();
            return;
        }
        if (!backend || backend->thread() != self->thread() ||
            !backend->acceptsUrl(safeUrl) || !self->isUrlAllowed(safeUrl)) {
            qWarning("[QtFileMgrWidget] connect: rejected backend or URL (url=\"%s\", backend=%p)",
                     qPrintable(safeUrl.toString()), static_cast<void *>(backend.data()));
            self->m_fileView->showMessage(result.error.isEmpty()
                ? QStringLiteral("连接失败或路径不在允许范围内") : result.error,
                QStringLiteral("error"));
            discard();
            return;
        }
        QPointer<QtFileMgrIO> oldBackend(self->m_io);
        self->setIO(backend);
        if (self->m_io != backend) {
            qWarning("[QtFileMgrWidget] connect: backend switch rejected (url=\"%s\")",
                     qPrintable(safeUrl.toString()));
            discard();
            return;
        }
        if (result.transferOwnership) {
            if (backend->parent() != self)
                backend->setParent(self);
            backend->setProperty(kBackendOwnerProperty,
                                 QVariant::fromValue(static_cast<QObject *>(self.data())));
        }
        if (!self->navigateToUrl(safeUrl, true)) {
            qWarning("[QtFileMgrWidget] connect: rejected connected URL after backend switch (url=\"%s\")",
                     qPrintable(safeUrl.toString()));
            self->m_fileView->showMessage(QStringLiteral("连接成功，但无法打开目标目录"),
                                          QStringLiteral("warning"));
            return;
        }
        if (result.addToPlaces && !result.displayName.isEmpty())
            self->activePlacesPanel()->registerPersistentNetworkPlace(
                result.displayName, safeUrl.toString(QUrl::RemovePassword),
                QStringLiteral("folder-remote"));
        else if (result.addToPlaces)
            qWarning("[QtFileMgrWidget] connect: skipped place without name (url=\"%s\")",
                     qPrintable(safeUrl.toString()));
        if (oldBackend != backend && ownsBackend(self, oldBackend))
            oldBackend->deleteLater();
        // 归并期间用户点了同主机的其他目标: 连通后导航到最新目标
        if (deferred.isValid()) {
            if (deferred != safeUrl)
                self->navigateToUrl(deferred, true);
        }
        self->m_fileView->showMessage(
            QStringLiteral("已连接 %1").arg(result.displayName),
            QStringLiteral("info"));
    });
}

void QtFileMgrWidget::onBreadcrumbUrlChanged(const QUrl &url)
{
    if (m_updatingUrl) {
        return;
    }

    if (navigateToUrl(url, true))
        m_filterBar->clearIfUnlocked();
    else
        m_breadcrumb->setUrl(m_url);
}

void QtFileMgrWidget::onFileViewUrlChanged(const QUrl &url)
{
    if (m_updatingUrl) {
        return;
    }
    if (navigateToUrl(url, true, false)) {
        m_filterBar->clearIfUnlocked();
    } else if (!m_url.isEmpty()) {
        m_updatingUrl = true;
        m_fileView->setUrl(m_url);
        m_updatingUrl = false;
    }
}

void QtFileMgrWidget::onZoomChanged(int value)
{
    m_fileView->setZoomLevel(value);
    m_config.view.zoomLevel = value;
}

void QtFileMgrWidget::showIconView()
{
    m_fileView->setViewMode(QtFileMgrFileView::IconsView);
    if (m_hamburgerMenu) m_hamburgerMenu->setViewMode(0);
    m_config.defaultViewMode = 0;
    if (m_iconViewAction) m_iconViewAction->setChecked(true);
}
void QtFileMgrWidget::showCompactView()
{
    m_fileView->setViewMode(QtFileMgrFileView::CompactView);
    if (m_hamburgerMenu) m_hamburgerMenu->setViewMode(1);
    m_config.defaultViewMode = 1;
    if (m_compactViewAction) m_compactViewAction->setChecked(true);
}
void QtFileMgrWidget::showDetailsView()
{
    m_fileView->setViewMode(QtFileMgrFileView::DetailsView);
    if (m_hamburgerMenu) m_hamburgerMenu->setViewMode(2);
    m_config.defaultViewMode = 2;
    if (m_detailViewAction) m_detailViewAction->setChecked(true);
}

void QtFileMgrWidget::toggleBreadcrumbEdit() { m_breadcrumb->setEditableMode(true); }

void QtFileMgrWidget::showSearchDialog()
{
    QPointer<QtFileMgrIO> searchIO = m_io;
    const bool searchRemote = searchIO && searchIO->isRemote();
    const QString root = searchRemote
        ? m_fileView->flatModel()->url() : m_fileView->url().toLocalFile();
    auto *dialog = new QtFileMgrSearchDialog(searchIO, root, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QtFileMgrSearchDialog::pathActivated, this,
            [this, searchIO, searchRemote](const QString &path, bool directory) {
        if (!searchIO || m_io != searchIO.data()) {
            m_fileView->showMessage(
                QStringLiteral("文件后端已经切换，已忽略旧搜索结果。"),
                QStringLiteral("warning"));
            return;
        }
        if (directory) {
            setUrl(searchRemote ? searchIO->urlForPath(path)
                                : QUrl::fromLocalFile(path));
            return;
        }
        QString parent;
        QString name;
        if (searchRemote) {
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            parent = slash > 0 ? path.left(slash)
                               : (slash == 0 ? QStringLiteral("/") : QString());
            name = path.mid(slash + 1);
            setUrl(searchIO->urlForPath(parent));
        } else {
            const QFileInfo info(path);
            parent = info.absolutePath();
            name = info.fileName();
            setUrl(QUrl::fromLocalFile(parent));
        }
        m_fileView->selectNewFiles({name});
    });
    dialog->open();
}

void QtFileMgrWidget::showTrashDialog()
{
    QtFileMgrTrashDialog dialog(this);
    connect(&dialog, &QtFileMgrTrashDialog::entryRestored, this,
            [this](const QString &path) {
        m_fileView->showMessage(
            QStringLiteral("已还原 %1").arg(QFileInfo(path).fileName()),
            QStringLiteral("info"));
    });
    connect(&dialog, &QtFileMgrTrashDialog::trashChanged,
            m_fileView, &QtFileMgrFileView::reload);
    dialog.exec();
}

void QtFileMgrWidget::toggleHiddenFiles()
{
    bool show = !m_fileView->showHiddenFiles();
    m_fileView->setShowHiddenFiles(show);
    if (m_hamburgerMenu) m_hamburgerMenu->setShowHiddenFiles(show);
    if (m_hiddenFilesAction) m_hiddenFilesAction->setChecked(show);
    m_config.view.showHiddenFiles = show;
}

void QtFileMgrWidget::toggleFoldersFirst()
{
    bool on = !m_fileView->foldersFirst();
    m_fileView->setFoldersFirst(on);
    if (m_hamburgerMenu) m_hamburgerMenu->setFoldersFirst(on);
    if (m_foldersFirstAction) m_foldersFirstAction->setChecked(on);
    m_config.view.foldersFirst = on;
}

void QtFileMgrWidget::updateStatusBar()
{
    int count = m_fileView->itemCount();
    QUrl url = m_fileView->url();
    QString pathStr = m_io && m_io->isRemote()
        ? url.toString(QUrl::RemovePassword) : url.toLocalFile();
    m_statusLabel->setText(QStringLiteral(" %1 项").arg(count));
    m_statusLabel->setToolTip(pathStr);
}

// ─── Config 配置应用 (核心) ─────────────────────────────

void QtFileMgrWidget::applyConfig(const QtFileMgrConfig &cfg)
{
    QTMGR_DBG("applyConfig: viewMode=%d hidden=%d foldersFirst=%d zoom=%d",
              cfg.defaultViewMode, cfg.view.showHiddenFiles,
              cfg.view.foldersFirst, cfg.view.zoomLevel);
    // --- 路径 ---
    if (!cfg.rootPath.isEmpty()) {
        const QUrl rootCandidate(cfg.rootPath);
        const QUrl configuredRoot = normalizedNavigationUrl(
            !rootCandidate.scheme().isEmpty() ? rootCandidate
                                             : QUrl::fromLocalFile(cfg.rootPath));
        // 仅配置的根路径相对上次应用变化时才导航;与当前浏览位置无关,
        // 进入子目录后改设置不再跳回 (审查#15/R2-WS-P1-2)
        if (configuredRoot != m_appliedRootUrl
            && isUrlCompatibleWithIO(configuredRoot, m_io)) {
            setUrl(configuredRoot);
            m_appliedRootUrl = configuredRoot;
        }
    }

    // --- 视图模式 + 汉堡菜单同步 ---
    const int configuredMode = cfg.rememberMode
        ? QtFileMgrConfigStore::loadDirectoryViewMode(m_url, cfg.defaultViewMode)
        : cfg.defaultViewMode;
    m_fileView->setViewMode(static_cast<QtFileMgrFileView::ViewMode>(configuredMode));
    if (m_hamburgerMenu) m_hamburgerMenu->setViewMode(configuredMode);
    if (m_iconViewAction) m_iconViewAction->setChecked(configuredMode == 0);
    if (m_compactViewAction) m_compactViewAction->setChecked(configuredMode == 1);
    if (m_detailViewAction) m_detailViewAction->setChecked(configuredMode == 2);

    // --- View 子配置 ---
    m_fileView->setShowHiddenFiles(cfg.view.showHiddenFiles);
    if (m_hamburgerMenu) m_hamburgerMenu->setShowHiddenFiles(cfg.view.showHiddenFiles);
    if (m_hiddenFilesAction) m_hiddenFilesAction->setChecked(cfg.view.showHiddenFiles);

    m_fileView->setFoldersFirst(cfg.view.foldersFirst);
    if (m_hamburgerMenu) m_hamburgerMenu->setFoldersFirst(cfg.view.foldersFirst);
    if (m_foldersFirstAction) m_foldersFirstAction->setChecked(cfg.view.foldersFirst);

    m_fileView->setZoomLevel(cfg.view.zoomLevel);
    if (m_zoomSlider) m_zoomSlider->setValue(cfg.view.zoomLevel);

    m_fileView->setShowVersions(cfg.showVersions);
    m_fileView->setSortColumn(cfg.view.sortColumn, cfg.view.sortOrder);
    m_fileView->setAlternatingRowColors(cfg.view.alternatingRowColors);
    const QList<int> columnWidths = cfg.view.detailColumnWidths.isEmpty()
        ? QtFileMgrConfigStore::loadDetailColumnWidths()
        : cfg.view.detailColumnWidths;
    m_fileView->setDetailColumnWidths(columnWidths);

    // --- 权限 ---
    // allowCreate/allowDelete/allowRename/allowDragDrop 通过 m_fileView->setConfig 生效
    // FileView 在上下文菜单/拖放事件中读取 m_cfg->allowXxx

    // --- 工具栏可见性 ---
    if (m_hamburgerMenu) {
        m_hamburgerMenu->setViewModeActionsVisible(
            cfg.toolbar.items.viewModeIcons || cfg.toolbar.items.viewModeCompact || cfg.toolbar.items.viewModeDetails);
        m_hamburgerMenu->setHiddenFilesActionVisible(cfg.toolbar.items.showHiddenFiles);
        m_hamburgerMenu->setFoldersFirstActionVisible(cfg.toolbar.items.showFoldersFirst);
    }
    if (m_settingsBtn) m_settingsBtn->setVisible(cfg.toolbar.items.settingsButton);
    if (m_breadcrumb) m_breadcrumb->setVisible(cfg.toolbar.items.breadcrumb);
    // toolbar iconSize: 应用到顶栏所有 QToolButton
    QSize toolbarIconSz(cfg.toolbar.iconSize, cfg.toolbar.iconSize);
    for (auto *child : ui->topBar->findChildren<QToolButton *>())
        child->setIconSize(toolbarIconSz);
    m_hamburgerMenu->setIconSize(toolbarIconSz);
    // Reserve a comfortable hit area while respecting the host font and icon size.
    int barH = qMax(cfg.toolbar.iconSize + 28, fontMetrics().height() + 28);
    ui->topBar->setMinimumHeight(barH);
    ui->topBar->setMaximumHeight(barH);

    // --- 状态栏 ---
    if (m_statusBar) m_statusBar->setVisible(cfg.statusBar.visible);
    if (m_zoomSlider)    m_zoomSlider->setVisible(cfg.statusBar.showZoomSlider);
    if (m_zoomLabel)     m_zoomLabel->setVisible(cfg.statusBar.showZoomSlider);
    if (m_diskSpaceLabel) m_diskSpaceLabel->setVisible(cfg.statusBar.showDiskSpace);
    if (m_statusLabel) m_statusLabel->setVisible(cfg.statusBar.showItemCount);

    // --- 外观 ---
    activePlacesPanel()->setIconSize(QSize(cfg.placesPanel.iconSize, cfg.placesPanel.iconSize));
    m_splitter->setHandleWidth(cfg.splitterHandleWidth);

    // --- Places ---
    activePlacesPanel()->setConfigPlaces(cfg.places);
    activePlacesPanel()->setShowDevices(cfg.showDevices);
    activePlacesPanel()->setDeviceRefreshMs(cfg.deviceRefreshMs);
    // placesPanel.width: 精确像素值;窗口未显示时宽度可为负,钳制 (R2-WS-P2-6 遗留)
    if (cfg.placesPanel.width > 0) {
        const int fileAreaWidth = qMax(0, m_splitter->width()
            - cfg.placesPanel.width - m_splitter->handleWidth());
        m_splitter->setSizes({cfg.placesPanel.width, fileAreaWidth});
    }

    // --- 过滤栏 ---
    m_filterBar->setVisible(cfg.filterBar.visible);
    m_filterBar->setFilterMode(static_cast<QtFileMgrFilterBar::FilterMode>(cfg.filterBar.defaultMode));
    m_filterBar->setCaseSensitive(cfg.filterBar.caseSensitive);

    // --- 功能 ---
    // setShowVersions/setDetailColumnWidths 已在上文应用,不再重复 (R2-WS-P2-7)
    m_fileView->setShowThumbnails(cfg.showThumbnails);

    // --- 权限 — 更新 FileView 的 config 指针 ---
    m_fileView->setConfig(&m_config);

    // --- allowedPaths 热更新: 配置重读后当前 URL 可能已不再允许,
    // 停留在原地会让白名单形同虚设;退回配置根(或 home) (R2-WS-P2-1)
    if (!m_config.allowedPaths.isEmpty()) {
        const QUrl currentUrl = m_fileView->url();
        if (currentUrl.isValid() && !currentUrl.isEmpty()
            && !isUrlAllowed(currentUrl)) {
            const QUrl fallback = !m_config.rootPath.isEmpty()
                ? QUrl::fromLocalFile(m_config.rootPath)
                : QUrl::fromLocalFile(QDir::homePath());
            qWarning("[QtFileMgrWidget] applyConfig: current url '%s' no longer allowed by updated allowedPaths, navigating to '%s'",
                     qPrintable(currentUrl.toString(QUrl::RemovePassword)),
                     qPrintable(fallback.toString(QUrl::RemovePassword)));
            setUrl(isUrlAllowed(fallback) ? fallback
                   : QUrl::fromLocalFile(QDir::homePath()));
        }
    }
}

// ─── Disk Space ──────────────────────────────────────────

void QtFileMgrWidget::updateDiskSpace()
{
    const QUrl currentUrl = m_fileView->url();
    if (!currentUrl.isLocalFile()) {
        m_diskSpaceLabel->clear();
        return;
    }
    const QString path = currentUrl.toLocalFile();
    QStorageInfo si(path);
    if (si.isValid() && si.bytesTotal() > 0) {
        QString free = formatDiskSize(si.bytesAvailable());
        m_diskSpaceLabel->setText(QStringLiteral(" 可用: %1").arg(free));
    } else {
        qWarning("[QtFileMgrWidget] disk space: unavailable for local path '%s'",
                 qPrintable(path));
        m_diskSpaceLabel->clear();
    }
}

QString QtFileMgrWidget::formatDiskSize(qint64 bytes)
{
    if (bytes < 1024LL) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024LL*1024) return QStringLiteral("%1 KB").arg(bytes/1024.0, 0, 'f', 1);
    if (bytes < 1024LL*1024*1024) return QStringLiteral("%1 MB").arg(bytes/(1024.0*1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes/(1024.0*1024*1024), 0, 'f', 2);
}

// ─── JSON 配置加载/保存 ──────────────────────────────────

static void initializeQtFileMgrResources()
{
    // An explicit reference keeps the qrc object when linking a static library.
    static const bool initialized = []() {
        Q_INIT_RESOURCE(qtfilemgr);
        return true;
    }();
    Q_UNUSED(initialized);
}

QtFileMgrConfig QtFileMgrConfig::defaultConfig()
{
    initializeQtFileMgrResources();
    QFile f(QStringLiteral(":/config/mgr_default_config.json"));
    if (f.open(QIODevice::ReadOnly)) {
        QJsonObject defaults = QJsonDocument::fromJson(f.readAll()).object();
        QJsonObject places = defaults.value(QStringLiteral("places")).toObject();
        places.remove(QStringLiteral("projectPlaces"));
        defaults.insert(QStringLiteral("places"), places);
        return loadFromJson(QJsonDocument(defaults).toJson());
    }
    qWarning("QtFileMgrConfig: Cannot load embedded default config");
    return QtFileMgrConfig();
}

QtFileMgrConfig QtFileMgrConfig::loadFromFile(const QString &filePath)
{
    if (filePath.isEmpty())
        return defaultConfig();

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("QtFileMgrConfig: Cannot open config file: %s", qPrintable(filePath));
        return defaultConfig();
    }
    return loadFromJson(f.readAll());
}

QtFileMgrConfig QtFileMgrConfig::loadFromJson(const QByteArray &json)
{
    // 从 C++ 默认值开始 (不能用 defaultConfig() 否则无限递归)
    QtFileMgrConfig cfg;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (!doc.isNull() && doc.isObject()) {
        cfg.mergeJsonObject(doc.object());
    } else if (!doc.isNull()) {
        qWarning("QtFileMgrConfig: JSON is not an object");
    } else {
        // 解析失败此前静默回退 C++ 默认值,损坏的配置无法排查 (DLG-P1-6)
        qWarning("QtFileMgrConfig: JSON parse failed (error=\"%s\", offset=%d); falling back to built-in defaults",
                 qPrintable(err.errorString()), static_cast<int>(err.offset));
    }
    return cfg;
}

void QtFileMgrConfig::mergeJsonObject(const QJsonObject &obj)
{
    // --- paths ---
    if (obj.contains(QStringLiteral("paths"))) {
        QJsonObject p = obj[QStringLiteral("paths")].toObject();
        rootPath = p.value(QStringLiteral("rootPath")).toString(rootPath);
        QJsonArray arr = p.value(QStringLiteral("allowedPaths")).toArray();
        if (!arr.isEmpty()) {
            allowedPaths.clear();
            for (const auto &v : arr) allowedPaths << v.toString();
        }
    }

    // --- view ---
    if (obj.contains(QStringLiteral("view"))) {
        QJsonObject v = obj[QStringLiteral("view")].toObject();
        defaultViewMode = v.value(QStringLiteral("defaultMode")).toInt(defaultViewMode);
        rememberMode = v.value(QStringLiteral("rememberMode")).toBool(rememberMode);
        view.showHiddenFiles = v.value(QStringLiteral("showHiddenFiles")).toBool(view.showHiddenFiles);
        view.foldersFirst = v.value(QStringLiteral("foldersFirst")).toBool(view.foldersFirst);
        view.zoomLevel = v.value(QStringLiteral("zoomLevel")).toInt(view.zoomLevel);
        view.sortColumn = v.value(QStringLiteral("sortColumn")).toInt(view.sortColumn);
        view.sortOrder = parseSortOrder(v.value(QStringLiteral("sortOrder")).toString());
        view.alternatingRowColors = v.value(QStringLiteral("alternatingRowColors")).toBool(view.alternatingRowColors);
        const QJsonArray columnWidths = v.value(QStringLiteral("detailColumnWidths")).toArray();
        if (!columnWidths.isEmpty()) {
            view.detailColumnWidths.clear();
            for (const QJsonValue &width : columnWidths)
                view.detailColumnWidths.append(width.toInt());
        }
    }

    // --- permissions ---
    if (obj.contains(QStringLiteral("permissions"))) {
        QJsonObject p = obj[QStringLiteral("permissions")].toObject();
        allowCreate = p.value(QStringLiteral("allowCreate")).toBool(allowCreate);
        allowDelete = p.value(QStringLiteral("allowDelete")).toBool(allowDelete);
        allowRename = p.value(QStringLiteral("allowRename")).toBool(allowRename);
        allowDragDrop = p.value(QStringLiteral("allowDragDrop")).toBool(allowDragDrop);
    }

    // --- appearance ---
    if (obj.contains(QStringLiteral("appearance"))) {
        QJsonObject a = obj[QStringLiteral("appearance")].toObject();

        if (a.contains(QStringLiteral("toolbar"))) {
            QJsonObject t = a[QStringLiteral("toolbar")].toObject();
            toolbar.iconSize = t.value(QStringLiteral("iconSize")).toInt(toolbar.iconSize);
            if (t.contains(QStringLiteral("items"))) {
                QJsonObject ti = t[QStringLiteral("items")].toObject();
                toolbar.items.viewModeIcons = ti.value(QStringLiteral("viewModeIcons")).toBool(true);
                toolbar.items.viewModeCompact = ti.value(QStringLiteral("viewModeCompact")).toBool(true);
                toolbar.items.viewModeDetails = ti.value(QStringLiteral("viewModeDetails")).toBool(true);
                toolbar.items.showHiddenFiles = ti.value(QStringLiteral("showHiddenFiles")).toBool(true);
                toolbar.items.showFoldersFirst = ti.value(QStringLiteral("showFoldersFirst")).toBool(true);
                toolbar.items.breadcrumb = ti.value(QStringLiteral("breadcrumb")).toBool(true);
                toolbar.items.settingsButton = ti.value(QStringLiteral("settingsButton")).toBool(true);
            }
        }
        if (a.contains(QStringLiteral("statusBar"))) {
            QJsonObject s = a[QStringLiteral("statusBar")].toObject();
            statusBar.visible = s.value(QStringLiteral("visible")).toBool(statusBar.visible);
            statusBar.showZoomSlider = s.value(QStringLiteral("showZoomSlider"))
                .toBool(statusBar.showZoomSlider);
            statusBar.showDiskSpace = s.value(QStringLiteral("showDiskSpace"))
                .toBool(statusBar.showDiskSpace);
            statusBar.showItemCount = s.value(QStringLiteral("showItemCount"))
                .toBool(statusBar.showItemCount);
        }
        if (a.contains(QStringLiteral("placesPanel"))) {
            QJsonObject pp = a[QStringLiteral("placesPanel")].toObject();
            placesPanel.width = pp.value(QStringLiteral("width")).toInt(placesPanel.width);
            placesPanel.iconSize = pp.value(QStringLiteral("iconSize")).toInt(placesPanel.iconSize);
        }
        if (a.contains(QStringLiteral("splitter"))) {
            splitterHandleWidth = a[QStringLiteral("splitter")].toObject()
                .value(QStringLiteral("handleWidth")).toInt(splitterHandleWidth);
        }
    }

    // --- places ---
    if (obj.contains(QStringLiteral("places"))) {
        QJsonObject pl = obj[QStringLiteral("places")].toObject();
        showDevices = pl.value(QStringLiteral("showDevices")).toBool(showDevices);
        deviceRefreshMs = pl.value(QStringLiteral("deviceRefreshMs")).toInt(deviceRefreshMs);

        // 内置 place 列表: 合并 builtin + projectPlaces
        QList<PlaceEntry> allPlaces;
        if (pl.contains(QStringLiteral("builtin")))
            allPlaces.append(parsePlaceArray(pl[QStringLiteral("builtin")].toArray()));
        if (pl.contains(QStringLiteral("projectPlaces")))
            allPlaces.append(parsePlaceArray(pl[QStringLiteral("projectPlaces")].toArray()));
        if (!allPlaces.isEmpty())
            places = allPlaces;
    }

    // --- features ---
    if (obj.contains(QStringLiteral("features"))) {
        QJsonObject f = obj[QStringLiteral("features")].toObject();
        if (f.contains(QStringLiteral("filterBar"))) {
            QJsonObject fb = f[QStringLiteral("filterBar")].toObject();
            filterBar.visible = fb.value(QStringLiteral("visible")).toBool(filterBar.visible);
            QString modeStr = fb.value(QStringLiteral("defaultMode")).toString();
            if (modeStr == QStringLiteral("glob")) filterBar.defaultMode = 1;
            else if (modeStr == QStringLiteral("regex")) filterBar.defaultMode = 2;
            else filterBar.defaultMode = 0;
            filterBar.caseSensitive = fb.value(QStringLiteral("caseSensitive")).toBool(filterBar.caseSensitive);
        }
        showThumbnails = f.value(QStringLiteral("showThumbnails")).toBool(showThumbnails);
        showVersions = f.value(QStringLiteral("showVersions")).toBool(showVersions);
    }
}

QList<PlaceEntry> QtFileMgrConfig::parsePlaceArray(const QJsonArray &arr)
{
    QList<PlaceEntry> result;
    for (const auto &v : arr) {
        QJsonObject o = v.toObject();
        PlaceEntry e;
        e.name = o.value(QStringLiteral("name")).toString();
        e.path = o.value(QStringLiteral("path")).toString();
        // ~ 展开为 home 目录
        if (e.path.startsWith(QStringLiteral("~/")))
            e.path = QDir::homePath() + e.path.mid(1);
        else if (e.path == QStringLiteral("~"))
            e.path = QDir::homePath();
        e.iconTheme = o.value(QStringLiteral("icon")).toString();
        result.append(e);
    }
    return result;
}

Qt::SortOrder QtFileMgrConfig::parseSortOrder(const QString &s)
{
    if (s == QStringLiteral("descending")) return Qt::DescendingOrder;
    return Qt::AscendingOrder;
}

QString QtFileMgrConfig::sortOrderToString(Qt::SortOrder order)
{
    return order == Qt::DescendingOrder ? QStringLiteral("descending") : QStringLiteral("ascending");
}

QJsonObject QtFileMgrConfig::toJsonObject() const
{
    QJsonObject obj;
    obj[QStringLiteral("version")] = 1;

    // paths
    QJsonObject paths;
    paths[QStringLiteral("rootPath")] = rootPath;
    QJsonArray allowedArr;
    for (const auto &p : allowedPaths) allowedArr.append(p);
    paths[QStringLiteral("allowedPaths")] = allowedArr;
    obj[QStringLiteral("paths")] = paths;

    // view
    QJsonObject viewObj;
    viewObj[QStringLiteral("defaultMode")] = defaultViewMode;
    viewObj[QStringLiteral("rememberMode")] = rememberMode;
    viewObj[QStringLiteral("showHiddenFiles")] = view.showHiddenFiles;
    viewObj[QStringLiteral("foldersFirst")] = view.foldersFirst;
    viewObj[QStringLiteral("zoomLevel")] = view.zoomLevel;
    viewObj[QStringLiteral("sortColumn")] = view.sortColumn;
    viewObj[QStringLiteral("sortOrder")] = sortOrderToString(view.sortOrder);
    viewObj[QStringLiteral("alternatingRowColors")] = view.alternatingRowColors;
    QJsonArray detailColumnWidths;
    for (int width : view.detailColumnWidths)
        detailColumnWidths.append(width);
    viewObj[QStringLiteral("detailColumnWidths")] = detailColumnWidths;
    obj[QStringLiteral("view")] = viewObj;

    // permissions
    QJsonObject perm;
    perm[QStringLiteral("allowCreate")] = allowCreate;
    perm[QStringLiteral("allowDelete")] = allowDelete;
    perm[QStringLiteral("allowRename")] = allowRename;
    perm[QStringLiteral("allowDragDrop")] = allowDragDrop;
    obj[QStringLiteral("permissions")] = perm;

    // appearance
    QJsonObject appearance;
    QJsonObject tb;
    tb[QStringLiteral("iconSize")] = toolbar.iconSize;
    QJsonObject tbItems;
    tbItems[QStringLiteral("viewModeIcons")] = toolbar.items.viewModeIcons;
    tbItems[QStringLiteral("viewModeCompact")] = toolbar.items.viewModeCompact;
    tbItems[QStringLiteral("viewModeDetails")] = toolbar.items.viewModeDetails;
    tbItems[QStringLiteral("showHiddenFiles")] = toolbar.items.showHiddenFiles;
    tbItems[QStringLiteral("showFoldersFirst")] = toolbar.items.showFoldersFirst;
    tbItems[QStringLiteral("breadcrumb")] = toolbar.items.breadcrumb;
    tbItems[QStringLiteral("settingsButton")] = toolbar.items.settingsButton;
    tb[QStringLiteral("items")] = tbItems;
    appearance[QStringLiteral("toolbar")] = tb;
    QJsonObject sb;
    sb[QStringLiteral("visible")] = statusBar.visible;
    sb[QStringLiteral("showZoomSlider")] = statusBar.showZoomSlider;
    sb[QStringLiteral("showDiskSpace")] = statusBar.showDiskSpace;
    sb[QStringLiteral("showItemCount")] = statusBar.showItemCount;
    appearance[QStringLiteral("statusBar")] = sb;
    QJsonObject pp;
    pp[QStringLiteral("width")] = placesPanel.width;
    pp[QStringLiteral("iconSize")] = placesPanel.iconSize;
    appearance[QStringLiteral("placesPanel")] = pp;
    QJsonObject sp;
    sp[QStringLiteral("handleWidth")] = splitterHandleWidth;
    appearance[QStringLiteral("splitter")] = sp;
    obj[QStringLiteral("appearance")] = appearance;

    // places
    QJsonObject placesObj;
    placesObj[QStringLiteral("showDevices")] = showDevices;
    placesObj[QStringLiteral("deviceRefreshMs")] = deviceRefreshMs;
    QJsonArray builtinArr;
    for (const auto &e : places) {
        QJsonObject eo;
        eo[QStringLiteral("name")] = e.name;
        eo[QStringLiteral("path")] = e.path;
        eo[QStringLiteral("icon")] = e.iconTheme;
        builtinArr.append(eo);
    }
    placesObj[QStringLiteral("builtin")] = builtinArr;
    obj[QStringLiteral("places")] = placesObj;

    // features
    QJsonObject features;
    QJsonObject fb;
    fb[QStringLiteral("visible")] = filterBar.visible;
    fb[QStringLiteral("defaultMode")] = filterBar.defaultMode == 1 ? QStringLiteral("glob")
                                       : filterBar.defaultMode == 2 ? QStringLiteral("regex")
                                       : QStringLiteral("plaintext");
    fb[QStringLiteral("caseSensitive")] = filterBar.caseSensitive;
    features[QStringLiteral("filterBar")] = fb;
    features[QStringLiteral("showThumbnails")] = showThumbnails;
    features[QStringLiteral("showVersions")] = showVersions;
    obj[QStringLiteral("features")] = features;

    return obj;
}

QByteArray QtFileMgrConfig::toJson() const
{
    return QJsonDocument(toJsonObject()).toJson(QJsonDocument::Indented);
}
