/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

// Renders the Plotter settings dialog to a PNG for a visual check.
//   render_settings_dialog <config.txt> <out.png>
// config.txt holds the console's "config" reply (one "key = value  help" per line).

#include "FirmwareDialog.h"
#include "MainWindow.h"
#include "ScopeWindow.h"
#include "SwitchWizard.h"
#include "PlotterSettingsDialog.h"
#include "HpglPreview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QPushButton>

#include <cmath>

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
    if (QString::fromLocal8Bit(argv[1]) == QLatin1String("--wizard")) {
        /* Plotter > Set up limits and homing, each step fed as a plotter would: --wizard <out-prefix> */
        QMap<QString, QString> cfg{{"dir_x", "1"}, {"width_mm", "600"}, {"pen_mode", "0"}, {"pen_io", "0"},
                                   {"switch_open", "0"}};
        MainWindow parent;
        SwitchWizard w(nullptr, cfg, &parent);
        w.show();
        const QString out = QString::fromLocal8Bit(argv[2]);
        int fails = 0;
        auto check = [&fails](bool c, const char *what) {
            std::printf("  %s %s\n", c ? "PASS" : "FAIL", what);
            fails += c ? 0 : 1;
        };
        auto shot = [&](const char *name) {
            QCoreApplication::processEvents();
            w.grab().save(out + QLatin1String(name) + QLatin1String(".png"));
        };
        w.setWantedForTest(true, true, true);
        shot("1_axis");
        w.setStepForTest(SwitchWizard::kStepFind);
        const int rest = (1 << 7) | (1 << 8);  /* DI-7, DI-8 closed at rest; A-9 open */
        w.feedLevels(rest);
        // Find each: press, release
        struct { int sw; int bit; } finds[] = {{SwitchWizard::kPlus, 7}, {SwitchWizard::kMinus, 8}, {SwitchWizard::kHome, 9}};
        for (auto f : finds) {
            w.findSwitch(f.sw);
            w.feedLevels(rest ^ (1 << f.bit));
            w.feedLevels(rest);
        }
        check(w.found(SwitchWizard::kPlus).input == 7 && w.found(SwitchWizard::kMinus).input == 8 &&
                  w.found(SwitchWizard::kHome).input == 9, "found DI-7, DI-8, A-9");
        check(!w.found(SwitchWizard::kPlus).normallyOpen && w.found(SwitchWizard::kHome).normallyOpen,
              "contacts from the rest levels");
        shot("2_find");
        w.setStepForTest(SwitchWizard::kStepContact);
        shot("3_contact");
        w.setStepForTest(SwitchWizard::kStepDirection);
        w.feedStop(+1, QStringLiteral("A-9"));
        check(w.homeSide() == 1, "home seen on the + side");
        w.feedStop(-1, QStringLiteral("DI-7"));  /* the + limit going -: ask */
        shot("4_direction_ask");
        w.answer(false);                          /* swapped in Find */
        check(w.found(SwitchWizard::kMinus).input == 7 && w.found(SwitchWizard::kMinus).proved, "swapped");
        w.feedStop(+1, QStringLiteral("DI-8"));
        check(w.found(SwitchWizard::kPlus).proved, "+ proved");
        shot("4_direction");
        const QStringList lim = w.limitCommands();
        check(lim.contains(QStringLiteral("set x_limit_plus DI-8")) && lim.contains(QStringLiteral("set switch_open 4")),
              "limit commands");
        w.setStepForTest(SwitchWizard::kStepLimits);
        auto swinfo = [&](int tripsP, int trP, double atP, int tripsM, int trM, double atM, double pos, int moving) {
            w.feedSwinfo({QStringLiteral("switch x+ input=DI-8 open=0 tripped=%1 trips=%2 at=%3").arg(trP).arg(tripsP).arg(atP),
                          QStringLiteral("switch x- input=DI-7 open=0 tripped=%1 trips=%2 at=%3").arg(trM).arg(tripsM).arg(atM),
                          QStringLiteral("pos x=%1 y=0.000 moving=%2").arg(pos).arg(moving)});
        };
        swinfo(0, 0, 0, 0, 0, 0, 10, 0);
        swinfo(1, 1, 120.5, 0, 0, 0, 120.9, 1);
        swinfo(1, 1, 120.5, 0, 0, 0, 120.8, 0);
        swinfo(1, 0, 120.5, 0, 0, 0, 115.0, 0);
        swinfo(1, 0, 120.5, 1, 1, -3.25, -3.5, 0);
        swinfo(1, 0, 120.5, 1, 0, -3.25, 2.0, 0);
        check(w.limitStopped(0) && w.limitCleared(0) && std::fabs(w.overtravelMm(0) - 0.3) < 1e-6, "+ limit stop and overtravel");
        check(w.limitStopped(1) && w.limitCleared(1), "- limit stop");
        shot("5_limits");
        w.setStepForTest(SwitchWizard::kStepHome);
        w.startHomeRunsForTest(3);
        auto home = [&](int ends, double latch, const char *last) {
            w.feedSwinfo({QStringLiteral("home mode=1 state=idle homed=1 x_ends=%1 x_latch=%2 y_ends=0 y_latch=0.000 last=%3")
                              .arg(ends).arg(latch).arg(QLatin1String(last))});
        };
        home(0, 0, "none");
        home(1, -40.0, "ok");
        home(2, 0.004, "ok");
        home(3, -0.006, "ok");
        check(w.homeRuns() == 3 && std::fabs(w.homeSpreadMm() - 0.010) < 1e-9, "3 homings, spread 0.010 mm");
        shot("6_home");
        const QStringList hc = w.homeCommands();
        check(hc.contains(QStringLiteral("set x_home A-9")) && hc.contains(QStringLiteral("set home_mode 1")), "home commands");
        w.setStepForTest(SwitchWizard::kStepReview);
        shot("7_review");
        for (const QString &c : w.applyCommands()) std::printf("    %s\n", qPrintable(c));
        std::printf("%d failures\n", fails);
        return fails ? 1 : 0;
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
