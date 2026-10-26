// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRDEBUG_H
#define QTFILEMGRDEBUG_H

#include <QDebug>

// ============================================================
// QtFileMgr DEBUG 宏体系
// 默认关闭, 编译时 -DQTMGR_DEBUG=1 开启调试输出
// ============================================================

#ifndef QTMGR_DEBUG
#define QTMGR_DEBUG 0
#endif

#if QTMGR_DEBUG
#define QTMGR_DBG(fmt, ...)  qDebug("[QtMgr] " fmt, ##__VA_ARGS__)
#define QTMGR_WARN(fmt, ...) qWarning("[QtMgr] " fmt, ##__VA_ARGS__)
#else
#define QTMGR_DBG(fmt, ...)  ((void)0)
#define QTMGR_WARN(fmt, ...) ((void)0)
#endif

// 始终输出的错误宏 (不受 QTMGR_DEBUG 控制)
#define QTMGR_ERR(fmt, ...)  qCritical("[QtMgr] " fmt, ##__VA_ARGS__)

#endif // QTFILEMGRDEBUG_H
