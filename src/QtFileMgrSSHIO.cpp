// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrSSHIO.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMutex>
#include <QPointer>
#include <QSaveFile>
#include <QSharedPointer>
#include <QStyle>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtDebug>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>

namespace {

QString cleanRemotePath(QString path)
{
    if (path.startsWith(QLatin1String("ssh://"))) {
        // 不用 QUrl 宽容模式解析: #?% 是合法文件名字符,会被当作
        // fragment/query 截断或百分号解码 (SSH-P2-4)
        const int authorityEnd = path.indexOf(QLatin1Char('/'), 6);
        path = authorityEnd >= 0 ? path.mid(authorityEnd)
                                 : QStringLiteral("/");
    }
    // 内部路径不裁剪空白: 末尾空格是合法文件名字符,trim 会把删除/
    // 复制/重命名导向另一个文件 (审查#3);用户输入入口已各自裁剪
    path = QDir::cleanPath(path);
    if (path.isEmpty() || path == QStringLiteral("."))
        return QStringLiteral("/");
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

QString joinedRemotePath(const QString &directory, const QString &name)
{
    const QString parent = cleanRemotePath(directory);
    return parent == QStringLiteral("/")
        ? parent + name : parent + QLatin1Char('/') + name;
}

QFile::Permissions qtPermissions(unsigned long mode)
{
    QFile::Permissions result;
    if (mode & S_IRUSR) result |= QFile::ReadOwner;
    if (mode & S_IWUSR) result |= QFile::WriteOwner;
    if (mode & S_IXUSR) result |= QFile::ExeOwner;
    if (mode & S_IRGRP) result |= QFile::ReadGroup;
    if (mode & S_IWGRP) result |= QFile::WriteGroup;
    if (mode & S_IXGRP) result |= QFile::ExeGroup;
    if (mode & S_IROTH) result |= QFile::ReadOther;
    if (mode & S_IWOTH) result |= QFile::WriteOther;
    if (mode & S_IXOTH) result |= QFile::ExeOther;
    return result;
}

mode_t nativePermissions(QFile::Permissions permissions)
{
    mode_t mode = 0;
    if (permissions.testFlag(QFile::ReadOwner)) mode |= S_IRUSR;
    if (permissions.testFlag(QFile::WriteOwner)) mode |= S_IWUSR;
    if (permissions.testFlag(QFile::ExeOwner)) mode |= S_IXUSR;
    if (permissions.testFlag(QFile::ReadGroup)) mode |= S_IRGRP;
    if (permissions.testFlag(QFile::WriteGroup)) mode |= S_IWGRP;
    if (permissions.testFlag(QFile::ExeGroup)) mode |= S_IXGRP;
    if (permissions.testFlag(QFile::ReadOther)) mode |= S_IROTH;
    if (permissions.testFlag(QFile::WriteOther)) mode |= S_IWOTH;
    if (permissions.testFlag(QFile::ExeOther)) mode |= S_IXOTH;
    return mode;
}

QString connectionLabel(const QtFileMgrSSHConnectionConfig &config)
{
    return QStringLiteral("%1@%2:%3")
        .arg(config.user, config.host).arg(config.port);
}

struct SSHConnectResult {
    bool success = false;
    bool unknownHost = false;
    QString fingerprint;
    QByteArray hostKeyHash;
    QString error;
};

struct SSHOperationResult {
    bool success = false;
    QString error;
};

struct SSHListResult {
    QList<FileEntry> entries;
    QString error;
};

struct SSHPreviewResult {
    QByteArray bytes;
    bool truncated = false;
    QString error;
};

} // namespace

// 阻塞操作的共享状态:事件循环与结果放堆上,facade 在 exec 期间被
// deleteLater 销毁时 worker 写入仍安全,收尾按 facadeAlive 决定是否
// 回写成员 (SSH-P1-2)。须在全局作用域定义以匹配头文件前置声明。
struct QtFileMgrSSHBlockingState {
    QEventLoop *loop = nullptr;
    SSHOperationResult result;
    QAtomicInt facadeAlive{1};
};

class QtFileMgrSSHWorker final : public QObject {
public:
    ~QtFileMgrSSHWorker() override { closeSession(); }

    // 传输层失效时通知 facade 回退连接状态 (R2-17)
    QPointer<QtFileMgrSSHIO> m_facade;

    SSHPreviewResult preview(const QString &path, qint64 limit, bool image,
                             const QSharedPointer<QAtomicInt> &cancelled)
    {
        SSHPreviewResult result;
        if (limit <= 0 || limit > (image ? 4 * 1024 * 1024 : 128 * 1024)) {
            result.error = QStringLiteral("预览大小上限无效");
            qWarning("[SSHIO] preview: invalid limit (path=\"%s\", limit=%lld)",
                     qPrintable(path), static_cast<long long>(limit));
            return result;
        }
        if (!ensureConnected(&result.error))
            return result;
        if (isCancelled(cancelled)) {
            result.error = QStringLiteral("预览已取消");
            return result;
        }
        sftp_attributes attr = attributesFor(path);
        if (!attr) {
            result.error = lastError(QStringLiteral("读取预览元数据失败"));
            return result;
        }
        const bool regular = S_ISREG(attr->permissions);
        const quint64 size = attr->size;
        sftp_attributes_free(attr);
        if (!regular || (image && size > static_cast<quint64>(limit))) {
            result.error = !regular ? QStringLiteral("只支持常规文件预览")
                                    : QStringLiteral("图片超出预览大小上限");
            qWarning("[SSHIO] preview: rejected file (path=\"%s\", regular=%d, size=%llu, limit=%lld)",
                     qPrintable(path), regular ? 1 : 0,
                     static_cast<unsigned long long>(size), static_cast<long long>(limit));
            return result;
        }
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        sftp_file file = sftp_open(m_sftp, encoded.constData(), O_RDONLY, 0);
        if (!file) {
            result.error = lastError(QStringLiteral("打开预览文件失败"));
            return result;
        }
        const qint64 target = limit + 1;
        while (result.bytes.size() < target) {
            if (isCancelled(cancelled)) {
                result.error = QStringLiteral("预览已取消");
                break;
            }
            char buffer[16 * 1024];
            const size_t count = static_cast<size_t>(qMin<qint64>(sizeof(buffer), target - result.bytes.size()));
            const ssize_t got = sftp_read(file, buffer, count);
            if (got == 0)
                break;
            if (got < 0) {
                result.error = lastError(QStringLiteral("读取预览失败"));
                break;
            }
            result.bytes.append(buffer, static_cast<int>(got));
        }
        if (sftp_close(file) != SSH_OK && result.error.isEmpty())
            result.error = lastError(QStringLiteral("关闭预览文件失败"));
        // 取消只在没有真实错误时改写错误文本:读取/关闭失败
        // 被"预览已取消"盖掉就无法排查 (SSH-P2-3)
        if (isCancelled(cancelled)) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral("预览已取消");
            } else {
                qWarning("[SSHIO] preview: cancelled but keeping real error (path=\"%s\", error=\"%s\")",
                         qPrintable(path), qPrintable(result.error));
            }
        }
        result.truncated = result.bytes.size() > limit;
        if (image && result.truncated && result.error.isEmpty())
            result.error = QStringLiteral("图片超出预览大小上限");
        if (result.truncated)
            result.bytes.truncate(static_cast<int>(limit));
        if (!result.error.isEmpty())
            result.bytes.clear();
        return result;
    }

    SSHConnectResult connectSession(const QtFileMgrSSHConnectionConfig &config,
                                    bool acceptUnknownKey,
                                    const QByteArray &confirmedHostKeyHash)
    {
        closeSession();
        SSHConnectResult result;
        m_config = config;
        m_ssh = ssh_new();
        if (!m_ssh) {
            result.error = QStringLiteral("无法创建 SSH 会话");
            return result;
        }

        if (qEnvironmentVariableIsSet("QTFILEMGR_SSH_KNOWN_HOSTS")) {
            const QString knownHosts = qEnvironmentVariable("QTFILEMGR_SSH_KNOWN_HOSTS");
            const QFileInfo file(knownHosts);
            const QFileInfo directory(file.absolutePath());
            if (knownHosts.isEmpty() || !file.isAbsolute() || file.isSymLink()
                || (file.exists() && (!file.isFile() || !file.isWritable()))
                || !directory.isDir() || directory.isSymLink() || !directory.isWritable()) {
                qWarning("[SSHIO] known_hosts: invalid configured path (path=\"%s\", host=\"%s\")",
                         qPrintable(knownHosts), qPrintable(config.host));
                result.error = QStringLiteral("指定的 known_hosts 路径无效或不可写: %1")
                                   .arg(knownHosts);
                closeSession();
                return result;
            }
            const QByteArray path = QFile::encodeName(knownHosts);
            if (ssh_options_set(m_ssh, SSH_OPTIONS_KNOWNHOSTS, path.constData()) != SSH_OK) {
                result.error = lastError(QStringLiteral("设置 known_hosts 路径失败"));
                closeSession();
                return result;
            }
        }

        const QByteArray host = config.host.toUtf8();
        const QByteArray user = config.user.toUtf8();
        const long timeout = qMax(1, config.timeoutSeconds);
        if (ssh_options_set(m_ssh, SSH_OPTIONS_HOST, host.constData()) != SSH_OK
            || ssh_options_set(m_ssh, SSH_OPTIONS_PORT, &config.port) != SSH_OK
            || ssh_options_set(m_ssh, SSH_OPTIONS_USER, user.constData()) != SSH_OK
            || ssh_options_set(m_ssh, SSH_OPTIONS_TIMEOUT, &timeout) != SSH_OK) {
            result.error = lastError(QStringLiteral("设置 SSH 连接参数失败"));
            closeSession();
            return result;
        }
        if (!config.identityFile.isEmpty()) {
            const QByteArray identity = QFile::encodeName(config.identityFile);
            if (ssh_options_set(m_ssh, SSH_OPTIONS_IDENTITY,
                                identity.constData()) != SSH_OK) {
                result.error = lastError(QStringLiteral("设置 SSH 私钥失败"));
                closeSession();
                return result;
            }
        }

        if (ssh_connect(m_ssh) != SSH_OK) {
            result.error = lastError(QStringLiteral("SSH 连接失败"));
            closeSession();
            return result;
        }

        ssh_key serverKey = nullptr;
        unsigned char *hash = nullptr;
        size_t hashLength = 0;
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 8, 0)
        const int publicKeyResult = ssh_get_server_publickey(m_ssh, &serverKey);
