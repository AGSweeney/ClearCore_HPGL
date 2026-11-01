/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Hpgl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double kEps = 1e-6;
static const char kEtx = 0x03;
static const double kDefaultChordDeg = 5.0;
static const double kMinChordDeg = 0.5;
/* Chords shorter than this (0.2 mm) only add tiny planner segments; on a 1 mm circle the
 * shape error at this length is under 0.01 mm. */
static const double kMinChordPu = 8.0;

static bool IsLetter(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char Upper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
}

static bool SamePoint(double ax, double ay, double bx, double by) {
    return fabs(ax - bx) < kEps && fabs(ay - by) < kEps;
}

/* Integers print without a fraction; anything else with up to 4 decimals. */
static void FormatNumber(char *out, size_t len, double v) {
    const double r = floor(v + 0.5);
    if (fabs(v - r) < 1e-6) {
        snprintf(out, len, "%ld", (long)r);
        return;
    }
    snprintf(out, len, "%.4f", v);
    char *end = out + strlen(out) - 1;
    while (end > out && *end == '0') {
        *end-- = '\0';
    }
    if (end > out && *end == '.') {
        *end = '\0';
    }
}

static long RoundToLong(double v) {
    return (long)floor(v + 0.5);
}

HpglInterpreter::HpglInterpreter()
    : m_sink(nullptr),
      m_hardW(0),
      m_hardH(0),
      m_state(PS_IDLE),
      m_m1(0),
      m_paramCount(0),
      m_paramsSeen(0),
      m_coordInstr(false),
      m_numLen(0),
      m_pjlMatch(0),
      m_labelTerm(kEtx),
      m_labelTermPrint(true),
      m_labelLastWasTerm(false),
      m_peBase64(true),
      m_peAbsNext(false),
      m_pePenUpNext(false),
      m_peFlag(0),
      m_peAcc(0),
      m_peMult(1),
      m_peFracBits(0),
      m_peHaveX(false),
      m_peX(0),
      m_absolute(true),
      m_penDown(false),
      m_physDown(false),
      m_scaleOn(false),
      m_scaleType(0),
      m_scLeft(50),
      m_scBottom(50),
      m_kx(1),
      m_ky(1),
      m_ux0(0),
      m_uy0(0),
      m_windowOn(false),
      m_rotation(0),
      m_chordDeg(kDefaultChordDeg),
      m_chordMode(0),
      m_tickPos(0.5),
      m_tickNeg(0.5),
      m_pen(1),
      m_error(HPGL_ERR_NONE),
      m_initialized(true),
      m_p1p2Changed(false),
      m_labelsSkipped(0),
      m_unknownCount(0),
      m_instructionCount(0),
      m_opHead(0),
      m_opCount(0),
      m_shapeSavedPen(false),
      m_arcStep(0),
      m_arcSteps(0),
      m_arcR(0),
      m_arcStart(0) {
    m_mnemonic[0] = m_mnemonic[1] = m_mnemonic[2] = '\0';
    m_num[0] = '\0';
    m_ident[0] = '\0';
    m_pos.x = m_pos.y = 0;
    m_physPos = m_pos;
    m_p1 = m_p2 = m_pos;
    m_scOrigin = m_pos;
    m_winLo = m_winHi = m_pos;
    for (int i = 0; i < 4; i++) {
        m_sc[i] = 0;
    }
}

void HpglInterpreter::Init(HpglSink *sink, double hardWidthPu, double hardHeightPu,
                           const char *ident) {
    m_sink = sink;
    m_hardW = hardWidthPu;
    m_hardH = hardHeightPu;
    SetIdentification(ident);
    m_state = PS_IDLE;
    m_opCount = 0;
    m_opHead = 0;
    m_pos.x = m_pos.y = 0;
    m_physPos = m_pos;
    m_physDown = false;
    m_penDown = false;
    DefaultsIN();
    m_error = HPGL_ERR_NONE;
}

void HpglInterpreter::SetHardClip(double widthPu, double heightPu) {
    m_hardW = widthPu;
    m_hardH = heightPu;
}

void HpglInterpreter::SetIdentification(const char *ident) {
    strncpy(m_ident, ident ? ident : "7475A", sizeof(m_ident) - 1);
    m_ident[sizeof(m_ident) - 1] = '\0';
    m_centerOrigin = CenterOriginModel(m_ident);
}

bool HpglInterpreter::CenterOriginModel(const char *ident) {
    return ident && ident[0] == '7' && ident[1] == '5' && (ident[2] == '8' || ident[2] == '9');
}

HpglInterpreter::Pt HpglInterpreter::Origin() const {
    Pt o;
    o.x = m_centerOrigin ? LogicalWidth() / 2.0 : 0.0;
    o.y = m_centerOrigin ? LogicalHeight() / 2.0 : 0.0;
    return o;
}

void HpglInterpreter::Initialize() {
    m_opCount = 0;
    m_opHead = 0;
    DefaultsIN();
}

/* ------------------------------------------------------------------------- */
/* Defaults                                                                   */
/* ------------------------------------------------------------------------- */

void HpglInterpreter::DefaultsDF() {
    m_absolute = true;
    m_scaleOn = false;
    m_scaleType = 0;
    m_windowOn = false;
    m_chordDeg = kDefaultChordDeg;
    m_chordMode = 0;
    m_tickPos = 0.5;
    m_tickNeg = 0.5;
    m_labelTerm = kEtx;
    m_labelTermPrint = true;
    RecalcScale();
    if (m_sink) {
        m_sink->SetVelocity(0);
    }
}

void HpglInterpreter::DefaultsIN() {
    LogicalPenUp();
    m_rotation = 0;
    DefaultP1P2();
    DefaultsDF();
    m_error = HPGL_ERR_NONE;
    m_initialized = true;
    m_p1p2Changed = false;
}

