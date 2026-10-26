// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include <QApplication>
#include <QMainWindow>

#include "QtFileMgrWorkspace.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qtfilemgr"));
#if QTFILEMGR_STANDALONE
    app.setOrganizationName(QStringLiteral("QtFileMgr"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
#endif

    // ---------- 方案1: 独立窗口 ----------
    QMainWindow mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("文件管理器"));
    mainWindow.resize(1100, 700);

    auto *workspace = new QtFileMgrWorkspace(&mainWindow);
    mainWindow.setCentralWidget(workspace);

    mainWindow.show();

    // ---------- 方案2: 嵌入已有界面 ----------
    // 把 QtFileMgrWidget 放到任何 layout 或 splitter 中即可:
    //
    //   QSplitter *splitter = new QSplitter(parent);
    //   QtFileMgrWidget *fileMgr = new QtFileMgrWidget(splitter);
    //   QWidget *otherPanel = new QWidget(splitter);
    //   splitter->addWidget(fileMgr);
    //   splitter->addWidget(otherPanel);

    return app.exec();
}
