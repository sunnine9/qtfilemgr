// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

//
#include "QtFileMgrBreadcrumbBar.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrStyle.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QCompleter>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QStyle>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QAbstractItemView>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include <QSet>
#include <QtDebug>
#include <QApplication>
#include <QGuiApplication>

static QString normalizedRemotePath(QString path)
{
    if (path.startsWith(QStringLiteral("remote://")))
        path = QUrl(path).path();
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/')) && !path.isEmpty())
        path.chop(1);
    if (path == QLatin1String(".") || path == QLatin1String(".."))
        return QString();
    return path;
}

static QUrl remoteUrlFromPath(const QString &path)
{
    const QString remotePath = normalizedRemotePath(path);
    QUrl url(QStringLiteral("remote:///"));
    if (!remotePath.isEmpty())
        url.setPath(QStringLiteral("/") + remotePath);
    return url;
}

static QString localPathFromEditText(QString text, const QString &baseDir)
{
    text = QDir::fromNativeSeparators(text.trimmed());
    if (text.startsWith(QLatin1String("file://")))
        text = QUrl(text).toLocalFile();
    if (text == QLatin1String("~"))
        text = QDir::homePath();
    else if (text.startsWith(QLatin1String("~/")))
        text = QDir::homePath() + text.mid(1);
    else if (QDir::isRelativePath(text))
        text = QDir(baseDir).absoluteFilePath(text);
    return QDir::cleanPath(text);
}

class QtFileMgrPathCompleter final : public QCompleter {
public:
    explicit QtFileMgrPathCompleter(QObject *parent)
        : QCompleter(parent)
    {
    }

    QString pathFromIndex(const QModelIndex &index) const override
    {
        return index.data(Qt::UserRole).toString();
    }

    QStringList splitPath(const QString &path) const override
    {
        const QString normalized = QDir::fromNativeSeparators(path);
        return {normalized.mid(normalized.lastIndexOf(QLatin1Char('/')) + 1)};
    }
};

QtFileMgrBreadcrumbBar::QtFileMgrBreadcrumbBar(QWidget *parent)
    : QWidget(parent)
    , m_url(QDir::homePath())
    , m_editable(false)
    , m_layout(new QHBoxLayout(this))
    , m_edit(nullptr)
    , m_backBtn(nullptr)
    , m_forwardBtn(nullptr)
{
    setObjectName(QStringLiteral("qtfilemgrBreadcrumb"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(0);
    m_layout->setContentsMargins(5, 4, 5, 4);
    m_layout->setSpacing(0);

    m_backBtn = new QToolButton(this);
    m_backBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Back, palette()));
    m_backBtn->setAutoRaise(true);
    m_backBtn->setToolTip(QStringLiteral("后退"));
    connect(m_backBtn, &QToolButton::clicked, this, &QtFileMgrBreadcrumbBar::goBack);
    m_layout->addWidget(m_backBtn);

    m_forwardBtn = new QToolButton(this);
    m_forwardBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Forward, palette()));
    m_forwardBtn->setAutoRaise(true);
    m_forwardBtn->setToolTip(QStringLiteral("前进"));
    connect(m_forwardBtn, &QToolButton::clicked, this, &QtFileMgrBreadcrumbBar::goForward);
    m_layout->addWidget(m_forwardBtn);

    m_upBtn = new QToolButton(this);
    m_upBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Up, palette()));
    m_upBtn->setAutoRaise(true);
    m_upBtn->setToolTip(QStringLiteral("上级"));
    connect(m_upBtn, &QToolButton::clicked, this, &QtFileMgrBreadcrumbBar::goUp);
    m_layout->addWidget(m_upBtn);

    for (QToolButton *button : {m_backBtn, m_forwardBtn, m_upBtn}) {
        button->setFixedSize(30, 30);
        button->setIconSize(QSize(18, 18));
    }

    QFrame *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    sep->setObjectName(QStringLiteral("breadcrumbDivider"));
    sep->setFixedWidth(1);
    sep->setFixedHeight(18);
    m_layout->addWidget(sep);

    m_edit = new QLineEdit(this);
    m_edit->setMinimumWidth(0);
    m_edit->setMinimumHeight(30);
    m_edit->setVisible(false);
    m_completer = new QtFileMgrPathCompleter(this);
    m_completionModel = new QStandardItemModel(m_completer);
    m_completer->setModel(m_completionModel);
    // 只做建议提示, 不自动填完, 不触发导航 — 用户按 Enter 才导航
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_edit->setCompleter(m_completer);
    connect(m_edit, &QLineEdit::returnPressed, this, &QtFileMgrBreadcrumbBar::onEditReturnPressed);
    connect(m_edit, &QLineEdit::textEdited,
            this, &QtFileMgrBreadcrumbBar::onEditTextEdited);
    m_edit->installEventFilter(this);
    m_layout->addWidget(m_edit, 1);

    // 只读标记 (参照 Dolphin KUrlNavigator::setReadOnlyBadgeVisible)
    m_readOnlyBadge = new QLabel(QStringLiteral("[RO]"), this);
    m_readOnlyBadge->setToolTip(QStringLiteral("此目录不可写"));
    m_readOnlyBadge->setObjectName(QStringLiteral("breadcrumbReadOnly"));
    m_readOnlyBadge->hide();
    m_layout->addWidget(m_readOnlyBadge);

    m_layout->addStretch();

    updateAppearance(palette());
    QtFileMgrStyle::observeApplicationPalette(this,
            [this](const QPalette &appearancePalette) {
        updateAppearance(appearancePalette);
    });

    setUrl(m_url);
    setNavigationState(false, false);
}

