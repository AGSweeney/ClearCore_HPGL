/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_HPGL_H__
#define __CLEARHPGL_HPGL_H__

/*
 * Streaming HP-GL interpreter. Portable C++ (no ClearCore headers) so the host
 * tests in Firmware/tests build it with MSVC or GCC.
 *
 * Bytes go in one at a time with Feed(). Geometry comes out through HpglSink as
 * PenUp / PenDown / MoveTo in physical plotter units (40 per mm), already scaled,
 * rotated, window-clipped and clamped to the hard-clip area. Pen-down arcs and
 * circles go to the sink as one ArcTo when it accepts them (same scale on both
 * axes, wholly inside the clip area); otherwise they become chords. Output
 * instructions (OI, OS, OA, ...) call Reply().
 *
 * Shapes that expand into many segments (CI, AA, EW, ...) are emitted from
 * Pump(), a few segments at a time, while Busy() is true. The caller stops
 * feeding bytes while Busy() and keeps calling Pump() as the sink frees room.
 */

#include <stdbool.h>
#include <stdint.h>

#define HPGL_PU_PER_MM 40.0
#define HPGL_MAX_PARAMS 16
#define HPGL_MAX_OPS 8

/* OE error numbers (HP 7475A). */
enum HpglError {
    HPGL_ERR_NONE = 0,
    HPGL_ERR_UNKNOWN_INSTRUCTION = 1,
    HPGL_ERR_PARAM_COUNT = 2,
    HPGL_ERR_BAD_PARAM = 3,
    HPGL_ERR_POSITION_OVERFLOW = 6,
};

/* OS status bits (HP 7475A). */
enum HpglStatusBits {
    HPGL_OS_PEN_DOWN = 1,
    HPGL_OS_P1P2_CHANGED = 2,
    HPGL_OS_INITIALIZED = 8,
    HPGL_OS_READY = 16,
    HPGL_OS_ERROR = 32,
};

class HpglSink {
public:
    virtual ~HpglSink() {}
    virtual void PenUp() = 0;
    virtual void PenDown() = 0;
    /* Physical plotter units, inside [0, width] x [0, height]. */
    virtual void MoveTo(double xPu, double yPu) = 0;
    /* Pen-down arc from the current point around (cx, cy) to (ex, ey), physical plotter
     * units. sweepDeg is signed (positive = counter-clockwise); +-360 is a full circle and
     * then (ex, ey) is the start point. Return false to have it drawn as chords instead. */
    virtual bool ArcTo(double cxPu, double cyPu, double exPu, double eyPu, double sweepDeg) {
        (void)cxPu; (void)cyPu; (void)exPu; (void)eyPu; (void)sweepDeg;
        return false;
    }
    /* Pen-down speed in mm/s. <= 0 restores the configured default. */
    virtual void SetVelocity(double mmPerSec) = 0;
    virtual void SelectPen(int pen) = 0;
    /* Reply body without terminator; the channel appends its output terminator. */
    virtual void Reply(const char *text) = 0;
    /* Physical plotter units of the pen right now. */
    virtual void ActualPosition(double *xPu, double *yPu, bool *penDown) = 0;
    /* PG / NR / AF: end of a page. */
    virtual void PageEnd() = 0;
    /* Free action slots. Pump() stops while this is below a small reserve. */
    virtual uint32_t Room() = 0;
};

class HpglInterpreter {
public:
    /* Worst case sink calls from one Feed() byte or one Pump() step. */
    static const uint32_t kRoomReserve = 8;

    HpglInterpreter();
    void Init(HpglSink *sink, double hardWidthPu, double hardHeightPu, const char *ident);
    /** True for models whose plotter-unit origin is the centre of the sheet: the large-format
     *  758x and DraftMaster 759x. Their drivers (e.g. AutoCAD's) send coordinates around 0,0
     *  at the middle; the 7475A / 7550A put 0,0 at the lower-left. Chosen by the ident. */
    static bool CenterOriginModel(const char *ident);
    bool CenterOrigin() const { return m_centerOrigin; }
    /* Takes effect at the next IN (and immediately for clamping). */
    void SetHardClip(double widthPu, double heightPu);
    void SetIdentification(const char *ident);

    void Feed(uint8_t c);
    bool Busy() const { return m_opCount > 0; }
    void Pump();

    /* Drop a partial instruction and any pending shape (ESC.K, abort). */
    void AbortInstruction();
    bool AtInstructionBoundary() const;
    /* After an abort or "origin": take the pen position from the sink. */
    void SyncPositionFromSink();
    /* IN without touching the stream state. */
    void Initialize();

    /* Diagnostics for the console. */
    uint8_t StatusByte() const;
    int Error() const { return m_error; }
    uint32_t LabelsSkipped() const { return m_labelsSkipped; }
    uint32_t UnknownCount() const { return m_unknownCount; }
    uint32_t InstructionCount() const { return m_instructionCount; }

private:
    enum ParseState {
        PS_IDLE,
        PS_MNEMONIC2,
        PS_PARAMS,
        PS_STRING,
        PS_LABEL,
        PS_DT,
        PS_PE,
        PS_PJL,
    };