void HpglInterpreter::DefaultP1P2() {
    m_p1.x = 0;
    m_p1.y = 0;
    m_p2.x = LogicalWidth();
    m_p2.y = LogicalHeight();
    RecalcScale();
}

double HpglInterpreter::LogicalWidth() const {
    return (m_rotation == 90 || m_rotation == 270) ? m_hardH : m_hardW;
}

double HpglInterpreter::LogicalHeight() const {
    return (m_rotation == 90 || m_rotation == 270) ? m_hardW : m_hardH;
}

/* ------------------------------------------------------------------------- */
/* Coordinate transforms                                                      */
/* ------------------------------------------------------------------------- */

void HpglInterpreter::RecalcScale() {
    m_kx = 1;
    m_ky = 1;
    m_ux0 = 0;
    m_uy0 = 0;
    m_scOrigin.x = 0;
    m_scOrigin.y = 0;
    if (!m_scaleOn) {
        return;
    }
    const double dxp = m_p2.x - m_p1.x;
    const double dyp = m_p2.y - m_p1.y;
    if (m_scaleType == 2) {
        /* SC xmin, xfactor, ymin, yfactor, 2: user unit = factor plotter units. */
        m_ux0 = m_sc[0];
        m_kx = m_sc[1];
        m_uy0 = m_sc[2];
        m_ky = m_sc[3];
        m_scOrigin = m_p1;
        return;
    }
    const double dxu = m_sc[1] - m_sc[0];
    const double dyu = m_sc[3] - m_sc[2];
    m_ux0 = m_sc[0];
    m_uy0 = m_sc[2];
    m_kx = dxp / dxu;
    m_ky = dyp / dyu;
    m_scOrigin = m_p1;
    if (m_scaleType == 1) {
        /* Isotropic: same magnitude on both axes, spare space split by left/bottom %. */
        const double k = fmin(fabs(m_kx), fabs(m_ky));
        const double kx = (m_kx < 0) ? -k : k;
        const double ky = (m_ky < 0) ? -k : k;
        m_scOrigin.x = m_p1.x + (dxp - dxu * kx) * m_scLeft / 100.0;
        m_scOrigin.y = m_p1.y + (dyp - dyu * ky) * m_scBottom / 100.0;
        m_kx = kx;
        m_ky = ky;
    }
}

HpglInterpreter::Pt HpglInterpreter::UserToPu(double ux, double uy) const {
    Pt p;
    if (!m_scaleOn) {
        const Pt o = Origin();
        p.x = ux + o.x;
        p.y = uy + o.y;
        return p;
    }
    p.x = m_scOrigin.x + (ux - m_ux0) * m_kx;
    p.y = m_scOrigin.y + (uy - m_uy0) * m_ky;
    return p;
}

HpglInterpreter::Pt HpglInterpreter::PuToUser(double px, double py) const {
    Pt u;
    if (!m_scaleOn || fabs(m_kx) < 1e-12 || fabs(m_ky) < 1e-12) {
        const Pt o = Origin();
        u.x = px - o.x;
        u.y = py - o.y;
        return u;
    }
    u.x = m_ux0 + (px - m_scOrigin.x) / m_kx;
    u.y = m_uy0 + (py - m_scOrigin.y) / m_ky;
    return u;
}

HpglInterpreter::Pt HpglInterpreter::LogicalToPhysical(Pt p) const {
    Pt q;
    switch (m_rotation) {
        case 90:
            q.x = m_hardW - p.y;
            q.y = p.x;
            break;
        case 180:
            q.x = m_hardW - p.x;
            q.y = m_hardH - p.y;
            break;
        case 270:
            q.x = p.y;
            q.y = m_hardH - p.x;
            break;
        default:
            q = p;
            break;
    }
    return q;
}

HpglInterpreter::Pt HpglInterpreter::PhysicalToLogical(Pt q) const {
    Pt p;
    switch (m_rotation) {
        case 90:
            p.x = q.y;
            p.y = m_hardW - q.x;
            break;
        case 180:
            p.x = m_hardW - q.x;
            p.y = m_hardH - q.y;
            break;
        case 270:
            p.x = m_hardH - q.y;
            p.y = q.x;
            break;
        default:
            p = q;
            break;
    }
    return p;
}

HpglInterpreter::Pt HpglInterpreter::ClampHard(Pt p) const {
    const double w = LogicalWidth();
    const double h = LogicalHeight();
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x > w) p.x = w;
    if (p.y > h) p.y = h;
    return p;
}

HpglInterpreter::Pt HpglInterpreter::CurrentUser() const {
    return PuToUser(m_pos.x, m_pos.y);
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */

void HpglInterpreter::PhysicalPen(bool down) {
    if (down == m_physDown) {
        return;
    }
    m_physDown = down;
    if (!m_sink) {
        return;
    }
    if (down) {
        m_sink->PenDown();
    } else {
        m_sink->PenUp();
    }
}

void HpglInterpreter::PhysicalMove(Pt p) {
    if (SamePoint(p.x, p.y, m_physPos.x, m_physPos.y)) {
        return;
    }
    m_physPos = p;
    if (m_sink) {
        const Pt q = LogicalToPhysical(p);
        m_sink->MoveTo(q.x, q.y);
    }
}

/* The window (when on) intersected with the hard clip; false when they do not overlap. */
bool HpglInterpreter::ClipBox(double *xmin, double *ymin, double *xmax, double *ymax) const {
    *xmin = 0;
    *ymin = 0;
    *xmax = LogicalWidth();
    *ymax = LogicalHeight();
    if (m_windowOn) {
        *xmin = fmax(*xmin, m_winLo.x);
        *ymin = fmax(*ymin, m_winLo.y);
        *xmax = fmin(*xmax, m_winHi.x);
        *ymax = fmin(*ymax, m_winHi.y);
    }
    return !(*xmin > *xmax + kEps || *ymin > *ymax + kEps);
}

/* Liang-Barsky against ClipBox(). */
bool HpglInterpreter::ClipSegment(Pt a, Pt b, double *t0, double *t1) const {
    double xmin, ymin, xmax, ymax;
    if (!ClipBox(&xmin, &ymin, &xmax, &ymax)) {
        return false;
    }
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    double lo = 0, hi = 1;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - xmin, xmax - a.x, a.y - ymin, ymax - a.y};
    for (int i = 0; i < 4; i++) {
        if (fabs(p[i]) < 1e-12) {
            if (q[i] < -kEps) {
                return false;
            }
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > hi) return false;
            if (t > lo) lo = t;
        } else {
            if (t < lo) return false;
            if (t < hi) hi = t;
        }
    }
    *t0 = lo;
    *t1 = hi;
    return true;
}

