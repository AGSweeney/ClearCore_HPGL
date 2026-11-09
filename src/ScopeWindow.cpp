/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include "ScopeWindow.h"

#include "Theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QAction>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>

using namespace clearpath;

// =====================================================================================================
// Catalog
// =====================================================================================================

namespace scope {

const std::vector<SignalDef> &Catalog() {
    static const std::vector<SignalDef> c = [] {
        std::vector<SignalDef> v;
        for (const MonVar &m : MonVars()) {
            SignalDef d;
            d.key = QStringLiteral("mon%1").arg(m.id);
            d.name = QString::fromLatin1(m.name);
            d.unit = QString::fromLatin1(MonUnit(m));
            d.source = Source::Stream;
            d.monId = m.id;
            d.note = (m.id == 27 || m.id == 2 || m.id == 3)
                         ? QStringLiteral("Streamed, about 845 samples/s (monitor variable %1). Scaled with the SDK's "
                                          "monitor gain; checked against MSP.")
                               .arg(m.id)
                         : QStringLiteral("Streamed, about 845 samples/s (monitor variable %1). Raw monitor units: "
                                          "only position error and velocity are scaled so far.")
                               .arg(m.id);
            v.push_back(d);
        }
        auto reg = [&](const char *key, const char *name, const char *unit, const ParamDef *p, double scale,
                       const char *note) {
            SignalDef d;
            d.key = QString::fromLatin1(key);
            d.name = QString::fromUtf8(name);
            d.unit = QString::fromUtf8(unit);
            d.source = Source::Register;
            d.param = p;
            d.scale = scale;
            d.note = QString::fromUtf8(note);
            v.push_back(d);
        };
        reg("reg.velocity", "Velocity (register 0x055)", "RPM", &params::VelocityA, 1.0 / 16555.0,
            "Read between stream samples. About 16,555 per RPM (capture). One of the two velocity registers; "
            "the captures do not say whether it is measured or commanded.");
        reg("reg.poserr", "Position error (register 0x05A)", "counts", &params::PositionError, 1.0,
            "Read between stream samples. Named in the public SDK; not yet checked against MSP.");
        reg("reg.posmeas", "Position, measured (0x053)", "counts", &params::PositionMeasured, 1.0,
            "Read between stream samples. Confirmed against MSP (capture).");
        reg("reg.bus", "Bus voltage (0x118, candidate)", "V", &params::BusVoltage, 120.0 / 32768.0,
            "Read between stream samples. A candidate: 120 V full scale fits one reading against MSP.");
        reg("reg.temp", "Drive temperature (0x123)", "°C", &params::DriveTemperature, 1.0,
            "Read between stream samples. Confirmed against MSP (capture).");
        auto dig = [&](const char *key, const char *name, int bit, const ParamDef *p, const char *note) {
            SignalDef d;
            d.key = QString::fromLatin1(key);
            d.name = QString::fromUtf8(name);
            d.source = Source::Digital;
            d.bit = bit;
            d.param = p;
            d.note = QString::fromUtf8(note);
            v.push_back(d);
        };
        dig("dig.hlfb", "HLFB (stream)", 3, nullptr,
            "Byte 6 bit 3 of every stream sample: high enabled at rest, toggling while moving, like the HLFB "
            "output in MSP. Provisional.");
        dig("dig.enable", "Enable (stream)", 0, nullptr,
            "Byte 6 bit 0 of every stream sample: 0 only while disabled, like the enable input. Provisional.");
        dig("dig.alert", "Alert present", -1, &params::AlertRegister,
            "The alert register (0x009) is not zero. Read about twice a second. Bit names follow the "
            "ClearPath-SC layout and are provisional on MC/SD.");
        return v;
    }();
    return c;
}

int MotorOf(const QString &key) {
    if (key.size() > 3 && key[0] == QLatin1Char('m') && key[1].isDigit() && key[2] == QLatin1Char('/')) {
        return key[1].digitValue();
    }
    return 0;
}

QString BaseKey(const QString &key) { return MotorOf(key) ? key.mid(3) : key; }

QString MotorKey(int motor, const QString &key) {
    return motor ? QStringLiteral("m%1/%2").arg(motor).arg(key) : key;
}

const SignalDef *Find(const QString &key) {
    const QString base = BaseKey(key);
    for (const SignalDef &d : Catalog()) {
        if (d.key == base) return &d;
    }
    return nullptr;
}

// A lane's name: the signal's, with its motor's in front once there is more than one.
static QString LaneTitle(const QString &key, const std::vector<QString> &labels) {
    const SignalDef *d = Find(key);
    if (!d) return key;
    if (labels.size() < 2) return d->name;
    const size_t m = static_cast<size_t>(MotorOf(key));
    return (m < labels.size() ? labels[m] : QStringLiteral("Motor %1").arg(m + 1)) + QStringLiteral(" · ") + d->name;
}

std::vector<Lane> DefaultLanes() {
    // The critical concerns first: tracking, speed, supply, heat, and whether the drive is happy.
    std::vector<Lane> out = {
        {QStringLiteral("mon27"), true, theme::kDisplacement},
        {QStringLiteral("reg.velocity"), true, theme::kVelocity},
        {QStringLiteral("reg.bus"), true, theme::kAccel},
        {QStringLiteral("reg.temp"), true, theme::kJerk},
        {QStringLiteral("dig.hlfb"), true, QColor(0x80, 0xcb, 0xc4)},
        {QStringLiteral("dig.enable"), true, QColor(0xb8, 0xc4, 0xd6)},
        {QStringLiteral("dig.alert"), true, theme::kWarn},
    };
    // Everything else is listed, off, to switch on.
    const QColor spare[] = {QColor(0xf4, 0x8f, 0xb1), QColor(0xd4, 0xe1, 0x57), QColor(0x4d, 0xab, 0xf7),
                            QColor(0xff, 0x92, 0x2b), QColor(0x94, 0xd8, 0x2d)};
    int i = 0;
    for (const SignalDef &d : Catalog()) {
        bool have = false;
        for (const Lane &l : out) have = have || l.key == d.key;
        if (!have) out.push_back({d.key, false, spare[i++ % 5]});
    }
    return out;
}

void Series::trim(double before) {
    const auto it = std::lower_bound(t.begin(), t.end(), before);
    const auto n = it - t.begin();
    if (n > 4096 || (n > 0 && n * 4 > static_cast<long>(t.size()))) {
        t.erase(t.begin(), it);
        v.erase(v.begin(), v.begin() + n);
    }
}

}  // namespace scope

using namespace scope;

// =====================================================================================================
// Worker: owns the link, on its own thread
// =====================================================================================================

class ScopeWorker : public QObject {
    Q_OBJECT
public:
    ScopeWorker(int motor, const QElapsedTimer *clock) : m_motor(motor), m_shared(clock) {}

public slots:
    void open(const QString &port) {
        close();
        try {
            std::unique_ptr<Transport> t;
            if (port == QLatin1String(kSimulatedPort)) {
                t = OpenSimulated();
            } else {
                QString err;
                t = OpenSerial(port, &err);
                if (!t) throw std::runtime_error(("Could not open " + port + ": " + err).toStdString());
            }
            m_link = std::make_unique<Link>(std::move(t));
            const int64_t id = m_link->get(params::DeviceId);
            const int64_t fw = m_link->get(params::FirmwareVersion);
            const int64_t sn = m_link->get(params::SerialNumber);
            const int64_t res = m_link->get(params::InputResolution);
            const QString info = QStringLiteral("%1 · %2 0x%3 · firmware 0x%4 · serial %5 · %6 steps/rev")
                                     .arg(port, (id >> 8) == 11 ? QStringLiteral("ClearPath MC/SD") : QStringLiteral("device"))
                                     .arg(id, 4, 16, QChar('0'))
                                     .arg(fw, 4, 16, QChar('0'))
                                     .arg(sn)
                                     .arg(res);
            if (!m_timer) {
                m_timer = new QTimer(this);
                connect(m_timer, &QTimer::timeout, this, &ScopeWorker::Tick);
            }
            m_timer->start(100);
            emit opened(info, QString::number(sn));
        } catch (const std::exception &e) {
            m_link.reset();
            emit failed(QString::fromStdString(e.what()));
        }
    }

    void close() {
        if (m_recording) stop();
        if (m_timer) m_timer->stop();
        if (m_link) {
            m_link.reset();  // back to 9600 baud
            emit closed();
        }
    }

    void start(int monId, const QStringList &registers) {
        if (!m_link) return;
        if (m_recording) stop();
        const MonVar *mv = FindMonVar(monId);
        if (!mv) mv = FindMonVar(27);
        m_registers = registers;
        try {
            m_link->setRate(Link::kStreamBaud);
            try {
                m_orig = m_link->monitorGet(1);
                m_link->monitorSet(1, MonState{mv->id, mv->gain, 0, m_orig.sync});
                if ((m_link->monitorGet(1).var & 0x0FFF) != mv->id) throw std::runtime_error("Channel 1 did not take the variable.");
                m_link->takeStream();
                m_link->scope(3);
            } catch (...) {
                try {
                    m_link->setRate(Link::kBaud);
                } catch (...) {
                }
                throw;
            }
        } catch (const std::exception &e) {
            emit failed(QStringLiteral("Start: %1").arg(QString::fromStdString(e.what())));
            return;
        }
        m_mon = mv->id;
        m_recording = true;
        m_off = m_shared->elapsed() / 1000.0 - m_link->now();  // this link's clock onto the shared one
        m_clock.reset();
        m_lastKeep = m_lastReg = m_lastAlert = m_lastEmit = m_link->now();
        m_regIndex = 0;
        m_timer->start(0);
        emit started(mv->id, QString::fromLatin1(FindMonVar(m_orig.var & 0x0FFF) ? FindMonVar(m_orig.var & 0x0FFF)->name
                                                                                  : "another variable"));
    }

    void setRegisters(const QStringList &registers) { m_registers = registers; }

    void stop() {
        if (!m_link || !m_recording) return;
        QStringList problems;
        auto attempt = [&](auto fn) {
            try {
                fn();
            } catch (const std::exception &e) {
                problems << QString::fromStdString(e.what());
            }
        };
        attempt([&] { m_link->scope(0); });
        attempt([&] { m_link->scope(0); });
        attempt([&] {
            MonState back = m_orig;
            back.var &= ~Link::kMonSaveNv;
            m_link->monitorSet(1, back);
        });
        attempt([&] {
            if (!(m_link->monitorGet(1) == m_orig)) problems << QStringLiteral("channel 1 restore mismatch");
        });
        attempt([&] { m_link->setRate(Link::kBaud); });
        Flush(true);
        m_recording = false;
        m_timer->start(100);
        emit stopped(problems.isEmpty() ? QStringLiteral("Stopped: channel 1 restored, link back at 9600 baud.")
                                        : QStringLiteral("Stopped with problems: ") + problems.join(QStringLiteral("; ")));
    }

    void clearEstop() {
        if (!m_link) return;
        try {
            m_link->clearEstop();
            emit message(QStringLiteral("Sent clear E-stop."));
        } catch (const std::exception &e) {
            emit failed(QString::fromStdString(e.what()));
        }
    }

signals:
    void opened(const QString &info, const QString &serial);
    void closed();
    void failed(const QString &message);
    void message(const QString &message);
    void started(int monId, const QString &wasOnChannel1);
    void stopped(const QString &message);
    void batch(const scope::Batch &b);
    void health(const QString &alerts, bool alertActive, bool enabled);

private:
    void Tick() {
        if (!m_link) return;
        try {
            const double now = m_link->now();
            if (m_recording) {
                m_link->pump(3);
                TakeStream();
                if (now - m_lastKeep > 0.3) {
                    m_link->keepalive();
                    m_lastKeep = now;
                }
                if (!m_registers.isEmpty() && now - m_lastReg >= 0.02) {
                    m_lastReg = now;
                    const QString key = m_registers[m_regIndex++ % m_registers.size()];
                    if (const SignalDef *d = Find(key); d && d->param) {
                        const double v = static_cast<double>(m_link->get(*d->param)) * d->scale;
                        m_out.regKey.push_back(key);
                        m_out.regT.push_back(m_link->now() + m_off);
                        m_out.regV.push_back(v);
                    }
                    TakeStream();
                }
                if (now - m_lastAlert >= 0.5) Health();
                if (now - m_lastEmit >= 0.033) Flush(false);
            } else if (now - m_lastAlert >= 0.3) {
                Health();
            }
            m_errors = 0;
        } catch (const std::exception &e) {
            emit failed(QString::fromStdString(e.what()));
            if (++m_errors >= 5) {
                emit failed(QStringLiteral("The motor stopped answering; disconnected."));
                m_recording = false;
                m_timer->stop();
                m_link.reset();
                emit closed();
            }
        }
    }

