// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrSearchDialog.h"

#include "QtFileMgrIO.h"
#include "QtFileMgrConfigStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const int kMaximumResults = 5000;
const int kNumericSortRole = Qt::UserRole + 10;

class SearchResultItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 3 || column == 4)
            return data(column, kNumericSortRole).toLongLong()
                < other.data(column, kNumericSortRole).toLongLong();
        return QTreeWidgetItem::operator<(other);
    }
};

QString parentPath(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash == 0)
        return QStringLiteral("/");
    return slash > 0 ? path.left(slash) : QString();
}

bool mimeMatchesType(const QString &mimeType,
                     QtFileMgrSearchDialog::TypeFilter type)
{
    switch (type) {
    case QtFileMgrSearchDialog::Images:
        return mimeType.startsWith(QStringLiteral("image/"));
    case QtFileMgrSearchDialog::Audio:
        return mimeType.startsWith(QStringLiteral("audio/"));
    case QtFileMgrSearchDialog::Video:
        return mimeType.startsWith(QStringLiteral("video/"));
    case QtFileMgrSearchDialog::Documents:
        return mimeType.startsWith(QStringLiteral("text/"))
            || mimeType == QStringLiteral("application/pdf")
            || mimeType.contains(QStringLiteral("document"))
            || mimeType.contains(QStringLiteral("spreadsheet"))
            || mimeType.contains(QStringLiteral("presentation"))
            || mimeType.contains(QStringLiteral("json"))
            || mimeType.contains(QStringLiteral("xml"));
    default:
        return true;
    }
}

} // namespace

QtFileMgrSearchDialog::QtFileMgrSearchDialog(
    QtFileMgrIO *io, const QString &rootPath, QWidget *parent)
    : QDialog(parent)
    , m_io(io)
    , m_rootPath(rootPath)
    , m_query(new QComboBox(this))
    , m_recursive(new QCheckBox(QStringLiteral("包含子文件夹"), this))
    , m_type(new QComboBox(this))
    , m_minimumSize(new QDoubleSpinBox(this))
    , m_maximumSize(new QDoubleSpinBox(this))
    , m_afterEnabled(new QCheckBox(QStringLiteral("修改时间晚于"), this))
    , m_beforeEnabled(new QCheckBox(QStringLiteral("早于"), this))
    , m_modifiedAfter(new QDateEdit(this))
    , m_modifiedBefore(new QDateEdit(this))
    , m_results(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_searchButton(new QPushButton(QStringLiteral("搜索"), this))
    , m_timer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("搜索文件"));
    resize(900, 600);
    auto *layout = new QVBoxLayout(this);
    auto *criteria = new QGridLayout;
    criteria->setColumnStretch(1, 1);

    m_query->setObjectName(QStringLiteral("searchQuery"));
    m_query->setEditable(true);
    m_query->setInsertPolicy(QComboBox::NoInsert);
    m_query->lineEdit()->setPlaceholderText(QStringLiteral("文件名包含"));
    m_query->lineEdit()->setClearButtonEnabled(true);
    criteria->addWidget(new QLabel(QStringLiteral("名称"), this), 0, 0);
    criteria->addWidget(m_query, 0, 1, 1, 3);
    m_recursive->setChecked(true);
    criteria->addWidget(m_recursive, 0, 4);
    criteria->addWidget(m_searchButton, 0, 5);

    m_type->setObjectName(QStringLiteral("searchType"));
    m_type->addItem(QStringLiteral("所有类型"), AnyType);
    m_type->addItem(QStringLiteral("仅文件"), FilesOnly);
    m_type->addItem(QStringLiteral("仅文件夹"), DirectoriesOnly);
    m_type->addItem(QStringLiteral("图片"), Images);
    m_type->addItem(QStringLiteral("文档"), Documents);
    m_type->addItem(QStringLiteral("音频"), Audio);
    m_type->addItem(QStringLiteral("视频"), Video);
    criteria->addWidget(new QLabel(QStringLiteral("类型"), this), 1, 0);
    criteria->addWidget(m_type, 1, 1);

    for (QDoubleSpinBox *size : {m_minimumSize, m_maximumSize}) {
        size->setDecimals(1);
        size->setRange(0.0, 1024.0 * 1024.0);
        size->setSuffix(QStringLiteral(" MB"));
        size->setSpecialValueText(QStringLiteral("不限"));
    }
    m_minimumSize->setObjectName(QStringLiteral("searchMinimumSize"));
    m_maximumSize->setObjectName(QStringLiteral("searchMaximumSize"));
    criteria->addWidget(new QLabel(QStringLiteral("大小"), this), 1, 2);
    criteria->addWidget(m_minimumSize, 1, 3);
    criteria->addWidget(new QLabel(QStringLiteral("至"), this), 1, 4);
    criteria->addWidget(m_maximumSize, 1, 5);

    m_modifiedAfter->setObjectName(QStringLiteral("searchModifiedAfter"));
    m_modifiedBefore->setObjectName(QStringLiteral("searchModifiedBefore"));
    m_modifiedAfter->setCalendarPopup(true);
    m_modifiedBefore->setCalendarPopup(true);
    m_modifiedAfter->setDate(QDate::currentDate().addMonths(-1));
    m_modifiedBefore->setDate(QDate::currentDate());
    m_modifiedAfter->setEnabled(false);
    m_modifiedBefore->setEnabled(false);
    connect(m_afterEnabled, &QCheckBox::toggled,
            m_modifiedAfter, &QDateEdit::setEnabled);
    connect(m_beforeEnabled, &QCheckBox::toggled,
            m_modifiedBefore, &QDateEdit::setEnabled);
    criteria->addWidget(m_afterEnabled, 2, 0);
    criteria->addWidget(m_modifiedAfter, 2, 1);
    criteria->addWidget(m_beforeEnabled, 2, 2);
    criteria->addWidget(m_modifiedBefore, 2, 3);
    layout->addLayout(criteria);

    m_results->setObjectName(QStringLiteral("searchResults"));
    m_results->setHeaderLabels({
        QStringLiteral("名称"), QStringLiteral("位置"), QStringLiteral("类型"),
        QStringLiteral("大小"), QStringLiteral("修改时间")
    });
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setSortingEnabled(true);
    m_results->header()->setStretchLastSection(false);
    m_results->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    layout->addWidget(m_results, 1);
    m_status->setObjectName(QStringLiteral("searchStatus"));
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout,
            this, &QtFileMgrSearchDialog::processNextDirectory);
    connect(m_searchButton, &QPushButton::clicked, this, [this]() {
        isSearching() ? stopSearch() : startSearch();
    });
    connect(m_query->lineEdit(), &QLineEdit::returnPressed,
            this, &QtFileMgrSearchDialog::startSearch);
    connect(m_results, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) {
        emit pathActivated(item->data(0, Qt::UserRole).toString(),
                           item->data(0, Qt::UserRole + 1).toBool());
        accept();
    });
    connect(this, &QDialog::finished, this, [this]() {
        if (m_searching)
            stopSearch();
    });
    loadHistory();
    m_query->setFocus();
}

