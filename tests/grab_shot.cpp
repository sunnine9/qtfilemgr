// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

// qtfilemgr 界面交互截图探针 (offscreen grab)。独立于 tests.pro, 显式构建运行。
//
// 用法: qtfilemgr_grab_shot [选项]
//   --url/--tab/--split/--switch/--size  布局与导航 (同旧版)
//   --wait <ms> 最终截图前额外等待 (默认 1500)
//   --out <path>  最终截图 (默认 /tmp/qtfilemgr-shot.png)
//   --actions "op:arg;op:arg;..."  交互脚本, 按序执行:
//     url:<url>            当前面板导航
//     tab:<url>            新开标签
//     split                当前标签分栏
//     switch:<n>           切到第 n 个标签
//     select:<名称[,...]>  按名称选中当前目录条目
//     filter:<文本>        设置过滤
//     view:icons|compact|details
//     zoom:<0-4>
//     key:<F2|Escape|Return|Delete|...>  向当前视图发按键
//     newfolder            新建文件夹(进入内联重命名)
//     open:settings|search|trash|props   打开对话框(非模态)
//     search:<关键字>      在搜索对话框中执行搜索
//     click:<按钮文本>     点击活动对话框中的按钮
//     close                关闭活动对话框
//     wait:<ms>            固定等待
//     wait:load            等当前面板模型加载完成 (超时 8s 告警继续)
//     shot:<path>          截取整个工作区
//     shotw:<path>         截取当前活动弹窗/模态框 (无则工作区)
//
// 每步间隔默认 500ms; 模态 exec() 会阻塞脚本, 对话框一律 show()。

#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSplitter>
#include <QTabWidget>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <memory>
#include "QtFileMgrConfigStore.h"
#include "QtFileMgrIO.h"
#include "QtFileMgrFileView.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrFilterBar.h"
#include "QtFileMgrSearchDialog.h"
#include "QtFileMgrSettingsDialog.h"
#include "QtFileMgrTrashDialog.h"
#include "QtFileMgrPropertiesDialog.h"
#include "QtFileMgrWidget.h"
#include "QtFileMgrWorkspace.h"

