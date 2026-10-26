// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// 参考 Dolphin FilterBar (filterbar/filterbar.cpp)
#include "QtFileMgrFilterBar.h"
#include "QtFileMgrStyle.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QComboBox>
#include <QAction>
#include <QKeyEvent>
#include <QShowEvent>
#include <QRegularExpression>
#include <QStyle>
#include <QIcon>
#include <QApplication>
#include <QGuiApplication>

QtFileMgrFilterBar::QtFileMgrFilterBar(QWidget *parent)
    : QWidget(parent)
    , m_edit(new QLineEdit(this))
    , m_lockBtn(new QToolButton(this))
    , m_caseBtn(new QToolButton(this))
    , m_modeCombo(new QComboBox(this))
    , m_invalidAction(nullptr)
    , m_editBasePalette(m_edit->palette())
{
    auto *lay = new QHBoxLayout(this);
    setObjectName(QStringLiteral("qtfilemgrFilterBar"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAttribute(Qt::WA_StyledBackground);
    lay->setContentsMargins(8, 7, 8, 7);
    lay->setSpacing(6);

    // Lock
    m_lockBtn->setAutoRaise(true);
    m_lockBtn->setCheckable(true);
    m_lockBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Unlock, palette()));
    m_lockBtn->setToolTip(QStringLiteral("切换目录时保持过滤条件"));
    connect(m_lockBtn, &QToolButton::toggled, this, &QtFileMgrFilterBar::onLockToggled);
    lay->addWidget(m_lockBtn);

    // Filter input
    m_edit->setClearButtonEnabled(true);
    m_edit->setPlaceholderText(QStringLiteral("过滤..."));
    m_edit->setLayoutDirection(Qt::LeftToRight);
    m_edit->setMinimumWidth(0);
    m_edit->setMinimumHeight(30);
    m_edit->installEventFilter(this);
    setFocusProxy(m_edit);
    connect(m_edit, &QLineEdit::textChanged, this, &QtFileMgrFilterBar::filterChanged);
    connect(m_edit, &QLineEdit::textChanged, this, &QtFileMgrFilterBar::updateInvalidPatternView);
    lay->addWidget(m_edit, 1);

    // Case sensitivity — 参照 Dolphin
    m_caseBtn->setAutoRaise(true);
    m_caseBtn->setCheckable(true);
    m_caseBtn->setText(QStringLiteral("Aa"));
    m_caseBtn->setToolTip(QStringLiteral("区分大小写"));
    connect(m_caseBtn, &QToolButton::toggled, this, &QtFileMgrFilterBar::updateInvalidPatternView);
    connect(m_caseBtn, &QToolButton::toggled, this, &QtFileMgrFilterBar::filterOptionsChanged);
    lay->addWidget(m_caseBtn);

    // Filter mode — 参照 Dolphin: Plain/Glob/Regex
    m_modeCombo->addItem(QStringLiteral("纯文本"), PlainText);
    m_modeCombo->addItem(QStringLiteral("通配符"), Glob);
    m_modeCombo->addItem(QStringLiteral("正则"), Regex);
    m_modeCombo->setMinimumHeight(30);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QtFileMgrFilterBar::updateInvalidPatternView);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &QtFileMgrFilterBar::filterOptionsChanged);
    lay->addWidget(m_modeCombo);

    // Close button — 参照 Dolphin
    m_closeBtn = new QToolButton(this);
    QToolButton *closeBtn = m_closeBtn;
    closeBtn->setAutoRaise(true);
    closeBtn->setObjectName(QStringLiteral("filterClose"));
    closeBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Close, palette()));
    closeBtn->setToolTip(QStringLiteral("隐藏过滤栏"));
    connect(closeBtn, &QToolButton::clicked, this, &QtFileMgrFilterBar::closeRequest);
    lay->addWidget(closeBtn);

    // Invalid pattern indicator (error marker inside QLineEdit)
    m_invalidAction = new QAction(
        style()->standardIcon(QStyle::SP_MessageBoxWarning), QString(), m_edit);
    m_edit->addAction(m_invalidAction, QLineEdit::LeadingPosition);
    m_invalidAction->setVisible(false);

    for (QToolButton *button : {m_lockBtn, m_caseBtn, closeBtn}) {
        button->setFixedSize(30, 30);
        button->setIconSize(QSize(16, 16));
    }
    updateAppearance(palette());
    QtFileMgrStyle::observeApplicationPalette(this,
            [this](const QPalette &appearancePalette) {
        updateAppearance(appearancePalette);
    });
}

