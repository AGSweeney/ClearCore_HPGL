/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/*
 * Host tests for the portable HP-GL interpreter and channel. No board needed.
 * Build: cmake -S Firmware/tests -B Firmware/tests/build && cmake --build Firmware/tests/build
 */

#include "Hpgl.h"
#include "HpglChannel.h"
#include "JobOptimizer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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

#define CHECK_EQ_STR(a, b)                                                       \
    do {                                                                         \
        g_checks++;                                                              \
        const std::string _a = (a), _b = (b);                                    \
        if (_a != _b) {                                                          \
            g_failures++;                                                        \
            printf("  FAIL %s:%d:\n    got:  %s\n    want: %s\n", __FILE__,      \
                   __LINE__, _a.c_str(), _b.c_str());                            \
        }                                                                        \
    } while (0)

static const double kW = 16000;  /* 400 mm */
static const double kH = 12000;  /* 300 mm */

class MockSink : public HpglSink {
public:
    std::vector<std::string> events;
    std::vector<std::string> replies;
    uint32_t room = 100000;
    double x = 0, y = 0;
    bool down = false;
    double velocity = -1;
    int pen = -1;
    int pages = 0;

    void PenUp() override {
        down = false;
        events.push_back("U");
    }
    void PenDown() override {
        down = true;
        events.push_back("D");
    }
    void MoveTo(double px, double py) override {
        x = px;
        y = py;
        char b[64];
        snprintf(b, sizeof(b), "M%.0f,%.0f", px, py);
        events.push_back(b);
        if (room > 0) room--;
    }
    void SetVelocity(double v) override { velocity = v; }
    void SelectPen(int p) override { pen = p; }
    void Reply(const char *t) override { replies.push_back(t); }
    void ActualPosition(double *px, double *py, bool *pd) override {
        *px = x;
        *py = y;
        *pd = down;
    }
    void PageEnd() override { pages++; }
    uint32_t Room() override { return room; }

    std::string Joined() const {
        std::string s;
        for (size_t i = 0; i < events.size(); i++) {
            if (i) s += " ";
            s += events[i];
        }
        return s;
    }
    void Clear() {
        events.clear();
        replies.clear();
    }
};

static void FeedAll(HpglInterpreter &h, const char *s, size_t len = (size_t)-1) {
    if (len == (size_t)-1) len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        h.Feed((uint8_t)s[i]);
    }
    while (h.Busy()) h.Pump();
}

struct Fixture {
    MockSink sink;
    HpglInterpreter h;
    Fixture() { h.Init(&sink, kW, kH, "7475A"); }
};

static void TestBasicPolyline() {
    printf("basic polyline\n");
    Fixture f;
    FeedAll(f.h, "IN;SP1;PU100,200;PD300,200,300,400;PU;");
    CHECK_EQ_STR(f.sink.Joined(), "M100,200 D M300,200 M300,400 U");
    CHECK(f.sink.pen == 1);
}

static void TestNoTerminatorsAndRelative() {
    printf("chained mnemonics, PR\n");
    Fixture f;
    FeedAll(f.h, "PA10,10PD20,20PU PR5,0PD0,5 pu");
    CHECK_EQ_STR(f.sink.Joined(), "M10,10 D M20,20 U M25,20 D M25,25 U");
}

static void TestLowerCaseAndDecimals() {
    printf("lower case, decimals, signs\n");
    Fixture f;
    FeedAll(f.h, "pa1000.5,2000;pd1500,2000-0;");
    /* "2000-0" splits into two numbers: 2000, -0 -> one pair after the first */
    CHECK(f.sink.events.size() >= 2);
    CHECK_EQ_STR(f.sink.events[0], "M1000,2000");  /* %.0f of 1000.5 rounds to even */
}

static void TestScaling() {
    printf("IP / SC\n");
    Fixture f;
    FeedAll(f.h, "IN;IP0,0,1000,1000;SC0,10,0,10;PU5,5;PD10,10;");
    CHECK_EQ_STR(f.sink.Joined(), "M500,500 D M1000,1000");
    f.sink.Clear();
    FeedAll(f.h, "OC;");
    CHECK(f.sink.replies.size() == 1 && f.sink.replies[0] == "10,10,1");
    f.sink.Clear();
    /* isotropic: 0..10 x 0..20 into a 1000x1000 frame -> 50 PU per unit, centred in x */
    FeedAll(f.h, "PU;SC0,10,0,20,1;PA10,20;");
    CHECK_EQ_STR(f.sink.Joined(), "U M750,1000");
    f.sink.Clear();
    /* point factor: 1 user unit = 40 PU (1 mm) from P1 */
    FeedAll(f.h, "SC0,40,0,40,2;PA2,3;");
    CHECK_EQ_STR(f.sink.Joined(), "M80,120");
    f.sink.Clear();
    FeedAll(f.h, "SC;PA7,9;");
    CHECK_EQ_STR(f.sink.Joined(), "M7,9");
}

