// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRFLATMODEL_H
#define QTFILEMGRFLATMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QHash>
#include <QIcon>
#include <QPointer>
#include "QtFileMgrIO.h"

class QTimer;

class QtFileMgrFlatModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit QtFileMgrFlatModel(QtFileMgrIO *io, QObject *parent = nullptr);
    ~QtFileMgrFlatModel() override;

    // QAbstractItemModel
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int col, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &) const override { return QModelIndex(); }
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    // 拖出时携带 text/uri-list 与源后端标识 (R2-FV-P1-1/P1-2)
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    /// datalist 拖放的源后端标识;目标侧仅在同标识时接受 datalist
    static QByteArray backendDragId(QtFileMgrIO *io);
    /// 该标识使用的 MIME 格式名
    static const char *backendDragMimeFormat();

    // 目录
    void setUrl(const QString &path);
    QString url() const { return m_url; }
    void reload();
    void rebuild();
    bool isLoading() const { return m_loading; }
    int sourceItemCount() const { return m_sourceItems.size(); }

    // IO 切换
    void setIO(QtFileMgrIO *io);
    void setIOAndUrl(QtFileMgrIO *io, const QString &path);

    // 名称合法性校验 (过滤空名、"."、".."、路径分隔符和 NUL)
    static bool isValidDisplayName(const QString &name);

    // 过滤
    enum FilterMode { PlainText, Glob, Regex };
    void setNameFilter(const QString &text);
    void setFilterMode(int mode);
    void setCaseSensitive(bool on);
    QString nameFilter() const { return m_filter; }

    // 排序
    void setFoldersFirst(bool on);
    bool foldersFirst() const { return m_foldersFirst; }
    void setSortColumn(int col, Qt::SortOrder order);
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }
    int directoryCount() const { return m_directoryCount; }
    int fileCount() const { return m_fileCount; }

    // 显示
    void setShowHidden(bool on);
    bool showHidden() const { return m_showHidden; }
    void setShowThumbnails(bool on);
    void setThumbnailSize(const QSize &size);
    void setShowVersions(bool on);

    // IO
    QtFileMgrIO *io() const { return m_io; }

    /// 当前过滤后可见条目的浅拷贝快照(QList 隐式共享,拷贝廉价),
    /// 供调用方免回查 IO 地读取 isDir/size 等元数据。
    QList<FileEntry> entriesSnapshot() const { return m_items; }

signals:
    void loadingChanged(bool loading);
    void directoryBatchLoaded(int batchSize, int totalSourceItems);
    void directoryLoadFinished(const QString &path, const QString &error);

private:
    void cancelActiveRequest();
    void setLoading(bool loading);
    void finishReset();

    QPointer<QtFileMgrIO> m_io;
    QString m_url;
    QList<FileEntry> m_sourceItems;
    QList<FileEntry> m_items;
    QString m_filter;
    int m_filterMode = 0;  // PlainText=0, Glob=1, Regex=2
    bool m_caseSensitive = false;
    bool m_foldersFirst = true;
    bool m_showHidden = false;
    bool m_showThumbnails = false;
    bool m_showVersions = false;
    QSize m_thumbnailSize = QSize(96, 96);
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    int m_directoryCount = 0;
    int m_fileCount = 0;
    mutable QHash<QString, QIcon> m_iconCache;
    QPointer<QtFileMgrDirectoryRequest> m_activeRequest;
    QTimer *m_batchRebuildTimer = nullptr;
    quint64 m_loadGeneration = 0;
    bool m_loading = false;
    bool m_resetInProgress = false;
    bool m_reloadPending = false;
    bool m_rebuildPending = false;
};

#endif
