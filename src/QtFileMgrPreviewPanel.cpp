// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include "QtFileMgrPreviewPanel.h"

#include "QtFileMgrIO.h"

#include <QFileInfo>
#include <QBuffer>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStyle>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringDecoder>
#else
#include <QTextCodec>
#endif
#include <QVBoxLayout>

QtFileMgrPreviewPanel::QtFileMgrPreviewPanel(QWidget *parent)
    : QWidget(parent)
    , m_stack(new QStackedWidget(this))
    , m_placeholder(new QLabel(this))
    , m_image(new QLabel(this))
    , m_text(new QPlainTextEdit(this))
    , m_name(new QLabel(this))
    , m_details(new QLabel(this))
{
    setMinimumWidth(220);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_placeholder->setObjectName(QStringLiteral("previewPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_image->setObjectName(QStringLiteral("previewImage"));
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(180, 180);
    m_text->setObjectName(QStringLiteral("previewText"));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setMinimumHeight(180);
    QFont textFont = m_text->font();
    textFont.setFamily(QStringLiteral("monospace"));
    textFont.setStyleHint(QFont::TypeWriter);
    m_text->setFont(textFont);
    m_stack->addWidget(m_placeholder);
    m_stack->addWidget(m_image);
    m_stack->addWidget(m_text);

    m_name->setObjectName(QStringLiteral("previewName"));
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setWordWrap(true);
    QFont nameFont = m_name->font();
    nameFont.setBold(true);
    m_name->setFont(nameFont);
    m_details->setObjectName(QStringLiteral("previewDetails"));
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setWordWrap(true);

    layout->addWidget(m_stack, 1);
    layout->addWidget(m_name);
    layout->addWidget(m_details);
    clear();
}

void QtFileMgrPreviewPanel::setIO(QtFileMgrIO *io)
{
    clear();
    m_io = io;
}

void QtFileMgrPreviewPanel::cancelPreview()
{
    ++m_selectionGeneration;
    if (m_request) {
        qWarning("[PreviewPanel] selection: cancelled outstanding preview (request=%p)",
                 static_cast<void *>(m_request.data()));
        m_request->cancel();
        m_request = nullptr;
    }
}

QString QtFileMgrPreviewPanel::previewText() const
{
    return m_text->toPlainText();
}

bool QtFileMgrPreviewPanel::isTextMimeType(const QString &mimeType) const
{
    return mimeType.startsWith(QStringLiteral("text/"))
        || mimeType == QStringLiteral("application/json")
        || mimeType.endsWith(QStringLiteral("+json"))
        || mimeType == QStringLiteral("application/xml")
        || mimeType.endsWith(QStringLiteral("+xml"))
        || mimeType == QStringLiteral("application/javascript")
        || mimeType == QStringLiteral("application/x-shellscript");
}

void QtFileMgrPreviewPanel::setFiles(const QStringList &paths)
{
    cancelPreview();
    if (!m_io || paths.isEmpty()) {
        if (!m_io && !paths.isEmpty()) {
            qWarning("[PreviewPanel] selection: cannot preview %d paths without IO",
                     paths.size());
        }
        clear();
        return;
    }
    if (paths.size() == 1)
        showSingleFile(paths.first());
    else
        showMultipleFiles(paths);
}

void QtFileMgrPreviewPanel::showSingleFile(const QString &path)
{
    m_mode = EmptyPreview;
    m_sourcePixmap = QPixmap();
    m_image->setPixmap(QPixmap());
    m_text->clear();
    const FileEntry entry = m_io->stat(path);
    if (entry.path.isEmpty()) {
        qWarning("[PreviewPanel] stat: backend '%s' returned incomplete metadata for '%s'",
                 m_io->metaObject()->className(), qPrintable(path));
    }
    const QString mimeType = entry.isDir
        ? QStringLiteral("inode/directory") : m_io->mimeType(path);
    m_name->setText(entry.name.isEmpty() ? m_io->displayName(path) : entry.name);

    QStringList details;
    details << (entry.isDir ? QStringLiteral("文件夹") : mimeType);
    if (!entry.isDir && entry.size >= 0)
        details << formatFileSize(entry.size);
    if (entry.modified.isValid())
        details << QLocale::system().toString(entry.modified, QLocale::ShortFormat);

    QPixmap pixmap = m_io->fileIcon(path).pixmap(128, 128);
    if (pixmap.isNull()) {
        qWarning("[PreviewPanel] icon: backend returned no icon for '%s'; using style fallback",
                 qPrintable(path));
        pixmap = style()->standardIcon(entry.isDir ? QStyle::SP_DirIcon
                                                  : QStyle::SP_FileIcon).pixmap(128, 128);
    }
    showPixmap(pixmap, IconPreview);

    if (entry.isSymlink && !m_io->isRemote()) {
        const QString target = m_io->readLink(path);
        if (target.isEmpty())
            qWarning("[PreviewPanel] symlink: empty target for '%s'", qPrintable(path));
        else
            details << QStringLiteral("链接到: %1").arg(target);
    }
    details << path;
    m_details->setText(details.join(QLatin1Char('\n')));

    const bool text = !entry.isDir && isTextMimeType(mimeType);
    const bool image = !entry.isDir && mimeType.startsWith(QStringLiteral("image/"));
    if (!text && !image)
        return;
    m_placeholder->setText(QStringLiteral("正在加载预览…"));
    m_stack->setCurrentWidget(m_placeholder);
    m_mode = EmptyPreview;
    m_details->setText(m_details->text() + QStringLiteral("\n正在加载预览…"));
    const quint64 generation = m_selectionGeneration;
    QtFileMgrPreviewRequest *request = m_io->previewRequest(
        path, image ? 4 * 1024 * 1024 : 128 * 1024, image, this);
    if (!request) {
        qWarning("[PreviewPanel] request: null request (path=\"%s\", backend=%s)",
                 qPrintable(path), m_io->metaObject()->className());
        m_details->setText(details.join(QLatin1Char('\n')) + QStringLiteral("\n预览不可用"));
        showPixmap(pixmap, IconPreview);
        return;
    }
    m_request = request;
    const auto handleResult =
            [this, request, generation, path, image, details, pixmap]
            (const QByteArray &data, bool truncated, const QString &error) {
        if (generation != m_selectionGeneration || request != m_request) {
            qWarning("[PreviewPanel] result: ignored stale selection (path=\"%s\", generation=%llu, current=%llu)",
                     qPrintable(path), static_cast<unsigned long long>(generation),
                     static_cast<unsigned long long>(m_selectionGeneration));
            return;
        }
        m_request = nullptr;
        QStringList resultDetails = details;
        if (!error.isEmpty()) {
            qWarning("[PreviewPanel] result: preview failed (path=\"%s\", error=\"%s\")",
                     qPrintable(path), qPrintable(error));
            resultDetails << QStringLiteral("预览失败: %1").arg(error);
            showPixmap(pixmap, IconPreview);
        } else if (!image) {
            if (data.contains('\0')) {
                qWarning("[PreviewPanel] text: rejected binary data (path=\"%s\", bytes=%d)",
                         qPrintable(path), data.size());
                resultDetails << QStringLiteral("预览失败: 文件包含二进制数据");
                showPixmap(pixmap, IconPreview);
            } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QStringDecoder decoder(QStringConverter::encodingForData(data)
                                           .value_or(QStringConverter::System));
                const QString decoded = decoder(data);
                if (decoder.hasError()) {
                    qWarning("[PreviewPanel] text: decoding replaced invalid bytes (path=\"%s\", bytes=%lld)",
                             qPrintable(path), static_cast<long long>(data.size()));
                }
                m_text->setPlainText(decoded);
#else
                QTextCodec *codec = QTextCodec::codecForUtfText(data, QTextCodec::codecForLocale());
                m_text->setPlainText(codec->toUnicode(data));
#endif
                m_stack->setCurrentWidget(m_text);
                m_mode = TextPreview;
                if (truncated) resultDetails << QStringLiteral("预览已截断为 128 KB");
            }
        } else {
            QBuffer buffer;
            buffer.setData(data);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const QSize dimensions = reader.size();
            if (truncated || !dimensions.isValid() || dimensions.width() > 32768
                || dimensions.height() > 32768) {
                qWarning("[PreviewPanel] image: rejected dimensions/truncation (path=\"%s\", width=%d, height=%d, truncated=%d)",
                         qPrintable(path), dimensions.width(), dimensions.height(), truncated ? 1 : 0);
                resultDetails << QStringLiteral("预览失败: 图片无效或过大");
                showPixmap(pixmap, IconPreview);
            } else {
                reader.setScaledSize(dimensions.scaled(QSize(512, 512), Qt::KeepAspectRatio));
                const QImage decoded = reader.read();
                if (decoded.isNull()) {
                    qWarning("[PreviewPanel] image: decode failed (path=\"%s\", error=\"%s\")",
                             qPrintable(path), qPrintable(reader.errorString()));
                    resultDetails << QStringLiteral("预览失败: 图片解码失败");
                    showPixmap(pixmap, IconPreview);
                } else {
                    showPixmap(QPixmap::fromImage(decoded), ImagePreview);
                    resultDetails << QStringLiteral("%1 x %2 像素")
                        .arg(dimensions.width()).arg(dimensions.height());
                }
            }
        }
        m_details->setText(resultDetails.join(QLatin1Char('\n')));
    };
    connect(request, &QtFileMgrPreviewRequest::finished, this, handleResult);
    if (request->isFinished()) {
        // 同步后端在连接前已完成:信号已丢失,直接消费缓存结果 (DLG-P1-3)
        handleResult(request->resultBytes(), request->resultTruncated(),
                     request->errorString());
    }
}

void QtFileMgrPreviewPanel::showMultipleFiles(const QStringList &paths)
{
    int directories = 0;
    int files = 0;
    qint64 totalSize = 0;
    for (const QString &path : paths) {
        const FileEntry entry = m_io->stat(path);
        if (entry.isDir) {
            ++directories;
        } else {
            ++files;
            if (entry.size >= 0)
                totalSize += entry.size;
            else
                qWarning("[PreviewPanel] multi: skipped unknown size for '%s'",
                         qPrintable(path));
        }
    }
    m_mode = EmptyPreview;
    m_sourcePixmap = QPixmap();
    m_image->setPixmap(QPixmap());
    m_text->clear();
    m_placeholder->setText(QStringLiteral("已选择 %1 个项目").arg(paths.size()));
    m_stack->setCurrentWidget(m_placeholder);
    m_name->setText(QStringLiteral("%1 个项目").arg(paths.size()));
    QStringList details;
    details << QStringLiteral("%1 个文件，%2 个文件夹")
                   .arg(files).arg(directories);
    if (files > 0)
        details << QStringLiteral("文件合计: %1").arg(formatFileSize(totalSize));
    m_details->setText(details.join(QLatin1Char('\n')));
}

void QtFileMgrPreviewPanel::showPixmap(
    const QPixmap &pixmap, PreviewMode mode)
{
    m_sourcePixmap = pixmap;
    m_mode = mode;
    m_stack->setCurrentWidget(m_image);
    updatePixmapGeometry();
}

void QtFileMgrPreviewPanel::updatePixmapGeometry()
{
    if (m_sourcePixmap.isNull()) {
        m_image->setPixmap(QPixmap());
        return;
    }
    const QSize available = m_image->size() - QSize(8, 8);
    m_image->setPixmap(m_sourcePixmap.scaled(
        available.boundedTo(QSize(512, 512)), Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
}

void QtFileMgrPreviewPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_mode == ImagePreview || m_mode == IconPreview)
        updatePixmapGeometry();
}

void QtFileMgrPreviewPanel::clear()
{
    cancelPreview();
    m_mode = EmptyPreview;
    m_sourcePixmap = QPixmap();
    m_image->setPixmap(QPixmap());
    m_text->clear();
    m_placeholder->setText(QStringLiteral("未选择项目"));
    m_stack->setCurrentWidget(m_placeholder);
    m_name->clear();
    m_details->clear();
}