static void TestHardClipClamp() {
    printf("hard clip clamps pen-up moves, clips pen-down\n");
    Fixture f;
    FeedAll(f.h, "PU-500,20000;");
    CHECK_EQ_STR(f.sink.Joined(), "M0,12000");
    f.sink.Clear();
    FeedAll(f.h, "PU15000,100;PD17000,100;");
    CHECK_EQ_STR(f.sink.Joined(), "M15000,100 D M16000,100 U");
}

static void TestWindowClip() {
    printf("IW clipping\n");
    Fixture f;
    FeedAll(f.h, "IW1000,1000,2000,2000;PU0,1500;PD3000,1500;PD3000,1600,0,1600;PU;");
    CHECK_EQ_STR(f.sink.Joined(),
                 "M0,1500 M1000,1500 D M2000,1500 U M2000,1600 D M1000,1600 U");
    f.sink.Clear();
    /* PD outside the window must not lower the pen */
    FeedAll(f.h, "PU500,500;PD;PU;");
    CHECK_EQ_STR(f.sink.Joined(), "M500,500");
}

static void TestRotation() {
    printf("RO 90 / 180 / 270\n");
    Fixture f;
    FeedAll(f.h, "RO90;PU0,0;PU100,0;");
    CHECK_EQ_STR(f.sink.Joined(), "M16000,0 M16000,100");
    f.sink.Clear();
    FeedAll(f.h, "OH;");
    CHECK(f.sink.replies.size() == 1 && f.sink.replies[0] == "0,0,12000,16000");
    f.sink.Clear();
    FeedAll(f.h, "RO180;PU0,0;");
    CHECK_EQ_STR(f.sink.Joined(), "M16000,12000");
    f.sink.Clear();
    FeedAll(f.h, "RO270;PU0,0;PU100,0;");
    CHECK_EQ_STR(f.sink.Joined(), "M0,12000 M0,11900");
}

static void TestCenterOrigin() {
    printf("centre origin (DraftMaster ident)\n");
    CHECK(HpglInterpreter::CenterOriginModel("7596A"));
    CHECK(HpglInterpreter::CenterOriginModel("7586B"));
    CHECK(!HpglInterpreter::CenterOriginModel("7475A"));
    CHECK(!HpglInterpreter::CenterOriginModel("7550A"));
    MockSink sink;
    HpglInterpreter h;
    h.Init(&sink, kW, kH, "7596A");
    /* AutoCAD's DraftMaster output: 0,0 is the middle of the bed */
    FeedAll(h, "IN;PU0,0;PU-8000,-6000;PU8000,6000;");
    CHECK_EQ_STR(sink.Joined(), "M8000,6000 M0,0 M16000,12000");
    sink.Clear();
    FeedAll(h, "OH;OP;");
    CHECK(sink.replies.size() == 2 && sink.replies[0] == "-8000,-6000,8000,6000" &&
          sink.replies[1] == "-8000,-6000,8000,6000");
    sink.Clear();
    FeedAll(h, "PA100,-200;OA;");
    CHECK(sink.replies.size() == 1 && sink.replies[0] == "100,-200,0");
    sink.Clear();
    /* IW is in the same frame: a window around the centre clips a line through it */
    FeedAll(h, "IW-1000,-1000,1000,1000;PU-2000,0;PD2000,0;PU;IW;");
    CHECK_EQ_STR(sink.Joined(), "M6000,6000 M7000,6000 D M9000,6000 U");
    sink.Clear();
    /* rotation turns about the centre */
    FeedAll(h, "RO90;PU0,0;");
    CHECK_EQ_STR(sink.Joined(), "M8000,6000");
    sink.Clear();
    /* switching back to a lower-left model restores the old frame */
    h.SetIdentification("7475A");
    FeedAll(h, "IN;PU0,0;OH;");
    CHECK_EQ_STR(sink.Joined(), "M0,0");
    CHECK(sink.replies.size() == 1 && sink.replies[0] == "0,0,16000,12000");
}

static void TestCircle() {
    printf("CI\n");
    Fixture f;
    FeedAll(f.h, "PU1000,1000;CI100;");
    /* move to start, pen down, 72 chords at 5 degrees, pen up, back to centre */
    CHECK(f.sink.events.size() == 1 + 1 + 1 + 72 + 1 + 1);
    CHECK_EQ_STR(f.sink.events[1], "M1100,1000");
    CHECK_EQ_STR(f.sink.events[2], "D");
    CHECK_EQ_STR(f.sink.events[f.sink.events.size() - 3], "M1100,1000");
    CHECK_EQ_STR(f.sink.events[f.sink.events.size() - 2], "U");
    CHECK_EQ_STR(f.sink.events.back(), "M1000,1000");
    f.sink.Clear();
    FeedAll(f.h, "CI100,90;");
    CHECK(f.sink.events.size() == 1 + 1 + 4 + 1 + 1);
}