    void Health() {
        m_lastAlert = m_link->now();
        const QByteArray a = m_link->getChars(params::AlertRegister);
        const QStringList names = AlertNames(a);
        bool enabled = true;
        try {
            const QByteArray s = Unpack(m_link->getChars(params::StatusRealtime), 6);
            enabled = !((static_cast<uint8_t>(s[3]) >> 6) & 1);  // bit 30 set while disabled (capture)
        } catch (...) {
        }
        if (m_recording) {
            m_out.regKey.push_back(QStringLiteral("dig.alert"));
            m_out.regT.push_back(m_link->now() + m_off);
            m_out.regV.push_back(names.isEmpty() ? 0.0 : 1.0);
        }
        emit health(names.join(QStringLiteral(" ")), !names.isEmpty(), enabled);
    }

    void TakeStream() {
        for (StreamSample s : m_link->takeStream()) {
            s.t = m_clock.place(s.t + m_off, s.counter);
            m_out.stream.push_back(s);
        }
    }

    void Flush(bool force) {
        if (!force && m_out.stream.empty() && m_out.regKey.empty()) return;
        m_out.monId = m_mon;
        m_out.motor = m_motor;
        emit batch(m_out);
        m_out = Batch();
        m_lastEmit = m_link ? m_link->now() : 0.0;
    }

    std::unique_ptr<Link> m_link;
    QTimer *m_timer = nullptr;
    bool m_recording = false;
    MonState m_orig;
    int m_mon = 27;
    QStringList m_registers;
    int m_regIndex = 0, m_errors = 0;
    double m_lastKeep = 0, m_lastReg = 0, m_lastAlert = 0, m_lastEmit = 0, m_off = 0;
    int m_motor = 0;
    const QElapsedTimer *m_shared = nullptr;
    StreamClock m_clock;
    Batch m_out;
};

// =====================================================================================================
// Plot: stacked lanes on one time axis
// =====================================================================================================

class ScopePlot : public QWidget {
public:
    explicit ScopePlot(QWidget *parent) : QWidget(parent) {
        setMouseTracking(true);
        setMinimumSize(480, 260);
    }
    void setSource(const std::vector<Lane> *lanes, const std::map<QString, Series> *data,
                   const std::vector<QString> *labels) {
        m_lanes = lanes;
        m_data = data;
        m_labels = labels;
    }
    // The timebase: seconds across the plot.
    static constexpr double kMinWindow = 0.01, kMaxWindow = 120.0;
    void setWindow(double s) {
        m_window = std::clamp(s, kMinWindow, kMaxWindow);
        ClampHold();
        update();
    }
    double window() const { return m_window; }
    // What the right edge shows, in order: frozen (held where it was), a trigger capture, stopped (held),
    // live (the newest data). A held view keeps its right edge where it is, and zoom and pan move it.
    void setHold(bool stopped, bool frozen, bool frozenWhileRecording) {
        const bool wasHeld = held();
        const double before = viewEnd();
        m_hold = stopped || frozen;
        m_frozenManual = frozen;
        m_frozenLabel = frozenWhileRecording;
        if (held() && !wasHeld) m_holdEnd = before;
        update();
    }
    bool held() const { return m_frozenManual || (m_hold && !m_trigView); }
    double viewEnd() const {
        if (held()) return m_holdEnd;
        if (m_trigView) return m_trigT + (1.0 - m_trigPre) * m_window;
        return m_end;
    }
    // The newest sample time. Repaints only when it moved and the view follows it.
    void setEnd(double t) {
        if (t == m_end) return;
        m_end = t;
        if (!held() && !m_trigView) update();
    }
    // Trigger: while `view`, the plot shows the capture with its trigger time `trigT` at `pre` (0..1)
    // across. Zoom keeps the trigger point where it is.
    void setTrigger(bool view, double trigT, double pre) {
        if (view == m_trigView && trigT == m_trigT && pre == m_trigPre) return;
        const bool wasHeld = held();
        const double before = viewEnd();
        m_trigView = view;
        m_trigT = trigT;
        m_trigPre = pre;
        if (held() && !wasHeld) m_holdEnd = before;
        update();
    }
    // The trigger's marks: its level in the source lane, and a line at the last trigger time.
    void setTriggerMarks(bool show, const QString &source, double level, double markT) {
        if (show == m_markShow && source == m_markSource && level == m_markLevel &&
            (markT == m_markT || (std::isnan(markT) && std::isnan(m_markT)))) {
            return;
        }
        m_markShow = show;
        m_markSource = source;
        m_markLevel = level;
        m_markT = markT;
        update();
    }
    int frames() const { return m_frames; }

    std::function<void(double)> onWindowChanged;  // zoom changed the timebase
    std::function<void()> onPanWhileLive;         // a pan began on a live view: hold it (Freeze)

    // Zoom by `factor` (> 1 in): a held view keeps the time under `anchorX` (the plot's x, -1 for the
    // middle) where it is; a live view keeps its right edge on the newest data.
    void zoomBy(double factor, double anchorX = -1.0) {
        const double win = std::clamp(m_window / factor, kMinWindow, kMaxWindow);
        if (win == m_window) return;
        if (held()) {
            const double left = kGutter, w = std::max(1.0, width() - kGutter - kRight);
            const double frac = anchorX >= left && anchorX <= left + w ? (anchorX - left) / w : 0.5;
            const double anchor = m_holdEnd - m_window + frac * m_window;
            m_holdEnd = anchor + (1.0 - frac) * win;
        }
        m_window = win;
        ClampHold();
        if (onWindowChanged) onWindowChanged(m_window);
        update();
    }
    // Everything recorded (up to the timebase limit), right edge on the newest sample.
    void fitAll() {
        double lo, hi;
        if (!DataRange(&lo, &hi)) return;
        m_window = std::clamp((hi - lo) * 1.02, kMinWindow, kMaxWindow);
        if (held()) m_holdEnd = hi + (hi - lo) * 0.01;
        ClampHold();
        if (onWindowChanged) onWindowChanged(m_window);
        update();
    }
    void panBy(double seconds) {
        if (!held() && onPanWhileLive) onPanWhileLive();
        if (!held()) return;
        m_holdEnd += seconds;
        ClampHold();
        update();
    }
    void zoomAtHover(double factor) { zoomBy(factor, m_hoverX); }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::MiddleButton) return QWidget::mousePressEvent(e);
        if (!held() && onPanWhileLive) onPanWhileLive();
        if (!held()) return;
        m_panning = true;
        m_panX = e->position().x();
        m_panEnd0 = m_holdEnd;
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            unsetCursor();
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        m_hoverX = e->position().x();
        if (m_panning) {
            const double w = std::max(1.0, width() - kGutter - kRight);
            m_holdEnd = m_panEnd0 - (e->position().x() - m_panX) / w * m_window;
            ClampHold();
        }
        update();
    }
    void leaveEvent(QEvent *) override {
        m_hoverX = -1;
        update();
    }
    void wheelEvent(QWheelEvent *e) override {
        const double steps = (e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x()) / 120.0;
        if (e->modifiers() & Qt::ControlModifier) {
            zoomBy(std::pow(1.25, steps), e->position().x());
        } else if ((e->modifiers() & Qt::ShiftModifier) || e->angleDelta().x() != 0) {
            panBy(-steps * 0.1 * m_window);
        } else {
            return QWidget::wheelEvent(e);
        }
        e->accept();
    }
    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
            case Qt::Key_Plus:
            case Qt::Key_Equal: zoomAtHover(1.25); return;
            case Qt::Key_Minus:
            case Qt::Key_Underscore: zoomAtHover(1.0 / 1.25); return;
            case Qt::Key_0: fitAll(); return;
            default: QWidget::keyPressEvent(e);
        }
    }

public:
    double paintMs() const { return m_paintMs; }
    double maxPaintMs() const { return m_maxPaintMs; }
    void resetPaintStats() { m_maxPaintMs = 0; }

