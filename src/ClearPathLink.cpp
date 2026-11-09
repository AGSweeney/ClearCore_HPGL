/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "ClearPathLink.h"

#include <QSerialPort>
#include <QCollator>
#include <QSerialPortInfo>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace clearpath {

// ---- Codec ---------------------------------------------------------------------------------------

QByteArray Pack7(const QByteArray &data) {
    QByteArray out;
    for (int i = 0; i < data.size(); i += 7) {
        const int n = std::min(7, static_cast<int>(data.size()) - i);
        int s[7] = {};
        for (int k = 0; k < n; ++k) s[k] = static_cast<uint8_t>(data[i + k]);
        int d[8];
        d[0] = s[0] & 0x7F;
        for (int k = 1; k < 7; ++k) d[k] = ((s[k] << k) | (s[k - 1] >> (8 - k))) & 0x7F;
        d[7] = (s[6] >> 1) & 0x7F;
        for (int k = 0; k < n + 1; ++k) out.append(static_cast<char>(d[k]));
    }
    return out;
}

QByteArray Frame(int type, int addr, const QByteArray &payload, bool fromNode) {
    const QByteArray body = payload.isEmpty() ? QByteArray() : Pack7(payload);
    QByteArray pkt(body.size() + 3, '\0');
    pkt[0] = static_cast<char>(0x80 | ((type & 7) << 4) | (addr & 0xF));
    pkt[1] = static_cast<char>((body.size() & 0x1F) | (fromNode ? 0x40 : 0));
    for (int i = 0; i < body.size(); ++i) pkt[2 + i] = body[i];
    int sum = 0;
    for (int i = 0; i < pkt.size() - 1; ++i) sum += static_cast<uint8_t>(pkt[i]);
    pkt[pkt.size() - 1] = static_cast<char>(-sum & 0x7F);
    return pkt;
}

QByteArray Unpack(const QByteArray &chars, int n) {
    // Little-endian 7-bit groups into a little-endian byte string.
    QByteArray out(n, '\0');
    int bit = 0;
    for (char c : chars) {
        const int v = static_cast<uint8_t>(c) & 0x7F;
        for (int b = 0; b < 7; ++b, ++bit) {
            if (bit / 8 >= n) return out;
            if ((v >> b) & 1) out[bit / 8] = static_cast<char>(static_cast<uint8_t>(out[bit / 8]) | (1 << (bit % 8)));
        }
    }
    return out;
}

