// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

//
#include "QtFileMgrFlatModel.h"
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QApplication>
#include <QStyle>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <QMimeData>
#include <QSet>
#include <algorithm>
#include <memory>

static QString fileTypeText(const FileEntry &entry)
{
    if (entry.isDir)
        return QStringLiteral("文件夹");
    if (entry.isSymlink)
        return QStringLiteral("链接");
    const QString suffix = QFileInfo(entry.name).suffix();
    if (suffix.isEmpty())
        return QStringLiteral("文件");
    return suffix.toUpper() + QStringLiteral(" 文件");
}

static int compareEntryName(const FileEntry &a, const FileEntry &b)
{
    int cmp = QString::localeAwareCompare(a.name.toLower(), b.name.toLower());
    if (cmp == 0)
        cmp = QString::localeAwareCompare(a.name, b.name);
    if (cmp == 0)
        cmp = QString::compare(a.path, b.path, Qt::CaseSensitive);
    return cmp;
}

QtFileMgrFlatModel::QtFileMgrFlatModel(QtFileMgrIO *io, QObject *parent)
    : QAbstractListModel(parent), m_io(nullptr)
{
    if (io && io->thread() == thread()) {
        m_io = io;
    } else if (io) {
        qWarning("[FlatModel] constructor: rejected backend thread mismatch (backend=%s, backendThread=%p, modelThread=%p)",
                 io->metaObject()->className(),
                 static_cast<void *>(io->thread()),
                 static_cast<void *>(thread()));
    }
    m_batchRebuildTimer = new QTimer(this);
    m_batchRebuildTimer->setSingleShot(true);
    m_batchRebuildTimer->setInterval(30);
    connect(m_batchRebuildTimer, &QTimer::timeout,
            this, &QtFileMgrFlatModel::rebuild);
}

QtFileMgrFlatModel::~QtFileMgrFlatModel()
{
    cancelActiveRequest();
}

int QtFileMgrFlatModel::rowCount(const QModelIndex &) const { return m_items.size(); }
int QtFileMgrFlatModel::columnCount(const QModelIndex &) const
{
    return m_showVersions ? 5 : 4;
}

QVariant QtFileMgrFlatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const FileEntry &e = m_items.at(index.row());
    int col = index.column();

    if (role == Qt::UserRole) return e.path;
    if (role == Qt::UserRole + 1) return e.isDir;
    if (role == Qt::DisplayRole) {
        switch (col) {
        case 0: return e.name;
        case 1: if (e.isDir) return QString(); return formatFileSize(e.size);
        case 2: return fileTypeText(e);
        case 3: return QLocale::system().toString(e.modified, QLocale::ShortFormat);
        case 4: return e.hasVersions ? QString::number(e.versionCount) : QString();
        }
    }
    if (role == Qt::TextAlignmentRole) {
        switch (col) {
        case 1:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        case 3:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    if (role == Qt::ToolTipRole) {
        QString info = QStringLiteral("%1\n类型: %2").arg(e.path, fileTypeText(e));
        if (!e.isDir)
            info += QStringLiteral("\n大小: %1").arg(formatFileSize(e.size));
        if (e.modified.isValid())
            info += QStringLiteral("\n修改时间: %1")
                .arg(QLocale::system().toString(e.modified, QLocale::LongFormat));
        if (e.isReadOnly)
            info += QStringLiteral("\n只读");
        return info;
    }
    if (role == Qt::DecorationRole && col == 0) {
        if (!m_io) {
            qWarning("[FlatModel] data: missing IO while resolving icon for row %d", index.row());
            return {};
        }
        const auto cached = m_iconCache.constFind(e.path);
        if (cached != m_iconCache.constEnd())
            return cached.value();
        QIcon icon;
        if (m_showThumbnails && !e.isDir && m_io->hasThumbnails()) {
            const QPixmap thumbnail = m_io->thumbnail(e.path, m_thumbnailSize);
            if (!thumbnail.isNull())
                icon = QIcon(thumbnail);
        }
        if (icon.isNull())
            icon = m_io->fileIcon(e.path);
        if (icon.isNull()) {
            const QStyle::StandardPixmap fallback = e.isDir
                ? QStyle::SP_DirIcon : QStyle::SP_FileIcon;
            icon = QIcon::fromTheme(e.isDir ? QStringLiteral("folder")
                                            : QStringLiteral("text-x-generic"),
                                    QApplication::style()->standardIcon(fallback));
        }
        if (m_iconCache.size() >= 2048)
            m_iconCache.clear();
        m_iconCache.insert(e.path, icon);
        return icon;
    }
    return {};
}

QModelIndex QtFileMgrFlatModel::index(int row, int col, const QModelIndex &) const
{
    if (row < 0 || row >= m_items.size()) return {};
    return createIndex(row, col, quintptr(row));
}

Qt::ItemFlags QtFileMgrFlatModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    // 仅名称列可编辑: 大小/日期等元数据列不得弹出重命名编辑器 (审查#20)
    Qt::ItemFlags flags =
        Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == 0)
        flags |= Qt::ItemIsEditable;
    return flags;
}