#else
        const int publicKeyResult = ssh_get_publickey(m_ssh, &serverKey);
#endif
        if (publicKeyResult == SSH_OK
            && ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256,
                                      &hash, &hashLength) == SSH_OK) {
            result.hostKeyHash = QByteArray(reinterpret_cast<const char *>(hash),
                                            static_cast<int>(hashLength));
            char *fingerprint = ssh_get_fingerprint_hash(
                SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
            if (fingerprint) {
                result.fingerprint = QString::fromLatin1(fingerprint);
                ssh_string_free_char(fingerprint);
            }
            ssh_clean_pubkey_hash(&hash);
        }
        if (serverKey)
            ssh_key_free(serverKey);

        if (result.hostKeyHash.isEmpty()) {
            result.error = lastError(QStringLiteral("无法获取服务器主机密钥指纹"));
            closeSession();
            return result;
        }
        if (acceptUnknownKey && (confirmedHostKeyHash.isEmpty()
                                 || result.hostKeyHash != confirmedHostKeyHash)) {
            qWarning("[SSHIO] connect: host key changed after confirmation (host=\"%s\", port=%d)",
                     qPrintable(config.host), config.port);
            result.error = QStringLiteral("服务器主机密钥与刚才确认的指纹不一致，已拒绝连接");
            closeSession();
            return result;
        }

#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 9, 0)
        const enum ssh_known_hosts_e known = ssh_session_is_known_server(m_ssh);
        const bool hostKnown = known == SSH_KNOWN_HOSTS_OK;
        const bool hostUnknown = known == SSH_KNOWN_HOSTS_UNKNOWN
            || known == SSH_KNOWN_HOSTS_NOT_FOUND;
        const bool hostChanged = known == SSH_KNOWN_HOSTS_CHANGED
            || known == SSH_KNOWN_HOSTS_OTHER;
#else
        const int known = ssh_is_server_known(m_ssh);
        const bool hostKnown = known == SSH_SERVER_KNOWN_OK;
        const bool hostUnknown = known == SSH_SERVER_NOT_KNOWN
            || known == SSH_SERVER_FILE_NOT_FOUND;
        const bool hostChanged = known == SSH_SERVER_KNOWN_CHANGED
            || known == SSH_SERVER_FOUND_OTHER;
#endif
        if (hostChanged) {
            result.error = QStringLiteral(
                "服务器主机密钥与 known_hosts 记录不一致，已拒绝连接");
            closeSession();
            return result;
        }
        if (!hostKnown && !hostUnknown) {
            result.error = lastError(QStringLiteral("无法验证服务器主机密钥"));
            closeSession();
            return result;
        }
        if (hostUnknown && !acceptUnknownKey) {
            result.unknownHost = true;
            closeSession();
            return result;
        }
        if (hostUnknown) {
            // ssh_session_update_known_hosts 整体重写 known_hosts 文件,
            // 进程内多个 SSHIO 实例并发写入会交错损坏,需进程级互斥 (R2-IO-P1-3)
            static QMutex knownHostsMutex;
            const QMutexLocker knownHostsLock(&knownHostsMutex);
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 9, 0)
            const int updateResult = ssh_session_update_known_hosts(m_ssh);
#else
            const int updateResult = ssh_write_knownhost(m_ssh);
#endif
            if (updateResult != SSH_OK) {
                result.error = lastError(
                    QStringLiteral("写入 known_hosts 失败"));
                closeSession();
                return result;
            }
        }

        int authResult = ssh_userauth_none(m_ssh, nullptr);
        if (authResult != SSH_AUTH_SUCCESS) {
            const QByteArray passphrase = config.password.toUtf8();
            authResult = ssh_userauth_publickey_auto(
                m_ssh, nullptr,
                passphrase.isEmpty() ? nullptr : passphrase.constData());
        }
        if (authResult != SSH_AUTH_SUCCESS && !config.password.isEmpty()) {
            const QByteArray password = config.password.toUtf8();
            authResult = ssh_userauth_password(
                m_ssh, nullptr, password.constData());
        }
        if (authResult != SSH_AUTH_SUCCESS) {
            result.error = lastError(QStringLiteral(
                "SSH 认证失败，请检查 SSH Agent、私钥或密码"));
            closeSession();
            return result;
        }

        m_sftp = sftp_new(m_ssh);
        if (!m_sftp || sftp_init(m_sftp) != SSH_OK) {
            result.error = lastError(QStringLiteral("初始化 SFTP 失败"));
            closeSession();
            return result;
        }
        result.success = true;
        return result;
    }

    void closeSession()
    {
        if (m_sftp) {
            sftp_free(m_sftp);
            m_sftp = nullptr;
        }
        if (m_ssh) {
            ssh_disconnect(m_ssh);
            ssh_free(m_ssh);
            m_ssh = nullptr;
        }
    }

    SSHListResult listDirectory(
        const QString &path,
        const QSharedPointer<QAtomicInt> &cancelled = {})
    {
        SSHListResult result;
        if (!ensureConnected(&result.error))
            return result;
        const QString directory = cleanRemotePath(path);
        const QByteArray encoded = directory.toUtf8();
        sftp_dir handle = sftp_opendir(m_sftp, encoded.constData());
        if (!handle) {
            result.error = lastError(
                QStringLiteral("无法打开远端目录 %1").arg(directory));
            return result;
        }

        while (sftp_attributes attributes = sftp_readdir(m_sftp, handle)) {
            if (cancelled && cancelled->loadAcquire() != 0) {
                sftp_attributes_free(attributes);
                break;
            }
            const QString serverName = QString::fromUtf8(attributes->name);
            if (serverName == QStringLiteral(".") || serverName == QStringLiteral("..")) {
                sftp_attributes_free(attributes);
                continue;
            }
            const QString name = QtFileMgrSSHIO::safeEntryName(directory, serverName);
            if (name.isEmpty()) {
                qWarning("[SSHIO] listDirectory: rejected unsafe entry (directory=\"%s\", name=\"%s\")",
                         qPrintable(directory), qPrintable(serverName));
                result.error = QStringLiteral("远端目录包含不安全的文件名，已拒绝处理: %1")
                                   .arg(serverName);
                sftp_attributes_free(attributes);
                break;
            }
            if (serverName != name)
                qWarning("[SSHIO] listDirectory: normalized direct-child path (directory=\"%s\", returned=\"%s\", name=\"%s\")",
                         qPrintable(directory), qPrintable(serverName), qPrintable(name));
            FileEntry entry;
            entry.name = name;
            entry.path = joinedRemotePath(directory, name);
            entry.size = static_cast<qint64>(attributes->size);
            entry.modified = QDateTime::fromSecsSinceEpoch(attributes->mtime);
            entry.isDir = S_ISDIR(attributes->permissions);
            entry.isHidden = name.startsWith(QLatin1Char('.'));
            // 任何一类写位(owner/group/other)存在都视为可写;
            // 只看 S_IWUSR 会把 group/other 可写的文件误标只读 (SSH-P2-5)
            entry.isReadOnly = !(attributes->permissions
                                 & (S_IWUSR | S_IWGRP | S_IWOTH));
            entry.isSymlink = S_ISLNK(attributes->permissions);
            entry.permissionBits = qtPermissions(attributes->permissions);
            if (attributes->owner)
                entry.ownerName = QString::fromUtf8(attributes->owner);
            if (attributes->group)
                entry.groupName = QString::fromUtf8(attributes->group);
            result.entries.append(entry);
            sftp_attributes_free(attributes);
        }
        if (result.error.isEmpty() && sftp_dir_eof(handle) == 0)
            result.error = lastError(
                QStringLiteral("读取远端目录 %1 失败").arg(directory));
        sftp_closedir(handle);
        return result;
    }

    SSHOperationResult makeDirectory(const QString &path)
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        result.success = sftp_mkdir(m_sftp, encoded.constData(), 0755) == SSH_OK;
        if (!result.success)
            result.error = lastError(QStringLiteral("创建远端目录失败"));
        return result;
    }

    SSHOperationResult renamePath(const QString &oldPath,
                                  const QString &newPath, bool noReplace = false)
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        const QByteArray source = cleanRemotePath(oldPath).toUtf8();
        const QByteArray destination = cleanRemotePath(newPath).toUtf8();
        // Normal SFTP v3 RENAME refuses an existing destination. Do not use
        // posix-rename (which replaces), or libssh's overwrite flags on newer
        // protocol versions, for backup/commit/rollback of user data.
        const int version = sftp_server_version(m_sftp);
        if (noReplace && version != 3) {
            result.error = QStringLiteral("安全重命名要求 SFTP v3，服务器版本为 %1；未移动任何项目")
                               .arg(version);
            qWarning("[SSHIO] renameNoReplace: rejected unsupported server version (source=\"%s\", destination=\"%s\", version=%d)",
                     qPrintable(oldPath), qPrintable(newPath), version);
            return result;
        }
        result.success = sftp_rename(m_sftp, source.constData(),
                                     destination.constData()) == SSH_OK;
        if (!result.success)
            result.error = lastError(QStringLiteral("重命名远端项目失败"));
        return result;
    }

    SSHOperationResult removePath(const QString &path,
                                  const QSharedPointer<QAtomicInt> &cancelled = {})
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        result.success = removeRecursive(cleanRemotePath(path), cancelled,
                                         &result.error);
        return result;
    }

    SSHOperationResult writeContents(const QString &path,
                                     const QByteArray &contents)
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        const QString clean = cleanRemotePath(path);
        // O_TRUNC 直写会先截断原文件,写一半失败/断开时旧内容已毁;
        // 先写同目录临时节点,全部写完后 rename 提交 (SSH-P2-7)
        sftp_attributes existing = sftp_stat(m_sftp, clean.toUtf8().constData());
        const bool replacing = existing != nullptr;
        if (existing)
            sftp_attributes_free(existing);
        const QString target = replacing
            ? clean + QStringLiteral(".qtfilemgr-write-tmp") : clean;
        const QByteArray encoded = target.toUtf8();
        sftp_file file = sftp_open(m_sftp, encoded.constData(),
            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (!file) {
            result.error = lastError(QStringLiteral("创建远端文件失败"));
            return result;
        }
        qint64 offset = 0;
        while (offset < contents.size()) {
            const ssize_t written = sftp_write(
                file, contents.constData() + offset,
                static_cast<size_t>(contents.size() - offset));
            if (written <= 0) {
                result.error = lastError(QStringLiteral("写入远端文件失败"));
                sftp_close(file);
                // 清理半写入的远端文件,与 upload/copy 的 cleanup 对齐 (R2-IO-P2-5)
                if (sftp_unlink(m_sftp, encoded.constData()) != SSH_OK) {
                    qWarning("[SSHIO] writeContents: failed to clean partial remote file (path=\"%s\", written=%lld)",
                             qPrintable(target), static_cast<long long>(offset));
                }
                return result;
            }
            offset += written;
        }
        if (sftp_close(file) != SSH_OK) {
            result.error = lastError(QStringLiteral("关闭远端文件失败"));
            return result;
        }
        if (replacing) {
            const SSHOperationResult renameResult = renamePath(target, clean);
            if (!renameResult.success) {
                // rename 失败: 临时文件残留,原文件完好
                qWarning("[SSHIO] writeContents: commit rename failed, original file intact (target=\"%s\", staging=\"%s\")",
                         qPrintable(clean), qPrintable(target));
                if (sftp_unlink(m_sftp, encoded.constData()) != SSH_OK)
                    qWarning("[SSHIO] writeContents: failed to remove staging file \"%s\"",
                             qPrintable(target));
                result.error = renameResult.error;
                return result;
            }
        }
        result.success = true;
        return result;
    }

    SSHOperationResult changePermissions(const QString &path,
                                         QFile::Permissions permissions)
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        result.success = sftp_chmod(m_sftp, encoded.constData(),
                                    nativePermissions(permissions)) == SSH_OK;
        if (!result.success)
            result.error = lastError(QStringLiteral("修改远端权限失败"));
        return result;
    }

    SSHOperationResult transfer(const QString &source,
                                const QString &destination,
                                bool move, bool exportToLocal,
                                bool importFromLocal,
                                const QSharedPointer<QAtomicInt> &cancelled,
                                const std::function<void(qint64, qint64)> &progress)
    {
        SSHOperationResult result;
        if (!ensureConnected(&result.error))
            return result;
        qint64 transferred = 0;
        if (exportToLocal) {
            result.success = downloadRecursive(
                cleanRemotePath(source), destination, cancelled,
                &transferred, progress, &result.error);
            return result;
        }

        if (importFromLocal) {
            // 显式上传方向由调用方声明,不再用本机路径存在性猜测 (审查#1)
            const QFileInfo localSource(source);
            if (!localSource.isAbsolute() || !localSource.exists()) {
                result.error = QStringLiteral("本地源不存在: %1").arg(source);
                qWarning("[SSHIO] transfer: import source missing locally (source=\"%s\")",
                         qPrintable(source));
                return result;
            }
            result.success = uploadRecursive(
                localSource.absoluteFilePath(), cleanRemotePath(destination),
                cancelled, &transferred, progress, &result.error);
            if (result.success && move) {
                const bool removed = localSource.isDir()
                    ? QDir(localSource.absoluteFilePath()).removeRecursively()
                    : QFile::remove(localSource.absoluteFilePath());
                if (!removed) {
                    result.success = false;
                    result.error = QStringLiteral(
                        "上传完成，但无法删除本地源项目 %1")
                            .arg(localSource.absoluteFilePath());
                }
            }
            return result;
        }

        // 远端到远端 (审查#1: 与本机同名文件无关)
        const QString remoteSource = cleanRemotePath(source);
        const QString remoteDestination = cleanRemotePath(destination);
        if (move) {
            result = renamePath(remoteSource, remoteDestination);
            return result;
        }
        // 词法检查之外再做服务端真实路径检查: 链接别名(/alias -> /src)
        // 可绕过词法前缀,把复制目标实际落到源树内 (R2-05)
        if (!rejectRemoteSelfCopy(remoteSource, remoteDestination,
                                  &result.error))
            return result;
        result.success = copyRemoteRecursive(
            remoteSource, remoteDestination, cancelled,
            &transferred, progress, &result.error);
        return result;
    }

