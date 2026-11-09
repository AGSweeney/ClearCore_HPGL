/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/*
 * Host tests for Switches.cpp: the switch filter and its edge position, limits blocking motion
 * toward them, contacts, simulated switches, and the homing sequence against simulated motion
 * (sample by sample at 5 kHz, with the ramped stop's overtravel).
 */

#include "Switches.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

static const uint16_t kAllHigh = 0x1FFF;  /* every input high: normally closed switches at rest */

static SwitchConfig Config(int xPlus, int xMinus, int xHome, int yPlus, int yMinus, int yHome, uint8_t no = 0) {
    SwitchConfig c;
    c.pin[SW_X_PLUS] = (int8_t)xPlus;
    c.pin[SW_X_MINUS] = (int8_t)xMinus;
    c.pin[SW_X_HOME] = (int8_t)xHome;
    c.pin[SW_Y_PLUS] = (int8_t)yPlus;
    c.pin[SW_Y_MINUS] = (int8_t)yMinus;
    c.pin[SW_Y_HOME] = (int8_t)yHome;
    c.normallyOpen = no;
    return c;
}

static void TestNames() {
    printf("input and switch names\n");
    CHECK(ParseInputName("DI-7") == 7);
    CHECK(ParseInputName("di7") == 7);
    CHECK(ParseInputName("IO-3") == 3);
    CHECK(ParseInputName("A-12") == 12);
    CHECK(ParseInputName("a9") == 9);
    CHECK(ParseInputName("8") == 8);
    CHECK(ParseInputName("none") == -1);
    CHECK(ParseInputName("-1") == -1);
    CHECK(ParseInputName("DI-6") == -2);  /* the estop's */
    CHECK(ParseInputName("IO-7") == -2);
    CHECK(ParseInputName("A-3") == -2);
    CHECK(ParseInputName("13") == -2);
    CHECK(strcmp(InputName(10), "A-10") == 0);
    CHECK(ParseSwitchName("x+") == SW_X_PLUS);
    CHECK(ParseSwitchName("Y_home") == SW_Y_HOME);
    CHECK(ParseSwitchName("yhome") == SW_Y_HOME);
    CHECK(ParseSwitchName("z+") == -1);
}

static void TestFilterAndLimits() {
    printf("filter, edge position, limit blocks motion toward it only\n");
    SwitchMonitor m;
    m.Configure(Config(7, 8, -1, -1, -1, -1));
    int32_t x = 0;
    for (int i = 0; i < 10; i++) m.Sample(kAllHigh, x, 0);
    CHECK(m.Tripped() == 0);
    /* X+ (DI-7) opens at x = 100 while moving +: tripped after 3 samples, the stop's edge at 100. */
    bool stop = false;
    for (int i = 0; i < 15 && !stop; i++) {
        x += 10;
        const uint16_t lv = x >= 100 ? (uint16_t)(kAllHigh & ~(1u << 7)) : kAllHigh;
        stop = m.Sample(lv, x, 0);
    }
    CHECK(stop);
    SwitchEvent e;
    CHECK(m.TakeEvent(&e));
    CHECK(e.limit && e.sw == SW_X_PLUS && e.x == 100);
    CHECK(m.Tripped() == (1u << SW_X_PLUS));
    /* Still moving + (the ramp): no second stop. */
    const uint16_t open7 = (uint16_t)(kAllHigh & ~(1u << 7));
    CHECK(!m.Sample(open7, x + 5, 0));
    x += 5;
    /* Away from it: allowed. */
    CHECK(!m.Sample(open7, x - 5, 0));
    x -= 5;
    CHECK(!m.TakeEvent(&e));
    /* Toward it again: stops again. */
    CHECK(m.Sample(open7, x + 1, 0));
    x += 1;
    CHECK(m.TakeEvent(&e) && e.limit);
    /* A glitch of 2 samples does not trip X-. */
    for (int i = 0; i < 2; i++) m.Sample((uint16_t)(open7 & ~(1u << 8)), x, 0);
    m.Sample(open7, x, 0);
    CHECK(!(m.Tripped() & (1u << SW_X_MINUS)));
    /* A limit on Y does not stop X motion. */
    SwitchMonitor my;
    my.Configure(Config(-1, -1, -1, 9, -1, -1));
    for (int i = 0; i < 5; i++) my.Sample((uint16_t)(kAllHigh & ~(1u << 9)), i * 10, 0);
    CHECK(my.Tripped() == (1u << SW_Y_PLUS));
    CHECK(!my.TakeEvent(&e));
}