/* Whole arc inside ClipBox(): its ends and every axis extreme it passes. Logical PU, angles
 * in radians, sweep signed (positive = counter-clockwise). */
bool HpglInterpreter::ArcInsideClip(Pt c, double r, double a0, double sweep) const {
    double xmin, ymin, xmax, ymax;
    if (!ClipBox(&xmin, &ymin, &xmax, &ymax)) {
        return false;
    }
    const double kTwoPi = 2.0 * M_PI;
    for (int k = -2; k < 4; k++) {
        double a;
        if (k == -2) {
            a = a0;
        } else if (k == -1) {
            a = a0 + sweep;
        } else {
            a = k * (M_PI / 2.0);
            /* how far along the sweep this extreme lies */
            double d = (sweep >= 0) ? (a - a0) : (a0 - a);
            d = fmod(d, kTwoPi);
            if (d < 0) d += kTwoPi;
            if (d > fabs(sweep)) {
                continue;
            }
        }
        const double x = c.x + r * cos(a);
        const double y = c.y + r * sin(a);
        if (x < xmin - kEps || x > xmax + kEps || y < ymin - kEps || y > ymax + kEps) {
            return false;
        }
    }
    return true;
}

void HpglInterpreter::LogicalPenUp() {
    m_penDown = false;
    PhysicalPen(false);
}

void HpglInterpreter::LogicalPenDown() {
    m_penDown = true;
    double t0, t1;
    if (!ClipSegment(m_pos, m_pos, &t0, &t1)) {
        return;  /* outside the window: stays up until a segment comes back in */
    }
    if (!SamePoint(m_physPos.x, m_physPos.y, m_pos.x, m_pos.y)) {
        PhysicalPen(false);
        PhysicalMove(m_pos);
    }
    PhysicalPen(true);
}

void HpglInterpreter::LineToPu(Pt target) {
    const Pt start = m_pos;
    m_pos = target;
    if (!m_penDown) {
        PhysicalPen(false);
        PhysicalMove(ClampHard(target));
        return;
    }
    double t0, t1;
    if (!ClipSegment(start, target, &t0, &t1)) {
        PhysicalPen(false);
        return;
    }
    Pt s, e;
    s.x = start.x + (target.x - start.x) * t0;
    s.y = start.y + (target.y - start.y) * t0;
    e.x = start.x + (target.x - start.x) * t1;
    e.y = start.y + (target.y - start.y) * t1;
    if (!SamePoint(m_physPos.x, m_physPos.y, s.x, s.y)) {
        PhysicalPen(false);
        PhysicalMove(s);
    }
    PhysicalPen(true);
    PhysicalMove(e);
    if (t1 < 1.0 - kEps) {
        PhysicalPen(false);  /* left the window: lift at the edge */
    }
}

void HpglInterpreter::LineToUser(double ux, double uy) {
    const Pt p = UserToPu(ux, uy);
    if (fabs(p.x) > 1e9 || fabs(p.y) > 1e9) {
        SetError(HPGL_ERR_POSITION_OVERFLOW);
        return;
    }
    LineToPu(p);
}

void HpglInterpreter::SyncPositionFromSink() {
    if (!m_sink) {
        return;
    }
    double x = 0, y = 0;
    bool down = false;
    m_sink->ActualPosition(&x, &y, &down);
    Pt q;
    q.x = x;
    q.y = y;
    m_pos = PhysicalToLogical(q);
    m_physPos = m_pos;
    m_physDown = down;
    m_penDown = down;
}

/* ------------------------------------------------------------------------- */
/* Shapes                                                                     */
/* ------------------------------------------------------------------------- */

void HpglInterpreter::PushOp(uint8_t type, double x, double y, double a, double b) {
    if (m_opCount >= HPGL_MAX_OPS) {
        return;
    }
    const uint8_t idx = (uint8_t)((m_opHead + m_opCount) % HPGL_MAX_OPS);
    m_ops[idx].type = type;
    m_ops[idx].x = x;
    m_ops[idx].y = y;
    m_ops[idx].a = a;
    m_ops[idx].b = b;
    if (m_opCount == 0) {
        m_arcSteps = 0;
    }
    m_opCount++;
}

void HpglInterpreter::BeginShape() {
    m_shapeSavedPen = m_penDown;
}

double HpglInterpreter::ChordDegrees(double requested) const {
    double deg = (requested > 0) ? requested : m_chordDeg;
    if (deg < kMinChordDeg) deg = kMinChordDeg;
    if (deg > 180) deg = 180;
    return deg;
}

/* A pen-down arc as one sink ArcTo, when a circle in user units is a circle on the paper
 * (same scale magnitude on both axes; RO only rotates) and the whole arc is inside the
 * window and hard clip. Needs m_arcR / m_arcStart. False: draw it as chords. */
