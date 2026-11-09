/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "FirmwareUpdate.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>

#include <algorithm>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace fw {
namespace {

constexpr qint64 kAppOffset = 0x4000;      // after the bootloader
constexpr qint64 kFlashEnd = 0x80000;      // SAME53N19: 512 KB of flash
constexpr int kBootloaderWaitMs = 15000;
constexpr int kBootWaitMs = 40000;  // a DHCP board with no server takes a while to settle

// The board's console over USB, one command at a time. On USB the port carries HP-GL until "menu" on a
// line of its own opens the console; "exit" gives the port back to HP-GL. The console echoes what it
// is sent.
class Console {
public:
    ~Console() { close(); }

    bool open(const QString &name, int waitMs, QString *error) {
        m_port.setPortName(name);
        m_port.setBaudRate(115200);
        QElapsedTimer t;
        t.start();
        // The port is there a moment before Windows will let it be opened.
        while (!m_port.open(QIODevice::ReadWrite)) {
            if (t.elapsed() >= waitMs) {
                if (error) *error = m_port.errorString();
                return false;
            }
            QThread::msleep(250);
        }
        m_port.setDataTerminalReady(true);
        // A line ending first, so "menu" starts a line (anything half-sent ends there), then the console.
        m_port.write("\r\n");
        readUntilQuiet(400);
        m_port.write("menu\r\n");
        readUntilQuiet(1200);
        return true;
    }

    void close() {
        if (!m_port.isOpen()) return;
        m_port.write("exit\r\n");
        m_port.waitForBytesWritten(500);
        readUntilQuiet(300);
        m_port.close();
    }

    // True on "ok"; the lines before it, or the "error:" line, are in *lines.
    bool ask(const QString &line, int timeoutMs, QStringList *lines) {
        lines->clear();
        m_rx.clear();
        m_port.write(line.toUtf8() + "\r\n");
        m_port.flush();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            if (m_port.bytesAvailable() == 0 && !m_port.waitForReadyRead(100)) {
                if (m_port.error() != QSerialPort::NoError && m_port.error() != QSerialPort::TimeoutError) return false;
                continue;
            }
            m_rx += m_port.readAll();
            int nl;
            while ((nl = m_rx.indexOf('\n')) >= 0) {
                QString l = QString::fromUtf8(m_rx.left(nl)).trimmed();
                m_rx.remove(0, nl + 1);
                while (l.startsWith(QLatin1Char('>'))) l = l.mid(1).trimmed();  // the prompt
                if (l.isEmpty() || l == line || l.startsWith(QStringLiteral("TEL "))) continue;  // echo, telemetry
                if (l == QStringLiteral("ok") || l.startsWith(QStringLiteral("ok "))) {
                    if (l.size() > 2) lines->append(l);
                    return true;
                }
                lines->append(l);
                if (l.startsWith(QStringLiteral("error:"))) return false;
            }
        }
        return false;
    }

private:
    void readUntilQuiet(int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms + 1000 && m_port.waitForReadyRead(ms)) m_port.readAll();
        m_port.clear(QSerialPort::Input);
    }

    QSerialPort m_port;
    QByteArray m_rx;
};

QString VersionFromReply(const QStringList &lines) {
    static const QRegularExpression re(QStringLiteral("^ClearHPGL_Firmware (\\S+)$"));
    for (const QString &l : lines) {
        const auto m = re.match(l);
        if (m.hasMatch()) return m.captured(1);
    }
    return {};
}

// "key=value" fields of the first status line.
QHash<QString, QString> Fields(const QStringList &lines) {
    QHash<QString, QString> kv;
    for (const QString &l : lines) {
        if (!l.startsWith(QStringLiteral("state="))) continue;
        for (const QString &tok : l.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const int eq = tok.indexOf(QLatin1Char('='));
            if (eq > 0) kv.insert(tok.left(eq), tok.mid(eq + 1));
        }
    }
    return kv;
}

// The port of one board, by its USB serial number when both sides have one.
bool SameBoard(const Port &p, const QString &serial) { return serial.isEmpty() || p.serial.isEmpty() || p.serial == serial; }

}  // namespace

bool IsClearCore(quint16 vendor, quint16 product) {
    return vendor == kVendorId && (product == kAppProductId || product == kBootProductId);
}

QString VersionInImage(const QByteArray &image) {
    // The firmware's startup line, "ClearHPGL_Firmware x.y.z ready", is one string in the image.
    static const QRegularExpression re(QStringLiteral("ClearHPGL_Firmware (\\d+\\.\\d+\\.\\d+)"));
    const auto m = re.match(QString::fromLatin1(image));
    return m.hasMatch() ? m.captured(1) : QString();
}

QString CheckImage(const QByteArray &image) {
    if (image.size() < 1024) return QStringLiteral("the firmware image is empty or cut short");
    if (image.size() > kFlashEnd - kAppOffset) return QStringLiteral("the firmware image is too big for the board");
    // A Cortex-M image starts with its stack pointer, in RAM at 0x2000_0000.
    if (static_cast<quint8>(image[3]) != 0x20) return QStringLiteral("the file is not a ClearCore firmware image");
    return {};
}