static void TestNormallyOpenAndIgnore() {
    printf("normally open contact, ignored limit\n");
    SwitchMonitor m;
    m.Configure(Config(-1, 3, -1, -1, -1, -1, (uint8_t)(1u << SW_X_MINUS)));
    for (int i = 0; i < 5; i++) m.Sample(0, 0, 0);  /* IO-3 low at rest: not tripped */
    CHECK(m.Tripped() == 0);
    m.SetLimitMask((uint8_t)(1u << SW_X_MINUS));
    bool stop = false;
    for (int i = 1; i < 6; i++) stop |= m.Sample(1u << 3, -i, 0);
    CHECK(m.Tripped() == (1u << SW_X_MINUS));
    CHECK(!stop);  /* ignored while homing onto it */
    m.SetLimitMask(0);
    CHECK(m.Sample(1u << 3, -6, 0));
}

static void TestStopOnSwitch() {
    printf("stop on a switch, stop on release\n");
    SwitchMonitor m;
    m.Configure(Config(-1, -1, 12, -1, -1, -1));
    m.ArmStop((uint8_t)(1u << SW_X_HOME), 0);
    int32_t x = 0;
    bool stop = false;
    while (!stop && x > -1000) {
        x -= 3;
        stop = m.Sample(x <= -500 ? (uint16_t)(kAllHigh & ~(1u << 12)) : kAllHigh, x, 0);
    }
    SwitchEvent e;
    CHECK(stop && m.TakeEvent(&e) && !e.limit && !e.released && e.sw == SW_X_HOME && e.x == -501);
    m.ArmStop(0, (uint8_t)(1u << SW_X_HOME));
    stop = false;
    while (!stop && x < 0) {
        x += 1;
        stop = m.Sample(x <= -500 ? (uint16_t)(kAllHigh & ~(1u << 12)) : kAllHigh, x, 0);
    }
    CHECK(stop && m.TakeEvent(&e) && e.released && e.x == -499 && e.input == 12);

    printf("stop on an input leaving its level (no switch set up)\n");
    SwitchMonitor u;
    u.Configure(Config(-1, -1, -1, -1, -1, -1));
    u.Sample(1u << 9, 0, 0);  /* A-9 high now, A-10 low */
    u.ArmInputStop((1u << 9) | (1u << 10));
    stop = false;
    int32_t p = 0;
    while (!stop && p < 100) {
        p += 2;
        stop = u.Sample(p >= 40 ? (uint16_t)((1u << 9) | (1u << 10)) : (uint16_t)(1u << 9), p, 0);
    }
    CHECK(stop && u.TakeEvent(&e) && e.sw == -1 && e.input == 10 && e.x == 40);
    CHECK(!u.Sample(0, p, 0));  /* disarmed by the stop */
}

static void TestSimulation() {
    printf("simulated switches fixed to the machine\n");
    SwitchMonitor m;
    m.Configure(Config(-1, -1, 7, -1, -1, -1));
    m.Sample(kAllHigh, 1000, 0);
    m.SimAt(7, 0, -1, 400);  /* 400 steps to the - side of here */
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 700, 0);
    CHECK(!(m.Tripped() & (1u << SW_X_HOME)));
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 600, 0);
    CHECK(m.Tripped() & (1u << SW_X_HOME));
    /* The position is set (homing): the switch stays where it is on the machine. */
    m.Shift(-600, 0);  /* the head's 600 becomes 0 */
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 0, 0);
    CHECK(m.Tripped() & (1u << SW_X_HOME));
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 10, 0);  /* 10 back toward + : machine 610 */
    CHECK(!(m.Tripped() & (1u << SW_X_HOME)));
    m.SimLevel(7, 1);
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 10, 0);
    CHECK(m.Tripped() & (1u << SW_X_HOME));
    m.SimClear();
    for (int i = 0; i < 5; i++) m.Sample(kAllHigh, 10, 0);
    CHECK(!(m.Tripped() & (1u << SW_X_HOME)));
}