private:
    // 服务端 canonicalize: 解析符号链接后的真实路径;失败返回空
    QString canonicalRemotePath(const QString &path) const
    {
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        char *resolved = sftp_canonicalize_path(m_sftp, encoded.constData());
        if (!resolved)
            return QString();
        const QString out = QString::fromUtf8(resolved);
        ssh_string_free_char(resolved);
        return out;
    }

    // 自复制/后代检查: 词法 + 服务端解析双通道 (R2-05)
    bool rejectRemoteSelfCopy(const QString &source, const QString &destination,
                              QString *error)
    {
        const auto isSelfOrChild = [](const QString &src, const QString &dst) {
            return dst == src
                || dst.startsWith(src == QStringLiteral("/")
                                      ? QStringLiteral("/")
                                      : src + QLatin1Char('/'));
        };
        if (isSelfOrChild(source, destination)) {
            qWarning("[SSHIO] transfer: rejected self/subdirectory copy (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            *error = QStringLiteral("不能将目录复制到其自身或其子目录");
            return false;
        }
        const QString canonicalSource = canonicalRemotePath(source);
        const QString destinationParent =
            destination.section(QLatin1Char('/'), 0, -2).isEmpty()
                ? QStringLiteral("/")
                : destination.section(QLatin1Char('/'), 0, -2);
        const QString canonicalParent = canonicalRemotePath(destinationParent);
        if (canonicalSource.isEmpty() || canonicalParent.isEmpty()) {
            qWarning("[SSHIO] transfer: canonicalize failed, lexical check only (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return true;
        }
        const QString canonicalDestination =
            (canonicalParent == QStringLiteral("/")
                 ? QString() : canonicalParent)
            + QLatin1Char('/') + destination.section(QLatin1Char('/'), -1);
        if (isSelfOrChild(canonicalSource, canonicalDestination)) {
            qWarning("[SSHIO] transfer: rejected aliased self/subdirectory copy (source=\"%s\" -> \"%s\", destination=\"%s\" -> \"%s\")",
                     qPrintable(source), qPrintable(canonicalSource),
                     qPrintable(destination), qPrintable(canonicalDestination));
            *error = QStringLiteral("目标实际位于源目录内部(经链接别名)");
            return false;
        }
        return true;
    }

    bool ensureConnected(QString *error) const
    {
        if (m_ssh && m_sftp)
            return true;
        if (error)
            *error = QStringLiteral("SSH 后端尚未连接");
        return false;
    }

    QString lastError(const QString &context) const
    {
        // SSH_FATAL = 传输层失效 (R2-17): 通知 facade 回退连接状态,
        // 否则死会话的指针非空会一直被 ensureConnected/复用逻辑当作已连接
        if (m_ssh && ssh_get_error_code(m_ssh) == SSH_FATAL && m_facade) {
            const QPointer<QtFileMgrSSHIO> facade(m_facade);
            QTimer::singleShot(0, facade, [facade]() {
                if (facade)
                    facade->noteSessionDead();
            });
        }
        const QString detail = m_ssh
            ? QString::fromUtf8(ssh_get_error(m_ssh))
            : QStringLiteral("SSH 会话不存在");
        return context + QStringLiteral(": ") + detail;
    }

    sftp_attributes attributesFor(const QString &path) const
    {
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        return sftp_stat(m_sftp, encoded.constData());
    }

    // lstat 版本: 删除等不得跟随符号链接的路径必须使用 (审查#2)
    sftp_attributes attributesForNoFollow(const QString &path) const
    {
        const QByteArray encoded = cleanRemotePath(path).toUtf8();
        return sftp_lstat(m_sftp, encoded.constData());
    }

    bool isCancelled(const QSharedPointer<QAtomicInt> &cancelled) const
    {
        return cancelled && cancelled->loadAcquire() != 0;
    }

    bool removeRecursive(const QString &path,
                         const QSharedPointer<QAtomicInt> &cancelled,
                         QString *error)
    {
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("操作已取消");
            return false;
        }
        sftp_attributes attributes = attributesForNoFollow(path);
        if (!attributes) {
            *error = lastError(QStringLiteral("读取远端项目失败"));
            return false;
        }
        // lstat 判型: 目录符号链接是链接而非目录,直接 unlink,
        // 不得跟随递归删除链接目标 (审查#2)
        const bool directory = S_ISDIR(attributes->permissions);
        sftp_attributes_free(attributes);
        const QByteArray encoded = path.toUtf8();
        if (!directory) {
            if (sftp_unlink(m_sftp, encoded.constData()) == SSH_OK)
                return true;
            *error = lastError(QStringLiteral("删除远端文件失败"));
            return false;
        }

        SSHListResult children = listDirectory(path, cancelled);
        if (!children.error.isEmpty()) {
            *error = children.error;
            return false;
        }
        for (const FileEntry &entry : children.entries) {
            if (!removeRecursive(entry.path, cancelled, error))
                return false;
        }
        if (sftp_rmdir(m_sftp, encoded.constData()) == SSH_OK)
            return true;
        *error = lastError(QStringLiteral("删除远端目录失败"));
        return false;
    }

    bool uploadRecursive(const QString &localPath, const QString &remotePath,
                         const QSharedPointer<QAtomicInt> &cancelled,
                         qint64 *transferred,
                         const std::function<void(qint64, qint64)> &progress,
                         QString *error)
    {
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("上传已取消");
            return false;
        }
        const QFileInfo info(localPath);
        if (info.isDir()) {
            const QByteArray encoded = remotePath.toUtf8();
            if (sftp_mkdir(m_sftp, encoded.constData(), 0755) != SSH_OK) {
                sftp_attributes existing = attributesFor(remotePath);
                const bool existingDirectory = existing
                    && S_ISDIR(existing->permissions);
                if (existing)
                    sftp_attributes_free(existing);
                if (!existingDirectory) {
                    *error = lastError(QStringLiteral("创建远端目录失败"));
                    return false;
                }
            }
            QDir directory(localPath);
            const QFileInfoList children = directory.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                    | QDir::System,
                QDir::Name | QDir::DirsFirst);
            for (const QFileInfo &child : children) {
                if (!uploadRecursive(
                        child.absoluteFilePath(),
                        joinedRemotePath(remotePath, child.fileName()),
                        cancelled, transferred, progress, error)) {
                    return false;
                }
            }
            return true;
        }

        QFile input(localPath);
        if (!input.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("无法读取本地文件 %1: %2")
                .arg(localPath, input.errorString());
            return false;
        }
        const QByteArray encoded = remotePath.toUtf8();
        sftp_attributes previous = attributesFor(remotePath);
        if (previous) {
            sftp_attributes_free(previous);
            *error = QStringLiteral("远端目标已存在，拒绝覆盖 %1").arg(remotePath);
            qWarning("[SSHIO] upload: rejected existing destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(localPath), qPrintable(remotePath));
            return false;
        }
        if (sftp_get_error(m_sftp) != SSH_FX_NO_SUCH_FILE) {
            *error = lastError(QStringLiteral("无法确认远端目标不存在"));
            qWarning("[SSHIO] upload: rejected uncertain destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(localPath), qPrintable(remotePath));
            return false;
        }
        sftp_file output = sftp_open(m_sftp, encoded.constData(),
            O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (!output) {
            *error = lastError(QStringLiteral("创建远端文件失败"));
            return false;
        }
        const auto cleanup = [&]() {
            if (sftp_unlink(m_sftp, encoded.constData()) != SSH_OK)
                qWarning("[SSHIO] upload: failed to remove partial remote file (source=\"%s\", destination=\"%s\")",
                         qPrintable(localPath), qPrintable(remotePath));
        };
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (!input.atEnd()) {
            if (isCancelled(cancelled)) {
                *error = QStringLiteral("上传已取消");
                sftp_close(output);
                cleanup();
                return false;
            }
            const qint64 read = input.read(buffer.data(), buffer.size());
            if (read < 0) {
                *error = QStringLiteral("读取本地文件失败: %1")
                    .arg(input.errorString());
                sftp_close(output);
                cleanup();
                return false;
            }
            qint64 offset = 0;
            while (offset < read) {
                const ssize_t written = sftp_write(
                    output, buffer.constData() + offset,
                    static_cast<size_t>(read - offset));
                if (written <= 0) {
                    *error = lastError(QStringLiteral("上传文件失败"));
                    sftp_close(output);
                    cleanup();
                    return false;
                }
                offset += written;
                *transferred += written;
                progress(*transferred, input.size());
            }
        }
        if (sftp_close(output) != SSH_OK) {
            *error = lastError(QStringLiteral("关闭远端文件失败"));
            cleanup();
            return false;
        }
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("上传已取消");
            cleanup();
            return false;
        }
        return true;
    }

    bool downloadRecursive(const QString &remotePath, const QString &localPath,
                           const QSharedPointer<QAtomicInt> &cancelled,
                           qint64 *transferred,
                           const std::function<void(qint64, qint64)> &progress,
                           QString *error)
    {
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("下载已取消");
            return false;
        }
        sftp_attributes attributes = attributesFor(remotePath);
        if (!attributes) {
            *error = lastError(QStringLiteral("读取远端项目失败"));
            return false;
        }
        const bool directory = S_ISDIR(attributes->permissions);
        // 单文件已知总大小,进度上报真实 total (R2-IO-P2-6)
        const qint64 totalSize = directory
            ? -1 : static_cast<qint64>(attributes->size);
        sftp_attributes_free(attributes);
        if (directory) {
            const QFileInfo destination(localPath);
            if (destination.exists() || destination.isSymLink()) {
                *error = QStringLiteral("本地目标已存在，拒绝覆盖目录 %1").arg(localPath);
                qWarning("[SSHIO] download: rejected existing directory target (source=\"%s\", destination=\"%s\")",
                         qPrintable(remotePath), qPrintable(localPath));
                return false;
            }
            const QString parent = destination.absolutePath();
            if (!QDir().mkpath(parent)) {
                *error = QStringLiteral("无法创建本地父目录 %1").arg(parent);
                return false;
            }
            QTemporaryDir staging(QDir(parent).filePath(
                QStringLiteral(".qtfilemgr-download-XXXXXX")));
            if (!staging.isValid()) {
                *error = QStringLiteral("无法在 %1 创建下载临时目录").arg(parent);
                return false;
            }
            const auto discardStaging = [&]() {
                if (!staging.remove())
                    qWarning("[SSHIO] download: failed to discard temporary directory (source=\"%s\", staging=\"%s\", destination=\"%s\")",
                             qPrintable(remotePath), qPrintable(staging.path()), qPrintable(localPath));
            };
            SSHListResult children = listDirectory(remotePath, cancelled);
            if (!children.error.isEmpty()) {
                *error = children.error;
            } else {
                for (const FileEntry &entry : children.entries) {
                    if (!downloadRecursive(
                            entry.path, QDir(staging.path()).filePath(entry.name),
                            cancelled, transferred, progress, error)) {
                        break;
                    }
                }
            }
            if (!error->isEmpty()) {
                discardStaging();
                return false;
            }
            if (isCancelled(cancelled)) {
                *error = QStringLiteral("下载已取消");
                discardStaging();
                return false;
            }
            if (QFileInfo::exists(localPath) || QFileInfo(localPath).isSymLink()
                || !QDir().rename(staging.path(), localPath)) {
                *error = QStringLiteral("无法提交下载目录 %1：目标已存在或重命名失败")
                    .arg(localPath);
                qWarning("[SSHIO] download: directory commit failed (source=\"%s\", staging=\"%s\", destination=\"%s\")",
                         qPrintable(remotePath), qPrintable(staging.path()), qPrintable(localPath));
                discardStaging();
                return false;
            }
            staging.setAutoRemove(false);
            return true;
        }

        const QByteArray encoded = remotePath.toUtf8();
        sftp_file input = sftp_open(m_sftp, encoded.constData(), O_RDONLY, 0);
        if (!input) {
            *error = lastError(QStringLiteral("打开远端文件失败"));
            return false;
        }
        QSaveFile output(localPath);
        if (!QDir().mkpath(QFileInfo(localPath).absolutePath())
            || !output.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("无法写入本地文件 %1: %2")
                .arg(localPath, output.errorString());
            sftp_close(input);
            return false;
        }
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (true) {
            if (isCancelled(cancelled)) {
                *error = QStringLiteral("下载已取消");
                sftp_close(input);
                output.cancelWriting();
                return false;
            }
            const ssize_t read = sftp_read(
                input, buffer.data(), static_cast<size_t>(buffer.size()));
            if (read == 0)
                break;
            if (read < 0) {
                *error = lastError(QStringLiteral("下载文件失败"));
                sftp_close(input);
                output.cancelWriting();
                return false;
            }
            if (output.write(buffer.constData(), read) != read) {
                *error = QStringLiteral("写入本地文件失败: %1")
                    .arg(output.errorString());
                sftp_close(input);
                output.cancelWriting();
                return false;
            }
            *transferred += read;
            progress(*transferred, totalSize);
        }
        if (sftp_close(input) != SSH_OK) {
            *error = lastError(QStringLiteral("关闭远端文件失败"));
            output.cancelWriting();
            return false;
        }
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("下载已取消");
            output.cancelWriting();
            return false;
        }
        if (!output.commit()) {
            *error = QStringLiteral("提交本地文件 %1 失败: %2")
                .arg(localPath, output.errorString());
            return false;
        }
        return true;
    }

    bool copyRemoteRecursive(const QString &source, const QString &destination,
                             const QSharedPointer<QAtomicInt> &cancelled,
                             qint64 *transferred,
                             const std::function<void(qint64, qint64)> &progress,
                             QString *error)
    {
        if (isCancelled(cancelled)) {
            *error = QStringLiteral("复制已取消");
            return false;
        }
        // 拒绝自复制与子目录复制: 先建目标再列源会把新目标纳入遍历,
        // 无限递归创建半成品目录树 (审查#11)
        const QString cleanSource = cleanRemotePath(source);
        const QString cleanDestination = cleanRemotePath(destination);
        if (cleanDestination == cleanSource
            || cleanDestination.startsWith(cleanSource == QStringLiteral("/")
                ? QStringLiteral("/") : cleanSource + QLatin1Char('/'))) {
            qWarning("[SSHIO] copyRemoteRecursive: rejected self/subdirectory copy (source=\"%s\", destination=\"%s\")",
                     qPrintable(cleanSource), qPrintable(cleanDestination));
            *error = QStringLiteral("不能将目录复制到其自身或其子目录");
            return false;
        }
        sftp_attributes attributes = attributesFor(source);
        if (!attributes) {
            *error = lastError(QStringLiteral("读取远端源项目失败"));
            return false;
        }
        const bool directory = S_ISDIR(attributes->permissions);
        const mode_t mode = attributes->permissions & 0777;
        // 单文件传输已知总大小,进度上报真实 total 而非恒 -1 (R2-IO-P2-6)
        const qint64 totalSize = directory
            ? -1 : static_cast<qint64>(attributes->size);
        sftp_attributes_free(attributes);
        if (directory) {
            const QByteArray encodedDestination = destination.toUtf8();
            if (sftp_mkdir(m_sftp, encodedDestination.constData(), mode) != SSH_OK) {
                sftp_attributes existing = attributesFor(destination);
                const bool existingDirectory = existing
                    && S_ISDIR(existing->permissions);
                if (existing)
                    sftp_attributes_free(existing);
                if (!existingDirectory) {
                    *error = lastError(QStringLiteral("创建远端目标目录失败"));
                    return false;
                }
            }
            SSHListResult children = listDirectory(source, cancelled);
            if (!children.error.isEmpty()) {
                *error = children.error;
                return false;
            }
            for (const FileEntry &entry : children.entries) {
                if (!copyRemoteRecursive(
                        entry.path, joinedRemotePath(destination, entry.name),
                        cancelled, transferred, progress, error)) {
                    return false;
                }
            }
            return true;
        }

        const QByteArray encodedSource = source.toUtf8();
        const QByteArray encodedDestination = destination.toUtf8();
        sftp_attributes previous = attributesFor(destination);
        if (previous) {
            sftp_attributes_free(previous);
            *error = QStringLiteral("远端目标已存在，拒绝覆盖 %1").arg(destination);
            qWarning("[SSHIO] copy: rejected existing destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return false;
        }
        if (sftp_get_error(m_sftp) != SSH_FX_NO_SUCH_FILE) {
            *error = lastError(QStringLiteral("无法确认远端目标不存在"));
            qWarning("[SSHIO] copy: rejected uncertain destination (source=\"%s\", destination=\"%s\")",
                     qPrintable(source), qPrintable(destination));
            return false;
        }
        sftp_file input = sftp_open(
            m_sftp, encodedSource.constData(), O_RDONLY, 0);
        sftp_file output = input ? sftp_open(
            m_sftp, encodedDestination.constData(),
            O_WRONLY | O_CREAT | O_EXCL, mode) : nullptr;
        if (!input || !output) {
            if (input)
                sftp_close(input);
            if (output)
                sftp_close(output);
            *error = lastError(QStringLiteral("打开远端复制文件失败"));
            return false;
        }
        const auto cleanup = [&]() {
            if (sftp_unlink(m_sftp, encodedDestination.constData()) != SSH_OK)
                qWarning("[SSHIO] copy: failed to remove partial remote file (source=\"%s\", destination=\"%s\")",
                         qPrintable(source), qPrintable(destination));
        };
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (true) {
            if (isCancelled(cancelled)) {
                *error = QStringLiteral("复制已取消");
                sftp_close(input);
                sftp_close(output);
                cleanup();
                return false;
            }
            const ssize_t read = sftp_read(
                input, buffer.data(), static_cast<size_t>(buffer.size()));
            if (read == 0)
                break;
            if (read < 0) {
                *error = lastError(QStringLiteral("读取远端源文件失败"));
                sftp_close(input);
                sftp_close(output);
                cleanup();
                return false;
            }
            qint64 offset = 0;
            while (offset < read) {
                const ssize_t written = sftp_write(
                    output, buffer.constData() + offset,
                    static_cast<size_t>(read - offset));
                if (written <= 0) {
                    *error = lastError(QStringLiteral("写入远端目标文件失败"));
                    sftp_close(input);
                    sftp_close(output);
                    cleanup();
                    return false;
                }
                offset += written;
                *transferred += written;
                progress(*transferred, totalSize);
            }
        }
        const bool inputClosed = sftp_close(input) == SSH_OK;
        const bool outputClosed = sftp_close(output) == SSH_OK;
        const bool closed = inputClosed && outputClosed;
        if (!closed)
            *error = lastError(QStringLiteral("关闭远端复制文件失败"));
        else if (isCancelled(cancelled))
            *error = QStringLiteral("复制已取消");
        if (!error->isEmpty())
            cleanup();
        return error->isEmpty();
    }

    ssh_session m_ssh = nullptr;
    sftp_session m_sftp = nullptr;
    QtFileMgrSSHConnectionConfig m_config;
};

