// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrWorkspace.h"

#include "QtFileMgrFileView.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrPlacesPanel.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrStyle.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

namespace {

const char kWorkspaceSessionGroup[] = "QtFileMgrWorkspace/session-v1";
// Set only by Widget for an explicitly adopted provider IO. QObject parentage
// alone does not authorize Workspace to take ownership of a host's backend.
const char kBackendOwnerProperty[] = "_qtfilemgr_backendOwner";

QUrl urlFromState(const QJsonValue &value)
{
    const QUrl url(value.toString());
    if (!url.isValid() || url.isEmpty()) {
        qWarning("[Workspace] session: invalid URL; using home (value=\"%s\")",
                 qPrintable(value.toString()));
        return QUrl::fromLocalFile(QDir::homePath());
    }
    if (!url.password().isEmpty())
        qWarning("[Workspace] session: removed embedded password (host=\"%s\")",
                 qPrintable(url.host()));
    return url.adjusted(QUrl::RemovePassword);
}

QString directoryPathFor(const QtFileMgrWidget *manager)
{
    if (!manager)
        return QString();
    const QUrl url = manager->url();
    if (url.isLocalFile())
        return url.toLocalFile();
    return manager->io() ? manager->io()->pathFromUrl(url) : url.path();
}

} // namespace

