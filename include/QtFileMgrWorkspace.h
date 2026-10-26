// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRWORKSPACE_H
#define QTFILEMGRWORKSPACE_H

#include <QWidget>
#include <QUrl>
#include <QJsonObject>

class QTabWidget;
class QToolButton;
class QTimer;
class QtFileMgrWidget;
class QtFileMgrPlacesPanel;

class QtFileMgrWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit QtFileMgrWorkspace(QWidget *parent = nullptr,
                                bool restoreSavedSession = true);
    ~QtFileMgrWorkspace() override;

    QtFileMgrWidget *currentFileManager() const;
    int tabCount() const;
    int paneCount() const;
    QJsonObject sessionState() const;
    bool restoreSessionState(const QJsonObject &state);

    /// 全局唯一的共享 Places 面板 (标签/分栏只控制文件区域,
    /// 对齐 Dolphin 布局)
    QtFileMgrPlacesPanel *placesPanel() const { return m_sharedPlacesPanel; }

public slots:
    void addTab();
    void addTab(const QUrl &url);
    void closeCurrentTab();
    void splitCurrentTab();
    void closeCurrentSplit();
    void restoreClosedTab();
    void copyToOtherPane();
    void moveToOtherPane();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *createTabPage(const QJsonObject &state = QJsonObject());
    QtFileMgrWidget *addPane(QWidget *page, const QUrl &url);
    void connectManager(QWidget *page, QtFileMgrWidget *manager);
    void closeTab(int index, bool remember);
    void duplicateTab(int index);
    void setActiveManager(QtFileMgrWidget *manager);
    void updateActivePane();
    void updateTransferActions();
    void updateTabCloseVisibility();
    void scheduleSessionSave();
    void saveSession() const;
    bool restoreSession();
    QJsonObject tabState(QWidget *page) const;
    QtFileMgrWidget *otherFileManager() const;
    void transferToOtherPane(bool move);
    void updateTabTitle(QWidget *page, const QUrl &url);
    QtFileMgrWidget *focusedFileManager() const;

    QTabWidget *m_tabs = nullptr;
    QtFileMgrPlacesPanel *m_sharedPlacesPanel = nullptr;
    QToolButton *m_copyToOtherButton = nullptr;
    QToolButton *m_moveToOtherButton = nullptr;
    QToolButton *m_closeSplitButton = nullptr;
    QTimer *m_sessionSaveTimer = nullptr;
    QtFileMgrWidget *m_activeManager = nullptr;
    QList<QJsonObject> m_closedTabs;
    bool m_restoreSavedSession = true;
    bool m_restoringSession = false;
};

#endif