QtFileMgrSSHIO::QtFileMgrSSHIO(QObject *parent)
    : QtFileMgrIO(parent)
    , m_workerThread(new QThread(this))
    , m_worker(new QtFileMgrSSHWorker)
{
    m_workerThread->setObjectName(QStringLiteral("QtFileMgrSSHWorker"));
    m_worker->moveToThread(m_workerThread);
    m_worker->m_facade = this;
    connect(m_workerThread, &QThread::finished,
            m_worker, &QObject::deleteLater);
    m_workerThread->start();
    if (!m_workerThread->isRunning()) {
        // start 失败时 finished 信号永远不会发,worker 的 deleteLater
        // 不会执行,必须手工回收;后端保持未连接状态,后续
        // connectToHost 会在 worker 空指针处失败并给明确报错 (SSH-P2-8)
        qWarning("[SSHIO] ctor: worker thread failed to start; worker reclaimed, backend unusable");
        m_worker->deleteLater();
        m_worker = nullptr;
    }
}

void QtFileMgrSSHIO::noteSessionDead()
{
    if (!m_connected)
        return;
    qWarning("[SSHIO] session died (transport fatal error); marking disconnected (host=\"%s\")",
             qPrintable(connectionLabel(m_config)));
    m_connected = false;
    ++m_sessionGeneration;
    m_entryCache.clear();
    m_directoryCache.clear();
    emit backendStateChanged(false, QStringLiteral("SSH 连接已断开"));
}

