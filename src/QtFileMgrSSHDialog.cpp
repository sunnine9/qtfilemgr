// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrSSHDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtDebug>

QtFileMgrSSHDialog::QtFileMgrSSHDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("连接服务器"));
    setMinimumWidth(430);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("开发服务器"));
    form->addRow(QStringLiteral("名称:"), m_nameEdit);

    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setPlaceholderText(QStringLiteral("主机名或 IP 地址"));
    form->addRow(QStringLiteral("服务器:"), m_hostEdit);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    form->addRow(QStringLiteral("端口:"), m_portSpin);

    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(QStringLiteral("用户名"));
    form->addRow(QStringLiteral("用户:"), m_userEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(
        QStringLiteral("可留空，优先使用 SSH Agent 或密钥"));
    form->addRow(QStringLiteral("密码:"), m_passwordEdit);

    auto *identityRow = new QWidget(this);
    auto *identityLayout = new QHBoxLayout(identityRow);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(4);
    m_identityEdit = new QLineEdit(identityRow);
    m_identityEdit->setPlaceholderText(QStringLiteral("可选的私钥文件"));
    auto *identityButton = new QPushButton(QStringLiteral("选择..."), identityRow);
    identityLayout->addWidget(m_identityEdit, 1);
    identityLayout->addWidget(identityButton);
    form->addRow(QStringLiteral("私钥:"), identityRow);
    connect(identityButton, &QPushButton::clicked, this, [this]() {
        const QString initial = m_identityEdit->text().isEmpty()
            ? QDir::homePath() + QStringLiteral("/.ssh")
            : m_identityEdit->text();
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 SSH 私钥"), initial);
        if (!file.isEmpty())
            m_identityEdit->setText(file);
    });

    m_pathEdit = new QLineEdit(QStringLiteral("/"), this);
    m_pathEdit->setPlaceholderText(QStringLiteral("/home/user"));
    form->addRow(QStringLiteral("远端路径:"), m_pathEdit);

    m_addToPlacesCheck = new QCheckBox(QStringLiteral("添加到网络位置"), this);
    m_addToPlacesCheck->setChecked(true);
    form->addRow(QString(), m_addToPlacesCheck);

    m_savePasswordCheck = new QCheckBox(
        QStringLiteral("记住密码（以混淆形式保存在本机，非加密）"), this);
    m_savePasswordCheck->setChecked(false);
    form->addRow(QString(), m_savePasswordCheck);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("连接"));
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QtFileMgrSSHDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addWidget(buttons);
}

void QtFileMgrSSHDialog::setUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().toLower() != QStringLiteral("ssh")) {
        qWarning("[SSHDialog] setUrl: ignored unsupported URL (scheme=\"%s\")",
                 qPrintable(url.scheme()));
        return;
    }
    if (!url.password().isEmpty())
        qWarning("[SSHDialog] setUrl: ignored embedded password (host=\"%s\", port=%d)",
                 qPrintable(url.host()), url.port(22));
    m_hostEdit->setText(url.host());
    m_portSpin->setValue(url.port(22));
    m_userEdit->setText(url.userName());
    if (!url.path().isEmpty())
        m_pathEdit->setText(url.path());
    if (!url.host().isEmpty())
        m_nameEdit->setText(url.host());
}

QtFileMgrSSHConnectionConfig QtFileMgrSSHDialog::connectionConfig() const
{
    QtFileMgrSSHConnectionConfig config;
    config.host = m_hostEdit->text().trimmed();
    config.port = m_portSpin->value();
    config.user = m_userEdit->text().trimmed();
    config.password = m_passwordEdit->text();
    config.identityFile = QDir::fromNativeSeparators(
        m_identityEdit->text().trimmed());
    config.initialPath = QDir::cleanPath(m_pathEdit->text().trimmed());
    if (config.initialPath.isEmpty() || config.initialPath == QStringLiteral("."))
        config.initialPath = QStringLiteral("/");
    if (!config.initialPath.startsWith(QLatin1Char('/')))
        config.initialPath.prepend(QLatin1Char('/'));
    return config;
}

QString QtFileMgrSSHDialog::displayName() const
{
    const QString name = m_nameEdit->text().trimmed();
    if (!name.isEmpty())
        return name;
    return QStringLiteral("%1@%2").arg(
        m_userEdit->text().trimmed(), m_hostEdit->text().trimmed());
}

bool QtFileMgrSSHDialog::addToPlaces() const
{
    return m_addToPlacesCheck->isChecked();
}

bool QtFileMgrSSHDialog::savePassword() const
{
    return m_savePasswordCheck && m_savePasswordCheck->isChecked()
        && !m_passwordEdit->text().isEmpty();
}

void QtFileMgrSSHDialog::setReconnectMode(bool on)
{
    if (on) {
        setWindowTitle(QStringLiteral("重新输入密码"));
        // 重连的是已存在的位置,"添加到网络位置"无意义且曾导致
        // 每次重连追加同名记录;隐藏并强制不勾选
        m_addToPlacesCheck->setChecked(false);
        m_addToPlacesCheck->hide();
    }
}

QUrl QtFileMgrSSHDialog::sshUrl() const
{
    const QtFileMgrSSHConnectionConfig config = connectionConfig();
    QUrl url;
    url.setScheme(QStringLiteral("ssh"));
    url.setHost(config.host);
    url.setPort(config.port);
    url.setUserName(config.user);
    url.setPath(config.initialPath);
    return url;
}

void QtFileMgrSSHDialog::accept()
{
    const QtFileMgrSSHConnectionConfig config = connectionConfig();
    if (config.host.isEmpty() || config.user.isEmpty()) {
        qWarning("[SSHDialog] accept: rejected incomplete connection (host=\"%s\", user=\"%s\", port=%d)",
                 qPrintable(config.host), qPrintable(config.user), config.port);
        QMessageBox::warning(this, QStringLiteral("连接服务器"),
                             QStringLiteral("服务器和用户名不能为空。"));
        return;
    }
    if (!config.identityFile.isEmpty()
        && !QFileInfo::exists(config.identityFile)) {
        qWarning("[SSHDialog] accept: rejected missing identity file (host=\"%s\", user=\"%s\", identity=\"%s\")",
                 qPrintable(config.host), qPrintable(config.user),
                 qPrintable(config.identityFile));
        QMessageBox::warning(this, QStringLiteral("连接服务器"),
                             QStringLiteral("指定的私钥文件不存在。"));
        return;
    }
    QDialog::accept();
}
