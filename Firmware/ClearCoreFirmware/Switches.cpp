/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#include "Switches.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Names                                                                      */
/* ------------------------------------------------------------------------- */

bool InputCodeValid(int code) {
    return (code >= 0 && code <= 5) || (code >= 7 && code <= kInputCodeMax);
}

const char *InputName(int code) {
    static const char *const kNames[] = {"IO-0", "IO-1", "IO-2", "IO-3", "IO-4", "IO-5", "DI-6",
                                         "DI-7", "DI-8", "A-9",  "A-10", "A-11", "A-12"};
    if (code < 0 || code > kInputCodeMax) {
        return "none";
    }
    return kNames[code];
}

int ParseInputName(const char *text) {
    if (!text || !*text) {
        return -2;
    }
    if (strcmp(text, "-1") == 0) {
        return -1;
    }
    char t[12];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(t); p++) {
        if (*p != '-' && *p != '_' && *p != ' ') {
            t[n++] = (char)tolower((unsigned char)*p);
        }
    }
    t[n] = '\0';
    if (strcmp(t, "none") == 0) {
        return -1;
    }
    const char *digits = t;
    if (strncmp(t, "io", 2) == 0) {
        digits = t + 2;
    } else if (strncmp(t, "di", 2) == 0) {
        digits = t + 2;
    } else if (t[0] == 'a') {
        digits = t + 1;
    }
    if (!*digits) {
        return -2;
    }
    for (const char *p = digits; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -2;
        }
    }
    const int v = atoi(digits);
    /* The prefix has to fit the number: IO-0..5, DI-7/8, A-9..12 (a bare number is any of them). */
    if (strncmp(t, "io", 2) == 0 && v > 5) return -2;
    if (strncmp(t, "di", 2) == 0 && v != 7 && v != 8) return -2;
    if (t[0] == 'a' && (v < 9 || v > 12)) return -2;
    return InputCodeValid(v) ? v : -2;
}

const char *SwitchName(int sw) {
    static const char *const kNames[] = {"X+", "X-", "X home", "Y+", "Y-", "Y home"};
    return (sw >= 0 && sw < SW_COUNT) ? kNames[sw] : "?";
}