protected:

    void paintEvent(QPaintEvent *) override {
        QElapsedTimer timer;
        timer.start();
        Paint();
        ++m_frames;
        m_paintMs = timer.nsecsElapsed() / 1e6;
        m_maxPaintMs = std::max(m_maxPaintMs, m_paintMs);
    }

    void Paint() {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(0x25, 0x29, 0x2f));
        if (!m_lanes || !m_data) return;
        // Lanes on screen. Each group is one lane: an analog signal marked Overlay joins the lane above
        // when that lane is analog too.
        std::vector<std::vector<const Lane *>> groups;
        for (const Lane &l : *m_lanes) {
            const SignalDef *d = l.on ? Find(l.key) : nullptr;
            if (!d) continue;
            const bool digital = d->source == Source::Digital;
            if (l.overlay && !digital && !groups.empty() && Find(groups.back().front()->key)->source != Source::Digital) {
                groups.back().push_back(&l);
            } else {
                groups.push_back({&l});
            }
        }
        constexpr double kTop = 6, kAxis = 24, kGap = 4;
        const QRectF all(kGutter, kTop, width() - kGutter - kRight, height() - kTop - kAxis);
        if (groups.empty() || all.width() < 20 || all.height() < 20) {
            p.setPen(theme::kAxisText);
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No lanes on. Choose some in Lanes."));
            return;
        }
        auto weightOf = [](const std::vector<const Lane *> &g) {
            return Find(g.front()->key)->source == Source::Digital ? 1.0 : 3.2 + 0.9 * (g.size() - 1);
        };
        double weights = 0;
        for (const auto &g : groups) weights += weightOf(g);
        const double unit = (all.height() - kGap * (groups.size() - 1)) / weights;
        const double end = viewEnd();
        // Times read back from the newest sample ("now"), wherever the view has been panned to.
        // Times on the axis: from the trigger while a capture is shown (0 at the trigger, as a scope reads),
        // otherwise back from the newest sample ("now"), wherever the view has been panned to.
        const bool atTrigger = m_trigView && !held();
        const double ref = atTrigger ? m_trigT : m_end;
        const double t0 = end - m_window;
        auto X = [&](double t) { return all.left() + (t - t0) / m_window * all.width(); };
        const double hoverT = m_hoverX >= all.left() && m_hoverX <= all.right() ? t0 + (m_hoverX - all.left()) / all.width() * m_window : -1e300;

        // Time grid shared by every lane.
        const double step = NiceStep(m_window, std::max(2, static_cast<int>(all.width() / 110)));
        double y = all.top();
        QFont small = font();
        small.setPointSize(8);
        QFont valueFont = font();
        valueFont.setPointSize(12);
        valueFont.setBold(true);

        struct Member {
            const Lane *l;
            const SignalDef *d;
            const Series *s = nullptr;
            size_t i0 = 0, i1 = 0;
            double lo = 0, hi = 1;
        };
        for (const auto &group : groups) {
            const bool digital = Find(group.front()->key)->source == Source::Digital;
            const double h = std::max(16.0, weightOf(group) * unit);
            const QRectF r(all.left(), y, all.width(), h);
            y += h + kGap;
            p.fillRect(r, theme::kCanvas);
            p.setPen(QPen(theme::kGrid, 1));
            for (int k = static_cast<int>(std::ceil((t0 - ref) / step - 1e-9)); ref + k * step <= end + 1e-9; ++k) {
                if (k == 0 && !atTrigger) continue;  // "now" is the right edge
                p.drawLine(QPointF(X(ref + k * step), r.top()), QPointF(X(ref + k * step), r.bottom()));
            }

            // Each signal's visible samples and its range (auto from what is visible, or fixed).
            std::vector<Member> ms;
            for (const Lane *l : group) {
                Member m{l, Find(l->key)};
                const auto it = m_data->find(l->key);
                m.s = it == m_data->end() ? nullptr : &it->second;
                if (m.s && !m.s->t.empty()) {
                    m.i0 = std::lower_bound(m.s->t.begin(), m.s->t.end(), t0) - m.s->t.begin();
                    if (m.i0 > 0) --m.i0;  // the sample before the window, so the trace starts at the edge
                    m.i1 = std::upper_bound(m.s->t.begin(), m.s->t.end(), end) - m.s->t.begin();
                }
                if (digital) {
                    m.lo = -0.15;
                    m.hi = 1.15;
                } else if (l->autoScale) {
                    m.lo = std::numeric_limits<double>::infinity();
                    m.hi = -m.lo;
                    for (size_t i = m.i0; i < m.i1; ++i) {
                        m.lo = std::min(m.lo, m.s->v[i]);
                        m.hi = std::max(m.hi, m.s->v[i]);
                    }
                } else {
                    m.lo = l->min;
                    m.hi = l->max;
                }
                ms.push_back(m);
            }
            // Overlaid signals in one unit, all on auto, share one scale; otherwise each keeps its own
            // (like the channels of an oscilloscope), labelled in its colour.
            bool shared = ms.size() > 1;
            for (const Member &m : ms) shared = shared && m.l->autoScale && m.d->unit == ms.front().d->unit;
            if (shared) {
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                for (const Member &m : ms) {
                    lo = std::min(lo, m.lo);
                    hi = std::max(hi, m.hi);
                }
                for (Member &m : ms) {
                    m.lo = lo;
                    m.hi = hi;
                }
            }
            for (Member &m : ms) {
                if (digital || !m.l->autoScale) {
                    if (m.hi - m.lo < 1e-12) m.hi = m.lo + 1;
                    continue;
                }
                if (!std::isfinite(m.lo)) {
                    m.lo = -1;
                    m.hi = 1;
                }
                if (m.hi - m.lo < 1e-9) {
                    m.lo -= std::max(1.0, std::fabs(m.lo) * 0.05);
                    m.hi += std::max(1.0, std::fabs(m.hi) * 0.05);
                }
                const double pad = (m.hi - m.lo) * 0.1;
                m.lo -= pad;
                m.hi += pad;
            }
            const bool oneScale = ms.size() == 1 || shared;

            // Scale labels and the zero line.
            if (!digital) {
                const Member &f = ms.front();
                auto Yf = [&](double v) { return r.bottom() - (v - f.lo) / (f.hi - f.lo) * r.height(); };
                p.setPen(QPen(theme::kGridMajor, 1));
                if (oneScale && f.lo < 0 && f.hi > 0) p.drawLine(QPointF(r.left(), Yf(0)), QPointF(r.right(), Yf(0)));
                p.setFont(small);
                if (oneScale) {
                    p.setPen(theme::kAxisText);
                    p.drawText(QRectF(r.left() - 70, r.top(), 64, 14), Qt::AlignRight | Qt::AlignTop, Num(f.hi));
                    p.drawText(QRectF(r.left() - 70, r.bottom() - 14, 64, 14), Qt::AlignRight | Qt::AlignBottom, Num(f.lo));
                } else {
                    const int fit = std::max(1, static_cast<int>(h / 2 / 13));
                    for (int i = 0; i < static_cast<int>(ms.size()) && i < fit; ++i) {
                        p.setPen(ms[i].l->color);
                        p.drawText(QRectF(r.left() - 70, r.top() + 13 * i, 64, 13), Qt::AlignRight | Qt::AlignTop, Num(ms[i].hi));
                        p.drawText(QRectF(r.left() - 70, r.bottom() - 13 * (i + 1), 64, 13), Qt::AlignRight | Qt::AlignBottom,
                                   Num(ms[i].lo));
                    }
                }
            }

            // Traces, per pixel column: one pass over the samples keeps each column's first, min, max
            // and last value, so a frame costs about the same whatever the window holds. Analog traces
            // are one thin antialiased polyline through those points (peaks kept); digital lanes are
            // runs of one state.
            p.save();
            p.setClipRect(r.adjusted(0, -1, 0, 1));
            const int cols = std::max(1, static_cast<int>(r.width()));
            for (const Member &m : ms) {
                const Series *s = m.s;
                if (!s || m.i1 <= m.i0) continue;
                auto Y = [&](double v) { return r.bottom() - (v - m.lo) / (m.hi - m.lo) * r.height(); };
                if (!digital && m.i1 - m.i0 <= static_cast<size_t>(cols)) {
                    QPolygonF poly;
                    poly.reserve(static_cast<int>(m.i1 - m.i0));
                    for (size_t i = m.i0; i < m.i1; ++i) poly << QPointF(X(s->t[i]), Y(s->v[i]));
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(TracePen(m.l->color, 1.0));
                    BoldPolyline(p, poly);
                    continue;
                }
                m_cols.assign(cols, Col{});
                const double k = cols / m_window;
                for (size_t i = m.i0; i < m.i1; ++i) {
                    const int c = std::clamp(static_cast<int>((s->t[i] - t0) * k), 0, cols - 1);
                    Col &cc = m_cols[c];
                    const double v = s->v[i];
                    if (!cc.any) {
                        cc.any = true;
                        cc.mn = cc.mx = cc.first = v;
                    } else {
                        cc.mn = std::min(cc.mn, v);
                        cc.mx = std::max(cc.mx, v);
                    }
                    cc.last = v;
                }
                if (digital) {
                    // Low, high, or toggling within a pixel (a brighter band). An empty column holds on.
                    p.setRenderHint(QPainter::Antialiasing, false);
                    QVector<QLineF> lines;
                    QColor high = m.l->color, busy = m.l->color;
                    high.setAlpha(70);
                    busy.setAlpha(150);
                    const double yHi = Y(1.0), yLo = Y(0.0);
                    int state = -1, runStart = 0;
                    auto flush = [&](int endCol) {
                        if (state < 0 || endCol <= runStart) return;
                        const double x0 = r.left() + runStart, x1 = r.left() + endCol;
                        if (state > 0) p.fillRect(QRectF(x0, yHi, x1 - x0, yLo - yHi), state == 2 ? busy : high);
                        const double yy = state == 0 ? yLo : yHi;
                        lines << QLineF(x0, yy, x1, yy);
                    };
                    int lastData = -1;
                    for (int c = 0; c < cols; ++c) {
                        if (m_cols[c].any) lastData = c;
                    }
                    for (int c = 0; c <= lastData; ++c) {
                        const Col &cc = m_cols[c];
                        if (!cc.any) continue;
                        const int st = cc.mx > 0.5 ? (cc.mn < 0.5 ? 2 : 1) : 0;
                        if (st != state) {
                            flush(c);
                            if (state >= 0) lines << QLineF(r.left() + c, yLo, r.left() + c, yHi);
                            state = st;
                            runStart = c;
                        }
                    }
                    flush(lastData + 1);
                    p.setPen(QPen(m.l->color, 1));
                    p.drawLines(lines);
                } else {
                    // The curve: a thin antialiased line through one point per column (the column's mean of
                    // first and last, so it follows the signal without stepping). The spread inside a column
                    // (noise, a fast edge, a peak) as a crisp vertical line where it exceeds a pixel: vertical
                    // lines gain nothing from antialiasing, and this keeps the smooth part to ~1 segment per px.
                    QPolygonF poly;
                    poly.reserve(cols);
                    QVector<QLineF> spread;
                    for (int c = 0; c < cols; ++c) {
                        const Col &cc = m_cols[c];
                        if (!cc.any) continue;
                        const double x = r.left() + c + 0.5;
                        poly << QPointF(x, Y((cc.first + cc.last) / 2.0));
                        const double y0 = Y(cc.mn), y1 = Y(cc.mx);
                        if (y0 - y1 > 1.5) spread << QLineF(x, y0, x, y1);
                    }
                    if (!spread.isEmpty()) {
                        QColor sc = m.l->color;
                        sc.setAlpha(170);
                        p.setRenderHint(QPainter::Antialiasing, false);
                        p.setPen(QPen(sc, 1));
                        p.drawLines(spread);
                    }
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(TracePen(m.l->color, 1.0));
                    BoldPolyline(p, poly);
                }
            }
            p.restore();
            if (m_markShow) {
                for (const Member &m : ms) {
                    if (m.l->key != m_markSource) continue;
                    const double yl = r.bottom() - (m_markLevel - m.lo) / (m.hi - m.lo) * r.height();
                    if (yl < r.top() || yl > r.bottom()) continue;
                    p.setRenderHint(QPainter::Antialiasing, false);
                    p.setPen(QPen(kTrigColor, 1, Qt::DashLine));
                    p.drawLine(QPointF(r.left(), yl), QPointF(r.right(), yl));
                    p.setRenderHint(QPainter::Antialiasing, true);
                    QPolygonF tri;
                    tri << QPointF(r.right() + 1, yl) << QPointF(r.right() + 10, yl - 5) << QPointF(r.right() + 10, yl + 5);
                    p.setPen(Qt::NoPen);
                    p.setBrush(kTrigColor);
                    p.drawPolygon(tri);
                    p.setBrush(Qt::NoBrush);
                }
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(theme::kGridMajor, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r);

            // Gutter: the colour bar (one band per signal), names and values (latest, or under the mouse).
            auto valueOf = [&](const Member &m) {
                if (!m.s || m.i1 == 0) return std::numeric_limits<double>::quiet_NaN();
                size_t k = m.i1 - 1;
                if (hoverT > -1e299) {
                    const auto it2 = std::upper_bound(m.s->t.begin(), m.s->t.begin() + m.i1, hoverT);
                    if (it2 != m.s->t.begin()) k = (it2 - m.s->t.begin()) - 1;
                }
                return m.s->v[k];
            };
            auto valueText = [&](const Member &m, double v) {
                if (std::isnan(v)) return QStringLiteral("—");
                if (digital) {
                    const bool alert = m.l->key == QLatin1String("dig.alert");
                    return v > 0.5 ? (alert ? QStringLiteral("ALERT") : QStringLiteral("on"))
                                   : (alert ? QStringLiteral("none") : QStringLiteral("off"));
                }
                return Num(v) + (m.d->unit.isEmpty() ? QString() : QStringLiteral(" ") + m.d->unit);
            };
            const double band = r.height() / ms.size();
            for (size_t i = 0; i < ms.size(); ++i) p.fillRect(QRectF(0, r.top() + band * i, 4, band), ms[i].l->color);
            p.setFont(small);
            if (ms.size() == 1) {
                const Member &m = ms.front();
                p.setPen(theme::kText);
                p.drawText(QRectF(6, r.top() + 1, kGutter - 12, 14), Qt::AlignLeft | Qt::AlignTop,
                           p.fontMetrics().elidedText(LaneTitle(m.l->key, *m_labels), Qt::ElideRight, static_cast<int>(kGutter - (digital ? 14 : 82))));
                const QString vt = valueText(m, valueOf(m));
                if (h >= 34) {
                    p.setFont(valueFont);
                    p.setPen(m.l->color);
                    p.drawText(QRectF(6, r.top() + 15, kGutter - 80, 20), Qt::AlignLeft | Qt::AlignVCenter, vt);
                } else {
                    p.setPen(m.l->color);
                    p.drawText(QRectF(98, r.top() + 1, kGutter - 170, 14), Qt::AlignRight | Qt::AlignTop, vt);
                }
            } else {
                // One row per signal: swatch, name, value.
                const double row = std::min(32.0, (h - 2) / ms.size());
                QFont bold = small;
                bold.setBold(true);
                for (size_t i = 0; i < ms.size(); ++i) {
                    const Member &m = ms[i];
                    const double ry = r.top() + 1 + row * i;
                    if (row >= 28) {
                        p.setPen(theme::kText);
                        p.setFont(small);
                        p.drawText(QRectF(10, ry, kGutter - 86, 14), Qt::AlignLeft | Qt::AlignTop,
                                   p.fontMetrics().elidedText(LaneTitle(m.l->key, *m_labels), Qt::ElideRight, static_cast<int>(kGutter - 88)));
                        p.setFont(bold);
                        p.setPen(m.l->color);
                        p.drawText(QRectF(10, ry + 13, kGutter - 86, 15), Qt::AlignLeft | Qt::AlignTop, valueText(m, valueOf(m)));
                    } else {
                        p.setFont(small);
                        p.setPen(m.l->color);
                        const QString vt = valueText(m, valueOf(m));
                        const int vw = p.fontMetrics().horizontalAdvance(vt);
                        p.drawText(QRectF(10, ry, kGutter - 86, 14), Qt::AlignRight | Qt::AlignTop, vt);
                        p.setPen(theme::kText);
                        p.drawText(QRectF(10, ry, kGutter - 92 - vw, 14), Qt::AlignLeft | Qt::AlignTop,
                                   p.fontMetrics().elidedText(LaneTitle(m.l->key, *m_labels), Qt::ElideRight, static_cast<int>(kGutter - 92 - vw)));
                    }
                }
            }
        }

        // The trigger moment.
        if (m_markShow && std::isfinite(m_markT) && m_markT >= t0 && m_markT <= end) {
            const double x = X(m_markT);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(kTrigColor, 1, Qt::DashLine));
            p.drawLine(QPointF(x, all.top()), QPointF(x, all.bottom()));
            p.setRenderHint(QPainter::Antialiasing, true);
            QPolygonF tri;
            tri << QPointF(x - 5, all.top() - 6) << QPointF(x + 5, all.top() - 6) << QPointF(x, all.top());
            p.setPen(Qt::NoPen);
            p.setBrush(kTrigColor);
            p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
        }

        // Time axis and the hover line.
        p.setFont(small);
        p.setPen(theme::kAxisText);
        for (int k = static_cast<int>(std::ceil((t0 - ref) / step - 1e-9)); ref + k * step <= end + 1e-9; ++k) {
            const double x = X(ref + k * step);
            const bool edge = !atTrigger && k == 0;  // "now", right-aligned on the edge
            QString text;
            if (atTrigger) {
                text = k == 0 ? QStringLiteral("T") : QStringLiteral("%1%2 s").arg(k > 0 ? QStringLiteral("+") : QString(), Num(k * step));
            } else {
                text = k == 0 ? QStringLiteral("now") : QStringLiteral("%1 s").arg(Num(k * step));
            }
            p.setPen(atTrigger && k == 0 ? kTrigColor : theme::kAxisText);
            p.drawText(edge ? QRectF(x - 80, all.bottom() + 4, 80, 16) : QRectF(x - 40, all.bottom() + 4, 80, 16),
                       edge ? (Qt::AlignRight | Qt::AlignTop) : (Qt::AlignHCenter | Qt::AlignTop), text);
        }
        p.setPen(theme::kAxisText);
        if (hoverT > -1e299) {
            p.setPen(QPen(theme::kCursor, 1, Qt::DashLine));
            p.drawLine(QPointF(m_hoverX, all.top()), QPointF(m_hoverX, all.bottom()));
            const QRectF box(m_hoverX - 44, all.bottom() + 3, 88, 17);
            p.fillRect(box, QColor(0x2c, 0x34, 0x40));
            p.setPen(theme::kText);
            p.drawText(box, Qt::AlignCenter, QStringLiteral("%1 s").arg(QString::number(hoverT - ref, 'f', 3)));
        }
    }