static void TestSmallCircleMinChord() {
    printf("small circles: chords no shorter than 0.2 mm\n");
    Fixture f;
    FeedAll(f.h, "PU1000,1000;CI40;");  /* r = 1 mm: 8 PU chords -> 11.46 deg -> 32 chords */
    CHECK(f.sink.events.size() == 1 + 1 + 1 + 32 + 1 + 1);
    f.sink.Clear();
    FeedAll(f.h, "CI2;");  /* r = 0.05 mm: limited to a square */
    CHECK(f.sink.events.size() == 1 + 1 + 4 + 1 + 1);
    f.sink.Clear();
    FeedAll(f.h, "IP0,0,16000,12000;SC0,1600,0,1200;CI1;SC;");  /* r = 1 user unit = 10 PU, scaled */
    CHECK(f.sink.events.size() == 1 + 1 + 8 + 1 + 1);  /* no PU move first: start, D, 8 chords, U, back */
}

static void TestArcs() {
    printf("AA / AR\n");
    Fixture f;
    FeedAll(f.h, "PU1100,1000;PD;AA1000,1000,90,45;PU;");
    CHECK_EQ_STR(f.sink.Joined(), "M1100,1000 D M1071,1071 M1000,1100 U");
    f.sink.Clear();
    FeedAll(f.h, "PD;AR0,-100,-90,90;PU;");
    CHECK_EQ_STR(f.sink.Joined(), "D M1100,1000 U");  /* clockwise from the top ends at the right */
}

/* A sink that takes native arcs (as the plotter does). */
class ArcSink : public MockSink {
public:
    bool ArcTo(double cx, double cy, double ex, double ey, double sweep) override {
        x = ex;
        y = ey;
        char b[96];
        snprintf(b, sizeof(b), "A%.0f,%.0f>%.0f,%.0f@%.0f", cx, cy, ex, ey, sweep);
        events.push_back(b);
        if (room > 0) room--;
        return true;
    }
};

struct ArcFixture {
    ArcSink sink;
    HpglInterpreter h;
    ArcFixture() { h.Init(&sink, kW, kH, "7475A"); }
};

static void TestNativeArcs() {
    printf("native arcs\n");
    {
        ArcFixture f;
        FeedAll(f.h, "PU1000,1000;CI100;");
        CHECK_EQ_STR(f.sink.Joined(), "M1000,1000 M1100,1000 D A1000,1000>1100,1000@360 U M1000,1000");
        f.sink.Clear();
        FeedAll(f.h, "PU1100,1000;PD;AA1000,1000,90;PU;");
        CHECK_EQ_STR(f.sink.Joined(), "M1100,1000 D A1000,1000>1000,1100@90 U");
        f.sink.Clear();
        FeedAll(f.h, "PD;AR0,-100,-90;PU;");  /* clockwise from the top */
        CHECK_EQ_STR(f.sink.Joined(), "D A1000,1000>1100,1000@-90 U");
        f.sink.Clear();
        FeedAll(f.h, "PU1000,1000;EW100,0,90;");
        CHECK_EQ_STR(f.sink.Joined(), "M1000,1000 D M1100,1000 A1000,1000>1000,1100@90 M1000,1000 U");
        f.sink.Clear();
        /* a line after the arc continues from the arc's end */
        FeedAll(f.h, "PU1100,1000;PD;AA1000,1000,90;PD1000,1200;PU;");
        CHECK_EQ_STR(f.sink.Joined(), "M1100,1000 D A1000,1000>1000,1100@90 M1000,1200 U");
    }
    {
        printf("native arcs: pen up stays chords\n");
        ArcFixture f;
        FeedAll(f.h, "PU1100,1000;AA1000,1000,90,45;");
        CHECK_EQ_STR(f.sink.Joined(), "M1100,1000 M1071,1071 M1000,1100");
    }
    {
        printf("native arcs: rotation keeps direction, mirrored scale reverses it\n");
        ArcFixture f;
        FeedAll(f.h, "IN;RO90;PU1100,1000;PD;AA1000,1000,90;PU;");
        /* RO90: physical = (W - y, x); centre 1000,1000 -> 15000,1000 */
        CHECK_EQ_STR(f.sink.Joined(), "M15000,1100 D A15000,1000>14900,1000@90 U");
        f.sink.Clear();
        FeedAll(f.h, "IN;IP0,0,16000,12000;SC1600,0,0,1200;PU100,100;PD;AA0,100,90;PU;");
        /* x mirrored (10 PU per unit): user CCW is clockwise on paper */
        CHECK_EQ_STR(f.sink.Joined(), "M15000,1000 D A16000,1000>16000,2000@-90 U");
    }
    {
        printf("native arcs: anisotropic scale and clipping fall back to chords\n");
        ArcFixture f;
        FeedAll(f.h, "IP0,0,16000,12000;SC0,1600,0,2400;PU110,100;PD;AA100,100,90,45;PU;SC;");
        CHECK(f.sink.Joined().find('A') == std::string::npos);
        CHECK(f.sink.events.size() == 1 + 1 + 2 + 1);
        f.sink.Clear();
        FeedAll(f.h, "IN;PU50,1000;CI100;");  /* crosses x = 0 */
        CHECK(f.sink.Joined().find('A') == std::string::npos);
        f.sink.Clear();
        FeedAll(f.h, "IN;IW0,0,1050,2000;PU1000,1000;CI100;");  /* window cuts the circle */
        CHECK(f.sink.Joined().find('A') == std::string::npos);
        f.sink.Clear();
        FeedAll(f.h, "IN;IW0,0,1200,2000;PU1000,1000;CI100;");  /* touches nothing */
        CHECK(f.sink.Joined().find("A1000,1000>1100,1000@360") != std::string::npos);
    }
    {
        printf("native arcs: buffered (optimizer) jobs are chords, pass-through forwards\n");
        ArcSink out;
        JobOptimizer *opt = new JobOptimizer();
        opt->Init(&out);
        HpglInterpreter h;
        h.Init(opt, kW, kH, "7475A");
        opt->BeginJob(false);
        FeedAll(h, "PU1000,1000;CI100;");
        CHECK(out.Joined().find("A1000,1000>1100,1000@360") != std::string::npos);
        out.Clear();
        opt->BeginJob(true);
        FeedAll(h, "PU2000,2000;CI100;");
        opt->EndJob();
        for (int i = 0; i < 100000 && opt->Busy(); i++) opt->Pump();
        CHECK(out.Joined().find('A') == std::string::npos);
        CHECK(out.events.size() > 70);
        delete opt;
    }
}