/// 内部拖放携带的源后端标识格式;目标仅在同标识时接受 datalist (R2-FV-P1-2)
static const char kBackendDragMime[] = "application/x-qtfilemgr-backend";

const char *QtFileMgrFlatModel::backendDragMimeFormat()
{
    return kBackendDragMime;
}

QByteArray QtFileMgrFlatModel::backendDragId(QtFileMgrIO *io)
{
    if (!io)
        return QByteArrayLiteral("local");
    return (QString::fromLatin1(io->metaObject()->className())
            + QLatin1Char('|')
            + io->urlForPath(io->rootPath()).toString(QUrl::RemovePassword))
        .toUtf8();
}

QStringList QtFileMgrFlatModel::mimeTypes() const
{
    QStringList types = QAbstractListModel::mimeTypes();
    types << QStringLiteral("text/uri-list")
          << QString::fromLatin1(kBackendDragMime);
    return types;
}

QMimeData *QtFileMgrFlatModel::mimeData(const QModelIndexList &indexes) const
{
    QMimeData *mime = QAbstractListModel::mimeData(indexes);
    if (!mime)
        return nullptr;
    QList<QUrl> urls;
    QSet<QString> seenPaths;
    for (const QModelIndex &idx : indexes) {
        if (!idx.isValid() || idx.column() != 0)
            continue;
        const QString path = idx.data(Qt::UserRole).toString();
        if (path.isEmpty() || seenPaths.contains(path))
            continue;
        seenPaths.insert(path);
        urls.append(m_io ? m_io->urlForPath(path)
                         : QUrl::fromLocalFile(path));
    }
    if (urls.isEmpty()) {
        qWarning("[FlatModel] mimeData: drag without resolvable paths (indexes=%d)",
                 indexes.size());
    }
    mime->setUrls(urls);  // 同时写入 text/uri-list,外部应用可取文件 URL
    mime->setData(QString::fromLatin1(kBackendDragMime), backendDragId(m_io));
    return mime;
}

QVariant QtFileMgrFlatModel::headerData(int section, Qt::Orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    static const char *headers[] = {"名称", "大小", "类型", "修改日期", "版本"};
    if (section >= 0 && section < columnCount())
        return QString::fromUtf8(headers[section]);
    return {};
}

void QtFileMgrFlatModel::setIO(QtFileMgrIO *io)
{
    if (!io || io == m_io) return;
    if (io->thread() != thread()) {
        qWarning("[FlatModel] setIO: rejected backend thread mismatch (backend=%s, backendThread=%p, modelThread=%p, url=\"%s\")",
                 io->metaObject()->className(),
                 static_cast<void *>(io->thread()),
                 static_cast<void *>(thread()), qPrintable(m_url));
        return;
    }
    m_io = io;
    m_iconCache.clear();
    reload();
}

void QtFileMgrFlatModel::setIOAndUrl(QtFileMgrIO *io, const QString &path)
{
    if (!io)
        return;
    if (io->thread() != thread()) {
        qWarning("[FlatModel] setIOAndUrl: rejected backend thread mismatch (backend=%s, backendThread=%p, modelThread=%p, url=\"%s\")",
                 io->metaObject()->className(),
                 static_cast<void *>(io->thread()),
                 static_cast<void *>(thread()), qPrintable(path));
        return;
    }
    m_io = io;
    m_url = path;
    m_iconCache.clear();
    reload();
}

