// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRBREADCRUMBBAR_H
#define QTFILEMGRBREADCRUMBBAR_H

#include <QWidget>
#include <QUrl>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QToolButton;
class QCompleter;
class QStandardItemModel;
class QResizeEvent;
class QPalette;

class QtFileMgrIO;

class QtFileMgrBreadcrumbBar : public QWidget {
    Q_OBJECT
public:
    explicit QtFileMgrBreadcrumbBar(QWidget *parent = nullptr);
    ~QtFileMgrBreadcrumbBar() override;

    QUrl url() const;
    void setUrl(const QUrl &url);
    void setUrl(const QUrl &url, bool addToHistory);
    void setPlaceholderText(const QString &text);
    void setDisplayRoot(const QString &path);
    void setPlaceInfo(const QString &name, const QString &rootPath);
    void setEditableMode(bool editable);
    void setNavButtonsVisible(bool visible);
    void setNavigationState(bool canGoBack, bool canGoForward);
    void setIO(QtFileMgrIO *io);
    QString text() const;

signals:
    void urlChanged(const QUrl &url);
    void backRequested();
    void forwardRequested();
    void statusMessage(const QString &msg, const QString &type);
    void focusFileViewRequested();

public slots:
    void goBack();
    void goForward();
    void goUp();
    void goHome();

private slots:
    void onSegmentClicked();
    void onEditReturnPressed();
    void onEditTextEdited(const QString &text);

private:
    void rebuildBreadcrumb();
    void updateAppearance(QPalette appearancePalette);
    void clearBreadcrumbs();
    void updateBreadcrumbVisibility();
    void navigateToUrl(const QUrl &url);
    void navigateToStoredPath(const QString &path);
    void requestCompletion(const QString &text);
    QString completionDirectory(const QString &text) const;
    void updateReadOnlyBadge();
    void updateDisplayRootForUrl();
    QString currentComparablePath() const;
    QString displayTextForUrl(const QUrl &url) const;
    QString normalizedDisplayRoot(const QString &path) const;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    QUrl m_url;
    bool m_editable;
    QHBoxLayout *m_layout;
    QLineEdit *m_edit;
    QToolButton *m_backBtn;
    QToolButton *m_forwardBtn;
    QToolButton *m_upBtn = nullptr;
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_completionModel = nullptr;
    QLabel *m_readOnlyBadge = nullptr;
    QList<QWidget *> m_breadcrumbWidgets;
    QList<QToolButton *> m_segmentButtons;
    QList<QLabel *> m_segmentChevrons;
    QToolButton *m_overflowButton = nullptr;
    QString m_displayRoot;
    QString m_placeName;      // Place 显示名称 (非空时显示在面包屑第一段)
    QString m_completionRequestText;
    QString m_completionCacheKey;
    QtFileMgrIO *m_io = nullptr;
};

#endif
