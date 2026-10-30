// Headless check of PlotterLink against a ClearHPGL console + HP-GL port
// (the real board, or Firmware/host/fake_plotter.py).
//
//   python Firmware/host/fake_plotter.py
//   test_plotterlink.exe 127.0.0.1

#include "HpglPreview.h"
#include "PlotterLink.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>
#include <functional>

static int g_failures = 0;

static void Check(bool ok, const char *what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failures++;
    }
}

static bool WaitFor(const std::function<bool()> &cond, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return cond();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString host = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("127.0.0.1");

    std::printf("preview (firmware interpreter on the host)\n");
    {
        const HpglPreviewResult r = PreviewHpgl("IN;PU100,100;PD200,100,200,200;PU;CI40;LBX\x03", 400, 300);
        Check(r.cutStripsMm.size() == 2, "two pen-down strokes (polyline + circle)");
        Check(r.labelsSkipped == 1, "LB counted as skipped");
        Check(qAbs(r.cutStripsMm[0].last().x() - 5.0f) < 1e-4f, "PU coordinates converted to mm");
    }

    PlotterLink link;
    QStringList lastLines;
    bool lastOk = false;
    QString lastCmd;
    int finished = 0;
    QObject::connect(&link, &PlotterLink::commandFinished, [&](const QString &c, bool ok, const QStringList &l) {
        if (c == QLatin1String("status")) {
            return;
        }
        lastCmd = c;
        lastOk = ok;
        lastLines = l;
        finished++;
    });
    bool sent = false;
    QString failure;
    QObject::connect(&link, &PlotterLink::jobSent, [&]() { sent = true; });
    QObject::connect(&link, &PlotterLink::jobFailed, [&](const QString &m) { failure = m; });

    std::printf("connect %s\n", qPrintable(host));
    QString err;
    Check(link.connectEthernet(host, &err), "console connects");
    if (!link.isConnected()) {
        std::printf("  %s\n", qPrintable(err));
        return 1;
    }
    Check(WaitFor([&]() { return link.status().valid; }, 3000), "status arrives");
    std::printf("  state=%s x=%.3f\n", qPrintable(link.status().state), link.status().xMm);

    std::printf("commands\n");
    link.sendCommand("config");
    Check(WaitFor([&]() { return finished >= 1; }, 3000) && lastOk && lastCmd == "config", "config ok");
    Check(lastLines.size() > 20, "config lists settings");
    link.sendCommand("set draw_mm_s 80");
    link.sendCommand("set no_such_key 1");
    Check(WaitFor([&]() { return finished >= 3; }, 3000), "queued commands finish in order");
    Check(lastCmd == "set no_such_key 1" && !lastOk, "unknown key reports error:");
    link.sendCommand("goto 10 20");
    Check(WaitFor([&]() { return finished >= 4; }, 3000) && lastOk, "goto ok");
    Check(WaitFor([&]() { return qAbs(link.status().xMm - 10.0) < 0.01 && qAbs(link.status().yMm - 20.0) < 0.01; },
                  3000),
          "status follows goto");

    std::printf("job\n");
    QByteArray job("IN;SP1;");
    for (int i = 0; i < 4000; i++) {
        job += QByteArray("PU") + QByteArray::number(i % 400) + "," + QByteArray::number(i % 300) + ";PD" +
               QByteArray::number(i % 400 + 40) + "," + QByteArray::number(i % 300) + ";";
    }
    job += "PU0,0;SP0;";
    Check(link.startJob(job, &err), "job starts");
    Check(!link.startJob(job, &err), "second job refused while sending");
    Check(WaitFor([&]() { return link.status().state == "plotting"; }, 5000), "board reports plotting");
    Check(WaitFor([&]() { return sent || !failure.isEmpty(); }, 60000) && sent, "whole job sent");
    Check(WaitFor([&]() { return link.status().state == "idle"; }, 10000), "board back to idle");
    Check(qAbs(link.status().xMm) < 0.01 && qAbs(link.status().yMm) < 0.01, "pen ends at PU0,0");

    std::printf("abort during a job\n");
    Check(link.startJob(job, &err), "job starts");
    WaitFor([&]() { return link.status().state == "plotting"; }, 5000);
    link.cancelJob();
    link.sendCommand("abort");
    Check(WaitFor([&]() { return link.status().state == "idle"; }, 5000), "idle after abort");
    Check(!link.jobActive(), "job no longer active");

    link.disconnectLink();
    Check(!link.isConnected(), "disconnects");
    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