QtFileMgrBreadcrumbBar::~QtFileMgrBreadcrumbBar()
{
    clearBreadcrumbs();
}

void QtFileMgrBreadcrumbBar::updateAppearance(QPalette appearancePalette)
{
    const auto c = QtFileMgrStyle::colors(appearancePalette);
    setPalette(appearancePalette);
    m_edit->setPalette(appearancePalette);
    setStyleSheet(QStringLiteral(
        "QtFileMgrBreadcrumbBar{background:%1;border:1px solid %2;border-radius:7px;}"
        "QtFileMgrBreadcrumbBar QToolButton{color:%3;background:transparent;"
        "border:0;border-radius:5px;padding:0 8px;min-height:30px;}"
        "QtFileMgrBreadcrumbBar QToolButton:hover{background:%4;}"
        "QtFileMgrBreadcrumbBar QToolButton:pressed{background:%5;}"
        "QtFileMgrBreadcrumbBar QToolButton[currentPath=\"true\"]{"
        "background:%4;font-weight:600;}"
        "QtFileMgrBreadcrumbBar QToolButton:disabled{color:%6;}"
        "QtFileMgrBreadcrumbBar QLineEdit{background:palette(base);color:palette(text);"
        "border:0;padding:0 8px;selection-background-color:%5;selection-color:%3;}"
        "QFrame#breadcrumbDivider{background:%2;border:0;margin:5px 0;}"
        "QLabel#breadcrumbReadOnly{color:%6;padding:0 6px;font-size:10px;}")
        .arg(c.base.name(), c.border.name(), c.text.name(), c.hover.name(),
             c.selection.name(), c.muted.name()));
    m_backBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Back, appearancePalette));
    m_forwardBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Forward, appearancePalette));
    m_upBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Up, appearancePalette));
    QPalette chevronPalette = appearancePalette;
    chevronPalette.setColor(QPalette::WindowText, c.muted);
    for (QLabel *chevron : qAsConst(m_segmentChevrons)) {
        if (chevron)
            chevron->setPalette(chevronPalette);
    }
}

void QtFileMgrBreadcrumbBar::clearBreadcrumbs()
{
    qDeleteAll(m_breadcrumbWidgets);
    m_breadcrumbWidgets.clear();
    m_segmentButtons.clear();
    m_segmentChevrons.clear();
    m_overflowButton = nullptr;
}

QUrl QtFileMgrBreadcrumbBar::url() const { return m_url; }
QString QtFileMgrBreadcrumbBar::text() const { return displayTextForUrl(m_url); }
void QtFileMgrBreadcrumbBar::setPlaceholderText(const QString &text) { m_edit->setPlaceholderText(text); }

void QtFileMgrBreadcrumbBar::setNavButtonsVisible(bool visible)
{
    if (m_backBtn)    m_backBtn->setVisible(visible);
    if (m_forwardBtn) m_forwardBtn->setVisible(visible);
    if (m_upBtn)      m_upBtn->setVisible(visible);
    // 分隔线也隐藏
    for (auto *child : findChildren<QFrame *>()) {
        if (child->frameShape() == QFrame::VLine) {
            child->setVisible(visible);
            break;
        }
    }
    updateBreadcrumbVisibility();
}

void QtFileMgrBreadcrumbBar::setUrl(const QUrl &url)
{
    setUrl(url, false);
}

