/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#ifndef __CLEARHPGL_SWITCHES_H__
#define __CLEARHPGL_SWITCHES_H__

/*
 * Limit and home switches, and the homing sequence. Portable C++ (no ClearCore headers): the host
 * tests in Firmware/tests run it against simulated motion.
 *
 * Switches belong to the head's axes (HP-GL X and Y, after dir_x / dir_y / swap_xy), not to the
 * motors, so the same rules hold on a CoreXY machine, where one axis moves both motors.
 *
 *   SwitchMonitor   every sample (the 5 kHz step interrupt): reads the input levels and the head's
 *                   position, filters each switch (tripped after 3 samples, the position taken at the
 *                   first), stops motion toward a tripped limit, stops a "stop on a switch" move, and
 *                   simulates switches for the bench.
 *   HomingSequencer the main loop: one axis at a time, off the switch, search, back off, latch, set
 *                   the position.
 */

#include <stdint.h>

enum SwitchId : uint8_t {
    SW_X_PLUS = 0,
    SW_X_MINUS,
    SW_X_HOME,
    SW_Y_PLUS,
    SW_Y_MINUS,
    SW_Y_HOME,
    SW_COUNT,
};

/* Board inputs a switch can use, by code: 0..5 IO-0..IO-5, 7 DI-7, 8 DI-8, 9..12 A-9..A-12.
 * DI-6 is the estop's. -1 = none. */
static const int kInputCodeMax = 12;
bool InputCodeValid(int code);
/* "DI-7", "IO-3", "A-9", "none". */
const char *InputName(int code);
/* Accepts the names above (any case, with or without the dash), the code as a number, "none". -2 if unknown. */
int ParseInputName(const char *text);
/* "X+", "X-", "X home", ... */
const char *SwitchName(int sw);
/* Switch by name ("x+", "x-", "xhome", "x_home", ...); -1 if unknown. */
int ParseSwitchName(const char *text);
static inline int SwitchAxis(int sw) { return sw < SW_Y_PLUS ? 0 : 1; }
static inline bool SwitchIsHome(int sw) { return sw == SW_X_HOME || sw == SW_Y_HOME; }
/* +1 for a + limit, -1 for a - limit, 0 for a home switch. */
static inline int SwitchSide(int sw) {
    const int k = sw % 3;
    return k == 0 ? 1 : (k == 1 ? -1 : 0);
}

struct SwitchConfig {
    int8_t pin[SW_COUNT];      /* input code, -1 none */
    uint8_t normallyOpen;      /* bit per switch: tripped when high (default: closed, tripped when low) */
};

/* What stopped a move: the switch, the head's position at its first tripped (or released) sample. */
struct SwitchEvent {
    int8_t sw = -1;            /* -1: an input stop (ArmInputStop), see input */
    int8_t input = -1;         /* the input code */
    bool limit = false;        /* a limit stopped motion toward it (a fault) */
    bool released = false;     /* a stop-on-release move: the switch let go */
    int32_t x = 0, y = 0;      /* head position, axis steps */
};

class SwitchMonitor {
public:
    static const uint8_t kFilter = 3;  /* samples in a row to change state (0.6 ms) */

    void Configure(const SwitchConfig &config);
    const SwitchConfig &Config() const { return m_cfg; }

    /* One sample. levels: bit per input code, 1 = high. x, y: head position, axis steps.
     * idle: nothing is moving or queued (a limit stops the next approach again).
     * Returns true when this sample asks for a stop; TakeEvent() says why. */
    bool Sample(uint16_t levels, int32_t x, int32_t y, bool idle = false);

    /* Filtered state, bit per switch. */
    uint8_t Tripped() const { return m_tripped; }
    /* Input levels after simulation, bit per code. */
    uint16_t Levels() const { return m_levels; }

    /* Limits: suppress motion-toward stops for these switches (a home switch shared with a limit,
     * while homing toward it). */
    void SetLimitMask(uint8_t ignore) { m_limitIgnore = ignore; }
    /* Stop on any of these switches tripping, or (onRelease) any of them letting go. 0 = none. */
    void ArmStop(uint8_t onTrip, uint8_t onRelease);
    /* Stop when any of these inputs (bit per code) leaves the level it reads now (filtered as a
     * switch). For finding which switch is which before any is set up. 0 = none. */
    void ArmInputStop(uint16_t codes);
    /* The latest stop, and whether one is unread. */
    bool TakeEvent(SwitchEvent *e);
    const SwitchEvent &LastEvent() const { return m_event; }

    /* The head's position was set (origin, homing): its frame moved by dx, dy steps. Taken off the
     * next sample's change, so a move that starts in that same sample still counts in full. */
    void Shift(int32_t dx, int32_t dy) {
        m_shiftX += dx;
        m_shiftY += dy;
    }
    /* The frame changed (scale, direction): the next sample is the new reference. */
    void Resync() {
        m_havePrev = false;
        m_shiftX = 0;
        m_shiftY = 0;
    }