QtFileMgrWorkspace::QtFileMgrWorkspace(
    QWidget *parent, bool restoreSavedSession)
    : QWidget(parent)
    , m_tabs(new QTabWidget(this))
    , m_restoreSavedSession(restoreSavedSession)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tools = new QWidget(this);
    tools->setObjectName(QStringLiteral("qtFileMgrWorkspaceTools"));
    auto *toolLayout = new QHBoxLayout(tools);
    toolLayout->setContentsMargins(6, 4, 8, 4);
    toolLayout->setSpacing(4);

    auto addTool = [this, toolLayout](QtFileMgrStyle::Symbol symbol,
                                      const QString &toolTip,
                                      const char *slot) {
        auto *button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setIcon(QtFileMgrStyle::toolIcon(symbol, palette()));
        button->setProperty("qtFileMgrSymbol", int(symbol));
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(30, 30);
        button->setToolTip(toolTip);
        connect(button, SIGNAL(clicked()), this, slot);
        toolLayout->addWidget(button);
        return button;
    };
    addTool(QtFileMgrStyle::Symbol::AddTab,
            QStringLiteral("新建标签页"),
            SLOT(addTab()));
    addTool(QtFileMgrStyle::Symbol::Split, QStringLiteral("切换双栏 (F3)"),
            SLOT(splitCurrentTab()));
    m_copyToOtherButton = addTool(
        QtFileMgrStyle::Symbol::CopyToOther,
        QStringLiteral("复制选中项到另一栏"),
        SLOT(copyToOtherPane()));
    m_moveToOtherButton = addTool(
        QtFileMgrStyle::Symbol::MoveToOther,
        QStringLiteral("移动选中项到另一栏"),
        SLOT(moveToOtherPane()));
    m_closeSplitButton = addTool(
        QtFileMgrStyle::Symbol::Close,
        QStringLiteral("关闭当前分栏"),
        SLOT(closeCurrentSplit()));

    m_tabs->setDocumentMode(true);
    m_tabs->setObjectName(QStringLiteral("qtFileMgrWorkspaceTabs"));
    m_tabs->tabBar()->setObjectName(QStringLiteral("qtFileMgrWorkspaceTabBar"));
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setElideMode(Qt::ElideRight);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setTabsClosable(false);
    m_tabs->setMovable(true);
    m_tabs->setCornerWidget(tools, Qt::TopRightCorner);
    m_tabs->tabBar()->installEventFilter(this);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this,
            [this](int index) { closeTab(index, true); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this]() {
        updateActivePane();
        scheduleSessionSave();
    });
    connect(m_tabs->tabBar(), &QTabBar::tabMoved,
            this, [this]() { scheduleSessionSave(); });
    connect(m_tabs->tabBar(), &QTabBar::tabBarDoubleClicked,
            this, [this](int index) {
        if (index < 0)
            addTab();
    });
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) {
        const int index = m_tabs->tabBar()->tabAt(pos);
        QMenu menu(this);
        if (index >= 0) {
            menu.addAction(QStringLiteral("复制标签页"),
                           this, [this, index]() { duplicateTab(index); });
            QAction *closeAction = menu.addAction(
                QStringLiteral("关闭标签页"),
                this, [this, index]() { closeTab(index, true); });
            closeAction->setEnabled(m_tabs->count() > 1);
        } else {
            menu.addAction(QStringLiteral("新建标签页"),
                           this, [this]() { addTab(); });
        }
        QAction *restoreAction = menu.addAction(
            QStringLiteral("恢复最近关闭的标签页"),
            this, &QtFileMgrWorkspace::restoreClosedTab);
        restoreAction->setEnabled(!m_closedTabs.isEmpty());
        menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });
    // 全局唯一共享 Places 面板: 标签/分栏只切换文件区域 (Dolphin 布局),
    // 面板宽度全局一致, 内容/高亮随活动面板同步
    m_sharedPlacesPanel = new QtFileMgrPlacesPanel(this);
    m_sharedPlacesPanel->setObjectName(QStringLiteral("qtFileMgrWorkspacePlaces"));
    // 面板最小宽度: 分栏后内容区最小尺寸变大时,QSplitter 会把面板
    // 压到 sizeHint 下限(约 69px 不可读),设下限防止挤压
    m_sharedPlacesPanel->setMinimumWidth(160);
    const QtFileMgrConfig sharedCfg = QtFileMgrConfigStore::loadDefault();
    m_sharedPlacesPanel->setIconSize(QSize(sharedCfg.placesPanel.iconSize,
                                           sharedCfg.placesPanel.iconSize));
    m_sharedPlacesPanel->setConfigPlaces(sharedCfg.places);
    m_sharedPlacesPanel->setShowDevices(sharedCfg.showDevices);
    m_sharedPlacesPanel->setDeviceRefreshMs(sharedCfg.deviceRefreshMs);
    connect(m_sharedPlacesPanel, &QtFileMgrPlacesPanel::placeActivated,
            this, [this](const QString &path) {
        if (QtFileMgrWidget *manager = currentFileManager())
            manager->onPlaceActivated(path);
    });
    connect(m_sharedPlacesPanel, &QtFileMgrPlacesPanel::trashRequested,
            this, [this]() {
        if (QtFileMgrWidget *manager = currentFileManager())
            manager->showTrashDialog();
    });
    connect(m_sharedPlacesPanel, &QtFileMgrPlacesPanel::errorMessage,
            this, [this](const QString &message) {
        if (QtFileMgrWidget *manager = currentFileManager())
            manager->fileView()->showMessage(message, QStringLiteral("warning"));
    });

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("qtFileMgrWorkspaceSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->addWidget(m_sharedPlacesPanel);
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    // 固定初始面板宽度: 配置缺省时也要显式给尺寸,否则 QSplitter 按
    // sizeHint 分配,分栏/窗口变化会把面板压到不可读 (自查发现)
    const int panelWidth = sharedCfg.placesPanel.width > 0
        ? sharedCfg.placesPanel.width : 220;
    splitter->setSizes({panelWidth, 1000});  // QSplitter 按比例归一
    layout->addWidget(splitter, 1);

    const auto applyAppearance = [this, tools](const QPalette &themePalette) {
        const auto c = QtFileMgrStyle::colors(themePalette);
        setStyleSheet(QStringLiteral(
        "QtFileMgrWorkspace{background:%1;}"
        "#qtFileMgrWorkspaceTabs::pane{border:0;background:%1;}"
        "#qtFileMgrWorkspaceTabBar{background:%2;}"
        "#qtFileMgrWorkspaceTabBar::tab{background:%2;color:%4;"
        "border:0;border-bottom:2px solid transparent;"
        "min-width:92px;max-width:210px;padding:9px 16px;margin:0 2px 0 0;}"
        "#qtFileMgrWorkspaceTabBar::tab:selected{background:%1;color:%5;"
        "border-bottom:2px solid %6;}"
        "#qtFileMgrWorkspaceTabBar::tab:hover:!selected{background:%7;color:%5;}"
        "#qtFileMgrWorkspaceTabBar QToolButton{background:%2;border:0;width:24px;}"
        "#qtFileMgrWorkspaceTools{background:%2;}"
        "#qtFileMgrWorkspaceTools QToolButton{border:0;border-radius:5px;padding:4px;}"
        "#qtFileMgrWorkspaceTools QToolButton:hover{background:%7;}"
        "#qtFileMgrWorkspaceTools QToolButton:pressed{background:%8;}"
        "#qtFileMgrWorkspaceTools QToolButton:focus{border:1px solid %6;padding:3px;}"
        "#qtFileMgrWorkspaceSplitter::handle{background:%3;}"
        "QSplitter::handle:horizontal{background:%3;}"
        "QSplitter::handle:horizontal:hover{background:%6;}"
        "QtFileMgrWidget[activePane=\"false\"] {"
        " border: 1px solid transparent;"
        "}"
        "QtFileMgrWidget[activePane=\"true\"] {"
        " border: 1px solid %6;"
        "}").arg(c.base.name(), c.surface.name(), c.border.name(), c.muted.name(),
                   c.text.name(), c.accent.name(), c.hover.name(), c.selection.name()));
        for (QToolButton *button : tools->findChildren<QToolButton *>()) {
            button->setIcon(QtFileMgrStyle::toolIcon(
                QtFileMgrStyle::Symbol(button->property("qtFileMgrSymbol").toInt()),
                themePalette));
        }
        updateTransferActions();
    };
    applyAppearance(palette());
    QtFileMgrStyle::observeApplicationPalette(this, applyAppearance);

    m_sessionSaveTimer = new QTimer(this);
    m_sessionSaveTimer->setSingleShot(true);
    m_sessionSaveTimer->setInterval(250);
    connect(m_sessionSaveTimer, &QTimer::timeout,
            this, &QtFileMgrWorkspace::saveSession);

    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *) { updateActivePane(); });

    auto addShortcut = [this](const QKeySequence &sequence, const char *slot) {
        auto *action = new QAction(this);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, SIGNAL(triggered()), this, slot);
    };
    addShortcut(QKeySequence::AddTab, SLOT(addTab()));
    addShortcut(QKeySequence::Close, SLOT(closeCurrentTab()));
    addShortcut(QKeySequence(Qt::Key_F3), SLOT(splitCurrentTab()));
    addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
                SLOT(restoreClosedTab()));

    auto addActionShortcut = [this](
        const QKeySequence &sequence, const std::function<void()> &callback) {
        auto *action = new QAction(this);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
    };
    addActionShortcut(QKeySequence::NextChild, [this]() {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1)
                                    % m_tabs->count());
    });
    addActionShortcut(QKeySequence::PreviousChild, [this]() {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex()
                                     + m_tabs->count() - 1)
                                    % m_tabs->count());
    });
    for (int i = 0; i < 9; ++i) {
        addActionShortcut(
            QKeySequence(Qt::CTRL | (Qt::Key_1 + i)), [this, i]() {
            if (i < m_tabs->count())
                m_tabs->setCurrentIndex(i);
        });
    }

    if (!m_restoreSavedSession || !restoreSession())
        addTab(QUrl::fromLocalFile(QDir::homePath()));
    updateActivePane();
}