namespace {

struct Context {
    QtFileMgrWorkspace *workspace = nullptr;
    QPointer<QDialog> activeDialog;
    int stepDelay = 500;
};

QtFileMgrFileView *currentView(Context &ctx)
{
    QtFileMgrWidget *manager = ctx.workspace->currentFileManager();
    return manager ? manager->fileView() : nullptr;
}

void grabTo(const QString &path, QWidget *widget, const QString &tag)
{
    if (!widget) {
        qWarning("[SHOT] %s: no widget, skipped", qPrintable(tag));
        return;
    }
    const bool ok = widget->grab().save(path);
    qWarning("[SHOT] %s saved=%d path=\"%s\" size=%dx%d", qPrintable(tag),
             ok ? 1 : 0, qPrintable(path), widget->width(),
             widget->height());
}

void dispatchStep(Context &ctx, const QString &op, const QString &arg)
{
    QtFileMgrWorkspace *workspace = ctx.workspace;
    if (op == QStringLiteral("url")) {
        if (QtFileMgrWidget *manager = workspace->currentFileManager())
            manager->setUrl(QUrl::fromUserInput(arg));
    } else if (op == QStringLiteral("tab")) {
        workspace->addTab(QUrl::fromUserInput(arg));
    } else if (op == QStringLiteral("split")) {
        workspace->splitCurrentTab();
    } else if (op == QStringLiteral("switch")) {
        if (QTabWidget *tabs = workspace->findChild<QTabWidget *>())
            tabs->setCurrentIndex(arg.toInt());
    } else if (op == QStringLiteral("select")) {
        if (QtFileMgrFileView *view = currentView(ctx))
            view->selectNewFiles(arg.split(QLatin1Char(',')));
    } else if (op == QStringLiteral("filter")) {
        QtFileMgrWidget *manager = workspace->currentFileManager();
        QtFileMgrFilterBar *bar = manager ? manager->findChild<QtFileMgrFilterBar *>() : nullptr;
        QLineEdit *editor = bar ? bar->findChild<QLineEdit *>() : nullptr;
        if (!editor) {
            qWarning("[SHOT] filter skipped: active manager has no filter editor (filter=\"%s\")",
                     qPrintable(arg));
            return;
        }
        bar->show();
        editor->setText(arg);
    } else if (op == QStringLiteral("view")) {
        QtFileMgrFileView *view = currentView(ctx);
        if (!view)
            return;
        if (arg == QStringLiteral("icons"))
            view->setViewMode(QtFileMgrFileView::IconsView);
        else if (arg == QStringLiteral("compact"))
            view->setViewMode(QtFileMgrFileView::CompactView);
        else if (arg == QStringLiteral("details"))
            view->setViewMode(QtFileMgrFileView::DetailsView);
        else
            qWarning("[SHOT] unknown view mode \"%s\"", qPrintable(arg));
    } else if (op == QStringLiteral("zoom")) {
        if (QtFileMgrFileView *view = currentView(ctx))
            view->setZoomLevel(arg.toInt());
    } else if (op == QStringLiteral("key")) {
        QtFileMgrFileView *view = currentView(ctx);
        if (!view)
            return;
        const QKeySequence seq(arg);
        if (seq.isEmpty()) {
            qWarning("[SHOT] unknown key \"%s\"", qPrintable(arg));
            return;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int code = seq[0].toCombined();
#else
        const int code = seq[0];
#endif
        QKeyEvent press(QEvent::KeyPress, code & ~Qt::KeyboardModifierMask,
                        Qt::KeyboardModifiers(code & Qt::KeyboardModifierMask));
        QApplication::sendEvent(view, &press);
    } else if (op == QStringLiteral("newfolder")) {
        if (QtFileMgrFileView *view = currentView(ctx))
            view->createNewFolder();
    } else if (op == QStringLiteral("open")) {
        QtFileMgrWidget *manager = workspace->currentFileManager();
        if (!manager)
            return;
        QDialog *dialog = nullptr;
        if (arg == QStringLiteral("settings")) {
            dialog = new QtFileMgrSettingsDialog(
                QtFileMgrConfigStore::loadDefault(), workspace);
        } else if (arg == QStringLiteral("search")) {
            dialog = new QtFileMgrSearchDialog(
                manager->io(),
                manager->io() ? manager->io()->pathFromUrl(manager->url())
                              : QString(),
                workspace);
        } else if (arg == QStringLiteral("trash")) {
            dialog = new QtFileMgrTrashDialog(QStringList(), workspace);
        } else if (arg == QStringLiteral("props")) {
            QtFileMgrFileView *view = currentView(ctx);
            const QStringList selection = view ? view->selectedFiles()
                                               : QStringList();
            if (selection.isEmpty()) {
                qWarning("[SHOT] open:props skipped, no selection");
                return;
            }
            dialog = new QtFileMgrPropertiesDialog(manager->io(), selection,
                                                   workspace);
        } else {
            qWarning("[SHOT] unknown dialog \"%s\"", qPrintable(arg));
            return;
        }
        dialog->resize(720, 480);
        dialog->show();
        ctx.activeDialog = dialog;
    } else if (op == QStringLiteral("search")) {
        if (auto *dialog =
                qobject_cast<QtFileMgrSearchDialog *>(ctx.activeDialog)) {
            dialog->setSearchCriteria(arg, QtFileMgrSearchDialog::AnyType);
            dialog->startSearch();
        } else {
            qWarning("[SHOT] search step without an open search dialog");
        }
    } else if (op == QStringLiteral("click")) {
        if (!ctx.activeDialog) {
            qWarning("[SHOT] click step without an active dialog");
            return;
        }
        for (QAbstractButton *button :
             ctx.activeDialog->findChildren<QAbstractButton *>()) {
            if (button->text() == arg) {
                button->click();
                return;
            }
        }
        qWarning("[SHOT] click: no button \"%s\" in active dialog",
                 qPrintable(arg));
    } else if (op == QStringLiteral("close")) {
        if (ctx.activeDialog)
            ctx.activeDialog->close();
        ctx.activeDialog = nullptr;
    } else if (op == QStringLiteral("shot")) {
        grabTo(arg, workspace, QStringLiteral("workspace"));
    } else if (op == QStringLiteral("shotw")) {
        QWidget *target = QApplication::activeModalWidget();
        if (!target)
            target = QApplication::activePopupWidget();
        if (!target && ctx.activeDialog)
            target = ctx.activeDialog;
        if (!target)
            target = workspace;
        grabTo(arg, target, QStringLiteral("active-window"));
    } else if (!op.isEmpty()) {
        qWarning("[SHOT] unknown step op \"%s\"", qPrintable(op));
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCommandLineParser parser;
    const QCommandLineOption urlOpt(QStringLiteral("url"), QString(),
                                    QStringLiteral("url"));
    const QCommandLineOption tabOpt(QStringLiteral("tab"), QString(),
                                    QStringLiteral("url"));
    const QCommandLineOption splitOpt(QStringLiteral("split"));
    const QCommandLineOption switchOpt(QStringLiteral("switch"), QString(),
                                       QStringLiteral("index"));
    const QCommandLineOption waitOpt(QStringLiteral("wait"), QString(),
                                     QStringLiteral("ms"));
    const QCommandLineOption sizeOpt(QStringLiteral("size"), QString(),
                                     QStringLiteral("WxH"));
    const QCommandLineOption outOpt(QStringLiteral("out"), QString(),
                                    QStringLiteral("path"));
    const QCommandLineOption actionsOpt(QStringLiteral("actions"), QString(),
                                        QStringLiteral("script"));
    parser.addOptions({urlOpt, tabOpt, splitOpt, switchOpt, waitOpt, sizeOpt,
                       outOpt, actionsOpt});
    parser.process(app);

    QtFileMgrWorkspace workspace(nullptr, false);
    int width = 1200, height = 700;
    const QString sizeValue = parser.value(sizeOpt);
    if (!sizeValue.isEmpty()) {
        const int x = sizeValue.indexOf(QLatin1Char('x'));
        if (x > 0) {
            width = sizeValue.left(x).toInt();
            height = sizeValue.mid(x + 1).toInt();
        }
    }
    workspace.resize(width, height);
    workspace.show();

    // 步骤序列: 旧版选项转成等价动作排在前, --actions 排在后
    QStringList actionSpecs;
    if (parser.isSet(urlOpt))
        actionSpecs << QStringLiteral("url:") + parser.value(urlOpt);
    const QStringList tabs = parser.values(tabOpt);
    for (const QString &tab : tabs)
        actionSpecs << QStringLiteral("tab:") + tab;
    if (parser.isSet(splitOpt))
        actionSpecs << QStringLiteral("split");
    if (parser.isSet(switchOpt))
        actionSpecs << QStringLiteral("switch:") + parser.value(switchOpt);
    const QString scripted = parser.value(actionsOpt);
    if (!scripted.isEmpty())
        actionSpecs << scripted.split(QLatin1Char(';'));
    const int waitMs = parser.isSet(waitOpt) ? parser.value(waitOpt).toInt()
                                             : 1500;
    const QString out = parser.value(outOpt).isEmpty()
        ? QStringLiteral("/tmp/qtfilemgr-shot.png") : parser.value(outOpt);
    actionSpecs << QStringLiteral("wait:") + QString::number(waitMs);
    actionSpecs << QStringLiteral("shot:") + out;

    Context ctx;
    ctx.workspace = &workspace;

    const QStringList steps = actionSpecs;
    const std::shared_ptr<int> stepIndex(new int(0));
    const std::shared_ptr<std::function<void()>> runNext(
        new std::function<void()>);
    *runNext = [&app, &workspace, ctx, steps, stepIndex, runNext]() mutable {
        if (*stepIndex >= steps.size()) {
            app.quit();
            return;
        }
        const QString stepText = steps.at((*stepIndex)++).trimmed();
        const int colon = stepText.indexOf(QLatin1Char(':'));
        const QString op = colon < 0 ? stepText : stepText.left(colon);
        const QString arg = colon < 0 ? QString() : stepText.mid(colon + 1);
        if (op == QStringLiteral("wait")) {
            if (arg == QStringLiteral("load")) {
                // 条件等待: 轮询当前面板模型加载完成
                auto *poll = new QTimer(&workspace);
                poll->setInterval(50);
                auto *elapsed = new QElapsedTimer;
                elapsed->start();
                QObject::connect(poll, &QTimer::timeout, &workspace,
                        [&workspace, poll, elapsed, runNext, ctx]() mutable {
                    QtFileMgrFileView *view = nullptr;
                    if (QtFileMgrWidget *manager =
                            workspace.currentFileManager())
                        view = manager->fileView();
                    const bool loaded = view && !view->flatModel()->isLoading();
                    if (loaded || elapsed->elapsed() > 8000) {
                        if (!loaded)
                            qWarning("[SHOT] wait:load timed out after 8s");
                        poll->stop();
                        poll->deleteLater();
                        delete elapsed;
                        (*runNext)();
                    }
                });
                poll->start();
                return;
            }
            QTimer::singleShot(arg.isEmpty() ? 500 : arg.toInt(), &workspace,
                               [runNext]() { (*runNext)(); });
            return;
        }
        dispatchStep(ctx, op, arg);
        QTimer::singleShot(ctx.stepDelay, &workspace,
                           [runNext]() { (*runNext)(); });
    };
    QTimer::singleShot(800, &workspace, [runNext]() { (*runNext)(); });
    return app.exec();
}