    /* Bench simulation of an input. mode: 0 clear, 1 forced low, 2 forced high. */
    void SimLevel(int code, int mode);
    /* A switch fixed to the machine: the input reads tripped while the head is at least dist steps
     * from where it is now, on side (+1 / -1) of axis (0 X, 1 Y); a negative dist puts the head
     * past it already. The level it gives follows the contact of the switch that uses the input
     * (normally closed when none). */
    void SimAt(int code, int axis, int side, int32_t dist);
    void SimClear();
    bool Simulated(int code) const;

private:
    bool LevelTripped(int sw, uint16_t levels) const;

    SwitchConfig m_cfg = {{-1, -1, -1, -1, -1, -1}, 0};
    uint8_t m_tripped = 0;
    uint8_t m_count[SW_COUNT] = {0};      /* samples in a row against the filtered state */
    int32_t m_edgeX[SW_COUNT] = {0}, m_edgeY[SW_COUNT] = {0};
    uint16_t m_levels = 0;
    int32_t m_prevX = 0, m_prevY = 0;
    bool m_havePrev = false;
    volatile int32_t m_shiftX = 0, m_shiftY = 0;
    int64_t m_machX = 0, m_machY = 0;     /* machine position: never reset (simulated switches) */
    volatile uint8_t m_limitIgnore = 0;
    volatile uint8_t m_stopTrip = 0, m_stopRelease = 0;
    SwitchEvent m_event;
    volatile bool m_eventNew = false;
    bool m_limitLatched[SW_COUNT] = {false};  /* one stop per approach */
    volatile uint16_t m_inStop = 0;
    uint16_t m_inBase = 0;
    uint8_t m_inCount[kInputCodeMax + 1] = {0};
    int32_t m_inEdgeX[kInputCodeMax + 1] = {0}, m_inEdgeY[kInputCodeMax + 1] = {0};

    struct Sim {
        uint8_t mode = 0;     /* 0 none, 1 low, 2 high, 3 at */
        uint8_t axis = 0;
        int8_t side = 0;
        int64_t at = 0;       /* machine position */
    } m_sim[kInputCodeMax + 1];
};

/* Homing settings of one axis, in mm and mm/s. */
struct HomingAxis {
    int8_t dir = -1;          /* toward the home switch */
    float fast = 20.0f;       /* search speed */
    float slow = 2.0f;        /* latch speed */
    float back = 3.0f;        /* back-off after the switch releases */
    float at = 0.0f;          /* the axis's position at the switch's edge */
    float travel = 0.0f;      /* longest search; 0 = from the plot area */
};

/* What the sequencer needs from the machine. */
class HomingIo {
public:
    virtual ~HomingIo() {}
    /* Move one axis (0 X, 1 Y) by mm (signed) at mm/s, pen up, stopping on the armed switches. */
    virtual void Move(int axis, double mm, double mmPerS, uint8_t stopOnTrip, uint8_t stopOnRelease) = 0;
    virtual bool Idle() = 0;
    virtual uint8_t Tripped() = 0;
    virtual bool TakeEvent(SwitchEvent *e) = 0;
    /* Head position of an axis, axis steps. */
    virtual int32_t Position(int axis) = 0;
    /* The axis's position becomes steps (stopped). Whole steps, so homing again lands on the same
     * step: a fractional frame would let the edge creep by a step per homing. */
    virtual void SetPosition(int axis, int32_t steps) = 0;
    /* Steps per mm of an axis (signed by dir). */
    virtual double StepsPerMm(int axis) = 0;
};

class HomingSequencer {
public:
    enum State : uint8_t { IDLE, CLEAR, SEARCH, BACK, BACK_MORE, LATCH, DONE, FAILED };

    /* Home the axes in order (one or two entries, 0 X / 1 Y). homePin: whether each axis has a
     * home switch; limitOnSide: the limit on the search side, when it is a different switch. */
    bool Start(HomingIo *io, const uint8_t *axes, int count, const HomingAxis cfg[2], const double plotMm[2]);
    /* Main loop. Returns true while running. */
    bool Poll();
    void Abort();
    State GetState() const { return m_state; }
    int Axis() const { return m_count > 0 && m_index < m_count ? m_axes[m_index] : -1; }
    const char *Error() const { return m_error; }
    /* The latched edge of the last homing of each axis (mm, before the position was set), and
     * how many homings completed since power-up. */
    double LatchMm(int axis) const { return m_latch[axis]; }
    uint32_t Completed(int axis) const { return m_done[axis]; }
    static const char *StateName(State s);

private:
    void BeginAxis();
    void Fail(const char *why);
    void Next(State s);

    HomingIo *m_io = nullptr;
    HomingAxis m_cfg[2];
    double m_plot[2] = {0, 0};
    uint8_t m_axes[2] = {0, 1};
    int m_count = 0, m_index = 0;
    State m_state = IDLE;
    bool m_moving = false;
    const char *m_error = nullptr;
    double m_latch[2] = {0, 0};
    uint32_t m_done[2] = {0, 0};
};

#endif