void QtFileMgrBreadcrumbBar::setUrl(const QUrl &url, bool addHistory)
{
    Q_UNUSED(addHistory);
    QUrl normalizedUrl = (url.scheme() == QStringLiteral("remote"))
        ? remoteUrlFromPath(url.toString())
        : url;

    if (m_url == normalizedUrl) {
        updateReadOnlyBadge();
        if (m_editable)
            m_edit->setText(displayTextForUrl(m_url));
        else if (m_breadcrumbWidgets.isEmpty())
            rebuildBreadcrumb();
        return;
    }

    m_url = normalizedUrl;
    updateDisplayRootForUrl();
    updateReadOnlyBadge();
    if (!m_editable) rebuildBreadcrumb();
    else m_edit->setText(displayTextForUrl(m_url));
}

void QtFileMgrBreadcrumbBar::goBack()
{
    emit backRequested();
}
void QtFileMgrBreadcrumbBar::goForward()
{
    emit forwardRequested();
}
void QtFileMgrBreadcrumbBar::goUp()
{
    if (m_io && m_io->isRemote() && m_io->acceptsUrl(m_url)) {
        const QString path = QDir::cleanPath(m_io->pathFromUrl(m_url));
        if (path.isEmpty() || path == QStringLiteral(".") || path == QStringLiteral("/"))
            return;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parent = slash > 0 ? path.left(slash)
            : (path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString());
        navigateToUrl(m_io->urlForPath(parent));
        return;
    }
    if (m_url.scheme() == QStringLiteral("ssh")) {
        QString path = m_url.path();
        if (path.endsWith('/')) path.chop(1);
        if (path == QStringLiteral("/")) return;
        int idx = path.lastIndexOf('/');
        QUrl up(m_url);
        up.setPath(idx > 0 ? path.left(idx) : QStringLiteral("/"));
        navigateToUrl(up);
    } else if (m_url.scheme() == QStringLiteral("remote")) {
        QString path = normalizedRemotePath(m_url.path());
        if (path.isEmpty())
            return;
        int idx = path.lastIndexOf('/');
        navigateToUrl(idx > 0 ? remoteUrlFromPath(path.left(idx)) : remoteUrlFromPath(QString()));
    } else {
        // toLocalFile() 为空时 QDir("") 即进程 CWD,cdUp 会导航到
        // 无关目录;先判空 (DLG-P2-9)
        const QString localPath = m_url.toLocalFile();
        if (localPath.isEmpty()) {
            qWarning("[BreadcrumbBar] goUp: skipped non-local URL (url=\"%s\")",
                     qPrintable(m_url.toString(QUrl::RemovePassword)));
            return;
        }
        QDir d(localPath);
        if (d.cdUp()) navigateToUrl(QUrl::fromLocalFile(d.absolutePath()));
    }
}
void QtFileMgrBreadcrumbBar::goHome()
{
    navigateToUrl(m_io && m_io->isRemote() ? m_io->urlForPath(m_io->rootPath())
                                          : QUrl::fromLocalFile(QDir::homePath()));
}

void QtFileMgrBreadcrumbBar::setDisplayRoot(const QString &path)
{
    m_displayRoot = normalizedDisplayRoot(path);
}

void QtFileMgrBreadcrumbBar::setNavigationState(bool canGoBack, bool canGoForward)
{
    if (m_backBtn)
        m_backBtn->setEnabled(canGoBack);
    if (m_forwardBtn)
        m_forwardBtn->setEnabled(canGoForward);
}

void QtFileMgrBreadcrumbBar::setPlaceInfo(const QString &name, const QString &rootPath)
{
    m_placeName = name;
    setDisplayRoot(name.isEmpty() ? QString() : rootPath);
    updateDisplayRootForUrl();
    rebuildBreadcrumb();
}

