// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRPATHPICKER_H
#define QTFILEMGRPATHPICKER_H

#include <QDialog>
#include <QPointer>
#include <QString>

class QtFileMgrIO;
class QtFileMgrWidget;

/// 目录选择器 — 嵌入 QtFileMgrWidget, 可选择本地或远端目录
class QtFileMgrPathPicker : public QDialog {
    Q_OBJECT
public:
    explicit QtFileMgrPathPicker(QWidget *parent = nullptr);

    /// 打开选择器, 返回用户选择的目录路径 (取消返回空串)
    QString selectPath(QtFileMgrIO *io, const QString &startPath = QString(),
                       const QString &title = QString());

private:
    // 布局与按钮只建一次;文件管理器每次调用替换,旧实例 deleteLater (DLG-P1-1)
    QPointer<QtFileMgrWidget> m_fileMgr;
};

#endif