QtFileMgrWorkspace::~QtFileMgrWorkspace()
{
    if (m_restoreSavedSession)
        saveSession();
}

int QtFileMgrWorkspace::tabCount() const
{
    return m_tabs->count();
}

int QtFileMgrWorkspace::paneCount() const
{
    const auto *splitter =
        qobject_cast<QSplitter *>(m_tabs->currentWidget());
    return splitter ? splitter->count() : 0;
}

QtFileMgrWidget *QtFileMgrWorkspace::focusedFileManager() const
{
    QWidget *page = m_tabs->currentWidget();
    if (!page)
        return nullptr;
    if (m_activeManager
        && (m_activeManager == page || page->isAncestorOf(m_activeManager))) {
        return m_activeManager;
    }

    QWidget *focus = QApplication::focusWidget();
    const QList<QtFileMgrWidget *> managers =
        page->findChildren<QtFileMgrWidget *>();
    for (QtFileMgrWidget *manager : managers) {
        if (focus == manager || manager->isAncestorOf(focus))
            return manager;
    }
    return managers.isEmpty() ? nullptr : managers.first();
}

QtFileMgrWidget *QtFileMgrWorkspace::currentFileManager() const
{
    return focusedFileManager();
}

QtFileMgrWidget *QtFileMgrWorkspace::addPane(
    QWidget *page, const QUrl &url)
{
    auto *splitter = qobject_cast<QSplitter *>(page);
    if (!splitter)
        return nullptr;
    auto *manager = new QtFileMgrWidget(splitter);
    manager->setObjectName(QStringLiteral("qtFileMgrWorkspacePane"));
    manager->setSharedPlacesPanel(m_sharedPlacesPanel);
    if (url.isValid())
        manager->setUrl(url);
    splitter->addWidget(manager);
    connectManager(page, manager);
    return manager;
}

