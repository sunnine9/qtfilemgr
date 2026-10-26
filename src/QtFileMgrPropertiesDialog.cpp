// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrPropertiesDialog.h"

#include "QtFileMgrIO.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace {

const QFile::Permission kPermissionFlags[] = {
    QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner,
    QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup,
    QFile::ReadOther, QFile::WriteOther, QFile::ExeOther
};

QString commonOrMixed(const QStringList &values)
{
    if (values.isEmpty())
        return QStringLiteral("-");
    const QString first = values.first();
    for (const QString &value : values) {
        if (value != first)
            return QStringLiteral("多种");
    }
    return first.isEmpty() ? QStringLiteral("-") : first;
}

QLabel *propertyLabel(QWidget *parent, const char *objectName)
{
    auto *label = new QLabel(parent);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace

QtFileMgrPropertiesDialog::QtFileMgrPropertiesDialog(
    QtFileMgrIO *io, const QStringList &paths, QWidget *parent)
    : QDialog(parent)
    , m_io(io)
{
    QSet<QString> seen;
    for (const QString &path : paths) {
        if (path.isEmpty()) {
            qWarning("[PropertiesDialog] input: skipped empty path");
            continue;
        }
        if (seen.contains(path)) {
            qWarning("[PropertiesDialog] input: skipped duplicate path '%s'",
                     qPrintable(path));
            continue;
        }
        seen.insert(path);
        m_paths.append(path);
    }

    setWindowTitle(m_paths.size() > 1
        ? QStringLiteral("%1 个项目的属性").arg(m_paths.size())
        : QStringLiteral("属性"));
    resize(540, 430);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_nameValue = propertyLabel(this, "propertiesName");
    m_typeValue = propertyLabel(this, "propertiesType");
    m_locationValue = propertyLabel(this, "propertiesLocation");
    m_sizeValue = propertyLabel(this, "propertiesSize");
    m_modifiedValue = propertyLabel(this, "propertiesModified");
    m_ownerValue = propertyLabel(this, "propertiesOwner");
    m_linkTargetValue = propertyLabel(this, "propertiesLinkTarget");
    form->addRow(QStringLiteral("名称:"), m_nameValue);
    form->addRow(QStringLiteral("类型:"), m_typeValue);
    form->addRow(QStringLiteral("位置:"), m_locationValue);
    form->addRow(QStringLiteral("大小:"), m_sizeValue);
    form->addRow(QStringLiteral("修改时间:"), m_modifiedValue);
    form->addRow(QStringLiteral("所有者:"), m_ownerValue);
    form->addRow(QStringLiteral("链接目标:"), m_linkTargetValue);
    layout->addLayout(form);

    auto *permissionGroup = new QGroupBox(QStringLiteral("权限"), this);
    permissionGroup->setObjectName(QStringLiteral("propertiesPermissions"));
    auto *permissionLayout = new QVBoxLayout(permissionGroup);
    const QStringList subjects = {
        QStringLiteral("所有者"), QStringLiteral("组"), QStringLiteral("其他")
    };
    for (int subject = 0; subject < 3; ++subject) {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(subjects.at(subject), permissionGroup));
        const QStringList actions = {
            QStringLiteral("读取"), QStringLiteral("写入"), QStringLiteral("执行")
        };
        for (int action = 0; action < 3; ++action) {
            auto *check = new QCheckBox(actions.at(action), permissionGroup);
            check->setTristate(true);
            check->setObjectName(QStringLiteral("permission_%1_%2")
                .arg(subject).arg(action));
            m_permissionChecks.append(check);
            row->addWidget(check);
        }
        row->addStretch(1);
        permissionLayout->addLayout(row);
    }
    layout->addWidget(permissionGroup);
    layout->addStretch(1);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    m_buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("应用"));
    m_buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &QtFileMgrPropertiesDialog::applyPermissions);
    layout->addWidget(m_buttons);

    buildSummary();
    buildPermissions();
}

