// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// Dolphin 风格汉堡菜单 — 整合视图/显示选项 + 可扩展区域
#include "QtFileMgrHamburgerMenu.h"
#include <QIcon>
#include <QStyle>

QtFileMgrHamburgerMenu::QtFileMgrHamburgerMenu(QWidget *parent)
    : QToolButton(parent)
{
    setText(QString(QChar(0x2630)));
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setToolTip(QStringLiteral("更多选项"));
    setPopupMode(QToolButton::InstantPopup);
    setAutoRaise(true);

    m_menu = new QMenu(this);
    setMenu(m_menu);

    populateMenu();
}

void QtFileMgrHamburgerMenu::populateMenu()
{
    // ─── 视图模式 ───
    m_viewModeGroup = new QActionGroup(this);
    m_viewModeGroup->setExclusive(true);

    m_iconAction = new QAction(QStringLiteral("图标"), m_viewModeGroup);
    m_iconAction->setCheckable(true);
    m_iconAction->setChecked(true);
    m_iconAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));

    m_compactAction = new QAction(QStringLiteral("紧凑"), m_viewModeGroup);
    m_compactAction->setCheckable(true);
    m_compactAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));

    m_detailAction = new QAction(QStringLiteral("列表"), m_viewModeGroup);
    m_detailAction->setCheckable(true);
    m_detailAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));

    connect(m_iconAction, &QAction::triggered, this, [this] { emit viewModeChanged(0); });
    connect(m_compactAction, &QAction::triggered, this, [this] { emit viewModeChanged(1); });
    connect(m_detailAction, &QAction::triggered, this, [this] { emit viewModeChanged(2); });

    m_menu->addAction(m_iconAction);
    m_menu->addAction(m_compactAction);
    m_menu->addAction(m_detailAction);

    m_menu->addSeparator();

    // ─── 显示选项 ───
    m_hiddenFilesAction = new QAction(QStringLiteral("显示隐藏文件"), this);
    m_hiddenFilesAction->setCheckable(true);
    connect(m_hiddenFilesAction, &QAction::triggered, this, &QtFileMgrHamburgerMenu::showHiddenFilesToggled);

    m_foldersFirstAction = new QAction(QStringLiteral("文件夹优先"), this);
    m_foldersFirstAction->setCheckable(true);
    m_foldersFirstAction->setChecked(true);
    connect(m_foldersFirstAction, &QAction::triggered, this, &QtFileMgrHamburgerMenu::foldersFirstToggled);

    m_menu->addAction(m_hiddenFilesAction);
    m_menu->addAction(m_foldersFirstAction);

    // ─── 扩展区锚点 (集成项目在此插入自定义菜单项) ───
    m_extensionAnchor = m_menu->addSeparator();
}

// ─── 状态同步 (不触发信号) ──────────────────────────────────

void QtFileMgrHamburgerMenu::setViewMode(int mode)
{
    const bool oldI = m_iconAction->blockSignals(true);
    const bool oldC = m_compactAction->blockSignals(true);
    const bool oldD = m_detailAction->blockSignals(true);

    m_iconAction->setChecked(mode == 0);
    m_compactAction->setChecked(mode == 1);
    m_detailAction->setChecked(mode == 2);

    m_iconAction->blockSignals(oldI);
    m_compactAction->blockSignals(oldC);
    m_detailAction->blockSignals(oldD);

}

int QtFileMgrHamburgerMenu::viewMode() const
{
    if (m_compactAction->isChecked()) return 1;
    if (m_detailAction->isChecked()) return 2;
    return 0;
}

void QtFileMgrHamburgerMenu::setShowHiddenFiles(bool show)
{
    const bool old = m_hiddenFilesAction->blockSignals(true);
    m_hiddenFilesAction->setChecked(show);
    m_hiddenFilesAction->blockSignals(old);
}

bool QtFileMgrHamburgerMenu::showHiddenFiles() const
{
    return m_hiddenFilesAction->isChecked();
}

void QtFileMgrHamburgerMenu::setFoldersFirst(bool on)
{
    const bool old = m_foldersFirstAction->blockSignals(true);
    m_foldersFirstAction->setChecked(on);
    m_foldersFirstAction->blockSignals(old);
}

bool QtFileMgrHamburgerMenu::foldersFirst() const
{
    return m_foldersFirstAction->isChecked();
}

// ─── 扩展接口 ──────────────────────────────────────────────

QAction *QtFileMgrHamburgerMenu::addExtensionSeparator()
{
    // 在扩展区锚点前插入分隔线 (锚点本身是 separator)
    QAction *sep = new QAction(this);
    sep->setSeparator(true);
    m_menu->insertAction(m_extensionAnchor, sep);
    return sep;
}

QAction *QtFileMgrHamburgerMenu::addExtensionAction(const QString &text)
{
    QAction *act = new QAction(text, this);
    m_menu->insertAction(m_extensionAnchor, act);
    return act;
}

// ─── 可见性控制 ────────────────────────────────────────────

void QtFileMgrHamburgerMenu::setViewModeActionsVisible(bool visible)
{
    m_iconAction->setVisible(visible);
    m_compactAction->setVisible(visible);
    m_detailAction->setVisible(visible);
}

void QtFileMgrHamburgerMenu::setHiddenFilesActionVisible(bool visible)
{
    m_hiddenFilesAction->setVisible(visible);
}

void QtFileMgrHamburgerMenu::setFoldersFirstActionVisible(bool visible)
{
    m_foldersFirstAction->setVisible(visible);
}