void QtFileMgrWorkspace::connectManager(
    QWidget *page, QtFileMgrWidget *manager)
{
    connect(manager, &QtFileMgrWidget::urlChanged, this,
            [this, page, manager](const QUrl &url) {
        if (manager == focusedFileManager())
            updateTabTitle(page, url);
        scheduleSessionSave();
    });
    connect(manager, &QObject::destroyed, this, [this, manager]() {
        if (m_activeManager == manager)
            m_activeManager = nullptr;
        QTimer::singleShot(0, this, [this]() { updateActivePane(); });
    });
}

QWidget *QtFileMgrWorkspace::createTabPage(const QJsonObject &state)
{
    auto *page = new QSplitter(Qt::Horizontal, m_tabs);
    page->setChildrenCollapsible(false);

    const QJsonArray panes = state.value(QStringLiteral("panes")).toArray();
    if (panes.isEmpty()) {
        addPane(page, QUrl::fromLocalFile(QDir::homePath()));
    } else {
        const int count = qMin(2, panes.size());
        for (int i = 0; i < count; ++i)
            addPane(page, urlFromState(panes.at(i)));
    }

    const QJsonArray sizes = state.value(QStringLiteral("sizes")).toArray();
    if (sizes.size() == page->count()) {
        QList<int> splitterSizes;
        for (const QJsonValue &value : sizes)
            splitterSizes << qMax(1, value.toInt(1));
        page->setSizes(splitterSizes);
    }
    connect(page, &QSplitter::splitterMoved,
            this, [this]() { scheduleSessionSave(); });

    const int index = m_tabs->addTab(page, QStringLiteral("文件"));
    updateTabCloseVisibility();
    m_tabs->setCurrentIndex(index);
    const QList<QtFileMgrWidget *> managers =
        page->findChildren<QtFileMgrWidget *>();
    const int activePane = qBound(
        0, state.value(QStringLiteral("activePane")).toInt(0),
        qMax(0, managers.size() - 1));
    if (!managers.isEmpty())
        setActiveManager(managers.at(activePane));
    updateTabTitle(page, currentFileManager()
        ? currentFileManager()->url() : QUrl());
    return page;
}

void QtFileMgrWorkspace::addTab(const QUrl &url)
{
    QJsonObject state;
    state[QStringLiteral("panes")] = QJsonArray{url.toString()};
    createTabPage(state);
    scheduleSessionSave();
}

void QtFileMgrWorkspace::addTab()
{
    const QUrl url = currentFileManager()
        ? currentFileManager()->url()
        : QUrl::fromLocalFile(QDir::homePath());
    addTab(url);
}

QJsonObject QtFileMgrWorkspace::tabState(QWidget *page) const
{
    QJsonObject state;
    const auto *splitter = qobject_cast<QSplitter *>(page);
    if (!splitter)
        return state;

    QJsonArray panes;
    int activePane = 0;
    const QList<QtFileMgrWidget *> managers =
        page->findChildren<QtFileMgrWidget *>();
    for (int i = 0; i < managers.size(); ++i) {
        panes.append(managers.at(i)->url().toString(QUrl::RemovePassword));
        if (managers.at(i) == m_activeManager)
            activePane = i;
    }
    QJsonArray sizes;
    for (int size : splitter->sizes())
        sizes.append(size);
    state[QStringLiteral("panes")] = panes;
    state[QStringLiteral("sizes")] = sizes;
    state[QStringLiteral("activePane")] = activePane;
    return state;
}