static void TestRectanglesAndWedges() {
    printf("EA / ER / RA / EW\n");
    Fixture f;
    FeedAll(f.h, "PU100,100;EA200,300;");
    CHECK_EQ_STR(f.sink.Joined(), "M100,100 D M200,100 M200,300 M100,300 M100,100 U");
    f.sink.Clear();
    FeedAll(f.h, "ER10,10;");
    CHECK_EQ_STR(f.sink.Joined(), "D M110,100 M110,110 M100,110 M100,100 U");
    f.sink.Clear();
    FeedAll(f.h, "PU1000,1000;EW100,0,90,45;");
    CHECK_EQ_STR(f.sink.Joined(), "M1000,1000 D M1100,1000 M1071,1071 M1000,1100 M1000,1000 U");
}

static void TestOutputs() {
    printf("OI / OS / OE / OA / OP / OF / OW\n");
    Fixture f;
    FeedAll(f.h, "OI;OS;OS;");
    CHECK(f.sink.replies.size() == 3);
    CHECK_EQ_STR(f.sink.replies[0], "7475A");
    CHECK_EQ_STR(f.sink.replies[1], "24");
    CHECK_EQ_STR(f.sink.replies[2], "16");
    f.sink.Clear();
    FeedAll(f.h, "ZZ1;OS;OE;OE;");
    CHECK(f.sink.replies.size() == 3);
    CHECK_EQ_STR(f.sink.replies[0], "48");
    CHECK_EQ_STR(f.sink.replies[1], "1");
    CHECK_EQ_STR(f.sink.replies[2], "0");
    f.sink.Clear();
    FeedAll(f.h, "PU1234,567;PD;OA;OP;OF;IW10,20,30,40;OW;");
    CHECK(f.sink.replies.size() == 4);
    CHECK_EQ_STR(f.sink.replies[0], "1234,567,1");
    CHECK_EQ_STR(f.sink.replies[1], "0,0,16000,12000");
    CHECK_EQ_STR(f.sink.replies[2], "40,40");
    CHECK_EQ_STR(f.sink.replies[3], "10,20,30,40");
    f.sink.Clear();
    FeedAll(f.h, "PA1;OE;");
    CHECK(f.sink.replies.size() == 1 && f.sink.replies[0] == "2");
}

static void TestVelocityAndPen() {
    printf("VS / SP0 / PG\n");
    Fixture f;
    FeedAll(f.h, "VS10;");
    CHECK(fabs(f.sink.velocity - 100.0) < 1e-9);
    FeedAll(f.h, "VS;");
    CHECK(f.sink.velocity == 0);
    FeedAll(f.h, "PU0,0;PD10,0;SP0;");
    CHECK(f.sink.events.back() == "U");
    CHECK(f.sink.pen == 0);
    FeedAll(f.h, "PG;");
    CHECK(f.sink.pages == 1);
}

