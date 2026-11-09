/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Plotter.h"

#include "ClearCore.h"
#include "CoordinatedMotionController.h"
#include "PlotterConfig.h"
#include "Settings.h"
#include "Switches.h"
#include "Transport.h"

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum ActionType : uint8_t {
    ACT_LINE,
    ACT_PEN_UP,
    ACT_PEN_DOWN,
    ACT_ARC_CW,      /* x, y = end; the next slot is ACT_ARC_CENTER */
    ACT_ARC_CCW,
    ACT_ARC_CENTER,  /* x, y = centre of the arc before it */
};

struct Action {
    uint8_t type;
    int32_t x;
    int32_t y;
    uint32_t speed;
};

static const uint32_t kPenZTimeoutMs = 5000;
/* Arcs smaller than this (steps) are left to chords. */
static const double kMinArcRadiusSteps = 4.0;

static Action g_actions[CHPGL_ACTION_QUEUE];
static uint16_t g_head = 0;
static uint16_t g_count = 0;

static CoordinatedMotionController g_xy;
static bool g_ready = false;
static bool g_enabled = false;
static bool g_paused = false;
static bool g_abortStop = false;
static bool g_jobAbort = false;
static const char *g_fault = nullptr;

static double g_stepsPerPu[2] = {4.0, 4.0};
static uint32_t g_drawSps = 1;
static uint32_t g_travelSps = 1;
/* Path acceleration in steps/s^2, for the curvature speed cap. */
static double g_accelSteps = 1.0;

/* Direction and length of the last queued segment. XY stops at every pen change, so a
 * pen action (or a resync) starts a new run with no previous direction. */
static bool g_prevDirValid = false;
static double g_prevDirX = 0.0;
static double g_prevDirY = 0.0;
static double g_prevLen = 0.0;
static uint32_t g_curveCapped = 0;

/* Queue tail: pen state and position after the last queued action. */
static bool g_queuedPenDown = false;
static int32_t g_tailX = 0;
static int32_t g_tailY = 0;
static uint32_t g_lastPushMs = 0;

/* Pen hardware. */
static bool g_physPenDown = false;
static bool g_penBusy = false;
static bool g_penTarget = false;
static bool g_penWaitZ = false;
static uint32_t g_penStartMs = 0;
static uint32_t g_penSettleMs = 0;
static bool g_resumeLower = false;
static int g_pen = 1;

static PlotterReplyFn g_reply = nullptr;
static void StartPen(bool down);
static uint32_t g_segments = 0;
static uint32_t g_dropped = 0;
static uint32_t g_arcs = 0;
static uint32_t g_arcFallbacks = 0;
static uint32_t g_pages = 0;

/* ------------------------------------------------------------------------- */
/* Hardware helpers                                                           */
/* ------------------------------------------------------------------------- */

static Connector *PenConnector() {
    switch (g_settings.penIo) {
        case 0: return &ConnectorIO0;
        case 1: return &ConnectorIO1;
        case 2: return &ConnectorIO2;
        case 3: return &ConnectorIO3;
        case 4: return &ConnectorIO4;
        case 5: return &ConnectorIO5;
        default: return nullptr;
    }
}

static void SetPenOutput(bool down) {
    Connector *c = PenConnector();
    if (!c) {
        return;
    }
    const bool on = down ? (g_settings.penActiveHigh != 0) : (g_settings.penActiveHigh == 0);
    c->State(on ? 1 : 0);
}

static void InitPenHardware() {
    if (g_settings.penMode == PEN_MODE_OUTPUT) {
        Connector *c = PenConnector();
        if (c) {
            c->Mode(Connector::OUTPUT_DIGITAL);
        }
        SetPenOutput(false);
    }
    g_physPenDown = false;
}

/* Same read as ClearCNC: State() non-zero = input ON. */
static bool EstopTripped() {
    if (g_settings.estopDi6 == 0) {
        return false;
    }
    const bool on = ConnectorDI6.State() != 0;
    return (g_settings.estopDi6 == 1) ? !on : on;
}

static bool XyIdle() {
    return !g_xy.IsActive() && g_xy.MotionQueueCount() == 0;
}

static bool UsesZ() {
    return g_settings.penMode == PEN_MODE_Z_AXIS;
}

static void ClearActions() {
    g_head = 0;
    g_count = 0;
}

static void SyncTailFromMotion() {
    g_tailX = g_xy.CurrentX();
    g_tailY = g_xy.CurrentY();
    g_queuedPenDown = g_physPenDown;
    g_prevDirValid = false;
}

/* Plotter axes (HP-GL X and Y in steps, signs from dir_x / dir_y) to the motion controller's X
 * and Y, and back. swap_xy exchanges them. With dir_x and dir_y that gives all eight ways a
 * machine can be built or wired: in CoreXY, exchanging motors A and B mirrors Y, and reversing
 * one motor exchanges X and Y and mirrors both. The controller then maps its X/Y onto the
 * motors (Cartesian: M0 = X, M1 = Y; CoreXY: M0 = A = X + Y, M1 = B = X - Y). Everything above
 * this (queue tail, curvature, arcs) works in controller coordinates; the exchange keeps
 * lengths and angles, and only mirrors the direction of arcs. */
static bool g_swapXy = false;

static void AxisToCtrl(int32_t ax, int32_t ay, int32_t *cx, int32_t *cy) {
    *cx = g_swapXy ? ay : ax;
    *cy = g_swapXy ? ax : ay;
}

static void CtrlToAxis(int32_t cx, int32_t cy, int32_t *ax, int32_t *ay) {
    *ax = g_swapXy ? cy : cx;
    *ay = g_swapXy ? cx : cy;
}

/* Head position in plotter units of each HP-GL axis. */
static void HeadPu(double *xPu, double *yPu) {
    int32_t ax, ay;
    CtrlToAxis(g_xy.CurrentX(), g_xy.CurrentY(), &ax, &ay);
    *xPu = (double)ax / g_stepsPerPu[0];
    *yPu = (double)ay / g_stepsPerPu[1];
}

/* The present position becomes 0,0, and the motors' step counters restart at 0 so that diag can
 * compare them with the head position. Motors stopped and nothing queued. */
/* ------------------------------------------------------------------------- */
/* Switches (sampled in the step interrupt)                                    */
/* ------------------------------------------------------------------------- */

/* The switches work in HP-GL axis steps: + is HP-GL +, whatever dir_x / dir_y / swap_xy say. */
static SwitchMonitor g_sw;
static int8_t g_axSign[2] = {1, 1};
static double g_axSpm[2] = {1.0, 1.0};   /* |steps per mm| */
static uint32_t g_rampDecel = 1;         /* limit and abort stops: 3 x accel, steps/s^2 */
static uint32_t g_stopDecel = 1;         /* jog release, homing stop: accel */
static Connector *g_inputs[kInputCodeMax + 1] = {nullptr};
static volatile bool g_swHold = false;   /* the position is being set: skip a sample */
static bool g_positionChanged = false;

static Connector *InputConnector(int code) {
    switch (code) {
        case 0: return &ConnectorIO0;
        case 1: return &ConnectorIO1;
        case 2: return &ConnectorIO2;
        case 3: return &ConnectorIO3;
        case 4: return &ConnectorIO4;
        case 5: return &ConnectorIO5;
        case 7: return &ConnectorDI7;
        case 8: return &ConnectorDI8;
        case 9: return &ConnectorA9;
        case 10: return &ConnectorA10;
        case 11: return &ConnectorA11;
        case 12: return &ConnectorA12;
        default: return nullptr;
    }
}