void QtFileMgrWorkspace::closeTab(int index, bool remember)
{
    if (index < 0 || index >= m_tabs->count() || m_tabs->count() <= 1)
        return;
    QWidget *page = m_tabs->widget(index);
    if (remember) {
        m_closedTabs.prepend(tabState(page));
        while (m_closedTabs.size() > 10)
            m_closedTabs.removeLast();
    }
    m_tabs->removeTab(index);
    updateTabCloseVisibility();
    page->deleteLater();
    updateActivePane();
    scheduleSessionSave();
}

void QtFileMgrWorkspace::closeCurrentTab()
{
    closeTab(m_tabs->currentIndex(), true);
}

void QtFileMgrWorkspace::duplicateTab(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;
    createTabPage(tabState(m_tabs->widget(index)));
    scheduleSessionSave();
}

void QtFileMgrWorkspace::restoreClosedTab()
{
    if (m_closedTabs.isEmpty())
        return;
    createTabPage(m_closedTabs.takeFirst());
    scheduleSessionSave();
}

void QtFileMgrWorkspace::splitCurrentTab()
{
    auto *splitter =
        qobject_cast<QSplitter *>(m_tabs->currentWidget());
    QtFileMgrWidget *source = focusedFileManager();
    if (!splitter || !source)
        return;
    if (splitter->count() >= 2) {
        closeCurrentSplit();
        return;
    }

    const QUrl sourceUrl = source->url();
    QtFileMgrWidget *manager = nullptr;
    if (source->io() && source->io()->isRemote()) {
        manager = addPane(splitter, QUrl());
        manager->setIO(source->io());
        if (manager->io() != source->io()) {
            qWarning("[Workspace] split: rejected shared backend (url=\"%s\")",
                     qPrintable(sourceUrl.toString(QUrl::RemovePassword)));
            delete manager;
            return;
        }
        QtFileMgrIO *const backend = source->io();
        if (backend->parent() == source
            && backend->property(kBackendOwnerProperty).value<QObject *>() == source) {
            // An adopted session belongs to the tab while either pane uses it.
            // An injected/shared backend retains its caller-selected lifetime.
            backend->setParent(splitter);
            backend->setProperty(kBackendOwnerProperty,
                                 QVariant::fromValue(static_cast<QObject *>(splitter)));
        }
        manager->setUrl(sourceUrl);
    } else {
        manager = addPane(splitter, sourceUrl);
    }
    splitter->setSizes({1, 1});
    setActiveManager(manager);
    manager->fileView()->focusCurrentView();
    scheduleSessionSave();
}

void QtFileMgrWorkspace::closeCurrentSplit()
{
    auto *splitter =
        qobject_cast<QSplitter *>(m_tabs->currentWidget());
    QtFileMgrWidget *manager = focusedFileManager();
    if (!splitter || !manager || splitter->count() <= 1)
        return;

    const QList<QtFileMgrWidget *> managers =
        splitter->findChildren<QtFileMgrWidget *>();
    QtFileMgrWidget *remaining = nullptr;
    for (QtFileMgrWidget *candidate : managers) {
        if (candidate != manager) {
            remaining = candidate;
            break;
        }
    }
    manager->deleteLater();
    setActiveManager(remaining);
    if (remaining)
        remaining->fileView()->focusCurrentView();
    scheduleSessionSave();
}

QtFileMgrWidget *QtFileMgrWorkspace::otherFileManager() const
{
    auto *splitter =
        qobject_cast<QSplitter *>(m_tabs->currentWidget());
    QtFileMgrWidget *source = focusedFileManager();
    if (!splitter || !source || splitter->count() != 2)
        return nullptr;
    const QList<QtFileMgrWidget *> managers =
        splitter->findChildren<QtFileMgrWidget *>();
    for (QtFileMgrWidget *manager : managers) {
        if (manager != source)
            return manager;
    }
    return nullptr;
}

