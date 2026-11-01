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
        paths.push_back({{x / 40.0, y / 40.0}});
    }
    void MoveTo(double px, double py) override {
        x = px;
        y = py;
        if (down) paths.back().push_back({x / 40.0, y / 40.0});
    }
    bool ArcTo(double cx, double cy, double ex, double ey, double sweepDeg) override {
        arcs++;
        const double r = std::hypot(x - cx, y - cy);
        const double a0 = std::atan2(y - cy, x - cx);
        const int n = int(std::ceil(std::fabs(sweepDeg)));
        for (int k = 1; k <= n; k++) {
            const double a = a0 + sweepDeg * kPi / 180.0 * double(k) / double(n);
            if (down) paths.back().push_back({(cx + r * std::cos(a)) / 40.0, (cy + r * std::sin(a)) / 40.0});
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

void Render(const QString &hpgl, RenderSink *sink) {
    HpglInterpreter h;
    h.Init(sink, 16000, 12000, "7475A");
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
    "0\nENDSEC\n0\nEOF\n";

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
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
    CHECK(strips.size() == 5);
    OrderStripsForCutting(&strips, &in.rapidSegmentsMm);

    QVector<int> offsets;
    const QString withArcs = BuildHpglFromPolylinesMm(strips, &offsets, true);
    const QString chords = BuildHpglFromPolylinesMm(strips, nullptr, false);
    std::printf("HP-GL with arcs:\n%s\n\n", qPrintable(withArcs));

    printf("AA output\n");
    /* circle, arc and the slot's two ends; the slot is entered at its nearest vertex, the middle of
     * one end, which splits that end in two */
    const int aa = int(withArcs.count(QLatin1String("AA")));
    CHECK(aa == 5);
    CHECK(withArcs.contains(QLatin1String(",360.000;")) || withArcs.contains(QLatin1String(",-360.000;")));
    CHECK(!chords.contains(QLatin1String("AA")));
    CHECK(withArcs.size() < chords.size() / 3);
    for (int s = 0; s < strips.size(); s++) {
        CHECK(offsets[s] >= 0 && withArcs.mid(offsets[s], 2) == QLatin1String("PD"));
    }

    printf("interpreted: arcs land on the drawing\n");
    RenderSink r;
    Render(withArcs, &r);
    CHECK(r.arcs == aa);
    double worst = 0.0;
    for (const auto &path2 : r.paths) {
        for (const Pt &p : path2) {
            worst = std::max(worst, DistToStrips(p, strips));
        }
    }
    /* chords of at most 11.25 degrees sit inside the true arc by r (1 - cos 5.6) <= 0.1 mm at r 20 */
    std::printf("  max distance from the strips: %.4f mm\n", worst);
    CHECK(worst < 0.12);
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