/* Every sample, before the motors refresh: a stop asked for here is taken in this same sample. */
static void SampleSwitches() {
    if (g_swHold) {
        return;
    }
    uint16_t levels = 0;
    for (int c = 0; c <= kInputCodeMax; c++) {
        Connector *k = g_inputs[c];
        if (k && k->State()) {
            levels |= (uint16_t)(1u << c);
        }
    }
    int32_t ax, ay;
    CtrlToAxis(g_xy.CurrentX(), g_xy.CurrentY(), &ax, &ay);
    const bool idle = !g_xy.IsActive() && g_xy.MotionQueueCount() == 0;
    if (g_sw.Sample(levels, ax * g_axSign[0], ay * g_axSign[1], idle) && g_enabled) {
        g_xy.StopRamped(g_rampDecel);
    }
}

/* Head position, HP-GL axis steps (+ = HP-GL +). */
static int32_t HeadSteps(int axis) {
    int32_t ax, ay;
    CtrlToAxis(g_xy.CurrentX(), g_xy.CurrentY(), &ax, &ay);
    return (axis ? ay : ax) * g_axSign[axis];
}

/* The head is now at x, y (HP-GL axis steps). Stopped, nothing queued. The motors' step counters
 * follow, so diag still compares; the switch monitor's machine position does not move. */
static void SetHeadSteps(int32_t x, int32_t y) {
    const int32_t shiftX = x - HeadSteps(0);
    const int32_t shiftY = y - HeadSteps(1);
    int32_t cx, cy, a, b;
    AxisToCtrl(x * g_axSign[0], y * g_axSign[1], &cx, &cy);
    Kinematics::ToMotor(g_xy.Kinematics(), cx, cy, a, b);
    g_swHold = true;
    g_xy.SetPosition(cx, cy);
    g_sw.Shift(shiftX, shiftY);
    g_swHold = false;
    ConnectorM0.PositionRefSet(a);
    ConnectorM1.PositionRefSet(b);
    SyncTailFromMotion();
    g_positionChanged = true;
}

static void ZeroPosition() {
    SetHeadSteps(0, 0);
}

/* Jog and homing state. */
enum HomePhase : uint8_t { HOME_NONE, HOME_PEN, HOME_RUN, HOME_HARD_OFF, HOME_HARD_WAIT };
static HomePhase g_homePhase = HOME_NONE;
static uint8_t g_homeAxes = 0;
static uint8_t g_homed = 0;
static uint32_t g_homeMs = 0;
static uint32_t g_homeEnds[2] = {0, 0};
static const char *g_homeLast = "none";
static HomingSequencer g_seq;

static bool g_jogOn = false;        /* a jog move is running */
static bool g_jogStopping = false;  /* released: ramping down */
static int g_jogAxis = 0;
static double g_jogV = 0.0;
static uint32_t g_jogUntil = 0;
static int8_t g_jogStoppedInput = -1;  /* a stop-on input stopped it; refused until jog stop */
static int8_t g_jogLimit = -1;         /* the limit that stopped the last jog */
static bool g_pendingJog = false;
static int g_pendingAxis = 0;
static double g_pendingV = 0.0;
static uint16_t g_pendingStopOn = 0;

/* Limit stops, per switch: how many, where the switch tripped (mm). */
static uint32_t g_trips[SW_COUNT] = {0};
static double g_tripAtMm[SW_COUNT] = {0};
static char g_limitMsg[48];
static SwitchEvent g_lastStop;
static bool g_haveLastStop = false;

static bool SwitchMotionBusy() {
    return g_homePhase != HOME_NONE || g_jogOn || g_jogStopping || g_pendingJog;
}

static double EdgeMm(const SwitchEvent &e, int axis) {
    return (axis ? (double)e.y : (double)e.x) / g_axSpm[axis];
}

/* Curves that are not native arcs (DXF tessellation, driver polylines, chorded HP-GL arcs)
 * arrive as short chords. The planner's per-axis corner limit (corner_mm_s)
 * is applied at every joint but not over time, so many small joints in a row add up to
 * far more than accel_mm_s2: on a 2 mm circle at 100 mm/s the axes are kicked about every
 * 2 ms, about 5000 mm/s^2, and the motors shake. Estimate the local radius from the turn
 * between consecutive segments (r ~ chord / angle) and cap the segment speed so the
 * centripetal acceleration v^2 / r stays within the path acceleration. */
static uint32_t CurvatureCap(int32_t dx, int32_t dy, uint32_t speed) {
    const double len = sqrt((double)dx * (double)dx + (double)dy * (double)dy);
    const double ux = (double)dx / len;
    const double uy = (double)dy / len;
    uint32_t capped = speed;
    if (g_prevDirValid) {
        double c = ux * g_prevDirX + uy * g_prevDirY;
        c = (c > 1.0) ? 1.0 : ((c < -1.0) ? -1.0 : c);
        const double angle = acos(c);
        if (angle > 1e-3) {
            const double radius = 0.5 * (len + g_prevLen) / angle;
            const double vMax = sqrt(g_accelSteps * radius);
            if (vMax < (double)speed) {
                capped = (vMax < 1.0) ? 1u : (uint32_t)vMax;
                g_curveCapped++;
            }
        }
    }
    g_prevDirValid = true;
    g_prevDirX = ux;
    g_prevDirY = uy;
    g_prevLen = len;
    return capped;
}

/* ------------------------------------------------------------------------- */
/* Action queue                                                               */
/* ------------------------------------------------------------------------- */

static void Push(uint8_t type, int32_t x, int32_t y, uint32_t speed) {
    if (g_count >= CHPGL_ACTION_QUEUE) {
        g_dropped++;
        return;
    }
    Action &a = g_actions[(g_head + g_count) % CHPGL_ACTION_QUEUE];
    a.type = type;
    a.x = x;
    a.y = y;
    a.speed = speed;
    g_count++;
    g_lastPushMs = Milliseconds();
}

static Action &Head() {
    return g_actions[g_head];
}

static void Pop() {
    g_head = (uint16_t)((g_head + 1) % CHPGL_ACTION_QUEUE);
    g_count--;
}

static bool IsMotion(uint8_t type) {
    return type == ACT_LINE || type == ACT_ARC_CW || type == ACT_ARC_CCW;
}

/* Motions (lines and arcs) at the head of the queue, at most limit; *slots = action slots
 * they take (an arc takes two). */
static uint16_t MotionRunAtHead(uint16_t limit, uint16_t *slots) {
    uint16_t n = 0;
    uint16_t i = 0;
    while (i < g_count && n < limit) {
        const uint8_t type = g_actions[(g_head + i) % CHPGL_ACTION_QUEUE].type;
        if (!IsMotion(type)) {
            break;
        }
        i = (uint16_t)(i + (type == ACT_LINE ? 1 : 2));
        n++;
    }
    *slots = i;
    return n;
}

static void QueueHeadMotion() {
    const Action a = Head();
    Pop();
    g_xy.ArcVelMax(a.speed);
    if (a.type == ACT_LINE) {
        if (g_xy.QueueLinear(a.x, a.y)) {
            g_segments++;
        } else {
            g_dropped++;  /* zero-length after rounding, or rejected by the planner */
        }
        return;
    }
    const Action c = Head();  /* ACT_ARC_CENTER, pushed with the arc */
    Pop();
    if (g_xy.QueueArcTo(c.x, c.y, a.x, a.y, a.type == ACT_ARC_CW)) {
        g_segments++;
        g_arcs++;
    } else if (g_xy.QueueLinear(a.x, a.y)) {
        g_segments++;  /* keep the controller where the queue tail says it is */
        g_arcFallbacks++;
    } else {
        g_dropped++;
    }
}