void QtFileMgrBreadcrumbBar::rebuildBreadcrumb()
{
    clearBreadcrumbs();

    const auto insertBeforeReadOnlyBadge = [this](QWidget *widget) {
        const int badgeIndex = m_layout->indexOf(m_readOnlyBadge);
        m_layout->insertWidget(badgeIndex >= 0 ? badgeIndex : m_layout->count() - 1,
                               widget);
    };

    m_overflowButton = new QToolButton(this);
    m_overflowButton->setObjectName(QStringLiteral("breadcrumbOverflow"));
    m_overflowButton->setText(QStringLiteral("..."));
    m_overflowButton->setAutoRaise(true);
    m_overflowButton->setPopupMode(QToolButton::InstantPopup);
    m_overflowButton->setToolTip(QStringLiteral("显示已折叠的上级路径"));
    m_overflowButton->setMenu(new QMenu(m_overflowButton));
    m_overflowButton->hide();
    m_breadcrumbWidgets.append(m_overflowButton);
    insertBeforeReadOnlyBadge(m_overflowButton);

    const bool isRemote = !m_url.scheme().isEmpty() && !m_url.isLocalFile();
    const bool useBackend = isRemote && m_io && m_io->isRemote()
        && m_io->acceptsUrl(m_url);
    const bool relativeRemote = m_url.scheme() == QStringLiteral("remote");
    const QString path = useBackend ? m_io->pathFromUrl(m_url)
        : (isRemote ? (relativeRemote ? normalizedRemotePath(m_url.path()) : m_url.path())
                    : m_url.toLocalFile());
    const bool absoluteBackendPath = path.startsWith(QLatin1Char('/'));
    const bool hasBackendDisplayRoot = isRemote && !m_displayRoot.isEmpty()
        && (path == m_displayRoot || m_displayRoot == QStringLiteral("/")
            || path.startsWith(m_displayRoot + QLatin1Char('/')));
    QStringList parts;

    if (isRemote) {
        if (path.isEmpty() || path == QStringLiteral("/")) {
            parts << QStringLiteral("/");
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
#else
            parts = path.split(QLatin1Char('/'),
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                Qt::SkipEmptyParts
#else
                QString::SkipEmptyParts
#endif
            );
#endif
        }
    } else {
        QDir dir(path);
        // 前缀匹配必须带 '/' 边界,否则 "/home/user2" 会误判进
        // displayRoot="/home/user" 的分支,面包屑错乱并导航错目录 (DLG-P1-2)
        const bool hasRoot = !m_displayRoot.isEmpty()
            && (path == m_displayRoot
                || path.startsWith(m_displayRoot + QLatin1Char('/')));
        if (hasRoot) {
            QStringList dirParts;
            QDir d(path);
            do {
                dirParts.prepend(d.dirName());
                if (d.absolutePath() == m_displayRoot) break;
            } while (d.cdUp());
            parts = dirParts;
        } else {
            do { parts.prepend(dir.dirName()); } while (dir.cdUp() && !dir.isRoot());
            parts.prepend(QStringLiteral("/"));
        }
    }

    if (hasBackendDisplayRoot) {
        parts = path.mid(m_displayRoot.size()).split(QLatin1Char('/'));
        parts.removeAll(QString());
        const QString rootLabel = m_displayRoot == QStringLiteral("/")
            ? QStringLiteral("/") : m_displayRoot.section(QLatin1Char('/'), -1);
        parts.prepend(rootLabel);
    }

    QString acc;
    for (int i = 0; i < parts.size(); ++i) {
        if (isRemote) {
            if (hasBackendDisplayRoot) {
                acc = m_displayRoot;
                const QString suffix = parts.mid(1, i).join(QLatin1Char('/'));
                if (!suffix.isEmpty())
                    acc += (acc.endsWith(QLatin1Char('/')) ? QString() : QStringLiteral("/")) + suffix;
            } else if (parts.size() == 1 && parts[0] == QStringLiteral("/")) {
                acc = absoluteBackendPath ? QStringLiteral("/") : QString();
            } else {
                acc = (absoluteBackendPath ? QStringLiteral("/") : QString())
                    + parts.mid(0, i + 1).join(QLatin1Char('/'));
            }
        } else if (i == 0 && parts[i] == QStringLiteral("/")) {
            acc = QStringLiteral("/");
        } else if (i == 0 && !m_displayRoot.isEmpty()
                   && (path == m_displayRoot
                       || path.startsWith(m_displayRoot + QLatin1Char('/')))) {
            acc = m_displayRoot;
        } else {
            acc = acc + QStringLiteral("/") + parts[i];
            if (acc.startsWith(QStringLiteral("//"))) acc = acc.mid(1);
        }

        QLabel *chevron = nullptr;
        if (i > 0 || isRemote || !m_displayRoot.isEmpty()) {
            chevron = new QLabel(QStringLiteral("›"), this);
            chevron->setFixedWidth(14);
            chevron->setAlignment(Qt::AlignCenter);
            QPalette chevronPalette = chevron->palette();
            chevronPalette.setColor(QPalette::WindowText,
                                     QtFileMgrStyle::colors(palette()).muted);
            chevron->setPalette(chevronPalette);
            m_breadcrumbWidgets.append(chevron);
            insertBeforeReadOnlyBadge(chevron);
        }
        m_segmentChevrons.append(chevron);

        QString label = parts[i];
        // 第一段: 用 Place 名称替代目录名 (如 /home/user → "Home")
        if (i == 0 && !m_placeName.isEmpty() && !m_displayRoot.isEmpty()) {
            label = m_placeName;
        } else if (i == 0 && !isRemote && parts[i] == QStringLiteral("/")) {
            label = QStringLiteral(" / ");
        }
        QToolButton *btn = new QToolButton(this);
        btn->setText(btn->fontMetrics().elidedText(
            label, Qt::ElideMiddle, 180));
        btn->setAutoRaise(true);
        btn->setMinimumWidth(0);
        btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        btn->setProperty("currentPath", i == parts.size() - 1);
        QString storedPath;
        if (useBackend) {
            storedPath = m_io->urlForPath(acc).toString(QUrl::RemovePassword);
        } else if (isRemote && !relativeRemote) {
            QUrl segmentUrl = m_url;
            segmentUrl.setPath(acc);
            storedPath = segmentUrl.toString();
        } else if (isRemote) {
            storedPath = remoteUrlFromPath(acc).toString();
        } else {
            storedPath = acc;
        }
        btn->setToolTip(storedPath);
        btn->setProperty("path", storedPath);
        btn->setProperty("fullLabel", label);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QToolButton::clicked, this, &QtFileMgrBreadcrumbBar::onSegmentClicked);
        m_breadcrumbWidgets.append(btn);
        m_segmentButtons.append(btn);
        insertBeforeReadOnlyBadge(btn);
    }
    updateBreadcrumbVisibility();
}

