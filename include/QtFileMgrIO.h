// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRIO_H
#define QTFILEMGRIO_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QIcon>
#include <QPixmap>
#include <QList>
#include <QFile>
#include <QAtomicInt>
#include <QSharedPointer>
#include <QUrl>
#include <functional>


// ─── 数据结构 ──────────────────────────────────────────────

/// 文件/目录条目 (参照 KIO::UDSEntry, 精简为 FlatModel 直接消费的字段)
struct FileEntry {
    QString name;            ///< 基本文件名 (不含路径)
    QString path;            ///< 完整路径 (IO 相关的绝对/相对路径)
    qint64 size = 0;         ///< 文件大小 (目录为 0)
    QDateTime modified;      ///< 最后修改时间
    bool isDir = false;      ///< 是否为目录
    bool isHidden = false;   ///< 是否为隐藏条目
    bool isReadOnly = false; ///< 是否为只读
    bool isSymlink = false;  ///< 是否为符号链接
    bool hasVersions = false;///< 是否有版本历史
    int versionCount = 0;    ///< 版本数量
    QFile::Permissions permissionBits = {}; ///< 后端已知的权限位
    QString ownerName;       ///< 所有者名称，未知时为空
    QString groupName;       ///< 用户组名称，未知时为空
};

/// 一次目录读取请求。请求对象负责取消状态和分批结果投递。
class QtFileMgrDirectoryRequest : public QObject {
    Q_OBJECT
public:
    explicit QtFileMgrDirectoryRequest(QObject *parent = nullptr);
    ~QtFileMgrDirectoryRequest() override;

    void cancel();
    bool isCancelled() const;
    QSharedPointer<QAtomicInt> cancellationState() const {
        return m_cancelled;
    }

    void deliverBatch(const QList<FileEntry> &entries);
    void finish(const QString &error = QString());
    bool isFinished() const { return m_finished; }
    QList<FileEntry> deliveredEntries() const { return m_deliveredEntries; }
    QString errorString() const { return m_error; }

signals:
    void entriesReady(const QList<FileEntry> &entries);
    void finished(const QString &error);

private:
    QSharedPointer<QAtomicInt> m_cancelled;
    QList<FileEntry> m_deliveredEntries;
    QString m_error;
    bool m_finished = false;
};

/// Bytes only: consumers decode text/images on the GUI thread. Cancellation is
/// cooperative; finished is emitted on the request's owning thread.
class QtFileMgrPreviewRequest : public QObject {
    Q_OBJECT
public:
    explicit QtFileMgrPreviewRequest(QObject *parent = nullptr);
    ~QtFileMgrPreviewRequest() override;
    void cancel();
    bool isCancelled() const;
    QSharedPointer<QAtomicInt> cancellationState() const { return m_cancelled; }
    void finish(const QByteArray &bytes, bool truncated = false,
                const QString &error = QString());
    // 同步后端可能在调用方连接信号前完成,补偿消费用 (DLG-P1-3)
    bool isFinished() const { return m_finished; }
    QByteArray resultBytes() const { return m_bytes; }
    bool resultTruncated() const { return m_truncated; }
    QString errorString() const { return m_error; }
signals:
    void finished(const QByteArray &bytes, bool truncated, const QString &error);
private:
    QSharedPointer<QAtomicInt> m_cancelled;
    QByteArray m_bytes;
    QString m_error;
    bool m_truncated = false;
    bool m_finished = false;
};

/// 单个复制/移动请求。后端可报告字节进度，并监听 cancellationRequested
/// 终止底层网络请求；finished 始终携带最终取消状态。
class QtFileMgrTransferRequest : public QObject {
    Q_OBJECT
public:
    explicit QtFileMgrTransferRequest(QObject *parent = nullptr);
    ~QtFileMgrTransferRequest() override;

    void cancel();
    bool isCancelled() const;
    bool isFinished() const { return m_finished; }
    bool succeeded() const { return m_success; }
    QString resultPath() const { return m_resultPath; }
    QString errorString() const { return m_error; }
    QSharedPointer<QAtomicInt> cancellationState() const {
        return m_cancelled;
    }