static void FeedXy() {
    const uint8_t cap = g_xy.PlannerQueueCapacity();
    if (XyIdle()) {
        uint16_t slots = 0;
        const uint16_t run = MotionRunAtHead((uint16_t)(cap - 1), &slots);
        if (run == 0) {
            return;
        }
        const bool runEnds = slots < g_count;  /* a pen change follows */
        if (run < cap - 1 && !runEnds && (Milliseconds() - g_lastPushMs) < CHPGL_START_WAIT_MS) {
            return;  /* give the host a moment so the planner sees more than one segment */
        }
        g_xy.SetDeferMotionQueueStart(true);
        for (uint16_t i = 0; i < run; i++) {
            QueueHeadMotion();
        }
        g_xy.SetDeferMotionQueueStart(false);
        g_xy.StartDeferredMotionIfPending();
        return;
    }
    while (g_count > 0 && IsMotion(Head().type) && g_xy.MotionQueueCount() + 1 < cap) {
        QueueHeadMotion();
    }
}

/* ------------------------------------------------------------------------- */
/* Pen                                                                        */
/* ------------------------------------------------------------------------- */

static void StartPen(bool down) {
    g_penBusy = true;
    g_penTarget = down;
    g_penStartMs = Milliseconds();
    g_penSettleMs = down ? g_settings.penDownMs : g_settings.penUpMs;
    if (UsesZ()) {
        ConnectorM2.Move(down ? g_settings.zDownSteps : g_settings.zUpSteps,
                         StepGenerator::MOVE_TARGET_ABSOLUTE);
        g_penWaitZ = true;
    } else {
        SetPenOutput(down);
        g_penWaitZ = false;
    }
}

static void PollPen() {
    if (!g_penBusy) {
        return;
    }
    const uint32_t now = Milliseconds();
    if (g_penWaitZ) {
        if (!ConnectorM2.StepsComplete()) {
            if (now - g_penStartMs > kPenZTimeoutMs) {
                g_fault = "pen Z move timed out";
                g_jobAbort = true;
                PlotterDisable();
            }
            return;
        }
        g_penWaitZ = false;
        g_penStartMs = now;
    }
    if (now - g_penStartMs < g_penSettleMs) {
        return;
    }
    g_physPenDown = g_penTarget;
    g_penBusy = false;
}

/* ------------------------------------------------------------------------- */
/* HpglSink                                                                   */
/* ------------------------------------------------------------------------- */

class PlotterSinkImpl : public HpglSink {
public:
    void PenUp() override {
        Push(ACT_PEN_UP, 0, 0, 0);
        g_queuedPenDown = false;
        g_prevDirValid = false;
    }
    void PenDown() override {
        Push(ACT_PEN_DOWN, 0, 0, 0);
        g_queuedPenDown = true;
        g_prevDirValid = false;
    }
    void MoveTo(double xPu, double yPu) override {
        int32_t sx, sy;
        AxisToCtrl((int32_t)lround(xPu * g_stepsPerPu[0]), (int32_t)lround(yPu * g_stepsPerPu[1]), &sx, &sy);
        if (sx == g_tailX && sy == g_tailY) {
            return;
        }
        const uint32_t speed = CurvatureCap(sx - g_tailX, sy - g_tailY,
                                            g_queuedPenDown ? g_drawSps : g_travelSps);
        Push(ACT_LINE, sx, sy, speed);
        g_tailX = sx;
        g_tailY = sy;
    }
    /* One QueueArcTo instead of chords. Declined (chords instead) when the axes have different
     * step scales (the circle would be an ellipse in steps), the arc is tiny, or rounding to
     * steps would change it: the controller takes the sweep from the rounded end points. */
    bool ArcTo(double cxPu, double cyPu, double exPu, double eyPu, double sweepDeg) override {
        const double kx = g_stepsPerPu[0];
        const double ky = g_stepsPerPu[1];
        if (fabs(fabs(kx) - fabs(ky)) > 1e-9 * fabs(kx) || g_count + 2 > CHPGL_ACTION_QUEUE) {
            return false;
        }
        int32_t cx, cy, ex, ey;
        AxisToCtrl((int32_t)lround(cxPu * kx), (int32_t)lround(cyPu * ky), &cx, &cy);
        AxisToCtrl((int32_t)lround(exPu * kx), (int32_t)lround(eyPu * ky), &ex, &ey);
        const double sx = (double)g_tailX - cx;
        const double sy = (double)g_tailY - cy;
        const double r = sqrt(sx * sx + sy * sy);
        const double ux = (double)ex - cx;
        const double uy = (double)ey - cy;
        if (r < kMinArcRadiusSteps || fabs(sqrt(ux * ux + uy * uy) - r) > 2.0) {
            return false;
        }
        /* One axis reversed mirrors the arc, and so does exchanging X and Y. */
        const bool ccw = (sweepDeg > 0) != (((kx < 0) != (ky < 0)) != g_swapXy);
        double span = atan2(uy, ux) - atan2(sy, sx);
        if (!ccw) {
            span = -span;
        }
        span = fmod(span, 2.0 * M_PI);
        if (span <= 0) {
            span += 2.0 * M_PI;  /* same end point: a full circle, as in the controller */
        }
        const double want = fabs(sweepDeg) * M_PI / 180.0;
        if (fabs(span - want) > fmax(0.01, 3.0 / r)) {
            return false;
        }
        /* centripetal acceleration v^2 / r within the path acceleration */
        uint32_t speed = g_queuedPenDown ? g_drawSps : g_travelSps;
        const double vMax = sqrt(g_accelSteps * r);
        if (vMax < (double)speed) {
            speed = (vMax < 1.0) ? 1u : (uint32_t)vMax;
            g_curveCapped++;
        }
        Push(ccw ? ACT_ARC_CCW : ACT_ARC_CW, ex, ey, speed);
        Push(ACT_ARC_CENTER, cx, cy, 0);
        g_tailX = ex;
        g_tailY = ey;
        /* exit tangent, for the curvature estimate of the segment after */
        const double ul = sqrt(ux * ux + uy * uy);
        g_prevDirValid = true;
        g_prevDirX = (ccw ? -uy : uy) / ul;
        g_prevDirY = (ccw ? ux : -ux) / ul;
        g_prevLen = 0.0;
        return true;
    }
    void SetVelocity(double mmPerSec) override {
        /* job_speed 0: the board's pen-down speed, whatever the job asks for (CAD drivers send
         * their own VS, e.g. AutoCAD's pen speed table). */
        double v = (mmPerSec <= 0 || !g_settings.jobSpeed) ? g_settings.drawMmS : mmPerSec;
        if (v > g_settings.maxMmS) {
            v = g_settings.maxMmS;
        }
        const double sps = v * fabs(SettingsStepsPerMm(0));
        g_drawSps = (sps < 1.0) ? 1u : (uint32_t)sps;
    }
    void SelectPen(int pen) override {
        g_pen = pen;
    }
    void Reply(const char *text) override {
        if (g_reply) {
            g_reply(text);
        }
    }
    void ActualPosition(double *xPu, double *yPu, bool *penDown) override {
        HeadPu(xPu, yPu);
        *penDown = g_physPenDown;
    }
    void PageEnd() override {
        g_pages++;
    }
    uint32_t Room() override {
        return (uint32_t)(CHPGL_ACTION_QUEUE - g_count);
    }
};

