// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

//
#include "QtFileMgrItemDelegate.h"
#include "QtFileMgrFlatModel.h"
#include "QtFileMgrStyle.h"
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QFileInfo>
#include <QAbstractItemView>
#include <QDir>
#include <QIcon>
#include <QStringList>
#include <QTimer>

// No QFileSystemModel dependency — all data from model roles

QtFileMgrItemDelegate::QtFileMgrItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent), m_mode(IconsView), m_iconSize(48), m_gridSize(100, 90)
{ }

void QtFileMgrItemDelegate::setViewMode(ViewMode mode) { m_mode = mode; }
void QtFileMgrItemDelegate::setIconSize(int size)  { m_iconSize = size; }
void QtFileMgrItemDelegate::setGridSize(const QSize &s) { m_gridSize = s; }
void QtFileMgrItemDelegate::setFilterText(const QString &text) { m_filterText = text; }

static QRect itemIconRect(const QRect &rect, QtFileMgrItemDelegate::ViewMode mode,
                          int iconSize)
{
    if (mode == QtFileMgrItemDelegate::IconsView)
        return QRect(rect.center().x() - iconSize / 2, rect.top() + 12,
                     iconSize, iconSize);

    const int side = mode == QtFileMgrItemDelegate::DetailsView
        ? qMin(18, rect.height() - 10) : qMin(iconSize, rect.height() - 8);
    const int inset = mode == QtFileMgrItemDelegate::DetailsView ? 14 : 12;
    return QRect(rect.left() + inset, rect.top() + (rect.height() - side) / 2,
                 side, side);
}

static QRect itemTextRect(const QRect &rect, QtFileMgrItemDelegate::ViewMode mode,
                          int iconSize, const QFontMetrics &fm)
{
    const QRect iconRect = itemIconRect(rect, mode, iconSize);
    if (mode == QtFileMgrItemDelegate::IconsView)
        return QRect(rect.left() + 10, iconRect.bottom() + 9,
                     rect.width() - 20, fm.lineSpacing() * 2);

    const int rightInset = mode == QtFileMgrItemDelegate::DetailsView ? 14 : 12;
    return QRect(iconRect.right() + 11, rect.top(),
                 rect.right() - rightInset - iconRect.right() - 10, rect.height());
}

// ---------- 低饱和选择背景，保持文件名和元数据的阅读层次 ----------
static void drawSelectionBg(QPainter *p, const QStyleOptionViewItem &opt,
                             bool selected, bool hovered, bool focused, bool isDetail)
{
    const auto colors = QtFileMgrStyle::colors(opt.palette);
    if (selected || hovered) {
        p->save();
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? colors.selection : colors.hover);
        if (isDetail)
            p->drawRect(opt.rect);
        else
            p->drawRoundedRect(QRectF(opt.rect).adjusted(3, 3, -3, -3), 8, 8);
        p->restore();
    }
    if (selected && !isDetail) {
        QColor outline = colors.accent;
        outline.setAlphaF(focused ? 0.65 : 0.30);
        QPen pen(outline); pen.setWidthF(1);
        p->save(); p->setPen(pen); p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(opt.rect).adjusted(3.5, 3.5, -3.5, -3.5), 8, 8);
        p->restore();
    }
}

static int textWidth(const QFontMetrics &fm, const QString &text)
{
    return fm.boundingRect(text).width();
}

static QStringList iconViewLines(const QFontMetrics &fm, const QString &text, int width)
{
    const QString clean = text.simplified();
    if (clean.isEmpty())
        return {};
    if (textWidth(fm, clean) <= width)
        return QStringList() << clean;

    QString first;
    int split = 0;
    for (int i = clean.size(); i > 0; --i) {
        const QString candidate = clean.left(i);
        if (textWidth(fm, candidate) <= width) {
            first = candidate;
            split = i;
            break;
        }
    }
    if (first.isEmpty())
        return QStringList() << fm.elidedText(clean, Qt::ElideRight, width);

    // 文件名优先在标点处断行，扩展名作为完整的一段留在第二行。
    const int extensionStart = clean.lastIndexOf(QLatin1Char('.'));
    if (extensionStart > 0 && extensionStart <= split) {
        split = extensionStart;
    } else {
        for (int i = split - 1; i > 0; --i) {
            const QChar ch = clean.at(i);
            if (ch.isSpace() || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
                split = i + 1;
                break;
            }
            if (ch == QLatin1Char('.')) {
                split = i;
                break;
            }
        }
    }
    first = clean.left(split).trimmed();
    QString second = clean.mid(split).trimmed();
    if (second.isEmpty())
        return QStringList() << first;
    return QStringList() << first << fm.elidedText(second, Qt::ElideMiddle, width);
}

