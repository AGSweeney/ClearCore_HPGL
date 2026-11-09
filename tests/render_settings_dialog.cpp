/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

// Renders the Plotter settings dialog to a PNG for a visual check.
//   render_settings_dialog <config.txt> <out.png>
// config.txt holds the console's "config" reply (one "key = value  help" per line).

#include "FirmwareDialog.h"
#include "MainWindow.h"
#include "ScopeWindow.h"
#include "PlotterSettingsDialog.h"
#include "HpglPreview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        return 2;
    }
    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--hpgl-extent")) {
        /* Where a job lands on the bed, through the firmware interpreter: --hpgl-extent <file> [ident] [w h] */
        QFile hf(QString::fromLocal8Bit(argv[2]));
        if (!hf.open(QIODevice::ReadOnly)) {
            return 3;
        }
        const double bedW = argc > 4 ? atof(argv[4]) : 400.0, bedH = argc > 5 ? atof(argv[5]) : 300.0;
        const HpglPreviewResult r = PreviewHpgl(hf.readAll(), bedW, bedH, argc > 3 ? argv[3] : "7475A");
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto &s : r.cutStripsMm) {
            for (const QVector3D &p : s) {
                x0 = qMin(x0, p.x()); y0 = qMin(y0, p.y()); x1 = qMax(x1, p.x()); y1 = qMax(y1, p.y());
            }
        }
        std::printf("strokes=%d drawn=%.0f mm  pen-up travel=%.0f mm  extent X %.1f..%.1f  Y %.1f..%.1f mm  unknown=%u error=%d\n",
                    int(r.cutStripsMm.size()), r.drawLengthMm, r.travelLengthMm, x0, x1, y0, y1, r.unknownInstructions, r.lastError);
        return 0;
    }
    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--scope")) {
        /* ClearPath scope on simulated motors, recording for a few seconds:
         * --scope <out.png> [help.png | -] [motors] */
        ScopeWindow w;
        w.resize(1400, 860);
        w.show();
        w.startSimulated(argc > 4 ? atoi(argv[4]) : 1);
        {
            /* Every motor's alert lane on (a second motor starts with only its fast signal). */
            std::vector<scope::Lane> lanes = w.lanes();
            for (scope::Lane &l : lanes) {
                if (scope::BaseKey(l.key) == QLatin1String("dig.alert")) {
                    l.on = true;
                }
            }
            w.setLanes(lanes);
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 4000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        const bool ok = w.grab().save(QString::fromLocal8Bit(argv[2]));
        if (argc > 3 && QString::fromLocal8Bit(argv[3]) != QLatin1String("-")) {
            w.ShowHelp();
            t.restart();
            while (t.elapsed() < 800) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }
            if (!w.helpDialog() || !w.helpDialog()->grab().save(QString::fromLocal8Bit(argv[3]))) {
                return 1;
            }
        }
        w.stopAndDisconnect();
        return ok ? 0 : 1;
    }
    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--firmware")) {
        /* Plotter > Update board firmware, after it has read the board on USB: --firmware <out.png> */
        MainWindow parent;
        FirmwareDialog d(&parent);
        d.resize(720, 520);
        d.show();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 6000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        return d.grab().save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    }
    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--main")) {
        /* Main window at its default size, after the first layout pass (initial splitter sizes). */
        MainWindow w;
        if (argc > 4) {
            w.resize(atoi(argv[3]), atoi(argv[4]));  // optional window size
        }
        w.show();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        // No close(): closeEvent would save this window's layout into the user's real settings.
        return w.grab().save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 3;
    }
    QStringList lines;
    QTextStream in(&f);
    while (!in.atEnd()) {
        lines << in.readLine();
    }
    MainWindow parent;  // same parent and inherited style sheet as in the app
    PlotterSettingsDialog dlg(lines, &parent);
    dlg.resize(660, argc > 3 ? atoi(argv[3]) : 1100);  // tall enough to show every section in one image
    return dlg.grab().save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
}