void QtFileMgrPropertiesDialog::buildSummary()
{
    if (!m_io || m_paths.isEmpty()) {
        qWarning("[PropertiesDialog] summary: missing IO or valid paths (io=%p, count=%d)",
                 static_cast<void *>(m_io.data()), m_paths.size());
        m_nameValue->setText(QStringLiteral("-"));
        m_typeValue->setText(QStringLiteral("-"));
        m_locationValue->setText(QStringLiteral("-"));
        m_sizeValue->setText(QStringLiteral("-"));
        m_modifiedValue->setText(QStringLiteral("-"));
        m_ownerValue->setText(QStringLiteral("-"));
        m_linkTargetValue->setText(QStringLiteral("-"));
        return;
    }

    QStringList names;
    QStringList types;
    QStringList locations;
    QStringList owners;
    QStringList linkTargets;
    qint64 totalSize = 0;
    int sizedFiles = 0;
    QDateTime latestModified;
    for (const QString &path : m_paths) {
        FileEntry entry = m_io->stat(path);
        if (entry.path.isEmpty()) {
            qWarning("[PropertiesDialog] summary: backend '%s' returned incomplete stat for '%s'; using requested path",
                     m_io->metaObject()->className(), qPrintable(path));
        }
        const QString name = entry.name.isEmpty()
            ? m_io->displayName(path) : entry.name;
        names << (name.isEmpty() ? QStringLiteral("/") : name);
        if (entry.isSymlink)
            types << QStringLiteral("符号链接 (%1)").arg(m_io->mimeType(path));
        else if (entry.isDir)
            types << QStringLiteral("文件夹");
        else
            types << m_io->mimeType(path);
        locations << QFileInfo(path).absolutePath();
        const QString owner = m_io->owner(path);
        const QString group = m_io->group(path);
        owners << QStringLiteral("%1 / %2")
            .arg(owner.isEmpty() ? QStringLiteral("-") : owner,
                 group.isEmpty() ? QStringLiteral("-") : group);
        if (!entry.isDir && entry.size >= 0) {
            totalSize += entry.size;
            ++sizedFiles;
        }
        if (entry.modified.isValid()
            && (!latestModified.isValid() || entry.modified > latestModified)) {
            latestModified = entry.modified;
        }
        if (entry.isSymlink) {
            const QString target = m_io->readLink(path);
            if (target.isEmpty()) {
                qWarning("[PropertiesDialog] symlink: backend returned empty target for '%s'",
                         qPrintable(path));
            } else {
                linkTargets << target;
            }
        }
    }

    m_nameValue->setText(m_paths.size() == 1
        ? names.first() : QStringLiteral("%1 个项目").arg(m_paths.size()));
    m_typeValue->setText(commonOrMixed(types));
    m_locationValue->setText(commonOrMixed(locations));
    m_sizeValue->setText(sizedFiles > 0
        ? formatFileSize(totalSize) : QStringLiteral("-"));
    m_modifiedValue->setText(latestModified.isValid()
        ? QLocale::system().toString(latestModified, QLocale::LongFormat)
        : QStringLiteral("-"));
    m_ownerValue->setText(commonOrMixed(owners));
    m_linkTargetValue->setText(linkTargets.isEmpty()
        ? QStringLiteral("-") : commonOrMixed(linkTargets));
}

Qt::CheckState QtFileMgrPropertiesDialog::commonPermissionState(
    QFile::Permission permission) const
{
    if (m_originalPermissions.isEmpty())
        return Qt::PartiallyChecked;
    const bool first = m_originalPermissions.first().testFlag(permission);
    for (QFile::Permissions permissions : m_originalPermissions) {
        if (permissions.testFlag(permission) != first)
            return Qt::PartiallyChecked;
    }
    return first ? Qt::Checked : Qt::Unchecked;
}

void QtFileMgrPropertiesDialog::buildPermissions()
{
    m_originalPermissions.clear();
    if (!m_io || !m_io->supportsPermissions() || m_paths.isEmpty()) {
        if (m_io && !m_io->supportsPermissions()) {
            qWarning("[PropertiesDialog] permissions: backend '%s' does not support permissions",
                     m_io->metaObject()->className());
        }
        for (QCheckBox *check : m_permissionChecks)
            check->setEnabled(false);
        m_buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        return;
    }
    for (const QString &path : m_paths)
        m_originalPermissions.append(m_io->permissions(path));
    for (int i = 0; i < m_permissionChecks.size(); ++i)
        m_permissionChecks.at(i)->setCheckState(
            commonPermissionState(kPermissionFlags[i]));
}

void QtFileMgrPropertiesDialog::applyPermissions()
{
    if (!m_io || !m_io->supportsPermissions() || m_paths.isEmpty()) {
        qWarning("[PropertiesDialog] apply: ignored without a permission-capable backend");
        return;
    }
    QStringList failed;
    for (int pathIndex = 0; pathIndex < m_paths.size(); ++pathIndex) {
        QFile::Permissions permissions = m_originalPermissions.value(pathIndex);
        for (int i = 0; i < m_permissionChecks.size(); ++i) {
            const Qt::CheckState state = m_permissionChecks.at(i)->checkState();
            if (state == Qt::PartiallyChecked)
                continue;
            if (state == Qt::Checked)
                permissions |= kPermissionFlags[i];
            else
                permissions &= ~QFile::Permissions(kPermissionFlags[i]);
        }
        if (!m_io->chmod(m_paths.at(pathIndex), permissions)) {
            failed << m_paths.at(pathIndex);
            qWarning("[PropertiesDialog] apply: chmod failed for '%s'",
                     qPrintable(m_paths.at(pathIndex)));
        } else {
            m_originalPermissions[pathIndex] = permissions;
        }
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("权限修改失败"),
            QStringLiteral("%1 个项目的权限未能修改。\n%2")
                .arg(failed.size()).arg(failed.join(QLatin1Char('\n'))));
        return;
    }
    emit permissionsApplied(m_paths);
}
