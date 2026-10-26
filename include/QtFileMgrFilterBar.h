// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRFILTERBAR_H
#define QTFILEMGRFILTERBAR_H

#include <QWidget>
#include <QPalette>

class QLineEdit;
class QToolButton;
class QComboBox;

// 参考 Dolphin FilterBar (filterbar/filterbar.cpp)
class QtFileMgrFilterBar : public QWidget {
    Q_OBJECT
public:
    explicit QtFileMgrFilterBar(QWidget *parent = nullptr);

    QString text() const;
    bool isLocked() const;
    bool isCaseSensitive() const;
    void setCaseSensitive(bool on);

    enum FilterMode { PlainText, Glob, Regex };
    FilterMode filterMode() const;
    void setFilterMode(FilterMode mode);

signals:
    void filterChanged(const QString &text);
    void filterOptionsChanged();
    void closeRequest();
    void focusFileViewRequested();
    void navigationKeyRequested(int key, Qt::KeyboardModifiers modifiers);

public slots:
    void clear();
    void clearIfUnlocked();
    void show();
    void hide();
    void setVisible(bool visible) override;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onLockToggled(bool on);
    void updateInvalidPatternView();

private:
    bool handleEditorKey(QKeyEvent *event);
    void updateAppearance(QPalette appearancePalette);
    QLineEdit *m_edit;
    QToolButton *m_lockBtn;
    QToolButton *m_caseBtn;
    QToolButton *m_closeBtn = nullptr;
    QComboBox *m_modeCombo;
    QAction *m_invalidAction;
    // 无效正则红底的恢复基底;不能用 QPalette() 默认构造,
    // 那会丢掉应用级调色板(暗色主题等) (DLG-P2-15)
    QPalette m_editBasePalette;
};

#endif
