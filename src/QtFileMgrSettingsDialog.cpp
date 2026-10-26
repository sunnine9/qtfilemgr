// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

//
#include "QtFileMgrSettingsDialog.h"
#include "QtFileMgrConfigStore.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QCloseEvent>
#include <QFileDialog>

// ─── 构造 ─────────────────────────────────────────────────

QtFileMgrSettingsDialog::QtFileMgrSettingsDialog(const QtFileMgrConfig &cfg, QWidget *parent)
    : QDialog(parent)
    , m_editCopy(cfg)           // 深拷贝, 所有修改在副本上进行
    , m_places(cfg.places)
{
    setWindowTitle(QStringLiteral("文件管理器设置"));
    resize(540, 460);

    auto *mainLay = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);

    // ════════════════════════════════════════════════════════
    // Tab 1: Places
    // ════════════════════════════════════════════════════════
    {
        auto *tab = new QWidget;
        auto *lay = new QVBoxLayout(tab);

        m_placeList = new QListWidget;
        lay->addWidget(m_placeList);

        auto *editLay = new QFormLayout;
        m_nameEdit = new QLineEdit;  connect(m_nameEdit, &QLineEdit::textChanged, this, &QtFileMgrSettingsDialog::markDirty);
        m_pathEdit = new QLineEdit;  connect(m_pathEdit, &QLineEdit::textChanged, this, &QtFileMgrSettingsDialog::markDirty);
        editLay->addRow(QStringLiteral("名称:"), m_nameEdit);
        editLay->addRow(QStringLiteral("路径:"), m_pathEdit);
        lay->addLayout(editLay);

        auto *btnLay = new QHBoxLayout;
        auto *addBtn    = new QPushButton(QStringLiteral("添加"));
        auto *removeBtn = new QPushButton(QStringLiteral("移除"));
        auto *upBtn     = new QPushButton(QStringLiteral("上移"));
        auto *downBtn   = new QPushButton(QStringLiteral("下移"));
        btnLay->addWidget(addBtn); btnLay->addWidget(removeBtn);
        btnLay->addWidget(upBtn); btnLay->addWidget(downBtn);
        btnLay->addStretch();
        lay->addLayout(btnLay);

        m_showDevices = new QCheckBox(QStringLiteral("显示设备组"));
        m_showDevices->setChecked(m_editCopy.showDevices);
        connect(m_showDevices, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        lay->addWidget(m_showDevices);

        auto *refreshLay = new QHBoxLayout;
        refreshLay->addWidget(new QLabel(QStringLiteral("设备刷新间隔(秒):")));
        m_deviceRefreshSec = new QSpinBox;
        m_deviceRefreshSec->setRange(5, 300);
        m_deviceRefreshSec->setSuffix(QStringLiteral(" s"));
        m_deviceRefreshSec->setValue(m_editCopy.deviceRefreshMs / 1000);
        connect(m_deviceRefreshSec, QOverload<int>::of(&QSpinBox::valueChanged), this, &QtFileMgrSettingsDialog::markDirty);
        refreshLay->addWidget(m_deviceRefreshSec);
        refreshLay->addStretch();
        lay->addLayout(refreshLay);

        connect(addBtn,    &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onAddPlace);
        connect(removeBtn, &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onRemovePlace);
        connect(upBtn,     &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onMoveUp);
        connect(downBtn,   &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onMoveDown);
        connect(m_placeList, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row >= 0 && row < m_places.size()) {
                m_loading = true;
                m_nameEdit->setText(m_places[row].name);
                m_pathEdit->setText(m_places[row].path);
                m_loading = false;
            }
        });
        connect(m_nameEdit, &QLineEdit::editingFinished,
                this, &QtFileMgrSettingsDialog::updateCurrentPlace);
        connect(m_pathEdit, &QLineEdit::editingFinished,
                this, &QtFileMgrSettingsDialog::updateCurrentPlace);

        m_tabs->addTab(tab, QStringLiteral("位置"));
    }

    // ════════════════════════════════════════════════════════
    // Tab 2: View
    // ════════════════════════════════════════════════════════
    {
        auto *tab = new QWidget;
        auto *lay = new QFormLayout(tab);

        m_viewModeCombo = new QComboBox;
        m_viewModeCombo->addItem(QStringLiteral("图标"), 0);
        m_viewModeCombo->addItem(QStringLiteral("紧凑"), 1);
        m_viewModeCombo->addItem(QStringLiteral("列表"), 2);
        connect(m_viewModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(QStringLiteral("默认视图:"), m_viewModeCombo);

        auto *zoomLay = new QHBoxLayout;
        m_zoomSlider = new QSlider(Qt::Horizontal);
        m_zoomSlider->setRange(0, 16);
        m_zoomValueLabel = new QLabel;
        m_zoomValueLabel->setFixedWidth(24);
        zoomLay->addWidget(m_zoomSlider);
        zoomLay->addWidget(m_zoomValueLabel);
        lay->addRow(QStringLiteral("缩放级别:"), zoomLay);
        connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) {
            m_zoomValueLabel->setText(QString::number(v));
            markDirty();
        });

        m_sortColumnCombo = new QComboBox;
        m_sortColumnCombo->addItem(QStringLiteral("名称"), 0);
        m_sortColumnCombo->addItem(QStringLiteral("大小"), 1);
        m_sortColumnCombo->addItem(QStringLiteral("类型"), 2);
        m_sortColumnCombo->addItem(QStringLiteral("修改日期"), 3);
        m_sortColumnCombo->addItem(QStringLiteral("版本"), 4);
        connect(m_sortColumnCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(QStringLiteral("排序方式:"), m_sortColumnCombo);

        m_sortOrderCombo = new QComboBox;
        m_sortOrderCombo->addItem(QStringLiteral("升序"), static_cast<int>(Qt::AscendingOrder));
        m_sortOrderCombo->addItem(QStringLiteral("降序"), static_cast<int>(Qt::DescendingOrder));
        connect(m_sortOrderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(QStringLiteral("排序方向:"), m_sortOrderCombo);

        m_showHiddenFiles = new QCheckBox(QStringLiteral("显示隐藏文件"));
        connect(m_showHiddenFiles, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(m_showHiddenFiles);

        m_foldersFirst = new QCheckBox(QStringLiteral("文件夹优先"));
        connect(m_foldersFirst, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(m_foldersFirst);

        m_alternatingColors = new QCheckBox(QStringLiteral("交替行颜色"));
        connect(m_alternatingColors, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(m_alternatingColors);

        m_rememberMode = new QCheckBox(QStringLiteral("记住每个目录的视图模式"));
        connect(m_rememberMode, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        lay->addRow(m_rememberMode);

        m_tabs->addTab(tab, QStringLiteral("视图"));
    }

    // ════════════════════════════════════════════════════════
    // Tab 3: Permissions
    // ════════════════════════════════════════════════════════
    {
        auto *tab = new QWidget;
        auto *lay = new QVBoxLayout(tab);

        m_allowCreate   = new QCheckBox(QStringLiteral("允许创建文件夹")); connect(m_allowCreate, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); lay->addWidget(m_allowCreate);
        m_allowDelete   = new QCheckBox(QStringLiteral("允许删除文件"));   connect(m_allowDelete, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); lay->addWidget(m_allowDelete);
        m_allowRename   = new QCheckBox(QStringLiteral("允许重命名文件")); connect(m_allowRename, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); lay->addWidget(m_allowRename);
        m_allowDragDrop = new QCheckBox(QStringLiteral("允许拖放"));       connect(m_allowDragDrop, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); lay->addWidget(m_allowDragDrop);
        lay->addStretch();

        m_tabs->addTab(tab, QStringLiteral("权限"));
    }

    // ════════════════════════════════════════════════════════
    // Tab 4: Appearance
    // ════════════════════════════════════════════════════════
    {
        auto *tab = new QWidget;
        auto *lay = new QVBoxLayout(tab);

        auto *tbGroup = new QGroupBox(QStringLiteral("工具栏"));
        auto *tbLay = new QVBoxLayout(tbGroup);
        auto *tbSizeLay = new QHBoxLayout;
        tbSizeLay->addWidget(new QLabel(QStringLiteral("图标大小:")));
        m_tbIconSize = new QSpinBox; m_tbIconSize->setRange(12, 48); m_tbIconSize->setSuffix(QStringLiteral(" px"));
        connect(m_tbIconSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &QtFileMgrSettingsDialog::markDirty);
        tbSizeLay->addWidget(m_tbIconSize); tbSizeLay->addStretch(); tbLay->addLayout(tbSizeLay);

        m_tbIcons      = new QCheckBox(QStringLiteral("图标视图按钮")); connect(m_tbIcons, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbIcons);
        m_tbCompact    = new QCheckBox(QStringLiteral("紧凑视图按钮")); connect(m_tbCompact, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbCompact);
        m_tbDetails    = new QCheckBox(QStringLiteral("列表视图按钮")); connect(m_tbDetails, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbDetails);
        m_tbHidden     = new QCheckBox(QStringLiteral("隐藏文件按钮")); connect(m_tbHidden, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbHidden);
        m_tbFolders    = new QCheckBox(QStringLiteral("文件夹优先按钮")); connect(m_tbFolders, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbFolders);
        m_tbBreadcrumb = new QCheckBox(QStringLiteral("面包屑导航")); connect(m_tbBreadcrumb, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbBreadcrumb);
        m_tbSettings   = new QCheckBox(QStringLiteral("设置按钮")); connect(m_tbSettings, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); tbLay->addWidget(m_tbSettings);
        lay->addWidget(tbGroup);

        auto *sbGroup = new QGroupBox(QStringLiteral("状态栏"));
        auto *sbLay = new QVBoxLayout(sbGroup);
        m_sbVisible   = new QCheckBox(QStringLiteral("显示状态栏")); connect(m_sbVisible, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); sbLay->addWidget(m_sbVisible);
        m_sbZoom      = new QCheckBox(QStringLiteral("显示缩放滑块")); connect(m_sbZoom, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); sbLay->addWidget(m_sbZoom);
        m_sbDiskSpace = new QCheckBox(QStringLiteral("显示磁盘空间")); connect(m_sbDiskSpace, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); sbLay->addWidget(m_sbDiskSpace);
        m_sbItemCount = new QCheckBox(QStringLiteral("显示项目统计")); connect(m_sbItemCount, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty); sbLay->addWidget(m_sbItemCount);
        lay->addWidget(sbGroup);

        auto *pnGroup = new QGroupBox(QStringLiteral("面板"));
        auto *pnLay = new QFormLayout(pnGroup);
        m_placesWidth = new QSpinBox; m_placesWidth->setRange(150, 500); m_placesWidth->setSuffix(QStringLiteral(" px"));
        connect(m_placesWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &QtFileMgrSettingsDialog::markDirty);
        pnLay->addRow(QStringLiteral("位置面板宽度:"), m_placesWidth);
        m_placesIconSize = new QSpinBox; m_placesIconSize->setRange(16, 48); m_placesIconSize->setSuffix(QStringLiteral(" px"));
        connect(m_placesIconSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &QtFileMgrSettingsDialog::markDirty);
        pnLay->addRow(QStringLiteral("面板图标大小:"), m_placesIconSize);
        m_splitterHandle = new QSpinBox; m_splitterHandle->setRange(0, 5); m_splitterHandle->setSuffix(QStringLiteral(" px"));
        connect(m_splitterHandle, QOverload<int>::of(&QSpinBox::valueChanged), this, &QtFileMgrSettingsDialog::markDirty);
        pnLay->addRow(QStringLiteral("分割线宽度:"), m_splitterHandle);
        lay->addWidget(pnGroup);

        lay->addStretch();
        m_tabs->addTab(tab, QStringLiteral("外观"));
    }

    // ════════════════════════════════════════════════════════
    // Tab 5: Features
    // ════════════════════════════════════════════════════════
    {
        auto *tab = new QWidget;
        auto *lay = new QVBoxLayout(tab);

        auto *fbGroup = new QGroupBox(QStringLiteral("过滤栏"));
        auto *fbLay = new QFormLayout(fbGroup);
        m_filterVisible = new QCheckBox(QStringLiteral("默认显示过滤栏"));
        connect(m_filterVisible, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        fbLay->addRow(m_filterVisible);
        m_filterMode = new QComboBox;
        m_filterMode->addItem(QStringLiteral("纯文本"), 0);
        m_filterMode->addItem(QStringLiteral("通配符"), 1);
        m_filterMode->addItem(QStringLiteral("正则表达式"), 2);
        connect(m_filterMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QtFileMgrSettingsDialog::markDirty);
        fbLay->addRow(QStringLiteral("默认过滤模式:"), m_filterMode);
        m_filterCaseSensitive = new QCheckBox(QStringLiteral("默认区分大小写"));
        connect(m_filterCaseSensitive, &QCheckBox::toggled, this, &QtFileMgrSettingsDialog::markDirty);
        fbLay->addRow(m_filterCaseSensitive);
        lay->addWidget(fbGroup);

        auto *displayGroup = new QGroupBox(QStringLiteral("文件展示"));
        auto *displayLayout = new QVBoxLayout(displayGroup);
        m_showThumbnails = new QCheckBox(QStringLiteral("显示图片缩略图"));
        m_showVersions = new QCheckBox(QStringLiteral("显示版本列"));
        connect(m_showThumbnails, &QCheckBox::toggled,
                this, &QtFileMgrSettingsDialog::markDirty);
        connect(m_showVersions, &QCheckBox::toggled,
                this, &QtFileMgrSettingsDialog::markDirty);
        displayLayout->addWidget(m_showThumbnails);
        displayLayout->addWidget(m_showVersions);
        lay->addWidget(displayGroup);

        lay->addStretch();
        m_tabs->addTab(tab, QStringLiteral("功能"));
    }

    mainLay->addWidget(m_tabs);

    // ── 按钮: 恢复默认 | 另存为 | (spacer) | 取消 | 应用 | 确定 ──
    auto *btnLay = new QHBoxLayout;
    auto *restoreBtn = new QPushButton(QStringLiteral("恢复默认"));
    connect(restoreBtn, &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onRestoreDefaults);
    btnLay->addWidget(restoreBtn);

    auto *saveAsBtn = new QPushButton(QStringLiteral("另存为..."));
    connect(saveAsBtn, &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onSaveAs);
    btnLay->addWidget(saveAsBtn);

    btnLay->addStretch();

    auto *cancelBtn = new QPushButton(QStringLiteral("取消"));
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLay->addWidget(cancelBtn);

    m_applyBtn = new QPushButton(QStringLiteral("应用"));
    m_applyBtn->setEnabled(false);
    connect(m_applyBtn, &QPushButton::clicked, this, &QtFileMgrSettingsDialog::onApply);
    btnLay->addWidget(m_applyBtn);

    auto *okBtn = new QPushButton(QStringLiteral("确定"));
    okBtn->setDefault(true);
    connect(okBtn, &QPushButton::clicked, this, [this]() {
        onApply();
        accept();
    });
    btnLay->addWidget(okBtn);

    mainLay->addLayout(btnLay);

    // ── 加载数据到 UI ──
    loadToUi();
    refreshPlaceList();
}

// ─── loadToUi: m_editCopy → UI 控件 ──────────────────────

void QtFileMgrSettingsDialog::loadToUi()
{
    struct Guard { bool &flag; Guard(bool &f) : flag(f) { flag = true; } ~Guard() { flag = false; } };
    Guard g(m_loading);

    // View
    m_viewModeCombo->setCurrentIndex(m_editCopy.defaultViewMode);
    m_zoomSlider->setValue(m_editCopy.view.zoomLevel);
    m_sortColumnCombo->setCurrentIndex(m_editCopy.view.sortColumn);
    m_sortOrderCombo->setCurrentIndex(m_editCopy.view.sortOrder == Qt::AscendingOrder ? 0 : 1);
    m_showHiddenFiles->setChecked(m_editCopy.view.showHiddenFiles);
    m_foldersFirst->setChecked(m_editCopy.view.foldersFirst);
    m_alternatingColors->setChecked(m_editCopy.view.alternatingRowColors);
    m_rememberMode->setChecked(m_editCopy.rememberMode);

    // Permissions
    m_allowCreate->setChecked(m_editCopy.allowCreate);
    m_allowDelete->setChecked(m_editCopy.allowDelete);
    m_allowRename->setChecked(m_editCopy.allowRename);
    m_allowDragDrop->setChecked(m_editCopy.allowDragDrop);

    // Appearance: Toolbar
    m_tbIconSize->setValue(m_editCopy.toolbar.iconSize);
    m_tbIcons->setChecked(m_editCopy.toolbar.items.viewModeIcons);
    m_tbCompact->setChecked(m_editCopy.toolbar.items.viewModeCompact);
    m_tbDetails->setChecked(m_editCopy.toolbar.items.viewModeDetails);
    m_tbHidden->setChecked(m_editCopy.toolbar.items.showHiddenFiles);
    m_tbFolders->setChecked(m_editCopy.toolbar.items.showFoldersFirst);
    m_tbBreadcrumb->setChecked(m_editCopy.toolbar.items.breadcrumb);
    m_tbSettings->setChecked(m_editCopy.toolbar.items.settingsButton);

    // Appearance: StatusBar
    m_sbVisible->setChecked(m_editCopy.statusBar.visible);
    m_sbZoom->setChecked(m_editCopy.statusBar.showZoomSlider);
    m_sbDiskSpace->setChecked(m_editCopy.statusBar.showDiskSpace);
    m_sbItemCount->setChecked(m_editCopy.statusBar.showItemCount);

    // Appearance: Panels
    m_placesWidth->setValue(m_editCopy.placesPanel.width);
    m_placesIconSize->setValue(m_editCopy.placesPanel.iconSize);
    m_splitterHandle->setValue(m_editCopy.splitterHandleWidth);

    // Places config
    m_showDevices->setChecked(m_editCopy.showDevices);
    m_deviceRefreshSec->setValue(m_editCopy.deviceRefreshMs / 1000);

    // Features
    m_filterVisible->setChecked(m_editCopy.filterBar.visible);
    m_filterMode->setCurrentIndex(m_editCopy.filterBar.defaultMode);
    m_filterCaseSensitive->setChecked(m_editCopy.filterBar.caseSensitive);
    m_showThumbnails->setChecked(m_editCopy.showThumbnails);
    m_showVersions->setChecked(m_editCopy.showVersions);
}

// ─── 脏标记 ───────────────────────────────────────────────

void QtFileMgrSettingsDialog::markDirty()
{
    if (m_loading) return;
    if (!m_dirty) {
        m_dirty = true;
        m_applyBtn->setEnabled(true);
    }
}

// ─── 收集 UI → m_editCopy ─────────────────────────────────

void QtFileMgrSettingsDialog::collectFromUi()
{
    updateCurrentPlace();
    m_editCopy.defaultViewMode = m_viewModeCombo->currentData().toInt();
    m_editCopy.rememberMode = m_rememberMode->isChecked();
    m_editCopy.view.showHiddenFiles = m_showHiddenFiles->isChecked();
    m_editCopy.view.foldersFirst = m_foldersFirst->isChecked();
    m_editCopy.view.zoomLevel = m_zoomSlider->value();
    m_editCopy.view.sortColumn = m_sortColumnCombo->currentData().toInt();
    m_editCopy.view.sortOrder = static_cast<Qt::SortOrder>(m_sortOrderCombo->currentData().toInt());
    m_editCopy.view.alternatingRowColors = m_alternatingColors->isChecked();

    m_editCopy.allowCreate = m_allowCreate->isChecked();
    m_editCopy.allowDelete = m_allowDelete->isChecked();
    m_editCopy.allowRename = m_allowRename->isChecked();
    m_editCopy.allowDragDrop = m_allowDragDrop->isChecked();

    m_editCopy.toolbar.iconSize = m_tbIconSize->value();
    m_editCopy.toolbar.items.viewModeIcons = m_tbIcons->isChecked();
    m_editCopy.toolbar.items.viewModeCompact = m_tbCompact->isChecked();
    m_editCopy.toolbar.items.viewModeDetails = m_tbDetails->isChecked();
    m_editCopy.toolbar.items.showHiddenFiles = m_tbHidden->isChecked();
    m_editCopy.toolbar.items.showFoldersFirst = m_tbFolders->isChecked();
    m_editCopy.toolbar.items.breadcrumb = m_tbBreadcrumb->isChecked();
    m_editCopy.toolbar.items.settingsButton = m_tbSettings->isChecked();

    m_editCopy.statusBar.visible = m_sbVisible->isChecked();
    m_editCopy.statusBar.showZoomSlider = m_sbZoom->isChecked();
    m_editCopy.statusBar.showDiskSpace = m_sbDiskSpace->isChecked();
    m_editCopy.statusBar.showItemCount = m_sbItemCount->isChecked();

    m_editCopy.placesPanel.width = m_placesWidth->value();
    m_editCopy.placesPanel.iconSize = m_placesIconSize->value();
    m_editCopy.splitterHandleWidth = m_splitterHandle->value();

    m_editCopy.showDevices = m_showDevices->isChecked();
    m_editCopy.deviceRefreshMs = m_deviceRefreshSec->value() * 1000;

    m_editCopy.filterBar.visible = m_filterVisible->isChecked();
    m_editCopy.filterBar.defaultMode = m_filterMode->currentData().toInt();
    m_editCopy.filterBar.caseSensitive = m_filterCaseSensitive->isChecked();
    m_editCopy.showThumbnails = m_showThumbnails->isChecked();
    m_editCopy.showVersions = m_showVersions->isChecked();

    m_editCopy.places = m_places;
}

void QtFileMgrSettingsDialog::onApply()
{
    collectFromUi();
    emit settingsApplied();

    m_dirty = false;
    m_applyBtn->setEnabled(false);
}

// ─── RestoreDefaults: 从 qrc 重载默认值到 UI ─────────────

void QtFileMgrSettingsDialog::onRestoreDefaults()
{
    const QtFileMgrConfig defaults = QtFileMgrConfig::defaultConfig();
    m_editCopy = defaults;
    m_places = defaults.places;

    loadToUi();
    refreshPlaceList();
    markDirty();
}

// ─── 关闭保护 ─────────────────────────────────────────────

bool QtFileMgrSettingsDialog::confirmDiscardIfDirty()
{
    if (!m_dirty)
        return true;
    auto ret = QMessageBox::warning(this,
        QStringLiteral("未保存的更改"),
        QStringLiteral("您有未保存的更改。"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ret == QMessageBox::Save) {
        onApply();
        return true;
    }
    return ret == QMessageBox::Discard;
}

void QtFileMgrSettingsDialog::closeEvent(QCloseEvent *event)
{
    if (confirmDiscardIfDirty())
        event->accept();
    else
        event->ignore();
}

void QtFileMgrSettingsDialog::reject()
{
    // Cancel 按钮与 Esc 都走 reject(),必须与关闭按钮同样做脏检查 (DLG-P1-4)
    if (confirmDiscardIfDirty())
        QDialog::reject();
}

// ─── Places 操作 ──────────────────────────────────────────

void QtFileMgrSettingsDialog::refreshPlaceList()
{
    const int currentRow = m_placeList->currentRow();
    m_placeList->clear();
    for (const auto &p : m_places)
        m_placeList->addItem(p.name + QStringLiteral("  →  ") + p.path);
    if (!m_places.isEmpty())
        m_placeList->setCurrentRow(qBound(0, currentRow, m_places.size() - 1));
}

void QtFileMgrSettingsDialog::updateCurrentPlace()
{
    if (m_loading)
        return;
    const int row = m_placeList->currentRow();
    if (row < 0 || row >= m_places.size())
        return;
    const QString name = m_nameEdit->text().trimmed();
    const QString path = m_pathEdit->text().trimmed();
    if (name.isEmpty() || path.isEmpty())
        return;
    if (m_places[row].name == name && m_places[row].path == path)
        return;
    m_places[row].name = name;
    m_places[row].path = path;
    const bool loading = m_loading;
    m_loading = true;
    m_placeList->item(row)->setText(name + QStringLiteral("  →  ") + path);
    m_loading = loading;
    markDirty();
}

void QtFileMgrSettingsDialog::onAddPlace()
{
    QString name = m_nameEdit->text().trimmed();
    QString path = m_pathEdit->text().trimmed();
    if (name.isEmpty() || path.isEmpty()) return;
    m_places.append({name, path, QStringLiteral("folder")});
    refreshPlaceList();
    m_placeList->setCurrentRow(m_places.size() - 1);
    markDirty();
}

void QtFileMgrSettingsDialog::onRemovePlace()
{
    int row = m_placeList->currentRow();
    if (row < 0 || row >= m_places.size()) return;
    m_places.removeAt(row);
    refreshPlaceList();
    markDirty();
}

void QtFileMgrSettingsDialog::onMoveUp()
{
    int row = m_placeList->currentRow();
    if (row <= 0) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_places.swapItemsAt(row, row - 1);
#else
    m_places.swap(row, row - 1);
#endif
    refreshPlaceList();
    m_placeList->setCurrentRow(row - 1);
    markDirty();
}

void QtFileMgrSettingsDialog::onMoveDown()
{
    int row = m_placeList->currentRow();
    if (row < 0 || row + 1 >= m_places.size()) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_places.swapItemsAt(row, row + 1);
#else
    m_places.swap(row, row + 1);
#endif
    refreshPlaceList();
    m_placeList->setCurrentRow(row + 1);
    markDirty();
}

void QtFileMgrSettingsDialog::addPage(const QString &title, QWidget *page)
{
    m_tabs->addTab(page, title);
}

void QtFileMgrSettingsDialog::onSaveAs()
{
    collectFromUi();

    const QString defDir = QFileInfo(
        QtFileMgrConfigStore::defaultConfigPath()).absolutePath();
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("另存配置为..."),
        defDir + "/mgr_config.json", QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;

    QString error;
    if (!QtFileMgrConfigStore::saveFile(m_editCopy, path, &error)) {
        QMessageBox::warning(this, QStringLiteral("错误"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("已保存"),
        QStringLiteral("配置已保存到:\n%1").arg(path));
}
