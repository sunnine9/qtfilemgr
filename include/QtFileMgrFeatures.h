// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRFEATURES_H
#define QTFILEMGRFEATURES_H

// CMake exports the selected SSH feature value
// through QtFileMgr::QtFileMgr, including to installed-package consumers.
#ifndef QTFILEMGR_WITH_SSH
#define QTFILEMGR_WITH_SSH 1
#endif

#ifndef QTFILEMGR_STANDALONE
#define QTFILEMGR_STANDALONE 1
#endif

#endif
