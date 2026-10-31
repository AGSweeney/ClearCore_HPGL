// Prints what the app's DXF importer produces: strips, vertices, extent, drawn length.
//   dxf_probe <file.dxf>

#include "DxfImport.h"

#include <QCoreApplication>
#include <QFile>

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
    /* Cut order: nothing inside a closed contour may be drawn after it (independent check: a strip
     * is inside when all of its vertices are, by ray casting). */
    {
        auto inside = [](const QVector<QVector3D> &poly, const QVector3D &p) {
            bool in = false;
            for (int i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                if ((poly[i].y() > p.y()) != (poly[j].y() > p.y()) &&
                    p.x() < poly[i].x() + (p.y() - poly[i].y()) * (poly[j].x() - poly[i].x()) /
                                              (poly[j].y() - poly[i].y())) {
                    in = !in;
                }
            }
            return in;
        };
        const auto &s = out.polylinesMm;
        int closed = 0, violations = 0;
        for (int c = 0; c < s.size(); c++) {
            if (s[c].size() < 4 || (s[c].first() - s[c].last()).length() > 0.08f) {
                continue;
            }
            closed++;
            for (int k = c + 1; k < s.size(); k++) {
                bool all = true;
                for (const QVector3D &p : s[k]) {
                    if (!inside(s[c], p)) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    violations++;
                    if (violations <= 5) {
                        std::printf("  order violation: strip %d is inside closed strip %d but drawn after it\n", k, c);
                    }
                }
            }
        }
        const auto &last = s.last();
        float lx0 = FLT_MAX, ly0 = FLT_MAX, lx1 = -FLT_MAX, ly1 = -FLT_MAX;
        for (const QVector3D &p : last) {
            lx0 = qMin(lx0, p.x());
            ly0 = qMin(ly0, p.y());
            lx1 = qMax(lx1, p.x());
            ly1 = qMax(ly1, p.y());
        }
        std::printf("cut order: %d closed contours, %d violations; last strip spans %.1f x %.1f mm\n", closed,
                    violations, lx1 - lx0, ly1 - ly0);
    }
    {
        /* Pen-up travel, starting from the lower-left corner (0,0 once placed at origin). */
        QVector3D pen(minX, minY, 0.f);
        double air = 0.0;
        for (const auto &strip : out.polylinesMm) {
            air += double((strip.first() - pen).length());
            pen = strip.last();
        }
        std::printf("pen-up travel %.0f mm (drawn %.0f mm)\n", air, len);
    }
    QVector<int> pdOffsets;
    const QByteArray hpgl = BuildHpglFromPolylinesMm(out.polylinesMm, &pdOffsets).toLatin1() + "\nPU;SP0;\n";
    {
        /* Active-line tracking: each strip's offset must point at its own PD instruction. */
        int bad = 0;
        for (int s = 0; s < out.polylinesMm.size(); s++) {
            const int o = pdOffsets.value(s, -1);
            if (out.polylinesMm[s].size() >= 2 && (o < 1 || hpgl.mid(o, 2) != "PD" || hpgl.at(o - 1) != '\n')) {
                bad++;
            }
        }
        std::printf("PD offsets: %d strips, %d wrong\n", int(out.polylinesMm.size()), bad);
    }
    std::printf("job HP-GL: %d bytes, %d instructions (';')\n", int(hpgl.size()), int(hpgl.count(';')));
    if (argc > 2) {
        /* Write the job as the app sends it with "Place DXF at origin" on. */
        QVector<QVector<QVector3D>> placed = out.polylinesMm;
        for (auto &strip : placed) {
            for (QVector3D &p : strip) {
                p -= QVector3D(minX, minY, 0.f);
            }
        }
        QFile f(QString::fromLocal8Bit(argv[2]));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(BuildHpglFromPolylinesMm(placed).toLatin1() + "\nPU;SP0;\n");
        }
    }
    return 0;
}