int ParseSwitchName(const char *text) {
    if (!text) {
        return -1;
    }
    char t[12];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(t); p++) {
        if (*p != '_' && *p != ' ') {
            t[n++] = (char)tolower((unsigned char)*p);
        }
    }
    t[n] = '\0';
    static const char *const kKeys[] = {"x+", "x-", "xhome", "y+", "y-", "yhome"};
    for (int i = 0; i < SW_COUNT; i++) {
        if (strcmp(t, kKeys[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* SwitchMonitor                                                              */
/* ------------------------------------------------------------------------- */

void SwitchMonitor::Configure(const SwitchConfig &config) {
    m_cfg = config;
    for (int i = 0; i < SW_COUNT; i++) {
        if (m_cfg.pin[i] >= 0 && !InputCodeValid(m_cfg.pin[i])) {
            m_cfg.pin[i] = -1;
        }
        m_count[i] = 0;
        m_limitLatched[i] = false;
    }
    m_tripped = 0;  /* refiltered from the next samples */
}

bool SwitchMonitor::LevelTripped(int sw, uint16_t levels) const {
    const int code = m_cfg.pin[sw];
    if (code < 0) {
        return false;
    }
    const bool high = (levels >> code) & 1u;
    return ((m_cfg.normallyOpen >> sw) & 1u) ? high : !high;
}

void SwitchMonitor::ArmStop(uint8_t onTrip, uint8_t onRelease) {
    m_stopTrip = onTrip;
    m_stopRelease = onRelease;
}

void SwitchMonitor::ArmInputStop(uint16_t codes) {
    m_inStop = 0;
    m_inBase = m_levels;
    for (int c = 0; c <= kInputCodeMax; c++) {
        m_inCount[c] = 0;
    }
    m_inStop = codes;
}

bool SwitchMonitor::TakeEvent(SwitchEvent *e) {
    if (!m_eventNew) {
        return false;
    }
    *e = m_event;
    m_eventNew = false;
    return true;
}

bool SwitchMonitor::Sample(uint16_t levels, int32_t x, int32_t y, bool idle) {
    if (!m_havePrev) {
        m_prevX = x;
        m_prevY = y;
        m_havePrev = true;
    }
    const int32_t sx = m_shiftX, sy = m_shiftY;
    m_shiftX -= sx;
    m_shiftY -= sy;
    const int32_t dx = x - m_prevX - sx;
    const int32_t dy = y - m_prevY - sy;
    m_prevX = x;
    m_prevY = y;
    m_machX += dx;
    m_machY += dy;

    /* Simulated inputs. */
    for (int code = 0; code <= kInputCodeMax; code++) {
        const Sim &s = m_sim[code];
        if (s.mode == 0) {
            continue;
        }
        bool high;
        if (s.mode == 1) {
            high = false;
        } else if (s.mode == 2) {
            high = true;
        } else {
            const int64_t pos = s.axis == 0 ? m_machX : m_machY;
            const bool tripped = s.side > 0 ? pos >= s.at : pos <= s.at;
            bool open = false;  /* the contact of the switch on this input */
            for (int sw = 0; sw < SW_COUNT; sw++) {
                if (m_cfg.pin[sw] == code) {
                    open = (m_cfg.normallyOpen >> sw) & 1u;
                    break;
                }
            }
            high = open ? tripped : !tripped;
        }
        levels = high ? (uint16_t)(levels | (1u << code)) : (uint16_t)(levels & ~(1u << code));
    }
    m_levels = levels;

    bool stop = false;
    const uint16_t inStop = m_inStop;
    if (inStop) {
        const uint16_t diff = (uint16_t)((levels ^ m_inBase) & inStop);
        for (int c = 0; c <= kInputCodeMax; c++) {
            if (!((inStop >> c) & 1u)) {
                continue;
            }
            if (!((diff >> c) & 1u)) {
                m_inCount[c] = 0;
                continue;
            }
            if (m_inCount[c] == 0) {
                m_inEdgeX[c] = x;
                m_inEdgeY[c] = y;
            }
            if (++m_inCount[c] >= kFilter && !m_eventNew) {
                m_event.sw = -1;
                m_event.input = (int8_t)c;
                m_event.limit = false;
                m_event.released = false;
                m_event.x = m_inEdgeX[c];
                m_event.y = m_inEdgeY[c];
                m_eventNew = true;
                m_inStop = 0;
                stop = true;
                break;
            }
        }
    }
    for (int sw = 0; sw < SW_COUNT; sw++) {
        const uint8_t bit = (uint8_t)(1u << sw);
        if (m_cfg.pin[sw] < 0) {
            m_tripped &= (uint8_t)~bit;
            m_count[sw] = 0;
            continue;
        }
        const bool raw = LevelTripped(sw, levels);
        const bool was = (m_tripped & bit) != 0;
        bool changed = false;
        if (raw != was) {
            if (m_count[sw] == 0) {
                m_edgeX[sw] = x;  /* the first sample of the change: where the edge is */
                m_edgeY[sw] = y;
            }
            if (++m_count[sw] >= kFilter) {
                m_tripped ^= bit;
                m_count[sw] = 0;
                changed = true;
            }
        } else {
            m_count[sw] = 0;
        }
        const bool now = (m_tripped & bit) != 0;

        if (changed && !m_eventNew) {
            if (now && (m_stopTrip & bit)) {
                m_event.sw = (int8_t)sw;
                m_event.input = m_cfg.pin[sw];
                m_event.limit = false;
                m_event.released = false;
                m_event.x = m_edgeX[sw];
                m_event.y = m_edgeY[sw];
                m_eventNew = true;
                m_stopTrip = 0;
                m_stopRelease = 0;
                m_inStop = 0;
                stop = true;
            } else if (!now && (m_stopRelease & bit)) {
                m_event.sw = (int8_t)sw;
                m_event.input = m_cfg.pin[sw];
                m_event.limit = false;
                m_event.released = true;
                m_event.x = m_edgeX[sw];
                m_event.y = m_edgeY[sw];
                m_eventNew = true;
                m_stopTrip = 0;
                m_stopRelease = 0;
                m_inStop = 0;
                stop = true;
            }
        }

        /* A limit blocks motion toward it; away is how to get off it. */
        const int side = SwitchSide(sw);
        if (side == 0) {
            continue;
        }
        const int32_t d = SwitchAxis(sw) == 0 ? dx : dy;
        if (!now || d * side < 0 || idle) {
            m_limitLatched[sw] = false;  /* released, or going away: the next approach stops again */
        }
        if (now && d * side > 0 && !m_limitLatched[sw] && !(m_limitIgnore & bit)) {
            m_limitLatched[sw] = true;
            if (!m_eventNew || !m_event.limit) {
                m_event.sw = (int8_t)sw;
                m_event.input = m_cfg.pin[sw];
                m_event.limit = true;
                m_event.released = false;
                m_event.x = m_edgeX[sw];
                m_event.y = m_edgeY[sw];
                m_eventNew = true;
            }
            m_stopTrip = 0;
            m_stopRelease = 0;
                m_inStop = 0;
            stop = true;
        }
    }
    return stop;
}

void SwitchMonitor::SimLevel(int code, int mode) {
    if (code < 0 || code > kInputCodeMax) {
        return;
    }
    m_sim[code].mode = (uint8_t)(mode == 1 ? 1 : (mode == 2 ? 2 : 0));
}

void SwitchMonitor::SimAt(int code, int axis, int side, int32_t dist) {
    if (code < 0 || code > kInputCodeMax) {
        return;
    }
    Sim &s = m_sim[code];
    s.axis = (uint8_t)(axis ? 1 : 0);
    s.side = (int8_t)(side < 0 ? -1 : 1);
    s.at = (axis ? m_machY : m_machX) + (int64_t)s.side * dist;  /* dist < 0: the head is past it now */
    s.mode = 3;
}

void SwitchMonitor::SimClear() {
    for (int code = 0; code <= kInputCodeMax; code++) {
        m_sim[code].mode = 0;
    }
}

bool SwitchMonitor::Simulated(int code) const {
    return code >= 0 && code <= kInputCodeMax && m_sim[code].mode != 0;
}

/* ------------------------------------------------------------------------- */
/* HomingSequencer                                                            */
/* ------------------------------------------------------------------------- */

const char *HomingSequencer::StateName(State s) {
    switch (s) {
    case IDLE: return "idle";
    case CLEAR: return "clearing";
    case SEARCH: return "searching";
    case BACK: return "backing_off";
    case BACK_MORE: return "backing_off";
    case LATCH: return "latching";
    case DONE: return "done";
    case FAILED: return "failed";
    }
    return "?";
}

bool HomingSequencer::Start(HomingIo *io, const uint8_t *axes, int count, const HomingAxis cfg[2],
                            const double plotMm[2]) {
    if (!io || count < 1 || count > 2) {
        return false;
    }
    m_io = io;
    m_cfg[0] = cfg[0];
    m_cfg[1] = cfg[1];
    m_plot[0] = plotMm[0];
    m_plot[1] = plotMm[1];
    m_count = count;
    for (int i = 0; i < count; i++) {
        m_axes[i] = axes[i] ? 1 : 0;
    }
    m_index = 0;
    m_error = nullptr;
    BeginAxis();
    return true;
}

void HomingSequencer::Next(State s) {
    m_state = s;
    m_moving = true;
}

void HomingSequencer::BeginAxis() {
    const int axis = m_axes[m_index];
    const HomingAxis &c = m_cfg[axis];
    const uint8_t home = (uint8_t)(1u << (axis ? SW_Y_HOME : SW_X_HOME));
    const double travel = c.travel > 0 ? c.travel : m_plot[axis] * 1.2 + 20.0;
    SwitchEvent stale;
    while (m_io->TakeEvent(&stale)) {
    }
    if (m_io->Tripped() & home) {
        /* Already on the switch: off it first, away from it. */
        Next(CLEAR);
        m_io->Move(axis, -c.dir * travel, c.fast, 0, home);
    } else {
        Next(SEARCH);
        m_io->Move(axis, c.dir * travel, c.fast, home, 0);
    }
}

void HomingSequencer::Fail(const char *why) {
    m_error = why;
    m_state = FAILED;
    m_moving = false;
}

void HomingSequencer::Abort() {
    if (m_state != IDLE && m_state != DONE && m_state != FAILED) {
        Fail("homing stopped");
    }
}

bool HomingSequencer::Poll() {
    if (m_state == IDLE || m_state == DONE || m_state == FAILED) {
        return false;
    }
    if (!m_io->Idle()) {
        return true;  /* the move, or the ramped stop after a switch, is still running */
    }
    SwitchEvent e;
    const bool haveEvent = m_io->TakeEvent(&e);
    const int axis = m_axes[m_index];
    const HomingAxis &c = m_cfg[axis];
    const int homeSw = axis ? SW_Y_HOME : SW_X_HOME;
    const uint8_t home = (uint8_t)(1u << homeSw);
    if (haveEvent && e.limit) {
        static char msg[48];
        msg[0] = '\0';
        strcat(msg, "limit ");
        strcat(msg, SwitchName(e.sw));
        strcat(msg, " reached before the home switch");
        Fail(msg);
        return false;
    }
    switch (m_state) {
    case CLEAR:
        if (!haveEvent || e.sw != homeSw || !e.released) {
            Fail(axis ? "Y home switch did not release" : "X home switch did not release");
            return false;
        }
        Next(BACK_MORE);
        m_io->Move(axis, -c.dir * c.back, c.fast, 0, 0);
        return true;
    case SEARCH:
        if (!haveEvent || e.sw != homeSw || e.released) {
            Fail(axis ? "Y home switch not found within the search travel"
                      : "X home switch not found within the search travel");
            return false;
        }
        Next(BACK);
        m_io->Move(axis, -c.dir * (c.back * 4.0 + 20.0), c.fast, 0, home);
        return true;
    case BACK:
        if (!haveEvent || e.sw != homeSw || !e.released) {
            Fail(axis ? "Y home switch did not release" : "X home switch did not release");
            return false;
        }
        Next(BACK_MORE);
        m_io->Move(axis, -c.dir * c.back, c.fast, 0, 0);
        return true;
    case BACK_MORE:
        Next(LATCH);
        m_io->Move(axis, c.dir * (c.back * 2.0 + 5.0), c.slow, home, 0);
        return true;
    case LATCH: {
        if (!haveEvent || e.sw != homeSw || e.released) {
            Fail(axis ? "Y home switch lost on the slow approach" : "X home switch lost on the slow approach");
            return false;
        }
        const double spm = m_io->StepsPerMm(axis);
        const int32_t latch = axis ? e.y : e.x;
        const int32_t now = m_io->Position(axis);
        const int32_t at = (int32_t)(c.at * spm + (c.at * spm >= 0 ? 0.5 : -0.5));
        m_latch[axis] = (double)latch / spm;
        m_io->SetPosition(axis, at + (now - latch));
        m_done[axis]++;
        if (++m_index < m_count) {
            BeginAxis();
            return true;
        }
        m_state = DONE;
        m_moving = false;
        return false;
    }
    default:
        return false;
    }
}