static void TestLabelsAndStrings() {
    printf("LB / DT / quoted strings / PJL\n");
    Fixture f;
    FeedAll(f.h, "LBHELLO;PA1,1\x03PU5,5;");
    CHECK_EQ_STR(f.sink.Joined(), "M5,5");
    CHECK(f.h.LabelsSkipped() == 1);
    f.sink.Clear();
    FeedAll(f.h, "DT*;LBABC*PU6,6;");
    CHECK_EQ_STR(f.sink.Joined(), "M6,6");
    f.sink.Clear();
    FeedAll(f.h, "BP1,\"job; PU9,9\",5,1;CO\"x\";PU7,7;");
    CHECK_EQ_STR(f.sink.Joined(), "M7,7");
    f.sink.Clear();
    FeedAll(f.h, "@PJL ENTER LANGUAGE = HPGL2\r\nPU8,8;");
    CHECK_EQ_STR(f.sink.Joined(), "M8,8");
    CHECK(f.h.Error() == 0);
}

/* HP-GL/2 PE encoder from the spec: sign in bit 0, base 64 digits LSB first. */
static std::string PeEncode(long v, bool base64 = true) {
    unsigned long u = (v < 0) ? ((unsigned long)(-v) << 1) | 1u : ((unsigned long)v << 1);
    std::string s;
    const unsigned base = base64 ? 64u : 32u;
    while (true) {
        unsigned d = (unsigned)(u % base);
        u /= base;
        if (u == 0) {
            s += (char)(base64 ? 191 + d : 95 + d);
            break;
        }
        s += (char)(63 + d);
    }
    return s;
}

static void TestPolylineEncoded() {
    printf("PE\n");
    Fixture f;
    std::string pe = "PE";
    pe += "=" + PeEncode(1000) + PeEncode(2000);   /* absolute pen-down draw */
    pe += PeEncode(500) + PeEncode(-250);          /* relative */
    pe += "<" + PeEncode(0) + PeEncode(1000);      /* pen-up move */
    pe += ";";
    FeedAll(f.h, pe.c_str(), pe.size());
    CHECK_EQ_STR(f.sink.Joined(), "D M1000,2000 M1500,1750 U M1500,2750");
    f.sink.Clear();
    std::string pe2 = "PE7=" + PeEncode(100, false) + PeEncode(200, false) + ";";
    FeedAll(f.h, pe2.c_str(), pe2.size());
    CHECK_EQ_STR(f.sink.Joined(), "D M100,200");
    f.sink.Clear();
    std::string pe3 = "PE>" + PeEncode(1) + "=" + PeEncode(3) + PeEncode(5) + ";";  /* 1 fractional bit */
    FeedAll(f.h, pe3.c_str(), pe3.size());
    CHECK(fabs(f.sink.x - 1.5) < 1e-9 && fabs(f.sink.y - 2.5) < 1e-9);
}

static void TestPumpRespectsRoom() {
    printf("Busy / Pump with a full sink\n");
    Fixture f;
    f.sink.room = 20;
    const char *s = "PU1000,1000;CI100;";
    for (const char *p = s; *p; p++) f.h.Feed((uint8_t)*p);
    CHECK(f.h.Busy());
    const size_t partial = f.sink.events.size();
    CHECK(partial < 30);
    f.sink.room = 100000;
    while (f.h.Busy()) f.h.Pump();
    CHECK(f.sink.events.size() == 1 + 1 + 1 + 72 + 1 + 1);
}

/* ---------------- channel ---------------- */

class MockIo : public HpglChannelIo {
public:
    std::string out;
    void ChannelSend(const uint8_t *d, uint16_t n) override { out.append((const char *)d, n); }
};

static void IngestAll(HpglChannel &c, const std::string &s) {
    for (size_t i = 0; i < s.size(); i++) c.Ingest((uint8_t)s[i]);
}

static std::string Drain(HpglChannel &c) {
    std::string s;
    uint8_t b;
    while (c.Pop(&b)) s += (char)b;
    return s;
}

static void TestChannelDeviceControl() {
    printf("channel: ESC.B / ESC.L / ESC.M / ESC.O / ESC.E / ESC.K\n");
    uint8_t mem[256];
    MockIo io;
    HpglChannel c;
    c.Init(&io, mem, sizeof(mem), HPGL_HS_NONE);
    IngestAll(c, "IN;PA0,0;");
    IngestAll(c, "\x1b.B");
    CHECK_EQ_STR(io.out, "247\r");
    io.out.clear();
    IngestAll(c, "\x1b.L\x1b.O");
    CHECK_EQ_STR(io.out, "256\r0\r");
    io.out.clear();
    IngestAll(c, "\x1b.M;;;13;10:\x1b.B:");
    CHECK_EQ_STR(io.out, "247\r\n");
    io.out.clear();
    CHECK_EQ_STR(Drain(c), "IN;PA0,0;");
    IngestAll(c, "\x1b.O");
    CHECK_EQ_STR(io.out, "8\r\n");
    io.out.clear();
    IngestAll(c, "\x1b.Q\x1b.E\x1b.E");
    CHECK_EQ_STR(io.out, "11\r\n0\r\n");
    io.out.clear();
    IngestAll(c, "PD1,1;\x1b.K");
    CHECK(c.Count() == 0);
    CHECK(c.TakeAbortRequest());
    CHECK(!c.TakeAbortRequest());
    IngestAll(c, "\x1b.)PU1,1;\x1b.(PU2,2;");
    CHECK_EQ_STR(Drain(c), "PU2,2;");
    IngestAll(c, "\x1b.RPU;\x1b.B");
    CHECK_EQ_STR(io.out, "253\r");
}