void QtFileMgrFlatModel::setUrl(const QString &path)
{
    m_url = path;
    reload();
}

void QtFileMgrFlatModel::setNameFilter(const QString &text)
{
    if (m_filter == text) return;
    m_filter = text;
    rebuild();
}

void QtFileMgrFlatModel::setFilterMode(int mode)
{
    int requestedMode = mode;
    mode = qBound(0, mode, 2);
    if (requestedMode != mode)
        qWarning("[FlatModel] setFilterMode: clamped invalid mode %d to %d", requestedMode, mode);
    if (m_filterMode == mode) return;
    m_filterMode = mode;
    rebuild();
}

void QtFileMgrFlatModel::setCaseSensitive(bool on)
{
    if (m_caseSensitive == on) return;
    m_caseSensitive = on;
    rebuild();
}

void QtFileMgrFlatModel::setFoldersFirst(bool on) { m_foldersFirst = on; rebuild(); }
void QtFileMgrFlatModel::setShowHidden(bool on)  { m_showHidden = on; rebuild(); }

void QtFileMgrFlatModel::setShowThumbnails(bool on)
{
    if (m_showThumbnails == on)
        return;
    m_showThumbnails = on;
    m_iconCache.clear();
    if (!m_items.isEmpty())
        emit dataChanged(index(0, 0), index(m_items.size() - 1, 0),
                         {Qt::DecorationRole});
}

void QtFileMgrFlatModel::setThumbnailSize(const QSize &size)
{
    if (size.isEmpty() || m_thumbnailSize == size)
        return;
    m_thumbnailSize = size;
    if (m_showThumbnails) {
        m_iconCache.clear();
        if (!m_items.isEmpty())
            emit dataChanged(index(0, 0), index(m_items.size() - 1, 0),
                             {Qt::DecorationRole});
    }
}

void QtFileMgrFlatModel::setShowVersions(bool on)
{
    if (m_showVersions == on)
        return;
    if (m_resetInProgress) {
        qWarning("[FlatModel] setShowVersions: coalesced update during reset (url=\"%s\", generation=%llu, visible=%d)",
                 qPrintable(m_url), static_cast<unsigned long long>(m_loadGeneration), on);
        m_showVersions = on;
        if (!on && m_sortColumn >= columnCount()) {
            qWarning("[FlatModel] setShowVersions: reset coalescing moved hidden sort column %d to 0 (url=\"%s\", generation=%llu)",
                     m_sortColumn, qPrintable(m_url), static_cast<unsigned long long>(m_loadGeneration));
            m_sortColumn = 0;
        }
        m_rebuildPending = true;
        return;
    }
    const QString resetPath = m_url;
    const quint64 resetGeneration = m_loadGeneration;
    QPointer<QtFileMgrFlatModel> guarded(this);
    m_resetInProgress = true;
    beginResetModel();
    if (!guarded) {
        qWarning("[FlatModel] setShowVersions: discarded reset after model destruction (source=modelAboutToBeReset, url=\"%s\", generation=%llu)",
                 qPrintable(resetPath), static_cast<unsigned long long>(resetGeneration));
        return;
    }
    m_showVersions = on;
    if (!on && m_sortColumn >= columnCount()) {
        // 版本列隐藏后排序列回落,避免按隐藏列排序与越界指示 (R2-FV-P2-1)
        qWarning("[FlatModel] setShowVersions: sort column %d out of range after hiding versions; falling back to 0",
                 m_sortColumn);
        m_sortColumn = 0;
    }
    endResetModel();
    if (guarded) {
        finishReset();
    } else {
        qWarning("[FlatModel] setShowVersions: discarded reset continuation after model destruction (source=modelReset, url=\"%s\", generation=%llu)",
                 qPrintable(resetPath), static_cast<unsigned long long>(resetGeneration));
    }
}

void QtFileMgrFlatModel::setSortColumn(int col, Qt::SortOrder order)
{
    // 模型层同样 clamp,越界列不得静默退化为按名排序 (R2-FV-P2-3)
    const int clamped = qBound(0, col, columnCount() - 1);
    if (clamped != col) {
        qWarning("[FlatModel] setSortColumn: clamped out-of-range column %d to %d (columns=%d)",
                 col, clamped, columnCount());
    }
    m_sortColumn = clamped;
    m_sortOrder = order;
    rebuild();
}