void QtFileMgrFilterBar::updateAppearance(QPalette appearancePalette)
{
    const auto c = QtFileMgrStyle::colors(appearancePalette);
    setPalette(appearancePalette);
    m_editBasePalette = appearancePalette;
    m_edit->setPalette(m_editBasePalette);
    setStyleSheet(QStringLiteral(
        "QtFileMgrFilterBar{background:%1;border-top:1px solid %2;}"
        "QtFileMgrFilterBar QLineEdit{background:palette(base);color:palette(text);"
        "border:1px solid %2;border-radius:6px;padding:0 9px;"
        "selection-background-color:%5;selection-color:%4;}"
        "QtFileMgrFilterBar QLineEdit:focus{border-color:%6;}"
        "QtFileMgrFilterBar QComboBox{background:%3;color:%4;"
        "border:1px solid %2;border-radius:6px;padding:0 8px;}"
        "QtFileMgrFilterBar QComboBox::drop-down{border:0;width:20px;}"
        "QtFileMgrFilterBar QToolButton{color:%4;background:transparent;"
        "border:0;border-radius:5px;padding:0;}"
        "QtFileMgrFilterBar QToolButton:hover{background:%7;}"
        "QtFileMgrFilterBar QToolButton:checked{background:%5;color:%6;}")
        .arg(c.surface.name(), c.border.name(), c.base.name(), c.text.name(),
             c.selection.name(), c.accent.name(), c.hover.name()));
    m_closeBtn->setIcon(QtFileMgrStyle::toolIcon(QtFileMgrStyle::Symbol::Close, appearancePalette));
    m_lockBtn->setIcon(QtFileMgrStyle::toolIcon(m_lockBtn->isChecked()
        ? QtFileMgrStyle::Symbol::Lock : QtFileMgrStyle::Symbol::Unlock, appearancePalette));
    updateInvalidPatternView();
}

QString QtFileMgrFilterBar::text() const               { return m_edit->text(); }
bool QtFileMgrFilterBar::isLocked() const              { return m_lockBtn->isChecked(); }
bool QtFileMgrFilterBar::isCaseSensitive() const       { return m_caseBtn->isChecked(); }

void QtFileMgrFilterBar::setCaseSensitive(bool on)
{
    m_caseBtn->setChecked(on);
}

QtFileMgrFilterBar::FilterMode QtFileMgrFilterBar::filterMode() const
{
    return static_cast<FilterMode>(m_modeCombo->currentData().toInt());
}

void QtFileMgrFilterBar::setFilterMode(FilterMode mode)
{
    int idx = m_modeCombo->findData(static_cast<int>(mode));
    if (idx >= 0)
        m_modeCombo->setCurrentIndex(idx);
}

void QtFileMgrFilterBar::clear()
{
    m_edit->clear();
}

// 参照 Dolphin: 切换目录时若未锁定则清除过滤词
void QtFileMgrFilterBar::clearIfUnlocked()
{
    if (!m_lockBtn->isChecked())
        clear();
}

// 参照 Dolphin: 解锁时清除过滤词
void QtFileMgrFilterBar::onLockToggled(bool on)
{
    m_lockBtn->setIcon(QtFileMgrStyle::toolIcon(on
        ? QtFileMgrStyle::Symbol::Lock : QtFileMgrStyle::Symbol::Unlock, palette()));
    if (!on) clear();
}

// 参照 Dolphin: Glob/Regex 模式时验证模式合法性
void QtFileMgrFilterBar::updateInvalidPatternView()
{
    FilterMode mode = filterMode();
    if (mode == PlainText) {
        m_edit->setPalette(m_editBasePalette);
        m_invalidAction->setVisible(false);
        return;
    }
    QString pattern = m_edit->text();
    if (pattern.isEmpty()) {
        m_edit->setPalette(m_editBasePalette);
        m_invalidAction->setVisible(false);
        return;
    }
    QRegularExpression re;
    if (mode == Regex)
        re.setPattern(pattern);
    else
        re.setPattern(QRegularExpression::wildcardToRegularExpression(pattern));
    if (m_caseBtn->isChecked())
        re.setPatternOptions(QRegularExpression::NoPatternOption);
    else
        re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);

    if (re.isValid()) {
        m_edit->setPalette(m_editBasePalette);
        m_invalidAction->setVisible(false);
    } else {
        QPalette p = m_editBasePalette;
        const QColor base = QtFileMgrStyle::colors(m_editBasePalette).base;
        const QColor error(208, 66, 83);
        p.setColor(QPalette::Base,
                   QColor((base.red() * 4 + error.red()) / 5,
                          (base.green() * 4 + error.green()) / 5,
                          (base.blue() * 4 + error.blue()) / 5));
        m_edit->setPalette(p);
        m_invalidAction->setVisible(true);
        m_invalidAction->setToolTip(re.errorString());
    }
}

void QtFileMgrFilterBar::show()
{
    QWidget::setVisible(true);
    m_edit->setFocus();
}

void QtFileMgrFilterBar::hide()
{
    QWidget::setVisible(false);
}

void QtFileMgrFilterBar::setVisible(bool v)
{
    QWidget::setVisible(v);
    if (v) m_edit->setFocus();
}

void QtFileMgrFilterBar::showEvent(QShowEvent *event)
{
    if (!event->spontaneous())
        m_edit->setFocus();
}

// 参照 Dolphin FilterBar::keyPressEvent
void QtFileMgrFilterBar::keyPressEvent(QKeyEvent *event)
{
    if (handleEditorKey(event))
        return;
    QWidget::keyPressEvent(event);
}

bool QtFileMgrFilterBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress
        && handleEditorKey(static_cast<QKeyEvent *>(event))) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

bool QtFileMgrFilterBar::handleEditorKey(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_edit->text().isEmpty()) {
            emit closeRequest();
            emit focusFileViewRequested();
        } else {
            m_edit->clear();
        }
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit focusFileViewRequested();
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up:
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
        emit navigationKeyRequested(event->key(), event->modifiers());
        return true;
    }
    return false;
}
