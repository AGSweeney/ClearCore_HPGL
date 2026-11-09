/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "Theme.h"

#include <QPalette>
#include <QWidget>

namespace theme {

QColor OutputColor(int output) {
    static const QColor kColors[] = {
        {0xff, 0x6b, 0x6b},  // IO-0 coral red
        {0xff, 0xd4, 0x3b},  // IO-1 yellow
        {0x4d, 0xab, 0xf7},  // IO-2 blue
        {0xf0, 0x65, 0x95},  // IO-3 pink
        {0xff, 0x92, 0x2b},  // IO-4 orange
        {0x94, 0xd8, 0x2d},  // IO-5 lime
    };
    const int n = static_cast<int>(sizeof(kColors) / sizeof(kColors[0]));
    return kColors[((output % n) + n) % n];
}

QColor SegmentColor(int index) {
    static const QColor kColors[] = {
        QColor(0x35, 0xd9, 0xff), QColor(0x7f, 0xe3, 0xa0), QColor(0xff, 0xcc, 0x80),
        QColor(0xc3, 0x9b, 0xff), QColor(0xff, 0x8a, 0x80), QColor(0x80, 0xcb, 0xc4),
        QColor(0xf4, 0x8f, 0xb1), QColor(0xd4, 0xe1, 0x57),
    };
    return kColors[index % (sizeof kColors / sizeof kColors[0])];
}

// Check boxes and radio buttons in the app's blue (Windows would draw them in its accent colour).
static QString IndicatorSheet() {
    return QStringLiteral(
        "QRadioButton, QCheckBox { spacing: 8px; color: #e6ebf3; }"
        "QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; background: #282d34; border: 1px solid #5a6575; }"
        "QCheckBox::indicator { border-radius: 2px; }"
        "QRadioButton::indicator { border-radius: 8px; }"
        "QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: #6aa0e0; }"
        "QCheckBox::indicator:checked { background: #3d7bd0; border-color: #5a8fd4; image: url(:/icons/check.svg); }"
        "QRadioButton::indicator:checked { border-color: #5a8fd4; background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 #f2f6fb, stop:0.32 #f2f6fb, stop:0.42 #3d7bd0, stop:1 #3d7bd0); }"
        "QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: #242930; border-color: #3a414b; }");
}

QString MainSheet() {
    return IndicatorSheet() + QStringLiteral(
        "QMainWindow { background: #25292f; border: none; }"
        "QMenuBar { background: #32363c; color: #d6d9de; border: none; border-bottom: 1px solid #20242a; padding: 4px 8px; }"
        "QMenuBar::item { padding: 6px 14px; border-radius: 2px; }"
        "QMenuBar::item:selected { background: #444b54; color: #f0f2f5; }"
        "QMenu { background: #2f343a; color: #d8dce2; border: 1px solid #20242a; padding: 4px 0; }"
        "QMenu::item { padding: 8px 32px 8px 16px; }"
        "QMenu::item:selected { background: #49525e; color: #f1f4f8; }"
        "QMenu::item:disabled { color: #6f7884; }"
        "QMenu::separator { height: 1px; background: #20242a; margin: 4px 8px; }"
        "QWidget { color: #dde2e8; font-size: 12px; }"
        "QGroupBox { background: #2f343a; border: 1px solid #1a1e24; border-radius: 3px; margin-top: 20px; padding-top: 2px; font-weight: 600; color: #e6ebf3; }"
        "QGroupBox::title {"
        " subcontrol-origin: margin; subcontrol-position: top left; left: 14px; top: 2px; padding: 0 6px; margin-top: 0px;"
        " color: #c9d1dc; background: transparent; border: none; font-weight: 600; }"
        "QPushButton, QToolButton { background: #3a3f46; color: #dce1e8; border: 1px solid #23272d; border-radius: 2px; min-height: 26px; padding: 3px 12px; font-weight: 600; }"
        "QPushButton:hover, QToolButton:hover { border-color: #586473; background: #424850; }"
        "QPushButton:pressed, QToolButton:pressed { background: #30353b; border-color: #6d7a8c; }"
        "QPushButton:checked, QToolButton:checked { background: #274874; color: #eef5ff; border-color: #5a8fd4; }"
        "QPushButton:disabled, QToolButton:disabled { background: #2d3238; color: #7b8490; border-color: #252a31; }"
        "QPushButton[role=\"primary\"] { background: #274874; color: #eef5ff; border: 1px solid #5a8fd4; font-weight: 700; }"
        "QPushButton[role=\"primary\"]:hover { background: #2e5385; border-color: #6aa0e0; }"
        "QPushButton[role=\"primary\"]:pressed { background: #1f3d5f; border-color: #4a7ec4; }"
        // A disabled primary button must look disabled (it would otherwise keep its blue).
        "QPushButton[role=\"primary\"]:disabled { background: #2d3238; color: #7b8490; border-color: #252a31; font-weight: 600; }"
        "QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { background: #282d34; color: #e5e9ef; border: 1px solid #20242a; border-radius: 2px; min-height: 24px; padding: 2px 8px; selection-background-color: #5a82c2; }"
        "QLineEdit:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { background: #242930; color: #7b8490; border-color: #20242a; }"
        "QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border-color: #5a82c2; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: right center; border: none; width: 24px; background: #2d3239; border-left: 1px solid #20242a; border-top-right-radius: 2px; border-bottom-right-radius: 2px; }"
        "QComboBox::drop-down:hover { background: #353b44; }"
        "QComboBox::down-arrow { image: url(:/icons/combo-down.svg); width: 12px; height: 8px; border: none; }"
        "QComboBox:hover { background: #2c3138; border-color: #3d4650; }"
        "QComboBox QAbstractItemView { background: #282d34; color: #e5e9ef; border: 1px solid #1a1e24; selection-background-color: #3d5a8c; selection-color: #f0f4f8; outline: 0; padding: 2px; }"
        "QComboBox QAbstractItemView::item { min-height: 24px; padding: 4px 8px; }"
        // An item that can't be picked has to look it: the item rule above otherwise paints it like the rest.
        "QComboBox QAbstractItemView::item:disabled { color: #5d6570; }"
        "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 18px; background: #2d3239; border-left: 1px solid #20242a; }"
        "QAbstractSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 2px; }"
        "QAbstractSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 2px; }"
        "QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: #3a414c; }"
        "QAbstractSpinBox::up-arrow { image: url(:/icons/spin-up.svg); width: 9px; height: 6px; }"
        "QAbstractSpinBox::down-arrow { image: url(:/icons/spin-down.svg); width: 9px; height: 6px; }"
        "QTableWidget { background: #1f242b; alternate-background-color: #232931; color: #e6ebf3; gridline-color: #2c333c; border: 1px solid #20242a; border-radius: 2px; selection-background-color: #3d5a8c; selection-color: #f0f4f8; }"
        "QTableWidget::item { padding: 2px 4px; }"
        "QTableWidget QLineEdit, QTableWidget QDoubleSpinBox { min-height: 18px; padding: 0 4px; }"
        "QTableWidget QComboBox { min-height: 18px; padding: 0 6px; border: none; background: transparent; }"
        "QHeaderView::section { background: #2c3138; color: #aeb6c2; border: none; border-right: 1px solid #20242a; border-bottom: 1px solid #20242a; padding: 4px 6px; font-weight: 700; }"
        "QTableCornerButton::section { background: #2c3138; border: none; }"
        "QScrollBar:vertical { background: #2a2f36; width: 12px; }"
        "QScrollBar::handle:vertical { background: #4a5260; border-radius: 4px; min-height: 30px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }"
        "QScrollBar:horizontal { background: #2a2f36; height: 12px; }"
        "QScrollBar::handle:horizontal { background: #4a5260; border-radius: 4px; min-width: 30px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QSplitter { border: none; }"
        "QSplitter::handle:vertical { background: #20242a; height: 3px; }"
        "QSplitter::handle:vertical:hover { background: #3f4853; }"
        "QSplitter::handle:horizontal { background: #20242a; width: 3px; }"
        "QSplitter::handle:horizontal:hover { background: #3f4853; }"
        "QLabel#ECamStats { background: #1f242b; color: #e6ebf3; border: 1px solid #20242a; border-radius: 2px; padding: 6px 8px; font-family: Consolas, 'Courier New', monospace; }"
        "QFrame#ECamSep { color: #4a5563; }"
        "QStatusBar { background: #2c3036; color: #9fa6af; border-top: 1px solid #20242a; padding: 4px 14px; }"
        "QStatusBar::item { border: none; }");
}

QString AppSheet() {
    return IndicatorSheet() + QStringLiteral(
        "QMessageBox { background-color: #2a2f36; }"
        "QMessageBox QLabel { color: #f0f4f8; background: transparent; }"
        "QMessageBox QPushButton { background: #3d4450; color: #f2f6fb; border: 1px solid #5a6575;"
        " border-radius: 2px; min-width: 92px; min-height: 26px; padding: 4px 14px; }"
        "QMessageBox QPushButton:hover { background: #4c5565; }"
        "QMessageBox QPushButton:pressed { background: #353b46; }"
        "QDialog { background-color: #2a2f36; }"
        "QDialog QWidget { color: #e6ebf3; }"
        "QDialog QLineEdit, QDialog QComboBox, QDialog QSpinBox { background: #282d34; color: #e5e9ef;"
        " border: 1px solid #3d4650; border-radius: 2px; min-height: 24px; padding: 2px 8px; }"
        "QDialog QPushButton { background: #3d4450; color: #f2f6fb; border: 1px solid #5a6575;"
        " border-radius: 2px; min-width: 88px; min-height: 26px; padding: 4px 14px; }"
        "QDialog QPushButton:hover { background: #4c5565; }"
        "QToolTip { background: #2f343a; color: #e6ebf3; border: 1px solid #4b5563; padding: 4px; }");
}

QPalette AccentPalette(QPalette pal) {
    const QColor accent(0x5a, 0x8f, 0xd4);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, QColor(0xf0, 0xf4, 0xf8));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    pal.setColor(QPalette::Accent, accent);
#endif
    return pal;
}