QtFileMgrSSHIO::~QtFileMgrSSHIO()
{
    // 明文密码随对象销毁一并抹除 (SSH-P2-6)
    m_config.password.fill(QLatin1Char('\0'));
    m_config.password.clear();
    // 进行中的阻塞操作:事件循环与结果在堆上,标记 facade 已死,
    // 操作收尾后不再回写成员 (SSH-P1-2)
    if (m_activeBlockingState)
        m_activeBlockingState->facadeAlive.storeRelease(0);
    cancelActiveTransfers();
    if (m_worker && m_workerThread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, [this]() {
            m_worker->closeSession();
        }, Qt::BlockingQueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait();
    }
    m_worker = nullptr;
}

void QtFileMgrSSHIO::connectToHost(
    const QtFileMgrSSHConnectionConfig &config)
{
    if (thread() != QThread::currentThread()) {
        qWarning("[SSHIO] connectToHost: rejected cross-thread facade call (backendThread=%p, currentThread=%p, host=\"%s\")",
                 static_cast<void *>(thread()),
                 static_cast<void *>(QThread::currentThread()),
                 qPrintable(config.host));
        emit connectionFinished(false,
                                QStringLiteral("SSH 后端线程与调用线程不一致"));
        return;
    }
    if (m_connecting) {
        // 同目标视为归并:在途连接的完成信号会送达;
        // 不同目标才真正被丢弃,必须回失败,否则调用方
        // 永远等不到 connectionFinished (R2-IO-P2-7)
        const bool sameTarget = m_config.host == config.host
            && m_config.port == config.port
            && m_config.user == config.user;
        if (sameTarget) {
            qWarning("[SSHIO] connectToHost: coalesced duplicate connection (target=\"%s\")",
                     qPrintable(connectionLabel(m_config)));
            return;
        }
        qWarning("[SSHIO] connectToHost: dropped connection to different target while connecting (current=\"%s\", requested=\"%s\")",
                 qPrintable(connectionLabel(m_config)),
                 qPrintable(connectionLabel(config)));
        emit connectionFinished(false, QStringLiteral(
            "正在连接 %1，请等待当前连接完成后重试")
            .arg(connectionLabel(m_config)));
        return;
    }
    if (config.host.isEmpty() || config.user.isEmpty()) {
        qWarning("[SSHIO] connectToHost: rejected invalid config (host=\"%s\", user=\"%s\", port=%d)",
                 qPrintable(config.host), qPrintable(config.user), config.port);
        emit connectionFinished(false, QStringLiteral("SSH 连接参数不完整"));
        return;
    }
    m_config = config;
    m_confirmedHostKeyHash.clear();
    ++m_sessionGeneration;
    m_config.initialPath = normalizedPath(config.initialPath);
    m_connecting = true;
    m_waitingForHostKey = false;
    m_connected = false;
    emit backendStateChanged(false, QStringLiteral("正在连接 %1")
                             .arg(connectionLabel(m_config)));
    startConnection(false);
}

