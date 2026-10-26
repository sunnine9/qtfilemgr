// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRPREVIEWPANEL_H
#define QTFILEMGRPREVIEWPANEL_H

#include <QPixmap>
#include <QPointer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QtFileMgrIO;
class QtFileMgrPreviewRequest;

class QtFileMgrPreviewPanel : public QWidget {
    Q_OBJECT
public:
    enum PreviewMode { EmptyPreview, IconPreview, ImagePreview, TextPreview };

    explicit QtFileMgrPreviewPanel(QWidget *parent = nullptr);

    void setIO(QtFileMgrIO *io);
    PreviewMode previewMode() const { return m_mode; }
    QString previewText() const;

public slots:
    void setFiles(const QStringList &paths);
    void clear();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void showPixmap(const QPixmap &pixmap, PreviewMode mode);
    void showSingleFile(const QString &path);
    void cancelPreview();
    void showMultipleFiles(const QStringList &paths);
    void updatePixmapGeometry();
    bool isTextMimeType(const QString &mimeType) const;

    QPointer<QtFileMgrIO> m_io;
    QPointer<QtFileMgrPreviewRequest> m_request;
    quint64 m_selectionGeneration = 0;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_placeholder = nullptr;
    QLabel *m_image = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_details = nullptr;
    QPixmap m_sourcePixmap;
    PreviewMode m_mode = EmptyPreview;
};

#endif