void QtFileMgrWorkspace::transferToOtherPane(bool move)
{
    QtFileMgrWidget *source = focusedFileManager();
    QtFileMgrWidget *destination = otherFileManager();
    if (!source || !destination)
        return;
    if (!source->io() || !destination->io()
        || source->io()->metaObject() != destination->io()->metaObject()
        || (source->io()->isRemote()
            && source->io() != destination->io())) {
        source->fileView()->showMessage(
            QStringLiteral("两个分栏使用不同文件后端，不能直接传输。"),
            QStringLiteral("warning"));
        return;
    }
    source->fileView()->transferSelectedTo(
        directoryPathFor(destination), move);
}

void QtFileMgrWorkspace::copyToOtherPane()
{
    transferToOtherPane(false);
}

void QtFileMgrWorkspace::moveToOtherPane()
{
    transferToOtherPane(true);
}

void QtFileMgrWorkspace::setActiveManager(QtFileMgrWidget *manager)
{
    QWidget *page = m_tabs->currentWidget();
    if (manager && page && !page->isAncestorOf(manager)) {
        return;
    }
    m_activeManager = manager;
    const QList<QtFileMgrWidget *> managers =
        page ? page->findChildren<QtFileMgrWidget *>()
             : QList<QtFileMgrWidget *>();
    for (QtFileMgrWidget *candidate : managers) {
        const bool active = candidate == manager;
        if (candidate->property("activePane").toBool() == active)
            continue;
        candidate->setProperty("activePane", active);
        candidate->style()->unpolish(candidate);
        candidate->style()->polish(candidate);
        candidate->update();
    }
    if (page && manager)
        updateTabTitle(page, manager->url());
    // 切换标签/分栏时共享面板高亮跟随当前活动面板的 URL
    if (m_sharedPlacesPanel && manager) {
        const QUrl url = manager->url();
        const QString path = (url.scheme() == QStringLiteral("remote")
                              || url.scheme() == QStringLiteral("ssh"))
            ? url.toString(QUrl::PreferLocalFile) : url.toLocalFile();
        m_sharedPlacesPanel->selectPlaceForPath(path);
    }
    updateTransferActions();
}

void QtFileMgrWorkspace::updateActivePane()
{
    QWidget *page = m_tabs->currentWidget();
    if (!page) {
        setActiveManager(nullptr);
        return;
    }
    QWidget *focus = QApplication::focusWidget();
    const QList<QtFileMgrWidget *> managers =
        page->findChildren<QtFileMgrWidget *>();
    for (QtFileMgrWidget *manager : managers) {
        if (focus == manager || manager->isAncestorOf(focus)) {
            setActiveManager(manager);
            return;
        }
    }
    if (!managers.contains(m_activeManager))
        setActiveManager(managers.isEmpty() ? nullptr : managers.first());
    else
        updateTransferActions();
}

void QtFileMgrWorkspace::updateTransferActions()
{
    auto *splitter =
        qobject_cast<QSplitter *>(m_tabs->currentWidget());
    const bool split = splitter && splitter->count() == 2;
    if (m_copyToOtherButton)
        m_copyToOtherButton->setVisible(split);
    if (m_moveToOtherButton)
        m_moveToOtherButton->setVisible(split);
    if (m_closeSplitButton)
        m_closeSplitButton->setVisible(split);

    if (!split || !m_activeManager)
        return;
    const QList<QtFileMgrWidget *> managers =
        splitter->findChildren<QtFileMgrWidget *>();
    const bool toRight =
        managers.indexOf(m_activeManager) <= 0;
    m_copyToOtherButton->setIcon(QtFileMgrStyle::toolIcon(
        QtFileMgrStyle::Symbol::CopyToOther, QApplication::palette(this), !toRight));
    m_moveToOtherButton->setIcon(QtFileMgrStyle::toolIcon(
        QtFileMgrStyle::Symbol::MoveToOther, QApplication::palette(this), !toRight));
}

void QtFileMgrWorkspace::updateTabCloseVisibility()
{
    m_tabs->setTabsClosable(m_tabs->count() > 1);
}