private:
    // A cosmetic pen: Qt draws it with its fast antialiased line rasterizer instead of stroking a path
    // (joins at every corner), which matters at thousands of points a frame. Keep the width at 1 px:
    // a wider cosmetic pen falls back to stroking (a 300-point bus voltage trace at 1.2 px cost 9.5 ms a
    // frame, at 1 px 0.08 ms).
    static QPen TracePen(const QColor &c, double w) {
        QPen pen(c, w);
        pen.setCosmetic(true);
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::BevelJoin);
        return pen;
    }
    // A bold-looking trace from the fast 1 px pen: the line, then copies shifted 0.8 px down and right, so
    // flat and steep parts both thicken (one shifted copy would leave edges perpendicular to it thin).
    // About three times a 1 px line's cost, which stays a small part of a frame; a wider pen instead
    // would leave the fast rasterizer (see TracePen).
    static void BoldPolyline(QPainter &p, const QPolygonF &poly) {
        p.drawPolyline(poly);
        p.drawPolyline(poly.translated(0.0, 0.8));
        p.drawPolyline(poly.translated(0.8, 0.0));
    }
    static QString Num(double v) {
        if (std::fabs(v) < 1e-12) return QStringLiteral("0");
        return QString::number(v, 'g', std::fabs(v) >= 1000 ? 6 : 4);
    }
    static double NiceStep(double span, int ticks) {
        const double raw = span / ticks;
        const double mag = std::pow(10.0, std::floor(std::log10(raw)));
        const double n = raw / mag;
        return (n < 1.5 ? 1.0 : n < 3.5 ? 2.0 : n < 7.5 ? 5.0 : 10.0) * mag;
    }

    static constexpr double kGutter = 230, kRight = 12;

    // The span of everything recorded.
    bool DataRange(double *lo, double *hi) const {
        if (!m_data) return false;
        *lo = std::numeric_limits<double>::infinity();
        *hi = -*lo;
        for (const auto &kv : *m_data) {
            if (kv.second.t.empty()) continue;
            *lo = std::min(*lo, kv.second.t.front());
            *hi = std::max(*hi, kv.second.t.back());
        }
        return std::isfinite(*lo);
    }
    // A held view stays over the recording: no further left than its start, no further right than its end.
    void ClampHold() {
        double lo, hi;
        if (!held() || !DataRange(&lo, &hi)) return;
        const double maxEnd = hi + 0.05 * m_window;
        const double minEnd = std::min(lo + 0.1 * m_window, maxEnd);
        m_holdEnd = std::clamp(m_holdEnd, minEnd, maxEnd);
    }

    const std::vector<Lane> *m_lanes = nullptr;
    const std::map<QString, Series> *m_data = nullptr;
    const std::vector<QString> *m_labels = nullptr;
    double m_window = 5.0, m_end = 0.0, m_holdEnd = 0.0, m_hoverX = -1;
    bool m_hold = false, m_frozenManual = false, m_frozenLabel = false;
    bool m_trigView = false;
    double m_trigT = 0.0, m_trigPre = 0.2;
    bool m_markShow = false;
    QString m_markSource;
    double m_markLevel = 0.0, m_markT = std::numeric_limits<double>::quiet_NaN();
    static inline const QColor kTrigColor{0xff, 0x9f, 0x43};
    bool m_panning = false;
    double m_panX = 0.0, m_panEnd0 = 0.0;
    double m_paintMs = 0, m_maxPaintMs = 0;
    int m_frames = 0;
    struct Col {
        double mn = 0, mx = 0, first = 0, last = 0;
        bool any = false;
    };
    std::vector<Col> m_cols;  // per pixel column, reused frame to frame
};

// =====================================================================================================
// Window
// =====================================================================================================

namespace {

QSettings &Settings() {
    static QSettings s(QStringLiteral("ClearHPGL"), QStringLiteral("ClearHPGL"));  // the app's own settings
    return s;
}

constexpr double kHistorySeconds = 120.0;

enum Col { kColOn, kColSignal, kColSource, kColOverlay, kColColor, kColScale, kColMin, kColMax, kCols };

QString SourceText(Source s) {
    switch (s) {
        case Source::Stream: return QStringLiteral("stream, fast");
        case Source::Register: return QStringLiteral("register");
        case Source::Digital: return QStringLiteral("digital");
    }
    return {};
}

}  // namespace

ScopeWindow::ScopeWindow(QWidget *parent) : QWidget(parent, Qt::Window) {
    m_wall.start();  // before anything reads it (the trigger, the smooth scroll)
    qRegisterMetaType<scope::Batch>("scope::Batch");
    setWindowTitle(QStringLiteral("ClearPath scope — ClearHPGL"));
    setStyleSheet(theme::DialogSheet() + QStringLiteral("ScopeWindow { background: #2a2f36; }"));
    setAttribute(Qt::WA_StyledBackground);
    theme::ApplyAccent(this);
    m_lanes = DefaultLanes();
    BuildUi();
    LoadSettings();
    RebuildLaneTable();
    RefreshTriggerSources();
    ApplyTriggerUi();
    RefreshPorts();

    m_shared.start();
    SyncState();

    // Frames follow the display, not the data: a timer at the refresh rate of the screen the window is
    // on (checked each frame, so moving to another monitor follows it) moves the view to the newest
    // sample. Data arriving only appends; a frame with nothing new, a frozen view or a hidden window
    // draws nothing.
    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_frameTimer, &QTimer::timeout, this, [this]() {
        const int ms = frameIntervalMs();
        if (ms != m_frameTimer->interval()) m_frameTimer->setInterval(ms);
        if (!isVisible() || isMinimized() || !m_recording || !m_haveClock) return;
        if (m_trigMode == TriggerMode::Auto) RunTrigger();
        // The view's right edge runs on a smooth clock between data batches (which arrive ~30 times a
        // second), 70 ms behind the newest sample so there is always data at the edge. It never runs
        // past the data and never goes back: when the link stalls the view stops, and with nothing new
        // the plot skips the frame.
        constexpr double kLag = 0.07;  // about two batches, so a late batch does not stall the scroll
        double t = m_clockT + (m_wall.nsecsElapsed() / 1e9 - m_clockWall) - kLag;
        t = std::clamp(t, m_viewEnd, m_lastT);
        m_viewEnd = t;
        m_plot->setEnd(t);
    });
    m_frameTimer->start(frameIntervalMs());
}

// One drive: its link lives on its own thread, so a slow one can't hold up the others.
struct ScopeWindow::Motor {
    int slot = 0;
    QThread *thread = nullptr;
    ScopeWorker *worker = nullptr;
    QString port, serial, alerts;
    bool opening = false, connected = false, recording = false, enabled = false, healthy = false;
    int activeMon = 27;
    QLineEdit *name = nullptr;
};

ScopeWindow::~ScopeWindow() {
    stopAndDisconnect();
    for (const auto &m : m_motors) {
        m->thread->quit();
        m->thread->wait(3000);
    }
}

void ScopeWindow::stopAndDisconnect() {
    for (const auto &m : m_motors) {
        if (!m->thread->isRunning()) continue;
        QMetaObject::invokeMethod(m->worker, &ScopeWorker::close, Qt::BlockingQueuedConnection);
        m->connected = m->recording = m->opening = false;
    }
    SyncState();
}

int ScopeWindow::motorCount() const {
    int n = 0;
    for (const auto &m : m_motors) n += m->connected;
    return n;
}

void ScopeWindow::setMotorName(int motor, const QString &name) {
    if (motor < 0 || motor >= static_cast<int>(m_motors.size()) || !m_motors[motor]->name) return;
    m_motors[motor]->name->setText(name);
    emit m_motors[motor]->name->editingFinished();
}

ScopeWindow::Motor *ScopeWindow::FreeMotor() {
    for (const auto &m : m_motors) {
        if (!m->connected && !m->opening) return m.get();
    }
    if (static_cast<int>(m_motors.size()) >= kMaxMotors) return nullptr;
    m_motors.push_back(std::make_unique<Motor>());
    Motor *p = m_motors.back().get();
    p->slot = static_cast<int>(m_motors.size()) - 1;
    p->thread = new QThread(this);
    p->worker = new ScopeWorker(p->slot, &m_shared);
    p->worker->moveToThread(p->thread);
    connect(p->thread, &QThread::finished, p->worker, &QObject::deleteLater);
    connect(p->worker, &ScopeWorker::opened, this, [this, p](const QString &info, const QString &serial) {
        p->opening = false;
        p->connected = true;
        p->serial = serial;
        m_status->setText(QStringLiteral("Connected: ") + info);
        ShowMotors();
        if (p->slot > 0) AddLanes(p->slot);
        SyncState();
    });
    connect(p->worker, &ScopeWorker::closed, this, [this, p]() {
        p->connected = p->recording = p->healthy = false;
        ShowMotors();
        ShowHealth();
        SyncState();
    });
    connect(p->worker, &ScopeWorker::failed, this, [this, p](const QString &m) {
        p->opening = false;
        m_status->setText(QStringLiteral("<span style='color:#ff8a80;'>%1</span>").arg(m.toHtmlEscaped()));
        SyncState();
    });
    connect(p->worker, &ScopeWorker::message, this, [this](const QString &m) { m_status->setText(m); });
    connect(p->worker, &ScopeWorker::started, this, [this, p](int monId, const QString &was) {
        p->activeMon = monId;
        p->recording = true;
        if (motorCount() == 1) {
            m_status->setText(QStringLiteral("Recording. Channel 1 streams %1 (it was %2; put back on Stop).")
                                  .arg(QString::fromLatin1(FindMonVar(monId)->name), was));
        }
        SyncState();
    });
    connect(p->worker, &ScopeWorker::stopped, this, [this, p](const QString &m) {
        p->recording = false;
        m_status->setText(m);
        SyncState();
    });
    connect(p->worker, &ScopeWorker::batch, this, &ScopeWindow::OnBatch);
    connect(p->worker, &ScopeWorker::health, this, [this, p](const QString &alerts, bool, bool enabled) {
        p->alerts = alerts;
        p->enabled = enabled;
        p->healthy = true;
        ShowHealth();
    });
    p->thread->start();
    return p;
}

void ScopeWindow::ConnectPort(const QString &port, bool wait) {
    if (m_recording) return;
    bool anyone = false;
    for (const auto &m : m_motors) {
        anyone = anyone || m->connected || m->opening;
        if ((m->connected || m->opening) && m->port == port && port != QLatin1String(kSimulatedPort)) return;
    }
    if (!anyone) {  // a fresh start: the last session's other motors go
        m_lanes.erase(std::remove_if(m_lanes.begin(), m_lanes.end(), [](const Lane &l) { return MotorOf(l.key) > 0; }),
                      m_lanes.end());
        m_labels.clear();
        RebuildLaneTable();
        RefreshTriggerSources();
        Settings().setValue("clearpathScope/port", port);
    }
    Motor *m = FreeMotor();
    if (!m) {
        m_status->setText(QStringLiteral("%1 motors at most.").arg(kMaxMotors));
        return;
    }
    m->port = port;
    m->opening = true;
    m_status->setText(QStringLiteral("Connecting to %1…").arg(port));
    SyncState();
    QMetaObject::invokeMethod(m->worker, "open", wait ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(QString, port));
}

