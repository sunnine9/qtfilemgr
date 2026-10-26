// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include <QtFileMgrWidget.h>
#include <QtFileMgrIO.h>
#if QTFILEMGR_WITH_SSH
#include <QtFileMgrSSHIO.h>
#endif

#include <QApplication>
#include <QDir>
#include <QTimer>

#ifdef QTFILEMGR_EXPECT_SSH
static_assert(QTFILEMGR_WITH_SSH == QTFILEMGR_EXPECT_SSH,
              "Installed target must export the selected SSH configuration");
#endif

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QtFileMgrConfig config = QtFileMgrConfig::defaultConfig();
    if (config.places.isEmpty())
        return 3;
    config.rootPath = QDir::homePath();
    QtFileMgrWidget widget(config);
    if (!widget.url().isLocalFile() || !widget.io())
        return 1;
#if QTFILEMGR_WITH_SSH
    QtFileMgrSSHIO ssh;
    if (!ssh.supportsCapability(QtFileMgrIO::LocalExportCapability))
        return 2;
#endif
    widget.show();
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