void QtFileMgrBreadcrumbBar::updateBreadcrumbVisibility()
{
    if (m_editable || !m_overflowButton || m_segmentButtons.isEmpty())
        return;

    for (int i = 0; i < m_segmentButtons.size(); ++i) {
        QToolButton *button = m_segmentButtons.at(i);
        button->setMaximumWidth(QWIDGETSIZE_MAX);
        button->setText(button->fontMetrics().elidedText(
            button->property("fullLabel").toString(), Qt::ElideMiddle, 180));
        button->show();
        if (m_segmentChevrons.value(i))
            m_segmentChevrons.at(i)->show();
    }
    m_overflowButton->hide();
    m_overflowButton->menu()->clear();

    QSet<QWidget *> pathWidgets;
    pathWidgets.insert(m_overflowButton);
    for (QToolButton *button : qAsConst(m_segmentButtons))
        pathWidgets.insert(button);
    for (QLabel *chevron : qAsConst(m_segmentChevrons)) {
        if (chevron)
            pathWidgets.insert(chevron);
    }

    const QMargins margins = m_layout->contentsMargins();
    int fixedWidth = margins.left() + margins.right();
    for (int i = 0; i < m_layout->count(); ++i) {
        QWidget *widget = m_layout->itemAt(i)->widget();
        if (widget && widget->isVisible() && !pathWidgets.contains(widget))
            fixedWidth += qMin(widget->sizeHint().width(), widget->maximumWidth());
    }
    const int availableWidth = qMax(0, width() - fixedWidth);
    int pathWidth = 0;
    for (int i = 0; i < m_segmentButtons.size(); ++i) {
        pathWidth += m_segmentButtons.at(i)->sizeHint().width();
        if (m_segmentChevrons.value(i))
            pathWidth += m_segmentChevrons.at(i)->sizeHint().width();
    }
    if (pathWidth <= availableWidth)
        return;

    // Keep the current directory legible even when a split pane leaves only a
    // small address area. Ancestor navigation still lives in the overflow menu.
    QToolButton *lastButton = m_segmentButtons.constLast();
    const int overflowWidth = m_segmentButtons.size() > 1
        ? m_overflowButton->sizeHint().width() : 0;
    const QLabel *lastChevron = m_segmentChevrons.constLast();
    const int lastWidth = qMax(16, availableWidth - overflowWidth
        - (lastChevron ? lastChevron->sizeHint().width() : 0));
    lastButton->setMaximumWidth(lastWidth);
    lastButton->setText(lastButton->fontMetrics().elidedText(
        lastButton->property("fullLabel").toString(), Qt::ElideMiddle,
        qMax(0, lastWidth - 16)));
    if (m_segmentButtons.size() == 1)
        return;

    int usedWidth = m_overflowButton->sizeHint().width();
    int firstVisible = m_segmentButtons.size() - 1;
    for (int i = m_segmentButtons.size() - 1; i >= 0; --i) {
        int segmentWidth = qMin(m_segmentButtons.at(i)->sizeHint().width(),
                                m_segmentButtons.at(i)->maximumWidth());
        if (m_segmentChevrons.value(i))
            segmentWidth += m_segmentChevrons.at(i)->sizeHint().width();
        if (i != m_segmentButtons.size() - 1
            && usedWidth + segmentWidth > availableWidth) {
            break;
        }
        usedWidth += segmentWidth;
        firstVisible = i;
    }

    for (int i = 0; i < firstVisible; ++i) {
        m_segmentButtons.at(i)->hide();
        if (m_segmentChevrons.value(i))
            m_segmentChevrons.at(i)->hide();
        QAction *action = m_overflowButton->menu()->addAction(
            m_segmentButtons.at(i)->property("fullLabel").toString());
        action->setData(m_segmentButtons.at(i)->property("path"));
        connect(action, &QAction::triggered, this, [this, action]() {
            navigateToStoredPath(action->data().toString());
        });
    }
    m_overflowButton->setVisible(firstVisible > 0);
}