void ScopeWindow::ConnectAll() {
    for (int i = 0; i < m_port->count(); ++i) {
        const QString port = m_port->itemData(i).toString();
        if (port != QLatin1String(kSimulatedPort)) ConnectPort(port, false);
    }
}

// A new motor's lanes sit after the same signal's on the motors before it, so they compare at a glance. Only
// its fast signal starts on: three motors of everything is a lot of lanes.
void ScopeWindow::AddLanes(int motor) {
    std::vector<Lane> first;
    for (const Lane &l : m_lanes) {
        if (MotorOf(l.key) == 0) first.push_back(l);
    }
    for (const Lane &l : first) {
        const QString key = MotorKey(motor, l.key);
        size_t at = 0;
        bool have = false;
        for (size_t i = 0; i < m_lanes.size(); ++i) {
            have = have || m_lanes[i].key == key;
            if (BaseKey(m_lanes[i].key) == l.key) at = i + 1;
        }
        if (have) continue;
        Lane n = l;
        n.key = key;
        n.on = l.on && Find(l.key)->source == Source::Stream;
        n.overlay = false;
        int h, s, v;
        l.color.getHsv(&h, &s, &v);
        n.color = QColor::fromHsv((std::max(h, 0) + 70 * motor) % 360, s, v);
        m_lanes.insert(m_lanes.begin() + static_cast<std::ptrdiff_t>(at), n);
    }
    RebuildLaneTable();
    ApplyLanes();
}

// Every connected motor gets a name box once there is more than one; names stay with the drive's serial.
void ScopeWindow::ShowMotors() {
    auto *row = static_cast<QHBoxLayout *>(m_motorRow->layout());
    const int count = motorCount();
    for (const auto &up : m_motors) {
        Motor *m = up.get();
        if (!m->name) {
            m->name = new QLineEdit(m_motorRow);
            m->name->setMaximumWidth(170);
            row->insertWidget(row->count() - 1, m->name);
            connect(m->name, &QLineEdit::editingFinished, this, [this, m]() {
                if (!m->serial.isEmpty() && m->serial != QLatin1String("0")) {
                    Settings().setValue(QStringLiteral("clearpathScope/names/") + m->serial, m->name->text());
                }
                ShowMotors();
            });
        }
        m->name->setVisible(m->connected);
        if (!m->connected) continue;
        if (!m->name->hasFocus() && m->name->text().isEmpty()) {
            m->name->setText(Settings().value(QStringLiteral("clearpathScope/names/") + m->serial).toString());
        }
        const bool simulated = m->port == QLatin1String(kSimulatedPort);
        m->name->setPlaceholderText(simulated ? QStringLiteral("Motor %1").arg(m->slot + 1) : m->port);
        m->name->setToolTip(QStringLiteral("%1, serial %2. The name shows on its lanes and stays with the drive.")
                                .arg(m->port, m->serial));
        if (static_cast<int>(m_labels.size()) <= m->slot) m_labels.resize(m->slot + 1);
        m_labels[m->slot] = m->name->text().isEmpty() ? m->name->placeholderText() : m->name->text();
    }
    m_motorRow->setVisible(count > 1);
    RebuildLaneTable();
    RefreshTriggerSources();
    m_plot->update();
}

void ScopeWindow::ShowHealth() {
    QStringList parts;
    bool estop = false;
    const bool many = motorCount() > 1;
    for (const auto &m : m_motors) {
        if (!m->connected || !m->healthy) continue;
        const QString who = many && m->slot < static_cast<int>(m_labels.size()) ? m_labels[m->slot] + QStringLiteral(": ") : QString();
        parts << who + QStringLiteral("%1   ·   %2")
                           .arg(m->enabled ? QStringLiteral("<span style='color:#7fe3a0;'>enabled</span>")
                                           : QStringLiteral("<span style='color:#9fa6af;'>disabled</span>"),
                                !m->alerts.isEmpty() ? QStringLiteral("<span style='color:#ff8a80;'>⚠ alert: %1</span>").arg(m->alerts.toHtmlEscaped())
                                                     : QStringLiteral("<span style='color:#9fa6af;'>no alerts</span>"));
        estop = estop || m->alerts.contains(QStringLiteral("EStopped"));
    }
    m_alerts->setText(parts.join(QStringLiteral("   |   ")));
    m_clearEstop->setEnabled(estop);
}

void ScopeWindow::closeEvent(QCloseEvent *e) {
    SaveSettings();
    stopAndDisconnect();
    QWidget::closeEvent(e);
}

void ScopeWindow::BuildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto *bar = new QHBoxLayout();
    bar->setSpacing(8);
    bar->addWidget(new QLabel(QStringLiteral("Motor"), this));
    m_port = new QComboBox(this);
    m_port->setObjectName(QStringLiteral("ECamScopePort"));
    m_port->setMinimumWidth(190);
    m_port->setToolTip(QStringLiteral("The ClearPath's own USB port (Teknic 2890:0201). Every one plugged in is listed; "
                                      "connecting shows its serial number. Close ClearPath-MSP first: one program owns "
                                      "the port."));
    bar->addWidget(m_port);
    m_refresh = new QPushButton(QStringLiteral("Refresh"), this);
    bar->addWidget(m_refresh);
    m_connect = new QPushButton(QStringLiteral("Connect"), this);
    m_connect->setToolTip(QStringLiteral("Connect the motor picked here. Pick another and Add it to record them together."));
    bar->addWidget(m_connect);
    m_connectAll = new QPushButton(QStringLiteral("Connect all"), this);
    m_connectAll->setToolTip(QStringLiteral("Connect every ClearPath in the list, to record them on one time axis."));
    bar->addWidget(m_connectAll);
    m_disconnect = new QPushButton(QStringLiteral("Disconnect"), this);
    m_disconnect->setEnabled(false);
    bar->addWidget(m_disconnect);
    auto *sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("ECamSep"));
    sep->setFrameShape(QFrame::VLine);
    bar->addWidget(sep);
    m_run = new QPushButton(QStringLiteral("▶  Start"), this);
    m_run->setProperty("role", QStringLiteral("primary"));
    m_run->setMinimumWidth(96);
    bar->addWidget(m_run);
    m_freeze = new QPushButton(QStringLiteral("Freeze"), this);
    m_freeze->setCheckable(true);
    m_freeze->setToolTip(QStringLiteral("Hold the view; recording carries on."));
    bar->addWidget(m_freeze);
    bar->addWidget(new QLabel(QStringLiteral("Window"), this));
    m_window = new QDoubleSpinBox(this);
    m_window->setRange(ScopePlot::kMinWindow, ScopePlot::kMaxWindow);
    m_window->setDecimals(2);
    m_window->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    m_window->setSuffix(QStringLiteral(" s"));
    m_window->setValue(5);
    m_window->setToolTip(QStringLiteral("Timebase: seconds across the plot.\n"
                                        "Ctrl+wheel or Ctrl + / Ctrl − zooms (about the mouse when stopped or frozen),\n"
                                        "middle-drag or Shift+wheel pans back in time (freezes a live view),\n"
                                        "Ctrl+0 fits everything recorded."));
    bar->addWidget(m_window);
    bar->addStretch(1);
    m_clearEstop = new QPushButton(QStringLiteral("Clear E-stop"), this);
    m_clearEstop->setToolTip(QStringLiteral("Clear the node's EStopped alert (public SDK netNodeStop, clear E-stop)."));
    bar->addWidget(m_clearEstop);
    m_save = new QPushButton(QStringLiteral("Save CSV…"), this);
    bar->addWidget(m_save);
    m_lanesButton = new QPushButton(QStringLiteral("Lanes"), this);
    m_lanesButton->setCheckable(true);
    m_lanesButton->setToolTip(QStringLiteral("Choose the signals, their order, colours and scales. Saved."));
    bar->addWidget(m_lanesButton);
    m_helpButton = new QPushButton(QStringLiteral("Help"), this);
    m_helpButton->setToolTip(QStringLiteral("How the scope works (F1)."));
    bar->addWidget(m_helpButton);
    root->addLayout(bar);

    // Trigger: mode, source, edge, level, where the trigger sits across the screen.
    auto *trig = new QHBoxLayout();
    trig->setSpacing(8);
    auto *trigLabel = new QLabel(QStringLiteral("Trigger"), this);
    trigLabel->setStyleSheet(QStringLiteral("QLabel { color: #ff9f43; font-weight: 600; }"));
    trig->addWidget(trigLabel);
    m_trigModeBox = new QComboBox(this);
    m_trigModeBox->addItems({QStringLiteral("Off"), QStringLiteral("Auto"), QStringLiteral("Normal"), QStringLiteral("Single")});
    m_trigModeBox->setToolTip(QStringLiteral("Off: roll.\nAuto: hold each capture; roll when nothing triggers for a sweep.\n"
                                             "Normal: hold the latest capture, re-arm after each.\nSingle: one capture, then "
                                             "wait for Arm."));
    trig->addWidget(m_trigModeBox);
    trig->addWidget(new QLabel(QStringLiteral("on"), this));
    m_trigSourceBox = new QComboBox(this);
    m_trigSourceBox->setMinimumWidth(200);
    m_trigSourceBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    trig->addWidget(m_trigSourceBox);
    m_trigEdgeBox = new QComboBox(this);
    m_trigEdgeBox->addItems({QStringLiteral("↑ rising"), QStringLiteral("↓ falling"), QStringLiteral("↕ either")});
    trig->addWidget(m_trigEdgeBox);
    trig->addWidget(new QLabel(QStringLiteral("level"), this));
    m_trigLevelBox = new QDoubleSpinBox(this);
    m_trigLevelBox->setRange(-1e9, 1e9);
    m_trigLevelBox->setDecimals(3);
    m_trigLevelBox->setMinimumWidth(120);
    m_trigLevelBox->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
    trig->addWidget(m_trigLevelBox);
    m_trigHalf = new QPushButton(QStringLiteral("50 %"), this);
    m_trigHalf->setToolTip(QStringLiteral("Level to the middle of the source's range on screen."));
    trig->addWidget(m_trigHalf);
    trig->addWidget(new QLabel(QStringLiteral("at"), this));
    m_trigPreBox = new QSpinBox(this);
    m_trigPreBox->setRange(0, 100);
    m_trigPreBox->setSuffix(QStringLiteral(" %"));
    m_trigPreBox->setValue(20);
    m_trigPreBox->setToolTip(QStringLiteral("Where the trigger sits across the screen: the share of the timebase shown before it."));
    trig->addWidget(m_trigPreBox);
    m_trigArm = new QPushButton(QStringLiteral("Arm"), this);
    m_trigArm->setToolTip(QStringLiteral("Wait for the next trigger (Single)."));
    trig->addWidget(m_trigArm);
    m_trigStatus = new QLabel(this);
    m_trigStatus->setTextFormat(Qt::RichText);
    trig->addWidget(m_trigStatus, 1);
    root->addLayout(trig);

    auto applyTrigger = [this]() { ApplyTriggerUi(); };
    connect(m_trigModeBox, &QComboBox::currentIndexChanged, this, applyTrigger);
    connect(m_trigSourceBox, &QComboBox::currentIndexChanged, this, applyTrigger);
    connect(m_trigEdgeBox, &QComboBox::currentIndexChanged, this, applyTrigger);
    connect(m_trigLevelBox, &QDoubleSpinBox::valueChanged, this, applyTrigger);
    connect(m_trigPreBox, &QSpinBox::valueChanged, this, applyTrigger);
    connect(m_trigArm, &QPushButton::clicked, this, [this]() {
        ResetTrigger(m_recording ? m_lastT : -std::numeric_limits<double>::infinity());
        RunTrigger();
    });
    connect(m_trigHalf, &QPushButton::clicked, this, [this]() {
        const auto it = m_data.find(m_trigSource);
        if (it == m_data.end() || it->second.t.empty()) return;
        const double end = m_plot->viewEnd(), t0 = end - m_window->value();
        const auto &s = it->second;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (size_t i = std::lower_bound(s.t.begin(), s.t.end(), t0) - s.t.begin(); i < s.t.size() && s.t[i] <= end; ++i) {
            lo = std::min(lo, s.v[i]);
            hi = std::max(hi, s.v[i]);
        }
        if (std::isfinite(lo)) m_trigLevelBox->setValue((lo + hi) / 2.0);
    });

    m_motorRow = new QWidget(this);
    {
        auto *row = new QHBoxLayout(m_motorRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->addWidget(new QLabel(QStringLiteral("Motors"), m_motorRow));
        row->addStretch(1);
    }
    m_motorRow->setVisible(false);
    root->addWidget(m_motorRow);

    auto *statusRow = new QHBoxLayout();
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("ECamHelp"));
    m_status->setTextFormat(Qt::RichText);
    m_alerts = new QLabel(this);
    m_alerts->setTextFormat(Qt::RichText);
    statusRow->addWidget(m_status, 1);
    m_frozenBadge = new QLabel(QStringLiteral("Frozen — recording continues"), this);
    m_frozenBadge->setStyleSheet(QStringLiteral("QLabel { color: #1f242b; background: #ffcc80; border-radius: 3px; "
                                                "padding: 1px 8px; font-weight: 600; }"));
    m_frozenBadge->setVisible(false);
    statusRow->addWidget(m_frozenBadge);
    statusRow->addSpacing(12);
    statusRow->addWidget(m_alerts);
    root->addLayout(statusRow);

    m_split = new QSplitter(Qt::Horizontal, this);
    m_plot = new ScopePlot(m_split);
    m_plot->setSource(&m_lanes, &m_data, &m_labels);
    m_split->addWidget(m_plot);

    // Lane settings.
    m_config = new QWidget(m_split);
    auto *cv = new QVBoxLayout(m_config);
    cv->setContentsMargins(8, 0, 0, 0);
    cv->setSpacing(6);
    auto *heading = new QLabel(QStringLiteral("Lanes"), m_config);
    heading->setObjectName(QStringLiteral("ECamHeading"));
    cv->addWidget(heading);
    auto *help = new QLabel(QStringLiteral("Tick the signals to show; the order is the order on screen. Overlay draws a "
                                           "signal in the lane above it. The motor streams one fast signal, so only one "
                                           "\"stream, fast\" lane can be on."),
                            m_config);
    help->setObjectName(QStringLiteral("ECamHelp"));
    help->setWordWrap(true);
    cv->addWidget(help);
    m_table = new QTableWidget(0, kCols, m_config);
    m_table->setHorizontalHeaderLabels({QString(), QStringLiteral("Signal"), QStringLiteral("Source"), QStringLiteral("Overlay"),
                                        QStringLiteral("Colour"), QStringLiteral("Scale"), QStringLiteral("Min"), QStringLiteral("Max")});
    m_table->horizontalHeaderItem(kColOverlay)->setToolTip(QStringLiteral("Draw this signal in the lane above it (analog signals)."));
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(kColSignal, QHeaderView::Stretch);
    for (int c : {kColOn, kColOverlay, kColColor}) m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(kColSource, 92);
    m_table->setColumnWidth(kColScale, 74);
    m_table->setColumnWidth(kColMin, 80);
    m_table->setColumnWidth(kColMax, 80);
    cv->addWidget(m_table, 1);
    auto *moves = new QHBoxLayout();
    auto *up = new QPushButton(QStringLiteral("▲ Up"), m_config);
    auto *down = new QPushButton(QStringLiteral("▼ Down"), m_config);
    auto *defaults = new QPushButton(QStringLiteral("Restore defaults"), m_config);
    moves->addWidget(up);
    moves->addWidget(down);
    moves->addStretch(1);
    moves->addWidget(defaults);
    cv->addLayout(moves);
    m_noteLabel = new QLabel(m_config);
    m_noteLabel->setObjectName(QStringLiteral("ECamCard"));
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setMinimumHeight(70);
    m_noteLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    cv->addWidget(m_noteLabel);
    m_split->addWidget(m_config);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    root->addWidget(m_split, 1);


    connect(m_refresh, &QPushButton::clicked, this, &ScopeWindow::RefreshPorts);
    connect(m_connect, &QPushButton::clicked, this, [this]() {
        const QString port = m_port->currentData().toString();
        if (!port.isEmpty()) ConnectPort(port, false);
    });
    connect(m_connectAll, &QPushButton::clicked, this, &ScopeWindow::ConnectAll);
    connect(m_disconnect, &QPushButton::clicked, this, &ScopeWindow::Disconnect);
    connect(m_run, &QPushButton::clicked, this, [this]() { m_recording ? Stop() : Start(); });
    connect(m_freeze, &QPushButton::toggled, this, [this](bool on) {
        m_plot->setHold(!m_recording, on, on && m_recording);
        m_frozenBadge->setVisible(on && m_recording);
    });
    connect(m_helpButton, &QPushButton::clicked, this, &ScopeWindow::ShowHelp);
    auto *helpAction = new QAction(this);
    helpAction->setShortcut(QKeySequence(Qt::Key_F1));
    helpAction->setShortcutContext(Qt::WindowShortcut);
    connect(helpAction, &QAction::triggered, this, &ScopeWindow::ShowHelp);
    addAction(helpAction);
    connect(m_window, &QDoubleSpinBox::valueChanged, this, [this](double s) {
        m_plot->setWindow(s);
        Settings().setValue("clearpathScope/window", s);
    });
    // Zoom on the plot changes the timebase box; a pan on a live view freezes it.
    m_plot->onWindowChanged = [this](double s) {
        const QSignalBlocker block(m_window);
        m_window->setValue(s);
        Settings().setValue("clearpathScope/window", s);
    };
    m_plot->onPanWhileLive = [this]() { m_freeze->setChecked(true); };
    auto shortcut = [this](const QList<QKeySequence> &keys, std::function<void()> fn) {
        auto *a = new QAction(this);
        a->setShortcuts(keys);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    shortcut({QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)}, [this]() { m_plot->zoomAtHover(1.25); });
    shortcut({QKeySequence(Qt::CTRL | Qt::Key_Minus)}, [this]() { m_plot->zoomAtHover(1.0 / 1.25); });
    shortcut({QKeySequence(Qt::CTRL | Qt::Key_0)}, [this]() { m_plot->fitAll(); });
    connect(m_clearEstop, &QPushButton::clicked, this,
            [this]() {
                for (const auto &m : m_motors) {
                    if (m->connected && m->alerts.contains(QStringLiteral("EStopped"))) {
                        QMetaObject::invokeMethod(m->worker, "clearEstop", Qt::QueuedConnection);
                    }
                }
            });
    connect(m_save, &QPushButton::clicked, this, &ScopeWindow::SaveCsv);
    connect(m_lanesButton, &QPushButton::toggled, this, [this](bool on) {
        m_config->setVisible(on);
        Settings().setValue("clearpathScope/lanesVisible", on);
    });
    connect(up, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r <= 0) return;
        std::swap(m_lanes[r], m_lanes[r - 1]);
        RebuildLaneTable();
        m_table->selectRow(r - 1);
        ApplyLanes();
    });
    connect(down, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r < 0 || r + 1 >= static_cast<int>(m_lanes.size())) return;
        std::swap(m_lanes[r], m_lanes[r + 1]);
        RebuildLaneTable();
        m_table->selectRow(r + 1);
        ApplyLanes();
    });
    connect(defaults, &QPushButton::clicked, this, [this]() {
        m_lanes = DefaultLanes();
        for (const auto &m : m_motors) {
            if (m->connected && m->slot > 0) AddLanes(m->slot);
        }
        RebuildLaneTable();
        ApplyLanes();
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || row >= static_cast<int>(m_lanes.size())) return;
        if (const SignalDef *d = Find(m_lanes[row].key)) {
            m_noteLabel->setText(QStringLiteral("<b>%1</b><br>%2").arg(d->name.toHtmlEscaped(), d->note.toHtmlEscaped()));
        }
    });
}