void QtFileMgrSSHIO::startConnection(bool acceptUnknownKey)
{
    if (!m_worker) {
        qWarning("[SSHIO] startConnection: rejected, worker thread unavailable (host=\"%s\")",
                 qPrintable(connectionLabel(m_config)));
        m_connecting = false;
        m_waitingForHostKey = false;
        const QString error = QStringLiteral("SSH 工作线程不可用");
        emit backendStateChanged(false, error);
        emit connectionFinished(false, error);
        return;
    }
    const QtFileMgrSSHConnectionConfig config = m_config;
    const QByteArray confirmedHostKeyHash = m_confirmedHostKeyHash;
    // 连接完成/未知主机回调都校验代次: 断开后迟到的成功回调不得
    // 复活 m_connected 并发成功信号 (审查#13)
    const quint64 generation = m_sessionGeneration;
    QPointer<QtFileMgrSSHIO> guarded(this);
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, guarded, config, acceptUnknownKey, confirmedHostKeyHash,
         generation]() {
        if (!guarded)
            return;
        const SSHConnectResult result = worker->connectSession(
            config, acceptUnknownKey, confirmedHostKeyHash);
        QTimer::singleShot(0, guarded, [guarded, result, config, generation]() {
            if (!guarded)
                return;
            if (guarded->m_sessionGeneration != generation) {
                qWarning("[SSHIO] connect: dropped stale completion (host=\"%s\", generation=%llu, current=%llu)",
                         qPrintable(connectionLabel(config)),
                         static_cast<unsigned long long>(generation),
                         static_cast<unsigned long long>(guarded->m_sessionGeneration));
                return;
            }
            if (result.unknownHost) {
                guarded->m_confirmedHostKeyHash = result.hostKeyHash;
                guarded->m_waitingForHostKey = true;
                emit guarded->unknownHostKey(
                    config.host, config.port, result.fingerprint);
                // 若无人连接 unknownHostKey 信号(或对话框被异常销毁),
                // m_connecting/m_waitingForHostKey 会永久卡住,后续连接全被
                // 当作重复连接拒绝;超时按"用户拒绝"同等处理 (SSH-P2-2)
                QTimer::singleShot(120000, guarded, [guarded, generation]() {
                    if (!guarded || !guarded->m_waitingForHostKey
                        || guarded->m_sessionGeneration != generation)
                        return;
                    qWarning("[SSHIO] connect: host key confirmation timed out after 120s (host=\"%s\"), treating as rejected",
                             qPrintable(connectionLabel(guarded->m_config)));
                    guarded->m_confirmedHostKeyHash.clear();
                    guarded->m_waitingForHostKey = false;
                    guarded->m_connecting = false;
                    const QString error = QStringLiteral(
                        "主机密钥确认超时，连接已取消");
                    emit guarded->backendStateChanged(false, error);
                    emit guarded->connectionFinished(false, error);
                });
                return;
            }
            guarded->m_connecting = false;
            guarded->m_waitingForHostKey = false;
            guarded->m_confirmedHostKeyHash.clear();
            guarded->m_connected = result.success;
            if (result.success) {
                guarded->m_entryCache.clear();
                guarded->m_directoryCache.clear();
                FileEntry root;
                root.name = QStringLiteral("/");
                root.path = QStringLiteral("/");
                root.isDir = true;
                guarded->m_entryCache.insert(QStringLiteral("/"), root);
            }
            emit guarded->backendStateChanged(result.success,
                result.success ? QStringLiteral("已连接 %1")
                                     .arg(connectionLabel(config))
                               : result.error);
            emit guarded->connectionFinished(result.success, result.error);
        });
    }, Qt::QueuedConnection);
}

void QtFileMgrSSHIO::acceptUnknownHostKey(bool accepted)
{
    if (!m_connecting || !m_waitingForHostKey) {
        qWarning("[SSHIO] acceptUnknownHostKey: ignored stale decision (host=\"%s\", connecting=%d, waiting=%d, accepted=%d)",
                 qPrintable(m_config.host), m_connecting ? 1 : 0,
                 m_waitingForHostKey ? 1 : 0, accepted ? 1 : 0);
        return;
    }
    m_waitingForHostKey = false;
    if (!accepted) {
        m_confirmedHostKeyHash.clear();
        m_connecting = false;
        const QString error = QStringLiteral("用户拒绝了未知服务器的主机密钥");
        emit backendStateChanged(false, error);
        emit connectionFinished(false, error);
        return;
    }
    startConnection(true);
}

void QtFileMgrSSHIO::cancelActiveTransfers()
{
    if (m_activeTransferCancellations.isEmpty())
        return;
    qWarning("[SSHIO] cancelActiveTransfers: cancelling %d in-flight transfer(s) (host=\"%s\")",
             m_activeTransferCancellations.size(),
             qPrintable(connectionLabel(m_config)));
    for (const QSharedPointer<QAtomicInt> &cancelled : m_activeTransferCancellations) {
        if (cancelled)
            cancelled->storeRelease(1);
    }
    m_activeTransferCancellations.clear();
}

void QtFileMgrSSHIO::disconnectFromHost()
{
    ++m_sessionGeneration;
    m_confirmedHostKeyHash.clear();
    // 断开后尽快抹掉内存中的明文密码;connectToHost 会重新填入 (SSH-P2-6)
    m_config.password.fill(QLatin1Char('\0'));
    m_config.password.clear();
    cancelActiveTransfers();
    if (!m_worker || !m_workerThread->isRunning())
        return;
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(m_worker,
        [worker]() { worker->closeSession(); },
        Qt::BlockingQueuedConnection);
    m_connected = false;
    m_connecting = false;
    m_waitingForHostKey = false;
    m_entryCache.clear();
    m_directoryCache.clear();
    emit backendStateChanged(false, QStringLiteral("SSH 连接已断开"));
}

QString QtFileMgrSSHIO::normalizedPath(const QString &path) const
{
    return cleanRemotePath(path);
}

QString QtFileMgrSSHIO::safeEntryName(const QString &directory, const QString &serverName)
{
    QString name = serverName;
    if (name.startsWith(QLatin1Char('/'))) {
        const QString parent = cleanRemotePath(directory);
        const QString prefix = parent == QStringLiteral("/") ? parent
                                                               : parent + QLatin1Char('/');
        if (!name.startsWith(prefix))
            return {};
        name = name.mid(prefix.size());
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/')) || name.contains(QChar::Null))
        return {};
    return name;
}

QString QtFileMgrSSHIO::rootPath() const
{
    return normalizedPath(m_config.initialPath);
}

QString QtFileMgrSSHIO::displayName(const QString &path) const
{
    const QString clean = normalizedPath(path);
    return clean == QStringLiteral("/")
        ? connectionLabel(m_config) : QFileInfo(clean).fileName();
}

bool QtFileMgrSSHIO::acceptsUrl(const QUrl &url) const
{
    return url.scheme().toLower() == QStringLiteral("ssh")
        && url.host().compare(m_config.host, Qt::CaseInsensitive) == 0
        && url.port(22) == m_config.port
        && (url.userName().isEmpty() || url.userName() == m_config.user);
}

QUrl QtFileMgrSSHIO::urlForPath(const QString &path) const
{
    QUrl url;
    url.setScheme(QStringLiteral("ssh"));
    url.setHost(m_config.host);
    url.setPort(m_config.port);
    url.setUserName(m_config.user);
    url.setPath(normalizedPath(path));
    return url;
}

QString QtFileMgrSSHIO::pathFromUrl(const QUrl &url) const
{
    if (!acceptsUrl(url)) {
        qWarning("[SSHIO] pathFromUrl: rejected foreign URL (backend=\"%s\", url=\"%s\")",
                 qPrintable(connectionLabel(m_config)),
                 qPrintable(url.toString(QUrl::RemovePassword)));
        return QString();
    }
    return normalizedPath(url.path());
}

bool QtFileMgrSSHIO::exists(const QString &path) const
{
    return m_connected && m_entryCache.contains(normalizedPath(path));
}

QList<FileEntry> QtFileMgrSSHIO::listDir(const QString &path) const
{
    return m_directoryCache.value(normalizedPath(path));
}

QtFileMgrDirectoryRequest *QtFileMgrSSHIO::listDirAsync(
    const QString &path, QObject *parent) const
{
    auto *request = new QtFileMgrDirectoryRequest(parent);
    if (!m_connected) {
        qWarning("[SSHIO] listDirAsync: rejected disconnected request (host=\"%s\", path=\"%s\")",
                 qPrintable(connectionLabel(m_config)), qPrintable(path));
        QTimer::singleShot(0, request, [request]() {
            request->finish(QStringLiteral("SSH 连接不可用"));
        });
        return request;
    }
    const QString directory = normalizedPath(path);
    const quint64 generation = m_sessionGeneration;
    const QSharedPointer<QAtomicInt> cancelled = request->cancellationState();
    QPointer<QtFileMgrDirectoryRequest> guardedRequest(request);
    QPointer<const QtFileMgrSSHIO> guardedBackend(this);
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, guardedBackend, guardedRequest, directory, cancelled, generation]() {
        if (!guardedBackend || cancelled->loadAcquire() != 0) {
            // 排队即取消/后端已毁的请求也必须收尾,调用方可能在等
            // finished (日志-c)
            qWarning("[SSHIO] listDirAsync: request cancelled or backend gone before worker start (path=\"%s\")",
                     qPrintable(directory));
            if (guardedRequest) {
                QTimer::singleShot(0, guardedRequest, [guardedRequest]() {
                    if (guardedRequest)
                        guardedRequest->finish(QStringLiteral("操作已取消"));
                });
            }
            return;
        }
        const SSHListResult result =
            worker->listDirectory(directory, cancelled);
        if (!guardedRequest) {
            qWarning("[SSHIO] listDirAsync: request destroyed before worker result (path=\"%s\", count=%d, error=\"%s\")",
                     qPrintable(directory), result.entries.size(),
                     qPrintable(result.error));
            return;
        }
        QTimer::singleShot(0, guardedRequest,
            [guardedBackend, guardedRequest, directory, result, generation]() {
            if (!guardedRequest)
                return;
            if (!guardedBackend) {
                qWarning("[SSHIO] listDirAsync: backend destroyed before result delivery (path=\"%s\")",
                         qPrintable(directory));
                guardedRequest->finish(QStringLiteral("SSH 后端已断开"));
                return;
            }
            if (guardedBackend->m_sessionGeneration != generation
                || !guardedBackend->m_connected) {
                // 断连重连后旧会话结果不得写入新会话缓存 (R2-IO-P1-1)
                qWarning("[SSHIO] listDirAsync: dropped stale session result (path=\"%s\", generation=%llu, current=%llu)",
                         qPrintable(directory),
                         static_cast<unsigned long long>(generation),
                         static_cast<unsigned long long>(guardedBackend->m_sessionGeneration));
                guardedRequest->finish(QStringLiteral("SSH 连接已变化"));
                return;
            }
            if (guardedRequest->isCancelled()) {
                qWarning("[SSHIO] listDirAsync: discarded cancelled result (host=\"%s\", path=\"%s\", count=%d)",
                         qPrintable(connectionLabel(guardedBackend->m_config)),
                         qPrintable(directory), result.entries.size());
                guardedRequest->finish();
                return;
            }
            if (result.error.isEmpty()) {
                guardedBackend->updateCache(directory, result.entries);
                guardedRequest->deliverBatch(result.entries);
            }
            guardedRequest->finish(result.error);
        });
    }, Qt::QueuedConnection);
    return request;
}

