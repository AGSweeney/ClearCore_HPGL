/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#pragma once

// ClearPath scope (Tools > ClearPath scope): stacked traces on one time axis from a ClearPath MC/SD on
// its own USB port (ClearPathLink.h).
//
// What the motor can give:
//   - one fast signal, streamed at about 845 samples/s: the monitor variable on channel 1;
//   - two digital lanes carried in every stream sample (byte 6: an HLFB-like bit and an enable bit,
//     provisional);
//   - registers, read in turn between stream samples (tens of reads per second in all).
// Lanes are configurable (which signals, order, colour, auto or fixed scale) and saved with the
// window length, the port and the window layout, so the scope opens as it was left.

#include "ClearPathLink.h"

#include <QColor>
#include <QPointer>
#include <QElapsedTimer>
#include <QStringList>
#include <QWidget>

#include <map>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialog;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTableWidget;
class QThread;
class QTimer;
class ScopePlot;
class ScopeWorker;

namespace scope {

enum class Source { Stream, Register, Digital };

// A signal the scope can show.
struct SignalDef {
    QString key;
    QString name;
    QString unit;
    Source source;
    int monId = 0;                          // Stream: monitor variable
    const clearpath::ParamDef *param = nullptr;  // Register (and the alert lane)
    double scale = 1.0;                     // Register: value = raw x scale
    int bit = -1;                           // Digital from the stream: bit in byte 6
    QString note;                           // what we know about it, and how well
};
const std::vector<SignalDef> &Catalog();
const SignalDef *Find(const QString &key);  // takes a lane's key too

// A lane's key is a signal's, with "m<n>/" in front for every motor after the first, so one motor keeps the
// keys it always had and the plot and trigger need not know how many there are.
constexpr int kMaxMotors = 4;
int MotorOf(const QString &key);
QString BaseKey(const QString &key);
QString MotorKey(int motor, const QString &key);

// One lane as configured.
struct Lane {
    QString key;
    bool on = true;
    QColor color;
    bool autoScale = true;
    double min = -1.0, max = 1.0;
    bool overlay = false;  // draw in the lane above (analog lanes)
};
std::vector<Lane> DefaultLanes();

// Recorded samples of one signal.
struct Series {
    std::vector<double> t, v;
    void add(double tt, double vv) {
        t.push_back(tt);
        v.push_back(vv);
    }
    void trim(double before);  // drop samples older than `before`
};

// What the worker hands the window, about 30 times a second.
struct Batch {
    std::vector<clearpath::StreamSample> stream;  // t already on the stream's own even spacing
    int monId = 0;                                // channel 1's variable for these samples
    int motor = 0;                                // whose they are
    std::vector<QString> regKey;                  // register / alert samples
    std::vector<double> regT, regV;
};

}  // namespace scope
Q_DECLARE_METATYPE(scope::Batch)

class ScopeWindow : public QWidget {
    Q_OBJECT
public:
    explicit ScopeWindow(QWidget *parent = nullptr);
    ~ScopeWindow() override;

    enum class TriggerMode { Off, Auto, Normal, Single };
    enum class TriggerEdge { Rising, Falling, Either };
    // Set the trigger as the controls would (also for tests); `pre` is the share of the screen before it.
    void setTrigger(TriggerMode mode, const QString &source, TriggerEdge edge, double level, double pre);
    int triggerCount() const { return m_trigCount; }
    double triggerTime() const { return m_capT; }  // the latest capture's trigger time (NaN: none)
    bool triggerArmed() const { return m_trigArmed; }
    bool triggerRolling() const;                   // showing the live roll, not a capture