/* A machine for the homing sequence: one axis moved at a time, sample by sample, through a
 * SwitchMonitor with simulated switches; a stop ramps down at kDecel. */
class SimMachine : public HomingIo {
public:
    static constexpr double kSpm = 40.0;     /* steps per mm */
    static constexpr double kDecel = 6000.0; /* mm/s^2 */
    SwitchMonitor mon;
    double pos[2] = {0, 0};                  /* head, mm (the controller's frame) */
    double mach[2] = {0, 0};                 /* where it really is on the machine, mm */
    int moves = 0;

    void Move(int axis, double mm, double mmPerS, uint8_t onTrip, uint8_t onRelease) override {
        moves++;
        mon.ArmStop(onTrip, onRelease);
        const double dir = mm > 0 ? 1.0 : -1.0;
        const double step = mmPerS / 5000.0;
        double left = fabs(mm);
        bool stopping = false;
        double v = mmPerS;
        while (left > 0 && v > 0) {
            double d = stopping ? v / 5000.0 : step;
            if (d > left) d = left;
            pos[axis] += dir * d;
            mach[axis] += dir * d;
            left -= d;
            if (stopping) {
                v -= kDecel / 5000.0;
            }
            const bool stop = mon.Sample(0x1FFF, (int32_t)lround(pos[0] * kSpm), (int32_t)lround(pos[1] * kSpm));
            if (stop && !stopping) {
                stopping = true;
                left = v * v / (2 * kDecel);  /* the ramp's distance from here */
            }
        }
        for (int i = 0; i < 4; i++) {  /* settle: the filter sees the resting level */
            mon.Sample(0x1FFF, (int32_t)lround(pos[0] * kSpm), (int32_t)lround(pos[1] * kSpm));
        }
    }
    bool Idle() override { return true; }
    uint8_t Tripped() override { return mon.Tripped(); }
    bool TakeEvent(SwitchEvent *e) override { return mon.TakeEvent(e); }
    int32_t Position(int axis) override { return (int32_t)lround(pos[axis] * kSpm); }
    void SetPosition(int axis, int32_t steps) override {
        /* As the controller: the frame moves by a whole number of steps. */
        const int32_t shift = steps - Position(axis);
        pos[axis] += (double)shift / kSpm;
        mon.Shift(axis ? 0 : shift, axis ? shift : 0);
    }
    double StepsPerMm(int) override { return kSpm; }
};

static bool RunHoming(SimMachine &m, HomingSequencer &h, const uint8_t *axes, int n, const HomingAxis cfg[2]) {
    const double plot[2] = {400, 300};
    if (!h.Start(&m, axes, n, cfg, plot)) return false;
    for (int i = 0; i < 100 && h.Poll(); i++) {
    }
    return h.GetState() == HomingSequencer::DONE;
}