int64_t Value(const QByteArray &chars, int size, bool isSigned) {
    const int bits = std::min(8 * size, 7 * static_cast<int>(chars.size()));
    if (bits <= 0) return 0;
    const QByteArray b = Unpack(chars, 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(b[i]);
    if (bits < 64) v &= (uint64_t(1) << bits) - 1;
    if (isSigned && bits < 64 && ((v >> (bits - 1)) & 1)) v -= uint64_t(1) << bits;
    return static_cast<int64_t>(v);
}

// ---- Serial transport ----------------------------------------------------------------------------

namespace {

class SerialTransport : public Transport {
public:
    explicit SerialTransport(QSerialPort *p) : m_p(p) {}
    ~SerialTransport() override {
        m_p->close();
        delete m_p;
    }
    void write(const QByteArray &bytes) override {
        m_p->write(bytes);
        m_p->waitForBytesWritten(1000);
    }
    QByteArray read(int ms) override {
        if (m_p->bytesAvailable() == 0 && ms > 0) m_p->waitForReadyRead(ms);
        return m_p->readAll();
    }
    void setBaud(int baud) override { m_p->setBaudRate(baud); }
    int baud() const override { return m_p->baudRate(); }
    void discardInput() override {
        m_p->waitForReadyRead(1);
        m_p->clear(QSerialPort::Input);
        m_p->readAll();
    }

private:
    QSerialPort *m_p;
};

}  // namespace

std::unique_ptr<Transport> OpenSerial(const QString &portName, QString *error) {
    auto *p = new QSerialPort(portName);
    p->setBaudRate(Link::kBaud);
    p->setDataBits(QSerialPort::Data8);
    p->setParity(QSerialPort::NoParity);
    p->setStopBits(QSerialPort::OneStop);
    p->setFlowControl(QSerialPort::NoFlowControl);
    if (!p->open(QIODevice::ReadWrite)) {
        if (error) *error = p->errorString();
        delete p;
        return nullptr;
    }
    p->setDataTerminalReady(false);
    p->setRequestToSend(false);
    p->setBreakEnabled(false);
    return std::make_unique<SerialTransport>(p);
}

// ---- Simulated node ------------------------------------------------------------------------------

namespace {

class SimTransport : public Transport {
public:
    SimTransport() {
        m_clock.start();
        m_mon[0] = {3, 0x00206C3C, 0, 0};   // as our motor at connect (capture)
        m_mon[1] = {35, 0x00033F36, 0, 0};
    }
    void write(const QByteArray &bytes) override {
        for (char c : bytes) {
            const uint8_t b = static_cast<uint8_t>(c);
            if (b & 0x80) {
                m_in.clear();
                m_in.append(c);
            } else if (!m_in.isEmpty()) {
                m_in.append(c);
            }
            if (m_in.size() >= 2 && m_in.size() == 3 + (static_cast<uint8_t>(m_in[1]) & 0x1F)) {
                Handle(m_in);
                m_in.clear();
            }
        }
    }
    QByteArray read(int ms) override {
        Produce();
        if (m_out.isEmpty() && ms > 0) {
            const double wait = m_streaming ? std::min<double>(ms, std::max(0.0, (m_next - Now()) * 1000.0) + 0.5) : ms;
            QThread::usleep(static_cast<unsigned long>(wait * 1000.0));
            Produce();
        }
        QByteArray out = m_out;
        m_out.clear();
        return out;
    }
    void setBaud(int baud) override { m_baud = baud; }
    int baud() const override { return m_baud; }
    void discardInput() override { m_out.clear(); }

private:
    static constexpr double kPeriod = 1.0 / 845.0;
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kCycle = 2.0;  // seconds per machine cycle

    double Now() const { return m_clock.nsecsElapsed() / 1e9; }

    // The motion: rise, dwell, return, dwell over one cycle; cycloidal moves of 2 revolutions.
    struct Motion {
        double rpm, accel, posErr, torque, util, busV;
        bool moving;
    };
    static Motion At(double t) {
        const double u = std::fmod(t, kCycle) / kCycle;
        auto cyc = [](double x, double sign) {  // cycloidal velocity and acceleration shape over x in [0,1)
            const double v = (1.0 - std::cos(2 * kPi * x)) * sign;
            const double a = std::sin(2 * kPi * x) * sign;
            return std::pair<double, double>(v, a);
        };
        std::pair<double, double> va{0.0, 0.0};
        bool moving = false;
        if (u < 0.3) {
            va = cyc(u / 0.3, 1.0);
            moving = true;
        } else if (u >= 0.5 && u < 0.8) {
            va = cyc((u - 0.5) / 0.3, -1.0);
            moving = true;
        }
        Motion m;
        m.rpm = 300.0 * va.first;
        m.accel = va.second;
        m.posErr = 140.0 * va.second + 25.0 * va.first * (va.first > 0 ? 1 : -1) * 0.2;
        m.torque = 55.0 * va.second + 4.0 * va.first;
        m.util = std::fabs(m.torque) * 0.8;
        m.busV = 74.5 - 1.6 * std::fabs(va.second);
        m.moving = moving;
        return m;
    }

    int MonRaw(const MonState &s, const Motion &m, double t) const {
        const double noise = std::sin(t * 913.0) * 0.6 + std::sin(t * 2711.0) * 0.4;
        double raw = 0.0;
        switch (s.var & 0x0FFF) {
            case 27: raw = (m.posErr + 3.0 * noise) * s.gain / 32768.0; break;
            case 2: raw = (m.rpm + 2.0 * noise) * 8.136 * s.gain / 0x00206C3C; break;
            case 3: raw = m.rpm * 8.136 * s.gain / 0x00206C3C; break;
            case 35: raw = (m.torque + noise) * 16.0; break;
            case 34: raw = (m.torque * 0.15 + 2.0 * noise) * 16.0; break;
            case 8: raw = m.torque * 8.0; break;
            case 39: raw = m.util * 8.0; break;
            case 36: raw = 12197.0 * m.busV / 74.5; break;
            default: break;
        }
        return static_cast<int>(std::lround(std::clamp(raw, -32768.0, 32767.0)));
    }

    void Produce() {
        if (!m_streaming) return;
        const double now = Now();
        int n = 0;
        while (m_next <= now && n < 4000) {
            const Motion m = At(m_next);
            const int raw = MonRaw(m_mon[0], m, m_next);
            QByteArray b(7, '\0');
            b[0] = 1;
            b[1] = static_cast<char>(m_counter & 3);
            b[2] = static_cast<char>(raw & 0xFF);
            b[3] = static_cast<char>((raw >> 8) & 0xFF);
            b[4] = static_cast<char>(m.moving ? 0xC1 : 0x00);
            b[5] = b[4];
            // Digital lanes, both nibbles equal: bit 3 HLFB-like (toggles while moving), bit 0 enable.
            const int hlfb = m.moving ? (m_counter & 1) : 1;
            const int nib = (hlfb << 3) | 1;
            b[6] = static_cast<char>((nib << 4) | nib);
            m_out.append(Frame(3, 0, b, true));
            ++m_counter;
            m_next += kPeriod;
            ++n;
        }
        if (m_next < now - 0.5) m_next = now;  // the reader fell far behind: drop the backlog
    }

    void Reply(const QByteArray &bytes) { m_out.append(Frame(1, 0, bytes)); }

    QByteArray Param(int number) const {
        const double t = Now();
        const Motion m = At(t);
        auto le = [](int64_t v, int n) {
            QByteArray b(n, '\0');
            for (int i = 0; i < n; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
            return b;
        };
        switch (number) {
            case 0x000: return le(0x0B01, 2);
            case 0x001: return le(0x0217, 2);
            case 0x002: return le(0x0003, 2);
            case 0x004: return le(123456, 4);
            case 0x009: return le(0, 12);
            case 0x010: return le(0x401ce20808LL, 6);  // enabled at rest (capture)
            case 0x01A: return le(0, 12);
            case 0x053:
            case 0x054: return le(static_cast<int64_t>(std::fmod(t, kCycle) * 1000), 3);
            case 0x05A: return le(static_cast<int64_t>(m.posErr), 4);
            case 0x055:
            case 0x056: return le(static_cast<int64_t>(m.rpm * 16555.0), 4);
            case 0x123: return le(44 + static_cast<int>(t / 60.0) % 2, 2);
            case 0x116:
            case 0x118: return le(static_cast<int64_t>(m.busV * 32768.0 / 120.0), 2);
            case 0x140: return le(800, 4);
            case 0x110: return le(6400, 4);
            default: return le(0, 4);
        }
    }

    void Handle(const QByteArray &pkt) {
        int sum = 0;
        for (char c : pkt) sum += static_cast<uint8_t>(c);
        if (sum & 0x7F) return;
        const int type = (static_cast<uint8_t>(pkt[0]) >> 4) & 7;
        const QByteArray chars = pkt.mid(2, static_cast<uint8_t>(pkt[1]) & 0x1F);
        const QByteArray b = Unpack(chars, chars.size() * 7 / 8);
        if (type == 5) {  // set address: one node on the link
            m_out.append(Frame(5, 1, QByteArray()));
            return;
        }
        if (type == 3 || type == 7) {  // link reset / link rate: echo
            m_out.append(Frame(type, 0, b, false));
            return;
        }
        if (type != 0 || b.isEmpty()) return;
        const int cmd = static_cast<uint8_t>(b[0]);
        switch (cmd) {
            case 0: case 2: case 11: case 13: {
                const int bank = cmd == 0 ? 0 : cmd == 2 ? 1 : cmd == 11 ? 2 : 3;
                Reply(Param(bank << 8 | (b.size() > 1 ? static_cast<uint8_t>(b[1]) : 0)));
                return;
            }
            case 0x16: case 0x1E: {
                MonState &s = m_mon[cmd == 0x16 ? 0 : 1];
                if (b.size() == 1) {
                    QByteArray r(10, '\0');
                    auto put = [&](int at, int64_t v, int n) {
                        for (int i = 0; i < n; ++i) r[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
                    };
                    put(0, s.var, 2);
                    put(2, s.gain, 4);
                    put(6, s.filter, 2);
                    put(8, s.sync, 2);
                    Reply(r);
                } else if (b.size() >= 10) {
                    auto get = [&](int at, int n) {
                        int64_t v = 0;
                        for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(b[at + i]);
                        return v;
                    };
                    s.var = static_cast<int>(get(1, 2));
                    s.gain = static_cast<int>(get(3, 4));
                    s.filter = static_cast<int16_t>(get(7, 2));
                    s.sync = static_cast<int>(get(9, 1));
                    Reply(QByteArray());
                } else {
                    Reply(QByteArray(1, '\0'));  // refused
                }
                return;
            }
            case 0x19:
                m_streaming = b.size() > 1 && b[1] != 0;
                m_next = Now();
                Reply(QByteArray());
                return;
            case 5: Reply(QByteArray(1, 72)); return;
            default: Reply(QByteArray()); return;
        }
    }

    QElapsedTimer m_clock;
    QByteArray m_in, m_out;
    int m_baud = Link::kBaud;
    MonState m_mon[2];
    bool m_streaming = false;
    double m_next = 0.0;
    int m_counter = 0;
};

}  // namespace

std::unique_ptr<Transport> OpenSimulated() { return std::make_unique<SimTransport>(); }

// ---- Link ----------------------------------------------------------------------------------------

namespace {
constexpr int kPktCmd = 0, kPktExtLow = 3, kPktSetAddr = 5, kPktExtHigh = 7;
int GetCmd(int bank) { static const int c[] = {0, 2, 11, 13}; return c[bank & 3]; }
}  // namespace

Link::Link(std::unique_ptr<Transport> t) : m_t(std::move(t)) {
    m_clock.start();
    QThread::msleep(200);
    bool answered = false;
    for (int baud : {kBaud, kStreamBaud}) {  // a previous session may have left the node fast
        m_t->setBaud(baud);
        m_t->discardInput();
        // Link reset pair, as ClearPath-MSP sends at connect. Never a break: it latches EStopped.
        m_t->write(Frame(kPktExtLow, 0, QByteArray(1, '\0')));
        m_t->write(Frame(kPktExtHigh, 0, QByteArray(1, '\0')));
        QThread::msleep(100);
        m_t->discardInput();
        m_rx.clear();
        m_replies.clear();
        m_t->write(Frame(kPktSetAddr, 0, QByteArray()));
        Packet p;
        if (ReadReply(1000, &p)) {
            m_nodes = static_cast<uint8_t>(p.raw[0]) & 0xF;
            answered = true;
            break;
        }
    }
    if (!answered) throw std::runtime_error("No reply to set-address. Is ClearPath-MSP holding the port?");
    if (m_t->baud() != kBaud) setRate(kBaud);
}

Link::~Link() {
    try {
        if (m_t->baud() != kBaud) setRate(kBaud);
    } catch (...) {
    }
}

void Link::pump(int waitMs) {
    const QByteArray in = m_t->read(waitMs);
    const double t = now();
    for (char c : in) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b & 0x80) {
            m_rx.clear();
            m_rx.append(c);
        } else if (!m_rx.isEmpty()) {
            m_rx.append(c);
        } else {
            continue;
        }
        if (m_rx.size() >= 2 && m_rx.size() == 3 + (static_cast<uint8_t>(m_rx[1]) & 0x1F)) {
            Packet p{m_rx, t};
            m_rx.clear();
            int sum = 0;
            for (char x : p.raw) sum += static_cast<uint8_t>(x);
            if (sum & 0x7F) {
                ++m_bad;
                continue;
            }
            if (p.isStream()) {
                const QByteArray ch = p.chars();
                if (ch.size() != 8) continue;  // not a scope sample (a reset echo)
                const QByteArray s = Unpack(ch, 7);
                StreamSample smp;
                smp.t = t;
                smp.counter = static_cast<uint8_t>(s[1]) & 3;
                smp.raw = static_cast<int16_t>(static_cast<uint8_t>(s[2]) | (static_cast<uint8_t>(s[3]) << 8));
                smp.digital = static_cast<uint8_t>(s[6]);
                m_stream.push_back(smp);
                if (m_stream.size() > 200000) m_stream.erase(m_stream.begin(), m_stream.begin() + 100000);
            } else {
                m_replies.push_back(p);
            }
        }
    }
}

Packet *Link::ReadReply(int ms, Packet *out) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (!m_replies.empty()) {
            *out = m_replies.front();
            m_replies.pop_front();
            return out;
        }
        pump(5);
    }
    if (!m_replies.empty()) {
        *out = m_replies.front();
        m_replies.pop_front();
        return out;
    }
    return nullptr;
}

