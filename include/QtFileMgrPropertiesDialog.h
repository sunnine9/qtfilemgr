// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRPROPERTIESDIALOG_H
#define QTFILEMGRPROPERTIESDIALOG_H

#include <QDialog>
#include <QFile>
#include <QPointer>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QtFileMgrIO;

class QtFileMgrPropertiesDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrPropertiesDialog(QtFileMgrIO *io,
                                       const QStringList &paths,
                                       QWidget *parent = nullptr);

    QStringList paths() const { return m_paths; }
    int itemCount() const { return m_paths.size(); }

signals:
    void permissionsApplied(const QStringList &paths);

private slots:
    void applyPermissions();

private:
    void buildSummary();
    void buildPermissions();
    Qt::CheckState commonPermissionState(QFile::Permission permission) const;

    QPointer<QtFileMgrIO> m_io;
    QStringList m_paths;
    QList<QFile::Permissions> m_originalPermissions;
    QLabel *m_nameValue = nullptr;
    QLabel *m_typeValue = nullptr;
    QLabel *m_locationValue = nullptr;
    QLabel *m_sizeValue = nullptr;
    QLabel *m_modifiedValue = nullptr;
    QLabel *m_ownerValue = nullptr;
    QLabel *m_linkTargetValue = nullptr;
    QList<QCheckBox *> m_permissionChecks;
    QDialogButtonBox *m_buttons = nullptr;
};

#endif