void ScopeWindow::RefreshPorts() {
    // The one picked here, else the last one connected.
    const QString picked = m_port->currentData().toString();
    const QString saved = picked.isEmpty() ? Settings().value("clearpathScope/port").toString() : picked;
    const QStringList found = FindPorts();
    m_port->clear();
    for (const QString &p : found) m_port->addItem(QStringLiteral("ClearPath on %1").arg(p), p);
    m_port->addItem(QString::fromLatin1(kSimulatedPort), QString::fromLatin1(kSimulatedPort));
    const int i = m_port->findData(saved);
    m_port->setCurrentIndex(i >= 0 ? i : 0);
    if (!m_connected) {
        m_status->setText(found.isEmpty() ? QStringLiteral("No ClearPath (USB 2890:0201) found. Plug in the motor's USB, "
                                                           "or try the simulated motor.")
                                          : QStringLiteral("ClearPath found on %1. Close ClearPath-MSP before connecting.")
                                                .arg(found.join(QStringLiteral(", "))));
    }
}

void ScopeWindow::LoadSettings() {
    QSettings &s = Settings();
    m_window->setValue(s.value("clearpathScope/window", 5.0).toDouble());
    m_plot->setWindow(m_window->value());
    const QJsonArray arr = QJsonDocument::fromJson(s.value("clearpathScope/lanes").toByteArray()).array();
    if (!arr.isEmpty()) {
        std::vector<Lane> lanes;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            Lane l;
            l.key = o.value(QStringLiteral("key")).toString();
            if (!Find(l.key)) continue;
            l.on = o.value(QStringLiteral("on")).toBool();
            l.color = QColor(o.value(QStringLiteral("color")).toString());
            l.autoScale = o.value(QStringLiteral("auto")).toBool(true);
            l.min = o.value(QStringLiteral("min")).toDouble(-1);
            l.max = o.value(QStringLiteral("max")).toDouble(1);
            l.overlay = o.value(QStringLiteral("overlay")).toBool(false);
            lanes.push_back(l);
        }
        // Signals added since the settings were saved go at the end, off.
        for (const Lane &d : DefaultLanes()) {
            bool have = false;
            for (const Lane &l : lanes) have = have || l.key == d.key;
            if (!have) {
                Lane l = d;
                l.on = false;
                lanes.push_back(l);
            }
        }
        if (!lanes.empty()) m_lanes = lanes;
    }
    {
        const QSignalBlocker b1(m_trigModeBox), b2(m_trigEdgeBox), b3(m_trigLevelBox), b4(m_trigPreBox);
        m_trigMode = static_cast<TriggerMode>(std::clamp(s.value("clearpathScope/trigMode", 0).toInt(), 0, 3));
        m_trigSource = s.value("clearpathScope/trigSource", QStringLiteral("mon27")).toString();
        if (!Find(m_trigSource)) m_trigSource = QStringLiteral("mon27");
        m_trigEdge = static_cast<TriggerEdge>(std::clamp(s.value("clearpathScope/trigEdge", 0).toInt(), 0, 2));
        m_trigLevel = s.value("clearpathScope/trigLevel", 0.0).toDouble();
        m_trigPre = std::clamp(s.value("clearpathScope/trigPre", 0.2).toDouble(), 0.0, 1.0);
        m_trigModeBox->setCurrentIndex(static_cast<int>(m_trigMode));
        m_trigEdgeBox->setCurrentIndex(static_cast<int>(m_trigEdge));
        m_trigLevelBox->setValue(m_trigLevel);
        m_trigPreBox->setValue(static_cast<int>(std::lround(m_trigPre * 100)));
    }
    restoreGeometry(s.value("clearpathScope/geometry").toByteArray());
    if (!s.contains("clearpathScope/geometry")) resize(1240, 760);
    m_split->restoreState(s.value("clearpathScope/split").toByteArray());
    const bool lanesVisible = s.value("clearpathScope/lanesVisible", false).toBool();
    m_lanesButton->setChecked(lanesVisible);
    m_config->setVisible(lanesVisible);
}

void ScopeWindow::SaveSettings() const {
    QSettings &s = Settings();
    QJsonArray arr;
    for (const Lane &l : m_lanes) {
        if (MotorOf(l.key) > 0) continue;  // those come with their motor
        QJsonObject o;
        o.insert(QStringLiteral("key"), l.key);
        o.insert(QStringLiteral("on"), l.on);
        o.insert(QStringLiteral("color"), l.color.name());
        o.insert(QStringLiteral("auto"), l.autoScale);
        o.insert(QStringLiteral("min"), l.min);
        o.insert(QStringLiteral("max"), l.max);
        o.insert(QStringLiteral("overlay"), l.overlay);
        arr.append(o);
    }
    s.setValue("clearpathScope/lanes", QJsonDocument(arr).toJson(QJsonDocument::Compact));
    s.setValue("clearpathScope/window", m_window->value());
    s.setValue("clearpathScope/trigMode", static_cast<int>(m_trigMode));
    s.setValue("clearpathScope/trigSource", m_trigSource);
    s.setValue("clearpathScope/trigEdge", static_cast<int>(m_trigEdge));
    s.setValue("clearpathScope/trigLevel", m_trigLevel);
    s.setValue("clearpathScope/trigPre", m_trigPre);
    s.setValue("clearpathScope/geometry", saveGeometry());
    s.setValue("clearpathScope/split", m_split->saveState());
}

