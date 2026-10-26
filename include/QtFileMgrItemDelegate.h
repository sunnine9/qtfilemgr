// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRITEMDELEGATE_H
#define QTFILEMGRITEMDELEGATE_H

#include <QStyledItemDelegate>

// 纯 QStyledItemDelegate, 不依赖 QFileSystemModel
// 数据全部通过 model roles 获取: DisplayRole(名称), DecorationRole(图标), UserRole(路径)
class QtFileMgrItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum ViewMode { IconsView, CompactView, DetailsView };

    explicit QtFileMgrItemDelegate(QObject *parent = nullptr);

    void setViewMode(ViewMode mode);
    void setIconSize(int size);
    void setGridSize(const QSize &size);
    void setFilterText(const QString &text);
    void setAlternatingRows(bool on) { m_alternatingRows = on; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;

signals:
    /// 重命名等操作失败(异步完成时编辑器已销毁,经此信号由 FileView 呈现)
    void operationFailed(const QString &message) const;

private:
    ViewMode m_mode;
    int m_iconSize;
    QSize m_gridSize;
    QString m_filterText;
    bool m_alternatingRows = true;
};

#endif
