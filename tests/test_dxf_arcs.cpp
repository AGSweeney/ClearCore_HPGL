/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

// DXF arcs go out as AA true arcs, and the firmware's interpreter draws them where the strips are.
//   test_dxf_arcs        (exit code 0 = pass)
//
// A small drawing (circle, arc, a slot made of a polyline with bulges, a line, a hexagon) is
// imported, ordered for cutting and written with BuildHpglFromPolylinesMm. The HP-GL is fed to
// HpglInterpreter with a sink that accepts arcs, as the plotter does; every drawn point must lie on
// the drawing and every arc must end on a strip vertex.

#include "DxfImport.h"
#include "Hpgl.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Pt {
    double x, y;
    double tol = 0.03;  // allowed distance from the strips (mm): chord sag on arcs
};

/* Records pen-down geometry in mm; arcs are sampled every degree. */
class RenderSink : public HpglSink {
public:
    std::vector<std::vector<Pt>> paths;  // pen-down runs
    std::vector<Pt> arcEnds;
    int arcs = 0;
    double x = 0, y = 0;
    bool down = false;

    void PenUp() override { down = false; }
    void PenDown() override {
        down = true;
        paths.push_back({{x / 40.0, y / 40.0, 0.03}});
    }
    void MoveTo(double px, double py) override {
        x = px;
        y = py;
        if (down) paths.back().push_back({x / 40.0, y / 40.0, 0.03});
    }
    bool ArcTo(double cx, double cy, double ex, double ey, double sweepDeg) override {
        arcs++;
        const double r = std::hypot(x - cx, y - cy);
        const double a0 = std::atan2(y - cy, x - cx);
        const int n = int(std::ceil(std::fabs(sweepDeg)));
        /* the strips are chords of at most 11.25 degrees: inside the arc by up to r (1 - cos 5.625) */
        const double sag = r / 40.0 * (1.0 - std::cos(kPi / 32.0)) + 0.03;
        for (int k = 1; k <= n; k++) {
            const double a = a0 + sweepDeg * kPi / 180.0 * double(k) / double(n);
            if (down) paths.back().push_back({(cx + r * std::cos(a)) / 40.0, (cy + r * std::sin(a)) / 40.0, sag});
        }
        x = ex;
        y = ey;
        arcEnds.push_back({ex / 40.0, ey / 40.0});
        return true;
    }
    void SetVelocity(double) override {}
    void SelectPen(int) override {}
    void Reply(const char *) override {}
    void ActualPosition(double *px, double *py, bool *pd) override {
        *px = x;
        *py = y;
        *pd = down;
    }
    void PageEnd() override {}
    uint32_t Room() override { return 100000; }
};

void Render(const QString &hpgl, RenderSink *sink, double bedWPu = 16000, double bedHPu = 16000) {
    HpglInterpreter h;
    h.Init(sink, bedWPu, bedHPu, "7475A");
    const QByteArray b = hpgl.toLatin1();
    for (char c : b) {
        h.Feed(uint8_t(c));
        while (h.Busy()) h.Pump();
    }
}

