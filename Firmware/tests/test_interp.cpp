/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

// libClearCore LinearInterpolator: no overshoot past the segment end, no step burst, exact end.
// Regression for the 20 mm/s motor fault: a 2-step X over 8800-step Y segment overran Y by
// 1685 steps with per-axis Q15 increments, then the end clamp snapped it back in one sample.

#include "LinearInterpolator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ClearCore;

struct Result {
    long overshoot;  // furthest past the end, either axis, in its direction of travel
    long maxStep;    // largest steps in one sample
    bool endOk;      // finished exactly on the end point
};

static Result Run(int sx, int sy, int ex, int ey, unsigned v, unsigned a, unsigned entry, unsigned exitV) {
    LinearInterpolator li;
    Result r{0, 0, true};
    if (!li.InitializeLinear(sx, sy, ex, ey, v, a, 5000, entry, exitV)) {
        return r;
    }
    long x = sx, y = sy, samples = 0;
    const int dx = ex > sx ? 1 : (ex < sx ? -1 : 0);
    const int dy = ey > sy ? 1 : (ey < sy ? -1 : 0);
    int32_t stx, sty;
    while (li.GenerateNextSteps(stx, sty) && ++samples < 50000000) {
        x += stx;
        y += sty;
        const long m = labs(stx) > labs(sty) ? labs(stx) : labs(sty);
        if (m > r.maxStep) r.maxStep = m;
        const long ox = dx > 0 ? x - ex : (dx < 0 ? ex - x : labs(x - ex));
        const long oy = dy > 0 ? y - ey : (dy < 0 ? ey - y : labs(y - ey));
        if (ox > r.overshoot) r.overshoot = ox;
        if (oy > r.overshoot) r.overshoot = oy;
    }
    r.endOk = (x == ex && y == ey);
    return r;
}

int main() {
    const unsigned accel = 7500 * 80;  // 7500 mm/s^2 at 80 steps/mm
    const Result r = Run(12204, 7606, 12206, 16406, 1600, accel, 0, 1600);
    printf("failing segment: overshoot %ld steps, max %ld steps/sample, ends on target %s\n", r.overshoot,
           r.maxStep, r.endOk ? "yes" : "NO");
    srand(1);
    long worstOver = 0, worstStep = 0, badEnds = 0;
    for (int i = 0; i < 20000; i++) {
        const int sx = rand() % 32000, sy = rand() % 24000;
        const int ex = (rand() % 4 == 0) ? sx + (rand() % 7) - 3 : rand() % 32000;  // many near-axis moves
        const int ey = (rand() % 4 == 1) ? sy + (rand() % 7) - 3 : rand() % 24000;
        const unsigned v = 400 + rand() % 23600;  // 5 .. 300 mm/s
        const unsigned en = (rand() % 2) ? 0 : v, ex2 = (rand() % 2) ? 0 : v;
        const Result q = Run(sx, sy, ex, ey, v, accel, en, ex2);
        if (q.overshoot > worstOver) worstOver = q.overshoot;
        const long allowed = (long)ceil(v / 5000.0) + 1;
        if (q.maxStep - allowed > worstStep) worstStep = q.maxStep - allowed;
        if (!q.endOk) badEnds++;
    }
    printf("20000 random segments: worst overshoot %ld steps, worst step above speed+1 %ld, wrong end %ld\n",
           worstOver, worstStep, badEnds);
    const bool ok = r.overshoot == 0 && r.maxStep <= 1 && r.endOk && worstOver == 0 && worstStep == 0 && badEnds == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