void QtFileMgrSSHIO::updateCache(
    const QString &directory, const QList<FileEntry> &entries) const
{
    const QList<FileEntry> oldEntries = m_directoryCache.value(directory);
    for (const FileEntry &entry : oldEntries)
        m_entryCache.remove(entry.path);
    m_directoryCache.insert(directory, entries);
    FileEntry directoryEntry = m_entryCache.value(directory);
    directoryEntry.path = directory;
    directoryEntry.name = directory == QStringLiteral("/")
        ? QStringLiteral("/") : QFileInfo(directory).fileName();
    directoryEntry.isDir = true;
    m_entryCache.insert(directory, directoryEntry);
    for (const FileEntry &entry : entries)
        m_entryCache.insert(entry.path, entry);
}

bool QtFileMgrSSHIO::isDir(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path)).isDir;
}

bool QtFileMgrSSHIO::isWritable(const QString &path) const
{
    if (!m_connected)
        return false;
    const QString clean = normalizedPath(path);
    if (clean == QStringLiteral("/") && !m_entryCache.contains(clean))
        return true;
    const auto it = m_entryCache.constFind(clean);
    return it != m_entryCache.constEnd() && !it->isReadOnly;
}

bool QtFileMgrSSHIO::runBlockingOperation(
    BlockingOperation operation, const QString &firstPath,
    const QString &secondPath, const QByteArray &contents,
    QFile::Permissions permissions, QString *error)
{
    if (error)
        error->clear();
    if (!m_connected) {
        if (error)
            *error = QStringLiteral("SSH 后端已断开");
        qWarning("[SSHIO] operation: rejected disconnected backend (operation=%d, path=\"%s\")",
                 static_cast<int>(operation), qPrintable(firstPath));
        return false;
    }
    if (m_blockingOperationActive) {
        if (error)
            *error = QStringLiteral("SSH 后端正在处理另一个同步操作");
        qWarning("[SSHIO] operation: rejected reentrant operation (operation=%d, path=\"%s\")",
                 static_cast<int>(operation), qPrintable(firstPath));
        return false;
    }
    m_blockingOperationActive = true;
    QSharedPointer<QtFileMgrSSHBlockingState> state(
        new QtFileMgrSSHBlockingState);
    state->loop = new QEventLoop;
    state->facadeAlive.storeRelease(1);
    m_activeBlockingState = state;
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, state, operation, firstPath, secondPath, contents,
         permissions]() {
        switch (operation) {
        case MakeDirectory:
            state->result = worker->makeDirectory(firstPath);
            break;
        case RenamePath:
            state->result = worker->renamePath(firstPath, secondPath);
            break;
        case RenamePathNoReplace:
            state->result = worker->renamePath(firstPath, secondPath, true);
            break;
        case RemovePath:
            state->result = worker->removePath(firstPath);
            break;
        case WriteContents:
            state->result = worker->writeContents(firstPath, contents);
            break;
        case ChangePermissions:
            state->result = worker->changePermissions(firstPath, permissions);
            break;
        }
        // facade 已销毁也必须退出事件循环,否则 GUI 永久挂死 (SSH-P1-2)
        QMetaObject::invokeMethod(state->loop, "quit", Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    state->loop->exec(QEventLoop::ExcludeUserInputEvents);
    const SSHOperationResult result = state->result;
    const bool facadeAlive = state->facadeAlive.loadAcquire() != 0;
    delete state->loop;
    state->loop = nullptr;
    if (!facadeAlive) {
        if (error)
            *error = QStringLiteral("SSH 后端在操作返回前已销毁；请检查服务器端结果");
        return false;  // this 已销毁,不得再触碰成员
    }
    m_activeBlockingState.clear();
    m_blockingOperationActive = false;
    if (!result.success) {
        if (error)
            *error = result.error;
        qWarning("[SSHIO] operation failed (backend=\"%s\", operation=%d, source=\"%s\", destination=\"%s\", error=\"%s\")",
                 qPrintable(connectionLabel(m_config)),
                 static_cast<int>(operation), qPrintable(firstPath),
                 qPrintable(secondPath), qPrintable(result.error));
        emit backendStateChanged(m_connected, result.error);
        return false;
    }
    invalidatePath(firstPath);
    if (!secondPath.isEmpty())
        invalidatePath(secondPath);
    QStringList changedPaths{firstPath};
    if (!secondPath.isEmpty())
        changedPaths.append(secondPath);
    emit pathsChanged(changedPaths);
    return true;
}

void QtFileMgrSSHIO::invalidatePath(const QString &path)
{
    const QString clean = normalizedPath(path);
    m_entryCache.remove(clean);
    m_directoryCache.remove(clean);
    const QString parent = QFileInfo(clean).path();
    m_directoryCache.remove(parent.isEmpty() ? QStringLiteral("/") : parent);
}

bool QtFileMgrSSHIO::mkdir(const QString &path)
{
    return runBlockingOperation(MakeDirectory, normalizedPath(path));
}

bool QtFileMgrSSHIO::rename(const QString &oldPath, const QString &newPath)
{
    return runBlockingOperation(RenamePath, normalizedPath(oldPath),
                                normalizedPath(newPath));
}

bool QtFileMgrSSHIO::renameNoReplace(const QString &oldPath,
                                    const QString &newPath, QString *error)
{
    return runBlockingOperation(RenamePathNoReplace, normalizedPath(oldPath),
                                normalizedPath(newPath), {}, {}, error);
}

void QtFileMgrSSHIO::renameAsync(const QString &oldPath, const QString &newPath,
                                 QObject *ctx, OpCallback cb)
{
    const QString from = normalizedPath(oldPath);
    const QString to = normalizedPath(newPath);
    // 与同步入口一致的就绪检查 + 绑定会话代次 (R2-14)
    if (!m_connected) {
        qWarning("[SSHIO] renameAsync: rejected disconnected backend (from=\"%s\", to=\"%s\")",
                 qPrintable(from), qPrintable(to));
        if (ctx) {
            QTimer::singleShot(0, ctx, [cb, to]() {
                cb(false, to);
            });
        }
        return;
    }
    const quint64 generation = m_sessionGeneration;
    const QPointer<QObject> guardedCtx(ctx);
    const QPointer<QtFileMgrSSHIO> guardedBackend(this);
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, guardedBackend, guardedCtx, from, to, cb, generation]() {
        if (!guardedBackend) {
            qWarning("[SSHIO] renameAsync: backend destroyed before worker run (from=\"%s\", to=\"%s\")",
                     qPrintable(from), qPrintable(to));
            if (guardedCtx) {
                QTimer::singleShot(0, guardedCtx, [cb, to]() {
                    cb(false, to);
                });
            }
            return;
        }
        const SSHOperationResult result = worker->renamePath(from, to);
        if (!guardedCtx) {
            qWarning("[SSHIO] renameAsync: context destroyed before delivery (from=\"%s\", to=\"%s\", success=%d)",
                     qPrintable(from), qPrintable(to), result.success ? 1 : 0);
            return;
        }
        QTimer::singleShot(0, guardedCtx,
            [guardedBackend, guardedCtx, cb, result, from, to, generation]() {
            if (!guardedCtx)
                return;
            // 旧会话结果不得更新新会话缓存 (R2-14)
            if (guardedBackend
                && guardedBackend->m_sessionGeneration != generation) {
                qWarning("[SSHIO] renameAsync: dropped stale session result (from=\"%s\", to=\"%s\", generation=%llu)",
                         qPrintable(from), qPrintable(to),
                         static_cast<unsigned long long>(generation));
                cb(false, to);
                return;
            }
            if (result.success && guardedBackend) {
                guardedBackend->invalidatePath(from);
                guardedBackend->invalidatePath(to);
                emit guardedBackend->pathsChanged({from, to});
            }
            cb(result.success, to);
        });
    }, Qt::QueuedConnection);
}

bool QtFileMgrSSHIO::remove(const QString &path)
{
    return removePermanent(path);
}

bool QtFileMgrSSHIO::removePermanent(const QString &path)
{
    return runBlockingOperation(RemovePath, normalizedPath(path));
}

bool QtFileMgrSSHIO::moveToTrash(const QString &path, QString *error)
{
    if (error)
        *error = QStringLiteral("SSH/SFTP 不提供可恢复的回收站操作");
    qWarning("[SSHIO] moveToTrash: rejected unsupported operation (backend=\"%s\", path=\"%s\")",
             qPrintable(connectionLabel(m_config)), qPrintable(path));
    return false;
}