bool HpglInterpreter::NativeArc(const Op &op) {
    if (!m_sink || !m_penDown || fabs(op.a) > 360.0 + kEps) {
        return false;
    }
    bool mirror = false;
    if (m_scaleOn) {
        const double ax = fabs(m_kx);
        const double ay = fabs(m_ky);
        if (ax < 1e-12 || fabs(ax - ay) > 1e-9 * ax) {
            return false;  /* anisotropic scaling: the arc is an ellipse */
        }
        mirror = (m_kx < 0) != (m_ky < 0);
    }
    const bool full = fabs(op.a) >= 360.0 - kEps;
    const Pt s = m_pos;
    const Pt c = UserToPu(op.x, op.y);
    Pt e = s;
    if (!full) {
        const double ae = m_arcStart + op.a * M_PI / 180.0;
        e = UserToPu(op.x + m_arcR * cos(ae), op.y + m_arcR * sin(ae));
    }
    if (fabs(c.x) > 1e9 || fabs(c.y) > 1e9 || fabs(e.x) > 1e9 || fabs(e.y) > 1e9) {
        return false;
    }
    const double r = sqrt((s.x - c.x) * (s.x - c.x) + (s.y - c.y) * (s.y - c.y));
    if (r < kEps) {
        return false;
    }
    /* Sweep on the paper: a mirrored scale reverses the direction. */
    const double sweepDeg = (full ? (op.a > 0 ? 360.0 : -360.0) : op.a) * (mirror ? -1.0 : 1.0);
    if (!ArcInsideClip(c, r, atan2(s.y - c.y, s.x - c.x), sweepDeg * M_PI / 180.0)) {
        return false;
    }
    if (!SamePoint(m_physPos.x, m_physPos.y, s.x, s.y)) {
        PhysicalPen(false);
        PhysicalMove(s);
    }
    PhysicalPen(true);
    const Pt cq = LogicalToPhysical(c);
    const Pt eq = LogicalToPhysical(e);
    if (!m_sink->ArcTo(cq.x, cq.y, eq.x, eq.y, sweepDeg)) {
        return false;
    }
    m_pos = e;
    m_physPos = e;
    return true;
}

/* One chord per call (or the whole arc through NativeArc). Returns true when the arc is
 * finished. */
bool HpglInterpreter::StepArc(Op &op) {
    if (m_arcSteps == 0) {
        const Pt start = CurrentUser();
        const double dx = start.x - op.x;
        const double dy = start.y - op.y;
        m_arcR = sqrt(dx * dx + dy * dy);
        if (m_arcR < kEps || fabs(op.a) < kEps) {
            return true;
        }
        m_arcStart = atan2(dy, dx);
        if (NativeArc(op)) {
            return true;
        }
        double chord;
        if (m_chordMode == 1 && op.b > 0) {
            /* CT1: chord parameter is the maximum deviation in user units. */
            const double c = 1.0 - op.b / m_arcR;
            chord = (c <= -1.0) ? 180.0 : 2.0 * acos(c) * 180.0 / M_PI;
            chord = ChordDegrees(chord);
        } else {
            chord = ChordDegrees(op.b);
        }
        const Pt cPu = UserToPu(op.x, op.y);
        const Pt sPu = UserToPu(start.x, start.y);
        const double rPu = sqrt((sPu.x - cPu.x) * (sPu.x - cPu.x) + (sPu.y - cPu.y) * (sPu.y - cPu.y));
        if (rPu > kEps) {
            double minDeg = (kMinChordPu / rPu) * 180.0 / M_PI;
            if (minDeg > 90.0) {
                minDeg = 90.0;  /* keep at least a square for very small circles */
            }
            if (chord < minDeg) {
                chord = minDeg;
            }
        }
        m_arcSteps = (uint32_t)ceil(fabs(op.a) / chord - 1e-9);
        if (m_arcSteps < 1) m_arcSteps = 1;
        m_arcStep = 0;
    }
    m_arcStep++;
    const double ang = m_arcStart + (op.a * M_PI / 180.0) * (double)m_arcStep / (double)m_arcSteps;
    LineToUser(op.x + m_arcR * cos(ang), op.y + m_arcR * sin(ang));
    if (m_arcStep >= m_arcSteps) {
        m_arcSteps = 0;
        return true;
    }
    return false;
}

