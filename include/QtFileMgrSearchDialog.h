// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRSEARCHDIALOG_H
#define QTFILEMGRSEARCHDIALOG_H

#include <QDateTime>
#include <QDialog>
#include <QPointer>
#include <QQueue>
#include <QSet>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QTreeWidget;
class QLabel;
class QPushButton;
class QTimer;
class QtFileMgrDirectoryRequest;
class QtFileMgrIO;
struct FileEntry;

class QtFileMgrSearchDialog : public QDialog {
    Q_OBJECT
public:
    enum TypeFilter {
        AnyType = 0,
        FilesOnly,
        DirectoriesOnly,
        Images,
        Documents,
        Audio,
        Video
    };

    explicit QtFileMgrSearchDialog(QtFileMgrIO *io,
                                   const QString &rootPath,
                                   QWidget *parent = nullptr);
    ~QtFileMgrSearchDialog() override;

    bool isSearching() const { return m_searching; }
    int resultCount() const { return m_resultCount; }
    QStringList resultPaths() const;
    QStringList searchHistory() const;
    void setSearchCriteria(const QString &query, TypeFilter type,
                           qint64 minimumBytes = -1,
                           qint64 maximumBytes = -1,
                           const QDateTime &modifiedAfter = QDateTime(),
                           const QDateTime &modifiedBefore = QDateTime(),
                           bool recursive = true);

signals:
    void pathActivated(const QString &path, bool directory);
    void searchStarted();
    void searchFinished(bool cancelled, int resultCount);

public slots:
    void startSearch();
    void stopSearch();

private slots:
    void processNextDirectory();

private:
    struct Criteria {
        QString query;
        TypeFilter type = AnyType;
        qint64 minimumBytes = -1;
        qint64 maximumBytes = -1;
        QDateTime modifiedAfter;
        QDateTime modifiedBefore;
        bool recursive = true;
    };

    void cancelActiveRequest();
    void finishSearch(bool cancelled);
    void processEntries(const QList<FileEntry> &entries);
    bool matches(const FileEntry &entry, QString *mimeType) const;
    void addResult(const FileEntry &entry, const QString &mimeType);
    void loadHistory();
    void storeQueryInHistory(const QString &query);
    bool hasMeaningfulCriteria(const Criteria &criteria) const;

    QPointer<QtFileMgrIO> m_io;
    QString m_rootPath;
    Criteria m_activeCriteria;
    QComboBox *m_query = nullptr;
    QCheckBox *m_recursive = nullptr;
    QComboBox *m_type = nullptr;
    QDoubleSpinBox *m_minimumSize = nullptr;
    QDoubleSpinBox *m_maximumSize = nullptr;
    QCheckBox *m_afterEnabled = nullptr;
    QCheckBox *m_beforeEnabled = nullptr;
    QDateEdit *m_modifiedAfter = nullptr;
    QDateEdit *m_modifiedBefore = nullptr;
    QTreeWidget *m_results = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_searchButton = nullptr;
    QTimer *m_timer = nullptr;
    QQueue<QString> m_pendingDirectories;
    QSet<QString> m_visitedDirectories;
    QtFileMgrDirectoryRequest *m_activeRequest = nullptr;
    quint64 m_searchGeneration = 0;
    int m_scannedDirectories = 0;
    int m_resultCount = 0;
    bool m_searching = false;
    bool m_limitWarningIssued = false;
};

#endif
