/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// ClearPath MC/SD USB link (the motor's own USB port, the one ClearPath-MSP uses), for the scope.
// A port of the clean part of the internal ClearPath USB tool: framing and command numbers follow
// Teknic's public sFoundation SDK; parameter meanings, the scope stream and the link behaviour come
// from our own USB captures (ClearCore_ROS docs/CLEARPATH_USB.md). Nothing here comes from Teknic's
// closed binaries.
//
// Rules kept from the tool:
//   - never send a serial break (it latches the node's EStopped alert);
//   - open with the link-reset pair MSP sends (type 3 and type 7, payload 00), then set-address;
//   - never set the save-to-motor flag (0x4000) on a monitor channel; put channel 1 back when the
//     scope stops, stop the stream twice, and return the link to 9600 baud.
// One program owns the COM port at a time: close ClearPath-MSP first.

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

class QSerialPort;

namespace clearpath {

// ---- Codec (public SDK framing) ------------------------------------------------------------------

// 8-bit bytes -> 7-bit link characters, 7 bytes per 8 characters (n + 1 for a short tail).
QByteArray Pack7(const QByteArray &data);
// A packet: start/type/address, length (+ source bit 0x40 for node-originated), payload, checksum.
QByteArray Frame(int type, int addr, const QByteArray &payload, bool fromNode = false);
// Link characters back to bytes (n bytes, little-endian).
QByteArray Unpack(const QByteArray &chars, int n);
// The value of the first `size` bytes, sign-extended when `isSigned`.
int64_t Value(const QByteArray &chars, int size, bool isSigned);

struct Packet {
    QByteArray raw;
    double t = 0.0;  // seconds on the link's clock when it arrived
    int type() const { return (static_cast<uint8_t>(raw[0]) >> 4) & 7; }
    bool fromNode() const { return (static_cast<uint8_t>(raw[1]) & 0x40) != 0; }
    bool isStream() const { return type() == 3 && fromNode(); }
    QByteArray chars() const { return raw.mid(2, static_cast<uint8_t>(raw[1]) & 0x1F); }
};

// ---- Transport -----------------------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;
    virtual void write(const QByteArray &bytes) = 0;
    // Bytes received so far, waiting up to `ms` for the first one.
    virtual QByteArray read(int ms) = 0;
    virtual void setBaud(int baud) = 0;
    virtual int baud() const = 0;
    virtual void discardInput() = 0;
};

// The motor's COM port. No break, DTR and RTS off (as .NET's SerialPort defaults in the tool).
std::unique_ptr<Transport> OpenSerial(const QString &portName, QString *error);

// A simulated MC/SD node speaking the same protocol: replies to the commands above, streams scope
// samples at about 845 per second while the scope runs, and moves through a repeating cam-like
// motion so every trace has something to show. For trying the scope without a motor, and for tests.
std::unique_ptr<Transport> OpenSimulated();
constexpr const char *kSimulatedPort = "Simulated motor";

// ---- Link ----------------------------------------------------------------------------------------

struct ParamDef {
    const char *name;
    int number;  // bank << 8 | index
    int size;
    bool isSigned;
};

struct MonState {
    int var = 0, gain = 0, filter = 0, sync = 0;
    bool operator==(const MonState &o) const {
        return var == o.var && gain == o.gain && filter == o.filter && sync == o.sync;
    }
};

// One scope sample: the monitor value (bytes 2-3) and the digital lanes (byte 6).
struct StreamSample {
    double t = 0.0;
    int raw = 0;
    int counter = 0;     // byte 1 bits 0-1, counts 0..3
    uint8_t digital = 0; // byte 6: bit 3 HLFB-like, bit 0 enable-like (provisional)
};

class Link {
public:
    static constexpr int kBaud = 9600, kStreamBaud = 230400;
    static constexpr int kMonSaveNv = 0x4000;

    // Takes the transport; throws std::runtime_error when the node does not answer.
    explicit Link(std::unique_ptr<Transport> t);
    ~Link();

    int nodeCount() const { return m_nodes; }
    int badPackets() const { return m_bad; }
    int currentBaud() const { return m_t->baud(); }

    void pump(int waitMs = 0);  // move received bytes into packets
    QByteArray command(const QByteArray &payload, int type = 0, int ms = 500);
    void sendNoWait(const QByteArray &payload);

    QByteArray getChars(const ParamDef &p);
    int64_t get(const ParamDef &p);
    void setRate(int baud);
    MonState monitorGet(int ch);
    void monitorSet(int ch, const MonState &s);  // working setting only; refuses the save flag
    void scope(int mode);                        // 3 = channel 1, 0 = stop
    void keepalive();
    void clearEstop();

    // Scope samples received since the last call.
    std::vector<StreamSample> takeStream();
    double now() const { return m_clock.elapsed() / 1000.0; }

private:
    std::unique_ptr<Transport> m_t;
    QByteArray m_rx;
    std::deque<Packet> m_replies;
    std::vector<StreamSample> m_stream;
    int m_bad = 0, m_nodes = 0;
    QElapsedTimer m_clock;
    Packet *ReadReply(int ms, Packet *out);
};

// ---- What we know about the registers and monitor variables ---------------------------------------

namespace params {
extern const ParamDef DeviceId, FirmwareVersion, SerialNumber, AlertRegister, StatusRealtime, PositionMeasured,
    PositionCommanded, PositionError, VelocityA, DriveTemperature, BusVoltage, InputResolution;
}

struct MonVar {
    const char *name;
    int id;
    int gain;  // the gain ClearPath-MSP writes for its default vertical scale (capture)
};
const std::vector<MonVar> &MonVars();
const MonVar *FindMonVar(int id);
// Physical value of a stream sample: counts for position error, RPM for velocity, raw otherwise.
double MonScale(const MonVar &v, int raw);
const char *MonUnit(const MonVar &v);

// The alert register's bits by name (ClearPath-SC layout from the public SDK; provisional on MC/SD).
QStringList AlertNames(const QByteArray &chars);

// Places stream samples in time. They arrive in USB bursts, so arrival times are rough: space them by the
// node's own period, using the 2-bit counter to step over lost ones, and lean gently on arrival so the
// result stays on the PC's clock. Drives share no clock, so that is what lines them up with each other.
class StreamClock {
public:
    static constexpr double kPeriod = 1.0 / 844.35;  // measured on three drives: 844.35 to 844.36 samples/s
    double place(double arrival, int counter);
    void reset() { m_have = false; }
    int lost() const { return m_lost; }
    int anchors() const { return m_anchors; }  // times it had to jump back onto the clock

private:
    bool m_have = false;
    double m_t = 0.0;
    int m_counter = 0, m_lost = 0, m_anchors = 0;
};

// Every ClearPath's COM port (USB 2890:0201), lowest number first.
QStringList FindPorts();

}  // namespace clearpath
