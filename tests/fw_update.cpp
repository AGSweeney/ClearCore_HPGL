/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
// Bench check of Plotter > Update board firmware, through the app's own code (src/FirmwareUpdate.cpp)
// against a real ClearCore on USB.
//
//   fw_update survey            list the ClearCores on USB and what each one runs
//   fw_update update <COMn>     put the app's firmware on that board and check it runs
//
// CLEARHPGL_FIRMWARE_DIR picks the image (default: the build tree). Exit code 0 = passed.

#include "FirmwareUpdate.h"

#include <QCoreApplication>
#include <QDir>

#include <cstdio>

static const char *KindName(fw::Kind k) {
    switch (k) {
    case fw::Kind::Unreachable: return "unreachable";
    case fw::Kind::Bootloader: return "bootloader";
    case fw::Kind::ClearHPGL: return "ClearHPGL";
    case fw::Kind::Other: return "other firmware";
    }
    return "?";
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::puts("fw_update survey | fw_update update <COMn>");
        return 2;
    }
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const fw::Package pkg = fw::FindPackage();
    std::printf("package: %s\n", pkg.ok ? qPrintable(QStringLiteral("firmware %1, %2, bossac %3")
                                                         .arg(pkg.version, QDir::toNativeSeparators(pkg.bin),
                                                              QDir::toNativeSeparators(pkg.bossac)))
                                        : qPrintable(pkg.error));
    const QList<fw::Port> ports = fw::ClearCorePorts();
    if (mode == QLatin1String("survey")) {
        if (ports.isEmpty()) std::puts("no ClearCore on USB");
        for (const fw::Port &p : ports) {
            const fw::Survey s = fw::Inspect(p);
            std::printf("%s serial=%s: %s %s%s%s\n", qPrintable(p.name), qPrintable(p.serial), KindName(s.kind),
                        qPrintable(s.version), s.enabled ? " (motors enabled)" : "",
                        s.blocked.isEmpty() ? "" : qPrintable(QStringLiteral(" blocked: ") + s.blocked));
        }
        return pkg.ok ? 0 : 1;
    }
    if (mode != QLatin1String("update") || argc < 3 || !pkg.ok) return 2;
    const QString name = QString::fromLocal8Bit(argv[2]);
    for (const fw::Port &p : ports) {
        if (p.name != name) continue;
        fw::Updater u(pkg, p);
        QObject::connect(&u, &fw::Updater::step, [](const QString &t) { std::printf("step: %s\n", qPrintable(t)); });
        QObject::connect(&u, &fw::Updater::log, [](const QString &t) { std::printf("  %s\n", qPrintable(t)); });
        int last = -1;
        QObject::connect(&u, &fw::Updater::percent, [&last](int v) {
            if (v / 25 != last / 25) std::printf("  %d%%\n", v);
            last = v;
        });
        const fw::Result r = u.run();
        std::printf("%s %s\n", r.ok ? "ok  " : "FAIL", qPrintable(r.message));
        return r.ok ? 0 : 1;
    }
    std::printf("FAIL no ClearCore on %s\n", qPrintable(name));
    return 1;
}