static PlotterSinkImpl g_sink;

HpglSink *PlotterSink() {
    return &g_sink;
}

void PlotterSetReplyHandler(PlotterReplyFn fn) {
    g_reply = fn;
}

uint32_t PlotterRoom() {
    return (uint32_t)(CHPGL_ACTION_QUEUE - g_count);
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

void PlotterApplySettings() {
    const double spmmX = SettingsStepsPerMm(0);
    const double spmmY = SettingsStepsPerMm(1);
    g_stepsPerPu[0] = spmmX / HPGL_PU_PER_MM * (g_settings.dir[0] < 0 ? -1.0 : 1.0);
    g_stepsPerPu[1] = spmmY / HPGL_PU_PER_MM * (g_settings.dir[1] < 0 ? -1.0 : 1.0);
    /* The interpolator works in steps, so path speed and accel use the X scale. */
    const double travel = g_settings.travelMmS * spmmX;
    g_travelSps = travel < 1.0 ? 1u : (uint32_t)travel;
    g_sink.SetVelocity(0);
    g_accelSteps = g_settings.accelMmS2 * spmmX;
    g_xy.ArcAccelMax((uint32_t)g_accelSteps);
    g_xy.JunctionDVmax((uint32_t)(g_settings.cornerMmS * spmmX));  /* per motor (A/B in CoreXY) */
    /* CoreXY: a motor runs up to sqrt(2) x the path speed (on a 45-degree move); this keeps each
     * one within its limit by lowering the path speed of a segment where needed. */
    const double motorMax = g_settings.motorMaxMmS * spmmX;
    g_xy.MotorVelMax(g_settings.motorMaxMmS > 0 ? (motorMax < 1.0 ? 1u : (uint32_t)motorMax) : 0u);
    for (MotorDriver *m : {&ConnectorM0, &ConnectorM1}) {
        m->EStopDecelMax((uint32_t)(g_settings.accelMmS2 * spmmX * 2.0));
    }
    /* Switches. */
    const double rampDecel = g_settings.accelMmS2 * spmmX * 3.0;
    g_rampDecel = rampDecel < 1.0 ? 1u : (uint32_t)rampDecel;
    g_stopDecel = g_accelSteps < 1.0 ? 1u : (uint32_t)g_accelSteps;
    {
        SwitchConfig sc;
        for (int i = 0; i < SW_COUNT; i++) {
            sc.pin[i] = g_settings.swPin[i];
        }
        sc.normallyOpen = g_settings.swOpen;
        g_swHold = true;
        g_sw.Configure(sc);
        g_swHold = false;
    }
    /* Kinematics and swap_xy change only with the motors disabled ([disabled] settings). The old
     * X/Y means nothing in the new frame, so the present position becomes the origin. */
    const CoordKinematics kin = g_settings.kinematics ? KINEMATICS_COREXY : KINEMATICS_CARTESIAN;
    const bool swap = g_settings.swapXy != 0;
    if ((kin != g_xy.Kinematics() || swap != g_swapXy) && !g_enabled && XyIdle()) {
        g_xy.SetKinematics(kin);
        g_swapXy = swap;
        ZeroPosition();
    }
    if (!g_enabled) {
        ConnectorM2.VelMax(g_settings.zVelSteps);
        ConnectorM2.AccelMax(g_settings.zAccelSteps);
        InitPenHardware();
        SyncTailFromMotion();
        /* Every input but the pen's reads as a digital input, so any of them can be found as a
         * switch. dir, swap_xy and the scales only change here: the switch frame restarts. */
        g_swHold = true;
        for (int c = 0; c <= kInputCodeMax; c++) {
            Connector *k = InputConnector(c);
            if (k && g_settings.penMode == PEN_MODE_OUTPUT && c == g_settings.penIo) {
                k = nullptr;
            }
            if (k) {
                k->Mode(Connector::INPUT_DIGITAL);
            }
            g_inputs[c] = k;
        }
        g_axSign[0] = g_stepsPerPu[0] < 0 ? -1 : 1;
        g_axSign[1] = g_stepsPerPu[1] < 0 ? -1 : 1;
        g_axSpm[0] = fabs(spmmX);
        g_axSpm[1] = fabs(spmmY);
        g_sw.Resync();
        g_swHold = false;
    }
}

bool PlotterInit() {
    MotorMgr.MotorModeSet(MotorManager::MOTOR_ALL, Connector::CPM_MODE_STEP_AND_DIR);
    Delay_ms(50);
    for (MotorDriver *m : {&ConnectorM0, &ConnectorM1, &ConnectorM2}) {
        m->HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
        m->HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
        m->EnableRequest(false);
    }
    ConnectorDI6.Mode(Connector::INPUT_DIGITAL);
    if (!g_xy.Initialize(&ConnectorM0, &ConnectorM1)) {
        g_fault = "motion controller init failed";
        return false;
    }
    /* Always plan to a stop at the tail; later segments re-plan it. */
    g_xy.StopAtQueueEnd(true);
    ZeroPosition();
    PlotterApplySettings();
    g_positionChanged = false;
    SysMgr.SampleCallbackSet(SampleSwitches);
    g_ready = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Control                                                                    */
/* ------------------------------------------------------------------------- */

bool PlotterEnabled() {
    return g_enabled;
}

static bool AllHlfbAsserted() {
    if (ConnectorM0.HlfbState() != MotorDriver::HLFB_ASSERTED) return false;
    if (ConnectorM1.HlfbState() != MotorDriver::HLFB_ASSERTED) return false;
    if (UsesZ() && ConnectorM2.HlfbState() != MotorDriver::HLFB_ASSERTED) return false;
    return true;
}

const char *PlotterEnable() {
    if (!g_ready) {
        return "motion controller not initialized";
    }
    if (EstopTripped()) {
        return "estop open (DI-6); wire it or set estop_di6 0";
    }
    if (g_enabled) {
        return nullptr;
    }
    g_fault = nullptr;
    for (MotorDriver *m : {&ConnectorM0, &ConnectorM1, &ConnectorM2}) {
        if (m == &ConnectorM2 && !UsesZ()) {
            continue;
        }
        m->ClearAlerts();
        m->EnableRequest(true);
    }
    if (g_settings.hlfbCheck) {
        const uint32_t start = Milliseconds();
        while (!AllHlfbAsserted()) {
            if (Milliseconds() - start > CHPGL_ENABLE_HLFB_WAIT_MS) {
                for (MotorDriver *m : {&ConnectorM0, &ConnectorM1, &ConnectorM2}) {
                    m->EnableRequest(false);
                }
                return "HLFB not asserted (check motors, or set hlfb_check 0)";
            }
        }
    }
    g_enabled = true;
    if (UsesZ()) {
        StartPen(false);  /* known pen-up position before the first segment */
    }
    return nullptr;
}

static void EndSwitchMotion();

const char *PlotterDisable() {
    EndSwitchMotion();
    g_xy.Stop();
    ConnectorM2.MoveStopAbrupt();
    for (MotorDriver *m : {&ConnectorM0, &ConnectorM1, &ConnectorM2}) {
        m->EnableRequest(false);
    }
    if (g_count > 0) {
        g_jobAbort = true;
    }
    g_enabled = false;
    g_paused = false;
    g_abortStop = false;
    g_resumeLower = false;
    g_penBusy = false;
    g_penWaitZ = false;
    ClearActions();
    if (!UsesZ()) {
        SetPenOutput(false);
    }
    g_physPenDown = false;
    SyncTailFromMotion();
    return nullptr;
}

void PlotterAbort() {
    ClearActions();
    g_paused = false;
    g_resumeLower = false;
    PlotterJogStop();
    PlotterHomeStop();
    if (g_enabled && !XyIdle()) {
        g_xy.StopRamped(g_rampDecel);  /* along the path, at 3 x accel */
        g_abortStop = true;
        return;
    }
    SyncTailFromMotion();
    if (g_physPenDown || g_penBusy) {
        g_sink.PenUp();
    }
}

void PlotterPause(bool on) {
    g_paused = on;
}

bool PlotterPaused() {
    return g_paused;
}

bool PlotterTakeJobAbort() {
    const bool r = g_jobAbort;
    g_jobAbort = false;
    return r;
}

static bool SwitchMotionBusy();

bool PlotterIdle() {
    return g_count == 0 && XyIdle() && !g_penBusy && !g_abortStop && !SwitchMotionBusy();
}

const char *PlotterSetOrigin() {
    if (!PlotterIdle()) {
        return "plotter busy";
    }
    ZeroPosition();
    g_positionChanged = false;  /* the caller resyncs */
    g_homed = 0;                /* the frame moved off the switches */
    return nullptr;
}

void PlotterClearFault() {
    g_fault = nullptr;
}

const char *PlotterFault() {
    return g_fault;
}

static void PollSwitchMotion();

void PlotterPoll() {
    if (!g_ready) {
        return;
    }
    PollSwitchMotion();
    if (EstopTripped()) {
        if (g_enabled || g_count > 0) {
            PlotterDisable();
            g_fault = "estop (DI-6)";
            g_jobAbort = true;
        }
        return;
    }
    if (g_enabled) {
        for (MotorDriver *m : {&ConnectorM0, &ConnectorM1}) {
            if (m->StatusReg().bit.MotorInFault || m->StatusReg().bit.AlertsPresent) {
                PlotterDisable();
                g_fault = (m == &ConnectorM0) ? "motor fault on M0 (X)" : "motor fault on M1 (Y)";
                g_jobAbort = true;
                return;
            }
        }
    }

    if (!g_enabled) {
        if (g_count == 0 || !g_settings.autoEnable || g_fault) {
            return;
        }
        const char *err = PlotterEnable();
        if (err) {
            g_fault = err;
            g_jobAbort = true;
            ClearActions();
            return;
        }
    }

    if (g_abortStop) {
        if (!XyIdle() || g_xy.StopRampedPending()) {
            return;
        }
        g_abortStop = false;
        SyncTailFromMotion();
        if (g_physPenDown) {
            g_sink.PenUp();
        }
    }

    PollPen();
    if (g_penBusy || SwitchMotionBusy()) {
        return;
    }
    if (g_fault && g_count > 0 && Head().type != ACT_PEN_UP) {
        return;  /* a limit stopped the last job: the next waits for clear (the pen still lifts) */
    }

    if (g_paused) {
        if (XyIdle() && g_physPenDown) {
            g_resumeLower = true;
            StartPen(false);
        }
        return;
    }
    if (g_resumeLower) {
        g_resumeLower = false;
        StartPen(true);
        return;
    }

    if (g_count == 0) {
        return;
    }
    const Action &a = Head();
    if (IsMotion(a.type)) {
        FeedXy();
        return;
    }
    if (!XyIdle()) {
        return;  /* pen changes happen with XY stopped */
    }
    const bool down = (a.type == ACT_PEN_DOWN);
    Pop();
    if (down != g_physPenDown) {
        StartPen(down);
    }
}

static const char *StateName() {
    if (g_fault) {
        return "fault";
    }
    if (TransportBuffering()) {
        return "buffering";  /* receiving a job to reorder; plotting starts when it ends */
    }
    if (g_homePhase != HOME_NONE) {
        return "homing";
    }
    if (!g_enabled) {
        return (g_count > 0) ? "waiting_enable" : "disabled";
    }
    if (g_jogOn || g_jogStopping) {
        return "jogging";
    }
    if (g_paused) {
        return "paused";
    }
    return PlotterIdle() ? "idle" : "plotting";
}

void PlotterTelemetry(char *buf, size_t len) {
    double x, y;
    HeadPu(&x, &y);
    x /= HPGL_PU_PER_MM;
    y /= HPGL_PU_PER_MM;
    snprintf(buf, len, "TEL ms=%lu x=%.3f y=%.3f pen=%d state=%s queue=%u", (unsigned long)Milliseconds(), x, y,
             g_physPenDown ? 1 : 0, StateName(), (unsigned)g_count);
}

void PlotterDiagReset() {
    g_xy.ResetTimingProbe();
}

void PlotterDiag(void (*out)(const char *line)) {
    char buf[200];
    snprintf(buf, sizeof(buf), "bursts=%lu (samples asking > %ld steps) excess_carried M0=%lu M1=%lu",
             (unsigned long)g_xy.BurstCount(), (long)CoordinatedMotionController::BURST_THRESHOLD,
             (unsigned long)ConnectorM0.CoordinatedExcessSteps(),
             (unsigned long)ConnectorM1.CoordinatedExcessSteps());
    out(buf);
    /* Controller position vs pulses actually sent to each motor (compare with the ClearPath's
     * own commanded count to tell software bookkeeping loss from lost pulses on the wire). */
    snprintf(buf, sizeof(buf),
             "timing: max_irq_off=%lu us (QueueLinear/QueueArc)  max_update_gap=%lu us (nominal 200)  "
             "late_updates(>250us)=%lu",
             (unsigned long)(g_xy.MaxIrqOffCycles() / 120u), (unsigned long)(g_xy.MaxUpdateGapCycles() / 120u),
             (unsigned long)g_xy.LateUpdates());
    out(buf);
    static const char *const kKind[] = {"line sample", "arc sample", "line start", "arc start"};
    snprintf(buf, sizeof(buf), "step interrupt: max=%lu us (%s)  slow(>150us of 200)=%lu",
             (unsigned long)(g_xy.MaxUpdateCycles() / 120u), kKind[g_xy.MaxUpdateKind() & 3],
             (unsigned long)g_xy.SlowUpdates());
    out(buf);
    /* The pulses sent to each motor, mapped back to X/Y: equal to the controller position when
     * nothing was lost (both start at 0 at power-up and at origin). */
    const int32_t m0 = ConnectorM0.PositionRefCommanded();
    const int32_t m1 = ConnectorM1.PositionRefCommanded();
    int32_t px, py;
    const bool onGrid = Kinematics::ToCartesian(g_xy.Kinematics(), m0, m1, px, py);
    snprintf(buf, sizeof(buf),
             "controller x=%ld y=%ld  pulses_sent M0=%ld M1=%ld (as x=%ld y=%ld%s)  kinematics=%s swap_xy=%d",
             (long)g_xy.CurrentX(), (long)g_xy.CurrentY(), (long)m0, (long)m1, (long)px, (long)py,
             onGrid ? "" : ", half step", g_xy.Kinematics() == KINEMATICS_COREXY ? "corexy" : "cartesian",
             g_swapXy ? 1 : 0);
    out(buf);
    CoordinatedMotionController::BurstEvent ev[CoordinatedMotionController::BURST_LOG_SIZE];
    const uint8_t n = g_xy.BurstLog(ev, CoordinatedMotionController::BURST_LOG_SIZE);
    for (uint8_t i = 0; i < n; i++) {
        const CoordinatedMotionController::BurstEvent &e = ev[i];
        snprintf(buf, sizeof(buf),
                 "ms=%lu path=%s q=%u steps=%ld,%ld seg=(%ld,%ld)->(%ld,%ld) last=(%ld,%ld) "
                 "entry=%lu exit=%lu nominal=%lu",
                 (unsigned long)e.ms, e.path == 1 ? "clamp" : (e.path == 0 ? "normal" : "arc"),
                 (unsigned)e.queued, (long)e.stepsX, (long)e.stepsY, (long)e.startX, (long)e.startY,
                 (long)e.endX, (long)e.endY, (long)e.lastX, (long)e.lastY, (unsigned long)e.entry,
                 (unsigned long)e.exitSpeed, (unsigned long)e.nominal);
        out(buf);
    }
}

void PlotterStatus(char *buf, size_t len) {
    const char *state = StateName();
    double x, y;
    HeadPu(&x, &y);
    x /= HPGL_PU_PER_MM;
    y /= HPGL_PU_PER_MM;
    snprintf(buf, len,
             "state=%s enabled=%d x_mm=%.3f y_mm=%.3f pen=%s sel_pen=%d queue=%u planner=%u "
             "segments=%lu arcs=%lu arc_fallbacks=%lu curve_capped=%lu dropped=%lu pages=%lu "
             "inputs=%04x sw=%02x homed=%u home_mode=%u estop=%s fault=%s",
             state, g_enabled ? 1 : 0, x, y, g_physPenDown ? "down" : "up", g_pen,
             (unsigned)g_count, (unsigned)g_xy.MotionQueueCount(), (unsigned long)g_segments,
             (unsigned long)g_arcs, (unsigned long)g_arcFallbacks, (unsigned long)g_curveCapped,
             (unsigned long)g_dropped, (unsigned long)g_pages, (unsigned)g_sw.Levels(),
             (unsigned)g_sw.Tripped(), (unsigned)g_homed, (unsigned)g_settings.homeMode,
             EstopTripped() ? "open" : "ok", g_fault ? g_fault : "none");
}

/* ------------------------------------------------------------------------- */
/* Jog and homing                                                              */
/* ------------------------------------------------------------------------- */

/* One axis by delta HP-GL axis steps, pen up, at mm/s. */
static bool MoveAxisSteps(int axis, int32_t delta, double mmPerS) {
    int32_t t[2] = {HeadSteps(0), HeadSteps(1)};
    t[axis] += delta;
    int32_t cx, cy;
    AxisToCtrl(t[0] * g_axSign[0], t[1] * g_axSign[1], &cx, &cy);
    const double sps = fabs(mmPerS) * g_axSpm[axis];
    g_xy.ArcVelMax(sps < 1.0 ? 1u : (uint32_t)sps);
    g_prevDirValid = false;
    if (!g_xy.QueueLinear(cx, cy)) {
        return false;
    }
    g_tailX = cx;
    g_tailY = cy;
    return true;
}

static void NoteStop(const SwitchEvent &e) {
    g_lastStop = e;
    g_haveLastStop = true;
    if (e.limit && e.sw >= 0 && e.sw < SW_COUNT) {
        g_trips[e.sw]++;
        g_tripAtMm[e.sw] = EdgeMm(e, SwitchAxis(e.sw));
    }
}

class PlotterHomingIo : public HomingIo {
public:
    void Move(int axis, double mm, double mmPerS, uint8_t onTrip, uint8_t onRelease) override {
        g_sw.ArmStop(onTrip, onRelease);
        MoveAxisSteps(axis, (int32_t)lround(mm * g_axSpm[axis]), mmPerS);
    }
    bool Idle() override {
        return XyIdle() && !g_xy.StopRampedPending();
    }
    uint8_t Tripped() override {
        return g_sw.Tripped();
    }
    bool TakeEvent(SwitchEvent *e) override {
        if (!g_sw.TakeEvent(e)) {
            return false;
        }
        NoteStop(*e);
        return true;
    }
    int32_t Position(int axis) override {
        return HeadSteps(axis);
    }
    void SetPosition(int axis, int32_t steps) override {
        SetHeadSteps(axis ? HeadSteps(0) : steps, axis ? steps : HeadSteps(1));
    }
    double StepsPerMm(int axis) override {
        return g_axSpm[axis];
    }
};

static PlotterHomingIo g_homeIo;

static void EndHoming(const char *err, bool stopped) {
    g_sw.SetLimitMask(0);
    g_sw.ArmStop(0, 0);
    for (int a = 0; a < 2; a++) {
        if (g_homeAxes & (1u << a)) {
            g_homeEnds[a]++;
        }
    }
    g_homePhase = HOME_NONE;
    if (!err) {
        g_homed |= g_homeAxes;
        g_homeLast = "ok";
        return;
    }
    g_homeLast = stopped ? "stopped" : err;
    if (!stopped) {
        g_fault = err;
        g_jobAbort = true;
    }
}

static void EndSwitchMotion() {
    g_jogOn = false;
    g_jogStopping = false;
    g_pendingJog = false;
    g_jogStoppedInput = -1;
    g_sw.ArmInputStop(0);
    if (g_homePhase != HOME_NONE) {
        if (g_homePhase == HOME_RUN) {
            g_seq.Abort();
        }
        EndHoming("stopped", true);
    }
    g_homed = 0;
}

static void StartJog(int axis, double v, uint16_t stopOn) {
    g_sw.ArmInputStop(stopOn);
    const double far = v < 0 ? -5000.0 : 5000.0;  /* mm: the deadman ends it long before */
    if (!MoveAxisSteps(axis, (int32_t)lround(far * g_axSpm[axis]), v)) {
        g_sw.ArmInputStop(0);
        return;
    }
    g_jogOn = true;
    g_jogAxis = axis;
    g_jogV = v;
    g_jogLimit = -1;
    g_jogUntil = Milliseconds() + CHPGL_JOG_DEADMAN_MS;
}

static void BeginHomingMotion(uint32_t now) {
    if (g_settings.homeMode == 1) {
        uint8_t order[2];
        int n = 0;
        const int first = g_settings.homeOrder ? 1 : 0;
        for (int k = 0; k < 2; k++) {
            const int a = k ? 1 - first : first;
            if (g_homeAxes & (1u << a)) {
                order[n++] = (uint8_t)a;
            }
        }
        HomingAxis cfg[2];
        for (int a = 0; a < 2; a++) {
            cfg[a].dir = g_settings.homeDir[a] < 0 ? -1 : 1;
            cfg[a].fast = g_settings.homeFast[a];
            cfg[a].slow = g_settings.homeSlow[a];
            cfg[a].back = g_settings.homeBack[a];
            cfg[a].at = g_settings.homeAt[a];
            cfg[a].travel = g_settings.homeTravel[a];
        }
        const double plot[2] = {g_settings.areaMm[0], g_settings.areaMm[1]};
        /* A home switch on a limit's input: that limit does not stop the homing onto it. */
        uint8_t mask = 0;
        for (int a = 0; a < 2; a++) {
            const int h = a ? SW_Y_HOME : SW_X_HOME;
            const int lims[2] = {a ? SW_Y_PLUS : SW_X_PLUS, a ? SW_Y_MINUS : SW_X_MINUS};
            for (int l : lims) {
                if (g_settings.swPin[h] >= 0 && g_settings.swPin[l] == g_settings.swPin[h]) {
                    mask |= (uint8_t)(1u << l);
                }
            }
        }
        g_sw.SetLimitMask(mask);
        g_seq.Start(&g_homeIo, order, n, cfg, plot);
        g_homePhase = HOME_RUN;
        return;
    }
    /* ClearPath hard-stop homing: each motor homes itself when enabled (set up in MSP, homing on
     * every enable); HLFB asserts when it is done. Already enabled: off first. */
    if (g_enabled) {
        for (MotorDriver *m : {&ConnectorM0, &ConnectorM1}) {
            m->EnableRequest(false);
        }
        g_enabled = false;
        g_homeMs = now;
    } else {
        g_homeMs = now - CHPGL_HARD_HOME_OFF_MS;
    }
    g_homePhase = HOME_HARD_OFF;
}

static void PollSwitchMotion() {
    const uint32_t now = Milliseconds();
    /* Stops outside homing: a limit (a fault for a job, a stop for a jog), a jog's stop-on input. */
    if (g_homePhase != HOME_RUN) {
        SwitchEvent e;
        if (g_sw.TakeEvent(&e)) {
            NoteStop(e);
            if (g_jogOn || g_jogStopping || g_pendingJog) {
                g_jogOn = false;
                g_pendingJog = false;
                g_jogStopping = true;
                if (e.limit) {
                    g_jogLimit = e.sw;
                } else {
                    g_jogStoppedInput = e.input;
                }
            } else if (e.limit) {
                snprintf(g_limitMsg, sizeof(g_limitMsg), "limit %s (%s)", SwitchName(e.sw), InputName(e.input));
                g_fault = g_limitMsg;
                g_jobAbort = true;
                ClearActions();
                g_paused = false;
                g_resumeLower = false;
                g_abortStop = true;  /* already ramping down: the pen lifts when it stops */
            }
        }
    }

    /* Jog: the deadman, then the ramp down. */
    if (g_jogOn && (int32_t)(now - g_jogUntil) > 0) {
        g_xy.StopRamped(g_stopDecel);
        g_jogOn = false;
        g_jogStopping = true;
    }
    if (g_jogStopping && XyIdle() && !g_xy.StopRampedPending()) {
        g_jogStopping = false;
        g_sw.ArmInputStop(0);
        SyncTailFromMotion();
        g_positionChanged = true;  /* the interpreter takes the new pen position */
    }
    if (g_pendingJog && !g_jogStopping && XyIdle() && !g_xy.StopRampedPending()) {
        g_pendingJog = false;
        StartJog(g_pendingAxis, g_pendingV, g_pendingStopOn);
    }

    /* Homing. */
    switch (g_homePhase) {
        case HOME_NONE:
            break;
        case HOME_PEN:
            if (g_enabled) {
                if (g_penBusy) {
                    break;  /* PlotterPoll runs the pen */
                }
                if (g_physPenDown) {
                    StartPen(false);
                    break;
                }
                if (!XyIdle()) {
                    break;
                }
            } else if (g_settings.homeMode == 1) {
                EndHoming("motors disabled", false);
                break;
            }
            BeginHomingMotion(now);
            break;
        case HOME_RUN:
            if (!g_enabled) {
                EndHoming("motors disabled", false);
                break;
            }
            if (!g_seq.Poll()) {
                if (g_seq.GetState() == HomingSequencer::DONE) {
                    EndHoming(nullptr, false);
                } else {
                    const char *err = g_seq.Error() ? g_seq.Error() : "homing failed";
                    EndHoming(err, strcmp(err, "homing stopped") == 0);
                }
            }
            break;
        case HOME_HARD_OFF:
        case HOME_HARD_WAIT: {
            if (EstopTripped()) {
                PlotterDisable();
                g_fault = "estop (DI-6)";
                break;
            }
            if (g_homePhase == HOME_HARD_OFF) {
                if (now - g_homeMs < CHPGL_HARD_HOME_OFF_MS) {
                    break;
                }
                for (MotorDriver *m : {&ConnectorM0, &ConnectorM1, &ConnectorM2}) {
                    if (m == &ConnectorM2 && !UsesZ()) {
                        continue;
                    }
                    m->ClearAlerts();
                    m->EnableRequest(true);
                }
                g_homeMs = now;
                g_homePhase = HOME_HARD_WAIT;
                break;
            }
            for (MotorDriver *m : {&ConnectorM0, &ConnectorM1}) {
                if (now - g_homeMs > 500 && (m->StatusReg().bit.MotorInFault || m->StatusReg().bit.AlertsPresent)) {
                    PlotterDisable();
                    g_homeLast = g_fault = (m == &ConnectorM0) ? "motor alert on M0 during ClearPath homing"
                                                               : "motor alert on M1 during ClearPath homing";
                    g_jobAbort = true;
                    return;
                }
            }
            if (now - g_homeMs > CHPGL_HARD_HOME_TIMEOUT_MS) {
                PlotterDisable();
                g_homeLast = g_fault = "ClearPath homing did not finish (HLFB not asserted)";
                g_jobAbort = true;
                break;
            }
            if (now - g_homeMs < 300 || !AllHlfbAsserted()) {
                break;
            }
            g_enabled = true;
            if (UsesZ()) {
                StartPen(false);
            }
            SetHeadSteps((int32_t)lround(g_settings.homeAt[0] * g_axSpm[0]),
                         (int32_t)lround(g_settings.homeAt[1] * g_axSpm[1]));
            EndHoming(nullptr, false);
            break;
        }
    }
}

const char *PlotterJog(int axis, double mmPerS, uint16_t stopOn) {
    static char msg[64];
    if (!g_ready) {
        return "motion controller not initialized";
    }
    if (!g_enabled) {
        return "motors disabled (enable first)";
    }
    if (axis < 0 || axis > 1 || !(fabs(mmPerS) > 0.0)) {
        return "usage: jog <x|y> <mm_s> [stopon <input>,...]";
    }
    if (g_homePhase != HOME_NONE) {
        return "homing";
    }
    if (g_count > 0 || g_abortStop || g_penBusy) {
        return "plotter busy";
    }
    if (g_jogStoppedInput >= 0) {
        snprintf(msg, sizeof(msg), "stopped on a switch: %s (jog stop)", InputName(g_jogStoppedInput));
        return msg;
    }
    if (mmPerS > g_settings.maxMmS) {
        mmPerS = g_settings.maxMmS;
    } else if (mmPerS < -g_settings.maxMmS) {
        mmPerS = -g_settings.maxMmS;
    }
    /* A tripped limit: only away from it. */
    const int lim = axis ? (mmPerS > 0 ? SW_Y_PLUS : SW_Y_MINUS) : (mmPerS > 0 ? SW_X_PLUS : SW_X_MINUS);
    if (g_sw.Tripped() & (1u << lim)) {
        if (g_jogOn) {
            PlotterJogStop();
        }
        snprintf(msg, sizeof(msg), "limit %s tripped: jog the other way", SwitchName(lim));
        return msg;
    }
    if (g_fault == g_limitMsg) {
        g_fault = nullptr;  /* jogging off the limit */
    }
    const uint32_t now = Milliseconds();
    if (g_jogOn) {
        if (axis == g_jogAxis && mmPerS == g_jogV) {
            g_jogUntil = now + CHPGL_JOG_DEADMAN_MS;
            return nullptr;
        }
        g_xy.StopRamped(g_stopDecel);
        g_jogOn = false;
        g_jogStopping = true;
    }
    if (g_jogStopping || !XyIdle()) {
        g_pendingJog = true;
        g_pendingAxis = axis;
        g_pendingV = mmPerS;
        g_pendingStopOn = stopOn;
        return nullptr;
    }
    StartJog(axis, mmPerS, stopOn);
    return g_jogOn ? nullptr : "jog refused by the planner";
}

void PlotterJogStop() {
    g_pendingJog = false;
    g_jogStoppedInput = -1;
    if (g_jogOn) {
        g_xy.StopRamped(g_stopDecel);
        g_jogOn = false;
        g_jogStopping = true;
    }
    g_sw.ArmInputStop(0);
}

const char *PlotterHome(uint8_t axes) {
    if (!g_ready) {
        return "motion controller not initialized";
    }
    if (g_settings.homeMode == 0) {
        return "homing is off (set home_mode 1 or 2)";
    }
    if (g_homePhase != HOME_NONE) {
        return "homing already";
    }
    if (g_count > 0 || !XyIdle() || g_abortStop || g_jogOn || g_jogStopping || g_pendingJog) {
        return "plotter busy";
    }
    if (EstopTripped()) {
        return "estop open (DI-6)";
    }
    axes &= 3u;
    if (!axes) {
        /* All: each axis with a home switch (both for ClearPath homing). */
        axes = g_settings.homeMode == 2 ? 3
                                        : (uint8_t)((g_settings.swPin[SW_X_HOME] >= 0 ? 1u : 0u) |
                                                    (g_settings.swPin[SW_Y_HOME] >= 0 ? 2u : 0u));
        if (!axes) {
            return "no home switch set (x_home, y_home)";
        }
    }
    if (g_settings.homeMode == 1) {
        if ((axes & 1u) && g_settings.swPin[SW_X_HOME] < 0) {
            return "no X home switch (set x_home)";
        }
        if ((axes & 2u) && g_settings.swPin[SW_Y_HOME] < 0) {
            return "no Y home switch (set y_home)";
        }
        if (!g_enabled) {
            if (const char *err = PlotterEnable()) {
                return err;
            }
        }
    } else {
        if (axes != 3) {
            return "ClearPath hard-stop homing homes both axes together";
        }
        if (!g_settings.hlfbCheck) {
            return "ClearPath homing is seen on HLFB: set hlfb_check 1";
        }
    }
    if (g_fault == g_limitMsg) {
        g_fault = nullptr;
    }
    g_homeAxes = axes;
    g_homed &= (uint8_t)~axes;
    g_homeLast = "running";
    g_homePhase = HOME_PEN;
    return nullptr;
}

void PlotterHomeStop() {
    switch (g_homePhase) {
        case HOME_NONE:
            break;
        case HOME_RUN:
            g_xy.StopRamped(g_stopDecel);
            g_seq.Abort();  /* the next poll ends it */
            break;
        case HOME_PEN:
            EndHoming("stopped", true);
            break;
        default:
            PlotterDisable();  /* a ClearPath stops homing when disabled */
            break;
    }
}

uint8_t PlotterHomed() {
    return g_homed;
}

bool PlotterHoldJobs(bool jobWaiting) {
    if (g_homePhase != HOME_NONE) {
        return true;
    }
    if (!g_settings.homeRequired || g_settings.homeMode == 0) {
        return false;
    }
    uint8_t need = 3;
    if (g_settings.homeMode == 1) {
        need = (uint8_t)((g_settings.swPin[SW_X_HOME] >= 0 ? 1u : 0u) | (g_settings.swPin[SW_Y_HOME] >= 0 ? 2u : 0u));
    }
    if (need == 0 || (g_homed & need) == need) {
        return false;
    }
    /* A failed homing leaves its fault: the job waits for clear, then homes again. */
    if (!jobWaiting || g_fault || !PlotterIdle()) {
        return true;
    }
    if (!g_enabled && !g_settings.autoEnable) {
        return true;
    }
    if (const char *err = PlotterHome(need)) {
        g_fault = err;
    }
    return true;
}

bool PlotterTakePositionChanged() {
    const bool r = g_positionChanged;
    g_positionChanged = false;
    return r;
}

void PlotterSimLevel(int code, int mode) {
    g_swHold = true;
    g_sw.SimLevel(code, mode);
    g_swHold = false;
}

void PlotterSimAt(int code, int axis, int side, double mm) {
    g_swHold = true;
    g_sw.SimAt(code, axis ? 1 : 0, side, (int32_t)lround(mm * g_axSpm[axis ? 1 : 0]));
    g_swHold = false;
}

void PlotterSimClear() {
    g_swHold = true;
    g_sw.SimClear();
    g_swHold = false;
}

void PlotterSwitchInfo(void (*out)(const char *line)) {
    static const char *const kKey[] = {"x+", "x-", "x_home", "y+", "y-", "y_home"};
    char buf[200];
    uint16_t sim = 0;
    for (int c = 0; c <= kInputCodeMax; c++) {
        if (g_sw.Simulated(c)) {
            sim |= (uint16_t)(1u << c);
        }
    }
    snprintf(buf, sizeof(buf), "inputs levels=%04x sim=%04x tripped=%02x", (unsigned)g_sw.Levels(), (unsigned)sim,
             (unsigned)g_sw.Tripped());
    out(buf);
    for (int s = 0; s < SW_COUNT; s++) {
        snprintf(buf, sizeof(buf), "switch %s input=%s open=%d tripped=%d trips=%lu at=%.3f", kKey[s],
                 InputName(g_settings.swPin[s]), (g_settings.swOpen >> s) & 1, (g_sw.Tripped() >> s) & 1,
                 (unsigned long)g_trips[s], g_tripAtMm[s]);
        out(buf);
    }
    snprintf(buf, sizeof(buf), "pos x=%.3f y=%.3f moving=%d", HeadSteps(0) / g_axSpm[0], HeadSteps(1) / g_axSpm[1],
             XyIdle() ? 0 : 1);
    out(buf);
    snprintf(buf, sizeof(buf), "jog on=%d axis=%c v=%.3f stopped=%s limit=%s", (g_jogOn || g_pendingJog) ? 1 : 0,
             g_jogAxis ? 'y' : 'x', g_jogV, g_jogStoppedInput >= 0 ? InputName(g_jogStoppedInput) : "none",
             g_jogLimit >= 0 ? kKey[g_jogLimit] : "none");
    out(buf);
    if (g_haveLastStop) {
        const SwitchEvent &e = g_lastStop;
        snprintf(buf, sizeof(buf), "laststop kind=%s switch=%s input=%s x=%.3f y=%.3f",
                 e.limit ? "limit" : (e.sw < 0 ? "input" : (e.released ? "release" : "trip")),
                 e.sw >= 0 ? kKey[e.sw] : "none", InputName(e.input), e.x / g_axSpm[0], e.y / g_axSpm[1]);
    } else {
        snprintf(buf, sizeof(buf), "laststop kind=none");
    }
    out(buf);
    const char *state = "idle";
    if (g_homePhase == HOME_PEN) {
        state = "pen_up";
    } else if (g_homePhase == HOME_RUN) {
        state = HomingSequencer::StateName(g_seq.GetState());
    } else if (g_homePhase != HOME_NONE) {
        state = "clearpath";
    }
    snprintf(buf, sizeof(buf),
             "home mode=%u state=%s homed=%u x_ends=%lu x_latch=%.3f y_ends=%lu y_latch=%.3f last=%s",
             (unsigned)g_settings.homeMode, state, (unsigned)g_homed, (unsigned long)g_homeEnds[0], g_seq.LatchMm(0),
             (unsigned long)g_homeEnds[1], g_seq.LatchMm(1), g_homeLast);
    out(buf);
}
