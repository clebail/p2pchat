#include "appstyle.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

QString AppStyle::styleSheet()
{
    return QStringLiteral(R"(
QMainWindow, QWidget#central, QWidget#home { background: #ffffff; }
QWidget#topBar { background: #ffffff; border-bottom: 1px solid #e1e3e1; }
QLabel#title { color: #1f1f1f; font-size: 17px; font-weight: 600; }
QLabel#subtitle { color: #5f6368; font-size: 12px; }
QToolButton#menuButton { border: none; border-radius: 20px; background: transparent; }
QToolButton#menuButton:hover { background: #e9eef6; }
QToolButton#menuButton::menu-indicator { image: none; }
QWidget#composerBar { background: #ffffff; }
QWidget#replyBar { background: #f0f4f9; border-left: 3px solid #0b57d0; border-radius: 6px; }
QLabel#replyLabel { color: #444746; font-size: 13px; }
QToolButton#replyClose { border: none; border-radius: 12px; color: #444746; font-size: 15px; }
QToolButton#replyClose:hover { background: #dde3ea; }
QPlainTextEdit#composer { background: #f0f4f9; color: #1f1f1f; border: none; border-radius: 24px;
                          padding: 10px 14px; font-size: 15px; }
QPlainTextEdit#composer:disabled { color: #9aa0a6; }
QToolButton#sendButton { background: #0b57d0; border: none; border-radius: 24px; }
QToolButton#sendButton:hover { background: #0842a0; }
QToolButton#sendButton:disabled { background: #c4c7c5; }
QToolButton#attachButton { border: none; border-radius: 24px; background: transparent; }
QToolButton#attachButton:hover { background: #e9eef6; }
QLabel#imagePreview { border-radius: 6px; }
QWidget#searchBar { background: #ffffff; border-bottom: 1px solid #e1e3e1; }
QLineEdit#searchEdit { background: #f0f4f9; border: none; border-radius: 16px; padding: 0 12px; min-height: 32px; }
QLabel#searchCount { color: #5f6368; padding: 0 6px; }
QToolButton#searchButton { border: none; border-radius: 16px; color: #444746; }
QToolButton#searchButton:hover { background: #e9eef6; }
QMenu { background: #f0f4f9; border: 1px solid #e1e3e1; border-radius: 8px; padding: 6px 0; }
QMenu::item { padding: 9px 28px 9px 20px; color: #1f1f1f; }
QMenu::item:selected { background: #dde3ea; }
QMenu::item:disabled { color: #9aa0a6; }
QMenu::item:disabled:selected { background: transparent; }
QMenu::separator { height: 1px; background: #e1e3e1; margin: 4px 0; }
QPushButton#primary { background: #0b57d0; color: white; border: none; border-radius: 20px;
                      padding: 10px 28px; font-weight: 600; }
QPushButton#primary:hover { background: #0842a0; }
QPushButton#tonal { background: #d3e3fd; color: #041e49; border: none; border-radius: 20px;
                    padding: 10px 28px; font-weight: 600; }
QPushButton#tonal:hover { background: #c2d7fb; }
QLabel#homeTitle { color: #1f1f1f; font-size: 22px; }
QLabel#homeText { color: #5f6368; font-size: 14px; }
QWidget#sidebar { background: #f0f4f9; border-right: 1px solid #e1e3e1; }
QLabel#sidebarTitle { color: #1f1f1f; font-size: 15px; font-weight: 600; }
QListWidget#convList { background: transparent; border: none; outline: none; }
QListWidget#convList::item { color: #1f1f1f; padding: 8px 10px; border-radius: 12px; margin: 1px 6px; }
QListWidget#convList::item:selected { background: #d3e3fd; color: #041e49; }
QListWidget#convList::item:hover:!selected { background: #e1e5ea; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #c4c7c5; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)");
}

// Icône « joindre » : trombone (Material « attach_file »), dessiné à 45°
QIcon AppStyle::attachIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(24, 24);
    p.rotate(45);
    p.setPen(QPen(QColor("#444746"), 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    // Trois branches emboîtées : extérieure, intérieure, centrale
    QPainterPath clip;
    clip.moveTo(6, 12);
    clip.lineTo(6, -12);
    clip.arcTo(QRectF(-6, -18, 12, 12), 0, 180);
    clip.lineTo(-6, 14);
    clip.arcTo(QRectF(-9, 8, 18, 14), 180, 180);
    clip.lineTo(9, -6);
    p.drawPath(clip);
    p.drawLine(QPointF(0, -10), QPointF(0, 10));
    return QIcon(pm);
}

// Icône « envoyer » (avion en papier Material) dessinée en blanc
QIcon AppStyle::sendIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.translate(1, 0);
    QPainterPath path;
    path.moveTo(2, 21);
    path.lineTo(23, 12);
    path.lineTo(2, 3);
    path.lineTo(2, 10);
    path.lineTo(17, 12);
    path.lineTo(2, 14);
    path.closeSubpath();
    p.fillPath(path, Qt::white);
    return QIcon(pm);
}

// Icône « ⋮ »
QIcon AppStyle::moreIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#444746"));
    for (int i = 0; i < 3; ++i)
        p.drawEllipse(QPointF(24, 12 + i * 12), 4, 4);
    return QIcon(pm);
}
