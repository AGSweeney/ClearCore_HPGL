#include "JobOptimizer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

const int kJoinTolPu = 3;               /* stroke ends this close (0.075 mm) are the same point */
const int kClosedTolPu = 3;             /* first ~ last: a closed contour */
const float kNearlyClosedFrac = 0.05f;  /* open, but gap < 5% of its size: still encloses */

inline int16_t ClampPu(double v) {
    if (v < -32767.0) return -32767;
    if (v > 32767.0) return 32767;
    return (int16_t)lround(v);
}

inline float Dist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

inline bool Near(int ax, int ay, int bx, int by, int tol) {
    return abs(ax - bx) <= tol && abs(ay - by) <= tol;
}

}  // namespace

void JobOptimizer::Init(HpglSink *out) {
    m_out = out;
    m_mode = M_PASS;
    ResetBuffer();
}

void JobOptimizer::ResetBuffer() {
    m_pointCount = 0;
    m_strokeCount = 0;
    m_chainCount = 0;
    m_pathCount = 0;
    m_stepCount = 0;
    m_inStroke = false;
    m_pageEndPending = false;
}

/* ------------------------------------------------------------------------- */
/* Job control                                                                */
/* ------------------------------------------------------------------------- */

void JobOptimizer::BeginJob(bool optimize) {
    ResetBuffer();
    m_passRest = false;
    if (!optimize || !m_out) {
        m_mode = M_PASS;
        return;
    }
    double x = 0, y = 0;
    bool down = false;
    m_out->ActualPosition(&x, &y, &down);
    m_x = x;
    m_y = y;
    m_down = down;
    m_realDown = down;
    m_velocity = m_curVelocity < 0 ? 0 : m_curVelocity;
    m_pen = m_curPen < 0 ? 1 : m_curPen;
    m_statStrokes = m_statPaths = 0;
    m_statBefore = m_statAfter = 0;
    m_statOverflow = false;
    m_mode = M_COLLECT;
}

void JobOptimizer::EndJob() {
    if (m_mode != M_COLLECT) {
        return;  /* passing through (off, or after a spill): nothing held back */
    }
    EndStroke();
    Order();
    m_replayRaw = false;
    m_replayStep = 0;
    m_replayPhase = 0;
    m_mode = M_REPLAY;
}

void JobOptimizer::Abort() {
    ResetBuffer();
    m_mode = M_PASS;
    m_passRest = false;
    m_curVelocity = -1;  /* unknown after an abort: re-sent by the next replay */
    m_curPen = -1;
}

/* ------------------------------------------------------------------------- */
/* Collection (HpglSink)                                                      */
/* ------------------------------------------------------------------------- */

void JobOptimizer::AddPoint(double x, double y) {
    if (m_pointCount >= kMaxPoints || m_strokeCount == 0) {
        return;  /* Room() spills before this */
    }
    const int16_t px = ClampPu(x), py = ClampPu(y);
    Stroke &s = m_strokes[m_strokeCount - 1];
    if (s.count > 0) {
        const uint16_t last = (uint16_t)(s.start + s.count - 1);
        if (m_in[2 * last] == px && m_in[2 * last + 1] == py) {
            return;
        }
    }
    m_in[2 * m_pointCount] = px;
    m_in[2 * m_pointCount + 1] = py;
    m_pointCount++;
    s.count++;
}

void JobOptimizer::StartStroke() {
    if (m_strokeCount >= kMaxStrokes) {
        return;  /* Room() spills before this */
    }
    Stroke &s = m_strokes[m_strokeCount++];
    s.start = m_pointCount;
    s.count = 0;
    s.velocity = m_velocity;
    s.pen = (int8_t)m_pen;
    m_inStroke = true;
    AddPoint(m_x, m_y);
}

void JobOptimizer::EndStroke() {
    m_inStroke = false;
}

void JobOptimizer::PenDown() {
    if (m_mode == M_PASS) {
        m_out->PenDown();
        m_realDown = true;
        return;
    }
    if (!m_down) {
        m_down = true;
        StartStroke();
    }
}

void JobOptimizer::PenUp() {
    if (m_mode == M_PASS) {
        m_out->PenUp();
        m_realDown = false;
        return;
    }
    m_down = false;
    EndStroke();
}

void JobOptimizer::MoveTo(double xPu, double yPu) {
    if (m_mode == M_PASS) {
        m_out->MoveTo(xPu, yPu);
        return;
    }
    m_x = xPu;
    m_y = yPu;
    if (m_down && m_inStroke) {
        AddPoint(xPu, yPu);
    }
}