Package FindPackage() {
    Package p;
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList binDirs, bossacs;
    const QString override = qEnvironmentVariable("CLEARHPGL_FIRMWARE_DIR");
    if (!override.isEmpty()) binDirs << override;
    binDirs << appDir + QStringLiteral("/firmware");
    bossacs << appDir + QStringLiteral("/firmware/bossac/bossac.exe");
    // From a build tree: up to the repository.
    QDir up(appDir);
    for (int i = 0; i < 5 && up.cdUp(); ++i) {
        binDirs << up.filePath(QStringLiteral("Firmware/ClearCoreFirmware/Release"));
        bossacs << up.filePath(QStringLiteral("tools/bossac/bossac.exe"));
    }
    for (const QString &d : binDirs) {
        const QString bin = QDir(d).filePath(QStringLiteral("ClearHPGL_Firmware.bin"));
        if (QFileInfo::exists(bin)) {
            p.bin = bin;
            break;
        }
    }
    for (const QString &b : bossacs) {
        if (QFileInfo::exists(b)) {
            p.bossac = b;
            break;
        }
    }
    if (p.bin.isEmpty()) {
        p.error = QStringLiteral("The firmware isn't with this copy of the app (no firmware\\ClearHPGL_Firmware.bin).");
        return p;
    }
    if (p.bossac.isEmpty()) {
        p.error = QStringLiteral("The flasher isn't with this copy of the app (no firmware\\bossac\\bossac.exe).");
        return p;
    }
    QFile f(p.bin);
    if (!f.open(QIODevice::ReadOnly)) {
        p.error = QStringLiteral("Can't read %1: %2").arg(p.bin, f.errorString());
        return p;
    }
    const QByteArray image = f.readAll();
    p.error = CheckImage(image);
    if (!p.error.isEmpty()) return p;
    p.version = VersionInImage(image);
    p.ok = true;
    return p;
}

QList<Port> ClearCorePorts() {
    QList<Port> ports;
    for (const QSerialPortInfo &i : QSerialPortInfo::availablePorts()) {
        if (!IsClearCore(i.vendorIdentifier(), i.productIdentifier())) continue;
        ports.append({i.portName(), i.serialNumber(), i.productIdentifier() == kBootProductId});
    }
    return ports;
}

Survey Inspect(const Port &port) {
    Survey s;
    if (port.bootloader) {
        s.kind = Kind::Bootloader;
        return s;
    }
    Console c;
    if (!c.open(port.name, 2000, &s.detail)) return s;
    QStringList r;
    const bool ok = c.ask(QStringLiteral("version"), 1500, &r);
    s.version = ok ? VersionFromReply(r) : QString();
    if (s.version.isEmpty()) {
        s.kind = Kind::Other;
        s.detail = r.join(QLatin1Char(' ')).left(80);
        return s;
    }
    s.kind = Kind::ClearHPGL;
    if (!c.ask(QStringLiteral("status"), 2000, &r)) {
        s.blocked = QStringLiteral("the board didn't answer status");
        return s;
    }
    const auto kv = Fields(r);
    const QString state = kv.value(QStringLiteral("state"));
    s.enabled = kv.value(QStringLiteral("enabled")) == QStringLiteral("1");
    if (state == QStringLiteral("plotting") || state == QStringLiteral("paused") || state == QStringLiteral("buffering") ||
        kv.value(QStringLiteral("queue"), QStringLiteral("0")) != QStringLiteral("0")) {
        s.blocked = QStringLiteral("a job is on the plotter (%1)").arg(state);
    }
    return s;
}

Updater::Updater(const Package &package, const Port &port, QObject *parent)
    : QObject(parent), m_package(package), m_port(port) {}

bool Updater::bossac(const QStringList &args, int timeoutMs, bool quiet) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
    p.start(m_package.bossac, args);
    if (!p.waitForStarted(5000)) {
        emit log(QStringLiteral("bossac didn't start: %1").arg(p.errorString()));
        return false;
    }
    static const QRegularExpression bar(QStringLiteral("\\((\\d+)/(\\d+) pages\\)|(\\d+)%"));
    QElapsedTimer t;
    t.start();
    QByteArray rx;
    auto drain = [&]() {
        rx += p.readAll();
        int end;
        // Its progress bar rewrites one line with carriage returns.
        while ((end = rx.indexOf('\n')) >= 0 || (end = rx.indexOf('\r')) >= 0) {
            const QString l = QString::fromLocal8Bit(rx.left(end)).trimmed();
            rx.remove(0, end + 1);
            if (l.isEmpty()) continue;
            const auto m = bar.match(l);
            if (m.hasMatch() && l.startsWith(QLatin1Char('['))) {
                const int pct = m.captured(3).isEmpty() ? m.captured(1).toInt() * 100 / std::max(1, m.captured(2).toInt()) : m.captured(3).toInt();
                emit percent(pct);
            } else if (!quiet) {
                emit log(l);
            }
        }
    };
    while (p.state() != QProcess::NotRunning) {
        p.waitForReadyRead(200);
        drain();
        if (t.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(2000);
            emit log(QStringLiteral("bossac timed out"));
            return false;
        }
    }
    drain();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool Updater::enterBootloader(Port *boot, QString *error) {
    QStringList before;
    for (const Port &p : ClearCorePorts()) {
        if (p.bootloader) before << p.name;
    }
    emit step(QStringLiteral("Starting the bootloader..."));
    // Opening at 1200 baud is the request. The board leaves at once, so bossac reports that it can't reach the
    // port: that isn't a fault, and whether it worked is whether the bootloader's port appears.
    bossac({QStringLiteral("--info"), QStringLiteral("--arduino-erase"), QStringLiteral("--port=") + m_port.name}, 20000, true);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < kBootloaderWaitMs) {
        for (const Port &p : ClearCorePorts()) {
            if (p.bootloader && !before.contains(p.name) && SameBoard(p, m_port.serial)) {
                *boot = p;
                return true;
            }
        }
        QThread::msleep(250);
    }
    *error = QStringLiteral("The board didn't start its bootloader. Double-tap the reset button on the ClearCore "
                            "(the IO-0 to IO-5 lights walk), then update again.");
    return false;
}

