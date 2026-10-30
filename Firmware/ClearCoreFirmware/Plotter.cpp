/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Plotter.h"

#include "ClearCore.h"
#include "CoordinatedMotionController.h"
#include "PlotterConfig.h"
#include "Settings.h"

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum ActionType : uint8_t {
    ACT_LINE,
    ACT_PEN_UP,
    ACT_PEN_DOWN,
};

struct Action {
    uint8_t type;
    int32_t x;
    int32_t y;
    uint32_t speed;
};

static const uint32_t kPenZTimeoutMs = 5000;

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
static uint32_t g_segments = 0;
static uint32_t g_dropped = 0;
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

static uint16_t LineRunAtHead(uint16_t limit) {
    uint16_t n = 0;
    while (n < g_count && n < limit) {
        if (g_actions[(g_head + n) % CHPGL_ACTION_QUEUE].type != ACT_LINE) {
            break;
        }
        n++;
    }
    return n;
}

static void QueueHeadLine() {
    const Action &a = Head();
    g_xy.ArcVelMax(a.speed);
    if (g_xy.QueueLinear(a.x, a.y)) {
        g_segments++;
    } else {
        g_dropped++;  /* zero-length after rounding, or rejected by the planner */
    }
    Pop();
}

static void FeedXy() {
    const uint8_t cap = g_xy.PlannerQueueCapacity();
    if (XyIdle()) {
        const uint16_t run = LineRunAtHead((uint16_t)(cap - 1));
        if (run == 0) {
            return;
        }
        const bool runEnds = run < g_count;  /* a pen change follows */
        if (run < cap - 1 && !runEnds && (Milliseconds() - g_lastPushMs) < CHPGL_START_WAIT_MS) {
            return;  /* give the host a moment so the planner sees more than one segment */
        }
        g_xy.SetDeferMotionQueueStart(true);
        for (uint16_t i = 0; i < run; i++) {
            QueueHeadLine();
        }
        g_xy.SetDeferMotionQueueStart(false);
        g_xy.StartDeferredMotionIfPending();
        return;
    }
    while (g_count > 0 && Head().type == ACT_LINE && g_xy.MotionQueueCount() + 1 < cap) {
        QueueHeadLine();
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
    }
    void PenDown() override {
        Push(ACT_PEN_DOWN, 0, 0, 0);
        g_queuedPenDown = true;
    }
    void MoveTo(double xPu, double yPu) override {
        const int32_t sx = (int32_t)lround(xPu * g_stepsPerPu[0]);
        const int32_t sy = (int32_t)lround(yPu * g_stepsPerPu[1]);
        if (sx == g_tailX && sy == g_tailY) {
            return;
        }
        Push(ACT_LINE, sx, sy, g_queuedPenDown ? g_drawSps : g_travelSps);
        g_tailX = sx;
        g_tailY = sy;
    }
    void SetVelocity(double mmPerSec) override {
        double v = (mmPerSec <= 0) ? g_settings.drawMmS : mmPerSec;
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
        *xPu = (double)g_xy.CurrentX() / g_stepsPerPu[0];
        *yPu = (double)g_xy.CurrentY() / g_stepsPerPu[1];
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
    g_xy.ArcAccelMax((uint32_t)(g_settings.accelMmS2 * spmmX));
    g_xy.JunctionDVmax((uint32_t)(g_settings.cornerMmS * spmmX));
    for (MotorDriver *m : {&ConnectorM0, &ConnectorM1}) {
        m->EStopDecelMax((uint32_t)(g_settings.accelMmS2 * spmmX * 2.0));
    }
    if (!g_enabled) {
        ConnectorM2.VelMax(g_settings.zVelSteps);
        ConnectorM2.AccelMax(g_settings.zAccelSteps);
        InitPenHardware();
        SyncTailFromMotion();
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
    g_xy.SetPosition(0, 0);
    PlotterApplySettings();
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

const char *PlotterDisable() {
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
    if (g_enabled && !XyIdle()) {
        g_xy.StopDecel();
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

bool PlotterIdle() {
    return g_count == 0 && XyIdle() && !g_penBusy && !g_abortStop;
}

const char *PlotterSetOrigin() {
    if (!PlotterIdle()) {
        return "plotter busy";
    }
    g_xy.SetPosition(0, 0);
    SyncTailFromMotion();
    return nullptr;
}

void PlotterClearFault() {
    g_fault = nullptr;
}

const char *PlotterFault() {
    return g_fault;
}

void PlotterPoll() {
    if (!g_ready) {
        return;
    }
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
        if (!XyIdle()) {
            return;
        }
        g_abortStop = false;
        SyncTailFromMotion();
        if (g_physPenDown) {
            g_sink.PenUp();
        }
    }

    PollPen();
    if (g_penBusy) {
        return;
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
    if (a.type == ACT_LINE) {
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

void PlotterStatus(char *buf, size_t len) {
    const char *state;
    if (g_fault) {
        state = "fault";
    } else if (!g_enabled) {
        state = (g_count > 0) ? "waiting_enable" : "disabled";
    } else if (g_paused) {
        state = "paused";
    } else if (PlotterIdle()) {
        state = "idle";
    } else {
        state = "plotting";
    }
    const double x = (double)g_xy.CurrentX() / g_stepsPerPu[0] / HPGL_PU_PER_MM;
    const double y = (double)g_xy.CurrentY() / g_stepsPerPu[1] / HPGL_PU_PER_MM;
    snprintf(buf, len,
             "state=%s enabled=%d x_mm=%.3f y_mm=%.3f pen=%s sel_pen=%d queue=%u planner=%u "
             "segments=%lu dropped=%lu pages=%lu estop=%s fault=%s",
             state, g_enabled ? 1 : 0, x, y, g_physPenDown ? "down" : "up", g_pen,
             (unsigned)g_count, (unsigned)g_xy.MotionQueueCount(), (unsigned long)g_segments,
             (unsigned long)g_dropped, (unsigned long)g_pages, EstopTripped() ? "open" : "ok",
             g_fault ? g_fault : "none");
}
