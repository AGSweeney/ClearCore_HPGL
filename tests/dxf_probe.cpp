// Prints what the app's DXF importer produces: strips, vertices, extent, drawn length.
//   dxf_probe <file.dxf>

#include "DxfImport.h"

#include <QCoreApplication>

#include <cfloat>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        return 2;
    }
    DxfImportOutput out;
    if (!ImportDxfFile(QString::fromLocal8Bit(argv[1]), &out)) {
        std::printf("import failed: %s\n", qPrintable(out.errorMessage));
        return 1;
    }
    double len = 0.0;
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    int verts = 0;
    for (const auto &strip : out.polylinesMm) {
        verts += strip.size();
        for (int i = 0; i < strip.size(); i++) {
            minX = qMin(minX, strip[i].x());
            minY = qMin(minY, strip[i].y());
            maxX = qMax(maxX, strip[i].x());
            maxY = qMax(maxY, strip[i].y());
            if (i > 0) {
                len += double((strip[i] - strip[i - 1]).length());
            }
        }
    }
    std::printf("insunits=%d scale=%.4f mm/unit\n", out.insunits, out.drawingToMmScale);
    std::printf("strips=%d vertices=%d drawn=%.3f mm\n", int(out.polylinesMm.size()), verts, len);
    std::printf("extent X %.3f..%.3f  Y %.3f..%.3f mm  (%.3f x %.3f mm)\n", minX, maxX, minY, maxY, maxX - minX,
                maxY - minY);
    const QByteArray hpgl = BuildHpglFromPolylinesMm(out.polylinesMm).toLatin1() + "\nPU;SP0;\n";
    std::printf("job HP-GL: %d bytes, %d instructions (';')\n", int(hpgl.size()), int(hpgl.count(';')));
    return 0;
}