Result Updater::run() {
    Result res;
    QStringList others;  // other ClearCores: never the one to talk to afterwards
    for (const Port &p : ClearCorePorts()) {
        if (p.name != m_port.name && !p.bootloader) others << p.name;
    }

    // 1. Look again, it may have changed since the window showed it.
    emit step(QStringLiteral("Reading the board..."));
    const Survey s = Inspect(m_port);
    if (s.kind == Kind::Unreachable) {
        res.message = QStringLiteral("Can't open %1: %2").arg(m_port.name, s.detail);
        return res;
    }
    if (!s.blocked.isEmpty()) {
        res.message = QStringLiteral("Not updated: %1.").arg(s.blocked);
        return res;
    }

    // 2. Motors off before the board restarts, so they don't drop out mid-update.
    if (s.kind == Kind::ClearHPGL && s.enabled) {
        emit step(QStringLiteral("Disabling the motors..."));
        Console c;
        QString error;
        QStringList r;
        if (!c.open(m_port.name, 2000, &error) || !c.ask(QStringLiteral("disable"), 3000, &r)) {
            res.message = QStringLiteral("Not updated: the motors could not be disabled (%1).")
                              .arg(error.isEmpty() ? (r.isEmpty() ? QStringLiteral("no reply") : r.last()) : error);
            return res;
        }
        emit log(QStringLiteral("Motors disabled."));
    }

    // 3. Flash.
    Port boot = m_port;
    if (!m_port.bootloader) {
        QString error;
        if (!enterBootloader(&boot, &error)) {
            res.message = error;
            return res;
        }
    }
    emit log(QStringLiteral("Bootloader on %1").arg(boot.name));
    emit step(QStringLiteral("Writing firmware %1...").arg(m_package.version));
    res.flashed = true;  // from here the chip may be erased
    if (!bossac({QStringLiteral("--info"), QStringLiteral("--port=") + boot.name, QStringLiteral("--usb-port"), QStringLiteral("--write"),
                 QStringLiteral("--erase"), QStringLiteral("--verify"), QStringLiteral("--offset=0x4000"), QStringLiteral("--reset"),
                 QDir::toNativeSeparators(m_package.bin)},
                120000)) {
        res.message = QStringLiteral("Flashing failed and the board has no working firmware. Double-tap its reset button "
                                     "(the IO-0 to IO-5 lights walk), press Refresh, then update again.");
        return res;
    }

    // 4. The new firmware, on the same board.
    emit step(QStringLiteral("Waiting for the board to start..."));
    Console c;
    QStringList r;
    QString running, error;
    QElapsedTimer t;
    t.start();
    bool opened = false;
    while (!opened && t.elapsed() < kBootWaitMs) {
        for (const Port &p : ClearCorePorts()) {
            if (p.bootloader || others.contains(p.name) || !SameBoard(p, m_port.serial)) continue;
            if (c.open(p.name, 3000, &error)) {
                opened = true;
                break;
            }
        }
        if (!opened) QThread::msleep(500);
    }
    if (!opened) {
        res.message = QStringLiteral("The firmware was written, but the board's port didn't come back%1.")
                          .arg(error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(error));
        return res;
    }
    for (int i = 0; i < 6 && running.isEmpty(); ++i) {
        if (c.ask(QStringLiteral("version"), 2000, &r)) running = VersionFromReply(r);
    }
    if (m_package.version.isEmpty()) {
        res.ok = true;
        res.message = QStringLiteral("Flashed a firmware that isn't ClearHPGL.");
        return res;
    }
    if (running != m_package.version) {
        res.message = QStringLiteral("The board reports firmware %1, not %2.")
                          .arg(running.isEmpty() ? QStringLiteral("nothing") : running, m_package.version);
        return res;
    }
    res.ok = true;
    res.message = QStringLiteral("Firmware %1 is running. The plotter settings and the network address are unchanged.").arg(running);
    return res;
}

}  // namespace fw
