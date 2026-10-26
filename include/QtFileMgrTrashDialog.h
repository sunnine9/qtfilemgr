// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRTRASHDIALOG_H
#define QTFILEMGRTRASHDIALOG_H

#include <QDialog>
#include <QStringList>

class QTreeWidget;
class QLabel;

struct QtFileMgrTrashEntry {
    QString name;
    QString trashedPath;
    QString originalPath;
    QString deletionDate;
    QString trashRoot;
    QString infoPath;
    qint64 size = 0;
    bool isDir = false;
    bool metadataValid = false;
};

class QtFileMgrTrashDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrTrashDialog(QWidget *parent = nullptr);
    explicit QtFileMgrTrashDialog(const QStringList &trashRoots,
                                  QWidget *parent = nullptr);

    static QString trashRootPath();
    static QStringList trashRootPaths();
    static QList<QtFileMgrTrashEntry> entries();
    static QList<QtFileMgrTrashEntry> entriesForRoots(
        const QStringList &trashRoots);
    static bool restore(const QtFileMgrTrashEntry &entry, QString *error = nullptr);
    static bool emptyTrash(QString *error = nullptr);
    static bool emptyTrashRoots(const QStringList &trashRoots,
                                QString *error = nullptr);
    int entryCount() const;

signals:
    void entryRestored(const QString &path);
    void trashChanged();

private slots:
    void reload();
    void restoreSelected();
    void empty();

private:
    QTreeWidget *m_items = nullptr;
    QLabel *m_status = nullptr;
    class QPushButton *m_restoreButton = nullptr;
    class QPushButton *m_emptyButton = nullptr;
    QStringList m_trashRoots;
};

Q_DECLARE_METATYPE(QtFileMgrTrashEntry)

#endif