void JobOptimizer::SetVelocity(double mmPerSec) {
    if (m_mode == M_PASS) {
        m_out->SetVelocity(mmPerSec);
        m_curVelocity = (float)mmPerSec;
        return;
    }
    if ((float)mmPerSec == m_velocity) {
        return;
    }
    m_velocity = (float)mmPerSec;
    if (m_inStroke) {  /* speed change mid-line: continue as a new stroke */
        EndStroke();
        StartStroke();
    }
}

void JobOptimizer::SelectPen(int pen) {
    if (m_mode == M_PASS) {
        m_out->SelectPen(pen);
        m_curPen = pen;
        return;
    }
    if (pen == m_pen) {
        return;
    }
    m_pen = pen;
    if (m_inStroke) {
        EndStroke();
        StartStroke();
    }
}

void JobOptimizer::Reply(const char *text) {
    m_out->Reply(text);
}

void JobOptimizer::ActualPosition(double *xPu, double *yPu, bool *penDown) {
    if (m_mode == M_COLLECT) {
        *xPu = m_x;  /* where the interpreter believes the pen is */
        *yPu = m_y;
        *penDown = m_down;
        return;
    }
    m_out->ActualPosition(xPu, yPu, penDown);
}

void JobOptimizer::PageEnd() {
    if (m_mode != M_COLLECT) {
        m_out->PageEnd();
        return;
    }
    /* Plot this page in order, then the page break, then collect the next page. */
    EndStroke();
    Order();
    m_replayRaw = false;
    m_replayStep = 0;
    m_replayPhase = 0;
    m_pageEndPending = true;
    m_mode = M_REPLAY;
}

uint32_t JobOptimizer::Room() {
    if (m_mode == M_PASS) {
        return m_out->Room();
    }
    if (m_mode == M_REPLAY) {
        return 0;  /* hold the interpreter until the buffered strokes are out */
    }
    const uint32_t points = kMaxPoints - m_pointCount;
    const uint32_t strokes = kMaxStrokes - m_strokeCount;
    const uint32_t room = points < strokes ? points : strokes;
    /* Keep a margin for the calls one Feed()/Pump() may make. Spill as soon as what is left
     * would stall the interpreter (<= kRoomReserve), else nothing ever frees space. */
    if (room <= 3 * HpglInterpreter::kRoomReserve + 2) {
        Spill();
        return 0;
    }
    return room - 2 * HpglInterpreter::kRoomReserve;
}

void JobOptimizer::Spill() {
    /* Too big to reorder: plot what is buffered exactly as received, then pass the rest of the
     * job through. The last stroke may still be open (pen down); replay leaves it down. */
    m_statOverflow = true;
    m_statStrokes = m_strokeCount;
    m_stepCount = 0;
    for (uint16_t i = 0; i < m_strokeCount; i++) {
        m_steps[m_stepCount].path = i;
        m_steps[m_stepCount].entry = 0;
        m_steps[m_stepCount].reverse = false;
        m_stepCount++;
    }
    m_replayRaw = true;
    m_replayStep = 0;
    m_replayPhase = 0;
    m_passRest = true;
    m_mode = M_REPLAY;
}

/* ------------------------------------------------------------------------- */
/* Paths: lists of (stroke, direction) over the received points               */
/* ------------------------------------------------------------------------- */

void JobOptimizer::StrokeEnd(uint16_t stroke, bool atStart, int *x, int *y) const {
    const Stroke &s = m_strokes[stroke];
    const uint16_t k = atStart ? s.start : (uint16_t)(s.start + s.count - 1);
    *x = m_in[2 * k];
    *y = m_in[2 * k + 1];
}

void JobOptimizer::PathEnd(uint16_t path, bool last, int *x, int *y) const {
    const Path &p = m_paths[path];
    const uint16_t e = m_chain[p.first + (last ? p.segs - 1 : 0)];
    const bool rev = (e & kRev) != 0;
    /* first point: the stroke's start, or its end when drawn reversed; last point: opposite */
    StrokeEnd((uint16_t)(e & ~kRev), last ? rev : !rev, x, y);
}

/* Forward: each segment's points in drawing order, the joint shared with the previous segment
 * once. Backward: the same sequence end to start. */