QJsonObject QtFileMgrWorkspace::sessionState() const
{
    QJsonObject state;
    state[QStringLiteral("version")] = 1;
    state[QStringLiteral("currentTab")] = m_tabs->currentIndex();
    QJsonArray tabs;
    for (int i = 0; i < m_tabs->count(); ++i)
        tabs.append(tabState(m_tabs->widget(i)));
    state[QStringLiteral("tabs")] = tabs;
    return state;
}

bool QtFileMgrWorkspace::restoreSessionState(const QJsonObject &state)
{
    const QJsonArray tabs = state.value(QStringLiteral("tabs")).toArray();
    if (state.value(QStringLiteral("version")).toInt() != 1
        || tabs.isEmpty()) {
        return false;
    }

    // 先验证再清除: 旧实现先删光现有标签再逐条恢复,若 JSON 中
    // 没有一条合法 tab,用户现有会话被白白清空 (R2-WS-P2-8)
    int validTabs = 0;
    for (const QJsonValue &tab : tabs) {
        if (tab.isObject())
            ++validTabs;
    }
    if (validTabs == 0) {
        qWarning("[Workspace] restoreSession: no valid tab objects in state (%d entries), keeping existing session",
                 tabs.size());
        return false;
    }
    if (validTabs < tabs.size()) {
        qWarning("[Workspace] restoreSession: skipped %d malformed tab entries (of %d)",
                 tabs.size() - validTabs, tabs.size());
    }

    m_restoringSession = true;
    while (m_tabs->count() > 0) {
        QWidget *page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete page;
    }
    for (const QJsonValue &tab : tabs) {
        if (tab.isObject())
            createTabPage(tab.toObject());
    }
    if (m_tabs->count() == 0) {
        // 防御: 全部 createTabPage 失败(理论上不会,addPane 有 home 兜底)
        qWarning("[Workspace] restoreSession: all %d tab(s) failed to create, restoring fallback home tab",
                 validTabs);
        m_restoringSession = false;
        createTabPage(QJsonObject());
        updateActivePane();
        return true;
    }
    m_tabs->setCurrentIndex(qBound(
        0, state.value(QStringLiteral("currentTab")).toInt(0),
        m_tabs->count() - 1));
    m_restoringSession = false;
    updateActivePane();
    return true;
}

void QtFileMgrWorkspace::scheduleSessionSave()
{
    if (!m_restoreSavedSession || m_restoringSession)
        return;
    m_sessionSaveTimer->start();
}

void QtFileMgrWorkspace::saveSession() const
{
    if (!m_restoreSavedSession || m_restoringSession)
        return;
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.beginGroup(QString::fromLatin1(kWorkspaceSessionGroup));
    settings.setValue(
        QStringLiteral("state"),
        QJsonDocument(sessionState()).toJson(QJsonDocument::Compact));
    settings.endGroup();
}

bool QtFileMgrWorkspace::restoreSession()
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.beginGroup(QString::fromLatin1(kWorkspaceSessionGroup));
    const QByteArray data = settings.value(QStringLiteral("state")).toByteArray();
    settings.endGroup();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    return error.error == QJsonParseError::NoError
        && document.isObject()
        && restoreSessionState(document.object());
}

bool QtFileMgrWorkspace::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabs->tabBar()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::MiddleButton) {
            const int index = m_tabs->tabBar()->tabAt(mouseEvent->pos());
            if (index < 0)
                return QWidget::eventFilter(watched, event);
            // 只在 press 上关标签;配套的 release/dblclick 也必须吃掉,
            // 否则 QTabBar 收到一个没有 press 的 release,内部按下
            // 状态错位 (R2-WS-P2-9)
            if (event->type() == QEvent::MouseButtonPress)
                closeTab(index, true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QtFileMgrWorkspace::updateTabTitle(
    QWidget *page, const QUrl &url)
{
    const int index = m_tabs->indexOf(page);
    if (index < 0)
        return;
    QString title = url.fileName();
    if (title.isEmpty())
        title = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (title.isEmpty())
        title = QStringLiteral("文件");
    m_tabs->setTabText(index, title);
    m_tabs->setTabToolTip(index, url.toString(QUrl::PreferLocalFile));
}
