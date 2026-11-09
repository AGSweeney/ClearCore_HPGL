/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// Colours and style sheets shared with the ClearHPGL look (dark slate, blue primary, cyan trace).

#include <QColor>
#include <QString>

class QPalette;
class QWidget;

namespace theme {

// Plot canvas (cam view, displacement diagram, timing view): near black, a touch of blue.
inline const QColor kCanvas{0x12, 0x14, 0x18};
inline const QColor kCanvasAlt{0x17, 0x1a, 0x1f};  // alternate segment band
inline const QColor kGrid{0x24, 0x29, 0x30};
inline const QColor kGridMajor{0x31, 0x38, 0x41};
inline const QColor kAxisText{0x9f, 0xa6, 0xaf};
inline const QColor kText{0xe6, 0xeb, 0xf3};
inline const QColor kSelectBand{0x2e, 0x53, 0x85, 70};
inline const QColor kSelect{0x5a, 0x8f, 0xd4};

// Curves
inline const QColor kDisplacement{0x35, 0xd9, 0xff};
inline const QColor kVelocity{0x7f, 0xe3, 0xa0};
inline const QColor kAccel{0xff, 0xcc, 0x80};
inline const QColor kJerk{0xc3, 0x9b, 0xff};
inline const QColor kWarn{0xff, 0x8a, 0x80};
inline const QColor kCursor{0xf0, 0xf3, 0xf7, 160};

// Cam drawing
inline const QColor kCamFill{0x3a, 0x3f, 0x46};
inline const QColor kCamEdge{0xb8, 0xc4, 0xd6};
inline const QColor kFollower{0xe6, 0xec, 0xf5};
inline const QColor kFollowerDark{0x8f, 0xa1, 0xb8};

// A colour per segment for the cam view's pitch-curve overlay.
QColor SegmentColor(int index);
// One color per output IO-0..IO-5 (bars, lane labels, lamps, the Outputs table), chosen apart from
// the displacement / velocity / acceleration / jerk curves.
QColor OutputColor(int output);

// Main-window sheet and the application-wide sheet for dialogs and message boxes.
QString MainSheet();
QString AppSheet();
// Start page and setup wizard: the main sheet plus their own pieces (step list, tiles, cards).
QString DialogSheet();
// The app's blue as the palette accent / highlight (Windows would use its own accent colour).
void ApplyAccent(QWidget *w);
QPalette AccentPalette(QPalette base);

}  // namespace theme