QByteArray Link::command(const QByteArray &payload, int type, int ms) {
    // Replies carry no command number: one command in flight at a time, and nothing left over from
    // an earlier one.
    m_replies.clear();
    m_t->write(Frame(type, 0, payload));
    Packet p;
    if (!ReadReply(ms, &p)) {
        // A late reply must not answer the next command.
        pump(30);
        m_replies.clear();
        throw std::runtime_error("No reply to command " + payload.toHex().toStdString());
    }
    return p.chars();
}

void Link::sendNoWait(const QByteArray &payload) {
    m_t->write(Frame(kPktCmd, 0, payload));
    m_replies.clear();
}

QByteArray Link::getChars(const ParamDef &p) {
    QByteArray b(2, '\0');
    b[0] = static_cast<char>(GetCmd(p.number >> 8));
    b[1] = static_cast<char>(p.number & 0xFF);
    return command(b);
}

int64_t Link::get(const ParamDef &p) { return Value(getChars(p), p.size, p.isSigned); }

void Link::setRate(int baud) {
    QByteArray b(2, '\0');
    b[0] = 9;  // MN_CTL_EXT_BAUD_RATE
    b[1] = static_cast<char>(baud / 9600);
    command(b, kPktExtHigh);
    m_t->setBaud(baud);
    QThread::msleep(50);
    m_t->discardInput();
    m_rx.clear();
    m_replies.clear();
}

