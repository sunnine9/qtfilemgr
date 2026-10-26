// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRSSHDIALOG_H
#define QTFILEMGRSSHDIALOG_H

#include "QtFileMgrFeatures.h"
#if QTFILEMGR_WITH_SSH
#include "QtFileMgrSSHIO.h"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QSpinBox;

/// SSH 连接参数对话框。密码只保留在当前连接对象中，不写入设置。
class QtFileMgrSSHDialog final : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrSSHDialog(QWidget *parent = nullptr);

    void setUrl(const QUrl &url);
    QtFileMgrSSHConnectionConfig connectionConfig() const;
    QString displayName() const;
    bool addToPlaces() const;
    bool savePassword() const;
    // 重连补密码模式: 隐藏"添加到网络位置"(该位置已存在),标题改为重新输入
    void setReconnectMode(bool on);
    QUrl sshUrl() const;

public slots:
    void accept() override;

private:
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_identityEdit = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QCheckBox *m_addToPlacesCheck = nullptr;
    QCheckBox *m_savePasswordCheck = nullptr;
};

#endif // QTFILEMGR_WITH_SSH
#endif