double DistToSegment(Pt p, Pt a, Pt b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

double DistToStrips(Pt p, const QVector<QVector<QVector3D>> &strips) {
    double best = 1e9;
    for (const auto &s : strips) {
        for (int i = 1; i < s.size(); i++) {
            best = std::min(best, DistToSegment(p, {s[i - 1].x(), s[i - 1].y()}, {s[i].x(), s[i].y()}));
        }
    }
    return best;
}

double DistToVertex(Pt p, const QVector<QVector<QVector3D>> &strips) {
    double best = 1e9;
    for (const auto &s : strips) {
        for (const QVector3D &v : s) {
            best = std::min(best, std::hypot(p.x - v.x(), p.y - v.y()));
        }
    }
    return best;
}

const char *kDxf =
    "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n"
    "0\nSECTION\n2\nENTITIES\n"
    // circle r 10 at 50,50
    "0\nCIRCLE\n8\n0\n10\n50\n20\n50\n30\n0\n40\n10\n"
    // arc r 20 at 100,50 from 0 to 135 degrees
    "0\nARC\n8\n0\n10\n100\n20\n50\n30\n0\n40\n20\n50\n0\n51\n135\n"
    // slot: 40 x 20 with semicircular ends (bulge 1)
    "0\nLWPOLYLINE\n8\n0\n90\n4\n70\n1\n"
    "10\n150\n20\n40\n42\n0\n10\n190\n20\n40\n42\n1\n10\n190\n20\n60\n42\n0\n10\n150\n20\n60\n42\n1\n"
    // a line
    "0\nLINE\n8\n0\n10\n10\n20\n120\n30\n0\n11\n60\n21\n120\n31\n0\n"
    // hexagon, r 15 at 250,50: its vertices are on a circle, but it is not one
    "0\nLWPOLYLINE\n8\n0\n90\n6\n70\n1\n"
    "10\n265\n20\n50\n10\n257.5\n20\n62.990381\n10\n242.5\n20\n62.990381\n"
    "10\n235\n20\n50\n10\n242.5\n20\n37.009619\n10\n257.5\n20\n37.009619\n"
    // Stencil outline from a real job (NewStencil_0.036_ID.dxf, in mm, shortened to fit): a rounded
    // rectangle whose corners are two bulges of about 45 degrees each, with straight sides. A
    // corner's last point, a straight side and the next corner's first point lie exactly on one
    // large circle (they are symmetric) in steps under 11.25 degrees, but they are not an arc: the
    // side was sent as a 9-degree AA bowing out 1.6 mm.
    "0\nLWPOLYLINE\n8\n0\n90\n12\n70\n1\n"
    "10\n184.4243\n20\n238.4158\n42\n0.000000\n"
    "10\n14.1427\n20\n238.4158\n42\n0.198658\n"
    "10\n9.6525\n20\n236.5682\n42\n0.200520\n"
    "10\n7.7927\n20\n232.1077\n42\n0.000000\n"
    "10\n7.7927\n20\n149.4737\n42\n0.200520\n"
    "10\n9.6525\n20\n145.0134\n42\n0.198658\n"
    "10\n14.1427\n20\n143.1658\n42\n0.000000\n"
    "10\n184.4243\n20\n143.1658\n42\n0.198658\n"
    "10\n188.9143\n20\n145.0134\n42\n0.200520\n"
    "10\n190.7743\n20\n149.4737\n42\n0.000000\n"
    "10\n190.7743\n20\n232.1077\n42\n0.200520\n"
    "10\n188.9143\n20\n236.5682\n42\n0.198658\n"
    "0\nENDSEC\n0\nEOF\n";

}  // namespace

/* test_dxf_arcs <file.dxf> [out.plt]: the same pipeline on a real drawing, placed at the origin as
 * the app's "place at origin" does, on a 600 x 300 mm bed. Every drawn point must be within chord
 * sag of the strips, every AA must be drawn as an arc (not clipped back to chords) and every strip
 * must be drawn in one pen-down run (nothing lifted at the bed edge). Writes the HP-GL when asked. */