    enum OpType {
        OP_PEN_UP,
        OP_PEN_DOWN,
        OP_LINE,     /* to (x, y) user units with the current logical pen */
        OP_ARC,      /* around (x, y) user units, a = sweep deg, b = chord */
        OP_RESTORE,  /* pen state saved when the shape began */
    };

    struct Op {
        uint8_t type;
        double x;
        double y;
        double a;
        double b;
    };

    struct Pt {
        double x;
        double y;
    };

    /* Parsing */
    void BeginInstruction();
    void EndNumber();
    void EndInstruction();
    void Execute();
    void ExecuteCoordPair(double x, double y);
    void FeedPe(uint8_t c);
    void PeValueDone(int32_t value);
    void SetError(int err);

    /* Coordinate systems: user units -> logical PU (scaling) -> physical PU (rotation). */
    void DefaultsDF();
    void DefaultsIN();
    void DefaultP1P2();
    void RecalcScale();
    Pt UserToPu(double ux, double uy) const;
    Pt PuToUser(double px, double py) const;
    Pt LogicalToPhysical(Pt p) const;
    Pt PhysicalToLogical(Pt p) const;
    double LogicalWidth() const;
    /** Where plotter-unit 0,0 sits in the internal (0..width, 0..height) frame. */
    Pt Origin() const;
    double LogicalHeight() const;
    Pt ClampHard(Pt p) const;
    Pt CurrentUser() const;

    /* Drawing */
    void LineToUser(double ux, double uy);
    void LineToPu(Pt target);
    void LogicalPenUp();
    void LogicalPenDown();
    void PhysicalMove(Pt p);
    void PhysicalPen(bool down);
    bool ClipBox(double *xmin, double *ymin, double *xmax, double *ymax) const;
    bool ClipSegment(Pt a, Pt b, double *t0, double *t1) const;
    bool ArcInsideClip(Pt c, double r, double a0, double sweep) const;

    /* Shapes */
    void PushOp(uint8_t type, double x = 0, double y = 0, double a = 0, double b = 0);
    void BeginShape();
    double ChordDegrees(double requested) const;
    bool StepArc(Op &op);
    bool NativeArc(const Op &op);

    /* Output */
    void ReplyF(const char *fmt, ...);

    HpglSink *m_sink;
    char m_ident[16];
    bool m_centerOrigin = false;
    double m_hardW;
    double m_hardH;

    /* Parser */
    ParseState m_state;
    char m_m1;
    char m_mnemonic[3];
    double m_params[HPGL_MAX_PARAMS];
    uint8_t m_paramCount;
    uint8_t m_paramsSeen;
    bool m_coordInstr;
    char m_num[24];
    uint8_t m_numLen;
    uint8_t m_pjlMatch;
    char m_labelTerm;
    bool m_labelTermPrint;
    bool m_labelLastWasTerm;

    /* PE */
    bool m_peBase64;
    bool m_peAbsNext;
    bool m_pePenUpNext;
    uint8_t m_peFlag;     /* ':' or '>' while a flag value is expected, else 0 */
    uint32_t m_peAcc;
    uint32_t m_peMult;
    uint8_t m_peFracBits;
    bool m_peHaveX;
    double m_peX;

    /* Plotter state */
    bool m_absolute;
    bool m_penDown;          /* logical (PU/PD) */
    bool m_physDown;         /* what the sink was told */
    Pt m_pos;                /* logical PU */
    Pt m_physPos;            /* logical PU of the last sink MoveTo */
    Pt m_p1;
    Pt m_p2;
    bool m_scaleOn;
    uint8_t m_scaleType;
    double m_sc[4];
    double m_scLeft;
    double m_scBottom;
    double m_kx;
    double m_ky;
    double m_ux0;
    double m_uy0;
    Pt m_scOrigin;
    bool m_windowOn;
    Pt m_winLo;
    Pt m_winHi;
    int m_rotation;          /* 0, 90, 180, 270 */
    double m_chordDeg;
    uint8_t m_chordMode;     /* CT: 0 = angle, 1 = deviation */
    double m_tickPos;
    double m_tickNeg;
    int m_pen;

    /* Status */
    int m_error;
    bool m_initialized;
    bool m_p1p2Changed;
    uint32_t m_labelsSkipped;
    uint32_t m_unknownCount;
    uint32_t m_instructionCount;

    /* Pending shape */
    Op m_ops[HPGL_MAX_OPS];
    uint8_t m_opHead;
    uint8_t m_opCount;
    bool m_shapeSavedPen;
    uint32_t m_arcStep;
    uint32_t m_arcSteps;
    double m_arcR;
    double m_arcStart;
};

#endif