void HpglInterpreter::Pump() {
    while (m_opCount > 0) {
        if (m_sink && m_sink->Room() <= kRoomReserve) {
            return;
        }
        Op &op = m_ops[m_opHead];
        bool done = true;
        switch (op.type) {
            case OP_PEN_UP:
                LogicalPenUp();
                break;
            case OP_PEN_DOWN:
                LogicalPenDown();
                break;
            case OP_LINE:
                LineToUser(op.x, op.y);
                break;
            case OP_ARC:
                done = StepArc(op);
                break;
            case OP_RESTORE:
                if (m_shapeSavedPen) {
                    LogicalPenDown();
                } else {
                    LogicalPenUp();
                }
                break;
            default:
                break;
        }
        if (done) {
            m_opHead = (uint8_t)((m_opHead + 1) % HPGL_MAX_OPS);
            m_opCount--;
            m_arcSteps = 0;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Parser                                                                     */
/* ------------------------------------------------------------------------- */

bool HpglInterpreter::AtInstructionBoundary() const {
    return m_state == PS_IDLE && m_opCount == 0;
}

void HpglInterpreter::AbortInstruction() {
    m_state = PS_IDLE;
    m_numLen = 0;
    m_paramCount = 0;
    m_paramsSeen = 0;
    m_opCount = 0;
    m_opHead = 0;
    m_arcSteps = 0;
}

void HpglInterpreter::SetError(int err) {
    m_error = err;
}

uint8_t HpglInterpreter::StatusByte() const {
    uint8_t s = HPGL_OS_READY;
    if (m_penDown) s |= HPGL_OS_PEN_DOWN;
    if (m_p1p2Changed) s |= HPGL_OS_P1P2_CHANGED;
    if (m_initialized) s |= HPGL_OS_INITIALIZED;
    if (m_error != HPGL_ERR_NONE) s |= HPGL_OS_ERROR;
    return s;
}

void HpglInterpreter::Feed(uint8_t c) {
    switch (m_state) {
        case PS_IDLE:
            if (IsLetter(c)) {
                m_m1 = Upper(c);
                m_state = PS_MNEMONIC2;
            } else if (c == '@') {
                m_pjlMatch = 0;
                m_state = PS_PJL;
            }
            /* separators, terminators, whitespace and stray bytes are ignored */
            return;

        case PS_MNEMONIC2:
            if (IsLetter(c)) {
                m_mnemonic[0] = m_m1;
                m_mnemonic[1] = Upper(c);
                m_mnemonic[2] = '\0';
                BeginInstruction();
            } else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
                /* tolerate whitespace inside a mnemonic */
            } else {
                m_unknownCount++;
                SetError(HPGL_ERR_UNKNOWN_INSTRUCTION);
                m_state = PS_IDLE;
                Feed(c);
            }
            return;

        case PS_PJL:
            /* "@PJL ..." lines from Windows drivers: skip to end of line. */
            if (c == '\n') {
                m_state = PS_IDLE;
            }
            return;

        case PS_LABEL:
            if ((char)c == m_labelTerm) {
                m_state = PS_IDLE;
            }
            return;

        case PS_DT:
            if (c == ';') {
                m_labelTerm = kEtx;
                m_labelTermPrint = true;
                m_state = PS_IDLE;
                return;
            }
            if (c == 0 || c == '\n' || c == 0x1b) {
                SetError(HPGL_ERR_BAD_PARAM);
                m_state = PS_IDLE;
                return;
            }
            m_labelTerm = (char)c;
            m_labelTermPrint = true;
            /* optional ",mode" follows as an ordinary parameter */
            m_paramCount = 0;
            m_paramsSeen = 0;
            m_numLen = 0;
            m_state = PS_PARAMS;
            return;

        case PS_STRING:
            if (c == '"') {
                if (m_paramCount < HPGL_MAX_PARAMS) {
                    m_params[m_paramCount++] = 0;
                }
                m_paramsSeen++;
                m_state = PS_PARAMS;
            }
            return;

        case PS_PE:
            FeedPe(c);
            return;

        case PS_PARAMS:
            break;
    }

    /* PS_PARAMS */
    if ((c >= '0' && c <= '9')) {
        if (m_numLen < sizeof(m_num) - 1) {
            m_num[m_numLen++] = (char)c;
        }
        return;
    }
    if (c == '+' || c == '-') {
        EndNumber();
        m_num[m_numLen++] = (char)c;
        return;
    }
    if (c == '.') {
        if (memchr(m_num, '.', m_numLen) != nullptr) {
            EndNumber();
        }
        if (m_numLen < sizeof(m_num) - 1) {
            m_num[m_numLen++] = '.';
        }
        return;
    }
    if (c == ',' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
        EndNumber();
        return;
    }
    if (c == '"') {
        EndNumber();
        m_state = PS_STRING;
        return;
    }
    if (c == ';') {
        EndNumber();
        EndInstruction();
        return;
    }
    if (IsLetter(c)) {
        EndNumber();
        EndInstruction();
        Feed(c);
        return;
    }
    /* other bytes inside parameters are ignored */
}

void HpglInterpreter::BeginInstruction() {
    m_instructionCount++;
    m_paramCount = 0;
    m_paramsSeen = 0;
    m_numLen = 0;
    const char *m = m_mnemonic;
    m_coordInstr = false;
    if (strcmp(m, "LB") == 0) {
        m_labelsSkipped++;
        m_state = PS_LABEL;
        return;
    }
    if (strcmp(m, "DT") == 0) {
        m_state = PS_DT;
        return;
    }
    if (strcmp(m, "PE") == 0) {
        m_peBase64 = true;
        m_peAbsNext = false;
        m_pePenUpNext = false;
        m_peFlag = 0;
        m_peAcc = 0;
        m_peMult = 1;
        m_peFracBits = 0;
        m_peHaveX = false;
        m_state = PS_PE;
        return;
    }
    m_state = PS_PARAMS;
    if (strcmp(m, "PA") == 0) {
        m_absolute = true;
        m_coordInstr = true;
    } else if (strcmp(m, "PR") == 0) {
        m_absolute = false;
        m_coordInstr = true;
    } else if (strcmp(m, "PU") == 0) {
        LogicalPenUp();
        m_coordInstr = true;
    } else if (strcmp(m, "PD") == 0) {
        LogicalPenDown();
        m_coordInstr = true;
    }
}

void HpglInterpreter::EndNumber() {
    if (m_numLen == 0) {
        return;
    }
    m_num[m_numLen] = '\0';
    m_numLen = 0;
    const bool onlySign = (m_num[0] == '+' || m_num[0] == '-') && m_num[1] == '\0';
    if (onlySign || (m_num[0] == '.' && m_num[1] == '\0')) {
        return;
    }
    const double v = strtod(m_num, nullptr);
    m_paramsSeen++;
    if (m_paramCount < HPGL_MAX_PARAMS) {
        m_params[m_paramCount++] = v;
    }
    if (m_coordInstr && m_paramCount == 2) {
        ExecuteCoordPair(m_params[0], m_params[1]);
        m_paramCount = 0;
    }
}

void HpglInterpreter::ExecuteCoordPair(double x, double y) {
    if (m_absolute) {
        LineToUser(x, y);
    } else {
        const Pt u = CurrentUser();
        LineToUser(u.x + x, u.y + y);
    }
}

void HpglInterpreter::EndInstruction() {
    m_state = PS_IDLE;
    if (m_coordInstr) {
        if (m_paramCount == 1) {
            SetError(HPGL_ERR_PARAM_COUNT);
        }
        m_paramCount = 0;
        return;
    }
    Execute();
    Pump();
}

void HpglInterpreter::ReplyF(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (m_sink) {
        m_sink->Reply(buf);
    }
}

static bool Is(const char *m, const char *name) {
    return m[0] == name[0] && m[1] == name[1];
}

void HpglInterpreter::Execute() {
    const char *m = m_mnemonic;
    const double *p = m_params;
    const uint8_t n = m_paramCount;

    /* ---- setup / coordinate system ---- */
    if (Is(m, "IN")) {
        DefaultsIN();
        return;
    }
    if (Is(m, "DF")) {
        DefaultsDF();
        return;
    }
    if (Is(m, "IP")) {
        if (n == 0) {
            DefaultP1P2();
        } else if (n == 2) {
            const Pt o = Origin();
            const double dx = m_p2.x - m_p1.x;
            const double dy = m_p2.y - m_p1.y;
            m_p1.x = p[0] + o.x;
            m_p1.y = p[1] + o.y;
            m_p2.x = m_p1.x + dx;
            m_p2.y = m_p1.y + dy;
        } else if (n == 4) {
            const Pt o = Origin();
            m_p1.x = p[0] + o.x;
            m_p1.y = p[1] + o.y;
            m_p2.x = p[2] + o.x;
            m_p2.y = p[3] + o.y;
        } else {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        m_p1p2Changed = true;
        RecalcScale();
        return;
    }
    if (Is(m, "IR")) {
        if (n == 0) {
            DefaultP1P2();
        } else if (n == 2 || n == 4) {
            const double w = LogicalWidth();
            const double h = LogicalHeight();
            const double dx = m_p2.x - m_p1.x;
            const double dy = m_p2.y - m_p1.y;
            m_p1.x = w * p[0] / 100.0;
            m_p1.y = h * p[1] / 100.0;
            if (n == 4) {
                m_p2.x = w * p[2] / 100.0;
                m_p2.y = h * p[3] / 100.0;
            } else {
                m_p2.x = m_p1.x + dx;
                m_p2.y = m_p1.y + dy;
            }
        } else {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        m_p1p2Changed = true;
        RecalcScale();
        return;
    }
    if (Is(m, "SC")) {
        if (n == 0) {
            m_scaleOn = false;
            RecalcScale();
            return;
        }
        if (n != 4 && n != 5 && n != 7) {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        const uint8_t type = (n >= 5) ? (uint8_t)p[4] : 0;
        if (type > 2) {
            SetError(HPGL_ERR_BAD_PARAM);
            return;
        }
        if (type == 2 ? (fabs(p[1]) < 1e-12 || fabs(p[3]) < 1e-12)
                      : (fabs(p[1] - p[0]) < 1e-12 || fabs(p[3] - p[2]) < 1e-12)) {
            SetError(HPGL_ERR_BAD_PARAM);
            return;
        }
        for (int i = 0; i < 4; i++) {
            m_sc[i] = p[i];
        }
        m_scaleType = type;
        m_scLeft = (n == 7) ? p[5] : 50.0;
        m_scBottom = (n == 7) ? p[6] : 50.0;
        m_scaleOn = true;
        RecalcScale();
        return;
    }
    if (Is(m, "IW")) {
        if (n == 0) {
            m_windowOn = false;
        } else if (n == 4) {
            const Pt o = Origin();
            m_winLo.x = fmin(p[0], p[2]) + o.x;
            m_winLo.y = fmin(p[1], p[3]) + o.y;
            m_winHi.x = fmax(p[0], p[2]) + o.x;
            m_winHi.y = fmax(p[1], p[3]) + o.y;
            m_windowOn = true;
        } else {
            SetError(HPGL_ERR_PARAM_COUNT);
        }
        return;
    }
    if (Is(m, "RO")) {
        const int rot = (n == 0) ? 0 : (int)p[0];
        if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
            SetError(HPGL_ERR_BAD_PARAM);
            return;
        }
        if (rot == m_rotation) {
            return;
        }
        const Pt physPos = LogicalToPhysical(m_pos);
        const Pt physLast = LogicalToPhysical(m_physPos);
        m_rotation = rot;
        m_pos = PhysicalToLogical(physPos);
        m_physPos = PhysicalToLogical(physLast);
        m_windowOn = false;
        DefaultP1P2();
        m_p1p2Changed = true;
        return;
    }

    /* ---- pen / speed ---- */
    if (Is(m, "SP")) {
        const int pen = (n == 0) ? 0 : (int)p[0];
        if (pen == 0) {
            LogicalPenUp();
        }
        m_pen = pen;
        if (m_sink) {
            m_sink->SelectPen(pen);
        }
        return;
    }
    if (Is(m, "VS")) {
        if (m_sink) {
            m_sink->SetVelocity(n == 0 ? 0 : p[0] * 10.0);
        }
        return;
    }
    if (Is(m, "CT")) {
        m_chordMode = (n == 0) ? 0 : (uint8_t)(p[0] != 0);
        return;
    }
    if (Is(m, "DT")) {
        /* terminator was taken in PS_DT; mode 1 = terminator not printed */
        m_labelTermPrint = (n == 0) || (p[0] == 0);
        return;
    }
    if (Is(m, "TL")) {
        m_tickPos = (n >= 1) ? p[0] : 0.5;
        m_tickNeg = (n >= 2) ? p[1] : ((n >= 1) ? 0 : 0.5);
        return;
    }
    if (Is(m, "PG") || Is(m, "NR") || Is(m, "AF") || Is(m, "AH") || Is(m, "FR")) {
        LogicalPenUp();
        if (m_sink) {
            m_sink->PageEnd();
        }
        return;
    }

    /* ---- arcs, circles, shapes ---- */
    if (Is(m, "AA") || Is(m, "AR")) {
        if (n < 3) {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        Pt c;
        if (Is(m, "AA")) {
            c.x = p[0];
            c.y = p[1];
        } else {
            const Pt u = CurrentUser();
            c.x = u.x + p[0];
            c.y = u.y + p[1];
        }
        PushOp(OP_ARC, c.x, c.y, p[2], n >= 4 ? p[3] : 0);
        return;
    }
    if (Is(m, "CI")) {
        if (n < 1) {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        const Pt c = CurrentUser();
        BeginShape();
        PushOp(OP_PEN_UP);
        PushOp(OP_LINE, c.x + p[0], c.y);
        PushOp(OP_PEN_DOWN);
        PushOp(OP_ARC, c.x, c.y, 360.0, n >= 2 ? p[1] : 0);
        PushOp(OP_PEN_UP);
        PushOp(OP_LINE, c.x, c.y);
        PushOp(OP_RESTORE);
        return;
    }
    if (Is(m, "EA") || Is(m, "ER") || Is(m, "RA") || Is(m, "RR")) {
        /* RA / RR fills are drawn as outlines. */
        if (n < 2) {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        const Pt a = CurrentUser();
        Pt b;
        if (m[1] == 'A') {
            b.x = p[0];
            b.y = p[1];
        } else {
            b.x = a.x + p[0];
            b.y = a.y + p[1];
        }
        BeginShape();
        PushOp(OP_PEN_DOWN);
        PushOp(OP_LINE, b.x, a.y);
        PushOp(OP_LINE, b.x, b.y);
        PushOp(OP_LINE, a.x, b.y);
        PushOp(OP_LINE, a.x, a.y);
        PushOp(OP_RESTORE);
        return;
    }
    if (Is(m, "EW") || Is(m, "WG")) {
        /* WG fills are drawn as outlines. */
        if (n < 3) {
            SetError(HPGL_ERR_PARAM_COUNT);
            return;
        }
        const Pt c = CurrentUser();
        const double r = p[0];
        const double a0 = p[1] * M_PI / 180.0;
        const double chord = n >= 4 ? p[3] : 0;
        const double sx = c.x + r * cos(a0);
        const double sy = c.y + r * sin(a0);
        BeginShape();
        if (fabs(p[2]) >= 360.0 - kEps) {
            PushOp(OP_PEN_UP);
            PushOp(OP_LINE, sx, sy);
            PushOp(OP_PEN_DOWN);
            PushOp(OP_ARC, c.x, c.y, p[2] > 0 ? 360.0 : -360.0, chord);
            PushOp(OP_PEN_UP);
            PushOp(OP_LINE, c.x, c.y);
        } else {
            PushOp(OP_PEN_DOWN);
            PushOp(OP_LINE, sx, sy);
            PushOp(OP_ARC, c.x, c.y, p[2], chord);
            PushOp(OP_LINE, c.x, c.y);
        }
        PushOp(OP_RESTORE);
        return;
    }
    if (Is(m, "XT") || Is(m, "YT")) {
        const bool x = Is(m, "XT");
        const double span = x ? (m_p2.y - m_p1.y) : (m_p2.x - m_p1.x);
        const double up = fabs(span) * m_tickPos / 100.0;
        const double dn = fabs(span) * m_tickNeg / 100.0;
        const Pt o = m_pos;
        const Pt hi = x ? PuToUser(o.x, o.y + up) : PuToUser(o.x + up, o.y);
        const Pt lo = x ? PuToUser(o.x, o.y - dn) : PuToUser(o.x - dn, o.y);
        const Pt back = PuToUser(o.x, o.y);
        BeginShape();
        PushOp(OP_PEN_UP);
        PushOp(OP_LINE, hi.x, hi.y);
        PushOp(OP_PEN_DOWN);
        PushOp(OP_LINE, lo.x, lo.y);
        PushOp(OP_PEN_UP);
        PushOp(OP_LINE, back.x, back.y);
        PushOp(OP_RESTORE);
        return;
    }

    /* ---- output instructions ---- */
    if (Is(m, "OI")) {
        ReplyF("%s", m_ident);
        return;
    }
    if (Is(m, "OS")) {
        ReplyF("%u", (unsigned)StatusByte());
        m_initialized = false;
        m_p1p2Changed = false;
        return;
    }
    if (Is(m, "OE")) {
        ReplyF("%d", m_error);
        m_error = HPGL_ERR_NONE;
        return;
    }
    if (Is(m, "OA") || Is(m, "OD")) {
        double x = 0, y = 0;
        bool down = false;
        if (m_sink) {
            m_sink->ActualPosition(&x, &y, &down);
        }
        Pt q;
        q.x = x;
        q.y = y;
        const Pt l = PhysicalToLogical(q);
        const Pt o = Origin();
        ReplyF("%ld,%ld,%d", RoundToLong(l.x - o.x), RoundToLong(l.y - o.y), down ? 1 : 0);
        return;
    }
    if (Is(m, "OC")) {
        const Pt u = CurrentUser();
        char xs[24], ys[24];
        FormatNumber(xs, sizeof(xs), u.x);
        FormatNumber(ys, sizeof(ys), u.y);
        ReplyF("%s,%s,%d", xs, ys, m_penDown ? 1 : 0);
        return;
    }
    if (Is(m, "OP")) {
        const Pt o = Origin();
        ReplyF("%ld,%ld,%ld,%ld", RoundToLong(m_p1.x - o.x), RoundToLong(m_p1.y - o.y),
               RoundToLong(m_p2.x - o.x), RoundToLong(m_p2.y - o.y));
        m_p1p2Changed = false;
        return;
    }
    if (Is(m, "OH")) {
        const Pt o = Origin();
        ReplyF("%ld,%ld,%ld,%ld", RoundToLong(-o.x), RoundToLong(-o.y), RoundToLong(LogicalWidth() - o.x),
               RoundToLong(LogicalHeight() - o.y));
        return;
    }
    if (Is(m, "OW")) {
        const Pt o = Origin();
        if (m_windowOn) {
            ReplyF("%ld,%ld,%ld,%ld", RoundToLong(fmax(0, m_winLo.x) - o.x), RoundToLong(fmax(0, m_winLo.y) - o.y),
                   RoundToLong(fmin(LogicalWidth(), m_winHi.x) - o.x),
                   RoundToLong(fmin(LogicalHeight(), m_winHi.y) - o.y));
        } else {
            ReplyF("%ld,%ld,%ld,%ld", RoundToLong(-o.x), RoundToLong(-o.y), RoundToLong(LogicalWidth() - o.x),
                   RoundToLong(LogicalHeight() - o.y));
        }
        return;
    }
    if (Is(m, "OF")) {
        ReplyF("40,40");
        return;
    }
    if (Is(m, "OO")) {
        /* 7475A option list: no digitizer, single pen, no buffer expansion. */
        ReplyF("0,1,0,0,1,0,0,0");
        return;
    }
    if (Is(m, "OT")) {
        ReplyF("%d,1", m_pen);
        return;
    }
    if (Is(m, "OG") || Is(m, "OL")) {
        ReplyF("0");
        return;
    }

    /* ---- accepted and ignored: line/fill types, label setup, HP-GL/2 job control ---- */
    static const char *const kIgnored[] = {
        "LT", "PT", "FT", "LA", "PW", "WU", "NP", "PC", "CR", "TR", "MC", "PP", "CO",
        "BP", "PS", "QL", "RP", "MG", "WD", "KY", "GC", "GM", "IM", "DC", "DP", "SM",
        "SS", "SA", "SI", "SR", "SL", "DI", "DR", "CS", "CA", "ES", "LO", "CP", "UC",
        "DS", "BL", "PB", "AD", "SD", "TD", "LM", "SB", "SV", "TR", "UL", "AC", "EC",
        "MT", "CV", "BF", "EP", "FP", "PM", "RF", "BZ", "BR", "ST", "NS", "FS", "DL",
    };
    for (size_t i = 0; i < sizeof(kIgnored) / sizeof(kIgnored[0]); i++) {
        if (Is(m, kIgnored[i])) {
            return;
        }
    }
    m_unknownCount++;
    SetError(HPGL_ERR_UNKNOWN_INSTRUCTION);
}

/* ------------------------------------------------------------------------- */
/* PE (HP-GL/2 polyline encoded)                                              */
/* ------------------------------------------------------------------------- */

void HpglInterpreter::FeedPe(uint8_t c) {
    if (c == ';') {
        m_state = PS_IDLE;
        return;
    }
    if (c <= ' ') {
        return;
    }
    if (c == ':' || c == '>') {
        m_peFlag = c;
        m_peAcc = 0;
        m_peMult = 1;
        return;
    }
    if (c == '<') {
        m_pePenUpNext = true;
        return;
    }
    if (c == '=') {
        m_peAbsNext = true;
        return;
    }
    if (c == '7') {
        m_peBase64 = false;
        return;
    }
    bool done = false;
    uint32_t digit = 0;
    if (m_peBase64) {
        if (c >= 63 && c <= 126) {
            digit = (uint32_t)(c - 63);
        } else if (c >= 191 && c <= 254) {
            digit = (uint32_t)(c - 191);
            done = true;
        } else {
            return;
        }
    } else {
        if (c >= 63 && c <= 94) {
            digit = (uint32_t)(c - 63);
        } else if (c >= 95 && c <= 126) {
            digit = (uint32_t)(c - 95);
            done = true;
        } else {
            return;
        }
    }
    m_peAcc += digit * m_peMult;
    m_peMult *= m_peBase64 ? 64u : 32u;
    if (!done) {
        return;
    }
    const int32_t mag = (int32_t)(m_peAcc >> 1);
    const int32_t value = (m_peAcc & 1u) ? -mag : mag;
    m_peAcc = 0;
    m_peMult = 1;
    PeValueDone(value);
}

void HpglInterpreter::PeValueDone(int32_t value) {
    if (m_peFlag == ':') {
        m_peFlag = 0;
        if (value == 0) {
            LogicalPenUp();
        }
        m_pen = value;
        if (m_sink) {
            m_sink->SelectPen(value);
        }
        return;
    }
    if (m_peFlag == '>') {
        m_peFlag = 0;
        m_peFracBits = (uint8_t)((value < 0) ? 0 : (value > 26 ? 26 : value));
        return;
    }
    const double d = (double)value / (double)(1u << m_peFracBits);
    if (!m_peHaveX) {
        m_peX = d;
        m_peHaveX = true;
        return;
    }
    m_peHaveX = false;
    double tx = m_peX;
    double ty = d;
    if (!m_peAbsNext) {
        const Pt u = CurrentUser();
        tx += u.x;
        ty += u.y;
    }
    if (m_pePenUpNext) {
        LogicalPenUp();
    } else if (!m_penDown) {
        LogicalPenDown();
    }
    LineToUser(tx, ty);
    m_peAbsNext = false;
    m_pePenUpNext = false;
}