int CheckFile(const QString &dxf, const QString &plt) {
    DxfImportOutput in;
    if (!ImportDxfFile(dxf, &in)) {
        std::printf("import failed: %s\n", qPrintable(in.errorMessage));
        return 2;
    }
    QVector<QVector<QVector3D>> strips = in.polylinesMm;
    OrderStripsForCutting(&strips, &in.rapidSegmentsMm);
    float minX = 1e30f, minY = 1e30f;
    for (const auto &s : strips) {
        for (const QVector3D &p : s) {
            minX = std::min(minX, p.x());
            minY = std::min(minY, p.y());
        }
    }
    for (auto &s : strips) {
        for (QVector3D &p : s) {
            p -= QVector3D(minX, minY, 0.f);
        }
    }
    const QString hpgl = BuildHpglFromPolylinesMm(strips, nullptr, true);
    if (!plt.isEmpty()) {
        QFile f(plt);
        if (f.open(QIODevice::WriteOnly)) f.write(hpgl.toLatin1() + "\nPU;SP0;\n");
    }
    RenderSink r;
    Render(hpgl, &r, 600 * 40, 300 * 40);
    const int aaText = int(hpgl.count(QLatin1String("AA")));
    std::printf("AA sent %d, drawn as arcs %d; strips %d, pen-down runs %d\n", aaText, r.arcs,
                int(strips.size()), int(r.paths.size()));
    double worst = 0.0;
    Pt at{0, 0};
    for (const auto &path : r.paths) {
        for (const Pt &p : path) {
            const double d = DistToStrips(p, strips) - p.tol;
            if (d > worst) {
                worst = d;
                at = p;
            }
        }
    }
    std::printf("%d strips, %d AA; worst excess over chord sag %.4f mm at (%.3f, %.3f)\n", int(strips.size()),
                r.arcs, worst, at.x, at.y);
    return (worst > 0.0 || r.arcs != aaText || r.paths.size() != size_t(strips.size())) ? 1 : 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        return CheckFile(QString::fromLocal8Bit(argv[1]), argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
    }
    QTemporaryDir dir;
    const QString path = dir.filePath("arcs.dxf");
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            std::printf("cannot write %s\n", qPrintable(path));
            return 2;
        }
        f.write(kDxf);
    }
    DxfImportOutput in;
    CHECK(ImportDxfFile(path, &in));
    QVector<QVector<QVector3D>> strips = in.polylinesMm;
    CHECK(strips.size() == 6);
    OrderStripsForCutting(&strips, &in.rapidSegmentsMm);

    QVector<int> offsets;
    const QString withArcs = BuildHpglFromPolylinesMm(strips, &offsets, true);
    const QString chords = BuildHpglFromPolylinesMm(strips, nullptr, false);
    std::printf("HP-GL with arcs:\n%s\n\n", qPrintable(withArcs));

    printf("AA output\n");
    /* circle, arc and the slot's two ends (the slot is entered at its nearest vertex, the middle of
     * one end, which splits that end in two): 5; the outline's eight corner bulges, less the one
     * the entry vertex splits into 2 + 2 steps (too short to call arcs: PD chords): 7. Its
     * straight sides stay PD. */
    const int aa = int(withArcs.count(QLatin1String("AA")));
    CHECK(aa == 12);
    CHECK(withArcs.contains(QLatin1String(",360.000;")) || withArcs.contains(QLatin1String(",-360.000;")));
    CHECK(!chords.contains(QLatin1String("AA")));
    CHECK(withArcs.size() < chords.size() / 2);
    for (int s = 0; s < strips.size(); s++) {
        CHECK(offsets[s] >= 0 && withArcs.mid(offsets[s], 2) == QLatin1String("PD"));
    }

    printf("interpreted: arcs land on the drawing\n");
    RenderSink r;
    Render(withArcs, &r);
    CHECK(r.arcs == aa);
    double worst = 0.0;
    double excess = 0.0;
    for (const auto &path2 : r.paths) {
        for (const Pt &p : path2) {
            const double d = DistToStrips(p, strips);
            worst = std::max(worst, d);
            excess = std::max(excess, d - p.tol);
        }
    }
    /* chords of at most 11.25 degrees sit inside the true arc by r (1 - cos 5.6) <= 0.1 mm at r 20 */
    std::printf("  max distance from the strips: %.4f mm\n", worst);
    CHECK(worst < 0.12);
    CHECK(excess <= 0.0);
    double endWorst = 0.0;
    for (const Pt &e : r.arcEnds) {
        endWorst = std::max(endWorst, DistToVertex(e, strips));
    }
    std::printf("  max arc end to nearest vertex: %.4f mm\n", endWorst);
    CHECK(endWorst < 0.03);

    printf("interpreted: same pen-down runs as the chords\n");
    RenderSink rc;
    Render(chords, &rc);
    CHECK(rc.arcs == 0);
    CHECK(rc.paths.size() == r.paths.size());
    for (size_t i = 0; i < rc.paths.size() && i < r.paths.size(); i++) {
        const Pt a = rc.paths[i].back(), b = r.paths[i].back();
        CHECK(std::hypot(a.x - b.x, a.y - b.y) < 0.03);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