    void reportProgress(qint64 bytesTransferred, qint64 bytesTotal = -1);
    void finish(bool success, const QString &newPath,
                const QString &error = QString());

signals:
    void progress(qint64 bytesTransferred, qint64 bytesTotal);
    void cancellationRequested();
    void finished(bool success, const QString &newPath,
                  const QString &error, bool cancelled);

private:
    QSharedPointer<QAtomicInt> m_cancelled;
    bool m_finished = false;
    bool m_success = false;
    QString m_resultPath;
    QString m_error;
};

/// 版本信息 (参照 KIO 版本控制插件接口)
struct VersionInfo {
    int version = 0;         ///< 版本号
    QDateTime timestamp;     ///< 版本创建时间
    qint64 size = 0;         ///< 该版本文件大小
    QString author;          ///< 创建者
    QString comment;         ///< 版本注释
};

/// 文件大小格式化 (通用工具)
inline static QString formatFileSize(qint64 bytes)
{
    // 负值(后端未知/错误约定 -1)不伪装成 "0 B" (IO-P2-7)
    if (bytes < 0) return QStringLiteral("-");
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1048576) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1073741824) return QString::number(bytes / 1048576.0, 'f', 1) + " MB";
    if (bytes < 1099511627776LL) return QString::number(bytes / 1073741824.0, 'f', 2) + " GB";
    return QString::number(bytes / 1099511627776.0, 'f', 2) + " TB";
}

// ─── IO 抽象接口 ───────────────────────────────────────────

/// 文件 I/O 抽象接口 — 参照 KDE KIO::Job / KFileItem 设计
///
/// 所有文件操作通过此接口进行，使 FileView/FlatModel 不依赖具体后端。
/// 子类实现对应本地文件系统 (QtFileMgrLocalIO) 或远端协议 (QtFileMgrNetworkIO)。
///
/// 使用方式:
///   - 集成项目创建 NetworkIO 实例, 通过 FileView::setIO() 注入
///   - nullptr = 默认 LocalIO (FileView 内部自动创建)
///
/// 方法分为三类:
///   [纯虚]  — 子类必须实现, 核心文件操作
///   [可选]  — 基类提供空/默认实现, 子类按需覆盖
///   [能力]  — 查询后端支持的特性
class QtFileMgrIO : public QObject {
    Q_OBJECT
public:
    explicit QtFileMgrIO(QObject *parent = nullptr) : QObject(parent) {}

    enum Capability {
        ReadCapability                = 0x0001,
        WriteCapability               = 0x0002,
        NonBlockingListingCapability  = 0x0004,
        ChangeNotificationCapability  = 0x0008,
        TransferProgressCapability    = 0x0010,
        VersionsCapability            = 0x0020,
        VersionRestoreCapability      = 0x0040,
        ThumbnailCapability           = 0x0080,
        SymlinkCapability             = 0x0100,
        PermissionCapability          = 0x0200,
        MimeTypeCapability            = 0x0400,
        RemoteCapability              = 0x0800,
        TrashCapability               = 0x1000,
        LocalExportCapability         = 0x2000,
        NonBlockingPreviewCapability  = 0x4000
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)
    Q_FLAG(Capabilities)

    // ══════════════════════════════════════════════════════════
    // [纯虚] 核心文件操作 — 子类必须实现
    // ══════════════════════════════════════════════════════════

    /// 返回此后端的根路径 (本地: 如 /home/user; 远端: 如 remote:///)
    virtual QString rootPath() const = 0;

    /// 返回路径的人类可读显示名 (通常为 basename)
    virtual QString displayName(const QString &path) const = 0;

    /// 判断 URL 是否属于当前后端。默认本地后端接受 file://，远端后端
    /// 接受 remote://；协议型后端应覆盖，避免不同主机之间误用同一会话。
    virtual bool acceptsUrl(const QUrl &url) const;