bool QtFileMgrSSHIO::copy(const QString &source, const QString &destination)
{
    qWarning("[SSHIO] copy: rejected synchronous copy; use copyRequest (source=\"%s\", destination=\"%s\")",
             qPrintable(source), qPrintable(destination));
    return false;
}

bool QtFileMgrSSHIO::writeFile(const QString &path,
                               const QByteArray &contents)
{
    return runBlockingOperation(WriteContents, normalizedPath(path),
                                QString(), contents);
}

bool QtFileMgrSSHIO::canWrite() const
{
    return m_connected;
}

FileEntry QtFileMgrSSHIO::stat(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path));
}

qint64 QtFileMgrSSHIO::fileSize(const QString &path) const
{
    const auto it = m_entryCache.constFind(normalizedPath(path));
    return it == m_entryCache.constEnd() ? -1 : it->size;
}

QString QtFileMgrSSHIO::mimeType(const QString &path) const
{
    return QMimeDatabase().mimeTypeForFile(QFileInfo(path).fileName(),
                                          QMimeDatabase::MatchExtension).name();
}

QtFileMgrPreviewRequest *QtFileMgrSSHIO::previewRequest(
    const QString &path, qint64 maxBytes, bool image, QObject *parent) const
{
    auto *request = new QtFileMgrPreviewRequest(parent);
    const QString remotePath = normalizedPath(path);
    if (!m_connected || !m_worker || !m_workerThread->isRunning()
        || thread() != QThread::currentThread()) {
        qWarning("[SSHIO] preview: rejected unavailable backend (path=\"%s\", connected=%d)",
                 qPrintable(remotePath), m_connected ? 1 : 0);
        QTimer::singleShot(0, request, [request]() {
            request->finish({}, false, QStringLiteral("SSH 连接不可用"));
        });
        return request;
    }
    const quint64 generation = m_sessionGeneration;
    const auto cancelled = request->cancellationState();
    QPointer<QtFileMgrPreviewRequest> guardedRequest(request);
    QPointer<const QtFileMgrSSHIO> backend(this);
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, backend, guardedRequest, cancelled, remotePath, maxBytes, image, generation]() {
        if (cancelled->loadAcquire() || !backend) {
            qWarning("[SSHIO] preview: skipped cancelled/destroyed request (path=\"%s\")",
                     qPrintable(remotePath));
            if (guardedRequest)
                QTimer::singleShot(0, guardedRequest, [guardedRequest]() {
                    if (guardedRequest) guardedRequest->finish({}, false, QStringLiteral("预览已取消"));
                });
            return;
        }
        const SSHPreviewResult result = worker->preview(remotePath, maxBytes, image, cancelled);
        if (!guardedRequest) {
            qWarning("[SSHIO] preview: request destroyed before delivery (path=\"%s\")",
                     qPrintable(remotePath));
            return;
        }
        QTimer::singleShot(0, guardedRequest,
            [backend, guardedRequest, result, generation, remotePath]() {
            if (!guardedRequest) return;
            if (!backend || backend->m_sessionGeneration != generation || !backend->m_connected) {
                qWarning("[SSHIO] preview: dropped stale session result (path=\"%s\", generation=%llu)",
                         qPrintable(remotePath), static_cast<unsigned long long>(generation));
                guardedRequest->finish({}, false, QStringLiteral("SSH 连接已变化"));
                return;
            }
            guardedRequest->finish(result.bytes, result.truncated, result.error);
        });
    }, Qt::QueuedConnection);
    return request;
}

QDateTime QtFileMgrSSHIO::modificationTime(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path)).modified;
}

QFile::Permissions QtFileMgrSSHIO::permissions(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path)).permissionBits;
}

QString QtFileMgrSSHIO::owner(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path)).ownerName;
}

QString QtFileMgrSSHIO::group(const QString &path) const
{
    return m_entryCache.value(normalizedPath(path)).groupName;
}

bool QtFileMgrSSHIO::chmod(const QString &path,
                           QFile::Permissions permissions)
{
    return runBlockingOperation(ChangePermissions, normalizedPath(path),
                                QString(), QByteArray(), permissions);
}

QIcon QtFileMgrSSHIO::fileIcon(const QString &path) const
{
    const bool directory = isDir(path);
    return QIcon::fromTheme(directory ? QStringLiteral("folder-remote")
                                      : QStringLiteral("text-x-generic"));
}

QtFileMgrIO::Capabilities QtFileMgrSSHIO::capabilities() const
{
    Capabilities result = ReadCapability | NonBlockingListingCapability
        | NonBlockingPreviewCapability | MimeTypeCapability
        | RemoteCapability | TransferProgressCapability
        | LocalExportCapability | PermissionCapability;
    if (m_connected)
        result |= WriteCapability;
    return result;
}

QtFileMgrTransferRequest *QtFileMgrSSHIO::startTransfer(
    const QString &source, const QString &destination,
    bool move, bool exportToLocal, QObject *parent, bool importFromLocal)
{
    auto *request = new QtFileMgrTransferRequest(parent);
    if (!m_connected) {
        qWarning("[SSHIO] transfer: rejected disconnected backend (source=\"%s\", destination=\"%s\", move=%d, export=%d)",
                 qPrintable(source), qPrintable(destination),
                 move ? 1 : 0, exportToLocal ? 1 : 0);
        QTimer::singleShot(0, request, [request, destination]() {
            request->finish(false, destination,
                            QStringLiteral("SSH 连接不可用"));
        });
        return request;
    }
    const QSharedPointer<QAtomicInt> cancelled = request->cancellationState();
    m_activeTransferCancellations.append(cancelled);
    QPointer<QtFileMgrTransferRequest> guardedRequest(request);
    QPointer<QtFileMgrSSHIO> guardedBackend(this);
    QtFileMgrSSHWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, guardedBackend, guardedRequest, source, destination, move,
         exportToLocal, cancelled, importFromLocal]() {
        // 断开/析构已置取消位时,排队中的传输不再启动 (R2-IO-P1-2)
        if (cancelled->loadAcquire() != 0) {
            if (guardedRequest) {
                QTimer::singleShot(0, guardedRequest,
                    [guardedBackend, guardedRequest, destination, cancelled]() {
                    if (guardedBackend)
                        guardedBackend->m_activeTransferCancellations.removeAll(cancelled);
                    if (guardedRequest)
                        guardedRequest->finish(false, destination,
                                               QStringLiteral("操作已取消"));
                });
            } else if (guardedBackend) {
                QTimer::singleShot(0, guardedBackend,
                    [guardedBackend, cancelled]() {
                    if (guardedBackend)
                        guardedBackend->m_activeTransferCancellations.removeAll(cancelled);
                });
            }
            return;
        }
        if (!guardedBackend)
            return;
        const auto progress = [guardedRequest](qint64 done, qint64 total) {
            if (!guardedRequest)
                return;
            QTimer::singleShot(0, guardedRequest,
                [guardedRequest, done, total]() {
                if (guardedRequest)
                    guardedRequest->reportProgress(done, total);
            });
        };
        const SSHOperationResult result = worker->transfer(
            source, destination, move, exportToLocal, importFromLocal,
            cancelled, progress);
        if (!guardedRequest) {
            qWarning("[SSHIO] transfer: request destroyed before worker result (source=\"%s\", destination=\"%s\", success=%d, error=\"%s\")",
                     qPrintable(source), qPrintable(destination),
                     result.success ? 1 : 0, qPrintable(result.error));
            if (guardedBackend) {
                QTimer::singleShot(0, guardedBackend,
                    [guardedBackend, cancelled]() {
                    if (guardedBackend)
                        guardedBackend->m_activeTransferCancellations.removeAll(cancelled);
                });
            }
            return;
        }
        QTimer::singleShot(0, guardedRequest,
            [guardedBackend, guardedRequest, source, destination,
             exportToLocal, result, cancelled]() {
            if (guardedBackend)
                guardedBackend->m_activeTransferCancellations.removeAll(cancelled);
            if (!guardedRequest)
                return;
            if (!guardedBackend) {
                qWarning("[SSHIO] transfer: backend destroyed before result delivery (source=\"%s\", destination=\"%s\")",
                         qPrintable(source), qPrintable(destination));
                guardedRequest->finish(false, destination,
                                        QStringLiteral("SSH 后端已断开"));
                return;
            }
            if (result.success && !exportToLocal) {
                guardedBackend->invalidatePath(source);
                guardedBackend->invalidatePath(destination);
                emit guardedBackend->pathsChanged({source, destination});
                // A direct notification slot can close the view/job that owns
                // the request. The committed result no longer has a receiver.
                if (!guardedRequest) {
                    qWarning("[SSHIO] transfer: request destroyed by pathsChanged notification; skipped result delivery (source=\"%s\", destination=\"%s\", success=%d)",
                             qPrintable(source), qPrintable(destination),
                             result.success ? 1 : 0);
                    return;
                }
            }
            guardedRequest->finish(result.success, destination, result.error);
        });
    }, Qt::QueuedConnection);
    return request;
}

QtFileMgrTransferRequest *QtFileMgrSSHIO::copyRequest(
    const QString &source, const QString &destination, QObject *parent)
{
    return startTransfer(source, destination, false, false, parent);
}

QtFileMgrTransferRequest *QtFileMgrSSHIO::moveRequest(
    const QString &source, const QString &destination, QObject *parent)
{
    return startTransfer(source, destination, true, false, parent);
}

QtFileMgrTransferRequest *QtFileMgrSSHIO::exportToLocalRequest(
    const QString &source, const QString &localDestination, QObject *parent)
{
    return startTransfer(source, localDestination, false, true, parent);
}

QtFileMgrTransferRequest *QtFileMgrSSHIO::importFromLocalRequest(
    const QString &localSource, const QString &remoteDestination,
    bool move, QObject *parent)
{
    return startTransfer(localSource, remoteDestination, move, false, parent,
                         true);
}