QtFileMgrSearchDialog::~QtFileMgrSearchDialog()
{
    cancelActiveRequest();
}

void QtFileMgrSearchDialog::setSearchCriteria(
    const QString &query, TypeFilter type, qint64 minimumBytes,
    qint64 maximumBytes, const QDateTime &modifiedAfter,
    const QDateTime &modifiedBefore, bool recursive)
{
    m_query->setEditText(query);
    const int typeIndex = m_type->findData(type);
    if (typeIndex < 0) {
        qWarning("[SearchDialog] criteria: invalid type filter %d; using AnyType",
                 static_cast<int>(type));
        m_type->setCurrentIndex(0);
    } else {
        m_type->setCurrentIndex(typeIndex);
    }
    m_minimumSize->setValue(minimumBytes > 0
        ? minimumBytes / (1024.0 * 1024.0) : 0.0);
    m_maximumSize->setValue(maximumBytes > 0
        ? maximumBytes / (1024.0 * 1024.0) : 0.0);
    m_afterEnabled->setChecked(modifiedAfter.isValid());
    m_beforeEnabled->setChecked(modifiedBefore.isValid());
    if (modifiedAfter.isValid())
        m_modifiedAfter->setDate(modifiedAfter.date());
    if (modifiedBefore.isValid())
        m_modifiedBefore->setDate(modifiedBefore.date());
    m_recursive->setChecked(recursive);
}

QStringList QtFileMgrSearchDialog::resultPaths() const
{
    QStringList paths;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i)
        paths << m_results->topLevelItem(i)->data(0, Qt::UserRole).toString();
    return paths;
}

QStringList QtFileMgrSearchDialog::searchHistory() const
{
    QStringList history;
    for (int i = 0; i < m_query->count(); ++i)
        history << m_query->itemText(i);
    return history;
}

bool QtFileMgrSearchDialog::hasMeaningfulCriteria(
    const Criteria &criteria) const
{
    return !criteria.query.isEmpty() || criteria.type != AnyType
        || criteria.minimumBytes >= 0 || criteria.maximumBytes >= 0
        || criteria.modifiedAfter.isValid()
        || criteria.modifiedBefore.isValid();
}

