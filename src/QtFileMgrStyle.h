// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#ifndef QTFILEMGRSTYLE_H
#define QTFILEMGRSTYLE_H

#include <QColor>
#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QWidget>
#include <QtMath>
#include <functional>
#include <utility>

// Internal visual primitives. Keep the host application's font and accent.
namespace QtFileMgrStyle {

inline QColor blend(const QColor &background, const QColor &foreground, qreal amount)
{
    return QColor::fromRgbF(
        background.redF() * (1.0 - amount) + foreground.redF() * amount,
        background.greenF() * (1.0 - amount) + foreground.greenF() * amount,
        background.blueF() * (1.0 - amount) + foreground.blueF() * amount);
}

struct Colors {
    QColor base, surface, border, muted, hover, selection, accent, text, onAccent;
};

inline Colors colors(const QPalette &palette)
{
    Colors c;
    c.base = palette.color(QPalette::Base);
    c.text = palette.color(QPalette::Text);
    c.accent = palette.color(QPalette::Highlight);
    c.onAccent = palette.color(QPalette::HighlightedText);
    c.surface = blend(c.base, c.text, 0.035);
    c.border = blend(c.base, c.text, 0.14);
    c.muted = blend(c.base, c.text, 0.64);
    c.hover = blend(c.base, c.accent, 0.07);
    c.selection = blend(c.base, c.accent, 0.16);
    return c;
}

// Qt5 QApplication::setPalette also uses ApplicationPaletteChange events;
// it does not consistently emit QGuiApplication::paletteChanged.
class PaletteObserver final : public QObject {
public:
    PaletteObserver(QWidget *owner, std::function<void(const QPalette &)> apply)
        : QObject(owner), m_owner(owner), m_palette(QApplication::palette(owner)),
          m_apply(std::move(apply))
    {
        qApp->installEventFilter(this);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QObject::connect(qApp, &QGuiApplication::paletteChanged, this,
                         [this](const QPalette &palette) { update(palette); });
#endif
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationPaletteChange)
            update(QApplication::palette(m_owner));
        return false;
    }

private:
    void update(const QPalette &palette)
    {
        if (palette == m_palette)
            return;
        m_palette = palette;
        m_apply(m_palette);
    }

    QWidget *m_owner;
    QPalette m_palette;
    std::function<void(const QPalette &)> m_apply;
};

inline void observeApplicationPalette(QWidget *owner,
                                      std::function<void(const QPalette &)> apply)
{
    new PaletteObserver(owner, std::move(apply));
}

enum class Symbol {
    Back, Forward, Up, Menu, Settings, AddTab, Split,
    CopyToOther, MoveToOther, Close, Search, Lock, Unlock
};

inline QPixmap symbolPixmap(Symbol symbol, const QColor &color, bool mirrored = false)
{
    QPixmap pixmap(48, 48);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    if (mirrored) {
        p.translate(24, 0);
        p.scale(-1, 1);
    }
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (symbol) {
    case Symbol::Back:
        p.drawLine(QPointF(14.5, 6.5), QPointF(9, 12));
        p.drawLine(QPointF(9, 12), QPointF(14.5, 17.5));
        break;
    case Symbol::Forward:
        p.drawLine(QPointF(9.5, 6.5), QPointF(15, 12));
        p.drawLine(QPointF(15, 12), QPointF(9.5, 17.5));
        break;
    case Symbol::Up:
        p.drawLine(QPointF(6.5, 11), QPointF(12, 5.5));
        p.drawLine(QPointF(12, 5.5), QPointF(17.5, 11));
        p.drawLine(QPointF(12, 6), QPointF(12, 18.5));
        break;
    case Symbol::Menu:
        for (int y : {7, 12, 17})
            p.drawLine(QPointF(5, y), QPointF(19, y));
        break;
    case Symbol::Settings:
        p.drawEllipse(QPointF(12, 12), 5, 5);
        p.drawEllipse(QPointF(12, 12), 1.8, 1.8);
        p.save();
        p.translate(12, 12);
        for (int i = 0; i < 8; ++i) {
            p.drawLine(QPointF(0, -6), QPointF(0, -8));
            p.rotate(45);
        }
        p.restore();
        break;
    case Symbol::AddTab:
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        break;
    case Symbol::Split:
        p.drawRoundedRect(QRectF(4, 5, 16, 14), 2, 2);
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        break;
    case Symbol::CopyToOther:
        p.drawRoundedRect(QRectF(4, 7, 8, 12), 1.2, 1.2);
        p.drawLine(QPointF(7, 4), QPointF(15, 4));
        p.drawLine(QPointF(15, 4), QPointF(15, 8));
        p.drawLine(QPointF(14, 13), QPointF(21, 13));
        p.drawLine(QPointF(18, 10), QPointF(21, 13));
        p.drawLine(QPointF(21, 13), QPointF(18, 16));
        break;
    case Symbol::MoveToOther:
        p.drawLine(QPointF(4, 12), QPointF(19, 12));
        p.drawLine(QPointF(14, 7), QPointF(19, 12));
        p.drawLine(QPointF(19, 12), QPointF(14, 17));
        p.drawLine(QPointF(4, 7), QPointF(4, 17));
        break;
    case Symbol::Close:
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
        break;
    case Symbol::Search:
        p.drawEllipse(QRectF(4.5, 4.5, 11, 11));
        p.drawLine(QPointF(14, 14), QPointF(19.5, 19.5));
        break;
    case Symbol::Lock:
    case Symbol::Unlock:
        p.drawRoundedRect(QRectF(6, 10, 12, 10), 2, 2);
        p.drawLine(QPointF(12, 14), QPointF(12, 16));
        if (symbol == Symbol::Lock) {
            p.drawArc(QRectF(8, 3.5, 8, 11), 0, 180 * 16);
            p.drawLine(QPointF(8, 9), QPointF(8, 10));
            p.drawLine(QPointF(16, 9), QPointF(16, 10));
        } else {
            p.drawArc(QRectF(8, 1.5, 10, 11), 0, 180 * 16);
            p.drawLine(QPointF(8, 7), QPointF(8, 10));
        }
        break;
    }
    return pixmap;
}

inline QIcon toolIcon(Symbol symbol, const QPalette &palette, bool mirrored = false)
{
    QIcon icon;
    icon.addPixmap(symbolPixmap(symbol, palette.color(QPalette::WindowText), mirrored), QIcon::Normal);
    icon.addPixmap(symbolPixmap(symbol, palette.color(QPalette::Disabled, QPalette::WindowText), mirrored), QIcon::Disabled);
    icon.addPixmap(symbolPixmap(symbol, palette.color(QPalette::Highlight), mirrored), QIcon::Normal, QIcon::On);
    return icon;
}

} // namespace QtFileMgrStyle

#endif
