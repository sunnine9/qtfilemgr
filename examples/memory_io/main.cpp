// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// A virtual filesystem: the GUI never turns memory:/// URLs into local files.
#include "MemoryIO.h"
#include <QtFileMgrFileView.h>
#include <QtFileMgrPreviewPanel.h>
#include <QtFileMgrWidget.h>
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QtFileMgr Memory IO Example"));
    QCoreApplication::setOrganizationName(QStringLiteral("QtFileMgrExamples"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read-only memory:/// backend example"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("Check navigation, selection and preview, then exit")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save the demonstrated window and exit"), QStringLiteral("path")});
    parser.process(app);

    // Stack order is intentional: the injected backend outlives the window and
    // all its request consumers. QtFileMgrWidget does not take IO ownership.
    MemoryIO io;
    QMainWindow window;
    window.setWindowTitle(QStringLiteral("自定义 IO 示例 · 只读内存文件"));
    window.resize(1180, 720);
    auto *central = new QWidget(&window);
    auto *layout = new QVBoxLayout(central);
    auto *intro = new QLabel(QStringLiteral("memory:/// · 文件保存在内存中 · 可浏览和预览 · 写入由后端明确拒绝"), central);
    intro->setWordWrap(true);
    intro->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(intro);
    QtFileMgrConfig config;
    config.rootPath.clear(); // Local rootPath config is replaced by explicit IO URLs below.
    config.showDevices = false;
    config.defaultViewMode = 2;
    config.allowCreate = false;
    config.allowDelete = false;
    config.allowRename = false;
    config.allowDragDrop = false;
    config.toolbar.items.settingsButton = false;
    config.statusBar.showDiskSpace = false;
    config.placesPanel.width = 180;
    config.places = {
        {QStringLiteral("内存文件"), QStringLiteral("memory:///"), QStringLiteral("folder")},
        {QStringLiteral("文档"), QStringLiteral("memory:///documents"), QStringLiteral("folder-documents")}
    };
    auto *manager = new QtFileMgrWidget(config, &io, central);
    layout->addWidget(manager, 1);
    window.setCentralWidget(central);
    manager->setUrl(io.urlForPath(QStringLiteral("/documents")));
    window.show();
    // Size the preview after the main splitter has its actual visible width.
    QTimer::singleShot(0, manager, [manager]() { manager->showSidePanel(true); });

    // The same short tour drives screenshots and the behavioral smoke check.
    // No clipboard, external service or user files are involved.
    const bool smoke = parser.isSet(QStringLiteral("smoke-test"));
    const QString screenshot = parser.value(QStringLiteral("screenshot"));
    const bool automated = smoke || parser.isSet(QStringLiteral("screenshot"));
    if (parser.isSet(QStringLiteral("screenshot")) && screenshot.isEmpty()) {
        qWarning("[MemoryExample] screenshot: rejected empty output path");
        return 2;
    }
    int navigationSignals = 0;
    int selectionSignals = 0;
    QObject::connect(manager, &QtFileMgrWidget::urlChanged, &window,
                     [&navigationSignals](const QUrl &) { ++navigationSignals; });
    QObject::connect(manager, &QtFileMgrWidget::filesSelected, &window,
                     [&selectionSignals](const QStringList &) { ++selectionSignals; });
    QTimer tour;
    QElapsedTimer elapsed;
    elapsed.start();
    int step = 0;
    QObject::connect(&tour, &QTimer::timeout, &window, [&]() {
        if (elapsed.elapsed() > 8000) {
            tour.stop();
            qWarning("[MemoryExample] tour timeout: step=%d url=\"%s\" items=%d selected=%d",
                     step, qPrintable(manager->url().toString()), manager->fileView()->itemCount(),
                     manager->fileView()->selectedFiles().size());
            if (automated) app.exit(1);
            return;
        }
        if (step == 0 && manager->fileView()->itemCount() >= 2) {
            manager->fileView()->selectNewFiles({QStringLiteral("readme.txt")});
            step = 1;
        } else if (step == 1) {
            auto *preview = manager->findChild<QtFileMgrPreviewPanel *>();
            if (!preview || preview->previewMode() != QtFileMgrPreviewPanel::TextPreview)
                return;
            if (!automated) { tour.stop(); return; }
            if (manager->io() != &io || io.canWrite() || selectionSignals == 0
                || manager->fileView()->selectedFiles() != QStringList{QStringLiteral("/documents/readme.txt")}
                || preview->previewText() != QString::fromUtf8(io.readPreview(
                    QStringLiteral("/documents/readme.txt"), 128 * 1024))) {
                qWarning("[MemoryExample] smoke: selection/preview/read-only contract failed (url=\"%s\")",
                         qPrintable(manager->url().toString()));
                tour.stop(); app.exit(1); return;
            }
            manager->goUp();
            step = 2;
        } else if (step == 2 && manager->url() == io.urlForPath(io.rootPath())
                   && manager->fileView()->itemCount() > 0) {
            manager->setUrl(io.urlForPath(QStringLiteral("/documents")));
            step = 3;
        } else if (step == 3 && manager->fileView()->itemCount() >= 2) {
            manager->fileView()->selectNewFiles({QStringLiteral("readme.txt")});
            step = 4;
        } else if (step == 4) {
            auto *preview = manager->findChild<QtFileMgrPreviewPanel *>();
            if (!preview || preview->previewMode() != QtFileMgrPreviewPanel::TextPreview)
                return;
            tour.stop();
            if (navigationSignals < 2) {
                qWarning("[MemoryExample] smoke: missing host navigation signals (observed=%d)", navigationSignals);
                app.exit(1); return;
            }
            if (!screenshot.isEmpty() && !window.grab().save(screenshot)) {
                qWarning("[MemoryExample] screenshot: cannot save path=\"%s\"", qPrintable(screenshot));
                app.exit(1); return;
            }
            qInfo("RESULT: memory example navigation, selection and preview PASS");
            app.exit(0);
        }
    });
    tour.start(40);
    return app.exec();
}
