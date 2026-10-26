// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRSETTINGSDIALOG_H
#define QTFILEMGRSETTINGSDIALOG_H

#include <QDialog>
#include "QtFileMgrWidget.h"

class QTabWidget;
class QListWidget;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QSlider;
class QLabel;
class QPushButton;

// 参照 DolphinSettingsDialog: 四按钮 + 副本模式 + 脏跟踪
class QtFileMgrSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrSettingsDialog(const QtFileMgrConfig &cfg, QWidget *parent = nullptr);

    // 返回修改后的配置 (仅在 Accepted 时有效)
    QtFileMgrConfig resultConfig() const { return m_editCopy; }

    // 扩展接口: 集成项目可添加自定义设置页
    void addPage(const QString &title, QWidget *page);
    QTabWidget *tabWidget() const { return m_tabs; }

signals:
    void settingsApplied();  // Apply/OK 时发出, 主界面据此刷新

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onAddPlace();
    void onRemovePlace();
    void onMoveUp();
    void onMoveDown();
    void updateCurrentPlace();
    void onApply();
    void onRestoreDefaults();
    void onSaveAs();
    void markDirty();

private:
    void loadToUi();
    void refreshPlaceList();
    void collectFromUi();
    bool confirmDiscardIfDirty();

    // ── 副本: 所有修改在副本上进行, OK/Apply 时写回 ──
    QtFileMgrConfig m_editCopy;

    // ── 按钮 ──
    QPushButton *m_applyBtn;

    // ── 脏跟踪 ──
    bool m_dirty = false;
    bool m_loading = false;  // loadToUi 期间抑制 markDirty

    // ── Places 页 ──
    QList<PlaceEntry> m_places;    // places 的可编辑列表副本
    QListWidget *m_placeList;
    QLineEdit *m_nameEdit;
    QLineEdit *m_pathEdit;

    // ── View 页 ──
    QComboBox *m_viewModeCombo;
    QComboBox *m_sortColumnCombo;
    QComboBox *m_sortOrderCombo;
    QCheckBox *m_showHiddenFiles;
    QCheckBox *m_foldersFirst;
    QCheckBox *m_alternatingColors;
    QCheckBox *m_rememberMode;
    QSlider *m_zoomSlider;
    QLabel *m_zoomValueLabel;

    // ── Permissions 页 ──
    QCheckBox *m_allowCreate;
    QCheckBox *m_allowDelete;
    QCheckBox *m_allowRename;
    QCheckBox *m_allowDragDrop;

    // ── Appearance 页 ──
    QCheckBox *m_tbIcons, *m_tbCompact, *m_tbDetails;
    QCheckBox *m_tbHidden, *m_tbFolders, *m_tbBreadcrumb, *m_tbSettings;
    QSpinBox *m_tbIconSize;
    QCheckBox *m_sbVisible, *m_sbZoom, *m_sbDiskSpace, *m_sbItemCount;
    QSpinBox *m_placesWidth, *m_placesIconSize;
    QSpinBox *m_splitterHandle;

    // ── Places 配置 ──
    QCheckBox *m_showDevices;
    QSpinBox *m_deviceRefreshSec;

    // ── Features 页 ──
    QCheckBox *m_filterVisible;
    QComboBox *m_filterMode;
    QCheckBox *m_filterCaseSensitive;
    QCheckBox *m_showThumbnails;
    QCheckBox *m_showVersions;

    // ── 扩展 ──
    QTabWidget *m_tabs;
};

#endif