bool QtFileMgrFlatModel::isValidDisplayName(const QString &name)
{
    if (name.isEmpty()) return false;
    if (name == QLatin1String(".") || name == QLatin1String("..")) return false;
    for (QChar c : name) {
        if (c.isNull() || c == QLatin1Char('/'))
            return false;
#ifdef Q_OS_WIN
        // 反斜杠仅在 Windows 是分隔符;POSIX 下是合法文件名字符,
        // 全平台拒绝会把 POSIX 下含反斜杠的真实文件从列表中隐藏
        if (c == QLatin1Char('\\'))
            return false;
#endif
    }
    return true;
}

void QtFileMgrFlatModel::rebuild()
{
    if (m_resetInProgress) {
        qWarning("[FlatModel] rebuild: deferred reentrant reset (url=\"%s\", generation=%llu)",
                 qPrintable(m_url), static_cast<unsigned long long>(m_loadGeneration));
        m_rebuildPending = true;
        return;
    }
    const QString resetPath = m_url;
    const quint64 resetGeneration = m_loadGeneration;
    QPointer<QtFileMgrFlatModel> guarded(this);
    m_resetInProgress = true;
    beginResetModel();
    if (!guarded) {
        qWarning("[FlatModel] rebuild: discarded reset after model destruction (source=modelAboutToBeReset, url=\"%s\", generation=%llu)",
                 qPrintable(resetPath), static_cast<unsigned long long>(resetGeneration));
        return;
    }
    m_items.clear();
    m_directoryCount = 0;
    m_fileCount = 0;

    const Qt::CaseSensitivity cs = m_caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QRegularExpression::PatternOptions reOptions = m_caseSensitive
        ? QRegularExpression::NoPatternOption
        : QRegularExpression::CaseInsensitiveOption;
    QRegularExpression filterRegex;
    // 非法正则降级为字面包含匹配:此前直接返回空列表,用户输入到
    // 一半(如 "foo(")整个目录消失,像是数据丢失 (UI-P2-4)
    bool regexUsable = false;
    if (!m_filter.isEmpty() && (m_filterMode == 1 || m_filterMode == 2)) {
        const QString pattern = m_filterMode == 1
            ? QRegularExpression::wildcardToRegularExpression(m_filter) : m_filter;
        filterRegex = QRegularExpression(pattern, reOptions);
        regexUsable = filterRegex.isValid();
        if (!regexUsable) {
            qWarning("[FlatModel] rebuild: invalid regex filter '%s' (%s), falling back to literal contains match",
                     qPrintable(m_filter), qPrintable(filterRegex.errorString()));
        }
    }
    for (const FileEntry &e : m_sourceItems) {
        if (e.isHidden && !m_showHidden) continue;
        if (!isValidDisplayName(e.name)) {
            qWarning("[FlatModel] rebuild: skipping file with invalid display name \"%s\" in %s",
                     qPrintable(e.name), qPrintable(m_url));
            continue;
        }
        if (!m_filter.isEmpty()) {
            bool matched = false;
            switch (m_filterMode) {
            case 0: matched = e.name.contains(m_filter, cs); break;
            case 1: // Wildcard
            case 2: // Regex
                matched = regexUsable
                    ? filterRegex.match(e.name).hasMatch()
                    : e.name.contains(m_filter, cs);
                break;
            }
            if (!matched) continue;
        }
        m_items.append(e);
        e.isDir ? ++m_directoryCount : ++m_fileCount;
    }

    // 排序
    std::stable_sort(m_items.begin(), m_items.end(), [this](const FileEntry &a, const FileEntry &b) {
        if (m_foldersFirst && a.isDir != b.isDir) return a.isDir;
        bool asc = (m_sortOrder == Qt::AscendingOrder);
        int cmp = 0;
        switch (m_sortColumn) {
        case 0: cmp = compareEntryName(a, b); break;
        case 1:
            // 目录优先统一由开头 m_foldersFirst 分支处理;此处再强制
            // 会让关闭"文件夹优先"的大小排序仍然目录在前 (R2-FV-P2-2)
            cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            break;
        case 2: {
            QString as = fileTypeText(a);
            QString bs = fileTypeText(b);
            cmp = QString::compare(as, bs, Qt::CaseInsensitive); break;
        }
        case 3: cmp = (a.modified < b.modified) ? -1 : (a.modified > b.modified) ? 1 : 0; break;
        case 4:
            cmp = a.versionCount < b.versionCount ? -1
                : (a.versionCount > b.versionCount ? 1 : 0);
            break;
        }
        if (cmp == 0)
            return compareEntryName(a, b) < 0;
        return asc ? (cmp < 0) : (cmp > 0);
    });

    endResetModel();
    if (guarded) {
        finishReset();
    } else {
        qWarning("[FlatModel] rebuild: discarded reset continuation after model destruction (source=modelReset, url=\"%s\", generation=%llu)",
                 qPrintable(resetPath), static_cast<unsigned long long>(resetGeneration));
    }
}