    /// 后端路径与外部 URL 之间的转换。文件视图内部只保存后端路径，
    /// 导航历史、剪贴板和 Places 使用 URL 保留协议及连接标识。
    virtual QUrl urlForPath(const QString &path) const;
    virtual QString pathFromUrl(const QUrl &url) const;

    /// 检查路径是否存在 (文件或目录)
    virtual bool exists(const QString &path) const = 0;

    /// 列出目录内容, 返回 FileEntry 列表 (不包含 . 和 ..)
    virtual QList<FileEntry> listDir(const QString &path) const = 0;

    /// 异步分批列出目录。默认实现下一事件循环调用同步 listDir()；
    /// 后端可覆写以提供真正的非阻塞读取。
    virtual QtFileMgrDirectoryRequest *listDirAsync(
        const QString &path, QObject *parent = nullptr) const;

    /// 判定路径是否为目录
    virtual bool isDir(const QString &path) const = 0;

    /// 判定路径是否可写入 (参照 KIO::isWritable)
    virtual bool isWritable(const QString &path) const = 0;

    /// 创建目录 (递归创建中间目录由实现决定)
    /// @return 成功返回 true
    virtual bool mkdir(const QString &path) = 0;

    /// 重命名/移动文件或目录 (参照 KIO::rename)
    /// @return 成功返回 true
    virtual bool rename(const QString &oldPath, const QString &newPath) = 0;

    /// Atomically refuse an existing destination. Implementations must not
    /// emulate this contract with exists()+rename(); unsupported backends fail.
    virtual bool renameNoReplace(const QString &source, const QString &destination,
                                 QString *error = nullptr);

    /// Opaque identity AND revision proof captured when a transfer completes.
    /// Empty means that a safe proof cannot be supplied. Custom backends may
    /// use immutable object/version IDs; default implementations refuse undo.
    /// A failed guarded action may return refreshedSnapshot ONLY after proving
    /// the same identity/content/tree survived its own reversible rename. This
    /// supports retry without accepting an externally modified object.
    /// A successful undoMoveIfUnchanged may return the restored source's exact
    /// proof after validating the same identity/content/tree across its rename.
    /// Callers must never attach that proof to unrelated earlier history.
    virtual QByteArray undoSnapshot(const QString &path, QString *error = nullptr) const;
    virtual bool undoCopyIfUnchanged(const QString &destination, const QByteArray &snapshot,
                                     QString *error = nullptr, QByteArray *refreshedSnapshot = nullptr);
    virtual bool undoMoveIfUnchanged(const QString &destination, const QString &source,
                                     const QByteArray &snapshot, QString *error = nullptr,
                                     QByteArray *refreshedSnapshot = nullptr);

    /// 删除文件或目录 (参照 KIO::del)
    /// @return 成功返回 true
    virtual bool remove(const QString &path) = 0;

    /// 永久删除 (不可恢复), 默认退化为 remove()
    /// NetworkIO 覆写: delMark=false 真正删除 vs remove() 的 delMark=true 标记删除
    virtual bool removePermanent(const QString &path);

    /// 将项目移动到后端回收站。旧远端后端默认兼容 remove() 的标记删除语义；
    /// 其他后端必须显式覆写并声明 TrashCapability。
    virtual bool moveToTrash(const QString &path, QString *error = nullptr);

    /// 复制文件 (参照 KIO::file_copy)
    /// @return 成功返回 true
    virtual bool copy(const QString &src, const QString &dest) = 0;

    /// 创建或完整写入一个文件。模板创建使用；默认后端明确返回不支持。
    virtual bool writeFile(const QString &path, const QByteArray &contents);

    /// 读取有大小上限的文本预览。返回原始字节，避免 IO 层猜测编码。
    virtual QByteArray readPreview(const QString &path, qint64 maxBytes,
                                   bool *truncated = nullptr,
                                   QString *error = nullptr) const;