void ScopeWindow::RebuildLaneTable() {
    const QSignalBlocker block(m_table);
    m_table->setRowCount(static_cast<int>(m_lanes.size()));
    for (int r = 0; r < static_cast<int>(m_lanes.size()); ++r) {
        const Lane &l = m_lanes[r];
        const SignalDef *d = Find(l.key);
        auto *on = new QCheckBox(m_table);
        on->setChecked(l.on);
        on->setStyleSheet(QStringLiteral("QCheckBox { margin-left: 6px; }"));
        connect(on, &QCheckBox::toggled, this, [this, r](bool checked) {
            m_lanes[r].on = checked;
            // One streamed signal at a time.
            const SignalDef *dd = Find(m_lanes[r].key);
            if (checked && dd->source == Source::Stream) {
                for (int i = 0; i < static_cast<int>(m_lanes.size()); ++i) {
                    if (i != r && m_lanes[i].on && MotorOf(m_lanes[i].key) == MotorOf(m_lanes[r].key) &&
                        Find(m_lanes[i].key)->source == Source::Stream) {
                        m_lanes[i].on = false;
                        m_noteLabel->setText(QStringLiteral("Switched off <b>%1</b>: the motor streams one fast signal at a time.")
                                                 .arg(Find(m_lanes[i].key)->name.toHtmlEscaped()));
                    }
                }
                // Not from inside this checkbox's own signal: the rebuild replaces it.
                QTimer::singleShot(0, this, [this]() { RebuildLaneTable(); });
            }
            ApplyLanes();
        });
        m_table->setCellWidget(r, kColOn, on);
        auto *name = new QTableWidgetItem(LaneTitle(l.key, m_labels));
        name->setToolTip(d->note);
        m_table->setItem(r, kColSignal, name);
        m_table->setItem(r, kColSource, new QTableWidgetItem(SourceText(d->source)));
        auto *overlay = new QCheckBox(m_table);
        overlay->setChecked(l.overlay);
        overlay->setEnabled(d->source != Source::Digital);
        overlay->setToolTip(QStringLiteral("Draw in the lane above (the nearest shown analog lane). Signals in one unit "
                                           "share a scale; otherwise each keeps its own, labelled in its colour."));
        overlay->setStyleSheet(QStringLiteral("QCheckBox { margin-left: 14px; }"));
        connect(overlay, &QCheckBox::toggled, this, [this, r](bool checked) {
            m_lanes[r].overlay = checked;
            ApplyLanes();
        });
        m_table->setCellWidget(r, kColOverlay, overlay);
        auto *color = new QPushButton(m_table);
        color->setFixedSize(34, 20);
        color->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid #1a1e24; border-radius: 2px; "
                                            "min-height: 0; padding: 0; }")
                                 .arg(l.color.name()));
        connect(color, &QPushButton::clicked, this, [this, r]() {
            const QColor c = QColorDialog::getColor(m_lanes[r].color, this, QStringLiteral("Lane colour"));
            if (!c.isValid()) return;
            m_lanes[r].color = c;
            QTimer::singleShot(0, this, [this]() { RebuildLaneTable(); });
            ApplyLanes();
        });
        m_table->setCellWidget(r, kColColor, color);
        const bool digital = d->source == Source::Digital;
        auto *scale = new QComboBox(m_table);
        scale->addItems({QStringLiteral("Auto"), QStringLiteral("Fixed")});
        scale->setCurrentIndex(l.autoScale ? 0 : 1);
        scale->setEnabled(!digital);
        m_table->setCellWidget(r, kColScale, scale);
        auto spin = [&](double v) {
            auto *s = new QDoubleSpinBox(m_table);
            s->setRange(-1e9, 1e9);
            s->setDecimals(3);
            s->setValue(v);
            s->setEnabled(!digital && !l.autoScale);
            s->setButtonSymbols(QAbstractSpinBox::NoButtons);
            return s;
        };
        QDoubleSpinBox *mn = spin(l.min), *mx = spin(l.max);
        m_table->setCellWidget(r, kColMin, mn);
        m_table->setCellWidget(r, kColMax, mx);
        connect(scale, &QComboBox::currentIndexChanged, this, [this, r, mn, mx](int i) {
            m_lanes[r].autoScale = i == 0;
            mn->setEnabled(i == 1);
            mx->setEnabled(i == 1);
            ApplyLanes();
        });
        connect(mn, &QDoubleSpinBox::valueChanged, this, [this, r](double v) {
            m_lanes[r].min = v;
            ApplyLanes();
        });
        connect(mx, &QDoubleSpinBox::valueChanged, this, [this, r](double v) {
            m_lanes[r].max = v;
            ApplyLanes();
        });
    }
}

int ScopeWindow::laneCount() const {
    int n = 0;
    for (const Lane &l : m_lanes) n += l.on;
    return n;
}

int ScopeWindow::StreamMonId(int motor) const {
    for (const Lane &l : m_lanes) {
        if (!l.on || MotorOf(l.key) != motor) continue;
        const SignalDef *d = Find(l.key);
        if (d && d->source == Source::Stream) return d->monId;
    }
    return 27;  // the stream runs anyway (it carries the digital lanes); position error by default
}

QStringList ScopeWindow::RegisterKeys(int motor) const {
    QStringList out;
    for (const Lane &l : m_lanes) {
        const SignalDef *d = Find(l.key);
        if (l.on && d && d->source == Source::Register && MotorOf(l.key) == motor) out << BaseKey(l.key);
    }
    return out;
}

void ScopeWindow::ApplyLanes() {
    RefreshTriggerSources();
    m_plot->update();
    SaveSettings();
    for (const auto &m : m_motors) {
        if (!m->recording) continue;
        if (StreamMonId(m->slot) != m->activeMon) {
            // A different streamed signal: restart the stream on it (channel 1 is put back first).
            QMetaObject::invokeMethod(m->worker, "start", Qt::QueuedConnection, Q_ARG(int, StreamMonId(m->slot)),
                                      Q_ARG(QStringList, RegisterKeys(m->slot)));
        } else {
            QMetaObject::invokeMethod(m->worker, "setRegisters", Qt::QueuedConnection,
                                      Q_ARG(QStringList, RegisterKeys(m->slot)));
        }
    }
}

void ScopeWindow::setLanes(const std::vector<Lane> &lanes) {
    m_lanes = lanes;
    RebuildLaneTable();
    ApplyLanes();
}

void ScopeWindow::Start() {
    if (!m_connected) return;
    m_data.clear();
    m_lastT = 0.0;
    m_haveClock = false;
    m_viewEnd = 0.0;
    ResetTrigger(-std::numeric_limits<double>::infinity());
    m_freeze->setChecked(false);
    m_run->setEnabled(false);
    m_status->setText(QStringLiteral("Starting…"));
    for (const auto &m : m_motors) {
        if (!m->connected) continue;
        QMetaObject::invokeMethod(m->worker, "start", Qt::QueuedConnection, Q_ARG(int, StreamMonId(m->slot)),
                                  Q_ARG(QStringList, RegisterKeys(m->slot)));
    }
}

void ScopeWindow::Stop() {
    m_run->setEnabled(false);
    for (const auto &m : m_motors) {
        if (m->recording) QMetaObject::invokeMethod(m->worker, "stop", Qt::QueuedConnection);
    }
}

void ScopeWindow::Disconnect() {
    for (const auto &m : m_motors) {
        if (m->connected) QMetaObject::invokeMethod(m->worker, "close", Qt::QueuedConnection);
    }
    m_status->setText(QStringLiteral("Disconnected."));
}

void ScopeWindow::startSimulated(int motors) {
    m_port->setCurrentIndex(m_port->findData(QString::fromLatin1(kSimulatedPort)));
    for (int i = 0; i < motors; ++i) {
        ConnectPort(QString::fromLatin1(kSimulatedPort), true);
        QCoreApplication::processEvents();  // its lanes
    }
    for (const auto &m : m_motors) {
        if (!m->connected) continue;
        QMetaObject::invokeMethod(m->worker, "start", Qt::BlockingQueuedConnection, Q_ARG(int, StreamMonId(m->slot)),
                                  Q_ARG(QStringList, RegisterKeys(m->slot)));
    }
    QCoreApplication::processEvents();
}

const Series *ScopeWindow::series(const QString &key) const {
    const auto it = m_data.find(key);
    return it == m_data.end() ? nullptr : &it->second;
}

QString ScopeWindow::statusText() const { return m_status->text(); }

double ScopeWindow::maxPaintMs() const { return m_plot->maxPaintMs(); }
void ScopeWindow::resetPaintStats() { m_plot->resetPaintStats(); }
void ScopeWindow::setWindowSeconds(int s) { m_window->setValue(s); }

double ScopeWindow::windowSeconds() const { return m_window->value(); }
double ScopeWindow::plotViewEnd() const { return m_plot->viewEnd(); }
QWidget *ScopeWindow::plotWidget() const { return m_plot; }

int ScopeWindow::frameIntervalMs() const {
    const QScreen *s = screen();
    const double hz = std::clamp(s && s->refreshRate() > 1.0 ? s->refreshRate() : 60.0, 24.0, 240.0);
    return std::max(4, static_cast<int>(1000.0 / hz));  // 60 Hz -> 16 ms
}

int ScopeWindow::plotFrames() const { return m_plot->frames(); }

int ScopeWindow::shownLaneCount() const {
    // Lanes on screen after overlays merge, as the plot groups them.
    int n = 0;
    bool prevAnalog = false;
    for (const Lane &l : m_lanes) {
        const SignalDef *d = l.on ? Find(l.key) : nullptr;
        if (!d) continue;
        const bool analog = d->source != Source::Digital;
        if (!(l.overlay && analog && prevAnalog)) ++n;
        prevAnalog = analog;
    }
    return n;
}

void ScopeWindow::SyncState() {
    bool opening = false;
    int count = 0, recording = 0;
    for (const auto &m : m_motors) {
        opening = opening || m->opening;
        count += m->connected;
        recording += m->recording;
    }
    m_connected = count > 0;
    m_recording = recording > 0;
    m_connect->setText(m_connected ? QStringLiteral("Add") : QStringLiteral("Connect"));
    m_connect->setEnabled(!opening && !m_recording && count < kMaxMotors);
    m_connectAll->setEnabled(!opening && !m_recording && count < kMaxMotors);
    m_disconnect->setEnabled(m_connected);
    m_port->setEnabled(!m_recording);
    m_refresh->setEnabled(!m_recording);
    m_run->setEnabled(m_connected && !opening);
    m_run->setText(m_recording ? QStringLiteral("■  Stop") : QStringLiteral("▶  Start"));
    // Stopped, the view holds (and can be zoomed and panned); recording, it is live unless frozen.
    m_plot->setHold(!m_recording, m_freeze->isChecked(), m_freeze->isChecked() && m_recording);
    m_frozenBadge->setVisible(m_freeze->isChecked() && m_recording);
    m_save->setEnabled(!m_data.empty());
    if (!m_connected) {
        m_clearEstop->setEnabled(false);
        m_alerts->clear();
    }
}

void ScopeWindow::OnBatch(const Batch &b) {
    const MonVar *mv = FindMonVar(b.monId);
    Series &mon = m_data[MotorKey(b.motor, QStringLiteral("mon%1").arg(b.monId))];
    Series &hlfb = m_data[MotorKey(b.motor, QStringLiteral("dig.hlfb"))];
    Series &enable = m_data[MotorKey(b.motor, QStringLiteral("dig.enable"))];
    for (const StreamSample &s : b.stream) {
        if (mv) mon.add(s.t, MonScale(*mv, s.raw));
        hlfb.add(s.t, (s.digital >> 3) & 1);
        enable.add(s.t, s.digital & 1);
        m_lastT = std::max(m_lastT, s.t);
        m_rateWindow.push_back(s.t);
    }
    for (size_t i = 0; i < b.regKey.size(); ++i) {
        m_data[MotorKey(b.motor, b.regKey[i])].add(b.regT[i], b.regV[i]);
        m_lastT = std::max(m_lastT, b.regT[i]);
    }
    // Keep the view clock in step with the data: re-anchor on a jump (start, a stall), otherwise nudge it
    // a little each batch so the scroll stays smooth.
    {
        const double wall = m_wall.nsecsElapsed() / 1e9;
        const double predicted = m_clockT + (wall - m_clockWall);
        if (!m_haveClock || std::fabs(predicted - m_lastT) > 0.15) {
            m_clockT = m_lastT;
            if (!m_haveClock) m_viewEnd = m_lastT - 0.07;
            m_haveClock = true;
        } else {
            m_clockT = predicted + (m_lastT - predicted) * 0.1;
        }
        m_clockWall = wall;
    }
    const double cut = m_lastT - std::max(kHistorySeconds, m_window->value() * 2.0);
    for (auto &kv : m_data) kv.second.trim(cut);
    // Motors' batches interleave, so this isn't quite in order; it is only a rate.
    m_rateWindow.erase(std::remove_if(m_rateWindow.begin(), m_rateWindow.end(), [this](double t) { return t < m_lastT - 1.0; }),
                       m_rateWindow.end());
    const int motors = std::max(1, motorCount());
    m_rate = static_cast<int>(m_rateWindow.size()) / motors;
    if (m_recording && mv && m_lastT - m_statusT >= 0.25) {  // a few times a second is plenty
        m_statusT = m_lastT;
        if (motors > 1) {
            m_status->setText(QStringLiteral("Recording %1 motors on one time axis · about %2 samples/s each").arg(motors).arg(m_rate));
        } else {
            m_status->setText(QStringLiteral("Recording · channel 1: %1 · %2 samples/s · %3 registers in turn")
                                  .arg(QString::fromLatin1(mv->name))
                                  .arg(m_rate)
                                  .arg(RegisterKeys(b.motor).size()));
        }
    }
    m_save->setEnabled(true);
    RunTrigger();
}

