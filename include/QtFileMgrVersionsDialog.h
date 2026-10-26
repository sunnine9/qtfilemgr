// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRVERSIONSDIALOG_H
#define QTFILEMGRVERSIONSDIALOG_H

#include <QDialog>
#include <QPointer>

class QLabel;
class QPushButton;
class QTreeWidget;
class QtFileMgrIO;

class QtFileMgrVersionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrVersionsDialog(QtFileMgrIO *io,
                                     const QString &path,
                                     QWidget *parent = nullptr);

    QString path() const { return m_path; }
    int versionCount() const;
    int selectedVersion() const;

    // askConfirmation=false 供宿主的自动化流程和测试直接恢复指定版本。
    void restoreVersion(int version, bool askConfirmation = false);

public slots:
    void reloadVersions();
    void restoreSelectedVersion();

signals:
    void versionRestored(const QString &path, int version);

private:
    // completionStatus 非空时,加载成功后用它替换默认的"共 N 个历史版本"
    // 状态文本,避免覆盖上游操作(如恢复成功)的提示 (DLG-P2-16)
    void reloadVersions(const QString &completionStatus);
    void setBusy(bool busy, const QString &status);
    void updateRestoreButton();

    QPointer<QtFileMgrIO> m_io;
    QString m_path;
    QTreeWidget *m_versions;
    QLabel *m_status;
    QPushButton *m_restoreButton;
    bool m_busy = false;
};

#endif