MonState Link::monitorGet(int ch) {
    const QByteArray b = Unpack(command(QByteArray(1, static_cast<char>(ch == 1 ? 0x16 : 0x1E))), 10);
    auto u = [&](int at, int n) {
        int64_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(b[at + i]);
        return v;
    };
    MonState s;
    s.var = static_cast<int>(u(0, 2));
    s.gain = static_cast<int32_t>(u(2, 4));
    s.filter = static_cast<int16_t>(u(6, 2));
    s.sync = static_cast<int>(u(8, 2));
    return s;
}

void Link::monitorSet(int ch, const MonState &s) {
    if (s.var & kMonSaveNv) throw std::runtime_error("Refusing to save the monitor setting to the motor.");
    QByteArray p(10, '\0');
    auto put = [&](int at, int64_t v, int n) {
        for (int i = 0; i < n; ++i) p[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    p[0] = static_cast<char>(ch == 1 ? 0x16 : 0x1E);
    put(1, s.var, 2);
    put(3, s.gain, 4);
    put(7, s.filter, 2);
    put(9, s.sync & 0xFF, 1);  // the SDK's packet length cuts tune-sync to one byte
    const QByteArray reply = command(p);
    if (!reply.isEmpty()) throw std::runtime_error("Monitor write refused (reply " + reply.toHex().toStdString() + ")");
}

void Link::scope(int mode) {
    QByteArray b(2, '\0');
    b[0] = 0x19;
    b[1] = static_cast<char>(mode);
    command(b);
}

// Awaited like any command: its reply (always 72) would otherwise answer the next register read.
void Link::keepalive() { command(QByteArray(1, 5)); }

void Link::clearEstop() {
    QByteArray b(2, '\0');
    b[0] = 4;
    b[1] = 0x18;  // netNodeStop STOP_TYPE_CLR_ESTOP
    command(b);
}

std::vector<StreamSample> Link::takeStream() {
    std::vector<StreamSample> out;
    out.swap(m_stream);
    return out;
}

// ---- Registers and monitor variables -------------------------------------------------------------

namespace params {
const ParamDef DeviceId{"Device ID", 0x000, 2, false};
const ParamDef FirmwareVersion{"Firmware version", 0x001, 2, false};
const ParamDef SerialNumber{"Serial number", 0x004, 4, false};
const ParamDef AlertRegister{"Alert register", 0x009, 12, false};
const ParamDef StatusRealtime{"Status (real-time)", 0x010, 6, false};
const ParamDef PositionMeasured{"Position, measured", 0x053, 3, true};
const ParamDef PositionCommanded{"Position, commanded", 0x054, 3, true};
const ParamDef PositionError{"Position error", 0x05A, 4, true};
const ParamDef VelocityA{"Velocity A", 0x055, 4, true};
const ParamDef DriveTemperature{"Drive temperature", 0x123, 2, true};
const ParamDef BusVoltage{"Bus voltage (candidate)", 0x118, 2, false};
const ParamDef InputResolution{"Input resolution", 0x140, 4, false};
}  // namespace params

const std::vector<MonVar> &MonVars() {
    static const std::vector<MonVar> v = {
        {"Position error (load tracking)", 27, 0x00800000},
        {"Velocity, commanded", 3, 0x00206C3C},
        {"Velocity, measured", 2, 0x00206C3C},
        {"Torque, measured (peak)", 35, 0x00033F36},
        {"Torque error (peak)", 34, 0x00040F04},
        {"Torque, commanded", 8, 0x00008000},
        {"Torque utilization", 39, 0x00008000},
        {"Bus voltage", 36, 0x00009999},
    };
    return v;
}

const MonVar *FindMonVar(int id) {
    for (const MonVar &m : MonVars()) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

double MonScale(const MonVar &v, int raw) {
    // Public SDK convertMonGain for position tracking: value = raw / 65536 x full scale, which comes to
    // raw x 32768 / gain encoder counts. Velocity: 8.136 per RPM at MSP's default gain (capture).
    if (v.id == 27) return raw * 32768.0 / v.gain;
    if (v.id == 2 || v.id == 3) return raw / (8.136 * v.gain / 0x00206C3C);
    return raw;
}

const char *MonUnit(const MonVar &v) {
    if (v.id == 27) return "counts";
    if (v.id == 2 || v.id == 3) return "RPM";
    return "raw";
}

QStringList AlertNames(const QByteArray &chars) {
    struct F {
        int bit;
        const char *name;
    };
    static const F kFields[] = {
        {0, "FwSelfTest"}, {1, "FwNetBufferOverrun"}, {2, "FwError0"}, {3, "FwError1"}, {4, "FwStackOverrun"},
        {5, "FwWatchdogRestarted"}, {6, "FwInvalidConfiguration"}, {11, "HwPowerProblem"}, {12, "HwClockProblem"},
        {13, "HwEEPROMdead"}, {14, "HwFlashCorrupt"}, {15, "HwFlashChanged"}, {16, "HwRAM"}, {17, "HwADC"},
        {18, "HwADCsat"}, {24, "NetVoltageLow"}, {25, "NetWatchdog"}, {28, "EStopped"}, {29, "ConfigOutOfDate"},
        {30, "RunTimeErr"}, {36, "JrkLimRequestBad"}, {37, "MoveBufUnderrun"}, {38, "JrkLimVelRequestBad"},
        {39, "MoveSpecAltered"}, {40, "PhaseSensorFailed"}, {41, "LimSwitchActivated"}, {42, "SoftLimitExceeded"},
        {45, "Mv2SwMaxIndexWarn"}, {52, "ACPowerLost"}, {53, "ACPhaseLost"}, {55, "MtrTempLow"}, {57, "MLError"},
        {59, "MtrVectorBad"}, {61, "MtrEncGlitch"}, {62, "MtrEncOverspeed"}, {65, "MtrPhaseOverload"},
        {67, "MtrBadSetupParams"}, {68, "HardStopBrokeFree"}, {70, "TrackingShutdown"}, {71, "RMSOverload"},
        {72, "RMSOverloadShutdown"}, {75, "BusVoltSat"}, {76, "TrqSat"}, {77, "NoCommSweepFailed"},
        {78, "NoCommSweepReversed"}, {79, "NoCommFailed"}, {80, "IndexCountZeroWarn"}, {81, "DriveTempHigh"},
        {82, "StatorTempHigh"}, {83, "BusOverCurrent"}, {84, "BusOverVoltage"}, {85, "BusVoltageLow"},
        {86, "BusRMSOverload"}, {88, "MtrEncIndexMissing"}, {89, "BusVoltageUnderOperatingV"},
        {94, "MtrEncIndexMisplaced"}, {95, "StepsDuringPosnRecovery"},
    };
    const QByteArray b = Unpack(chars, 12);
    QStringList out;
    for (const F &f : kFields) {
        if ((static_cast<uint8_t>(b[f.bit / 8]) >> (f.bit % 8)) & 1) out << QString::fromLatin1(f.name);
    }
    // Set bits with no name.
    for (int bit = 0; bit < 96; ++bit) {
        if (!((static_cast<uint8_t>(b[bit / 8]) >> (bit % 8)) & 1)) continue;
        bool named = false;
        for (const F &f : kFields) named = named || f.bit == bit;
        if (!named) out << QStringLiteral("bit %1").arg(bit);
    }
    return out;
}

double StreamClock::place(double arrival, int counter) {
    if (!m_have || std::fabs(m_t - arrival) > 0.08) {  // the start, or a stall
        if (m_have) ++m_anchors;
        m_t = arrival;
        m_have = true;
    } else {
        int step = (counter - m_counter) & 3;
        if (step == 0) step = 4;
        m_lost += step - 1;
        m_t += step * kPeriod;
        m_t += (arrival - m_t) * 0.002;  // well under a period, so samples stay in order
    }
    m_counter = counter;
    return m_t;
}

QStringList FindPorts() {
    QStringList out;
    for (const QSerialPortInfo &i : QSerialPortInfo::availablePorts()) {
        if (i.hasVendorIdentifier() && i.vendorIdentifier() == 0x2890 && i.hasProductIdentifier() &&
            i.productIdentifier() == 0x0201) {
            out << i.portName();
        }
    }
    QCollator order;
    order.setNumericMode(true);  // COM9 before COM10
    std::sort(out.begin(), out.end(), order);
    return out;
}

}  // namespace clearpath
