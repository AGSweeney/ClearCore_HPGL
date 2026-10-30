#pragma once

// Preview of an HP-GL job using the firmware's own interpreter
// (Firmware/ClearCoreFirmware/Hpgl.cpp), so scaling, rotation, window clipping,
// hard-clip clamping, chorded arcs and PE decode exactly as the board will.

#include <QByteArray>
#include <QVector>
#include <QVector3D>

struct HpglPreviewResult {
    /// Pen-down runs in plotter mm (physical frame, origin = plotter 0,0).
    QVector<QVector<QVector3D>> cutStripsMm;
    /// Pen-up moves as flat start/end pairs (mm).
    QVector<QVector3D> rapidSegmentsMm;
    double drawLengthMm = 0.0;
    double travelLengthMm = 0.0;
    int penLifts = 0;
    unsigned labelsSkipped = 0;
    unsigned unknownInstructions = 0;
    int lastError = 0;
};

HpglPreviewResult PreviewHpgl(const QByteArray &hpgl, double plotWidthMm, double plotHeightMm);