static void TestChannelPclPjl() {
    printf("channel: PCL / PJL escapes are dropped\n");
    uint8_t mem[256];
    MockIo io;
    HpglChannel c;
    c.Init(&io, mem, sizeof(mem), HPGL_HS_NONE);
    IngestAll(c, "\x1b%-12345X@PJL ENTER LANGUAGE=HPGL2\r\n\x1b" "E\x1b%0BIN;PD;\x1b%0A\x1b&l1O");
    CHECK_EQ_STR(Drain(c), "@PJL ENTER LANGUAGE=HPGL2\r\nIN;PD;");
    CHECK(io.out.empty());
}

static void TestChannelXonXoff() {
    printf("channel: XON/XOFF hysteresis\n");
    uint8_t mem[512];
    MockIo io;
    HpglChannel c;
    c.Init(&io, mem, sizeof(mem), HPGL_HS_XONXOFF);
    std::string fill(460, 'A');
    IngestAll(c, fill);
    c.Poll();
    CHECK(io.out.size() == 1 && (uint8_t)io.out[0] == HpglChannel::kXoff);
    CHECK(c.XoffSent());
    io.out.clear();
    uint8_t b;
    for (int i = 0; i < 100; i++) c.Pop(&b);
    c.Poll();
    CHECK(io.out.empty());
    for (int i = 0; i < 200; i++) c.Pop(&b);
    c.Poll();
    CHECK(io.out.size() == 1 && (uint8_t)io.out[0] == HpglChannel::kXon);
    /* host's own XON/XOFF bytes are not data */
    const size_t before = c.Count();
    IngestAll(c, "\x11\x13");
    CHECK(c.Count() == before);
}

static void TestChannelEnqAck() {
    printf("channel: ENQ/ACK (ESC.I)\n");
    uint8_t mem[256];
    MockIo io;
    HpglChannel c;
    c.Init(&io, mem, sizeof(mem), HPGL_HS_NONE);
    IngestAll(c, "\x1b.I200;5;6:");
    IngestAll(c, "\x05");
    CHECK_EQ_STR(io.out, "\x06");
    io.out.clear();
    IngestAll(c, std::string(100, 'B'));
    IngestAll(c, "\x05");
    CHECK(io.out.empty());
    uint8_t b;
    for (int i = 0; i < 60; i++) c.Pop(&b);
    c.Poll();
    CHECK_EQ_STR(io.out, "\x06");
}

static void TestChannelOverflow() {
    printf("channel: overflow sets ESC.E 16\n");
    uint8_t mem[16];
    MockIo io;
    HpglChannel c;
    c.Init(&io, mem, sizeof(mem), HPGL_HS_NONE);
    IngestAll(c, std::string(20, 'C'));
    CHECK(c.Count() == 16);
    CHECK(c.Overflows() == 4);
    IngestAll(c, "\x1b.E");
    CHECK_EQ_STR(io.out, "16\r");
}

/* Interpreter -> JobOptimizer -> MockSink, as on the board. Static: the optimizer's buffers are
 * ~85 KB. */
static JobOptimizer g_opt;

struct OptFixture {
    MockSink sink;
    HpglInterpreter h;
    explicit OptFixture(bool optimize) {
        g_opt.Init(&sink);
        g_opt.Abort();
        h.Init(&g_opt, kW, kH, "7475A");
        g_opt.BeginJob(optimize);
    }
    void Run(const char *job) {
        for (const char *c = job; *c; c++) {
            while (g_opt.Room() <= HpglInterpreter::kRoomReserve && g_opt.Busy()) {
                g_opt.Pump();
            }
            h.Feed((uint8_t)*c);
            while (h.Busy()) {
                g_opt.Pump();
                h.Pump();
            }
        }
        g_opt.EndJob();
        for (int i = 0; i < 100000 && g_opt.Busy(); i++) {
            g_opt.Pump();
        }
    }
    /* Pen-down runs in output order, as "x,y x,y ..." */
    std::vector<std::string> Strokes() const {
        std::vector<std::string> out;
        std::string cur;
        bool down = false;
        std::string lastMove;
        for (const std::string &e : sink.events) {
            if (e == "D") {
                down = true;
                cur = lastMove.substr(1);
            } else if (e == "U") {
                if (down && !cur.empty()) out.push_back(cur);
                down = false;
                cur.clear();
            } else if (e[0] == 'M') {
                lastMove = e;
                if (down) cur += " " + e.substr(1);
            }
        }
        if (down && !cur.empty()) out.push_back(cur);
        return out;
    }
};