void QtFileMgrBreadcrumbBar::setEditableMode(bool editable)
{
    m_editable = editable;
    for (QWidget *w : qAsConst(m_breadcrumbWidgets))
        w->setVisible(!editable);
    if (editable) {
        m_completionRequestText.clear();
        m_completionCacheKey.clear();
        m_completionModel->clear();
        m_edit->setText(displayTextForUrl(m_url));
        m_edit->setStyleSheet(QString());
        m_edit->setToolTip(QString());
        m_edit->setVisible(true);
        m_edit->setFocus();
        m_edit->selectAll();
    } else {
        m_completionRequestText.clear();
        if (m_completer)
            m_completer->popup()->hide();
        m_edit->setVisible(false);
        rebuildBreadcrumb();  // 恢复面包屑显示
    }
}

// 参照 Dolphin: 双击 → 编辑, Ctrl+L → 编辑, 点击空白 → 编辑
void QtFileMgrBreadcrumbBar::mousePressEvent(QMouseEvent *event)
{
    if (!m_editable && event->button() == Qt::LeftButton) {
        const QPoint eventPos =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            event->position().toPoint();
#else
            event->pos();
#endif
        QWidget *clicked = childAt(eventPos);
        if (!clicked) {
            setEditableMode(true);
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void QtFileMgrBreadcrumbBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_editable) {
        const QPoint eventPos =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            event->position().toPoint();
#else
            event->pos();
#endif
        QWidget *clicked = childAt(eventPos);
        // 双击任何位置进入编辑 (空白/路径段/箭头)
        if (!clicked || qobject_cast<QToolButton *>(clicked)
            || qobject_cast<QLabel *>(clicked)) {
            setEditableMode(true);
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void QtFileMgrBreadcrumbBar::keyPressEvent(QKeyEvent *event)
{
    if (!m_editable && event->key() == Qt::Key_L
        && event->modifiers() == Qt::ControlModifier) {
        setEditableMode(true);
        return;
    }
    if (m_editable && event->key() == Qt::Key_Escape) {
        m_edit->setText(displayTextForUrl(m_url));
        setEditableMode(false);
        emit focusFileViewRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void QtFileMgrBreadcrumbBar::onSegmentClicked()
{
    QToolButton *btn = qobject_cast<QToolButton *>(sender());
    if (!btn) return;
    QString path = btn->property("path").toString();
    if (path.isEmpty()) return;
    navigateToStoredPath(path);
}

void QtFileMgrBreadcrumbBar::navigateToStoredPath(const QString &path)
{
    const QUrl target(path);
    navigateToUrl(!target.scheme().isEmpty() ? target : QUrl::fromLocalFile(path));
}

void QtFileMgrBreadcrumbBar::setIO(QtFileMgrIO *io)
{
    m_io = io;
    updateReadOnlyBadge();
}

void QtFileMgrBreadcrumbBar::onEditReturnPressed()
{
    QString path = m_edit->text().trimmed();
    if (path.isEmpty()) return;

    auto rejectPath = [this](const QString &message) {
        qWarning("[Breadcrumb] edit rejected: %s", qPrintable(message));
        m_edit->setStyleSheet(QStringLiteral("QLineEdit{color:#b00020;}"));
        m_edit->setToolTip(message);
        m_edit->setFocus();
        m_edit->selectAll();
        emit statusMessage(message, QStringLiteral("warning"));
    };

    const QUrl typedUrl(path);
    const bool typedRemote = !typedUrl.scheme().isEmpty() && !typedUrl.isLocalFile();
    const bool isRemote = typedRemote
        || (m_io && m_io->isRemote());
    QUrl targetUrl;
    if (isRemote) {
        if (typedUrl.scheme() == QStringLiteral("ssh")
            && typedUrl.host().isEmpty()) {
            // ssh:///path 没有主机,后续连接流程必然失败 (DLG-P2-10)
            rejectPath(QStringLiteral("SSH 地址缺少主机名: %1").arg(path));
            return;
        }
        if (typedRemote && (!m_io || !m_io->acceptsUrl(typedUrl))) {
            targetUrl = typedUrl;
        } else if (!m_io || !m_io->isRemote()) {
            rejectPath(QStringLiteral("当前后端不支持远端路径: %1").arg(path));
            return;
        } else {
            QString target;
            if (typedRemote) {
                target = m_io->pathFromUrl(typedUrl);
            } else {
                target = typedUrl.isLocalFile() ? typedUrl.toLocalFile() : path;
                if (QDir::isRelativePath(target)) {
                    const QString base = m_io->pathFromUrl(m_url);
                    target = QDir(base).filePath(target);
                }
            }
            targetUrl = m_io->urlForPath(QDir::cleanPath(target));
        }
    } else {
        if (typedRemote) {
            rejectPath(QStringLiteral("当前后端不支持远端路径: %1").arg(path));
            return;
        }
        const QString baseDir = m_url.isLocalFile() ? m_url.toLocalFile() : QDir::homePath();
        QFileInfo fi(localPathFromEditText(path, baseDir));
        if (!fi.exists()) {
            rejectPath(QStringLiteral("路径不存在: %1").arg(fi.absoluteFilePath()));
            return;
        }
        if (fi.isDir()) {
            targetUrl = QUrl::fromLocalFile(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            targetUrl = QUrl::fromLocalFile(fi.dir().absolutePath());
            emit statusMessage(QStringLiteral("输入的是文件，已打开所在目录。"),
                               QStringLiteral("info"));
        } else {
            rejectPath(QStringLiteral("不支持的路径类型: %1").arg(fi.absoluteFilePath()));
            return;
        }
    }

    m_edit->setStyleSheet(QString());
    setEditableMode(false);
    navigateToUrl(targetUrl);
    emit focusFileViewRequested();
}

void QtFileMgrBreadcrumbBar::onEditTextEdited(const QString &text)
{
    requestCompletion(text);
}

QString QtFileMgrBreadcrumbBar::completionDirectory(const QString &text) const
{
    const bool localUrl = m_url.isLocalFile() || m_url.scheme().isEmpty();
    const QUrl typedUrl(text.trimmed());
    if (!localUrl || (!typedUrl.scheme().isEmpty() && !typedUrl.isLocalFile()))
        return QString();

    const QString trimmed = QDir::fromNativeSeparators(text.trimmed());
    if (trimmed.isEmpty())
        return QString();

    const QString baseDir = m_url.toLocalFile().isEmpty()
        ? m_url.path() : m_url.toLocalFile();
    if (trimmed.endsWith(QLatin1Char('/')))
        return localPathFromEditText(trimmed, baseDir);

    const int separator = trimmed.lastIndexOf(QLatin1Char('/'));
    if (separator < 0)
        return QDir(baseDir).absolutePath();
    return localPathFromEditText(trimmed.left(separator + 1), baseDir);
}

void QtFileMgrBreadcrumbBar::requestCompletion(const QString &text)
{
    m_completionRequestText = text;
    const QString directory = completionDirectory(text);
    if (!m_completer || !m_completionModel || directory.isEmpty()
        || !QFileInfo(directory).isDir()) {
        if (m_completer)
            m_completer->popup()->hide();
        return;
    }

    const QString normalizedText =
        QDir::fromNativeSeparators(text.trimmed());
    const int separator = normalizedText.lastIndexOf(QLatin1Char('/'));
    const QString typedDirectory =
        separator >= 0 ? normalizedText.left(separator + 1) : QString();
    const QString typedName = normalizedText.mid(separator + 1);
    const bool includeHidden = typedName.startsWith(QLatin1Char('.'));
    const QString cacheKey = directory + QLatin1Char('\n') + typedDirectory
        + QLatin1Char('\n') + (includeHidden ? QLatin1Char('1') : QLatin1Char('0'));
    if (cacheKey != m_completionCacheKey) {
        m_completionCacheKey = cacheKey;
        m_completionModel->clear();
        QDir::Filters filters = QDir::AllDirs | QDir::NoDotAndDotDot;
        if (includeHidden)
            filters |= QDir::Hidden;
        const QFileInfoList entries = QDir(directory).entryInfoList(
            filters, QDir::Name | QDir::IgnoreCase);
        // GUI 线程同步枚举,超大目录会卡住输入;截断到 500 条,
        // 补全本来就是前缀过滤,超出部分用户继续打字即可命中 (DLG-P2-1)
        const int kCompletionLimit = 500;
        if (entries.size() > kCompletionLimit) {
            qWarning("[Breadcrumb] completion: directory enumeration truncated (directory=\"%s\", total=%d, limit=%d)",
                     qPrintable(directory), entries.size(), kCompletionLimit);
        }
        const QIcon folderIcon = QIcon::fromTheme(
            QStringLiteral("folder"), style()->standardIcon(QStyle::SP_DirIcon));
        for (int i = 0; i < qMin(entries.size(), kCompletionLimit); ++i) {
            const QFileInfo &entry = entries.at(i);
            auto *item = new QStandardItem(entry.fileName());
            item->setIcon(folderIcon);
            item->setData(typedDirectory + entry.fileName(), Qt::UserRole);
            m_completionModel->appendRow(item);
        }
    }

    // 候选项已同步建立，延迟到当前按键事件结束后再显示，避免 QLineEdit
    // 内部补全处理用旧前缀覆盖本次请求。
    QTimer::singleShot(0, this, [this, text]() {
        if (!m_editable || m_edit->text() != text)
            return;
        const QString normalized =
            QDir::fromNativeSeparators(text.trimmed());
        m_completer->setCompletionPrefix(
            normalized.mid(normalized.lastIndexOf(QLatin1Char('/')) + 1));
        m_completer->complete();
    });
}

// 编辑框失焦 → 退出编辑模式
bool QtFileMgrBreadcrumbBar::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_edit && event->type() == QEvent::KeyPress && m_editable) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            m_edit->setText(displayTextForUrl(m_url));
            setEditableMode(false);
            emit focusFileViewRequested();
            return true;
        }
    }
    if (obj == m_edit && event->type() == QEvent::FocusOut && m_editable) {
        // QCompleter 弹出下拉列表时 QLineEdit 会短暂失焦, 不退出编辑模式
        if (m_edit->completer() && m_edit->completer()->popup()->isVisible())
            return false;
        setEditableMode(false);
    }
    return QWidget::eventFilter(obj, event);
}

void QtFileMgrBreadcrumbBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBreadcrumbVisibility();
}

void QtFileMgrBreadcrumbBar::navigateToUrl(const QUrl &url)
{
    QUrl oldUrl = m_url;
    setUrl(url);
    if (m_url != oldUrl)
        emit urlChanged(m_url);
}

QString QtFileMgrBreadcrumbBar::displayTextForUrl(const QUrl &url) const
{
    if (url.scheme() == QStringLiteral("remote"))
        return remoteUrlFromPath(url.toString()).toString();
    if (!url.scheme().isEmpty() && !url.isLocalFile())
        return url.toString(QUrl::RemovePassword);
    return url.toLocalFile();
}

QString QtFileMgrBreadcrumbBar::normalizedDisplayRoot(const QString &path) const
{
    if (path.isEmpty()) return QString();
    if (m_io && m_io->isRemote()) {
        const QUrl url(path);
        const QString normalized = QDir::cleanPath(
            m_io->acceptsUrl(url) ? m_io->pathFromUrl(url) : path);
        return normalized == QStringLiteral(".") ? QString() : normalized;
    }
    if (path.startsWith(QStringLiteral("remote://")) || m_url.scheme() == QStringLiteral("remote"))
        return normalizedRemotePath(path);
    if (path.startsWith(QStringLiteral("ssh://")) || m_url.scheme() == QStringLiteral("ssh")) {
        const QUrl url(path);
        return QDir::cleanPath(url.scheme().isEmpty() ? path : url.path());
    }
    return QDir(path).absolutePath();
}

QString QtFileMgrBreadcrumbBar::currentComparablePath() const
{
    if (m_io && m_io->isRemote() && m_io->acceptsUrl(m_url))
        return QDir::cleanPath(m_io->pathFromUrl(m_url));
    if (m_url.scheme() == QStringLiteral("remote"))
        return normalizedRemotePath(m_url.path());
    if (m_url.scheme() == QStringLiteral("ssh"))
        return QDir::cleanPath(m_url.path());
    return m_url.toLocalFile();
}

void QtFileMgrBreadcrumbBar::updateDisplayRootForUrl()
{
    if (m_displayRoot.isEmpty())
        return;

    const QString path = currentComparablePath();
    const bool insideRoot = path == m_displayRoot || m_displayRoot == QStringLiteral("/")
        || path.startsWith(m_displayRoot + QLatin1Char('/'));
    if (!insideRoot) {
        m_displayRoot.clear();
        m_placeName.clear();
    }
}

void QtFileMgrBreadcrumbBar::updateReadOnlyBadge()
{
    if (!m_readOnlyBadge)
        return;

    const bool remote = m_io && m_io->isRemote();
    if (remote) {
        const QString path = m_io->pathFromUrl(m_url);
        const bool writable = m_io->isWritable(path);
        m_readOnlyBadge->setVisible(!writable);
        updateBreadcrumbVisibility();
        return;
    }

    const QString path = m_url.toLocalFile();
    bool isDir = QFileInfo(path).isDir();
    bool writable = (m_io && !m_io->isRemote()) ? m_io->isWritable(path) : QFileInfo(path).isWritable();
    m_readOnlyBadge->setVisible(isDir && !writable);
    updateBreadcrumbVisibility();
}