    /// image=true requires a complete image, rejected when over maxBytes;
    /// text may be truncated. Legacy backends use a next-tick synchronous adapter.
    virtual QtFileMgrPreviewRequest *previewRequest(
        const QString &path, qint64 maxBytes, bool image,
        QObject *parent = nullptr) const;

    /// 后端是否支持写操作 (参照 KIO::supportsWriting)
    virtual bool canWrite() const = 0;

    // ══════════════════════════════════════════════════════════
    // [可选] 元数据查询 — 基类返回空/默认值
    // ══════════════════════════════════════════════════════════

    /// 获取单个路径的完整 FileEntry (stat 等价, 参照 KIO::stat)
    virtual FileEntry stat(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    /// 文件大小 (不查询其他元数据, 比 stat() 轻量)
    virtual qint64 fileSize(const QString &path) const {
        Q_UNUSED(path);
        return -1;
    }

    /// 最后修改时间
    virtual QDateTime modificationTime(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    /// MIME 类型 (参照 KIO::mimetype)
    virtual QString mimeType(const QString &path) const {
        Q_UNUSED(path);
        return QStringLiteral("application/octet-stream");
    }

    /// 文件权限 (参照 KIO::permissions)
    virtual QFile::Permissions permissions(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    /// 文件所有者 (参照 KIO::owner)
    virtual QString owner(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    /// 文件所属组 (参照 KIO::group)
    virtual QString group(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    /// 设置文件权限 (参照 KIO::chmod)
    virtual bool chmod(const QString &path, QFile::Permissions permissions) {
        Q_UNUSED(path);
        Q_UNUSED(permissions);
        return false;
    }

    // ══════════════════════════════════════════════════════════
    // [可选] 符号链接 — 基类返回空/false
    // ══════════════════════════════════════════════════════════

    /// 创建符号链接 (参照 KIO::symlink)
    virtual bool symlink(const QString &target, const QString &linkPath) {
        Q_UNUSED(target);
        Q_UNUSED(linkPath);
        return false;
    }

    /// 读取符号链接目标 (参照 KIO::readLink)
    virtual QString readLink(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    // ══════════════════════════════════════════════════════════
    // [可选] 缩略图/预览 — 基类返回空
    // ══════════════════════════════════════════════════════════

    /// 获取文件缩略图 (参照 KIO::PreviewJob)
    virtual QPixmap thumbnail(const QString &path, const QSize &size) {
        Q_UNUSED(path);
        Q_UNUSED(size);
        return {};
    }

    /// 文件图标 (参照 KIO::iconForUrl)
    virtual QIcon fileIcon(const QString &path) const {
        Q_UNUSED(path);
        return {};
    }

    // ══════════════════════════════════════════════════════════
    // [可选] 版本控制 — 基类返回空/false
    // ══════════════════════════════════════════════════════════

    /// 获取文件版本历史列表
    virtual QList<VersionInfo> versions(const QString &path) {
        Q_UNUSED(path);
        return {};
    }

    /// 回退文件到指定版本 (文件内容替换为该版本)
    virtual bool restoreVersion(const QString &path, int version) {
        Q_UNUSED(path);
        Q_UNUSED(version);
        return false;
    }

    using VersionsCallback = std::function<void(
        const QList<VersionInfo> &versions, const QString &error)>;
    using ResultCallback = std::function<void(
        bool success, const QString &error)>;

    /// 异步读取版本列表。默认下一事件循环调用同步 versions()。
    virtual void versionsAsync(const QString &path, QObject *ctx,
                               VersionsCallback callback);
    /// 异步恢复指定版本。默认下一事件循环调用同步 restoreVersion()。
    virtual void restoreVersionAsync(const QString &path, int version,
                                     QObject *ctx, ResultCallback callback);

    // ══════════════════════════════════════════════════════════
    // [可选] 异步批量操作 — 带去重和回调 (原 QtFileMgrFileOp 逻辑)
    // ══════════════════════════════════════════════════════════

    /// 单文件操作回调: success, newPath (去重后的目标路径)
    using OpCallback = std::function<void(bool success, const QString &newPath)>;

    /// 异步重命名: 远端后端覆写为 worker 线程执行,不得在调用线程
    /// 阻塞 (DLG-P1-5);默认实现经事件循环适配同步 rename。
    virtual void renameAsync(const QString &oldPath, const QString &newPath,
                             QObject *ctx, OpCallback cb);
    /// 批量操作回调: okCount, failCount, newNames (去重后的文件名列表)
    using BatchCallback = std::function<void(int okCount, int failCount, const QStringList &newNames)>;

    /// 异步复制单个文件 (带回调)
    virtual void copyAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb);
    /// 异步移动单个文件 (带回调)
    virtual void moveAsync(const QString &src, const QString &dest, QObject *ctx, OpCallback cb);
    /// 可取消、可报告字节进度的复制请求。默认适配 copyAsync()。
    virtual QtFileMgrTransferRequest *copyRequest(
        const QString &src, const QString &dest,
        QObject *parent = nullptr);
    /// 可取消、可报告字节进度的移动请求。默认适配 moveAsync()。
    virtual QtFileMgrTransferRequest *moveRequest(
        const QString &src, const QString &dest,
        QObject *parent = nullptr);
    /// 将后端文件/目录导出到本地绝对路径。仅声明 LocalExportCapability
    /// 的后端需要覆盖；默认实现返回一个明确失败的请求。
    virtual QtFileMgrTransferRequest *exportToLocalRequest(
        const QString &source, const QString &localDestination,
        QObject *parent = nullptr);
    /// 本机项目上传到远端目标。默认适配 copyRequest()（旧后端保持原行为）；
    /// 远端后端应覆写为显式上传，不得用本机路径存在性猜测方向 (审查#1)。
    virtual QtFileMgrTransferRequest *importFromLocalRequest(
        const QString &localSource, const QString &remoteDestination,
        bool move, QObject *parent = nullptr);
    /// 异步批量复制 — srcs → destDir, 自动去重
    virtual void batchCopyAsync(const QStringList &srcs, const QString &destDir, QObject *ctx, BatchCallback cb);
    /// 异步批量移动 — srcs → destDir, 自动去重
    virtual void batchMoveAsync(const QStringList &srcs, const QString &destDir, QObject *ctx, BatchCallback cb);

    /// 生成不冲突的目标文件名 (子类可用)
    static QString uniqueDestName(const QString &destDir, const QString &name, QtFileMgrIO *io);

    // ══════════════════════════════════════════════════════════
    // [能力] 查询后端特性
    // ══════════════════════════════════════════════════════════

    /// 是否支持版本管理
    virtual bool hasVersions() const { return false; }
    /// 是否支持将文件恢复到历史版本
    virtual bool supportsVersionRestore() const { return false; }
    /// 是否支持缩略图
    virtual bool hasThumbnails() const { return false; }
    /// 是否为远端后端 (影响 URL scheme 和路径解析)
    virtual bool isRemote() const { return false; }
    /// 是否支持符号链接
    virtual bool supportsSymlinks() const { return false; }
    /// 是否支持权限修改
    virtual bool supportsPermissions() const { return false; }
    /// 是否支持 MIME 类型识别
    virtual bool supportsMimeTypes() const { return false; }

    /// 汇总后端能力。旧后端无需覆盖；需要声明真正异步列目录、刷新通知或
    /// 字节进度时覆盖并在基类结果上增加对应 capability。
    virtual Capabilities capabilities() const;
    bool supportsCapability(Capability capability) const {
        return capabilities().testFlag(capability);
    }

signals:
    /// 后端数据变化通知。空列表表示整个后端可能变化；路径使用后端格式。
    void pathsChanged(const QStringList &paths);
    /// 后端连接/认证等可用状态变化。
    void backendStateChanged(bool available, const QString &message);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(QtFileMgrIO::Capabilities)

#endif