void QtFileMgrSearchDialog::startSearch()
{
    if (m_searching)
        finishSearch(true);
    m_results->clear();
    m_scannedDirectories = 0;
    m_resultCount = 0;
    m_limitWarningIssued = false;
    if (!m_io) {
        qWarning("[SearchDialog] start: missing IO for root '%s'",
                 qPrintable(m_rootPath));
        m_status->setText(QStringLiteral("缺少文件后端"));
        return;
    }

    m_activeCriteria.query = m_query->currentText().trimmed();
    m_activeCriteria.type = static_cast<TypeFilter>(m_type->currentData().toInt());
    m_activeCriteria.minimumBytes = m_minimumSize->value() > 0.0
        ? static_cast<qint64>(m_minimumSize->value() * 1024.0 * 1024.0) : -1;
    m_activeCriteria.maximumBytes = m_maximumSize->value() > 0.0
        ? static_cast<qint64>(m_maximumSize->value() * 1024.0 * 1024.0) : -1;
    m_activeCriteria.modifiedAfter = m_afterEnabled->isChecked()
        ? QDateTime(m_modifiedAfter->date(), QTime(0, 0, 0)) : QDateTime();
    m_activeCriteria.modifiedBefore = m_beforeEnabled->isChecked()
        ? QDateTime(m_modifiedBefore->date(), QTime(23, 59, 59, 999)) : QDateTime();
    m_activeCriteria.recursive = m_recursive->isChecked();
    if (!hasMeaningfulCriteria(m_activeCriteria)) {
        qWarning("[SearchDialog] start: rejected unconstrained search under '%s'",
                 qPrintable(m_rootPath));
        m_status->setText(QStringLiteral("请至少输入一个搜索条件"));
        return;
    }
    if (m_activeCriteria.minimumBytes >= 0
        && m_activeCriteria.maximumBytes >= 0
        && m_activeCriteria.minimumBytes > m_activeCriteria.maximumBytes) {
        qWarning("[SearchDialog] start: invalid size range %lld..%lld under '%s'",
                 static_cast<long long>(m_activeCriteria.minimumBytes),
                 static_cast<long long>(m_activeCriteria.maximumBytes),
                 qPrintable(m_rootPath));
        m_status->setText(QStringLiteral("最小大小不能大于最大大小"));
        return;
    }
    if (m_activeCriteria.modifiedAfter.isValid()
        && m_activeCriteria.modifiedBefore.isValid()
        && m_activeCriteria.modifiedAfter > m_activeCriteria.modifiedBefore) {
        qWarning("[SearchDialog] start: invalid date range '%s'..'%s' under '%s'",
                 qPrintable(m_activeCriteria.modifiedAfter.toString(Qt::ISODate)),
                 qPrintable(m_activeCriteria.modifiedBefore.toString(Qt::ISODate)),
                 qPrintable(m_rootPath));
        m_status->setText(QStringLiteral("开始日期不能晚于结束日期"));
        return;
    }

    storeQueryInHistory(m_activeCriteria.query);
    ++m_searchGeneration;
    m_pendingDirectories.enqueue(m_rootPath);
    m_searching = true;
    m_searchButton->setText(QStringLiteral("停止"));
    m_status->setText(QStringLiteral("正在搜索..."));
    emit searchStarted();
    m_timer->start(0);
}

void QtFileMgrSearchDialog::cancelActiveRequest()
{
    if (!m_activeRequest)
        return;
    disconnect(m_activeRequest, nullptr, this, nullptr);
    m_activeRequest->cancel();
    m_activeRequest = nullptr;
}

void QtFileMgrSearchDialog::stopSearch()
{
    if (!m_searching)
        return;
    finishSearch(true);
}

void QtFileMgrSearchDialog::finishSearch(bool cancelled)
{
    cancelActiveRequest();
    m_timer->stop();
    m_pendingDirectories.clear();
    m_visitedDirectories.clear();
    ++m_searchGeneration;
    const bool wasSearching = m_searching;
    m_searching = false;
    m_searchButton->setText(QStringLiteral("搜索"));
    m_status->setText(cancelled
        ? QStringLiteral("已停止：扫描 %1 个文件夹，找到 %2 项")
              .arg(m_scannedDirectories).arg(m_resultCount)
        : QStringLiteral("完成：扫描 %1 个文件夹，找到 %2 项")
              .arg(m_scannedDirectories).arg(m_resultCount));
    if (wasSearching)
        emit searchFinished(cancelled, m_resultCount);
}