void ApplyAccent(QWidget *w) { w->setPalette(AccentPalette(w->palette())); }

QString DialogSheet() {
    return MainSheet() + QStringLiteral(
        "QDialog { background: #2a2f36; }"
        "QWidget#ECamSide { background: #23272d; border-right: 1px solid #1a1e24; }"
        "QLabel#ECamStep { color: #8a929d; padding: 6px 10px; border-left: 3px solid transparent; }"
        "QLabel#ECamStep[current=\"true\"] { color: #eef5ff; font-weight: 700; border-left: 3px solid #5a8fd4; background: #2c3440; }"
        "QLabel#ECamStep[done=\"true\"] { color: #b8c2cf; }"
        "QLabel#ECamTitle { color: #f0f4f8; font-size: 20px; font-weight: 700; }"
        "QLabel#ECamHeading { color: #f0f4f8; font-size: 16px; font-weight: 700; }"
        "QLabel#ECamHelp { color: #9fa6af; }"
        "QLabel#ECamDerived { color: #7fc8e8; }"
        "QLabel#ECamWarn { color: #ffb07a; }"
        "QLabel#ECamCard { background: #1f242b; border: 1px solid #1a1e24; border-radius: 3px; padding: 10px 12px; }"
        "QListWidget { background: #1f242b; color: #e6ebf3; border: 1px solid #1a1e24; border-radius: 3px; outline: 0; padding: 4px; }"
        "QListWidget::item { padding: 6px 6px; border-radius: 2px; }"
        "QListWidget::item:hover { background: #2a313a; }"
        "QListWidget::item:selected { background: #2e4a70; color: #f0f4f8; }"
        "QPushButton#ECamTile { text-align: left; padding: 10px 14px; min-height: 44px; font-size: 13px; }"
        "QScrollArea { border: none; background: transparent; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }");
}

}  // namespace theme