static void TestOptimizerReorders() {
    printf("job optimizer: joins segments, hole before outline\n");
    OptFixture f(true);
    /* Outline 0..4000 as four separate segments (as AutoCAD sends it), then a hole inside. */
    f.Run("IN;SP1;PU0,0;PD4000,0;PU;PU4000,0;PD4000,4000;PU;PU4000,4000;PD0,4000;PU;"
          "PU0,4000;PD0,0;PU;PU1000,1000;PD2000,1000,2000,2000,1000,2000,1000,1000;PU;PA0,0;");
    const std::vector<std::string> s = f.Strokes();
    CHECK(s.size() == 2);
    if (s.size() == 2) {
        CHECK_EQ_STR(s[0], "1000,1000 2000,1000 2000,2000 1000,2000 1000,1000");  /* hole first */
        /* outline joined into one closed path, entered at the corner nearest the hole's end */
        CHECK_EQ_STR(s[1], "0,0 4000,0 4000,4000 0,4000 0,0");
    }
    CHECK(g_opt.LastStrokes() == 5 && g_opt.LastPaths() == 2);
    /* the pen ends where the job did (PA0,0), pen up */
    CHECK(f.sink.x == 0 && f.sink.y == 0 && !f.sink.down);
    CHECK(!g_opt.Busy());
}

static void TestOptimizerOffPassesThrough() {
    printf("job optimizer: off = unchanged\n");
    const char *job = "IN;SP1;PU0,0;PD4000,0;PU;PU1000,1000;PD2000,1000;PU;";
    OptFixture f(false);
    f.Run(job);
    Fixture direct;
    FeedAll(direct.h, job);
    CHECK_EQ_STR(f.sink.Joined(), direct.sink.Joined());
}

static void TestOptimizerPenGroups() {
    printf("job optimizer: pen groups keep their order\n");
    OptFixture f(true);
    /* pen 1: far stroke then near; pen 2 after. Reordered within pen 1 only. */
    f.Run("IN;SP1;PU8000,0;PD9000,0;PU;PU100,0;PD200,0;PU;SP2;PU5000,0;PD6000,0;PU;SP0;");
    const std::vector<std::string> s = f.Strokes();
    CHECK(s.size() == 3);
    if (s.size() == 3) {
        CHECK_EQ_STR(s[0], "100,0 200,0");
        CHECK_EQ_STR(s[1], "8000,0 9000,0");
        CHECK_EQ_STR(s[2], "6000,0 5000,0");  /* entered from the end nearer the pen (9000,0) */
    }
    CHECK(f.sink.pen == 0);
}

static void TestOptimizerEndsWithPage() {
    printf("job optimizer: job ending with PG finishes\n");
    OptFixture f(true);
    f.Run("IN;SP1;PU5000,0;PD6000,0;PU;PU100,0;PD200,0;PU;SP0;PG;");
    const std::vector<std::string> s = f.Strokes();
    CHECK(s.size() == 2 && s[0] == "100,0 200,0");
    CHECK(f.sink.pages == 1);
    CHECK(!g_opt.Busy());  /* was stuck collecting a next page that never came */
}

static void TestOptimizerSpill() {
    printf("job optimizer: too big = as received, nothing lost\n");
    OptFixture f(true);
    /* 1000 strokes of 7 points each (more strokes and points than the buffer holds), sent in
     * reverse position order: reordering would change the order, spilling must not. */
    std::string job = "IN;SP1;";
    char b[96];
    const int n = 1000;
    for (int i = n - 1; i >= 0; i--) {
        const int x = 10 * i;
        snprintf(b, sizeof(b), "PU%d,0;PD%d,10,%d,20,%d,30,%d,40,%d,50,%d,60;", x, x, x, x, x, x, x);
        job += b;
    }
    job += "PU;";
    f.Run(job.c_str());
    const std::vector<std::string> s = f.Strokes();
    CHECK(g_opt.LastOverflowed());
    CHECK(s.size() == (size_t)n);
    bool ordered = s.size() == (size_t)n;
    for (int i = 0; ordered && i < n; i++) {
        snprintf(b, sizeof(b), "%d,0 ", 10 * (n - 1 - i));
        ordered = s[i].compare(0, strlen(b), b) == 0;
    }
    CHECK(ordered);
}