void QtFileMgrSearchDialog::processNextDirectory()
{
    if (!m_searching)
        return;
    if (!m_io || m_pendingDirectories.isEmpty()) {
        finishSearch(false);
        return;
    }
    const QString directory = m_pendingDirectories.dequeue();
    if (m_visitedDirectories.contains(directory)) {
        qWarning("[SearchDialog] traversal: skipped already visited directory '%s' from root '%s'",
                 qPrintable(directory), qPrintable(m_rootPath));
        m_timer->start(0);
        return;
    }
    m_visitedDirectories.insert(directory);
    ++m_scannedDirectories;
    const quint64 generation = m_searchGeneration;
    QtFileMgrDirectoryRequest *request = m_io->listDirAsync(directory, this);
    if (!request) {
        qWarning("[SearchDialog] traversal: backend '%s' returned no request for '%s'",
                 m_io->metaObject()->className(), qPrintable(directory));
        m_timer->start(0);
        return;
    }
    m_activeRequest = request;
    connect(request, &QtFileMgrDirectoryRequest::entriesReady,
            this, [this, generation](const QList<FileEntry> &entries) {
        if (!m_searching || generation != m_searchGeneration) {
            qWarning("[SearchDialog] result: skipped stale batch of %d entries (generation=%llu, current=%llu)",
                     entries.size(), static_cast<unsigned long long>(generation),
                     static_cast<unsigned long long>(m_searchGeneration));
            return;
        }
        processEntries(entries);
    });
    connect(request, &QtFileMgrDirectoryRequest::finished,
            this, [this, generation, request, directory](const QString &error) {
        if (generation != m_searchGeneration || !m_searching)
            return;
        if (m_activeRequest == request)
            m_activeRequest = nullptr;
        if (!error.isEmpty()) {
            qWarning("[SearchDialog] traversal: failed listing '%s': %s",
                     qPrintable(directory), qPrintable(error));
        }
        m_status->setText(QStringLiteral("已扫描 %1 个文件夹，找到 %2 项")
                              .arg(m_scannedDirectories).arg(m_resultCount));
        m_timer->start(0);
    });
    // 同步后端可能在连接前已完成: 补偿消费缓存结果,否则遍历永久
    // 停在搜索中 (审查#14; 与 FlatModel::reload 同一模式)
    if (request->isFinished()) {
        const QList<FileEntry> delivered = request->deliveredEntries();
        if (!delivered.isEmpty() && m_searching
            && generation == m_searchGeneration)
            processEntries(delivered);
        if (m_activeRequest == request)
            m_activeRequest = nullptr;
        const QString error = request->errorString();
        if (!error.isEmpty()) {
            qWarning("[SearchDialog] traversal: failed listing '%s': %s",
                     qPrintable(directory), qPrintable(error));
        }
        m_status->setText(QStringLiteral("已扫描 %1 个文件夹，找到 %2 项")
                              .arg(m_scannedDirectories).arg(m_resultCount));
        m_timer->start(0);
    }
}

bool QtFileMgrSearchDialog::matches(
    const FileEntry &entry, QString *mimeType) const
{
    if (!m_activeCriteria.query.isEmpty()
        && !entry.name.contains(m_activeCriteria.query, Qt::CaseInsensitive)) {
        return false;
    }
    if (m_activeCriteria.type == FilesOnly && entry.isDir)
        return false;
    if (m_activeCriteria.type == DirectoriesOnly && !entry.isDir)
        return false;
    if ((m_activeCriteria.minimumBytes >= 0
         || m_activeCriteria.maximumBytes >= 0) && entry.isDir) {
        return false;
    }
    if (m_activeCriteria.minimumBytes >= 0
        && entry.size < m_activeCriteria.minimumBytes) {
        return false;
    }
    if (m_activeCriteria.maximumBytes >= 0
        && entry.size > m_activeCriteria.maximumBytes) {
        return false;
    }
    if (m_activeCriteria.modifiedAfter.isValid()
        && (!entry.modified.isValid()
            || entry.modified < m_activeCriteria.modifiedAfter)) {
        return false;
    }
    if (m_activeCriteria.modifiedBefore.isValid()
        && (!entry.modified.isValid()
            || entry.modified > m_activeCriteria.modifiedBefore)) {
        return false;
    }
    if (entry.isDir) {
        *mimeType = QStringLiteral("inode/directory");
    } else {
        *mimeType = m_io->mimeType(entry.path);
    }
    if (m_activeCriteria.type >= Images
        && !mimeMatchesType(*mimeType, m_activeCriteria.type)) {
        return false;
    }
    return true;
}