void JobOptimizer::IterBegin(uint16_t path, bool backward, Iter *it) const {
    const Path &p = m_paths[path];
    it->backward = backward;
    if (!backward) {
        it->seg = 0;
        it->off = 0;
        return;
    }
    it->seg = (uint16_t)(p.segs - 1);
    it->off = (uint16_t)(m_strokes[m_chain[p.first + it->seg] & ~kRev].count - (it->seg > 0 ? 1 : 0));
}

bool JobOptimizer::IterNext(uint16_t path, Iter *it, int16_t *x, int16_t *y) const {
    const Path &p = m_paths[path];
    while (it->seg < p.segs) {
        const uint16_t e = m_chain[p.first + it->seg];
        const Stroke &s = m_strokes[e & ~kRev];
        const bool rev = (e & kRev) != 0;
        const uint16_t low = it->seg > 0 ? 1 : 0;  /* k = 0 is the previous segment's end */
        uint16_t k;
        if (!it->backward) {
            if (it->off < low) {
                it->off = low;
            }
            if (it->off >= s.count) {
                it->seg++;
                it->off = 0;
                continue;
            }
            k = it->off++;
        } else {
            if (it->off == 0) {
                if (it->seg == 0) {
                    it->seg = p.segs;  /* done */
                    return false;
                }
                it->seg--;
                const Stroke &prev = m_strokes[m_chain[p.first + it->seg] & ~kRev];
                it->off = (uint16_t)(prev.count - (it->seg > 0 ? 1 : 0));
                continue;
            }
            k = (uint16_t)(low + it->off - 1);
            it->off--;
        }
        const uint16_t idx = rev ? (uint16_t)(s.start + s.count - 1 - k) : (uint16_t)(s.start + k);
        *x = m_in[2 * idx];
        *y = m_in[2 * idx + 1];
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Ordering                                                                   */
/* ------------------------------------------------------------------------- */

void JobOptimizer::Order() {
    m_chainCount = 0;
    m_pathCount = 0;
    m_stepCount = 0;
    m_statStrokes += m_strokeCount;
    if (m_strokeCount == 0) {
        return;
    }
    /* Travel as received, from the pen's position before the first stroke. */
    double px = 0, py = 0;
    bool pd = false;
    m_out->ActualPosition(&px, &py, &pd);
    {
        float x = (float)px, y = (float)py;
        for (uint16_t i = 0; i < m_strokeCount; i++) {
            int sx, sy, ex, ey;
            StrokeEnd(i, true, &sx, &sy);
            StrokeEnd(i, false, &ex, &ey);
            m_statBefore += Dist(x, y, (float)sx, (float)sy) / (float)HPGL_PU_PER_MM;
            x = (float)ex;
            y = (float)ey;
        }
    }
    /* Groups of consecutive strokes with the same pen and speed keep their order. */
    int16_t penX = ClampPu(px), penY = ClampPu(py);
    uint16_t g = 0;
    while (g < m_strokeCount) {
        uint16_t e = g;
        while (e + 1 < m_strokeCount && m_strokes[e + 1].pen == m_strokes[g].pen &&
               m_strokes[e + 1].velocity == m_strokes[g].velocity) {
            e++;
        }
        const uint16_t firstPath = m_pathCount;
        JoinGroup(g, e);
        OrderGroup(firstPath, (uint16_t)(m_pathCount - 1), &penX, &penY);
        g = (uint16_t)(e + 1);
    }
    m_statPaths += m_pathCount;
}

/* Join strokes whose ends meet into paths (outlines sent as separate segments). Greedy in the
 * order received: each unused stroke is extended backward from its start, then forward from its
 * end, until it closes or nothing more touches it. */
void JobOptimizer::JoinGroup(uint16_t first, uint16_t last) {
    for (uint16_t i = first; i <= last; i++) {
        m_used[i] = false;
    }
    /* Unused, open stroke with an end at (x, y): returns it and whether that end is its start. */
    auto findAt = [&](int x, int y, bool *atStart) -> int {
        for (uint16_t j = first; j <= last; j++) {
            if (m_used[j] || m_strokes[j].count < 2) {
                continue;
            }
            int sx, sy, ex, ey;
            StrokeEnd(j, true, &sx, &sy);
            StrokeEnd(j, false, &ex, &ey);
            if (m_strokes[j].count >= 4 && Near(sx, sy, ex, ey, kClosedTolPu)) {
                continue;  /* already closed */
            }
            if (Near(sx, sy, x, y, kJoinTolPu)) {
                *atStart = true;
                return j;
            }
            if (Near(ex, ey, x, y, kJoinTolPu)) {
                *atStart = false;
                return j;
            }
        }
        return -1;
    };
    for (uint16_t i = first; i <= last; i++) {
        if (m_used[i]) {
            continue;
        }
        m_used[i] = true;
        int hx, hy, tx, ty;  /* head (first point) and tail (last point) of the path */
        StrokeEnd(i, true, &hx, &hy);
        StrokeEnd(i, false, &tx, &ty);
        const bool single = m_strokes[i].count < 2;
        bool closed = !single && m_strokes[i].count >= 4 && Near(hx, hy, tx, ty, kClosedTolPu);
        /* backward: strokes ending at the head, nearest first */
        uint16_t back = 0;
        while (!single && !closed) {
            bool atStart = false;
            const int j = findAt(hx, hy, &atStart);
            if (j < 0) {
                break;
            }
            m_used[j] = true;
            /* its start touches the head: drawn reversed, so its end becomes the new head */
            m_back[back++] = (uint16_t)(j | (atStart ? kRev : 0));
            StrokeEnd((uint16_t)j, !atStart, &hx, &hy);
            closed = Near(hx, hy, tx, ty, kClosedTolPu);
        }
        Path &p = m_paths[m_pathCount];
        p.first = m_chainCount;
        while (back > 0) {
            m_chain[m_chainCount++] = m_back[--back];
        }
        m_chain[m_chainCount++] = i;
        while (!single && !closed) {
            bool atStart = false;
            const int j = findAt(tx, ty, &atStart);
            if (j < 0) {
                break;
            }
            m_used[j] = true;
            /* its end touches the tail: drawn reversed, so its start becomes the new tail */
            m_chain[m_chainCount++] = (uint16_t)(j | (atStart ? 0 : kRev));
            StrokeEnd((uint16_t)j, !atStart, &tx, &ty);
            closed = Near(hx, hy, tx, ty, kClosedTolPu);
        }
        p.segs = (uint16_t)(m_chainCount - p.first);
        /* points, bounding box, closure, area */
        p.points = 0;
        p.x0 = p.y0 = 32767;
        p.x1 = p.y1 = -32767;
        double a = 0;
        int lx, ly;
        PathEnd(m_pathCount, true, &lx, &ly);
        double prevX = lx, prevY = ly;
        Iter it;
        int16_t x, y;
        IterBegin(m_pathCount, false, &it);
        while (IterNext(m_pathCount, &it, &x, &y)) {
            p.points++;
            if (x < p.x0) p.x0 = x;
            if (y < p.y0) p.y0 = y;
            if (x > p.x1) p.x1 = x;
            if (y > p.y1) p.y1 = y;
            a += prevX * y - (double)x * prevY;
            prevX = x;
            prevY = y;
        }
        int fx, fy;
        PathEnd(m_pathCount, false, &fx, &fy);
        const float diag = Dist(p.x0, p.y0, p.x1, p.y1);
        p.closed = p.points >= 4 && Near(fx, fy, lx, ly, kClosedTolPu);
        p.encloses = p.points >= 4 && (p.closed || Dist((float)fx, (float)fy, (float)lx, (float)ly) <= kNearlyClosedFrac * diag);
        p.area = p.encloses ? (float)fabs(a) * 0.5f : 0.f;
        p.pending = 0;
        p.done = false;
        m_pathCount++;
    }
}

bool JobOptimizer::PointInPath(uint16_t path, float x, float y) const {
    bool inside = false;
    int lx, ly;
    PathEnd(path, true, &lx, &ly);  /* closing edge: last -> first */
    float px = (float)lx, py = (float)ly;
    Iter it;
    int16_t qx, qy;
    IterBegin(path, false, &it);
    while (IterNext(path, &it, &qx, &qy)) {
        const float cx = qx, cy = qy;
        if ((cy > y) != (py > y)) {
            const float xc = cx + (y - cy) * (px - cx) / (py - cy);
            if (x < xc) {
                inside = !inside;
            }
        }
        px = cx;
        py = cy;
    }
    return inside;
}

/* outer must wait for inner. A contour waits for what it encloses (most of up to 9 sampled
 * vertices inside); an open path that encloses nothing -- a piece of an outline broken by a
 * gap -- waits for every smaller path inside its bounding box. */
bool JobOptimizer::Encloses(uint16_t outer, uint16_t inner) const {
    const Path &o = m_paths[outer];
    const Path &i = m_paths[inner];
    if (outer == inner || i.x0 < o.x0 - 2 || i.y0 < o.y0 - 2 || i.x1 > o.x1 + 2 || i.y1 > o.y1 + 2) {
        return false;
    }
    if (!o.encloses) {
        return (float)(i.x1 - i.x0) * (float)(i.y1 - i.y0) < (float)(o.x1 - o.x0) * (float)(o.y1 - o.y0);
    }
    if (i.encloses && o.area <= i.area) {
        return false;
    }
    const uint16_t samples = i.points < 9 ? i.points : 9;
    uint16_t taken = 0, in = 0, index = 0;
    Iter it;
    int16_t x, y;
    IterBegin(inner, false, &it);
    while (taken < samples && IterNext(inner, &it, &x, &y)) {
        const uint16_t want = (uint16_t)(samples > 1 ? ((uint32_t)taken * (i.points - 1)) / (samples - 1) : 0);
        if (index++ == want) {
            in += PointInPath(outer, x, y) ? 1 : 0;
            taken++;
        }
    }
    return in * 2 > samples;
}

/* Greedy nearest ready path: a path is ready once every path it encloses is drawn. */
void JobOptimizer::OrderGroup(uint16_t firstPath, uint16_t lastPath, int16_t *penX, int16_t *penY) {
    for (uint16_t a = firstPath; a <= lastPath; a++) {
        for (uint16_t b = firstPath; b <= lastPath; b++) {
            if (Encloses(a, b)) {
                m_paths[a].pending++;
            }
        }
    }
    float x = *penX, y = *penY;
    for (uint16_t step = firstPath; step <= lastPath; step++) {
        int best = -1;
        uint16_t bestEntry = 0;
        bool bestRev = false;
        float bestD = 3.4e38f, endX = x, endY = y;
        for (uint16_t a = firstPath; a <= lastPath; a++) {
            const Path &p = m_paths[a];
            if (p.done || p.pending > 0) {
                continue;
            }
            const float bx = fmaxf(0.f, fmaxf((float)p.x0 - x, x - (float)p.x1));
            const float by = fmaxf(0.f, fmaxf((float)p.y0 - y, y - (float)p.y1));
            if (sqrtf(bx * bx + by * by) >= bestD) {
                continue;
            }
            if (p.closed) {
                Iter it;
                int16_t vx, vy;
                uint16_t k = 0;
                IterBegin(a, false, &it);
                while (k + 1 < p.points && IterNext(a, &it, &vx, &vy)) {  /* last = first */
                    const float d = Dist(x, y, vx, vy);
                    if (d < bestD) {
                        bestD = d, best = a, bestEntry = k, bestRev = false, endX = vx, endY = vy;
                    }
                    k++;
                }
            } else {
                int fx, fy, lx, ly;
                PathEnd(a, false, &fx, &fy);
                PathEnd(a, true, &lx, &ly);
                const float df = Dist(x, y, (float)fx, (float)fy);
                const float dl = Dist(x, y, (float)lx, (float)ly);
                if (fminf(df, dl) < bestD) {
                    bestD = fminf(df, dl), best = a, bestEntry = 0, bestRev = dl < df;
                    endX = (float)(bestRev ? fx : lx);
                    endY = (float)(bestRev ? fy : ly);
                }
            }
        }
        if (best < 0) {  /* only with a containment cycle: take any remaining path */
            for (uint16_t a = firstPath; a <= lastPath && best < 0; a++) {
                if (!m_paths[a].done) {
                    best = a;
                }
            }
            int lx, ly;
            PathEnd((uint16_t)best, true, &lx, &ly);
            bestEntry = 0, bestRev = false, bestD = 0, endX = (float)lx, endY = (float)ly;
        }
        m_paths[best].done = true;
        for (uint16_t a = firstPath; a <= lastPath; a++) {
            if (!m_paths[a].done && m_paths[a].pending > 0 && Encloses(a, (uint16_t)best)) {
                m_paths[a].pending--;
            }
        }
        m_steps[m_stepCount].path = (uint16_t)best;
        m_steps[m_stepCount].entry = bestEntry;
        m_steps[m_stepCount].reverse = bestRev;
        m_stepCount++;
        m_statAfter += bestD / (float)HPGL_PU_PER_MM;
        x = endX;  /* a closed path ends where it was entered; an open one at its far end */
        y = endY;
    }
    *penX = (int16_t)x;
    *penY = (int16_t)y;
}

/* ------------------------------------------------------------------------- */
/* Replay                                                                     */
/* ------------------------------------------------------------------------- */

void JobOptimizer::Pump() {
    while (m_mode == M_REPLAY) {
        if (m_out->Room() <= HpglInterpreter::kRoomReserve + 2) {
            return;
        }
        if (m_replayStep < m_stepCount) {
            const Step &st = m_steps[m_replayStep];
            /* Next point of this step: strokes as received (spill); an open path forward or
             * backward; a closed path from its entry vertex round to the same vertex. */
            auto next = [&](int16_t *px, int16_t *py) -> bool {
                if (m_replayRaw) {
                    const Stroke &s = m_strokes[st.path];
                    if (m_emitted >= s.count) {
                        return false;
                    }
                    const uint16_t idx = (uint16_t)(s.start + m_emitted++);
                    *px = m_in[2 * idx];
                    *py = m_in[2 * idx + 1];
                    return true;
                }
                const Path &p = m_paths[st.path];
                if (!p.closed) {
                    return IterNext(st.path, &m_it, px, py);
                }
                const uint16_t firstLeg = (uint16_t)(p.points - 1 - st.entry);  /* entry .. n-2 */
                if (m_emitted < firstLeg) {
                    m_emitted++;
                    return IterNext(st.path, &m_it, px, py);
                }
                if (!m_wrapped) {
                    IterBegin(st.path, false, &m_it);
                    m_wrapped = true;
                }
                if (m_emitted >= p.points) {  /* second leg: 0 .. entry */
                    return false;
                }
                m_emitted++;
                return IterNext(st.path, &m_it, px, py);
            };
            int16_t px, py;
            const float vel = m_replayRaw ? m_strokes[st.path].velocity
                                          : m_strokes[m_chain[m_paths[st.path].first] & ~kRev].velocity;
            const int pen = m_replayRaw ? m_strokes[st.path].pen : m_strokes[m_chain[m_paths[st.path].first] & ~kRev].pen;
            switch (m_replayPhase) {
                case 0:  /* pen and speed */
                    if (pen != m_curPen) {
                        m_out->SelectPen(pen);
                        m_curPen = pen;
                    }
                    if (vel != m_curVelocity) {
                        m_out->SetVelocity(vel);
                        m_curVelocity = vel;
                    }
                    m_emitted = 0;
                    m_wrapped = false;
                    if (!m_replayRaw) {
                        const Path &p = m_paths[st.path];
                        IterBegin(st.path, !p.closed && st.reverse, &m_it);
                        if (p.closed) {
                            for (uint16_t k = 0; k < st.entry && IterNext(st.path, &m_it, &px, &py); k++) {
                            }
                        }
                    }
                    m_replayPhase = 1;
                    break;
                case 1:  /* travel to the first point */
                    if (m_realDown) {
                        m_out->PenUp();
                        m_realDown = false;
                    }
                    if (next(&px, &py)) {
                        m_out->MoveTo(px, py);
                        m_replayPhase = 2;
                    } else {
                        m_replayPhase = 4;
                    }
                    break;
                case 2:
                    m_out->PenDown();
                    m_realDown = true;
                    m_replayPhase = 3;
                    break;
                case 3:  /* draw */
                    if (next(&px, &py)) {
                        m_out->MoveTo(px, py);
                    } else {
                        m_replayPhase = 4;
                    }
                    break;
                default:  /* lift, unless this is a spilled stroke still being drawn */
                    if (!(m_replayRaw && m_replayStep + 1 == m_stepCount && m_down && m_inStroke)) {
                        if (m_realDown) {
                            m_out->PenUp();
                            m_realDown = false;
                        }
                    }
                    m_replayStep++;
                    m_replayPhase = 0;
                    break;
            }
            continue;
        }
        /* Done: leave the plotter where the interpreter believes it is. */
        if (m_pen != m_curPen) {
            m_out->SelectPen(m_pen);
            m_curPen = m_pen;
        }
        if (m_velocity != m_curVelocity) {
            m_out->SetVelocity(m_velocity);
            m_curVelocity = m_velocity;
        }
        if (!m_down) {
            if (m_realDown) {
                m_out->PenUp();
                m_realDown = false;
            }
            /* The replay ended elsewhere than the job as received did; later relative moves
             * and the next job start from where the interpreter believes the pen is. */
            m_out->MoveTo(m_x, m_y);
        }
        if (m_passRest) {
            m_mode = M_PASS;  /* spilled: the rest of the job streams through */
        } else if (m_pageEndPending) {
            m_out->PageEnd();
            ResetBuffer();
            m_mode = M_COLLECT;  /* next page of the same job */
        } else {
            ResetBuffer();
            m_mode = M_PASS;
        }
        return;
    }
}