static void TestHoming() {
    printf("homing: search, back off, latch, set the position\n");
    HomingAxis cfg[2];
    cfg[0].dir = -1;
    cfg[0].at = -2.0f;  /* the switch's edge is 2 mm left of 0 */
    cfg[1].dir = +1;
    cfg[1].at = 305.0f;
    {
        SimMachine m;
        m.mon.Configure(Config(-1, -1, 7, -1, -1, 8));
        m.mon.Sample(0x1FFF, 0, 0);
        m.mon.SimAt(7, 0, -1, (int32_t)(123.4 * SimMachine::kSpm));  /* 123.4 mm to the left */
        m.mon.SimAt(8, 1, +1, (int32_t)(57.0 * SimMachine::kSpm));
        HomingSequencer h;
        const uint8_t axes[2] = {0, 1};
        CHECK(RunHoming(m, h, axes, 2, cfg));
        /* The machine point 123.4 mm left of the start is now X = -2; 57 mm up is Y = 305. */
        const double xEdge = m.pos[0] - (m.mach[0] - (-123.4));
        const double yEdge = m.pos[1] - (m.mach[1] - 57.0);
        printf("  X edge at %.4f mm (want -2), Y edge at %.4f mm (want 305)\n", xEdge, yEdge);
        CHECK(fabs(xEdge - -2.0) < 0.05);
        CHECK(fabs(yEdge - 305.0) < 0.05);
        CHECK(h.Completed(0) == 1 && h.Completed(1) == 1);
        /* Home again from where it ended: the same within the latch resolution. */
        double first = xEdge;
        for (int k = 0; k < 4; k++) {
            CHECK(RunHoming(m, h, axes, 1, cfg));
            const double e = m.pos[0] - (m.mach[0] - (-123.4));
            CHECK(fabs(e - first) < 0.03);
        }
    }
    {
        printf("homing: already on the switch\n");
        SimMachine m;
        m.mon.Configure(Config(-1, -1, 7, -1, -1, -1));
        m.mon.Sample(0x1FFF, 0, 0);
        m.mon.SimAt(7, 0, -1, -(int32_t)(5 * SimMachine::kSpm));  /* the edge 5 mm to the RIGHT, side -: on it now */
        for (int i = 0; i < 5; i++) m.mon.Sample(0x1FFF, 0, 0);
        CHECK(m.mon.Tripped() & (1u << SW_X_HOME));
        HomingSequencer h;
        const uint8_t axes[1] = {0};
        CHECK(RunHoming(m, h, axes, 1, cfg));
        const double xEdge = m.pos[0] - (m.mach[0] - 5.0);
        CHECK(fabs(xEdge - -2.0) < 0.05);
    }
    {
        printf("homing: not found, limit first\n");
        SimMachine m;
        m.mon.Configure(Config(-1, -1, 7, -1, -1, -1));
        m.mon.Sample(0x1FFF, 0, 0);
        HomingSequencer h;
        const uint8_t axes[1] = {0};
        HomingAxis c[2] = {cfg[0], cfg[1]};
        c[0].travel = 50.0f;
        CHECK(!RunHoming(m, h, axes, 1, c));
        CHECK(h.GetState() == HomingSequencer::FAILED && strstr(h.Error(), "not found"));

        SimMachine m2;
        m2.mon.Configure(Config(-1, 8, 7, -1, -1, -1));
        m2.mon.Sample(0x1FFF, 0, 0);
        m2.mon.SimAt(8, 0, -1, (int32_t)(30 * SimMachine::kSpm));   /* X- limit at 30 mm */
        m2.mon.SimAt(7, 0, -1, (int32_t)(60 * SimMachine::kSpm));   /* home beyond it */
        HomingSequencer h2;
        CHECK(!RunHoming(m2, h2, axes, 1, cfg));
        CHECK(h2.GetState() == HomingSequencer::FAILED && strstr(h2.Error(), "limit X-"));
    }
    {
        printf("homing: on a limit switch (home = X- input)\n");
        SimMachine m;
        m.mon.Configure(Config(-1, 8, 8, -1, -1, -1));
        m.mon.SetLimitMask((uint8_t)(1u << SW_X_MINUS));
        m.mon.Sample(0x1FFF, 0, 0);
        m.mon.SimAt(8, 0, -1, (int32_t)(80 * SimMachine::kSpm));
        HomingSequencer h;
        const uint8_t axes[1] = {0};
        CHECK(RunHoming(m, h, axes, 1, cfg));
        const double xEdge = m.pos[0] - (m.mach[0] - (-80.0));
        CHECK(fabs(xEdge - -2.0) < 0.05);
    }
}

int main() {
    TestNames();
    TestFilterAndLimits();
    TestNormallyOpenAndIgnore();
    TestStopOnSwitch();
    TestSimulation();
    TestHoming();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
