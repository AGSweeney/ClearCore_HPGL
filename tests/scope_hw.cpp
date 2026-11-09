/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
// Bench check of the ClearPath scope link against a real motor on its own USB port. Never enables or
// moves the motor: it streams channel 1 while reading registers in between (as the scope does), then
// checks that channel 1 is back exactly as it was and the link is back at 9600 baud.
//
//   scope_hw <COMn | "Simulated motor"> [seconds] [monitor variable id]
//
// Exit code 0 = all passed. Close ClearPath-MSP first (one program owns the port).

#include "ClearPathLink.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace clearpath;

static int g_fail = 0;
static void Check(bool ok, const QString &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

struct Range {
    double lo = std::numeric_limits<double>::infinity(), hi = -std::numeric_limits<double>::infinity();
    int n = 0;
    void add(double v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        ++n;
    }
};

int main(int argc, char *argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 2) {
        std::printf("usage: scope_hw <COMn | \"Simulated motor\"> [seconds] [monitor variable id]\n");
        return 2;
    }
    const QString port = a[1];
    const double seconds = a.size() > 2 ? a[2].toDouble() : 3.0;
    const int monId = a.size() > 3 ? a[3].toInt() : 27;
    const MonVar *mv = FindMonVar(monId);
    if (!mv) {
        std::printf("unknown monitor variable %d\n", monId);
        return 2;
    }

    std::unique_ptr<Transport> t;
    if (port == QLatin1String(kSimulatedPort)) {
        t = OpenSimulated();
    } else {
        QString err;
        t = OpenSerial(port, &err);
        if (!t) {
            std::printf("could not open %s: %s\n", qPrintable(port), qPrintable(err));
            return 2;
        }
    }
    try {
        Link link(std::move(t));
        Check(link.nodeCount() >= 1, QStringLiteral("set-address answered (%1 node)").arg(link.nodeCount()));
        const int64_t id = link.get(params::DeviceId);
        const int64_t res = link.get(params::InputResolution);
        std::printf("     device 0x%04llx, firmware 0x%04llx, serial %lld, %lld steps/rev\n", static_cast<long long>(id),
                    static_cast<long long>(link.get(params::FirmwareVersion)), static_cast<long long>(link.get(params::SerialNumber)),
                    static_cast<long long>(res));
        Check((id >> 8) == 11, QStringLiteral("a ClearPath MC/SD (device 0x%1)").arg(id, 4, 16, QChar('0')));
        const QStringList alerts = AlertNames(link.getChars(params::AlertRegister));
        std::printf("     alerts at start: %s\n", alerts.isEmpty() ? "none" : qPrintable(alerts.join(' ')));

        // As the scope's Start: 230400 baud, channel 1 on the variable (working setting only), stream.
        const MonState m1 = link.monitorGet(1), m2 = link.monitorGet(2);
        std::printf("     channel 1 before: var %d gain 0x%08x filter %d sync %d; channel 2: var %d\n", m1.var, m1.gain,
                    m1.filter, m1.sync, m2.var);
        link.setRate(Link::kStreamBaud);
        Check(link.currentBaud() == Link::kStreamBaud, QStringLiteral("link at 230400 baud"));
        link.monitorSet(1, MonState{mv->id, mv->gain, 0, m1.sync});
        Check((link.monitorGet(1).var & 0x0FFF) == mv->id, QStringLiteral("channel 1 takes %1").arg(QString::fromLatin1(mv->name)));
        link.takeStream();
        link.scope(3);

        // Record: stream samples, and the registers in turn between them, with the keepalive.
        const ParamDef *regs[] = {&params::VelocityA, &params::BusVoltage, &params::DriveTemperature};
        const double scale[] = {1.0 / 16555.0, 120.0 / 32768.0, 1.0};
        const char *names[] = {"velocity (0x055) RPM", "bus voltage (0x118) V", "drive temperature (0x123) C"};
        Range r[3], mon, gaps;
        int failures = 0, samples = 0, lost = 0, hlfbHigh = 0, enableHigh = 0, prevCounter = -1;
        QElapsedTimer clock;
        clock.start();
        double lastKeep = 0, lastReg = 0;
        int ri = 0;
        while (clock.elapsed() < seconds * 1000) {
            link.pump(3);
            const double now = clock.elapsed() / 1000.0;
            try {
                if (now - lastKeep > 0.3) {
                    link.keepalive();
                    lastKeep = now;
                }
                if (now - lastReg >= 0.02) {
                    lastReg = now;
                    const int k = ri++ % 3;
                    r[k].add(static_cast<double>(link.get(*regs[k])) * scale[k]);
                }
            } catch (const std::exception &e) {
                ++failures;
                std::printf("     read failed: %s\n", e.what());
            }
            for (const StreamSample &s : link.takeStream()) {
                ++samples;
                mon.add(MonScale(*mv, s.raw));
                if (prevCounter >= 0) {
                    const int step = (s.counter - prevCounter) & 3;
                    if (step != 1) lost += step == 0 ? 3 : step - 1;
                }
                prevCounter = s.counter;
                hlfbHigh += (s.digital >> 3) & 1;
                enableHigh += s.digital & 1;
            }
        }
        const double rate = samples / seconds;
        std::printf("     %d stream samples in %.1f s (%.0f/s), %d lost by the counter, %d bad packets\n", samples, seconds, rate,
                    lost, link.badPackets());
        std::printf("     %s: %.4g .. %.4g %s\n", mv->name, mon.lo, mon.hi, MonUnit(*mv));
        std::printf("     HLFB bit high in %d%%, enable bit high in %d%% of samples\n", samples ? hlfbHigh * 100 / samples : 0,
                    samples ? enableHigh * 100 / samples : 0);
        for (int k = 0; k < 3; ++k) std::printf("     %s: %d reads, %.4g .. %.4g\n", names[k], r[k].n, r[k].lo, r[k].hi);
        Check(rate > 700 && rate < 1000, QStringLiteral("stream at about 845 samples/s (%1/s)").arg(rate, 0, 'f', 0));
        Check(failures == 0, QStringLiteral("every register read answered while streaming (%1 failed)").arg(failures));
        Check(r[0].n > 5 && r[1].n > 5 && r[2].n > 5, QStringLiteral("registers read in turn between stream samples"));
        Check(r[2].n == 0 || (r[2].lo >= 5 && r[2].hi <= 90), QStringLiteral("drive temperature plausible (each read got its own reply)"));
        Check(r[1].n == 0 || (r[1].lo >= 5 && r[1].hi <= 100), QStringLiteral("bus voltage plausible"));

        // As the scope's Stop: stream off twice, channel 1 back, 9600 baud.
        link.scope(0);
        link.scope(0);
        MonState back = m1;
        back.var &= ~Link::kMonSaveNv;
        link.monitorSet(1, back);
        link.setRate(Link::kBaud);
        const MonState after = link.monitorGet(1);
        Check(after == m1, QStringLiteral("channel 1 restored exactly (var %1 gain 0x%2)").arg(after.var).arg(after.gain, 8, 16, QChar('0')));
        Check(link.monitorGet(2) == m2, QStringLiteral("channel 2 untouched"));
        Check(link.currentBaud() == Link::kBaud, QStringLiteral("link back at 9600 baud"));
        const QStringList alertsAfter = AlertNames(link.getChars(params::AlertRegister));
        Check(alertsAfter == alerts, QStringLiteral("alerts unchanged by the scope (%1)").arg(alertsAfter.isEmpty() ? QStringLiteral("none") : alertsAfter.join(' ')));
    } catch (const std::exception &e) {
        std::printf("FAIL %s\n", e.what());
        return 1;
    }
    std::printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
