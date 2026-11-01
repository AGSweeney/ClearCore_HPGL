/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#pragma once

#include <QVector3D>
#include <QString>
#include <QVector>

// Minimal ASCII DXF import (LINE, LWPOLYLINE, POLYLINE/VERTEX, ARC, CIRCLE).
// LWPOLYLINE / POLYLINE vertex bulge (group 42) is tessellated to arcs; straight if bulge is 0.
// Binary DXF is not supported. Geometry is returned in millimeters (Z often 0 for 2D).
// $INSUNITS: 0 (unitless) and missing variable both use inches for raw coordinates; 4 = mm, 1 = inches, etc.
struct DxfImportOutput {
    QString errorMessage;
    /// Each polyline is one continuous pen-down path (mm).
    QVector<QVector<QVector3D>> polylinesMm;
    /// Pen-up moves as flat start/end pairs in mm (between polylines).
    QVector<QVector3D> rapidSegmentsMm;
    /// HP/GL-2 style commands (plotter units: 40 units per mm); suitable for preview / export.
    QString hpgl;
    /// Raw DXF $INSUNITS (same convention as import).
    int insunits = 0;
    /// Multiply drawing-unit coordinates by this to get millimeters (round-trip for export).
    double drawingToMmScale = 25.4;
};

bool ImportDxfFile(const QString &filePath, DxfImportOutput *out);

/// Order strips for cutting: first join strips whose ends meet (within 0.06 mm) into longer paths,
/// then draw inside-out -- everything enclosed by a closed contour is drawn before that contour, so
/// the outermost outline is last (a vinyl or laser cutter would otherwise free the part before its
/// interior cuts). Among the shapes that are ready, the nearest to the pen is drawn next, entering a
/// closed contour at its nearest vertex and an open path at its nearer end, to keep pen-up travel
/// short. Rebuilds the pen-up moves.
void OrderStripsForCutting(QVector<QVector<QVector3D>> *strips, QVector<QVector3D> *rapids);

/// HP-GL for the strips (mm). With arcs, runs of points that lie on one circle in tessellation-sized
/// steps (DXF ARC, CIRCLE and bulges, also after joining, reversing and moving) go out as AA true
/// arcs; everything else as PD chains.
/// pdOffsets (optional): per strip, the character offset of its first pen-down instruction (PD, or
/// the PD; before an AA), -1 if skipped.
QString BuildHpglFromPolylinesMm(const QVector<QVector<QVector3D>> &polylinesMm, QVector<int> *pdOffsets = nullptr,
                                 bool arcs = true);

/// Write minimal ASCII DXF (LWPOLYLINE per strip, bulge 0) in drawing units from mm geometry.
bool ExportAsciiDxfMinimal(const QString &filePath,
                           const QVector<QVector<QVector3D>> &polylinesMm,
                           int insunits,
                           double drawingToMmScale,
                           QString *errorMessage);