static QString siblingPathForName(const QString &oldPath, const QString &newName, bool remote)
{
    if (!remote)
        return QFileInfo(oldPath).dir().absoluteFilePath(newName);

    QString parent;
    const int slash = oldPath.lastIndexOf(QLatin1Char('/'));
    if (slash > 0)
        parent = oldPath.left(slash);
    else if (slash == 0)
        parent = QStringLiteral("/");

    if (parent.isEmpty() || parent == QStringLiteral("/"))
        return parent == QStringLiteral("/") ? parent + newName : newName;
    return parent + QLatin1Char('/') + newName;
}

void QtFileMgrItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    bool selected = option.state & QStyle::State_Selected;
    bool hovered  = option.state & QStyle::State_MouseOver;
    bool focused  = option.state & QStyle::State_HasFocus;
    const auto colors = QtFileMgrStyle::colors(option.palette);

    // 交替行背景（Detail 模式）
    if (m_mode == DetailsView && m_alternatingRows && (index.row() & 1)) {
        QColor alternate = colors.surface;
        alternate.setAlphaF(0.55);
        painter->fillRect(option.rect, alternate);
    }

    // Breeze 选择/悬停背景
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    drawSelectionBg(painter, option, selected, hovered, focused, m_mode == DetailsView);
    if (m_mode == DetailsView && selected && index.column() == 0)
        painter->fillRect(option.rect.left() + 1, option.rect.top() + 6,
                          2, option.rect.height() - 12, colors.accent);
    painter->restore();

    const QFontMetrics fm(option.font);
    const QColor textColor = m_mode == DetailsView && index.column() > 0 && !selected
        ? colors.muted : colors.text;
    const QString text = index.data(Qt::DisplayRole).toString();
    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));

    painter->save();
    painter->setFont(option.font);
    painter->setPen(textColor);

    if (m_mode == IconsView) {
        const QRect iconRect = itemIconRect(option.rect, m_mode, m_iconSize);
        if (!icon.isNull())
            icon.paint(painter, iconRect, Qt::AlignCenter);

        const QRect textRect = itemTextRect(option.rect, m_mode, m_iconSize, fm);
        const QStringList lines = iconViewLines(fm, text, textRect.width());
        for (int i = 0; i < lines.size(); ++i) {
            const QRect lineRect(textRect.left(), textRect.top() + i * fm.lineSpacing(),
                                 textRect.width(), fm.height());
            painter->drawText(lineRect, Qt::AlignCenter, lines.at(i));
        }
    } else if (m_mode == CompactView) {
        const QRect iconRect = itemIconRect(option.rect, m_mode, m_iconSize);
        if (!icon.isNull())
            icon.paint(painter, iconRect, Qt::AlignCenter);
        const QRect textRect = itemTextRect(option.rect, m_mode, m_iconSize, fm);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          fm.elidedText(text, Qt::ElideRight, textRect.width()));
    } else {
        const QRect r = option.rect.adjusted(14, 0, -14, 0);
        if (index.column() == 0) {
            const QRect iconRect = itemIconRect(option.rect, m_mode, m_iconSize);
            if (!icon.isNull())
                icon.paint(painter, iconRect, Qt::AlignCenter);
            const QRect textRect = itemTextRect(option.rect, m_mode, m_iconSize, fm);
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                              fm.elidedText(text, Qt::ElideRight, textRect.width()));
        } else {
            const int align = index.data(Qt::TextAlignmentRole).toInt();
            painter->drawText(r, align ? align : int(Qt::AlignVCenter | Qt::AlignLeft),
                              fm.elidedText(text, Qt::ElideRight, r.width()));
        }
    }

    painter->restore();
}

QSize QtFileMgrItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &idx) const
{
    if (m_mode == IconsView) return m_gridSize;
    if (m_mode == CompactView) return m_gridSize;  // 使用 updateZoom 设置的网格大小
    // 与 FileView 的固定行高一致，不受平台样式默认最小行高影响。
    QSize s = QStyledItemDelegate::sizeHint(option, idx);
    s.setHeight(qMax(30, option.fontMetrics.height() + 10));
    return s;
}

// ---------- 重命名: 数据全从 model roles 取, 文件操作委托给 model::setData ----------
QWidget *QtFileMgrItemDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                              const QModelIndex &) const
{
    QLineEdit *ed = new QLineEdit(parent);
    ed->setFrame(true);
    ed->setFont(option.font);
    ed->setAlignment(m_mode == IconsView ? Qt::AlignCenter : Qt::AlignLeft);
    const auto colors = QtFileMgrStyle::colors(option.palette);
    ed->setStyleSheet(QStringLiteral(
        "QLineEdit{background:%1;color:%2;border:1px solid %3;"
        "border-radius:4px;padding:2px 4px;selection-background-color:%3;"
        "selection-color:%4;}")
        .arg(colors.base.name(), colors.text.name(), colors.accent.name(),
             colors.onAccent.name()));
    return ed;
}

void QtFileMgrItemDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    QLineEdit *le = qobject_cast<QLineEdit *>(editor);
    if (!le) return;
    QString name = index.data(Qt::DisplayRole).toString();
    le->setText(name);
    // 文件选中名字不含后缀, 目录全选
    bool isDir = index.data(Qt::UserRole + 1).toBool();  // FlatModel 提供 isDir
    if (!isDir) {
        int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) le->setSelection(0, dot);
        else le->selectAll();
    } else {
        le->selectAll();
    }
}

void QtFileMgrItemDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                          const QModelIndex &index) const
{
    QLineEdit *le = qobject_cast<QLineEdit *>(editor);
    if (!le) return;
    QString newName = le->text().trimmed();
    QString oldName = index.data(Qt::DisplayRole).toString();
    if (newName.isEmpty() || newName == oldName) return;
    if (newName.contains(QLatin1Char('/'))) return;

    QString oldPath = index.data(Qt::UserRole).toString();

    // 通过 FlatModel 获取 IO 接口
    auto *fm = qobject_cast<QtFileMgrFlatModel *>(model);
    QtFileMgrIO *io = fm ? fm->io() : nullptr;
    QString newPath = siblingPathForName(oldPath, newName, io && io->isRemote());

    if (io && io->exists(newPath)) {
        le->setStyleSheet(QStringLiteral("QLineEdit{color:red}"));
        return;
    }

    // QFileInfo 检查仅对本地路径有效 (远端路径无意义)
    if (!io || !io->isRemote()) {
        QFileInfo oldFi(oldPath);
        if (oldFi.isHidden() && !newName.startsWith(QLatin1Char('.'))) {
            if (QMessageBox::question(nullptr, QStringLiteral("重命名"),
                    QStringLiteral("文件 \"%1\" 是隐藏文件。\n确认要显示它吗?").arg(oldName))
                != QMessageBox::Yes)
                return;
        }
        if (oldFi.isFile() && oldFi.suffix() != QFileInfo(newPath).suffix()) {
            if (QMessageBox::question(nullptr, QStringLiteral("重命名"),
                    QStringLiteral("要将扩展名从 \".%1\" 改为 \".%2\" 吗?\n文件可能无法使用。")
                    .arg(oldFi.suffix(), QFileInfo(newPath).suffix()))
                != QMessageBox::Yes)
                return;
        }
    }

    if (io) {
        // 异步重命名: 远端后端不再同步阻塞 UI (DLG-P1-5);
        // 失败经 operationFailed 信号由 FileView 消息条呈现
        const QString renamedDirectory = fm->url();
        const QPointer<QtFileMgrFlatModel> guardedModel(fm);
        const QPointer<const QtFileMgrItemDelegate> guardedSelf(this);
        io->renameAsync(oldPath, newPath, fm,
                        [guardedModel, guardedSelf, renamedDirectory, oldName,
                         newName, oldPath, newPath](bool ok, const QString &) {
            if (ok) {
                if (guardedModel) {
                    if (guardedModel->url() != renamedDirectory) {
                        qWarning("[ItemDelegate] rename refresh: skipped stale directory (renamedDirectory=\"%s\", currentDirectory=\"%s\")",
                                 qPrintable(renamedDirectory),
                                 qPrintable(guardedModel->url()));
                        return;
                    }
                    guardedModel->reload();
                }
            } else {
                qWarning("[ItemDelegate] rename failed (oldPath=\"%s\", newPath=\"%s\")",
                         qPrintable(oldPath), qPrintable(newPath));
                if (guardedSelf) {
                    emit guardedSelf->operationFailed(
                        QStringLiteral("无法将 \"%1\" 重命名为 \"%2\"。")
                            .arg(oldName, newName));
                }
            }
        });
        return;
    }

    // 无 IO 的本地回退 (直调模型测试等场景)
    const bool ok = QFile::rename(oldPath, newPath);
    if (ok) {
        const QString renamedDirectory = fm->url();
        QTimer::singleShot(0, fm, [fm, renamedDirectory]() {
            if (fm->url() != renamedDirectory) {
                qWarning("[ItemDelegate] rename refresh: skipped stale directory (renamedDirectory=\"%s\", currentDirectory=\"%s\")",
                         qPrintable(renamedDirectory),
                         qPrintable(fm->url()));
                return;
            }
            fm->reload();
        });
    } else {
        qWarning("[ItemDelegate] rename failed without backend (oldPath=\"%s\", newPath=\"%s\")",
                 qPrintable(oldPath), qPrintable(newPath));
        emit operationFailed(
            QStringLiteral("无法将 \"%1\" 重命名为 \"%2\"。").arg(oldName, newName));
    }
}

void QtFileMgrItemDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                                                  const QModelIndex &) const
{
    const QFontMetrics fm(option.font);
    const QRect textRect = itemTextRect(option.rect, m_mode, m_iconSize, fm);
    const int editorHeight = qMax(24, fm.height() + 8);
    if (m_mode == IconsView) {
        editor->setGeometry(textRect.left() - 4, textRect.top() - 4,
                            textRect.width() + 8, textRect.height() + 8);
    } else {
        editor->setGeometry(textRect.left() - 4,
                            option.rect.center().y() - editorHeight / 2,
                            textRect.width() + 8, editorHeight);
    }
}
