// Renders the Plotter settings dialog to a PNG for a visual check.
//   render_settings_dialog <config.txt> <out.png>
// config.txt holds the console's "config" reply (one "key = value  help" per line).

#include "MainWindow.h"
#include "PlotterSettingsDialog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include <cstdlib>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        return 2;
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
        const bool ok = w.grab().save(QString::fromLocal8Bit(argv[2]));
        w.close();
        return ok ? 0 : 1;
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