/* test_hpgl <file.plt> [ident]: run a job through interpreter + optimizer and report. */
static int OptimizeFile(const char *path, const char *ident) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("cannot open %s\n", path);
        return 2;
    }
    std::string job;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) job.append(buf, n);
    fclose(fp);
    MockSink sink;
    HpglInterpreter h;
    g_opt.Init(&sink);
    h.Init(&g_opt, kW, kH, ident);
    g_opt.BeginJob(true);
    size_t fed = 0;
    for (char c : job) {
        if (getenv("OPT_TRACE") && (fed++ % 200) == 0) {
            fprintf(stderr, "fed %zu busy=%d replay=%d room=%u\n", fed, g_opt.Busy(), g_opt.Replaying(), (unsigned)g_opt.Room());
        }
        long waits = 0;
        while (g_opt.Room() <= HpglInterpreter::kRoomReserve && g_opt.Busy()) {
            g_opt.Pump();
            if (getenv("OPT_TRACE") && ++waits == 1000000) {
                fprintf(stderr, "replay stuck before byte %zu: replay=%d sinkroom=%u events=%zu last=%s\n", fed,
                        g_opt.Replaying(), (unsigned)sink.room, sink.events.size(),
                        sink.events.empty() ? "-" : sink.events.back().c_str());
            }
        }
        h.Feed((uint8_t)c);
        long spins = 0;
        while (h.Busy()) {
            g_opt.Pump();
            h.Pump();
            if (getenv("OPT_TRACE") && ++spins == 1000000) {
                fprintf(stderr, "stuck at byte %zu ('%c'): busy=%d replay=%d room=%u sinkroom=%u events=%zu\n", fed, c,
                        g_opt.Busy(), g_opt.Replaying(), (unsigned)g_opt.Room(), (unsigned)sink.room, sink.events.size());
            }
        }
    }
    if (getenv("OPT_TRACE")) fprintf(stderr, "stage: all bytes fed\n");
    h.Feed(';');
    while (h.Busy()) { g_opt.Pump(); h.Pump(); }
    if (getenv("OPT_TRACE")) fprintf(stderr, "stage: interpreter idle\n");
    g_opt.EndJob();
    if (getenv("OPT_TRACE")) {
        fprintf(stderr, "stage: EndJob returned: sinkroom=%u events=%zu\n", (unsigned)sink.room, sink.events.size());
        for (size_t k = 0; k < sink.events.size() && k < 60; k++) fprintf(stderr, "%s ", sink.events[k].c_str());
        fprintf(stderr, "\n...\n");
        for (size_t k = sink.events.size() > 30 ? sink.events.size() - 30 : 0; k < sink.events.size(); k++) fprintf(stderr, "%s ", sink.events[k].c_str());
        fprintf(stderr, "\n");
    }
    while (g_opt.Busy()) g_opt.Pump();
    if (getenv("OPT_TRACE")) fprintf(stderr, "stage: replay done\n");

    /* drawn length must not change: same lines, other order */
    auto drawn = [](const MockSink &s) {
        double len = 0, x = 0, y = 0;
        bool down = false;
        for (const std::string &e : s.events) {
            if (e == "D") down = true;
            else if (e == "U") down = false;
            else if (e[0] == 'M') {
                double nx = 0, ny = 0;
                sscanf(e.c_str() + 1, "%lf,%lf", &nx, &ny);
                if (down) len += hypot(nx - x, ny - y);
                x = nx;
                y = ny;
            }
        }
        return len / 40.0;
    };
    MockSink direct;
    HpglInterpreter hd;
    hd.Init(&direct, kW, kH, ident);
    FeedAll(hd, job.c_str(), job.size());
    if (getenv("OPT_TRACE")) fprintf(stderr, "stage: direct run done\n");

    printf("drawn %.1f mm optimized vs %.1f mm as received\n", drawn(sink), drawn(direct));
    printf("%s (%s): %u strokes -> %u paths, pen-up travel %.0f -> %.0f mm%s, end at %.0f,%.0f pen %s\n", path, ident,
           (unsigned)g_opt.LastStrokes(), (unsigned)g_opt.LastPaths(), g_opt.LastTravelBeforeMm(),
           g_opt.LastTravelAfterMm(), g_opt.LastOverflowed() ? " (overflow: as received)" : "", sink.x, sink.y,
           sink.down ? "down" : "up");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        return OptimizeFile(argv[1], argc > 2 ? argv[2] : "7475A");
    }
    TestBasicPolyline();
    TestNoTerminatorsAndRelative();
    TestLowerCaseAndDecimals();
    TestScaling();
    TestHardClipClamp();
    TestWindowClip();
    TestRotation();
    TestCenterOrigin();
    TestCircle();
    TestSmallCircleMinChord();
    TestArcs();
    TestNativeArcs();
    TestRectanglesAndWedges();
    TestOutputs();
    TestVelocityAndPen();
    TestLabelsAndStrings();
    TestPolylineEncoded();
    TestPumpRespectsRoom();
    TestChannelDeviceControl();
    TestChannelPclPjl();
    TestChannelXonXoff();
    TestChannelEnqAck();
    TestChannelOverflow();
    TestOptimizerReorders();
    TestOptimizerOffPassesThrough();
    TestOptimizerPenGroups();
    TestOptimizerEndsWithPage();
    TestOptimizerSpill();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
