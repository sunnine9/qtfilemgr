// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrConfigStore.h"

#include <QDir>
#include <QSet>
#include <QSysInfo>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QtDebug>

QString QtFileMgrConfigStore::settingsOrganization()
{
    return QStringLiteral("QtFileMgr");
}

QString QtFileMgrConfigStore::settingsApplication()
{
    return QStringLiteral("QtFileMgr");
}

QString QtFileMgrConfigStore::defaultConfigPath()
{
    const QString overridePath = QString::fromLocal8Bit(qgetenv("QTFILEMGR_CONFIG_FILE"));
    if (!overridePath.isEmpty())
        return overridePath;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/qtfilemgr/mgr_default_config.json");
}

QtFileMgrConfig QtFileMgrConfigStore::loadDefault()
{
    const QString path = defaultConfigPath();
    return QFileInfo(path).isFile()
        ? QtFileMgrConfig::loadFromFile(path)
        : QtFileMgrConfig::defaultConfig();
}

bool QtFileMgrConfigStore::saveDefault(const QtFileMgrConfig &config, QString *error)
{
    return saveFile(config, defaultConfigPath(), error);
}

bool QtFileMgrConfigStore::saveFile(const QtFileMgrConfig &config,
                                    const QString &filePath,
                                    QString *error)
{
    if (filePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("配置文件路径为空");
        return false;
    }

    const QString directory = QFileInfo(filePath).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("无法创建配置目录: %1").arg(directory);
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入配置文件: %1").arg(filePath);
        return false;
    }
    const QByteArray json = config.toJson();
    if (file.write(json) != json.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("保存配置失败: %1").arg(filePath);
        return false;
    }
    return true;
}

QList<PlaceEntry> QtFileMgrConfigStore::loadCustomPlaces()
{
    QList<PlaceEntry> entries;
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    const int count = settings.beginReadArray(QStringLiteral("customPlaces"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        PlaceEntry entry;
        entry.name = settings.value(QStringLiteral("name")).toString();
        entry.path = settings.value(QStringLiteral("path")).toString();
        entry.iconTheme = settings.value(
            QStringLiteral("icon"), QStringLiteral("folder")).toString();
        if (entry.name.isEmpty() || entry.path.isEmpty()) {
            qWarning("[ConfigStore] custom places: skipped invalid entry (index=%d, name=\"%s\", path=\"%s\")",
                     i, qPrintable(entry.name), qPrintable(entry.path));
            continue;
        }
        entries.append(entry);
    }
    settings.endArray();
    return entries;
}

void QtFileMgrConfigStore::saveCustomPlaces(const QList<PlaceEntry> &entries)
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    // beginWriteArray 不清除旧尾部条目，先整体移除避免 INI 残留键累积
    settings.remove(QStringLiteral("customPlaces"));
    settings.beginWriteArray(QStringLiteral("customPlaces"));
    for (int i = 0; i < entries.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), entries.at(i).name);
        settings.setValue(QStringLiteral("path"), entries.at(i).path);
        settings.setValue(QStringLiteral("icon"), entries.at(i).iconTheme);
    }
    settings.endArray();
}

QList<PlaceEntry> QtFileMgrConfigStore::loadNetworkPlaces()
{
    QList<PlaceEntry> entries;
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    QSet<QString> seenKeys;
    const int count = settings.beginReadArray(QStringLiteral("networkPlaces"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        PlaceEntry entry;
        entry.name = settings.value(QStringLiteral("name")).toString().trimmed();
        entry.path = settings.value(QStringLiteral("url")).toString().trimmed();
        entry.iconTheme = settings.value(
            QStringLiteral("icon"), QStringLiteral("folder-remote")).toString();
        const QUrl url(entry.path);
        if (entry.name.isEmpty() || !url.isValid()
            || url.scheme().toLower() != QStringLiteral("ssh")
            || url.host().isEmpty()) {
            qWarning("[ConfigStore] network places: skipped invalid entry (index=%d, name=\"%s\", url=\"%s\")",
                     i, qPrintable(entry.name), qPrintable(entry.path));
            continue;
        }
        // 规范化判重: 主机小写+端口归一+路径清理,历史重复记录只保留首条
        QUrl normalized = url;
        normalized.setHost(url.host().toLower());
        if (normalized.port(-1) == 22)
            normalized.setPort(-1);
        normalized.setPath(QDir::cleanPath(url.path()));
        const QString key = normalized.toString(QUrl::RemovePassword);
        if (seenKeys.contains(key)) {
            qWarning("[ConfigStore] network places: skipped duplicate entry (index=%d, name=\"%s\", url=\"%s\")",
                     i, qPrintable(entry.name), qPrintable(entry.path));
            continue;
        }
        seenKeys.insert(key);
        entries.append(entry);
    }
    settings.endArray();
    return entries;
}

void QtFileMgrConfigStore::saveNetworkPlaces(const QList<PlaceEntry> &entries)
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    // 同 saveCustomPlaces：先清除旧数组，避免缩短后残留键累积
    settings.remove(QStringLiteral("networkPlaces"));
    settings.beginWriteArray(QStringLiteral("networkPlaces"));
    for (int i = 0; i < entries.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), entries.at(i).name);
        settings.setValue(QStringLiteral("url"), entries.at(i).path);
        settings.setValue(QStringLiteral("icon"), entries.at(i).iconTheme);
    }
    settings.endArray();
}