void QtFileMgrSearchDialog::processEntries(const QList<FileEntry> &entries)
{
    const bool sorting = m_results->isSortingEnabled();
    m_results->setSortingEnabled(false);
    for (const FileEntry &entry : entries) {
        if (entry.name.isEmpty() || entry.path.isEmpty()) {
            qWarning("[SearchDialog] result: skipped incomplete entry name='%s' path='%s' from root '%s'",
                     qPrintable(entry.name), qPrintable(entry.path),
                     qPrintable(m_rootPath));
            continue;
        }
        QString mimeType;
        if (matches(entry, &mimeType) && m_resultCount < kMaximumResults)
            addResult(entry, mimeType);
        else if (m_resultCount >= kMaximumResults && !m_limitWarningIssued) {
            m_limitWarningIssued = true;
            qWarning("[SearchDialog] results: capped at %d items for root '%s' and query '%s'",
                     kMaximumResults, qPrintable(m_rootPath),
                     qPrintable(m_activeCriteria.query));
        }
        if (m_activeCriteria.recursive && entry.isDir && !entry.isSymlink) {
            if (m_visitedDirectories.contains(entry.path)
                || m_pendingDirectories.contains(entry.path)) {
                qWarning("[SearchDialog] traversal: skipped duplicate child '%s' from '%s'",
                         qPrintable(entry.path), qPrintable(m_rootPath));
            } else {
                m_pendingDirectories.enqueue(entry.path);
            }
        } else if (m_activeCriteria.recursive && entry.isDir && entry.isSymlink) {
            qWarning("[SearchDialog] traversal: skipped directory symlink '%s'",
                     qPrintable(entry.path));
        }
    }
    m_results->setSortingEnabled(sorting);
}

void QtFileMgrSearchDialog::addResult(
    const FileEntry &entry, const QString &mimeType)
{
    auto *item = new SearchResultItem(m_results);
    item->setText(0, entry.name);
    item->setText(1, parentPath(entry.path));
    item->setText(2, entry.isDir ? QStringLiteral("文件夹") : mimeType);
    item->setText(3, entry.isDir ? QString() : formatFileSize(entry.size));
    item->setText(4, entry.modified.isValid()
        ? QLocale::system().toString(entry.modified, QLocale::ShortFormat)
        : QString());
    item->setData(0, Qt::UserRole, entry.path);
    item->setData(0, Qt::UserRole + 1, entry.isDir);
    item->setData(3, kNumericSortRole, entry.isDir ? -1 : entry.size);
    item->setData(4, kNumericSortRole,
                  entry.modified.isValid() ? entry.modified.toMSecsSinceEpoch() : 0);
    const QIcon icon = m_io->fileIcon(entry.path);
    if (!icon.isNull())
        item->setIcon(0, icon);
    else
        qWarning("[SearchDialog] result: backend returned no icon for '%s'",
                 qPrintable(entry.path));
    ++m_resultCount;
}

void QtFileMgrSearchDialog::loadHistory()
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.beginGroup(QStringLiteral("Search"));
    const QStringList stored = settings.value(QStringLiteral("history")).toStringList();
    settings.endGroup();
    QSet<QString> seen;
    for (const QString &query : stored) {
        const QString trimmed = query.trimmed();
        if (trimmed.isEmpty()) {
            qWarning("[SearchDialog] history: skipped empty stored query");
            continue;
        }
        if (seen.contains(trimmed)) {
            qWarning("[SearchDialog] history: skipped duplicate stored query '%s'",
                     qPrintable(trimmed));
            continue;
        }
        if (m_query->count() >= 20) {
            qWarning("[SearchDialog] history: skipped '%s' because history is capped at 20",
                     qPrintable(trimmed));
            continue;
        }
        seen.insert(trimmed);
        m_query->addItem(trimmed);
    }
    m_query->setEditText(QString());
}

void QtFileMgrSearchDialog::storeQueryInHistory(const QString &query)
{
    if (query.isEmpty())
        return;
    QStringList history = searchHistory();
    if (history.removeAll(query) > 0) {
        qWarning("[SearchDialog] history: moved duplicate query '%s' to the front",
                 qPrintable(query));
    }
    history.prepend(query);
    while (history.size() > 20) {
        qWarning("[SearchDialog] history: dropped oldest query '%s' at capacity 20",
                 qPrintable(history.last()));
        history.removeLast();
    }
    m_query->clear();
    m_query->addItems(history);
    m_query->setEditText(query);
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.beginGroup(QStringLiteral("Search"));
    settings.setValue(QStringLiteral("history"), history);
    settings.endGroup();
}