void QtFileMgrFlatModel::finishReset()
{
    m_resetInProgress = false;
    if (m_reloadPending) {
        m_reloadPending = false;
        m_rebuildPending = false;
        reload();
        return;
    }
    if (m_rebuildPending) {
        m_rebuildPending = false;
        m_batchRebuildTimer->start();
    }
}

void QtFileMgrFlatModel::cancelActiveRequest()
{
    m_batchRebuildTimer->stop();
    if (!m_activeRequest)
        return;
    const QPointer<QtFileMgrDirectoryRequest> request = m_activeRequest;
    m_activeRequest.clear();
    disconnect(request.data(), nullptr, this, nullptr);
    request->cancel();
}

void QtFileMgrFlatModel::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged(loading);
}

void QtFileMgrFlatModel::reload()
{
    // Invalidate the old callbacks before reset/loading signals can navigate again.
    const quint64 generation = ++m_loadGeneration;
    const QString path = m_url;
    QPointer<QtFileMgrFlatModel> guarded(this);
    const auto current = [guarded, generation, path](const char *source) {
        if (!guarded) {
            qWarning("[FlatModel] reload: discarded continuation after model destruction (source=%s, url=\"%s\", generation=%llu)",
                     source, qPrintable(path), static_cast<unsigned long long>(generation));
            return false;
        }
        if (generation != guarded->m_loadGeneration) {
            qWarning("[FlatModel] reload: discarded superseded continuation (source=%s, url=\"%s\", generation=%llu, currentUrl=\"%s\", currentGeneration=%llu)",
                     source, qPrintable(path), static_cast<unsigned long long>(generation),
                     qPrintable(guarded->m_url),
                     static_cast<unsigned long long>(guarded->m_loadGeneration));
            return false;
        }
        return true;
    };
    cancelActiveRequest();
    if (m_resetInProgress) {
        qWarning("[FlatModel] reload: deferred navigation during reset (source=reload, url=\"%s\", generation=%llu)",
                 qPrintable(path), static_cast<unsigned long long>(generation));
        m_reloadPending = true;
        return;
    }

    m_resetInProgress = true;
    beginResetModel();
    if (!guarded) {
        current("modelAboutToBeReset");
        return;
    }
    m_sourceItems.clear();
    m_items.clear();
    m_directoryCount = 0;
    m_fileCount = 0;
    m_iconCache.clear();
    endResetModel();
    if (!guarded) {
        current("modelReset");
        return;
    }
    finishReset();
    if (!current("initial reset"))
        return;

    const auto failBeforeRequest = [guarded, current, path](const QString &error) {
        guarded->setLoading(false);
        if (current("loadingChanged(false) before request"))
            emit guarded->directoryLoadFinished(path, error);
    };
    if (!m_io) {
        const QString error = QStringLiteral("缺少文件后端");
        qWarning("[FlatModel] reload: missing IO (source=reload, url=\"%s\", generation=%llu)",
                 qPrintable(path), static_cast<unsigned long long>(generation));
        failBeforeRequest(error);
        return;
    }
    if (m_io->thread() != thread()) {
        const QString error = QStringLiteral("文件后端线程与模型线程不一致");
        qWarning("[FlatModel] reload: rejected backend thread mismatch (backend=%s, backendThread=%p, modelThread=%p, url=\"%s\", generation=%llu)",
                 m_io->metaObject()->className(), static_cast<void *>(m_io->thread()),
                 static_cast<void *>(thread()), qPrintable(path),
                 static_cast<unsigned long long>(generation));
        failBeforeRequest(error);
        return;
    }

    setLoading(true);
    if (!current("loadingChanged(true)"))
        return;
    if (!m_io) {
        const QString error = QStringLiteral("文件后端在目录请求前被销毁");
        qWarning("[FlatModel] reload: backend destroyed before request (source=loadingChanged, url=\"%s\", generation=%llu)",
                 qPrintable(path), static_cast<unsigned long long>(generation));
        failBeforeRequest(error);
        return;
    }
    QtFileMgrDirectoryRequest *returnedRequest = m_io->listDirAsync(path, this);
    // The backend may have run host callbacks before returning. Do not even
    // touch the returned child pointer if those callbacks destroyed this model.
    if (!guarded) {
        current("listDirAsync return");
        return;
    }
    QPointer<QtFileMgrDirectoryRequest> request(returnedRequest);
    if (!current("listDirAsync return")) {
        if (request) {
            request->cancel();
            request->deleteLater();
        }
        return;
    }
    if (!request) {
        const QString error = QStringLiteral("文件后端未创建目录读取请求");
        qWarning("[FlatModel] reload: backend returned no directory request (source=listDirAsync, url=\"%s\", generation=%llu)",
                 qPrintable(path), static_cast<unsigned long long>(generation));
        failBeforeRequest(error);
        return;
    }
    m_activeRequest = request;
    struct LoadState { bool completed = false; };
    const auto state = std::make_shared<LoadState>();
    const auto complete = [guarded, current, request, state, path](const QString &error) {
        if (!current("directory completion"))
            return;
        if (state->completed) {
            qWarning("[FlatModel] reload: discarded duplicate completion (source=directory completion, url=\"%s\", generation=%llu)",
                     qPrintable(path), static_cast<unsigned long long>(guarded->m_loadGeneration));
            return;
        }
        state->completed = true;
        const QString finalError = error;
        if (request)
            QObject::disconnect(request.data(), nullptr, guarded.data(), nullptr);
        guarded->m_activeRequest.clear();
        guarded->m_batchRebuildTimer->stop();
        guarded->rebuild();
        if (!current("completion modelReset"))
            return;
        guarded->setLoading(false);
        if (current("completion loadingChanged(false)"))
            emit guarded->directoryLoadFinished(path, finalError);
    };
    const auto acceptEntries = [guarded, current, state, path](const QList<FileEntry> &entries) {
        if (!current("entriesReady"))
            return;
        if (state->completed) {
            qWarning("[FlatModel] reload: discarded entries after completion (source=entriesReady, url=\"%s\", generation=%llu, count=%d)",
                     qPrintable(path), static_cast<unsigned long long>(guarded->m_loadGeneration), entries.size());
            return;
        }
        if (entries.isEmpty())
            return;
        guarded->m_sourceItems.append(entries);
        emit guarded->directoryBatchLoaded(entries.size(), guarded->m_sourceItems.size());
        if (!current("directoryBatchLoaded"))
            return;
        // A batch slot can synchronously finish the request as well as navigate.
        if (!state->completed && !guarded->m_batchRebuildTimer->isActive())
            guarded->m_batchRebuildTimer->start();
    };
    connect(request.data(), &QtFileMgrDirectoryRequest::entriesReady, this, acceptEntries);
    connect(request.data(), &QtFileMgrDirectoryRequest::finished, this, complete);
    connect(request.data(), &QObject::destroyed, this,
            [current, state, complete, path, generation]() {
        if (state->completed || !current("request destroyed"))
            return;
        const QString error = QStringLiteral("目录读取请求在完成前被销毁");
        qWarning("[FlatModel] reload: directory request destroyed before finish (source=request destroyed, url=\"%s\", generation=%llu)",
                 qPrintable(path), static_cast<unsigned long long>(generation));
        complete(error);
    });

    // Include batches emitted before listDirAsync returned, even if unfinished.
    // Read all request data before notifying host slots, which may destroy it.
    const QList<FileEntry> delivered = request->deliveredEntries();
    const bool alreadyFinished = request->isFinished();
    const QString error = request->errorString();
    acceptEntries(delivered);
    if (!current("synchronous result replay") || state->completed)
        return;
    if (alreadyFinished)
        complete(error);
}
