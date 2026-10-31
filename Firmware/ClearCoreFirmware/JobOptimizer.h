/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

/*
 * JobOptimizer: buffers a whole HP-GL job and replays it in a better order.
 *
 * Sits between the interpreter and the plotter as an HpglSink. Off, it forwards every call.
 * On, it records the pen-down strokes (physical plotter units, after scaling, rotation and
 * clipping) instead of plotting them. At the end of the job (or a page) it
 *   - joins strokes whose ends meet into paths (outlines drawn as separate segments),
 *   - orders them inside-out: a path is drawn only after everything it encloses, so outlines
 *     come last (a cutter would otherwise free a part before its interior is cut),
 *   - and among the paths that are ready draws the nearest next, entering a closed contour at
 *     its nearest vertex and an open path at its nearer end,
 * then replays them to the plotter as fast as its queue accepts. Pen or speed changes split the
 * job into groups that keep their order. A job too large for the buffer is replayed as received
 * and the rest streams through, so nothing is lost or reordered wrongly.
 *
 * Paths are lists of (stroke, direction) over the received points: nothing is copied, which
 * keeps RAM for the network stack's heap (each TCP connection allocates ~6 KB).
 *
 * Same ordering rules as the ClearHPGL app's DXF import (src/DxfImport.cpp). No ClearCore
 * dependencies: host-tested in tests/test_hpgl.cpp.
 */
#ifndef CLEARHPGL_JOB_OPTIMIZER_H
#define CLEARHPGL_JOB_OPTIMIZER_H

#include "Hpgl.h"

#include <stdint.h>

class JobOptimizer : public HpglSink {
public:
    static const uint32_t kMaxPoints = 8000;  /* per page */
    static const uint32_t kMaxStrokes = 500;

    void Init(HpglSink *out);

    /* A job starts; optimize = buffer and reorder it, else pass through. */
    void BeginJob(bool optimize);
    /* The job's data has ended: order and replay what was buffered. */
    void EndJob();
    /* Abort, fault, ESC.K: drop everything buffered and stop replaying. */
    void Abort();
    /* Replay step; call from the main loop. */
    void Pump();

    /* Buffering a job (nothing plotted yet). */
    bool Buffering() const { return m_mode == M_COLLECT && m_strokeCount > 0; }
    /* Buffering or replaying: the job is not finished. */
    bool Busy() const { return m_mode == M_COLLECT || m_mode == M_REPLAY; }
    /* Replaying: the next job has to wait. */
    bool Replaying() const { return m_mode == M_REPLAY; }

    /* Last job: strokes received, paths after joining, pen-up travel before and after (mm). */
    uint32_t LastStrokes() const { return m_statStrokes; }
    uint32_t LastPaths() const { return m_statPaths; }
    float LastTravelBeforeMm() const { return m_statBefore; }
    float LastTravelAfterMm() const { return m_statAfter; }
    bool LastOverflowed() const { return m_statOverflow; }

    /* HpglSink */
    void PenUp() override;
    void PenDown() override;
    void MoveTo(double xPu, double yPu) override;
    void SetVelocity(double mmPerSec) override;
    void SelectPen(int pen) override;
    void Reply(const char *text) override;
    void ActualPosition(double *xPu, double *yPu, bool *penDown) override;
    void PageEnd() override;
    uint32_t Room() override;

private:
    enum Mode : uint8_t { M_PASS, M_COLLECT, M_REPLAY };
    static const uint16_t kRev = 0x8000;  /* chain entry: stroke drawn end to start */

    struct Stroke {           /* as received */
        uint16_t start;       /* index into m_in */
        uint16_t count;
        float velocity;
        int8_t pen;
    };
    struct Path {             /* joined strokes: m_chain[first .. first + segs) */
        uint16_t first;
        uint16_t segs;
        uint16_t points;      /* shared joints counted once */
        int16_t x0, y0, x1, y1;  /* bounding box */
        float area;
        uint16_t pending;     /* enclosed paths not yet drawn */
        bool closed;          /* ends meet: entered at any vertex */
        bool encloses;        /* closed or nearly: can contain other paths */
        bool done;
    };
    struct Step {             /* replay order */
        uint16_t path;        /* path, or stroke when replaying as received */
        uint16_t entry;       /* closed: vertex to start at */
        bool reverse;         /* open: drawn end to start */
    };
    struct Iter {             /* walks a path's points */
        uint16_t seg;
        uint16_t off;
        bool backward;
    };

    void StartStroke();
    void EndStroke();
    void AddPoint(double x, double y);
    void Spill();             /* buffer full: replay as received, then pass through */
    void Order();             /* strokes -> paths -> steps */
    void JoinGroup(uint16_t first, uint16_t last);
    void OrderGroup(uint16_t firstPath, uint16_t lastPath, int16_t *penX, int16_t *penY);
    bool Encloses(uint16_t outer, uint16_t inner) const;
    bool PointInPath(uint16_t path, float x, float y) const;
    void StrokeEnd(uint16_t stroke, bool atStart, int *x, int *y) const;
    void PathEnd(uint16_t path, bool last, int *x, int *y) const;
    void IterBegin(uint16_t path, bool backward, Iter *it) const;
    bool IterNext(uint16_t path, Iter *it, int16_t *x, int16_t *y) const;
    void ResetBuffer();

    HpglSink *m_out = nullptr;
    Mode m_mode = M_PASS;
    bool m_passRest = false;     /* after a spill: forward the rest of the job */
    bool m_pageEndPending = false;
    bool m_jobEnded = false;     /* EndJob() came while a page was being replayed */

    /* collection */
    int16_t m_in[2 * kMaxPoints];
    Stroke m_strokes[kMaxStrokes];
    uint16_t m_chain[kMaxStrokes];  /* stroke | kRev, grouped by path */
    uint16_t m_back[kMaxStrokes];   /* scratch while joining */
    bool m_used[kMaxStrokes];
    Path m_paths[kMaxStrokes];
    Step m_steps[kMaxStrokes];
    uint16_t m_pointCount = 0;
    uint16_t m_strokeCount = 0;
    uint16_t m_chainCount = 0;
    uint16_t m_pathCount = 0;
    uint16_t m_stepCount = 0;
    bool m_down = false;
    bool m_inStroke = false;
    double m_x = 0, m_y = 0;     /* pen position as the interpreter sees it */
    float m_velocity = 0;
    int m_pen = 1;

    /* replay */
    uint16_t m_replayStep = 0;
    uint8_t m_replayPhase = 0;
    bool m_replayRaw = false;    /* strokes as received (spill) */
    Iter m_it;
    uint16_t m_emitted = 0;      /* points of the current step sent */
    bool m_wrapped = false;      /* closed path: second leg (vertex 0 .. entry) */
    bool m_realDown = false;
    float m_curVelocity = -1;
    int m_curPen = -1;

    /* statistics */
    uint32_t m_statStrokes = 0, m_statPaths = 0;
    float m_statBefore = 0, m_statAfter = 0;
    bool m_statOverflow = false;
};

#endif
