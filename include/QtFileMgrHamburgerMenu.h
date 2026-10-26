// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRHAMBURGERMENU_H
#define QTFILEMGRHAMBURGERMENU_H

#include <QToolButton>
#include <QMenu>
#include <QActionGroup>

// Dolphin 风格汉堡菜单 — 整合视图模式/隐藏文件/文件夹优先 + 可扩展
// 集成项目通过 addSeparator()/addAction() 在扩展区插入自定义项 (如网络切换)
class QtFileMgrHamburgerMenu : public QToolButton {
    Q_OBJECT
public:
    explicit QtFileMgrHamburgerMenu(QWidget *parent = nullptr);

    // 状态同步 (外部状态变更后调用, 不会触发信号)
    void setViewMode(int mode);       // 0=Icons, 1=Compact, 2=Details
    void setShowHiddenFiles(bool show);
    void setFoldersFirst(bool on);

    // 当前状态
    int viewMode() const;
    bool showHiddenFiles() const;
    bool foldersFirst() const;

    // 扩展接口 (集成项目在扩展区插入菜单项)
    QAction *addExtensionSeparator();
    QAction *addExtensionAction(const QString &text);
    QMenu   *menu() const { return m_menu; }

    // 配置项可见性
    void setViewModeActionsVisible(bool visible);
    void setHiddenFilesActionVisible(bool visible);
    void setFoldersFirstActionVisible(bool visible);

signals:
    void viewModeChanged(int mode);        // 0=Icons, 1=Compact, 2=Details
    void showHiddenFilesToggled(bool show);
    void foldersFirstToggled(bool on);

private:
    void populateMenu();

    QMenu       *m_menu;
    QActionGroup *m_viewModeGroup;
    QAction      *m_iconAction;
    QAction      *m_compactAction;
    QAction      *m_detailAction;
    QAction      *m_hiddenFilesAction;
    QAction      *m_foldersFirstAction;

    // 扩展区边界 (在此之前的 separator 之后插入)
    QAction      *m_extensionAnchor;
};

#endif // QTFILEMGRHAMBURGERMENU_H