QList<int> QtFileMgrConfigStore::loadDetailColumnWidths()
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    QList<int> result;
    const QVariantList values =
        settings.value(QStringLiteral("view/detailColumnWidths")).toList();
    for (const QVariant &value : values)
        result.append(value.toInt());
    return result;
}

void QtFileMgrConfigStore::saveDetailColumnWidths(const QList<int> &widths)
{
    QVariantList values;
    for (int width : widths)
        values.append(width);
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.setValue(QStringLiteral("view/detailColumnWidths"), values);
}

static QString viewModeKey(const QUrl &url)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha1).toHex());
}

int QtFileMgrConfigStore::loadDirectoryViewMode(const QUrl &url, int fallback)
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    return settings.value(
        QStringLiteral("viewModes/") + viewModeKey(url), fallback).toInt();
}

void QtFileMgrConfigStore::saveDirectoryViewMode(const QUrl &url, int mode)
{
    if (!url.isValid())
        return;
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.setValue(QStringLiteral("viewModes/") + viewModeKey(url), mode);
}

// ─── SSH 凭据(混淆存储) ─────────────────────────────────

#if QTFILEMGR_WITH_SSH
namespace {

// 机器绑定混淆密钥: 非加密, 仅防明文落盘
QByteArray sshCredentialKey()
{
    const QByteArray material = QSysInfo::machineUniqueId()
        + QByteArrayLiteral("|qtfilemgr-ssh-credential-v1");
    return QCryptographicHash::hash(material, QCryptographicHash::Sha256);
}

QByteArray obfuscateCredential(const QByteArray &plain)
{
    const QByteArray key = sshCredentialKey();
    QByteArray out = plain;
    for (int i = 0; i < out.size(); ++i)
        out[i] = out[i] ^ key[i % key.size()];
    return out.toBase64(QByteArray::Base64UrlEncoding);
}

QString deobfuscateCredential(const QByteArray &encoded, bool *ok)
{
    const QByteArray raw =
        QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
    const QByteArray key = sshCredentialKey();
    QByteArray out = raw;
    for (int i = 0; i < out.size(); ++i)
        out[i] = out[i] ^ key[i % key.size()];
    // 完整性标记, 防篡改/损坏后被当成密码送出
    if (!out.startsWith(QByteArrayLiteral("KPM1"))) {
        *ok = false;
        return QString();
    }
    *ok = true;
    return QString::fromUtf8(out.mid(4));
}

QString sshCredentialSettingsKey(const QString &user, const QString &host,
                                 int port)
{
    const QString identity = QStringLiteral("%1@%2:%3")
                                 .arg(user, host.toLower()).arg(port);
    return QStringLiteral("sshCredentials/")
        + QString::fromLatin1(
            identity.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
}

void hardenCredentialFilePermissions(QSettings &settings)
{
    settings.sync();
    const QString filePath = settings.fileName();
    if (!filePath.isEmpty()
        && !QFile::setPermissions(filePath, QFileDevice::ReadOwner
                                            | QFileDevice::WriteOwner)) {
        qWarning("[ConfigStore] ssh credential: failed to restrict permissions on \"%s\"",
                 qPrintable(filePath));
    }
}

} // namespace

void QtFileMgrConfigStore::saveSshCredential(const QString &user,
                                             const QString &host,
                                             int port, const QString &password)
{
    if (user.isEmpty() || host.isEmpty() || password.isEmpty()) {
        qWarning("[ConfigStore] saveSshCredential: rejected incomplete identity (user=\"%s\", host=\"%s\", port=%d, passwordEmpty=%d)",
                 qPrintable(user), qPrintable(host), port,
                 password.isEmpty() ? 1 : 0);
        return;
    }
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    const QByteArray payload = QByteArrayLiteral("KPM1") + password.toUtf8();
    settings.setValue(sshCredentialSettingsKey(user, host, port),
                      obfuscateCredential(payload));
    hardenCredentialFilePermissions(settings);
}

QString QtFileMgrConfigStore::loadSshCredential(const QString &user,
                                                const QString &host, int port)
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    const QByteArray encoded =
        settings.value(sshCredentialSettingsKey(user, host, port)).toByteArray();
    if (encoded.isEmpty())
        return QString();
    bool ok = false;
    const QString password = deobfuscateCredential(encoded, &ok);
    if (!ok) {
        qWarning("[ConfigStore] ssh credential: rejected corrupted entry (user=\"%s\", host=\"%s\", port=%d)",
                 qPrintable(user), qPrintable(host), port);
        return QString();
    }
    return password;
}

void QtFileMgrConfigStore::removeSshCredential(const QString &user,
                                               const QString &host, int port)
{
    QSettings settings(QtFileMgrConfigStore::settingsOrganization(),
                       QtFileMgrConfigStore::settingsApplication());
    settings.remove(sshCredentialSettingsKey(user, host, port));
    hardenCredentialFilePermissions(settings);
}
#endif // QTFILEMGR_WITH_SSH