    // For tests: connect to simulated motors and record.
    void startSimulated(int motors = 1);
    int motorCount() const;  // connected
    void setMotorName(int motor, const QString &name);
    bool recording() const { return m_recording; }
    int laneCount() const;
    const scope::Series *series(const QString &key) const;
    std::vector<scope::Lane> lanes() const { return m_lanes; }
    void setLanes(const std::vector<scope::Lane> &lanes);
    QString statusText() const;
    // Drawing cost (ms), for keeping the scope responsive.
    double maxPaintMs() const;
    void resetPaintStats();
    void setWindowSeconds(int s);
    int shownLaneCount() const;  // lanes on screen, overlays merged
    int frameIntervalMs() const; // the frame timer: the screen's refresh period
    int plotFrames() const;      // frames drawn so far
    double windowSeconds() const;  // the timebase
    double plotViewEnd() const;    // time at the plot's right edge
    QWidget *plotWidget() const;
    void ShowHelp();               // the help document (Help, F1)
    QDialog *helpDialog() const;
    bool frozenBadgeShown() const;
    void stopAndDisconnect();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void BuildUi();
    void RefreshPorts();
    void LoadSettings();
    void SaveSettings() const;
    void RebuildLaneTable();
    void ApplyLanes();  // lanes changed: plot, worker's register list, settings
    struct Motor;
    int StreamMonId(int motor) const;
    QStringList RegisterKeys(int motor) const;
    Motor *FreeMotor();
    void ConnectPort(const QString &port, bool wait);
    void ConnectAll();
    void AddLanes(int motor);
    void ShowMotors();
    void ShowHealth();
    void Start();
    void Stop();
    void Disconnect();
    void SaveCsv();
    void SyncState();
    void OnBatch(const scope::Batch &b);
    void RefreshTriggerSources();
    void ApplyTriggerUi();
    void ResetTrigger(double scanFrom);
    void RunTrigger();

    std::vector<scope::Lane> m_lanes;
    std::map<QString, scope::Series> m_data;
    int m_activeMon = 27;
    bool m_connected = false, m_recording = false;
    double m_lastT = 0.0;
    // Smooth scrolling: a clock anchored to the newest data, read each frame.
    QElapsedTimer m_wall;
    double m_clockT = 0.0, m_clockWall = 0.0, m_viewEnd = 0.0;
    bool m_haveClock = false;
    double m_statusT = -1e9;  // when the status line was last written (stream time)
    int m_rate = 0;  // stream samples in the last second
    std::vector<double> m_rateWindow;

    // Trigger state.
    TriggerMode m_trigMode = TriggerMode::Off;
    QString m_trigSource = QStringLiteral("mon27");
    TriggerEdge m_trigEdge = TriggerEdge::Rising;
    double m_trigLevel = 0.0, m_trigPre = 0.2;
    bool m_trigArmed = true, m_trigRolling = true;
    bool m_havePrev = false, m_readyLow = false, m_readyHigh = false;  // hysteresis
    double m_prevV = 0.0, m_prevT = 0.0, m_scanT = 0.0;
    double m_pendingT = 0.0, m_capT = 0.0;  // NaN when none
    int m_trigCount = 0;
    double m_lastCapWall = 0.0;

    std::vector<std::unique_ptr<Motor>> m_motors;  // a slot each; a slot's lanes stay until the next first connect
    std::vector<QString> m_labels;                 // their names, for lanes
    QElapsedTimer m_shared;                        // every motor's samples are on this clock
    QTimer *m_frameTimer = nullptr;

    QComboBox *m_port = nullptr;
    QWidget *m_motorRow = nullptr;
    QPushButton *m_connectAll = nullptr, *m_disconnect = nullptr;
    QPushButton *m_refresh = nullptr, *m_connect = nullptr, *m_run = nullptr, *m_freeze = nullptr, *m_save = nullptr,
                *m_lanesButton = nullptr, *m_clearEstop = nullptr;
    QDoubleSpinBox *m_window = nullptr;
    QLabel *m_status = nullptr, *m_alerts = nullptr;
    QSplitter *m_split = nullptr;
    ScopePlot *m_plot = nullptr;
    QWidget *m_config = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_noteLabel = nullptr;
    QComboBox *m_trigModeBox = nullptr, *m_trigSourceBox = nullptr, *m_trigEdgeBox = nullptr;
    QDoubleSpinBox *m_trigLevelBox = nullptr;
    QSpinBox *m_trigPreBox = nullptr;
    QPushButton *m_trigArm = nullptr, *m_trigHalf = nullptr;
    QLabel *m_trigStatus = nullptr;
    QPushButton *m_helpButton = nullptr;
    QLabel *m_frozenBadge = nullptr;
    QPointer<QDialog> m_help;  // closes and deletes itself
};