// ---- Help --------------------------------------------------------------------------------------------

// The scope's help page (resources/help/scope.html, with pictures), in a reader of its own: one instance,
// brought to the front when it is already open.
void ScopeWindow::ShowHelp() {
    if (m_help) {
        m_help->show();
        m_help->raise();
        m_help->activateWindow();
        return;
    }
    static const char *kCss = R"CSS(
body { color: #d8dee6; font-size: 10.5pt; }
h1 { color: #f0f4f8; font-size: 20pt; font-weight: 700; margin-top: 4px; margin-bottom: 8px; }
h2 { color: #8fc6ff; font-size: 14pt; font-weight: 700; margin-top: 18px; margin-bottom: 6px; }
h3 { color: #e6ebf3; font-size: 11.5pt; font-weight: 700; margin-top: 12px; margin-bottom: 4px; }
p, li { color: #d8dee6; }
a { color: #8fc6ff; }
b { color: #f0f4f8; }
i { color: #c3cbd6; }
kbd { color: #f0f4f8; background-color: #2c333c; font-family: Consolas, monospace; }
th { color: #9fa6af; background-color: #262c34; text-align: left; padding: 4px 10px; border-bottom: 1px solid #3a424d; }
td { padding: 4px 10px; border-bottom: 1px solid #2c333c; vertical-align: top; }
p.note { background-color: #1e3048; color: #dbe8f7; padding: 8px 12px; }
p.caption { color: #9fa6af; font-size: 9pt; margin-top: 2px; }
)CSS";
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("ClearPath scope — Help"));
    dlg->setStyleSheet(theme::DialogSheet());
    dlg->resize(940, 820);
    auto *layout = new QVBoxLayout(dlg);
    auto *browser = new QTextBrowser(dlg);
    browser->setOpenExternalLinks(true);
    browser->setStyleSheet(QStringLiteral("QTextBrowser { background: #1f242b; border: 1px solid #1a1e24; border-radius: 3px; padding: 10px; }"));
    browser->document()->setDefaultStyleSheet(QString::fromUtf8(kCss));
    browser->setSearchPaths({QStringLiteral(":/help")});
    browser->setSource(QUrl(QStringLiteral("qrc:/help/scope.html")));
    layout->addWidget(browser);
    m_help = dlg;
    dlg->show();
}

QDialog *ScopeWindow::helpDialog() const { return m_help; }
bool ScopeWindow::frozenBadgeShown() const { return m_frozenBadge->isVisible(); }

// ---- Trigger -------------------------------------------------------------------------------------------

void ScopeWindow::RefreshTriggerSources() {
    // The lanes on screen, in their order; the chosen source stays listed even if its lane is off.
    const QSignalBlocker block(m_trigSourceBox);
    m_trigSourceBox->clear();
    for (const Lane &l : m_lanes) {
        const SignalDef *d = Find(l.key);
        if (d && (l.on || l.key == m_trigSource)) m_trigSourceBox->addItem(LaneTitle(l.key, m_labels) + (l.on ? QString() : QStringLiteral(" (off)")), l.key);
    }
    const int i = m_trigSourceBox->findData(m_trigSource);
    m_trigSourceBox->setCurrentIndex(std::max(0, i));
}

void ScopeWindow::ApplyTriggerUi() {
    m_trigMode = static_cast<TriggerMode>(m_trigModeBox->currentIndex());
    if (m_trigSourceBox->currentIndex() >= 0) m_trigSource = m_trigSourceBox->currentData().toString();
    m_trigEdge = static_cast<TriggerEdge>(m_trigEdgeBox->currentIndex());
    m_trigLevel = m_trigLevelBox->value();
    m_trigPre = m_trigPreBox->value() / 100.0;
    const SignalDef *d = Find(m_trigSource);
    const bool digital = d && d->source == Source::Digital;
    const bool on = m_trigMode != TriggerMode::Off;
    for (QWidget *w : std::initializer_list<QWidget *>{m_trigSourceBox, m_trigEdgeBox, m_trigPreBox}) w->setEnabled(on);
    m_trigLevelBox->setEnabled(on && !digital);
    m_trigHalf->setEnabled(on && !digital);
    m_trigArm->setVisible(m_trigMode == TriggerMode::Single);
    m_trigLevelBox->setSuffix(d && !d->unit.isEmpty() ? QStringLiteral(" ") + d->unit : QString());
    SaveSettings();
    // A new setting looks again from the start of what is recorded: a stopped recording can be studied
    // with the trigger after the fact.
    ResetTrigger(-std::numeric_limits<double>::infinity());
    RunTrigger();
}

void ScopeWindow::ResetTrigger(double scanFrom) {
    m_trigArmed = true;
    m_trigCount = 0;
    m_capT = m_pendingT = std::numeric_limits<double>::quiet_NaN();
    m_scanT = scanFrom;
    m_havePrev = m_readyLow = m_readyHigh = false;
    m_lastCapWall = m_wall.nsecsElapsed() / 1e9;
}

bool ScopeWindow::triggerRolling() const { return m_trigRolling; }

void ScopeWindow::setTrigger(TriggerMode mode, const QString &source, TriggerEdge edge, double level, double pre) {
    const QSignalBlocker b1(m_trigModeBox), b2(m_trigSourceBox), b3(m_trigEdgeBox), b4(m_trigLevelBox), b5(m_trigPreBox);
    m_trigSource = source;
    RefreshTriggerSources();
    m_trigModeBox->setCurrentIndex(static_cast<int>(mode));
    m_trigEdgeBox->setCurrentIndex(static_cast<int>(edge));
    m_trigLevelBox->setValue(level);
    m_trigPreBox->setValue(static_cast<int>(std::lround(pre * 100)));
    ApplyTriggerUi();
}

// Looks for the trigger in the source's new samples; a capture is complete once the data reaches the
// screen's worth after it. Analog sources trigger with hysteresis (2 % of their range on screen) so noise
// at the level cannot re-trigger, and the time is interpolated between the two samples either side.
void ScopeWindow::RunTrigger() {
    const bool on = m_trigMode != TriggerMode::Off;
    const SignalDef *d = Find(m_trigSource);
    const bool digital = d && d->source == Source::Digital;
    const double level = digital ? 0.5 : m_trigLevel;
    const double win = m_window->value();
    const double post = (1.0 - m_trigPre) * win;
    if (!on) {
        m_trigRolling = true;
        m_plot->setTrigger(false, 0.0, m_trigPre);
        m_plot->setTriggerMarks(false, m_trigSource, level, std::numeric_limits<double>::quiet_NaN());
        m_trigStatus->clear();
        return;
    }
    const auto it = m_data.find(m_trigSource);
    if (it != m_data.end() && !it->second.t.empty()) {
        const Series &s = it->second;
        double hyst = 0.0;
        if (!digital) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (size_t i = std::lower_bound(s.t.begin(), s.t.end(), s.t.back() - win) - s.t.begin(); i < s.t.size(); ++i) {
                lo = std::min(lo, s.v[i]);
                hi = std::max(hi, s.v[i]);
            }
            hyst = std::isfinite(lo) ? 0.02 * (hi - lo) : 0.0;
        }
        size_t i = std::upper_bound(s.t.begin(), s.t.end(), m_scanT) - s.t.begin();
        for (; i < s.t.size() && m_trigArmed; ++i) {
            const double t = s.t[i], v = s.v[i];
            // A trigger in waiting completes once the screen's worth after it has arrived.
            if (!std::isnan(m_pendingT) && t >= m_pendingT + post) {
                m_capT = m_pendingT;
                m_pendingT = std::numeric_limits<double>::quiet_NaN();
                ++m_trigCount;
                m_lastCapWall = m_wall.nsecsElapsed() / 1e9;
                if (m_trigMode == TriggerMode::Single) m_trigArmed = false;
            }
            if (std::isnan(m_pendingT) && m_trigArmed) {
                if (v < level - hyst) m_readyLow = true;   // went below: a rising crossing may follow
                if (v > level + hyst) m_readyHigh = true;  // went above: a falling one may
                if (m_havePrev) {
                    const bool rise = m_readyLow && m_prevV < level && v >= level;
                    const bool fall = m_readyHigh && m_prevV > level && v <= level;
                    const bool fire = (m_trigEdge == TriggerEdge::Rising && rise) || (m_trigEdge == TriggerEdge::Falling && fall) ||
                                      (m_trigEdge == TriggerEdge::Either && (rise || fall));
                    // Captures do not overlap: the next may start once the last one's screen has passed.
                    const bool clear = std::isnan(m_capT) || t >= m_capT + post;
                    if (fire && clear) {
                        m_pendingT = (!digital && v != m_prevV) ? m_prevT + (level - m_prevV) / (v - m_prevV) * (t - m_prevT) : t;
                        m_readyLow = m_readyHigh = false;
                    }
                }
            }
            m_prevV = v;
            m_prevT = t;
            m_havePrev = true;
            m_scanT = t;
        }
    }

    // What the plot shows: the latest capture, or (Auto with nothing for a sweep, or before the first
    // capture) the live roll.
    const double wall = m_wall.nsecsElapsed() / 1e9;
    bool rolling = std::isnan(m_capT);
    if (m_trigMode == TriggerMode::Auto && !rolling && m_recording && wall - m_lastCapWall > std::max(win, 0.3)) rolling = true;
    m_trigRolling = rolling;
    m_plot->setTrigger(!rolling, m_capT, m_trigPre);
    m_plot->setTriggerMarks(true, m_trigSource, level, m_capT);

    QString st;
    const QString count = m_trigCount ? QStringLiteral(" · %1 capture%2").arg(m_trigCount).arg(m_trigCount == 1 ? "" : "s") : QString();
    if (m_trigMode == TriggerMode::Single && !m_trigArmed) {
        st = QStringLiteral("<span style='color:#7fe3a0;'>captured</span> — Arm for another");
    } else if (m_trigMode == TriggerMode::Auto && rolling && m_trigCount) {
        st = QStringLiteral("<span style='color:#9fa6af;'>no trigger for a sweep — rolling</span>") + count;
    } else if (!std::isnan(m_pendingT)) {
        st = QStringLiteral("<span style='color:#ff9f43;'>triggered, filling the screen…</span>") + count;
    } else if (m_trigCount) {
        st = QStringLiteral("<span style='color:#7fe3a0;'>triggered</span>") + count;
    } else {
        st = QStringLiteral("<span style='color:#ff9f43;'>armed — waiting for a trigger</span>");
    }
    if (m_trigStatus->text() != st) m_trigStatus->setText(st);
}

void ScopeWindow::SaveCsv() {
    if (m_data.empty()) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save scope data"),
        QDir(Settings().value("files/lastDir").toString()).filePath(QStringLiteral("clearpath_scope.csv")),
        QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Save scope data"), f.errorString());
        return;
    }
    QTextStream ts(&f);
    ts << "t_s,signal,value,unit\n";
    for (const Lane &l : m_lanes) {
        const auto it = m_data.find(l.key);
        if (it == m_data.end()) continue;
        const SignalDef *d = Find(l.key);
        for (size_t i = 0; i < it->second.t.size(); ++i) {
            ts << QString::number(it->second.t[i], 'f', 5) << ',' << '"' << LaneTitle(l.key, m_labels) << '"' << ','
               << QString::number(it->second.v[i], 'g', 9) << ',' << d->unit << '\n';
        }
    }
    ts.flush();
    if (!f.commit()) {
        QMessageBox::warning(this, QStringLiteral("Save scope data"), f.errorString());
        return;
    }
    m_status->setText(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(path)));
}

#include "ScopeWindow.moc"
