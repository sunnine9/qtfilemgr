// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrPathPicker.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrBreadcrumbBar.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrLocalIO.h"
#include "QtFileMgrConfigStore.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QDebug>
#include <QPushButton>
#include <QUrl>
#include <QDir>

QtFileMgrPathPicker::QtFileMgrPathPicker(QWidget *parent)
    : QDialog(parent)
{
}

QString QtFileMgrPathPicker::selectPath(QtFileMgrIO *io, const QString &startPath,
                                          const QString &title)
{
    if (!io) {
        qWarning("[PathPicker] selectPath: rejected null IO (startPath=\"%s\")",
                 qPrintable(startPath));
        return QString();
    }
    setWindowTitle(title.isEmpty() ? QStringLiteral("选择目录") : title);
    resize(700, 480);

    // 布局与按钮只在首次调用时创建;重复调用不再 new QVBoxLayout (DLG-P1-1)
    QVBoxLayout *lay = qobject_cast<QVBoxLayout *>(layout());
    if (!lay) {
        lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        // 底部按钮
        auto *btnLay = new QHBoxLayout;
        btnLay->setContentsMargins(8, 4, 8, 8);
        btnLay->addStretch();
        auto *okBtn = new QPushButton(QStringLiteral("确定"), this);
        auto *cancelBtn = new QPushButton(QStringLiteral("取消"), this);
        btnLay->addWidget(okBtn);
        btnLay->addWidget(cancelBtn);
        connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        // 文件管理器后续插入 index 0,按钮固定垫底
        lay->addLayout(btnLay);
    }

    // 构造精简配置: 只显示面包屑+文件视图, 不自动导航
    QtFileMgrConfig cfg;
    cfg.rootPath.clear();  // 阻止 applyConfig 自动 setUrl 到 homePath
    cfg.showDevices = false;
    cfg.toolbar.items.breadcrumb = true;
    cfg.toolbar.items.settingsButton = false;
    cfg.toolbar.items.showHiddenFiles = false;
    cfg.toolbar.items.showFoldersFirst = false;
    cfg.toolbar.items.viewModeIcons = false;
    cfg.toolbar.items.viewModeCompact = false;
    cfg.toolbar.items.viewModeDetails = false;
    cfg.statusBar.visible = false;
    cfg.filterBar.visible = false;
    // 远端模式: 复用 ConfigStore 的持久化读取 (R2-18: 此前直读
    // QSettings 的 "path" 键,而写入方写的是 "url",全部退化为空)
    if (io && io->isRemote()) {
        cfg.placesPanel.width = 200;
        const QList<PlaceEntry> networkPlaces =
            QtFileMgrConfigStore::loadNetworkPlaces();
        for (const PlaceEntry &entry : networkPlaces) {
            // 只保留与当前后端兼容的位置
            if (io->acceptsUrl(QUrl(entry.path)))
                cfg.places.append(entry);
        }
        if (cfg.places.isEmpty()) {
            qWarning("[PathPicker] no compatible saved places for backend %s",
                     io->metaObject()->className());
        }
    } else {
        cfg.placesPanel.width = 0;
    }

    auto *fileMgr = new QtFileMgrWidget(cfg, this);
    fileMgr->setIO(io);
    // 起始地址经后端 URL 映射构造,硬编码 remote:/// 会被 SSHIO 拒绝 (R2-18)
    QUrl startUrl = io->isRemote()
        ? io->urlForPath(io->rootPath())
        : QUrl::fromLocalFile(QDir::homePath());
    if (!startPath.isEmpty())
        startUrl = io->isRemote()
            ? io->urlForPath(startPath)
            : QUrl::fromLocalFile(startPath);
    fileMgr->setUrl(startUrl);

    // 只允许选目录, 不打开文件
    connect(fileMgr->fileView(), &QtFileMgrFileView::fileActivated,
            this, [](const QString &) { /* 忽略双击文件 */ });

    // 替换上一次调用的文件管理器,旧实例 deleteLater 防止控件树泄漏 (DLG-P1-1)
    if (m_fileMgr) {
        lay->removeWidget(m_fileMgr);
        m_fileMgr->hide();
        m_fileMgr->deleteLater();
    }
    m_fileMgr = fileMgr;
    lay->insertWidget(0, fileMgr, 1);

    if (exec() == QDialog::Accepted) {
        QUrl url = fileMgr->url();
        return io->isRemote() ? url.path() : url.toLocalFile();
    }
    return QString();
}
