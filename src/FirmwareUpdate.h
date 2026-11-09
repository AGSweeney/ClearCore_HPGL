/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// Puts the firmware the app was built with on a ClearCore over USB (Plotter > Update board firmware).
// From the Electronic CAM Workbench (ECamUI), without its profile backup: ClearHPGL keeps all its settings
// in the ClearCore's NVM user page, which flashing does not erase, so the plotter settings, the network
// address and the plot area stay.
//
// Everything here blocks: run it off the UI thread.

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace fw {

// A ClearCore's two USB identities. Vendor 0x2890 alone is not enough: ClearPath motors have it too.
constexpr quint16 kVendorId = 0x2890;
constexpr quint16 kAppProductId = 0x8022;   // running firmware
constexpr quint16 kBootProductId = 0x0022;  // in its bootloader
bool IsClearCore(quint16 vendor, quint16 product);

// What the app carries: firmware\ next to the exe when installed or deployed, the build folders when
// developing. CLEARHPGL_FIRMWARE_DIR in the environment points somewhere else.
struct Package {
    bool ok = false;
    QString error;
    QString bin;
    QString bossac;
    QString version;
};
Package FindPackage();
// The version in a firmware image ("ClearHPGL_Firmware x.y.z"), empty if it isn't a ClearHPGL image.
QString VersionInImage(const QByteArray &image);
// Why an image can't go on the board, empty if it can.
QString CheckImage(const QByteArray &image);

struct Port {
    QString name;
    QString serial;
    bool bootloader = false;
};
QList<Port> ClearCorePorts();

enum class Kind { Unreachable, Bootloader, ClearHPGL, Other };
struct Survey {
    Kind kind = Kind::Unreachable;
    QString version;  // ClearHPGL firmware's version
    QString detail;   // why it can't be reached, or what other firmware said
    bool enabled = false;  // motors enabled: the update disables them first
    QString blocked;  // why it can't be updated now, empty if it can
};
Survey Inspect(const Port &port);

struct Result {
    bool ok = false;
    bool flashed = false;
    QString message;
};

class Updater : public QObject {
    Q_OBJECT
public:
    Updater(const Package &package, const Port &port, QObject *parent = nullptr);
    Result run();

signals:
    void step(const QString &text);
    void percent(int value);
    void log(const QString &line);

private:
    bool bossac(const QStringList &args, int timeoutMs, bool quiet = false);
    bool enterBootloader(Port *boot, QString *error);

    Package m_package;
    Port m_port;
};

}  // namespace fw
