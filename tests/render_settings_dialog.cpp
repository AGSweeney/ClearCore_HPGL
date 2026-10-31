// Renders the Plotter settings dialog to a PNG for a visual check.
//   render_settings_dialog <config.txt> <out.png>
// config.txt holds the console's "config" reply (one "key = value  help" per line).

#include "MainWindow.h"
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
        /* Where a job lands on the bed, through the firmware interpreter: --hpgl-extent <file> [ident] */
        QFile hf(QString::fromLocal8Bit(argv[2]));
        if (!hf.open(QIODevice::ReadOnly)) {
            return 3;
        }
        const HpglPreviewResult r = PreviewHpgl(hf.readAll(), 400.0, 300.0, argc > 3 ? argv[3] : "7475A");
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto &s : r.cutStripsMm) {
            for (const QVector3D &p : s) {
                x0 = qMin(x0, p.x()); y0 = qMin(y0, p.y()); x1 = qMax(x1, p.x()); y1 = qMax(y1, p.y());
            }
        }
        std::printf("strokes=%d drawn=%.0f mm  extent X %.1f..%.1f  Y %.1f..%.1f mm  unknown=%u error=%d\n",
                    int(r.cutStripsMm.size()), r.drawLengthMm, x0, x1, y0, y1, r.unknownInstructions, r.lastError);
        return 0;
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
